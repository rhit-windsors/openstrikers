# OpenStrikers issue sheet

This is the short, current list of visible or gameplay-affecting problems. Keep
the detailed investigation history in `README.md` and update this sheet as an
issue is reproduced, narrowed down, or fixed.

## Active issues

### Players sometimes hover while running with the ball

- **Expected:** A player carrying the ball stays planted on the pitch, and the
  ball remains in the correct dribbling position relative to the player.
- **Actual:** Players sometimes lift into the air while running with the ball;
  when this happens, the ball trails behind them.
- **Status:** Reported, not yet reproduced here. Capture which movement,
  possession, or animation-state transition starts the hover, then compare the
  player's ground height and the ball attachment/offset before and after it.

### The game crashes after a goal is scored

- **Expected:** A goal plays its celebration and replay, then the match resumes
  from kickoff.
- **Actual:** The game crashes after a goal is scored.
- **Priority:** High. A goal is unavoidable in normal play, so this ends most
  matches. The replay-specific pointer and highlight-layout faults have now
  been fixed, so another goal-to-kickoff run will show whether this was the same
  bug or whether a separate goal-presentation fault remains.
- **Status:** Not reproduced since the replay fix, and one run now argues
  against it. A 60-second `capture.ps1` run on the current build pushed
  `art/fe/goal_overlay.fen`, went 0-2 by frame 1800 and 0-3 by frame 3300, and
  was still rendering normally when the harness killed it on its timer -- no
  `FATAL EXCEPTION`, no early exit. So the build now survives goals that used to
  end the match, which is consistent with this having been the replay pointer
  bug all along.
- **Do not close it on that.** It is one run, and the harness ended it rather
  than the match ending on its own. Confirm with a full match played to the
  final whistle before marking this fixed. To reproduce if it does recur, drive
  a match with `OPENSTRIKERS_INPUT` (see the README) and let the AI score, then
  capture the final stderr and note whether the crash lands before the goal
  overlay, during the replay, or on the return to kickoff;
  `art/fe/goal_overlay.fen` and `x2_sts.fen` scene pushes bracket the region.

### The match crashes when A is held down through play

- **Expected:** Mashing or holding A during a match shoots and passes; it does
  not destabilise the match.
- **Actual:** A run driven by `OPENSTRIKERS_AUTO_A=1`, which pulses A about
  twice a second for the whole match, dies partway through gameplay. Two of
  three such runs died at frames 1080 and 1440; the same build without auto-A
  ran a match, quit it and reached the main menu over 75 seconds, and
  `smoke-test.ps1` passes a 40-second survival window.
- **Symptom shape:** The process dies with no `FATAL EXCEPTION` line, so the
  unhandled-exception filter never ran -- consistent with a stack overflow
  (`0xC00000FD`), which SEH cannot report without a reserved guard page.
- **Priority:** Medium. It does not block play, but it makes every unattended
  capture that uses auto-A unreliable, and it is probably a real bug in whatever
  repeated A triggers.
- **Status:** Reproducible but intermittent. Reproduce with
  `OPENSTRIKERS_SKIP_FE=1 OPENSTRIKERS_AUTO_A=1` and no input script, and get a
  stack under gdb rather than from the crash handler.

### AI shooting can divide by zero

- **Expected:** An AI fielder choosing a shot always produces a valid shot
  windup time.
- **Actual:** An unattended match can raise `0xC0000094` in
  `nlRandom(unsigned int, unsigned int*)` after `cFielder::DesireShoot` converts
  `(fShotWindupTime - 0.2f)` to a zero integer range.
- **Status:** Reproduced once while verifying replays, after replay playback had
  already completed and returned to gameplay. Guard or correct the range at
  `FielderDesires.cpp:2565`; keep this separate from the replay fix.

### Goalkeepers do not defend

- **Expected:** Goalkeepers track play, position themselves, and attempt saves.
- **Actual:** Goalkeepers do not defend the goal during a match.
- **Additional symptom:** Goalkeepers sometimes move erratically, as though
  their animations or state updates are playing too quickly.
- **Partial cause found, needs re-testing.** The goalie save table is built from
  animation *milestone* percentages, and every one of them was zero, so a keeper
  had no idea when in a dive animation the hands actually reach the ball.
  Milestones come from animation trigger callbacks, and no trigger ever matched:
  `CharacterTriggers` read `cb->m_nParam1` as a `cSAnim*` and compared
  `GetHashID()` against the trigger id, when `m_nParam1` is really the
  `AnimTagCBInfo`. That cast worked on the GameCube by coincidence --
  `cSAnim::m_uHashID` and `AnimTagCBInfo::ScriptInfo.Trigger` are both at 0x4
  there -- but `AnimTagCBInfo` leads with an eight-byte pointer here, so the
  comparison read the top half of that pointer. Fixed in
  `patches/decomp-late/110-anim-trigger-callback-info-cast.patch`. Nothing
  crashed; the whole trigger system just silently did nothing, which is why this
  took so long to see.
