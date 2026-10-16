> Historical investigation log, preserved from the former issue sheet.
> Status statements are snapshots; see [current issues](../../ISSUES.md).
> Code paths in backticks are relative to the repository root.

# OpenStrikers issue sheet

This is the short, current list of visible or gameplay-affecting problems. Keep
the detailed investigation history in [porting history](porting-history.md) and update this sheet as an
issue is reproduced, narrowed down, or fixed.

## Active issues

None open as of 2026-10-10. Audio lives in `build-audio/` ([audio notes](audio.md)); report what sounds wrong there.

## Fixed, keep an eye on

- **Players a little too dark in replays** (reported 2026-10-10, no code
  change). Measured, not fixed: a real goal replay runs in task state 0x10,
  where -- as in the original -- characters keep the fixed in-game key/fill
  lights (camera-relative lighting is only for 0x100 cutscenes,
  `TransitionTask::StateTransition`). Logged in the drawing camera's space
  the key light sits where it does in live play (replay -0.32,0.68,0.66 vs
  live -0.29,0.53,0.80), and ambient is black in both (authentic: the
  game's `world_ambient` is a static zero). Players darken only in shots
  facing into the key light, along with the stadium. The user found replays
  looking fine afterwards. Trap: `OPENSTRIKERS_TEST_AUTO_REPLAY` leaves state
  0x10 within a frame and plays the replay camera in gameplay state, which
  renders players near-black -- do not judge replay lighting with it.
- **Probably fixed (2026-10-10): access violation at startup in aurora's
  pipeline-cache loader** (`pipeline_cache.cpp:1043`, `pipeline_worker`
  thread, reading 0x44c000), seen once in `build-audio/`. 0x44C000 is the
  second MusyX sound stack's ARAM base, which MusyX was writing past its
  undersized work block (`patches/musyx/003`, see [audio notes](audio.md)).
  Report it if it comes back.

These are fixed but were seen by the user in ordinary play, so reopen them with
the new crash report (or a screenshot) if they come back.

- [Lighting and some character materials look incorrect](#lighting-and-some-character-materials-look-incorrect) — fixed; the user judged it "pretty spot on" on 2026-10-08.
- [The match crashes when A is held down through play](#the-match-crashes-when-a-is-held-down-through-play) — fixed; the user found it no longer happens on 2026-10-08.
- [The game crashes during the end-of-game recaps](#the-game-crashes-during-the-end-of-game-recaps) — fixed 2026-10-08; several full games with recaps since, no crash.
- [The match crashes after a couple of minutes](#the-match-crashes-after-a-couple-of-minutes) — fixed 2026-10-08; the last open fault was the heap overrun (decomp-late/133).
- [Crashes that look like heap damage](#crashes-that-look-like-heap-damage) — fixed and verified 2026-10-08 (sideline fragment overrun).
- [The match clock goes negative at the end of a game](#the-match-clock-goes-negative-at-the-end-of-a-game) — fixed 2026-10-08 (decomp-late/135).
- [Super goals do not display the x2 indicator](#super-goals-do-not-display-the-x2-indicator) — fixed 2026-10-08 (decomp-late/136).

## Added

- **1:00 match length** (decomp-late/137, 2026-10-08). Options → Gameplay →
  Game Time, one press left of 2:00. The menu art has no 1:00 slide, so the
  entry reuses the 2:00 slide and replaces its `GAMETIME` text; moving back to
  2:00 restores it. Confirmed in the menu by the user. Handy for checking
  anything that happens at the final whistle.
- **Audio** (`build-audio/`, 2026-10-08 to 10-09). Music, crowd, sound
  effects and stadium reverb. Sound effects were silent until two 64-bit bugs
  in MusyX were fixed (`patches/musyx/002`): see [audio notes](audio.md).
- **Movies** (2026-10-10). The intro movie, NLG logos and Strikers 101
  lessons were being skipped (big-endian THP header); they now play, with
  sound in `build-audio/`. Lesson 1 was played through Strikers 101 with
  scripted input (2026-10-10).

## Issue details

### Crashes that look like heap damage

- **Fixed and verified 2026-10-08.** On the validator build, seeds 401, 402 and
  404 each ran 20 minutes (196,800 frames, 5 full-length matches, 61-68 goals)
  with every allocation and free checked and no findings; before the fix 401
  and 402 crashed the normal build and 401 tripped the validator by frame
  ~40,000.
- **Root cause.** Every sideline
  explosion overran the heap. `SidelineExplodable` allocated each fragment's
  physics object as `sizeof(PhysicsBox)` and constructed the derived
  `SidelineExplosionPhysicsObject` in it: 0x2C for an 0x30 object on the
  GameCube, which the heap's rounding absorbed, but 64 for 72 here, so the
  trailing `mpExplosionFragment` pointer landed on the allocator's bookkeeping
  past the block. `patches/decomp-late/133`.
- **Why it looked like five different bugs.** The damage surfaced minutes later
  wherever the free ring was next walked: the render list's texture AVL tree
  (`0x94000000a0`), abseil's `flat_hash_set::clear` in aurora's texture cache,
  and -- twice, identically, on full-length soak seeds 401 and 402 --
  `MemoryAllocator::Free` on aurora's render worker following a null ring link
  while releasing imgui draw data. Probably also the free-ring corruption at
  frame 7620 noted under "The match crashes after a couple of minutes".
- **How it was found.** A `-DOPENSTRIKERS_MEMALLOC_VALIDATE=ON` build under
  `soak.ps1 -ExePath build-validate\openstrikers.exe` first reported "handed
  out a block still on the free ring", one allocation after a free had put a
  ~2 GB bogus block on the ring. The validator's extent check works in 32 bits
  and the wrapped extent overlapped nothing, so `patches/decomp-late/134` checks
  each free directly (live allocation? header describes an extent inside the
  heap?) and prints the freeing caller. The next run named
  `ExplosionFragment::Deactivate` freeing a 64-byte block whose trailing word
  had been overwritten.
- **Swept for the pattern** (allocate one class, construct another; hard-coded
  GameCube sizes): no other instances.

### The match clock goes negative at the end of a game

- **Expected:** The match clock stops at zero when regulation time expires.
- **Actual:** The displayed clock continues below zero at the end of the game.
- **Priority:** Medium. It is visible in a completed match but does not by
  itself cause the recap crash.
- **Cause:** The game clock keeps running past the match length while a
  last-second shot is in the air (the buzzer-beater window), so the HUD's
  remaining time goes negative. `OverlayHandlerHUD.cpp` converts it with
  `(unsigned long)fRemainingTime`. CodeWarrior clamps a negative float to 0
  there, so retail shows `0.0`; GCC on x64 converts through a signed 64-bit
  integer, so the first second past zero printed negative tenths (`0.-3`)
  and anything longer a garbage minute count.
- **Status:** Fixed 2026-10-08 (decomp-late/135): the remaining time is
  clamped at 0 before conversion. Checked against a host test of the
  conversion; still to be seen in a close game.

### Super goals do not display the x2 indicator

- **Expected:** A super goal displays its x2 scoring indicator during the goal
  presentation.
- **Actual:** The super goal is awarded without displaying the x2 indicator.
- **Priority:** Medium. The scoring presentation is incomplete, but this does
  not by itself cause the recap crash.
- **Cause:** `STSX2Overlay` (`x2_sts.fen`) shows itself on a goal event whose
  goal type is 6 (captain super strike). It read the goal type by raw GC
  offset, `*(u16*)(data + 6) >> 1`. On the host the 8-byte vtable pointer
  covers offset 6, so it never matched and the overlay never showed.
- **Status:** Fixed 2026-10-08 (decomp-late/136): it reads
  `GoalScoredData::uGoalType` like `GoalOverlay` does, and logs
  `openstrikers: x2 overlay shown` to stderr so soak runs can count it.

### The game crashes during the end-of-game recaps

- **Expected:** The end-of-game highlight recaps play through and advance to
  the post-game summary.
- **Actual:** The process exits during the recap sequence after a completed
  match.
- **Priority:** High. It prevents a normally completed match from reaching the
  post-game flow.
- **2026-10-08:** `abort()`, `std::terminate` and CRT fast-fail now report a
  stack through the same `FATAL EXCEPTION` path (see [soak testing](../DEVELOPMENT.md#soak-testing-playing-whole-matches-unattended)), so
  the next reproduction of this will say where it is.
- **Status:** Fixed. After the 2026-10-08 heap and file-pool fixes
  (decomp-late/129 and /133) the user played several complete games, recaps
  included, with no crash. The heap overrun is the likely cause, but this was
  never caught with a stack, so reopen it with the new crash report if it recurs.
- **History:** Separate from the negative clock and missing x2
  indicator. The 2026-09-09 reproduction exited through the Windows runtime
  fast-fail/abort path (`0xC0000409` in `ucrtbase.dll`) near host frame 24001,
  without reaching the game's in-process fatal exception logger. A subsequent
  GDB reproduction was stopped at the user's request before the match reached
  the recaps. Reproduce under GDB and collect all thread stacks at the abort.

### The game crashes after a goal is scored

- **Expected:** A goal plays its celebration and replay, then the match resumes
  from kickoff.
- **Actual:** The game crashes after a goal is scored.
- **Priority:** High. A goal is unavoidable in normal play, so this ends most
  matches. The replay-specific pointer and highlight-layout faults have now
  been fixed, so another goal-to-kickoff run will show whether this was the same
  bug or whether a separate goal-presentation fault remains.
- **Status:** **Fixed, confirmed 2026-09-09.** A 60-second `capture.ps1` run on
  the current build pushed
  `art/fe/goal_overlay.fen`, went 0-2 by frame 1800 and 0-3 by frame 3300, and
  was still rendering normally when the harness killed it on its timer -- no
  `FATAL EXCEPTION`, no early exit. So the build now survives goals that used to
  end the match, which is consistent with this having been the replay pointer
  bug all along.
- **User verification:** Goals, their presentation path and the return to play
  now complete without the old crash.

### Replay backgrounds render incorrectly

- **Expected:** A replay looks like the live match it is replaying.
- **Actual:** The background textures are not right during replay playback.
- **Worst at the start, over the celebration.** The opening stretch of the
  replay -- the part where the scoring character is celebrating -- is where the
  background goes most obviously wrong. Later in the same replay it is less
  pronounced.
- **Priority:** Medium. Replays play through and return to gameplay now, so this
  is cosmetic rather than blocking, but it is on the path every goal takes.
- **Status:** **Fixed, confirmed 2026-09-09.** The intro reproduction and real
  replay presentation now render without the duplicated background layer.
- **Confirmed as view 17, `GLV_DepthOfField`.** Skipping view 17 alone restores
  the full stadium; skipping views 6 or 12 does not. Aurora renders the EFB at
  1280x960, but `glx_DOFGrab` still captured the GameCube-era 640x448 rectangle.
  That copied only the upper-left part of the current frame, then the fullscreen
  DOF quad stretched it over the distant stadium. Full-frame EFB copies now use
  `AuroraGetRenderSize`, in
  `patches/decomp-late/118-scale-full-frame-efb-copies.patch`. An unmasked intro
  capture preserves the intended distant blur while showing the complete
  stadium. The capture evidence and bisect history are in
  [background-layer notes](background-layer.md).
- **The in-stadium screens were one of the causes.** The stadium has screens set
  into the wall below the crowd at both ends of the pitch, driven by
  `Jumbotron`, and they play a different 16-frame animation for kickoff, for a
  goal and for a win. Aurora's `GXInvalidateTexAll` was a no-op, and the
  jumbotron refills one fixed buffer behind texture objects it never re-creates,
  so the first animation to play pinned all sixteen frames: **a goal replayed
  the kickoff animation**. That is on the celebration path, which is where this
  issue is reported to look worst. Fixed by
  `patches/aurora/010-honour-gxinvalidatetexall.patch`; the crowd atlas had the
  same fault and stopped updating with crowd mood.
- **What that does not explain.** The fix was verified at the upload level (24
  jumbotron frames and a crowd atlas now reach the GPU that previously never
  did), not by watching a replay. Whether the replay background still looks
  wrong once the screens are correct is the open question -- watch one and say.
  Do not compare screenshots across runs to decide it; runs do not line up frame
  for frame.
- **Tools for the next look.** `OPENSTRIKERS_JUMBO_DEBUG=1` traces the jumbotron
  state machine and reports, per frame, how many screen surfaces drew and how
  many took an animation texture. `OPENSTRIKERS_JUMBO_FORCE=1` makes the screens
  opaque and gives them a known-good frame, which is how they were located.
  Both are inert unless set; `patches/decomp-late/116-*`.
- **The auto-replay hook does not currently render a replay.** A run on
  2026-09-08 printed `AUTO_REPLAY_TEST begin=0.020 end=6.040` and
  `completed time=3.060` on consecutive lines, between host frames 360 and 420,
  and every dumped frame either side is live gameplay with the match clock still
  running. So it enters and leaves the replay without playing it back, and it
  cannot be used to reproduce this issue until that is understood -- let the AI
  actually score instead. Worth fixing first, since it is the cheap path.
- **Reproduce without scoring:** `OPENSTRIKERS_TEST_AUTO_REPLAY=1` forces a goal
  replay once the buffer fills (see [replay diagnostics](../DEVELOPMENT.md)). Note that it fabricates its
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

### The match crashes after a couple of minutes

- **Expected:** A match runs through to the final whistle.
- **Actual:** The game can crash after playing for a couple of minutes.
- **Priority:** High. This interrupts ordinary match play.
- **Status:** Five faults in the normal-play crash chain were captured and
  fixed on 2026-09-09. The
  first manual diagnostic run died at frame 12900 in
  `PhysicsBanana::Contact`, reading address `0x24`: the code fetched
  `PhysicsAIBall::m_pAIBall` through the GameCube byte offset `other+0x40`,
  which is not its offset in the 64-bit host layout. Patch 119 replaces that
  chain (including the ball owner, shot timer and damage flag) with typed field
  access.
- The rebuilt game passed that failure point, then exposed a second fault at
  frame 16260 during a replay. `Replay::Play` decodes both interpolation
  endpoints each rendered frame, and `RenderSnapshot` applied the discrete
  crowd state for both. When the endpoints straddled a crowd-state transition,
  they started two asynchronous texture loads per frame until the 32-entry
  `DolphinFile` pool returned null. Patch 120 consumes the current endpoint's
  serialized crowd state without applying its side effects.
- A longer normal-play run reached frame 75960 before the same exhausted-file
  failure recurred from live `CrowdManager::SetState`. That exposed the
  underlying boundary bug: the pool-backed class `operator new` can return
  null, but GCC still invokes the constructor for ordinary throwing `new`, so
  the `DolphinFile` constructor wrote its vtable through address zero. Patch
  121 acquires the pool slot explicitly and uses placement new only after a
  successful allocation; queue saturation now follows `nlOpen`'s existing
  null/failure path. The equivalent TDEV path is guarded as well.
- That safe failure exposed a fourth fault at frame 17400: `NisPlayer` reserved
  its arena space and marked a cinematic load started before checking whether
  `nlOpen` succeeded, then passed the null file to `AsyncManager::AddEntry`.
  Patch 122 opens first; if the file pool is temporarily full, it leaves the
  queue entry untouched and retries on a later frame.
- The next run exposed a fifth fault at frame 15900. An audio stream open could
  fail while the file pool was saturated, but `StereoAudioStream::Warm` still
  called `nlSeek` through the null handle. Patch 123 makes stream opening
  report failure, avoids queueing failed streams, and leaves random crowd
  streams eligible for a later retry.
- **Root cause of the pool exhaustion, found 2026-10-08.** Patches 120-123 each
  made one consumer survive a full `DolphinFile` pool; none said why it was
  full. `patches/decomp-late/126` makes an exhausted pool print the path in
  every slot, and the first soak run answered it: all 32 were audio streams,
  with duplicates -- the stadium intro music open three times, the generic goal
  announcer four. `StreamTrack::StopQStream` read the queued stream's
  `TrackOwnsStream` bit from raw GameCube offset `0xB`, which on the host is
  inside the 8-byte `pStream` pointer, so ownership was an arbitrary address
  bit, mostly 0, and a stopped stream was never put on the delete list. Every
  stopped stream kept its file. `patches/decomp-late/129` uses the named
  bitfield. Before it, every seed of a 60-second Legend soak reported
  exhaustion (one 1293 times) and died within its first match; after it, seed
  100 played three matches back to back (0-7, 0-3, 0-2) with no exhaustion at
  all. The guards from 120-123 stay: a full pool is still possible, just no
  longer inevitable.
- **Verification:** The deterministic goal-replay hook now prints both
  `AUTO_REPLAY_TEST begin=...` and `completed time=...`, returns to gameplay,
  and passes the 45-second survival window. A five-minute patch-123 smoke run
  crossed the previous frame-15900 failure point without a fatal marker. A
  later manual run found a separate free-ring corruption at frame 7620.
- **Status:** Fixed 2026-10-08. The frame-7620 free-ring corruption was the
  heap overrun described under
  [Crashes that look like heap damage](#crashes-that-look-like-heap-damage)
  (decomp-late/133). Validator soaks of 20 minutes per seed (five full matches
  each) found nothing after it, and the user has played full games since
  without a crash.

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
- **Priority:** Low. It does not block play.
- **Status:** Fixed, confirmed by the user in play 2026-10-08. Both of the 2026-10-08 root causes
  (the sideline-fragment heap overrun and the leaking stream files that filled
  the `DolphinFile` pool) fit a crash a minute in, and stack overflow is now
  reported by name if that was it instead. If it does recur, reproduce with
  `OPENSTRIKERS_SKIP_FE=1 OPENSTRIKERS_AUTO_A=1` and no input script, and get a
  stack under gdb rather than from the crash handler.

### Match intro screen textures are wrong

- **Expected:** The pre-match intro scene looks like the original game's.
- **Was:** The intro ran without displaying at all while still waiting for
  input. The intro screen now runs and is visible.
- **Status:** **Fixed 2026-09-08.** The animated pennants resolve their frame
  textures, and color-sourced texgens now sample the post-lighting channel, so
  the stadium and character light ramps match the retail scene.
- **Harness behavior:** `capture.ps1` still pulses A with `OPENSTRIKERS_AUTO_A=1`
  to skip the intro and reach gameplay, which is worth keeping regardless.

### Lighting and some character materials look incorrect

- **Expected:** Stadium and character lighting and material shading match the
  original game and remain stable from frame to frame.
- **Actual:** Lighting still glitches, particularly during replays and the
  pre-match intro screens.
- **Priority:** Medium. Gameplay remains functional, but the defect is highly
  visible during presentation cameras.
- **Status:** Fixed. On 2026-10-08 the user judged the lighting "pretty spot
  on" in play. The 2026-09-08 fix corrected the consistently wrong base shading
  by feeding `GX_TG_COLOR0/1` texgens from the completed lighting channel; the
  frame-to-frame glitches reported afterwards are no longer seen. The history
  below is kept for reference: if a glitch comes back, capture it at a tight
  frame cadence before narrowing it.

The intro investigation fixed the texture-animation and base light-ramp bugs.
The duplicated replay/intro background is now also fixed; intermittent lighting
glitches were the last open report, closed 2026-10-08.

### What the captures say so far

Captured 2026-09-08 off the match intro, which is the cheapest of the three to
reproduce: run with `OPENSTRIKERS_SKIP_FE=1` and **no** `OPENSTRIKERS_AUTO_A`,
so the intro sits there waiting on A, and dump with `AURORA_DUMP_FRAME=every:30`.
Frames 600/900/1200 of that run are the evidence below.

- **The symptom is narrower than "background textures are wrong".** Characters,
  grass, tunnel, stadium arches, brickwork and sky all render correctly and look
  right. What is wrong is a handful of **billboard-shaped quads that draw as a
  flat untextured colour** -- hard-edged rectangles and parallelograms hanging
  in the background.
- **The flat colour changes between shots**: white rectangles in the sky at
  frame 600, magenta parallelograms above the characters at 900, red and dark
  purple at 1200. So it is the *material/vertex* colour showing through, not one
  bad asset. Correctly-textured soft clouds appear in the same skies as the flat
  white rectangles, so it is not "clouds are broken" either.
- **Texture decoding is not the fault.** A run with
  `OPENSTRIKERS_TEXTURE_DUMPS=1` uploads and successfully decodes 161 textures
  to RGBA8 (127 CMPR, 31 C8 with their TLUTs resolved, 2 RGBA8, 1 I8); every one
  of them wrote a DDS, which the dump path skips when conversion fails or a TLUT
  is missing. The 161 `texture_replacement: missing runtime key` warnings are
  the replacement system reporting no HD replacement pack, not an error.
- **So the remaining suspects are binding and TEV**, which is where the entry
  above already pointed: a draw that should sample a texmap sampling nothing and
  falling through to the rasterized colour. Aurora has no magenta placeholder,
  so the flat colour is the game's own.

### The reference comparison

Dolphin screenshots of the retail game arrived on 2026-09-08 (16 shots of
Peach & Toad Stadium and the tunnel intro, 4813x2688). The clean pair to work
from is **our intro frame 240** against **the retail wide stadium shot**: same
camera, same scene, so the differences are the bug list.

**Two distinct defects, not one.**

Both defects below were fixed on 2026-09-08 and verified in a new frame-300
Palace capture against the Dolphin reference.

1. **The dome-top pennants draw as flat white rectangles.** Retail has small
   textured flags -- yellow and red on a purple pole -- on the domes and towers.
   We draw a plain white quad, larger than the pennant, in the same place. The
   flat quads in the other intro frames are the same thing seen from other
   angles: white rectangles in the sky at frame 600, magenta parallelograms at
   900, red and purple at 1200. The geometry and its position are right; what it
   is filled with is not.

2. **The stonework is far too bright and too saturated.** Retail's stadium is
   dark olive-grey stone with vines; ours is a bright yellow-green, same hue but
   washed out, and the crowd is vivid where retail's is muted. The purple tower
   banners and the vines are close to correct in both, so this is not a global
   tint -- it is the lit surfaces that are wrong. This is the same defect as the
   "lighting and some character materials look incorrect" entry above; the
   retail tunnel shots make it plainest, where retail is nearly black and ours
   is fully lit.

### What has been ruled out

- **Texture decoding.** `OPENSTRIKERS_TEXTURE_DUMPS=1` uploads and decodes 161
  textures to RGBA8 (127 CMPR, 31 C8 with TLUTs resolved, 2 RGBA8, 1 I8) and
  every one wrote a file; the dump path skips a texture whose conversion fails
  or whose TLUT is missing. The 161 `texture_replacement: missing runtime key`
  warnings are the replacement system reporting no HD pack, not errors.
- **Untextured draws.** Flipping `EnableDebugPrints` in
  `extern/aurora/lib/gx/gx.hpp` dumps every distinct shader config. The intro
  builds 230 of them and **not one** has all its TEV stages sampling
  `GX_TEXMAP_NULL`, and none has zero stages. So the white quads are not draws
  that were never given a texture -- they sample a texmap and get white.
- **Alpha compare.** The 230 configs use four settings, all sensible:
  183 `GX_ALWAYS`, 24 `GX_GREATER ref 0`, 22 `GX_GREATER ref 64`, 1
  `GX_GREATER ref 3`. Cutout billboards are being asked for correctly.
- **Texgen.** 212 of the texgens are `GX_TG_TEX0` through `GX_IDENTITY`, with a
  normal-mapped `GX_TEXMTX9` set for environment mapping and a few `GX_TG_SRTG`.
  Nothing looks mis-translated at the config level.

### Resolution

For (1), the missing handles were the three canonical texture-animation IDs in
the Palace GLG. `GLInventory::GetTextureAnim` bypassed the typed AVL tree with a
hand-written 32-bit object-layout view; that view reads the wrong member on a
64-bit host, so every animation lookup failed and the renderer selected its
white/magenta fallback texture. Calling the AVL tree's real `FindGet` resolves
all three animations and their frame textures. The clean-checkout fix is stored
as `patches/decomp/095-texture-animation-tree-host-layout.patch`.

For (2), the runtime ambient trace showed black, not white. The light records
were valid; the error was downstream in Aurora. `GX_TG_SRTG` with
`GX_TG_COLOR0/1` must use the post-lighting channel result. Aurora used the raw
vertex color, which drove the Palace's 256-pixel light ramp to the bright end
regardless of the surface normal. The shader generator now uses `out.cc0/cc1`,
and shader-info dependency tracking includes channels used only by texgen. The
Aurora fix is stored as `patches/aurora/009-color-texgen-uses-lit-channel.patch`.

The screenshots are in `dolphinscreenshots/` (not committed -- they are retail
game frames).

## Recently fixed

### Power-up sounds outlived their power-ups (2026-10-08, audio build)

The first crash that only the audio build can reach: after 56 matches,
`Audio::Update3DSFXEmitters` followed an emitter to a freed physics object
(`PhysicsObject::GetLinearVelocity` reading `0xffffffffffffffff`). Power-ups
ask `Play()` for the emitter of their in-effect sound and keep it in
`m_uVoiceID`, to stop it when they go away. `Play()` returns a `uintptr_t`,
but the result went through an `unsigned long` local (32 bits on Windows) into
a `u32` member, and the emitter table is a static array in the executable,
above 4 GB. `cGameSFX::StopEmitter` finds the emitter by comparing pointers,
so the truncated one never matched, the emitter was never stopped, and it kept
following the power-up's physics object after the power-up was destroyed.
`patches/decomp-late/140` makes the handle pointer-wide. Invisible with audio
off, where `Play()` returns 0.

### Three more hand-written GameCube offsets (2026-10-08)

A 10-minute soak of the audio build crashed after 14 matches (frame 127689,
reading `0xffffffffffffffff`) in `SidelineExplosionPhysicsObject::Contact`,
when a fast explosion fragment hit a player. The handler reached the player
through stand-in structs padded to the GameCube offsets of
`PhysicsCharacter::m_pAICharacter` (0x8C) and `cCharacter::m_v3Velocity`
(0x30). Not an audio bug: it is rare, and the audio-off build has the same
code. A sweep for padded stand-ins found two more: `AIEventHandler` read the
ball's Magnus-effect flag at 0x3B, and `CupTickerManager` read
`GameInfoManager` fields (knockout flag, game mode, tournament mode at
0x4948) through an accessor struct, so the cup and tournament ticker chose
its messages from garbage. All three use the real fields now
(`patches/decomp-late/139`).

### Unattended soak runs found and fixed four crashes (2026-10-08)

`soak.ps1` plays seeded autoplay matches against a Legend AI and groups
failures by stack (see [soak testing](../DEVELOPMENT.md#soak-testing-playing-whole-matches-unattended)). Its first runs found:

- **A cinematic cleared flags across gigabytes.** `PlayRandomCharDialogue`'s
  clearing loops end at `charDialogueSFX[baseIndex + numRandom]`; for the last
  dialogue group that is one past the 29-entry table. Retail read
  `charDialogueSFXInfo[0]` there (0, so the loop ran no times); GCC's layout puts
  string data there instead (`"CHAR"`, ~1.4 billion), and the loop wrote until it
  faulted. Reached through `Nis::Trigger::Fire`, so any celebration cinematic
  that played that group. `patches/decomp-late/127`.
- **Chain Chomp wrote to `0x7f`.** A failed `Play()` returns
  `GetSndIDError()` (-1) even when the caller asked for an emitter; Chain Chomp
  and the character movement loop cast that to `SFXEmitter*`, it passed every
  `!= NULL` check, and the position write landed at `-1 + 0x80`.
  `patches/decomp-late/128`.
- **The FE package crash after the final whistle** (`FEScene::LoadPackage` ->
  `nlRead(NULL)` on pushing `summary.fen`) and the **pool-exhaustion family**
  behind patches 120-123: the stream ownership bug, see "The match crashes after
  a couple of minutes". `patches/decomp-late/129`.
- **The post-match summary soft-locked, and corrupted a pointer doing it.**
  Seed 100 finished three matches, then sat on the summary screen for 20,000+
  frames with A being pressed throughout. Backing out of the post-game menu
  (B) re-pushes the summary and sets its button state through raw GameCube
  offset `0xC3C`; on the host `mButtonState` is at `0xC54` and `0xC3C` is the
  upper half of `mSlideMenu`. So the summary was left with no A button -- the
  only way on from it -- and with a wild slide-menu pointer.
- **A sweep for the same raw-offset pattern** found two more. Starting a cup
  wrote `CupHubScene::mDoAutoSave` through offset `0x226`, which on the host is
  inside the standings table (a stray 1 in the standings, and no autosave).
  `PhysicsBanana::SetContactInfo` read `PhysicsNPC::mpAINPC` through `0x30`
  (it is at `0x48`) and made a virtual call through whatever was there; retail's
  call is slot `0x0C` of the NPC's vtable, `GetSkinAnimatedNPC_Type`, i.e.
  "a banana with no thrower does not hit Bowser". The other hits of that sweep
  are fine: the `0x4A` packet strides match the host's `sizeof(glModelPacket)`,
  `ShotMeter` has no pointers, and the save-icon offsets are file format.
  `patches/decomp-late/130`.
- **Strikers 101 crashed on entry.** A soak run that had played five matches
  quit to the main menu and wandered into the tutorial, which faulted reading
  through `-1` in `NSNMessengerScene::EnableScrolling`. `FrontEnd::Update` and
  the lesson-select scene converted the ticker overlay to `NSNMessengerScene` by
  subtracting 4 bytes by hand -- the `BaseOverlayHandler` base follows
  `FEIMessenger`, one vtable pointer, which is 8 bytes here. `static_cast`
  instead. `patches/decomp-late/131`.

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
