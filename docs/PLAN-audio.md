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
  crossings/s. **Not yet confirmed by ear.**
- Open: all voices stop ~30 s in (voices 0, output 0) -- likely streams not
  restarting after a state change; aux A/B buses, surround, ITD, low-pass,
  depop not mixed; volume curve (`mix.vL` scale) unverified.

## Stages (each ends with a soak run: audio must not cost stability)

1. **Compile MusyX in, still silent.** (done 2026-10-08, see notes) Add the runtime sources to the build
   with `MUSY_TARGET_DOLPHIN` semantics, turn `GOLDEN_DISABLE_AUDIO` off, and
   satisfy the DSP/AI calls with a null device that just "completes" each
   frame. Expect the usual 64-bit traps in MusyX's data loading (pool/project/
   sample-directory structs read from disc, big-endian, 4-byte pointers).
   Pass: game plays matches as before; `sndFXStart` returns real voice IDs.
2. **Mixer for sound effects.** ADPCM/PCM decode, SRC, volume ramps, main bus
   to SDL3. Pass: menu clicks and kicks are audible and at the right pitch.
3. **Streams.** Music and announcer/crowd streams (`sndStreamAllocEx`, ARAM
   ring buffers fed by `GCStream`). Pass: menu music and stadium music play,
   loop, and stop cleanly; the file pool stays healthy over a soak.
4. **Aux buses and effects.** Reverb/chorus/delay callbacks, surround mixdown
   (Dolby Pro Logic II off). Pass: stadium ambience sounds right.
5. **Movies.** THP audio (`THPAudioDecode` is stubbed). Lowest priority.

New log lines for soak: voices active per second, stream underruns, and
mixer frames late, so a regression shows up without listening.