- **Also suspect:** `Goalie.cpp` still casts `this` to `unsigned int` in four
  places (`DoNavigation`, `SetupBlender`) to pass itself as an animation
  callback parameter -- lines 1787, 1807, 3355 and 3374. Those truncate on this
  host, so the callback recovers a bad `Goalie*`. This is the same bug class as
  the replay fix and is a likely cause of the erratic movement.
- **Status:** Re-test before investigating further. Two real faults underneath
  this have been fixed or identified since it was last observed, so confirm
  whether keepers still fail to defend at all, and whether the erratic movement
  survives fixing the four truncating casts above.

### Match intro screens do not display

- **Expected:** The pre-match intro scene is visible before gameplay begins.
- **Actual:** The intro scene runs without displaying correctly and still waits
  for input.
- **Harness behavior:** `capture.ps1` pulses A with `OPENSTRIKERS_AUTO_A=1` to
  skip the invisible intro and reach gameplay.
- **Status:** Reproducible; not yet investigated.

### Lighting and some character materials look incorrect

- **Expected:** Stadium and character lighting and material shading match the
  original game.
- **Actual:** Lighting is visibly off. Some character areas appear flat
  yellow/lime or unusually bright. Geometry, animation, texture streams, and
  texture-bundle loading are present.
- **Status:** Likely a lighting or material-state translation issue; not yet
  isolated.

## Recently fixed

### More 32-bit handles that had to become host width

Found by auditing the tree against the patch stack, alongside the replay work.
All the same bug: a pointer stored in a GameCube-width integer.

- **Sound handles.** `cGameSFX::Play` and its overrides return a "voice id" that
  is a `SFXEmitter*` cast to `unsigned long` -- 32 bits in this build. Every
  handle lost the top half of its address. Widened to `std::uintptr_t` across
  the whole virtual chain; the base and every override must move together or the
  overrides quietly stop overriding.
  `patches/decomp-late/107-sound-handle-host-pointer-width.patch`.
  `GetSndIDError()` also returned `0`, which is a plausible handle, and now
  returns `(u32)-1` to match what the Nis code compares against.
- **The script VM stack.** `InterpreterCore` pushed string addresses, saved base
  pointers and return addresses onto a `u32*` stack, so any script that made a
  call or took a string returned to a half address.
  `patches/decomp-late/108-interpreter-host-width-stack.patch`.
- **`nlAsyncLoadFileToVirtualMemory`** forwarded its trailing argument as an
  `unsigned long` "alignment" when it is really the callback's `nlUserParam`,
  truncating any pointer passed through it.
  `patches/decomp-late/109-nlfile-async-user-param.patch`.
- **Animation trigger callbacks** never matched at all. See the goalkeeper issue
  above. `patches/decomp-late/110-anim-trigger-callback-info-cast.patch`.

**This class is not finished.** A build prints `-Wl` "cast ... loses precision"
on many remaining sites; that warning list is the working to-do for the rest of
the 64-bit port. The concentrations are `Game/Sys/GCStream.h`, `Game/SAnim.h`
and `PowerPC_EABI_Support/.../msl_tree.h` (all header templates, so the counts
are per-instantiation rather than distinct sites), then `ode/obstack.cpp`,
`Camera/animcam.cpp`, `Render/Bowser.cpp` and `Goalie.cpp`. Build with
`cmake --build build --clean-first 2>&1 | grep 'loses precision'` to regenerate
it.

### Replays crashed or corrupted playback on the 64-bit host

- Replay snapshots stored pose, animation, effect-group, texturing, and effect
  callback pointers in GameCube-width 32-bit integers. On the 64-bit Windows
  build this discarded the upper half of each address; restoring an emission
  callback could jump into an invalid low address.
- Replay highlight saving also wrote fields through hard-coded offsets from
  `ReplayChoreo`. Those offsets describe the 32-bit GameCube class layout, not
  the host layout, so saving a goal highlight corrupted neighboring state.
- Patch `patches/decomp-late/106-replay-host-pointer-width-and-highlight-layout.patch`
  serializes host pointers as `std::uintptr_t`, restores them explicitly on
  load, and accesses every highlight through its named fields.
- Verified by recording 4.02 seconds of live gameplay, entering replay state,
  playing the entire buffer, and returning to gameplay without the former
  invalid callback jump. A separate AI shooting divide-by-zero occurred later
  and is tracked above.

### The controlled-player marker did not follow the player

