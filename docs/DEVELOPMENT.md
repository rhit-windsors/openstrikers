# Testing and debugging

Run commands from the repository root unless a command changes directory.
For setup, see [Building and patching](BUILDING.md). Scripts default to
`build/` unless noted; select the audio executable explicitly when testing sound.

## Quick checks

In MSYS2 UCRT64 (or a Linux shell), build the three test executables before
running CTest. They are excluded from the default build and need no disc or GPU:

```bash
cmake --build build-audio --target openstrikers-launch-options-test openstrikers-net-test openstrikers-detmath-test
ctest --test-dir build-audio -R '^openstrikers-' --output-on-failure
```

The name filter selects this project's tests. Running CTest without it also
discovers Aurora's tests, which are not built by the targets above.

For launcher logic, in PowerShell:

```powershell
npm.cmd --prefix launcher test
```

Game integration checks need your own disc and open a game window. In PowerShell:

```powershell
$env:OPENSTRIKERS_ISO = "D:\Games\Strikers.iso"
.\smoke-test.ps1 -ExePath .\build-audio\openstrikers.exe
.\tests\render-smoke.ps1 -BuildDir build-audio -PictureMode Widescreen
.\tests\replay-check.ps1 -BuildDir build-audio
```

The render check reads actual GPU pixels at 4K; `Original` and `Stretch` are
also supported. The replay check records input, then replays it with timing
jitter and compares state hashes. See [network diagnostics](NETPLAY.md) for
the transport test with a bot.

## Debugging reference

### The smoke test

`smoke-test.ps1` is the regression harness. It launches the game with
`OPENSTRIKERS_SKIP_FE=1`, which skips the front end and starts an
exhibition at Peach & Toad Stadium (`GameInfo.cpp`, `main.cpp`), waits for
`-- Memory upon Exiting InitializeGameState` on stderr, and then checks the
process is still alive some seconds later.

```powershell
.\smoke-test.ps1                        # 90s to initialise, 20s of gameplay
.\smoke-test.ps1 -SurvivalSeconds 90    # hold longer
.\smoke-test.ps1 -KeepOpen              # leave it running on success
.\smoke-test.ps1 -Manual                # front end live, drive it yourself
```

`-Manual` leaves `OPENSTRIKERS_SKIP_FE` unset so the front end runs, waits for
the process to end on its own instead of killing it, and requires no ready
marker. Use it when a bug only appears on the path through the menus; a crash
you reach by hand still gets a symbolized stack.

Exit 0 is a pass, 1 is a crash or early exit, 2 is an initialisation timeout. It
writes `build/logs/smoke-<stamp>.stderr.log` and `.stdout.log` (`manual-` when
run with `-Manual`), and on a crash it
pipes the backtrace through `addr2line` for you. Run it after every change: most
of the fixes in this port move the failure rather than remove it, and the log
pair is the fastest way to see which.

Both stdio streams are unbuffered (`src/main.cpp`). The game's own diagnostics go
to **stdout** through `nlPrintf` and the port's go to **stderr**, and a
block-buffered stdout loses the last few thousand characters when the process
faults -- which are exactly the ones that say why. Check both logs; a message you
expected and did not get is itself a finding.

Note that `nlBreak()` (`*(u32*)1 = 0`) is a deliberate abort, so an access
violation *writing address 0x1* is an assertion, not a wild pointer. The
allocator's out-of-memory path is the usual one.

Build with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (the default suggested above) —
it keeps line tables while staying fast enough that the game reaches the crash
in reasonable time. `Debug` works but is slow.

```bash
cd build
gdb -q --args ./openstrikers.exe "C:/path/to/game.iso"
```

Useful batch invocation — run to the fault and dump every thread:

```bash
gdb -q -batch \
    -ex 'set pagination off' -ex run \
    -ex 'bt 30' -ex 'thread apply all bt 8' \
    --args ./openstrikers.exe "C:/path/to/game.iso"
```

Note that the crash is often on a worker thread (Aurora runs the GX FIFO
processor and a render worker), so `thread apply all bt` is usually more
informative than `bt` alone.

### Soak testing: playing whole matches unattended

