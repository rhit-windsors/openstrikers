# Background-layer investigation: resolved in the depth-of-field capture

Written 2026-09-09 mid-investigation, for whoever picks this up next.

This is the "backgrounds render wrong in replays, intros, etc" issue from
`ISSUES.md`. It was reproduced on demand, confirmed against the retail game,
bisected from 34 render views to `GLV_DepthOfField`, and fixed on 2026-09-09.
The remaining task is to confirm the same correction during a naturally
triggered scored-goal replay.

## Resolution

The three final single-view captures identified view 17 conclusively:

- Skipping view 6 (`build/bx-v6`) leaves the defect.
- Skipping view 12 (`build/bx-v12`) leaves the defect.
- Skipping view 17 (`build/bx-v17`) restores the stadium.

`glx_DOFGrab` asked GX to copy 640x448, but Aurora's hosted
`GXAdjustForOverscan` makes the active EFB 1280x960 at the default window size.
The copy therefore contained only the upper-left portion of the current frame.
The fullscreen DOF pass stretched that partial image over distant pixels,
creating the apparent extra interior-stadium layer.

`patches/decomp-late/118-scale-full-frame-efb-copies.patch` makes all three
whole-EFB copy/clear operations use `AuroraGetRenderSize`. The unmasked
verification capture is `build/bx-dof-srcfix/frame_600.png`: it shows the full
stadium with the intended distant blur still enabled. `build/bx-v17/frame_600.png`
is the no-DOF control, and `build/bx-dof-full/frame_600.png` is the diagnostic
that exposed the cropped source over the entire screen.

---

## 1. What is actually wrong

The stadium background is occluded and darkened by something drawn in one of
the engine's render views. Characters, the pitch and the sky are fine; the
stands, crowd, arches and vines are swallowed by large dark geometry.

It is **not** replay-specific. It reproduces in the match intro, which is far
cheaper to capture than a goal replay, and the intro is what everything below
uses. That the intro and the replay show the same defect is the working
assumption — see *Open questions*.

### The reference frame

The clean comparison is our **intro frame 600** — Mario and two Toads running
out of the tunnel — against the retail Dolphin capture
`dolphinscreenshots/G4QE01_2026-09-08_17-46-23.png`. Same camera, same shot,
same character poses. Retail shows an open, brightly lit stadium: stone arches
with yellow-and-purple pennants, a full crowd, green vines, a blue jumbotron
screen, the pitch beyond. Ours shows a dark enclosure with a narrow gap.

`dolphinscreenshots/` is not committed (retail frames), but it is on this
machine.

---

## 2. How to reproduce

The disc image is at:

```
C:\Users\Riley\Documents\GC Iso's\Super Mario Strikers (USA).iso
```

The harness is `scratchpad/run-intro.ps1` from the 2026-09-09 session; it is
short enough to retype and is reproduced here in full, because it must **not**
set `OPENSTRIKERS_AUTO_A` — the intro waits on A, and a run left alone sits in
the intro and plays it out, which is the whole point. `capture.ps1` does set
AUTO_A, so it skips past the shot we need.

```powershell
param([string]$OutDir, [string]$Mask = '', [int]$Seconds = 40)
$env:OPENSTRIKERS_ISO = "C:\Users\Riley\Documents\GC Iso's\Super Mario Strikers (USA).iso"
Remove-Item -Recurse -Force $OutDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
foreach ($k in @('OPENSTRIKERS_AUTO_A','OPENSTRIKERS_SKIP_VIEW_MASK','OPENSTRIKERS_SKY_DEBUG')) {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
$env:OPENSTRIKERS_SKIP_FE = '1'
$env:AURORA_DUMP_FRAME = 'every:150'
if ($Mask -ne '') { $env:OPENSTRIKERS_SKIP_VIEW_MASK = $Mask }
$p = Start-Process -FilePath 'C:\Users\Riley\Documents\git\openstrikers\build\openstrikers.exe' `
     -ArgumentList "`"$env:OPENSTRIKERS_ISO`"" -WorkingDirectory $OutDir `
     -RedirectStandardOutput "$OutDir\out.log" -RedirectStandardError "$OutDir\err.log" -PassThru