- `UpdateAndRenderPlayerIndicators` reached the player position, the game
  tweaks and the player's own state through hand-written stand-in structs that
  navigated by raw byte offset -- `char pad0[0x68]` into the render snapshot,
  `char pad0[0x2B4]` into `GameTweaks`, `char pad0[0x19C]` into `cPlayer`.
  Those are GameCube offsets, and every pointer ahead of them is eight bytes
  here rather than four, so each landed somewhere else entirely.
- `UpdateAndRenderOffScreenIndicators`, in the same file, already used the real
  types. That is why the offscreen arrow tracked correctly while the overhead
  marker did not -- and it is worth remembering as a diagnostic: when one of two
  sibling code paths behaves, read what it does differently before anything
  else.
- Fixed by using the named members: `RenderSnapshot::GetCharacter(i).mPosition`,
  `GameTweaks::fIndicatorDistAboveHead` / `fIndicatorDistInPixels`,
  `cPlayer::m_UserControlledTime` and `cPlayer::m_pBall`. Patch
  `patches/decomp-late/104-overhead-indicator-real-types.patch`.
- The position member matters as much as the type. The stand-in's
  `0x68 + i * 0x58` was not `mCharacters[i]` but `mCharacters[i].mHeadPosition`
  (`mCharacters` at 0x4C, `mHeadPosition` at 0x1C within the element), so the
  base position is the head, not the character origin --
  `fIndicatorDistAboveHead` is measured from there. Reading `mPosition`
  instead leaves the marker's centre sitting on the head rather than clear of
  it.
- Verified by logging the marker's pixel position against the player's own
  projected position over a match: they track to within about 1.5 px in x and
  follow the player frame to frame, and the marker sits 61.7 px above the
  player's mid-body over 1000 samples, against 33.0 px when it was reading
  `mPosition` -- the extra ~29 px being the origin-to-head half capsule.
- A second patch was needed to see it at all.
  `patches/decomp-late/105-skipfe-assigns-port-0.patch`: `GameInfoManager` is
  constructed long before the first pad sample, and aurora reports a port as
  connected only once it has been read, so the `skipfe` path's
  `IsConnected` checks all answered false and every match started with ten AI
  players and no indicator. Port 0 is now given side 0 when the host
  `OPENSTRIKERS_SKIP_FE` switch asked for the skip.

### Exiting a match crashed the game

- Quitting a match now tears down gameplay state and returns to the main menu,
  and the process stays alive afterwards. Two separate faults were in the way.
- **The GameCube array header was being subtracted from a pointer that never
  had one.** `GoalieSave::ClearData`, `EffectsGroup::~EffectsGroup` and
  `ParticleSystem` each free `pointer - 0x10`, because MWCC's array cookie sits
  in those 16 bytes and the stored pointer is the allocation base plus 0x10.
  Under GCC the cookie is not there: the Itanium ABI emits none at all for an
  element type with a trivial destructor, and this port's
  `__construct_new_array` shim returns the block unchanged rather than the
  MSL header-writing original. So each of those frees handed `nlFree` a pointer
  16 bytes *before* the block, and the free-ring links landed in the middle of
  the preceding allocation. `Config::Config` had already been fixed this way;
  these three were the missed instances. Patch
  `patches/decomp-late/101-gc-array-header-offset.patch`, with a
  `__is_trivially_destructible` static assert at each site so a future
  destructor cannot silently reintroduce a cookie.
- **`skipfe` left the front end marked as never booted.** With the front end
  skipped, `TransitionTask::InitializeFEState`'s `gAlreadyBooted` was still
  false when a quit brought control back, so the return took the first-boot
  branch: the legal/logo sequence instead of the main menu, and no
  `AudioLoader::LoadFE` to recreate the `"FE"` stream track that
  `InitializeGameState`'s `DestroyAllTracks` had removed.
  `TitleScene::SceneCreated` then dereferenced the null track. Patch
  `patches/decomp-late/102-skipfe-counts-as-booted-front-end.patch`.
- Verified with `OPENSTRIKERS_SKIP_FE=1` and
  `OPENSTRIKERS_INPUT="400:A;520:A;640:A;760:A;1500:START;1620:DUP;1700:A;1800:DUP;1880:A"`,
  which pauses, selects QUIT, confirms, and lands on `main_menuv2.fen`.

### Shared vertex-array corruption

- Stadium and character geometry previously rendered as enormous triangles or
  flat bands.
- Packets shared vertex arrays, but Aurora sized the upload from only the first
  packet's indices. Later packets reused that binding with higher indices.
- Patch `patches/decomp-late/100-event-payload-host-layout-and-vertex-arrays.patch`
  (which absorbed the original `097-size-shared-arrays-across-model`) records the
  model-wide maximum index and uploads the required array prefix.
- Verified through frames 300, 600, and 900 of a gameplay capture.