The smoke test proves a match starts. Most of the crashes left take minutes of
real play to reach -- goals, replays, cinematics, the final whistle, the recaps
-- and a person playing through them is slow and does not reproduce. `soak.ps1`
plays matches by itself and collects every failure:

```powershell
$env:OPENSTRIKERS_ISO = 'C:\path\to\game.iso'
.\soak.ps1 -Runs 10                      # ten seeds, full-length matches
.\soak.ps1 -Runs 10 -MatchSeconds 60     # short matches: more whistles and recaps per hour
.\soak.ps1 -Seed 1234 -Runs 1            # rerun one seed
```

Each run is a skip-front-end match with these switches, all of which also work
on their own:

| Switch | Effect |
|---|---|
| `OPENSTRIKERS_AUTOPLAY=<seed>` | Plays port 0. While the ball is live (`osHostLiveGameplay()`, `patches/decomp-late/132`) it runs the stick in random directions and passes, shoots, charges shots, uses items and turbos at random. Anywhere else -- intro, goal celebration, replay, summary, post-game menu -- it presses nothing for 20 game-seconds, so replays and cinematics play out, then taps A once a second, which steps through the summary and takes Rematch. One run therefore plays match after match (seven 60-second matches in six minutes). Random input in the menus used to wander off to the main menu and Strikers 101 instead. Replaces `OPENSTRIKERS_AUTO_A`; `OPENSTRIKERS_INPUT` presses still apply on top. Logs `heartbeat frame N` every 600 frames. |
| `OPENSTRIKERS_FIXED_STEP=16.6667` | Constant game delta, so a seed replays the same input against the same game time. |
| `OPENSTRIKERS_UNCAPPED=1` | No frame pacing. With a fixed step this is fast-forward: about 2.75x real time here, limited by the 165 Hz present (Dawn picks `Mailbox`). |
| `OPENSTRIKERS_AI_SKILL=4` | 0 Training .. 4 Legend; the host's version of the game's own `soak_diff` key. At Legend the AI scores constantly -- seven goals in a 60-second match is normal -- which is the point: every goal is a replay, an overlay and a celebration cinematic. |
| `OPENSTRIKERS_MATCH_SECONDS=<n>` | Overrides the 300-second match length. |

