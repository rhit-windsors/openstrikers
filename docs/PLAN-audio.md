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

## Stages (each ends with a soak run: audio must not cost stability)

1. **Compile MusyX in, still silent.** Add the runtime sources to the build
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
