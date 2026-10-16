# Current issues and limits

The previous tracker reported no open gameplay issues as of 2026-10-10;
a two-PC LAN match was subsequently verified on 2026-10-15. This is the
recorded project status, not a new exhaustive compatibility test.

## Known limits

- Windows UCRT64 is the tested build. Linux has built and booted, but has
  not been rechecked since the rendering and input work.
- Sound requires `OPENSTRIKERS_AUDIO=ON`; CMake still defaults to a silent
  build. The [Windows guide](docs/PLAYING-WINDOWS.md) enables sound.
- Internet play needs a reachable host via port forwarding or a VPN.
  There is no relay service.
- A fresh shader cache can cause brief black patches as materials compile.
  Keep `initial_pipeline_cache.db` beside the executable and let the game
  warm up in its menus.
- Online participants need the same game executable and matching disc
  region/revision. Real network testing recorded so far covers two PCs on a LAN.

## Watch for regressions

- **Cup trophy texture crash, fixed 2026-10-16** (`patches/decomp-late/155`).
  The asynchronous bundle reader interpreted the texture count in host byte
  order, then read outside the bundle when a cup trophy loaded after a match.
  See the [diagnosis and validation](docs/archive/async-trophy-texture-crash.md).
- A one-off audio-build startup crash in Aurora's pipeline-cache loader was
  likely caused by the MusyX sound-stack overrun fixed in `patches/musyx/003`.
  Reopen with a new log if it returns.
- Replay lighting was investigated and later judged correct. The artificial
  `OPENSTRIKERS_TEST_AUTO_REPLAY` hook can render the replay camera in the wrong
  gameplay state; use naturally triggered replays to assess lighting.

## Report a problem

Record the build/commit, disc region/revision, steps to reproduce, expected
behavior, actual behavior, and whether the issue occurs locally or online.
Attach a game log from **Settings > Open logs folder**. For crashes and
rendering checks, see [Testing and debugging](docs/DEVELOPMENT.md).

Keep confirmed open problems and their current status here. Put investigation
writeups and resolved history in the [archive](docs/archive/README.md);
the former issue sheet is preserved in [the historical issue log](docs/archive/issues.md).
