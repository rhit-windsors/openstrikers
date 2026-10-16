# Cup trophy texture crash (2026-10-16)

Reported from `game-2026-10-16T21-20-19-501Z.log`, using the Windows audio build.
Two matches completed (7–3 and 2–4). At host frame 50993, after the second
result, the game raised `0xc0000005` reading `0xe0b1ecc0`.

## Diagnosis

The matching executable's symbolized stack led through:

```text
glx_MakeTexture
glplatTextureAdd
glxParseTextureBundle / glplatEndLoadTextureBundle
nlLoadEntireFileAsyncCallback
AsyncManager::Service
BeginFrameTask::Run
```

`Presentation::ReadTrophyTexture` uses this asynchronous texture-bundle path.
The synchronous reader already reads `glTexBundleHeader::numTextures` through
its big-endian wrapper, but the asynchronous reader used a native `int` read
at file offset 4. For a six-texture bundle, that returns `0x06000000`
(100,663,296) on Windows. The reader then adds that many dictionary entries
to the file pointer before trying to create even the first texture.

`patches/decomp-late/155-async-texture-bundle-count-big-endian.patch` reads
the count through the existing on-disc header type and uses the dictionary
type's stride for the start of texture data. The patch applies after the
existing stack; the local decomp source and `build-audio/openstrikers.exe`
were updated together.

## Validation

- Confirmed the patch applies to a copy of the previously patched source.
- Compiled focused before/after harnesses from the actual parser function,
  using the project's real bundle types. Read the eight supported cup trophy
  bundles directly from the user's US disc into memory. For every bundle,
  the old parser produced the wrong texture pointer; the fixed parser passed
  texture-count, pointer, dictionary and file-boundary checks on both its add
  and replacement callback paths. No disc assets were written to disk.
- Rebuilt the Windows audio executable successfully.
- The rebuilt game initialized a match and passed the ten-second gameplay
  smoke check. The automated run used an isolated writable cache under
  `build-audio/diagnostics/cache/` and muted audio output.

The focused check stubs GPU texture creation. It does not constitute a full
playthrough of the cup presentation. Local harness sources and results are
under `build-audio/diagnostics/`, which is ignored by Git.
