# Building and patching

For the tested Windows build, follow the [Windows play guide](PLAYING-WINDOWS.md#build-from-source).
It configures an audio-enabled `build-audio/` and packages the launcher.
Run the commands below from the repository root unless noted.

## Linux

Linux has built and booted, but has not been rechecked since the rendering
and input work. Enter the development environment with `nix develop`, or
install CMake (3.25 or later), Ninja, GCC, Git and the SDL3 build dependencies
(X11/Wayland, ALSA/PipeWire and Vulkan loader). Node.js and npm are needed
if you want the launcher; `just` is optional for the convenience recipes.

```bash
git clone --recurse-submodules https://github.com/rhit-windsors/openstrikers.git
cd openstrikers
bash ./apply_patches.sh
cmake -S . -B build-audio -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENSTRIKERS_AUDIO=ON
cmake --build build-audio
./build-audio/openstrikers "/path/to/game.iso"
```

With Nix, enter `nix develop` after cloning and before applying patches.
The launcher is packaged in `build-audio/launcher/OpenStrikersLauncher-linux-x64/`
on an x64 Linux host. The packaged Linux launcher is not yet tested.

The `just gen` / `just build` recipes use `build/` and leave audio at its
default, off. `just gen-dbg` / `just build-dbg` use `build-dbg/`.

## Build options

| CMake option | Default | Purpose |
|---|---|---|
| `OPENSTRIKERS_AUDIO` | `OFF` | Enable MusyX and host sound output; use `ON` for playing |
| `OPENSTRIKERS_LAUNCHER` | `ON` | Package the Electron launcher if npm is available |
| `OPENSTRIKERS_MEMALLOC_VALIDATE` | `OFF` | Validate the allocator on every allocation/free |
| `OPENSTRIKERS_CHECK_GCPTR` | `OFF` | Abort on invalid non-MEM1 pointers in `nlGCPtr32` |
| `OPENSTRIKERS_FRAME_STATS` | `OFF` | Log rendering statistics |

See [Testing and debugging](DEVELOPMENT.md) before enabling diagnostics.
`-DFETCHCONTENT_BASE_DIR=<directory>` can keep downloaded dependencies outside
the build directory. Rebuild an already-patched checkout with
`cmake --build build-audio`; patch application is a separate first-time step.

## Applying the patch stack

The submodules are checked out at upstream commits and are **not** buildable
as-is. `patches/` holds the diffs that make them build; apply them with:

```bash
just patch          # or: bash ./apply_patches.sh
```

`apply_patches.sh` resets each submodule to a clean `HEAD` before applying, so
it **refuses to run if a submodule has uncommitted edits** — that is a guard
against silently discarding work that has not been saved as a patch yet.

An applied patch stack itself makes the submodules dirty. Do not rerun this
script as a routine build step. Before resetting or updating dependencies,
save any additional work against the patched baseline using the workflow below.
For a clean rebuild without resetting submodules, configure a new build directory.
`just unpatch` discards submodule changes after a prompt; use it only when all
work you need has been saved.

**On Windows, a fresh clone looks dirty before you touch anything.**
`git status` in `extern/decomp` reports `include/dolphin/GX.h` and
`include/dolphin/VI.h` as modified. They are symlinks whose targets are `gx.h`
and `vi.h` — the same names on a case-insensitive filesystem — so git writes the
target's content and then sees the link as changed. Nothing is wrong and there
is no edit to save; `apply_patches.sh` knows about these two by name and steps
over them. Any *other* path in that list is a real unsaved edit.

**Patch files must not be line-ending normalised.** A diff of CRLF sources
carries a literal CR at the end of every context, `+` and `-` line, as content,
while its `---`/`+++`/`@@` headers do not. Normalisation cannot tell the two
apart, so it rewrites the file uniformly and the patch stops applying — one
`.patch` here went from 275 CR bytes to 0 on the way into git. `.gitattributes`
marks `*.patch` as `-text` to keep them byte-exact. If you add a patch directory,
make sure it is covered.

## Patch stages, and why the order matters

The patches are applied in five stages, in this order:

| Stage | Applied to | What lives there |
|---|---|---|
| `patches/decomp/` | `extern/decomp` | fixes that apply to the plain upstream sources |
| `patches/musyx/` | `extern/decomp/extern/musyx` | the audio submodule |
| `patches/aurora/` | `extern/aurora` | the system-library re-implementation |
| `patches/tmp/` | `extern/decomp` | known workarounds, not real fixes |
| `patches/decomp-late/` | `extern/decomp` | decomp fixes that depend on the `tmp` stage |

The last stage exists because of a trap that is easy to fall into and hard to
see. The workflow below builds its baseline by applying **every** stage, which
is the only way to get a tree that matches what you are editing. So a patch you
save is written against a post-`tmp` tree. If it touches a file that a `tmp`
patch also touches — `GameInfo.cpp` and `main.cpp` are the ones that bite —
its context does not exist yet when the `decomp` stage runs, and it will fail
to apply even though it looked correct when you made it. Put such a patch in
`patches/decomp-late/`.

**Check your patch actually applies from clean**, in a throwaway worktree,
before you consider it saved:

```bash
patch_workdir=$(mktemp -d)
git -C extern/decomp worktree add --detach "$patch_workdir/decomp" HEAD
for p in patches/decomp/*.patch patches/tmp/*.patch patches/decomp-late/*.patch; do
    git -C "$patch_workdir/decomp" apply --check "$PWD/$p" || echo "FAILS: $p"
    git -C "$patch_workdir/decomp" apply "$PWD/$p"
done
git -C extern/decomp worktree remove --force "$patch_workdir/decomp"
```

This stack has drifted from the tree once already, silently, and cost a session
to reconstruct — `patches/decomp-late/100-*` is the reconstruction, and its
header records what was lost and what had to be folded into it. The invariant
worth protecting is that a clean checkout plus the patch stack equals the tree
you are building.

## Saving your own changes as a patch

`git -C extern/decomp diff` will **not** do what you want: the working tree has
every existing patch applied, so it emits the whole cumulative stack rather than
your delta. Build the current baseline in a throwaway worktree and diff against
that instead:

```bash
patch_workdir=$(mktemp -d)
git -C extern/decomp worktree add --detach "$patch_workdir/decomp" HEAD
for p in patches/decomp/*.patch patches/tmp/*.patch patches/decomp-late/*.patch; do
    git -C "$patch_workdir/decomp" apply "$PWD/$p"
done
# for each file that differs:
#   diff -u --label a/<f> --label b/<f> "$patch_workdir/decomp/<f>" "extern/decomp/<f>"
git -C extern/decomp worktree remove --force "$patch_workdir/decomp"
```

**Mind the line endings.** The decomp's sources are CRLF in the working tree. An
editor (or script) that rewrites a file as LF produces a patch that is tens of
thousands of lines of line-ending churn concealing a two-line change. Check the
patch size against the size of the change you actually made.

---

## Building with audio

Audio is off by default: `build/` defines `GOLDEN_DISABLE_AUDIO`, which stubs
the game's audio layer. Configure a second build directory with
`-DOPENSTRIKERS_AUDIO=ON` (same flags otherwise):

```bash
cmake -S . -B build-audio -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DOPENSTRIKERS_AUDIO=ON
cmake --build build-audio
```

This compiles the game's sound engine, MusyX (`extern/decomp/extern/musyx`,
patched by `patches/musyx/`), on its GameCube code paths. The one piece of
GameCube hardware it needs that nothing provides is the DSP, which mixed the
voices; `src/musyx_host.c` stands in for it, decoding DSP-ADPCM and PCM from
emulated ARAM, resampling, applying volumes and envelopes, and mixing the main
and effect buses, and MusyX's own reverb runs on the effect bus. Output goes to
an SDL3 audio stream at 32 kHz stereo. Movies (THP) mix their sound into the
same output. Environment switches:

