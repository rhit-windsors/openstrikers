# Plan: audio (written 2026-10-08)

## Where it stands

- The build defines `GOLDEN_DISABLE_AUDIO` (`CMakeLists.txt`). Under it,
  `NL/plat/plataudio.cpp` replaces the whole `PlatAudio` layer (the game's
  wrapper over MusyX: `PlaySFX`, emitters, sound groups, volume groups) with
  stubs, and `GCStream`, `PlatStream`, `AudioLoader`, `movie.cpp` and
  `StreamTrack.h` skip their MusyX calls.
- MusyX itself (`extern/decomp/extern/musyx`, ~19k lines of C: synth, sequencer,
  3D emitters, streams, aux reverb/chorus/delay, and `hw_dspctrl.c` which builds
  the DSP's per-voice parameter blocks) is **not compiled at all**.
- The game-side stream plumbing does run: stream files open, headers are read
  and byte-swapped (README: "Audio stream headers are big-endian too"), and the
  leak in `StreamTrack::StopQStream` was fixed (decomp-late/129).
- `hw_pc.c` (MusyX's PC hardware layer) is a skeleton: no mixer, output calls
  commented out. Aurora has ARAM (`lib/dolphin/AR.cpp`) but no AI/DSP/AX, and
  `compat_shims.cpp` stubs the AI DMA calls.
- On a GameCube the DSP does the mixing: it reads each voice's parameter block
  (`_PB`: sample address and format, ADPCM coefficients and history, loop
  points, sample-rate-converter ratio, per-bus volumes and ramps) every 5 ms
  and mixes into the AI buffer and the aux buses.

## Approach

Keep MusyX and the game's audio code exactly as decompiled, and replace only
the DSP: a host mixer that, every 5 ms frame, walks the voice parameter blocks
MusyX wrote, decodes DSP-ADPCM / PCM16 / PCM8 out of emulated ARAM, resamples,
applies the volumes, mixes main + aux A/B, runs MusyX's own aux callbacks
(its reverb/chorus/delay are plain C), and hands 32 kHz stereo to an SDL3 audio
stream. Writing our own mixer (not copying Dolphin's AX HLE, which is GPL) keeps
the licence clean. The alternative, replacing MusyX with a new engine, would
throw away the game's sound data path and is far more work.

## Building it

`cmake -S . -B build-audio ... -DOPENSTRIKERS_AUDIO=ON` (same flags as `build/`
otherwise). `build/` stays audio-off and playable throughout. MusyX's changes
live in `patches/musyx/`, the game side in `patches/decomp-late/138`, the host
layer in `src/musyx_host.c`. `OPENSTRIKERS_AUDIO_TRACE=1` logs the tick count
and peak active voices every 600 frames.

## Stage 1 notes (2026-10-08)

- MusyX builds on its Dolphin code paths (`MUSY_TARGET_DOLPHIN`), the game
  too; `musyx.h` takes stdint types on a 64-bit host so both agree. The game's
  force-included `compat_shims/musyx_types.h` is only used with audio off.
- `src/musyx_host.c` replaces `hw_dolphin.c`. MusyX's 5 ms tick runs on the
  game thread from `osHostFrameBegin` (1/60 s of ticks per host frame), so its
  interrupt masking is a no-op; `salCtrlDsp` still builds the command list and
  then writes silence.
- 64-bit fixes in MusyX: an out-parameter that is `size_t`, `long` depop
  sums, the ARQ request pointer passed through `u32`, and the ARAM queue
  count, which wrapped because aurora's ARQ completes inside the post.
- **The sound data is big-endian and MusyX reads it in place.**
  `SetupSoundBuffers` now hands each file to the host layer once: the project
  (46 groups: ID lists, FX tables) and pool (883 macros; the curve, keymap and
  layer sections are empty, offset 0) are swapped in place, and the sample
  directory (787 samples) is rebuilt with host-width entries, because each
  entry holds a pointer. Song pages are not swapped: the game never starts a
  sequence.
- Result: matches play, MusyX ticks at 200 Hz and starts voices (up to 7 at
  once). MusyX ends one-shot voices itself, from a play position it tracks on
  the CPU (`playInfo`), so voices finish even without a DSP; what the DSP's
  write-back is still needed for is `hwGetPos` (streams, virtual samples),
  which reads `currentAddr` -- stage 2/3 must advance it.
- Soak gate: two crashes found and fixed on the way (ISSUES.md): stand-in
  structs with GameCube offsets in the sideline-fragment contact, cup ticker
  and ball handler (decomp-late/139, not audio), and power-up emitter handles
  truncated to 32 bits (140, audio-only). Then seed 502 survived 15 minutes:
  510000 frames, 59 matches, 143 goals.

## Stage 2 notes (2026-10-08, in progress)

- `salCtrlDsp` now mixes (`src/musyx_host.c`, "host DSP"): per 1 ms sub-frame
  it applies the voice's queued patch pairs (`patchData`, `update.updNum`),
  then for 32 samples steps the source at the 16.16 ratio (linear
  interpolation; `srcSelect` 2 = none), decodes DSP-ADPCM / PCM16 / PCM8 from
  ARAM (`ARGetStorageAddress`), applies the envelope (`ve`, per-sample delta)
  and main L/R volume (`mix`, per-sample delta over the frame), and writes the
  position back into the block. End of sample jumps to the loop address
  (one-shots loop into ARAM's zero buffer; MusyX ends them itself).
- Output: SDL3 audio stream at 32000 Hz stereo, opened in
  `compat_shims.cpp:pumpAudio`; queue kept between ~15 and 150 ms.
  `OPENSTRIKERS_AUDIO_MUTE=1` (soak.ps1 sets it), `OPENSTRIKERS_AUDIO_RAW=<file>`
  dumps the mix (s16 LE stereo 32 kHz), `OPENSTRIKERS_AUDIO_TRACE=2` logs
  every voice as it starts (format, addresses, ratio, coefficients).
- Advancing positions woke up the stream callback, which exposed two
  game-side bugs (decomp-late/141): MusyX's stream user value is a u32 and
  the game passed a host pointer (now a small handle), and the stream
  header's ADPCM coefficients were handed over still big-endian, which made
  every stream full-scale noise.
- Measured on a 40 s capture: varied levels, no clipping, ~3-4.5k zero
  crossings/s. Confirmed by ear 2026-10-09: music and crowd play; "some of
  the audio works" -- next session, ask which sounds are missing or wrong.
- The "all voices stop ~30 s in" seen once did not come back: a 4-minute
  run kept voices and output going through three goals. Short quiet spells
  (a second or two at level 0) do occur around goals and replays.

## Stage 2 notes, part 2 (2026-10-09): sound effects

"Some of the audio works" turned out to mean only the streams (music,
crowd): 402 of 408 sound-effect starts in a 45 s match failed inside MusyX.
Two 64-bit bugs in MusyX itself (`patches/musyx/002`):

- `dataGetMacro` binary-searched the macro table with a literal element size
  of 8; the entry holds a pointer and is 16 bytes on x64, so every lookup
  missed and `macStart` gave up ("no macro"). Now `sizeof(MAC_SUBTAB)`.
- `voiceAllocate` (2.0.1+ path) had no return statement; CodeWarrior left
  the allocated voice in r3, GCC returns garbage. `voiceAllocateDo` now
  returns the voice and `voiceAllocate` passes it on. (A `-Wreturn-type`
  pass over all of MusyX found no other.)

After: every plain effect start succeeds; 3D emitter starts return -1 by
design (a caller-owned emitter is started later by the 3D update). Up to 57
voices at once, one clipped sample in 40 s.

## Stage 4 notes (2026-10-09): aux buses and reverb

- The mixer now follows the DSP command list per studio: voices go into
  MusyX's own buffers -- `main[salFrame]`, `auxA/auxB[salAuxFrame]`, each
  left/right/surround x 160 s32 -- with all nine volumes and their per-sample
  ramps; studio inputs mix another studio's previous frame; the aux return is
  `aux[(salAuxFrame + 1) % 3]`, which MusyX's `salHandleAuxProcessing` ran
  the effect callback on during the previous frame (two frames of latency,
  as on the hardware); depop fades from the studio block are added; master
  studios are summed, surround matrixed into L/R at -3 dB. Voices write their
  last per-bus output to `pb->dpop` so stopped voices fade out. One-pole
  low-pass (`pb->lpf`) is applied too. ITD is not.
- The game puts one reverb on aux A per stadium (`InitializeReverb`,
  standard or high quality from the audio config). MusyX's reverb inner loops
  are PowerPC paired-single assembly with empty stubs elsewhere; `reverb.c`
  (HI: crosstalk, 3 combs, 3 all-passes, low-pass, pre-delay) and
  `creverb.c` (STD: 2 combs, 2 all-passes) now have C versions of
  `HandleReverb` / `DoCrossTalk` transcribed from the assembly. Chorus is
  still a stub (the game never selects it).
- `PlatAudio::AddAuxEffect` stored the settings pointer through `void**`
  into an `s32` global (overwriting its neighbour) and the shutdown path cast
  the truncated value back to a pointer (decomp-late/142: `intptr_t`).
- `OPENSTRIKERS_AUDIO_TRACE` logs when a studio's effects change
  ("studio 0 effects: aux A on").
- Soak gate (stages 2 and 4 together): seed 502, audio build, muted, 15
  minutes -- survived, 436800 frames, 12 matches, 148 goals.
- Open: volume curve unverified by ear; chorus. (ITD and Pro Logic II:
  see "ITD and Dolby Pro Logic II" below.)

## Stage 3 notes (2026-10-09): streams, and the crowd piling up

- Streams: menu music plays from the legal screens on, and stadium music and
  crowd streams ran through two 15-minute soaks; the file pool stayed healthy.
- "Slightly odd in places" (user, 2026-10-09) was mostly clipping: in a real
  match the output hit full scale every few seconds, up to 2.3% of samples
  clipped. `OPENSTRIKERS_AUDIO_TRACE=3` (lists what is playing when the
  output clips) showed 6-7 copies of the same long crowd samples at once.
  The cause was game-side: `AudioScriptEventMgr`'s settings were
  `static const ... = { 0 }`, written by `Init()` through a cast. MWCC left
  that object writable and `Poll()` read the config values; GCC folds reads
  of a zero-initialised const to 0, so the minimum period between "good
  position" crowd reactions and the bored timers were 0 and those events
  fired every frame (560 in 35 s), each starting a 3.7 s crowd sample
  (decomp-late/143; no other const object in the decomp is written this
  way). After: peak voices ~24 instead of ~58, no clipping in 90 s.
- Three audio calls were compiled out with `#ifndef TARGET_PC` (the heckle
  stream's pan and `sndVolume`, which applies the SFX volume option); they
  now key on `GOLDEN_DISABLE_AUDIO` (decomp-late/144).
- Voice lifetimes were checked against sample length and pitch (trace 2
  logs "voice N end"); they match exactly, so MusyX ends voices correctly.
- Still linear interpolation where the DSP used a 4-tap polyphase filter;
  if pitched-down effects sound dull or grainy, that is next. (Done in
  stage 5's session.)

## Stage 5 notes (2026-10-10): movies, and the polyphase resampler

- Movies were silently skipped, not just silent: `THPSimpleOpen` read the
  big-endian header raw, failed its version check, and `MovieStart` returned
  false. `THPVideoDecode` and `THPAudioDecode` were stubs as well, so this
  stage brings up the video too. Movies: `intromovie.thp` (after the legal
  screens, 65 s), the NLG logos, and 12 Strikers 101 lessons; all 512x416,
  29.97 fps, 32 kHz stereo audio (the lessons' audio track is silent -- their
  narration is the Announcer stream).
- `src/thp_host.c` (both builds): the SDK's video decoder is paired-single
  assembly, so it is rewritten in C from the format -- baseline JPEG, 4:2:0,
  written straight into the three GX I8 textures (8x4 tiles) that the FE
  material combines. THP scans have **no 0xFF00 byte stuffing** (checked over
  every frame of lesson1) and restarts are a byte realign with no RST marker.
  About 2 ms a frame. The audio decoder follows `THPAudio.c`, reading its
  header big-endian, and writes left first (the SDK wrote right first, the
  AI's order; the host mixer is left first).
- `decomp-late/145`: the header, component and info blocks and the per-frame
  size words are swapped; `THPSimpleDecode` indexed `mIsValid` as
  `int[i * 3]` (a 12-byte read buffer; 16 bytes with a 64-bit pointer); the
  GameCube chained onto the AI DMA callback and cast buffers through `u32`, so
  on the host `musyxHostSetPostMix` hands each finished 5 ms frame to the
  player instead (AudioSystem 3: MusyX's frame plus the movie). Without audio
  to pace it (`build/`, or a movie without sound) decoding follows the movie's
  frame rate on `OSGetTime`.
- Verified on the intro movie: the in-game mix correlates 1.0000 with the
  offline decode at every point checked, zero samples of drift over 65 s, no
  clipping, menu music resumes after. In `build/` it ran 3900 host frames
  (65.0 s). The FE's YUV combine renders correctly on aurora unchanged. The
  lesson movies are the same path; lesson 1 was later played through
  Strikers 101 with `OPENSTRIKERS_INPUT` (main menu: four D-pad downs, A,
  then A through captain and team select; the 101 menu opens by itself in
  the match, and A, A, A reaches the lesson and plays its movie).
- The mixer's SRC is now a 4-tap polyphase filter for `srcSelect` 0 (MusyX's
  default), linear for 1, none for 2. Coefficients are our own (Hann-windowed
  sinc, 128 phases, sets read as 8/12/16 kHz cutoffs; MusyX picks set 1).
  Linear interpolation's response swung between 1.0 and 0.38 at 3/4 Nyquist
  depending on phase; set 1 stays within 0.41-0.61.
- Soak gate: seed 502, audio build, muted, 15 minutes -- survived, 447000
  frames, 12 matches, 154 goals (gameplay only; movies are front-end).

## Second sound stack (2026-10-10): logo voices, menu trumpets, silent character select

User report: the Nintendo / Next Level Games logo voices were corrupted,
"random trumpet sounds" played when clicking around the menus, and
characters made no sound when selected. All one bug. `PlatAudio::Initialize`
adds a second MusyX sound stack (ARAM 0x44C000-0x700000) for groups the
front end loads and drops (the logo dialogue groups 44/45, the FE button
group and others), with work memory sized by `sndStackGetSize()` -- a
literal 0x618, `GSTACK_INST` on the GameCube. On x64 the struct is 0xC20
(`gs[128]` holds three pointers each), so MusyX wrote the stack's ARAM
range ~0x600 bytes past the block: the stack read back as base 0, top 0,
every sample pushed onto it got ARAM address 0, and those voices played
whatever sat at the bottom of ARAM. Found by logging each sample upload
(`base 0x0 write 0x4 top 0x0` on stack 0). `patches/musyx/003` returns
`sizeof(GSTACK_INST)`, and also stops `hwSaveSample` reading the sample
header pointer and writing the sample address through `u32` casts (it only
worked while those heap pointers were below 4 GB).

The overrun also wrote 0x44C000 (that stack's ARAM base) into a neighbouring
heap block -- the address the one-off startup crash in aurora's pipeline
cache was reading (ISSUES.md).

Soak gate: seed 503, audio build, muted, 15 minutes -- survived, 414000
frames, 11 matches, 146 goals.

## ITD and Dolby Pro Logic II (2026-10-10)

- ITD: a voice with ITD on (`hwSetITDMode`) has its far ear delayed by
  0-32 samples from the pan (`itdOffTab`). The mixer keeps the delay line in
  MusyX's own per-voice `itdBuffer`, which MusyX clears when such a voice
  starts -- through a pointer it had built via `(u32)` (`patches/musyx/004`).
  Shifts move to their targets one sample per millisecond. **This game never
  turns ITD on**: 0 of 464 voice starts in a 75 s match had it; the support
  is there for completeness.
- Pro Logic II (Options > sound > Dolby): switching restarts the whole sound
  system (`ActivateDPL2`: streams, groups, `Audio::Shutdown`/`Initialize`,
  `sndInit`, the stacks); that survived in the front end. In a DPL2 studio
  MusyX zeroes the surround send and puts the rear left/right on aux B
  (`CalcBusDPL2`), and runs no aux B effect. The mixer encodes front L/R and
  the rears (plus aux A's surround as a centre rear) to Lt/Rt with the usual
  coefficients (0.8718/0.4899) **and** a 90-degree shift between fronts and
  rears (two 4-stage all-pass chains, Niemitalo's public-domain
  coefficients, 90 +/- 1 degrees from 50 Hz to 14 kHz). Without the shift a
  sound sent to front and rear at once cancelled on the left and doubled on
  the right: a match measured L/R RMS 2094/3979; with it 2375/2448, no
  clipping. The output stays stereo -- a Pro Logic II receiver decodes it,
  plain stereo just plays it.
- `OPENSTRIKERS_TEST_SOUND_MODE=DOLBY` (`decomp-late/146`) starts in that
  mode without the menus; `soak.ps1 -Env @{ OPENSTRIKERS_TEST_SOUND_MODE =
  'DOLBY' }` soaks it.
- Soak: seed 504 in Pro Logic II, stopped by hand ~4 minutes in (67200
  frames, no crash) so the user could test; not a full 15-minute gate.

## Stages (each ends with a soak run: audio must not cost stability)

1. **Compile MusyX in, still silent.** (done 2026-10-08, see notes) Add the runtime sources to the build
   with `MUSY_TARGET_DOLPHIN` semantics, turn `GOLDEN_DISABLE_AUDIO` off, and
   satisfy the DSP/AI calls with a null device that just "completes" each
   frame. Expect the usual 64-bit traps in MusyX's data loading (pool/project/
   sample-directory structs read from disc, big-endian, 4-byte pointers).
   Pass: game plays matches as before; `sndFXStart` returns real voice IDs.
2. **Mixer for sound effects.** (done 2026-10-09) ADPCM/PCM decode, SRC, volume ramps, main bus
   to SDL3. Pass: menu clicks and kicks are audible and at the right pitch.
3. **Streams.** (done 2026-10-09; soak pending at time of writing) Music and announcer/crowd streams (`sndStreamAllocEx`, ARAM
   ring buffers fed by `GCStream`). Pass: menu music and stadium music play,
   loop, and stop cleanly; the file pool stays healthy over a soak.
4. **Aux buses and effects.** (reverb done 2026-10-09, ITD and DPL2 2026-10-10; chorus not -- unused) Reverb/chorus/delay callbacks, surround mixdown
   (Dolby Pro Logic II off). Pass: stadium ambience sounds right.
5. **Movies.** (done 2026-10-10, see notes) THP audio (`THPAudioDecode` is stubbed). Lowest priority.

New log lines for soak: voices active per second, stream underruns, and
mixer frames late, so a regression shows up without listening.