$null = $p.WaitForExit($Seconds * 1000)
if (-not $p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000) }
```

A 40-second run yields 14 frames; `frame_600.ppm` is the shot that matters.
`frame_150.ppm` and `frame_300.ppm` are wide stadium shots and show the same
defect as darkened, near-black stands — they match retail rows in the reference
sheet too, so they work as a second check.

There is no ImageMagick or PIL on this machine. `ffmpeg` is present and is what
the PPM→PNG conversion and the contact sheets used. `ffmpeg` here is built
without glob support, so tile inputs must be numbered `%02d.png`.

---

## 3. The bisect

`OPENSTRIKERS_SKIP_VIEW_MASK` is a 64-bit mask of `eGLView` ordinals to drop at
the packet loop (`extern/decomp/src/NL/glx/glxSend.cpp`, around line 2973). The
enum is in `extern/decomp/include/NL/gl/gl.h`.

Views that carry packets during the intro, with per-frame packet counts derived
from the `VIEWDBG` lines (those counters are cumulative over 60 frames):

| View | Name | packets/frame |
|---|---|---|
| 2 | `GLV_Skybox` | 1 |
| 3 | `GLV_Shadowed` | 445 |
| 6 | `GLV_WorldShadowed` | 2 |
| 11 | `GLV_Characters` | 35 |
| 12 | `GLV_CoPlanar0` | 10 |
| 16 | `GLV_UnsortedPerspective` | 2 |
| 17 | `GLV_DepthOfField` | 1 |
| 19 | `GLV_Particles` | 57 |
| 20 | `GLV_InvisiblePlane` | 8 |
| 21 | `GLV_ElectricFence` | 22 |
| 29 | `GLV_Transitions` | 2 |
| 31 | `GLV_Anark` | 18 |

Runs so far, all on the build as of commit `c18b368`:

| Run dir | Mask | Views skipped | Background |
|---|---|---|---|
| `build/bx-base` | *(none)* | — | **wrong** |
| `build/bx-grp` | `0xA03A1040` | 6,12,17,19,20,21,29,31 | **correct** |
| `build/bx-a` | `0x21040` | 6,12,17 | **correct** |
| `build/bx-b` | `0xA0100000` | 20,29,31 | **wrong** |

Earlier single-view runs from 2026-09-08 (`build/iv-no3`, `iv-no16`, `iv-no21`,
masks `8`, `0x10000`, `0x200000`) each left the defect in place, so views 3, 16
and 21 are individually cleared.

The subsequent single-view runs confirmed the culprit is view 17,
`GLV_DepthOfField`. Views 6 and 12 are individually cleared.

### Final single-view runs

These were the final bisect commands:

```powershell
.\run-intro.ps1 -OutDir build\bx-v6  -Mask '0x40'
.\run-intro.ps1 -OutDir build\bx-v12 -Mask '0x1000'
.\run-intro.ps1 -OutDir build\bx-v17 -Mask '0x20000'
```

Only the `bx-v17` result looks like the retail shot.

---

## 4. Code the answer is probably in

Once the view is known:

- **View 6 `GLV_WorldShadowed`** — `extern/decomp/src/Game/Render/RenderShadow.cpp:38`
  sets `g_CharacterShadowView = GLV_WorldShadowed`; the attach is at line 551
  (`quad.Attach`) and 555 (`SubdivideAndRender`). Two packets a frame is very
  few for character shadows, which is itself worth asking about.
- **View 12 `GLV_CoPlanar0`** — projected shadows, `RenderShadow.cpp:373` and
  `:404`, both under `g_bCoPlanarProjectedShadows`. Also
  `DrawableModel.cpp:1086` (`DrawPlanarShadow` picks `GLV_CoPlanar0` for
  `bFieldOnlyShadow`) and `DrawCoPlanarReference` at `DrawableModel.cpp:986`,
  which builds a quad from a *shadow bounding square*. A bounding square
  computed wrong would be exactly the "huge dark polygon" this looks like.
- **View 17 `GLV_DepthOfField`** — `extern/decomp/src/Game/Render/depthoffield.cpp:97`,
  one fullscreen packet a frame. A DoF pass that darkens the far field and
  leaves near geometry alone would match the symptom closely.

The view a model lands in is chosen in `DrawableModel::DrawModel`
(`extern/decomp/src/Game/Drawable/DrawableModel.cpp`, around line 443) and the
attach is `glViewAttachModel(view, m_uRenderLayer, newModel)` at line 678.

---

## 5. Ruled out, so do not re-derive it

- **Endianness of the world-object flags.** `WorldObjectChunkData` is overlaid
  on `.wld` file bytes and its scalars are plain `u32`, which looks like a bug
  but is not: `WorldSwapChunkWords(pChunk->GetData(), 0x80, 0xE0)` at
  `world.cpp:583` swaps the whole tail past the two 64-byte name fields, which
  covers the creation flags and the render layer. The other three chunk types
  are swapped correctly too. `nlBE<T>` exists in `nlEndian.h` but is used
  nowhere; that is fine, this pass does the job.
- **Texture decoding, untextured draws, alpha compare, texgen.** All four were
  cleared during the 2026-09-08 intro investigation; see the "What has been
  ruled out" section in `ISSUES.md`.
- **Views 3, 16, 21 individually.** See the table above.
- **The jumbotron stale-frame bug** was real and is fixed
  (`patches/aurora/010-honour-gxinvalidatetexall.patch`), but it is a different
  defect — it made a goal replay the kickoff animation on the in-stadium
  screens. It is not what is darkening the background.

---

## 6. Traps

- **`VIEWDBG` counts do not record the mask.** `glx_DebugViewCounts[view]++`
  runs *before* the skip test, so every run logs identical per-view counts
  whatever mask was set. The logs from a run cannot tell you what was skipped —
  only the command that launched it can. Record the mask in the directory name,
  which is why the run dirs are named the way they are.
- **Do not A/B by pixel diff across runs.** Runs do not line up frame for frame
  even with `OPENSTRIKERS_FIXED_STEP`. What makes the comparison above valid is
  that it is qualitative — "is the stadium visible or not" survives a few
  frames of drift, and the character poses in the frames compared were near
  identical, which is the check that it had not desynced.
- **`OPENSTRIKERS_SKIP_VIEW_MASK` is parsed with `strtoull(s, NULL, 0)`**, so
  `0x`-prefixed hex works and is much easier to read than decimal.
- The 331 `frame_*.ppm` files in the repo root are stale dumps from 2026-09-07,
  gitignored, and unrelated to any of this. They can go.

---

## 7. Open questions

- **Is the replay defect the same defect?** The intro is what was bisected. The
  replay report in `ISSUES.md` says the background is worst at the start of a
  replay, over the celebration. It is plausible but unproven that fixing the
  view fixes both. Once the view is identified, confirm on a replay — let the
  AI actually score rather than using `OPENSTRIKERS_TEST_AUTO_REPLAY`, which
  fabricates a null scorer and does not currently render playback at all.
- **Why so few packets in view 6?** Two a frame, for what should be every
  character's shadow. Either shadows are mostly going somewhere else or most
  are being dropped.
- Once a fix lands, it needs a numbered patch under `patches/decomp-late/` and
  a verification that a clean checkout plus the stack reproduces the tree —
  the patch stack is the source of truth, and it has drifted before.