A run ends as **crash** (the in-process handler printed `FATAL EXCEPTION`;
the stack is symbolized and runs with the same innermost frames are grouped),
**died** (exited with no report), **hang** (heartbeat stopped for
`-StallSeconds`), or **survived** (still playing at `-RunSeconds`, which is the
pass). The line for each run gives its seed, the frame it got to, how many
matches it finished and how many goals were scored -- the HUD logs
`openstrikers: score L-R` on every change -- and a failure prints the command
that reruns its seed. Everything is in `build\logs\soak-<stamp>\`, one directory
per seed plus `summary.tsv`.

Run one at a time (`-Parallel` defaults to 1). Uncapped, each instance takes
every frame the GPU will give it; two at once made the desktop unusable. Runs
start at below-normal priority for the same reason, and are not minimized,
because aurora skips frames for a minimized window and the game would stall.

**Seeds are not fully deterministic.** The fixed step pins the game's delta,
but asynchronous file loads still complete on whatever frame they complete, so
a rerun of a crashing seed reproduces the conditions, not always the crash.
Rerun it a few times, and trust the grouping across seeds more than any single
run.

**Every way out is reported now.** The exception filter only ever saw SEH
faults; `abort()` (which `std::terminate` and failed library assertions end
in), CRT invalid-parameter and pure-call failures went out through
`__fastfail` as a bare `0xC0000409`, and a stack overflow had no stack left to
report on. `src/compat_shims.cpp` now catches `SIGABRT`, terminate, invalid
parameter and pure call with the same `FATAL EXCEPTION` header plus a `kind`
line, and reserves stack for the filter with `SetThreadStackGuarantee`. Every
report also gives the host frame and thread id.

### Capturing what is actually on screen

Do not try to screenshot the window from outside the process. The swapchain is a
DXGI flip-model surface, so GDI's `CopyFromScreen` captures whatever the desktop
compositor has at those coordinates and `PrintWindow` returns a stale DWM
bitmap. Both will happily hand you a plausible-looking image of something else.

Instead, read the frame off the GPU:

```bash
AURORA_DUMP_FRAME=every:2000 ./openstrikers.exe "C:/path/to/game.iso"
```

This writes `frame_<n>.ppm` (the present source, at render resolution) into the
working directory. `AURORA_DUMP_FRAME=<n>` dumps exactly frame n instead. The
readback blocks on the submit that carries it, so only the requested frames pay
for it.

Frame dumping is not free of side effects: `resolve_frame_dump`
(`extern/aurora/lib/aurora.cpp`) releases its staging buffer on the render
worker thread, and the decomp overrides the global `operator new`/`operator
delete` to route to `nlMalloc`/`nlFree`. So the dump path frees aurora memory
through the game's heap, off-thread, and can trip the heap validator on its own.
If a run dumping frames crashes in `MemoryAllocator::Free` under
`resolve_frame_dump`, that is the tool, not the game — re-run without
`AURORA_DUMP_FRAME` before believing it.

### Driving the front end without a keyboard in front of you

`SetForegroundWindow` fails from a background shell, so `SendInput` goes to
whatever window actually has focus and never reaches the game. Post the messages
to the window instead: `PostMessage(hwnd, WM_KEYDOWN/WM_KEYUP, vk, lParam)` is
delivered whether or not the window is focused, and SDL3 picks it up.

The keyboard layout is in `installDefaultKeyboardBindings` in
`src/compat_shims.cpp`: X = A, Z = B, Enter = Start, T/G/F/H = D-pad
up/down/left/right, and the **arrow keys are the analog stick**, not the D-pad.

**Or script the presses inside the process.** `OPENSTRIKERS_INPUT` drives the
virtual pad off the same frame counter the game runs on, so a sequence repeats
run to run instead of racing the window manager:

```bash
OPENSTRIKERS_SKIP_FE=1 \
OPENSTRIKERS_INPUT="400:A;520:A;640:A;760:A;1500:START;1620:DUP;1700:A;1800:DUP;1880:A" \
    ./openstrikers.exe "C:/path/to/game.iso"
