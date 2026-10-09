# Plan: testing the remaining issues (written 2026-10-08)

Everything here runs unattended through `soak.ps1` (README, "Soak testing").
One game window at a time. Lighting: the user judged it "pretty spot on", closed.
Set `OPENSTRIKERS_ISO` first; launch `soak.ps1` by absolute path.

## 1. End-of-game recap crash (high) -- DONE

The user played several full games with recaps after the heap fixes: no crash.
Kept below for reference in case it recurs.

Soak logs cannot yet show the recaps ever played: nothing logs a highlight
replay, and autoplay starts tapping A 20 game-seconds after the whistle, which
may skip part of the reel. The user's crash came watching recaps through.

- Log start/finish of each `REPLAY_TYPE_HIGHLIGHT` (`ReplayChoreo.cpp:294`).
- Make autoplay press nothing while any replay is playing (extend
  `osHostLiveGameplay`, decomp-late/132, or add a replay-active query).
- Soak 4 seeds x 20 min with `-MatchSeconds 60` (Legend AI: big reels, a match
  end every ~1.5 min).
- Pass: dozens of complete reels, no crash. On a crash the abort/fast-fail
  handlers now give a stack; rerun the seed.

## 2. Negative match clock -- FIXED (decomp-late/135)

Cause found by reading: float->unsigned conversion of a negative remaining time
(CodeWarrior clamps to 0, x64 does not). Confirm in a close game; the soak below
is only needed if it is still seen.

- Log game time and the value the HUD formats, once per game-second, from 5 s
  before the whistle until the summary appears.
- 2 seeds with `-MatchSeconds 30`; keep going until both a decisive end and a
  tie (overtime has its own clock path) have happened.
- Pass: HUD value never below 0. Otherwise the log says whether the game clock
  overruns or only the display.

## 3. Super goal x2 indicator -- FIXED (decomp-late/136)

Cause: raw GC offset read of the goal type in STSX2Overlay. Verify with a soak
(AI super strikes count too): every 2-point jump in the `score` log should be
paired with `openstrikers: x2 overlay shown`.

- Log when a goal is a super strike (worth 2) and whether the goal overlay
  selects and plays the x2 component (`x2_sts.fen` already loads at match
  start, so loading is not the question).
- Check autoplay lands super strikes; if not, add a "charge until meter full,
  release" action to autoplay.
- Pass: every 2-point goal logs the x2 component playing; then one frame-dump
  capture of a super-strike goal to confirm it is visible.

## 4. Crash with A held / mashed -- DONE (user confirmed in play)

Probably fixed by today's heap/file-pool fixes; unconfirmed.

- 3 seeds with `OPENSTRIKERS_AUTO_A=1` instead of autoplay, 10 min each.
- Pass: no crash. Stack overflow is now reported, so a recursion is named.

## 5. Regression soak on the normal build

The sideline-fragment fix (decomp-late/133) was verified on `build-validate/`
only. Run 4 seeds x 20 min of full-length matches on `build/` as the gate;
keep this as the after-every-fix check, and use the validator build whenever a
crash looks like heap damage.

Order: 1 and 4, then 2 and 3, then 5. ~2.5 h of mostly unattended runs. Record
findings in ISSUES.md as they land; save new log lines as patches.

## State at handoff

Nothing from the 2026-10-08 session is committed yet: `src/compat_shims.cpp`,
`soak.ps1`, `README.md`, `ISSUES.md`, `.gitignore`, this file, and
`patches/decomp-late/124`-`136` (all verified to apply from clean).