| Variable | Effect |
|---|---|
| `OPENSTRIKERS_AUDIO_MUTE=1` | mix but do not open an audio device (`soak.ps1` sets it) |
| `OPENSTRIKERS_AUDIO_RAW=<file>` | also write the mix to a file (s16 little-endian, stereo, 32 kHz) |
| `OPENSTRIKERS_AUDIO_TRACE=1` | log audio ticks, peak voices and peak level every 600 frames |
| `OPENSTRIKERS_AUDIO_TRACE=2` | also log each voice as it starts |
| `OPENSTRIKERS_TEST_SOUND_MODE=DOLBY` | start in that sound mode (`STEREO`, `MONO` too), as if picked in the options -- the front end applies it with the saved options, a skip-front-end run starts in it |

---

## Working on the launcher

The launcher is an Electron app in `launcher/`. To run it from source,
use PowerShell on Windows:

```powershell
Set-Location launcher
npm.cmd ci
npm.cmd start
npm.cmd test
```

On Linux use `npm` instead of `npm.cmd`. CMake normally installs dependencies
and packages the launcher automatically. For a game-only build, configure with
`-DOPENSTRIKERS_LAUNCHER=OFF` and launch the game directly.

## Sharing a Windows build

The recipient needs the complete runtime, rather than the game executable alone.
After a successful audio build, copy these into a separate distribution folder:

- `build-audio/openstrikers.exe`.
- **Every DLL** beside that executable (Dawn, SDL3, nod, DXC and compiler/runtime dependencies).
- `build-audio/initial_pipeline_cache.db`.
- The complete `build-audio/launcher/OpenStrikersLauncher-win32-x64/` directory,
  keeping it under `launcher/` in the distribution folder.

Zip that distribution folder. Build intermediates such as `CMakeFiles/` and
`_deps/` are not needed. Keep the entire Electron directory, including its
resources, locales and supporting files. The recipient can extract it, open
the launcher, and choose their own disc image in Settings. Include a link to
the [Windows play guide](PLAYING-WINDOWS.md#open-the-launcher).

The build copies runtime DLLs using Aurora and
`cmake/CopyCompilerRuntimeDLLs.cmake`. Launch the copied game from an ordinary
PowerShell or Explorer to check it without relying on the MSYS2 shell's PATH.
A missing DLL can stop Windows before the game prints any log
(`0xC0000135`). Distribute the shader seed so a new PC can compile known
pipelines during startup. Do not include your disc image, extracted game data,
memory-card saves, personal settings or logs in the archive.