```

That one is the quit-a-match sequence: four A pulses to get through kickoff,
then Start to pause, D-pad up to wrap onto QUIT, A, D-pad up to move the popup
off its default NO, A.

Entries are `frame:BUTTONS[:holdFrames]` separated by `;` or `,`. Frames are
host frames since process start. `BUTTONS` is one or more of `A B X Y Z L R
START DUP DDOWN DLEFT DRIGHT SUP SDOWN SLEFT SRIGHT`, joined with `+`. The hold
defaults to 6 frames -- long enough for an edge-triggered `JustPressed`, short
enough not to trip a menu's auto-repeat. Every press is echoed to stderr as it
fires, so the log says what the game was actually handed.

**Gameplay needs the analog axes, not just the button word.** `SUP`, `SDOWN`,
`SLEFT` and `SRIGHT` deflect the *analog stick*; the D-pad entries do not move
it, and nothing in a match reads the D-pad. `L` and `R` drive their analog
trigger axes as well as their digital bits, which matters because the game asks
for R through `GetPressure(PAD_TURBO)` -- that reads `triggerRight`, so a script
that set only the digital bit could never turn turbo on. Turbo wants both, R
held *and* the stick off centre, so running with turbo is `frame:R+SUP:600`,
not `frame:R:600`.

Setting `OPENSTRIKERS_INPUT` also stands `OPENSTRIKERS_AUTO_A` down at the first
scripted frame: auto-A is useful for reaching kickoff and actively harmful once
a script starts opening menus, because it keeps mashing A through them. Note
that leaving auto-A running through a whole match is unreliable in its own right
-- see the A-mashing entry in `ISSUES.md` -- so prefer scripting the few presses
you need over holding A for minutes.

Two menus in the front end are worth knowing about when scripting: the pause
menu wraps, so one D-pad up from the top lands on QUIT (item 5 of 6), and the
quit popup starts on NO, so it needs a direction press before A.

Time presses that go through the window against the scene, not against process
start. A press sent before its screen exists is simply gone. Now that the host frame is paced to
60 Hz the front end runs at wall-clock speed — the health-and-safety screen is
up about 12 s in and each subsequent screen takes a few seconds — but the
reliable signal is still the log, not a stopwatch: `FESceneManager` scene pushes
name the `.fen` being loaded. Send a sequence, then read back which scenes were
pushed.

Two screens do not respond to <kbd>X</kbd> alone. Team select needs the cursor
moved off a locked (silhouetted) captain first, and the choose-a-side widget
needs <kbd>←</kbd> or <kbd>→</kbd> to move the controller icon onto a team
before A means anything — otherwise you get **"AT LEAST ONE PLAYER MUST CHOOSE
A SIDE"** on a loop, which looks exactly like a hang and is not one.

### Other diagnostics

* `cmake -S . -B build -DOPENSTRIKERS_FRAME_STATS=ON` logs per-frame draw counts and
  the render-pass list (label, size, clear value, command count) plus the
  logical and render viewport/scissor. This is the fastest way to tell "the
  game submitted nothing" apart from "the game submitted something invisible".
  The define lands on `aurora_gx`, not `openstrikers` -- `recording.cpp` belongs to
  that target, so putting it in `CMAKE_CXX_FLAGS` for the main target silently does
  nothing. Toggling the option rebuilds one file.
* `OPENSTRIKERS_TEXTURE_DUMPS=1` writes every uploaded texture to
  `%APPDATA%/openstrikers/texture_dumps` as DDS, named by its source key
  (format, dimensions, hash), which answers "is the right asset being decoded?"
  directly. The dump is skipped when the conversion fails or a palette
  texture's TLUT is missing, so a texture that is *absent* from the dump is
  itself the finding. Note that it also makes aurora log a
  `texture_replacement: missing runtime key` warning per texture -- that is the
  replacement system saying no HD replacement pack is installed, not an error.
* **Flipping `EnableDebugPrints` in `extern/aurora/lib/gx/gx.hpp`** (a
  `constexpr bool`, so it needs a rebuild) makes aurora log every distinct
  shader config it builds -- TEV stages with their texmap and channel ids, the
  colour channels' lighting/material/ambient sources, the texgens and the alpha
  compare. That is the fastest way to answer "is this draw even being asked to
  sample a texture?" or "is lighting enabled on this surface?" without guessing
  from pixels. The intro builds about 230 configs, so aggregate the log rather
  than reading it. Turn it back off when done; it is not a patch.

* `OPENSTRIKERS_TEST_AUTO_REPLAY=1` triggers a goal replay without scoring a
  goal. Waiting for the AI to score is slow and not reproducible; with this set,
  once the replay buffer holds about six seconds the hook fills in
  `GoalScoredData`, raises a `SHOT_AT_GOAL` event, calls
  `PlayAutoReplay(REPLAY_TYPE_GOAL)`, and returns to gameplay when the choreo
  reports done. It prints `AUTO_REPLAY_TEST begin=... end=...` and
  `AUTO_REPLAY_TEST completed time=...` to stderr, so a run that never prints
  the second line died inside replay. Inert unless the variable is set.
  `patches/decomp-late/111-replay-auto-replay-test-hook.patch`.
* `OPENSTRIKERS_JUMBO_DEBUG=1` traces the in-stadium screens: the presentation
  opcodes that drive them (5 load, 13 play, 25 stop, 28 unload) with the state
  machine's answer, the animation frame the game selected, the first bytes
  actually sitting in that frame's buffer, and per-frame counts of how many
  screen surfaces were submitted, drew, and took the animation texture. When one
  did not draw, it names the early return in `DrawableModel::DrawModel` that ate
  it -- which is how "the screens are transparent in the gameplay camera" was
  established as deliberate rather than broken.
  `OPENSTRIKERS_JUMBO_FORCE=1` keeps every screen surface opaque and hands it a
  known-good frame, so a capture shows where the screens are. Both inert unless
  set.
* `OPENSTRIKERS_NO_TEX_INVALIDATE=1` restores aurora's old no-op
  `GXInvalidateTexAll`, for A/B against the fix described above. With it set,
  any texture the game refills in place stops updating after its first draw.
* **The compiler's own warnings are a to-do list.** `cmake --build build
  --clean-first 2>&1 | grep 'loses precision'` enumerates every remaining site
  that casts a pointer to a 32-bit integer -- the bug class behind the replay,
  sound-handle, interpreter and animation-trigger fixes. When something behaves
  as though it read a garbage pointer, check whether its file is on that list
  before instrumenting anything.

### Crashes that only happen without a debugger

Some faults here come from a pointer that has lost its top 32 bits, so whether
they crash depends on what the loader happened to map at the truncated address.
Running under gdb changes that layout and the crash disappears; gdb also cannot
always attach to the process after the fact. So the process reports its own
fault: an unhandled-exception filter in `src/compat_shims.cpp` prints the
exception code, the faulting address, and a stack of link-time addresses.

Turn those into source locations with:

```bash
addr2line -e build/openstrikers.exe -f -C -i -p 0x140020a24
```

`-i` matters: most of this code inlines heavily, and without it you get the
outermost function instead of the one that actually faulted.

The addresses really are link-time ones. Recovering them is fiddlier than it
looks: the loader **rewrites `OptionalHeader.ImageBase` in the mapped image** to
wherever it actually put the module, so subtracting the in-memory value is a
no-op and every frame comes back `?? ??:0`. The handler reads the link-time
`ImageBase` back out of the executable file instead, and prints both bases so
you can check.

The `at` line is a raw runtime address, not rebased — subtract the loaded base
and add the linked one to look it up.

Two things that make a debugger-only repro tractable:

* `_NO_DEBUG_HEAP=1 gdb --args ./openstrikers.exe ...` — Windows gives a
  debugged process a different heap by default, which is enough to hide some
  overruns.
* The check below, which catches the truncation at the store instead of at the
  eventual fault.

### The 32-bit pointer check

Serialized `.fen` structs keep the GameCube's four-byte pointers
(`nlGCPtr32`), which is lossless only for MEM1 addresses. Storing a host stack
or image address in one truncates silently, and the crash lands somewhere else
entirely, minutes later. Build with:

```bash
cmake -S . -B build -DOPENSTRIKERS_CHECK_GCPTR=ON
```

and any such store aborts on the spot with the offending pointer. This is the
first thing to reach for when a pointer in a backtrace looks like a stack
address with its top bytes missing.

### The allocator validator

Most porting bugs in this project surface as heap corruption several thousand
allocations after the actual mistake, because the `nl` allocator stores its
free-list nodes *inside* the free memory they describe. Configure with:

```bash
cmake -S . -B build -DOPENSTRIKERS_MEMALLOC_VALIDATE=ON
```

This compiles the free-ring validator in `extern/decomp/src/NL/MemAlloc.cpp`,
which checks the ring invariants on entry to and exit from every `Allocate` and
`Free` and dumps a log of recent operations when one breaks. It is a per-source
define, so toggling it rebuilds one file rather than the whole game.

Reading its output:

* **The node it names is usually not an address.** The ring links live inside
  free memory, so an overrun rewrites them with whatever was written. The line
  also reports `after node=`, the last node that read back cleanly — *that* is
  the block that was actually written over, and it is the address the op log is
  replayed for.
* **The hex dump around that node identifies the writer faster than any
  backtrace.** The write has already happened; what is sitting there is a sample
  of what wrote it. A repeating 8-byte pattern whose first byte is `0x_9`/`0x_a`
  is DSP-ADPCM audio, for instance, which is what pointed at the stream headers
  above.
* **The op log carries a thread id per entry.** The game is single-threaded, but
  this allocator is also the global `operator new`, so aurora's FIFO and render
  workers are in it too. "Which thread owns the block either side of the damage"
  is usually most of the answer.
* **`NL_TRACE_ALLOC_SIZE=<n>`** (decimal or `0x`-prefixed) prints a backtrace for
  every allocation of exactly that user size. The op log says where a block is
  and what happened to it; this says who asked for it. The op log's `alloc user`
  entries report `detail` as `(size << 8) | alignment`, so the size to trace is
  already in front of you.

Both the validator's output and anything else that needs a stack use
`osDumpBacktrace` (`src/compat_shims.cpp`), which prints link-time addresses for
`addr2line` — see below.

---
