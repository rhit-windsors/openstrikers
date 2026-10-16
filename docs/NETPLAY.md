# Network play reference

For lobby setup and joining a match, see the [Windows play guide](PLAYING-WINDOWS.md#play-with-friends-online).
This page covers direct terminal startup, determinism and network diagnostics.

## Terminal startup

Up to eight machines can play one match, one human per fielder: each team's
captain and three sidekicks. Goalies stay with the computer, as in the original
game. One machine hosts and the others join it:

```powershell
# host: listens on UDP 52320 (or the port given) and waits for 2..8 machines in all
openstrikers.exe "C:\path\to\game.iso" --net-host --net-players 2 --net-name Riley
# everyone else
openstrikers.exe "C:\path\to\game.iso" --net-join 192.168.1.20 --net-name Sam
```

Each player's `--net-name` is drawn over the fielder they control, in place
of the controller number: capitals in the HUD's heading font, in that number's
colour (1 yellow, 2 blue, 3 green, 4 red; the away team's slots reuse them, as
the number art does), lightening while the player is on the ball. A machine
driving several fielders gets "NAME 1", "NAME 2". Up to 12 characters of
A-Z, 0-9 and punctuation the font has; anything else is left out. Offline,
`--player-names Riley,Sam` names the players on ports 1 and 2 the same way.
The name is drawn at 42% of the font's size; a player off screen shows it at
the screen edge instead of the number, shrinking with distance. The drawing is
`patches/decomp-late/152-player-names-over-heads.patch`; names never reach the
simulation, so they cannot desync a match.

Only the host needs a reachable port: forward UDP 52320 to it, or put everyone
on the same Tailscale/ZeroTier network and join the host's address there.
Everyone needs the **same openstrikers build and the same disc image**: the
host turns anyone else away, because the game only computes identically from
identical binaries. The [launcher](PLAYING-WINDOWS.md#play-with-friends-online)'s lobby is the easy way to set
up a match: who plays which fielder is chosen there, and passed to the games
as `--net-slots` (host) and `--net-seat` (everyone else). From the command line
without them, machines alternate teams in join order (host home captain, first
guest away captain, then home sidekick 1, away sidekick 1, ...). A team with all four fielders human is locked: nobody swaps
players. A team with fewer keeps the game's normal switching.

The host sets up the match, and its choices apply on every machine:

```powershell
openstrikers.exe game.iso --net-host --net-players 4 --home peach --home-sidekick birdo `
    --away dk --away-sidekick hammerbros --stadium forbidden --match-minutes 4 --items on --bowser off
```

(`--skill` sets how good the computer is at whatever fielders and goalies it
plays. `--home super` / `--away super` fields the Super Team, the game's
`TEAM_MYSTERY`; it ignores the sidekick option, since the game fixes its
robots.) The same options set up the match `OPENSTRIKERS_SKIP_FE=1` starts
offline. The host's picture also decides whether the game runs in its own 16:9
mode for everyone (`--widescreen`, or 4:3 otherwise), because 16:9 moves the
game's camera and what is on screen changes how the game draws random numbers;
each player still chooses fit or `--stretch`, and the window and resolution.

**How it works.** It is delay-based lockstep, the way Dolphin's netplay works:
the machines exchange only controller input, and every one runs the whole
game, frame by frame, from the same input. A machine's own input is played
`--net-delay` frames late (default 4, about 67 ms), which is the time it has
to reach everyone else. Raise it for distant players; if it is too low the game
pauses briefly whenever a frame's input has not arrived. The host gathers
every player's input, assembles each frame and sends it to all; any machine
runs a frame only once it has it.

That only works if the game is **deterministic**, which a PC port is not by
default. A session makes it so (`src/net/session.cpp`, decomp-late/149, aurora/014):

* every frame advances exactly 1/60 s instead of the wall-clock delta;
* the random seed is the host's, not the boot time;
* disc reads complete inside the game's file-system service, at the same
  frame on every machine, instead of whenever aurora's DVD thread finishes them
  (aurora's deferred DVD mode);
* every controller's input for the frame comes from the agreed frame, local
  pads included, and MusyX runs a fixed number of ticks per frame;
* the physics' sine, cosine and arctangent are openstrikers' own
  (`src/net/detmath.cpp`), because the C runtime's can differ in the last bit
  between CPUs and Windows versions (decomp-late/151);
* the match settings, including the switches `OPENSTRIKERS_MATCH_SECONDS` and
  `OPENSTRIKERS_AI_SKILL` and the game's 16:9 mode, are the host's.

Every 30 frames each machine checks a hash of the game state (`src/net/state_hash.cpp`)
against the host's. A mismatch ends the match for everyone with
"The game went out of sync", instead of letting two machines play different
games.

**Tested:** two PCs on a LAN (2026-10-15), one reading its disc image off a
network share, played a match in sync. Slow disc
storage costs hitches, never sync: reads complete on the game thread. The only
blemish was brief black patches on the second PC while its shader cache filled,
which is rendering only and the reason `initial_pipeline_cache.db` ships with
the build.

**Checking determinism without a network.** The same machinery records and
replays input:

* `OPENSTRIKERS_NET_RECORD=<file>` plays normally and records every frame's
  input and state hash.
* `OPENSTRIKERS_NET_REPLAY=<file>` feeds the recording back and compares the
  hash every frame, naming the first part of the state that differs (random
  seed, frame clock, match state, ball, home team, away team). It exits when the
  recording ends: code 0 if identical throughout, 3 otherwise.
* `OPENSTRIKERS_NET_JITTER_MS=<n>` sleeps a random 0..n ms every frame, to show
  that wall-clock timing no longer reaches the game.

`tests/replay-check.ps1` does all of it unattended: it records an autoplayed
session uncapped, then replays it with jitter. Before netplay went in it passed
26 matches and 63 goals over three seeds with not one frame differing.
`tests/netplay-bot-check.ps1` hosts a game and has `openstrikers-net-bot` (a
stand-in player that runs no game) join over a link with added latency and
loss. `ctest -R openstrikers-net` runs the lockstep core against a simulated
network that drops, duplicates and reorders packets, with up to eight machines.

From PowerShell, to exercise the transport with a bot (your disc is required):

```powershell
cmake --build build-audio --target openstrikers-net-bot
.\tests\netplay-bot-check.ps1 -BuildDir build-audio -IsoPath "D:\Games\Strikers.iso"
```

## How the launcher hands off a match

The lobby chooses controller ownership before launching the games. The host
gets `--net-host` and `--net-slots`, and guests get `--net-join` and `--net-seat`.
Seats therefore follow the lobby's choices rather than connection order.
Each machine uses local controllers in the order of its assigned fielder slots.

Lobby matches also pass `--end-after-match`. At the summary's A press or a
forfeit, the game exits and prints `openstrikers: result H-A [ot] [forfeit]`.
The launcher parses that result to update the co-op cup and bring everyone
back to the lobby (`patches/decomp-late/153`).

## Co-op cup rules and storage

The host chooses Mushroom, Flower, Star or Bowser, or a Super cup. The
launcher uses the game's fixture lists and randomly drawn captains. Cups
start with a round robin; Super cups play home and away. The last league round
uses the cup's stadium. A win earns three points and an overtime loss one.
Bowser cups finish with a top-four knockout: first against fourth, second
against third, then a final.

The players' fixtures run in the game. Other fixtures use the game's
`StatsTracker::SimulateGame` approach: fifteen chances per side at 20%, with
a coin toss for overtime. Progress is saved after every match in the host's
`%LOCALAPPDATA%\OpenStrikers\cup.json` on Windows. This is separate from the
game's cup screens, trophies and memory-card unlocks.
