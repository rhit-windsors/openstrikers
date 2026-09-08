# OpenStrikers issue sheet

This is the short, current list of visible or gameplay-affecting problems. Keep
the detailed investigation history in `README.md` and update this sheet as an
issue is reproduced, narrowed down, or fixed.

## Active issues

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

### Replay backgrounds render incorrectly

- **Expected:** A replay looks like the live match it is replaying.
- **Actual:** The background textures are not right during replay playback.
- **Worst at the start, over the celebration.** The opening stretch of the
  replay -- the part where the scoring character is celebrating -- is where the
  background goes most obviously wrong. Later in the same replay it is less
  pronounced.
- **Priority:** Medium. Replays play through and return to gameplay now, so this
  is cosmetic rather than blocking, but it is on the path every goal takes.
- **Status:** Reported from watching a replay; not yet characterised here. The
  first thing to establish is whether this is its own bug or the existing
  lighting/material issue below showing up more plainly against a replay
  background -- capture the same camera angle live and in replay and compare,
  rather than assuming either way.
- **Reproduce without scoring:** `OPENSTRIKERS_TEST_AUTO_REPLAY=1` forces a goal
  replay once the buffer fills (see the README). Note that it fabricates its
  `GoalScoredData` with a null scorer, so it is good for the playback path but
  may not reproduce a celebration that depends on a real scorer -- for the
  celebration specifically, let the AI actually score.
- **Lead worth checking first.** `DrawableCharacter::Replay` restores
  `mEffectsTexturing` only under `ReplayFrameTraits<T>::IsLoadFrame &&
  frame.mInterval == 1`, while the save path writes it unconditionally
  (`patches/decomp-late/106`). That guard mirrors what the original pointer-cast
  version did, but it is the one texturing-related pointer that replay restores
  conditionally, and this is a texturing symptom. Confirm the interval actually
  is 1 on the frames that look wrong before going further afield.

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

### Match intro screen textures are wrong

- **Expected:** The pre-match intro scene looks like the original game's.
- **Was:** The intro ran without displaying at all while still waiting for
  input. **That part is fixed** -- the intro screen now runs and is visible.
- **Actual:** It draws, but with texture oddities, and the background textures
  in particular look wrong.
- **Harness behavior:** `capture.ps1` still pulses A with `OPENSTRIKERS_AUTO_A=1`
  to skip the intro and reach gameplay, which is worth keeping regardless.
- **Status:** Reproducible. Treat it as one of the background-texture group
  below rather than as an intro-specific bug until that is ruled out.

### Lighting and some character materials look incorrect

- **Expected:** Stadium and character lighting and material shading match the
  original game.
- **Actual:** Lighting is visibly off. Some character areas appear flat
  yellow/lime or unusually bright. Geometry, animation, texture streams, and
  texture-bundle loading are present.
- **Status:** Likely a lighting or material-state translation issue; not yet
  isolated.

**These three are probably one bug.** Background textures now look wrong in
replays, on the match intro screen, and materials look wrong in open play --
three different scenes, one symptom family. Before investigating any of them
separately, get the same background captured in two of those contexts and
compare; a single texture-format, material-state or TEV-stage translation fault
would explain all three, and chasing them as three bugs is how a session gets
spent three times over.

## Recently fixed

### Players sometimes floated above the pitch

- Outfield players and goalkeepers rose off the pitch during normal play, the
  ball carrier included -- the ball kept trailing along the ground behind them,
  which was the clue: the drawn skeleton and `m_v3Position` had come apart.
- `cPoseAccumulator::BlendTrans` rewrites a mirrored animation's translation
  into a local, `vtemp`, and repoints `pTrans` at it. `vtemp` was declared
  *inside* the `if (bMirror)` block while `pTrans` is dereferenced after that
  block closes, so every mirrored blend read a stack slot whose lifetime had
  ended. `BlendRot` directly above declares its `qtemp` outside the block and is
  correct; this one had drifted.
- MWCC left the slot alone and the retail game worked by accident. GCC reuses
  it, so a mirrored blend picked up whatever was there. On a limb that is a
  small wrong offset; on the animated root node, whose translation is the
  character's hip height, it lifts the entire skeleton.
  `patches/decomp-late/115-mirrored-blend-trans-dangling-local.patch`.
- **Verified.** Instrumented against the lower foot's posed world height: before
  the fix a two-minute match logged 46-93 sustained lifts, feet held ~0.87 above
  the pitch for up to 320 frames, with the root node's accumulator reading 0.84
  while every animation feeding it read ~0.29. After the fix the same run logs
  none, with or without R held.
- **It is undefined behaviour, so it moves when you look at it.** Adding an
  inert branch to `cSAnim::BlendTrans` made the symptom disappear without
  fixing anything, twice. Any future diagnostic for this class of bug has to sit
  outside the animation path -- the working one hung off
  `cCharacter::PostPhysicsUpdate` and walked the pose tree from cold code.
- **R was a red herring, and the old note here was wrong.** Gameplay does read
  R: `cAIPad::IsTurboPressed` (`src/Game/AIPad.cpp:53`) asks for
  `GetPressure(0x14, true)`, and `0x14` is `PAD_TURBO` written as a literal,
  which is why grepping for the enum name found nothing. R plus a deflected
  stick is turbo, and the turbo run states force mirror swaps
  (`mActionRunningWBTurboVars.bForcedMirrorSwap`), which is why holding R with
  the ball reproduced a mirrored blend so reliably.

### Far goalkeepers hold a stale pose

Not a bug, but it looks like one and it tripped the float investigation. A
goalkeeper is only posed while the ball is on their half -- both
`cCharacter::PrePhysicsUpdate` and `PostPhysicsUpdate` guard on
`m_v3Position.x * g_pBall->m_v3Position.x > 0`. With play at the other end the
far keeper's node matrices stay frozen at whatever they last were, feet
included. The give-away is that every joint height is bit-identical frame to
frame while the animation id keeps changing.

### Goalkeepers did not defend

- The goalie save table was built from animation milestones, but every
  milestone was zero because `CharacterTriggers` interpreted its callback data
  as a `cSAnim*` instead of the actual `AnimTagCBInfo`. The corrected callback
  lookup in `patches/decomp-late/110-anim-trigger-callback-info-cast.patch`
  restores those milestones and lets keepers time their saves.
- The goalie animation callbacks also carried `this` through 32-bit integers,
  truncating the pointer on the 64-bit host. Their parameters now use
  `std::uintptr_t`, restoring normal movement and animation timing.
  `patches/decomp-late/114-animation-callback-context-host-width.patch`.
- **Verified:** Goalkeepers now track play and defend the goal. The floating
  that was still left after this is fixed too -- see **Players sometimes floated
  above the pitch** above; it was never goalie-specific.

### AI shooting could divide by zero

- `cFielder::DesireShoot` uses the integer `nlRandom` overload for its clear-shot
  windup. With the stock 0.75-second setting, converting
  `(fShotWindupTime - 0.2f)` to an integer produces a zero range. GameCube's
  `divwu` did not trap, but the host remainder operation raised `0xC0000094`.
- `nlRandom` now returns zero for an empty range while still advancing its seed,
  giving the clear-shot path its intended 0.1-second minimum without crashing.
  `patches/decomp-late/113-guard-zero-range-nlrandom.patch`.
- The patch was verified against a clean reconstructed patch stack, and the
  full project builds and links successfully. The optimized object contains a
  zero test and branches around the division while retaining the seed update.

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
`Camera/animcam.cpp` and `Render/Bowser.cpp`. Build with
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
  and has also been fixed.

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
