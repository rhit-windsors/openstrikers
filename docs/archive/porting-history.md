# Porting history

Historical status and fix writeups extracted from the former README. Statements
about current problems, build behavior and test results describe the sessions
recorded here. Use the [Windows guide](../PLAYING-WINDOWS.md) for setup and
[issue tracker](../../ISSUES.md) for current status. Code paths in backticks
are relative to the repository root.

## Status

Whole matches play from kickoff to the recaps and back to the menu, with
goals, super strikes, replays and celebration cinematics. Network play works:
the first real test, two PCs on a LAN on 2026-10-15, played a match in
sync. There are no known open issues (`ISSUES.md`). Audio is complete but still
lives in the separate `build-audio/` build.

| | |
|---|---|
| Windows (native, MSYS2 UCRT64) | front end complete; full matches play -- `smoke-test.ps1` passes |
| Network play | up to 8 machines, one human per fielder, delay-based lockstep; verified on two PCs over a LAN (one reading the disc image from a network share) -- see [Network play](../NETPLAY.md). Internet play needs a forwarded port or a VPN such as Tailscale; a relay server is designed for but not built |
| Launcher | `OpenStrikersLauncher.exe` (Electron, `launcher/`): play locally with names over heads, or an online lobby where everyone picks their fielders and the host starts the match on every computer, or a co-op cup for up to four against the computer; disc, resolution up to 4K, fullscreen, 4:3 / 16:9 / stretch |
| Stability | unattended `soak.ps1` runs play match after match: 15 minutes of fast-forwarded play (59 matches, 143 goals) without a crash, and the allocator validator finds no heap damage |
| Linux / WSL (GCC) | builds and boots (not re-verified since the render/input work) |
| Rendering | 2D/front end, the in-match HUD, stadium, lighting and animated characters draw correctly. A machine with an empty shader cache can show brief black patches until its shaders compile; the build ships a starter cache so most compile during boot (see [Running](../PLAYING-WINDOWS.md)) |
| Match options | adds a 1:00 game time (Options → Gameplay → Game Time) alongside the original lengths |
| Characters | stay planted on the pitch -- see [Characters floated because a mirrored blend read a dead stack slot](#characters-floated-because-a-mirrored-blend-read-a-dead-stack-slot) |
| 3D models | `.bmd` loads (342 models for the palace) plus skin meshes; shared-array geometry corruption is fixed -- see [The match rendering fix](#the-match-rendering-fix) |
| Leaving a match | quitting from the pause menu returns to the main menu and keeps running |
| Replays | snapshot playback completes on the 64-bit host; pointer-width and highlight-layout faults fixed |
| Input | keyboard on port 0, plus any SDL gamepad, paced to 60 Hz |
| Memory card | creates, writes and reads its save file |
| Audio | off in `build/`; `-DOPENSTRIKERS_AUDIO=ON` (`build-audio/`) plays music, crowd, sound effects and stadium reverb through MusyX and a host stand-in for the DSP -- see [Building with audio](../BUILDING.md) and [audio notes](audio.md). The Dolby Pro Logic II sound mode encodes the rear channels into the stereo output |
| Movies | the intro, the NLG logo and the Strikers 101 lessons play in both builds (`src/thp_host.c` decodes THP video; their sound needs `build-audio/`) |

The game boots, loads the front-end `.fen` scene package, runs its timeline
animations, presents the health-and-safety screen, checks the memory card,
creates its save file if there is not one, and lands on a real, navigable front
end with correctly localized text. The legal/logo sequence cross-fades
correctly, the main menu works, and an exhibition match can be set up all the
way through: captain grid, sidekick grid, the choose-a-side widget, and stadium
select. Picking a stadium tears down the front end and starts loading the match.

### A match now loads and runs

`smoke-test.ps1` sets `OPENSTRIKERS_SKIP_FE=1`, starts an exhibition match at a
Toad Stadium, waits for the `-- Memory upon Exiting InitializeGameState` marker
on stderr and then watches the process stay alive. It passes, and holds for 90
seconds. Six bugs stood between "the load reaches the first frame" and that, and
three of them are new classes worth stating on their own.

- **The display list is big-endian everywhere it is read on the CPU, not just in
  `DrawableModel`.** `NetMeshModelLoader::LoadGeometryFromModel` and
  `ReadEdgesFromGeometryPacket` and `ModeledScreenTransition::RenderOutline` each
  walk a `DisplayList` and read a vertex index out of it natively. The net-mesh
  loader takes `max(index) + 1` as its particle count, so a true maximum of 255
  came back as 65280 and `DrawableNetMesh::Render` asked the frame allocator for
  783372 bytes of vertex memory out of a 896 KB arena. The rule from
  `GetAABBDimensions` holds everywhere: **anything read back out of a GX display
  list on the CPU has to be swapped.**
- **A by-value base subobject that the game constructs a derived type into is a
  devirtualisation trap.** `Event::m_data` is declared `EventData m_data;` and
  every event placement-news its own payload -- `NISData`, `GoalScoredData`,
  `PenaltyData` -- on top of it. A compiler may assume a by-value member's
  dynamic type is its declared type, and GCC duly folded `m_data.GetID()` to
  `EventData::GetID()`, which answers -1. `Event::GetData` therefore decided
  *every* event in the game carried no payload and handed back NULL, and the
  first handler to use one without checking took the process down. The payload is
  now reached only through `Event::Data()`/`Event::Payload()`, which
  `std::launder`, and the ~200 `m_data` uses go through them.
- Those same events were being built at the wrong offset. The GameCube header
  ahead of the payload is 0x10 bytes, so 46 sites spell
  `new ((u8*)pEvent + 0x10) Foo()`. Two 8-byte ring pointers put `m_data` at 0x18
  here, so the literal landed the payload's vtable pointer on
  `m_uEventID`/`m_nReferenceCount` and `DispatchEvents` indexed `m_dest` with the
  low half of a code address. The pool stride grew too: every size handed to
  `CreateValidEvent` is a GameCube total, and a GC-sized slot no longer holds the
  object placed in it.
- **A `void`-in-practice function that is declared to return a pointer.**
  `ModeledScreenTransition::LoadFromParser` is declared
  `ModeledScreenTransition*` and returns nothing; every caller ignores the value,
  so on PowerPC it was merely untidy. GCC emits no return at all and control runs
  into whichever function it laid down next -- one `model` line in the transitions
  script became a million `cPoseAccumulator` allocations and an exhausted heap.
  This is the third instance of this class in the port, after
  `SidelineExplodableTextureLoadCallback` and `FileWriteIconCB`; `-Wreturn-type`
  is on, and the build log is now clean of it.
- `parse_group` in `EffectsGroup.cpp` indexes its 64-entry stack array of
  `EffectsSpec` by shifting a running byte offset right by 6, because the struct
  is 0x40 bytes on the GameCube. Two pointers inside it grow here, so the shift
  picked a slot short of the one just written and eventually ran off the end of
  the stack frame -- a `call` through `0x100000000`. Same story for the
  `specCount << 6` memcpy that follows.
- aurora's GX backend had no `GX_CULL_ALL`, and `DEFAULT_FATAL`'d on it the first
  time the match asked for one. WebGPU has no "cull everything" mode, so the
  pipeline now rasterises as if culling back faces and is given an empty colour
  write mask and no depth writes (`patches/aurora/007`).

**`__ctype_map` was a table of zeroes.** The MSL `ctype.c` that carries the real
one is not in the build, and `src/compat_shims.cpp` supplied a zero-filled stand-in.
MSL's `isalpha`/`isspace`/`isdigit` inlines index it, and so does the game
directly -- `world.cpp` tests `__ctype_map[c] & __digit`. The visible casualty was
`SimpleParser::IsWhitespace`, which is `(__ctype_map[c] & 0x6) != 0`: nothing was
ever whitespace, so a whole line came back as a single token, `"begin
electric_fence"` never compared equal to `"begin"`, and `art/effects/scripts.fx`
parsed to **zero** effects groups. `fxGetGroup` then returned NULL for every
effect and the first one the match tried to emit dereferenced it. An earlier
patch had papered over this by rewriting the test as `isspace(c & 0x6)`, which is
false for the same reason; that patch is deleted and the table is filled in.

Filling the table woke up a lot of previously dead parsing at once -- the
transitions script, the effects bundle, the sound-event script -- which is where
the transition and effects-spec bugs above came from. Expect that: a parser that
silently produced nothing was hiding every bug in the code it feeds.

### The replay crash fix

Replay serialization contained a second family of 32-bit-host assumptions.
`EmissionController`, `cPN_SAnimController`, `DrawableCharacter`, and
`EmissionManager` wrote live pointers -- including effect update and completion
callbacks -- through `unsigned int` or Windows `unsigned long`. Both are 32 bits
in this build. A replay could therefore restore only the low half of a callback
address and jump outside the executable; one captured failure jumped to
`0xa52bc800` while the module was loaded at `0x7ff7a51e0000`.

`patches/decomp-late/106-replay-host-pointer-width-and-highlight-layout.patch`
serializes those fields as `std::uintptr_t` and explicitly restores the typed
pointers when loading a frame. It also replaces `ReplayChoreo::SaveHighlight`'s
raw GameCube offsets from `this` with `mHighlights[idx]` and its named members.
That matters because the pointers before the highlight array expand on this
host, so offsets `0x230` through `0x260` no longer name the array. Highlight
playback now reads the saved goal and replay-pad members directly as well.

The playback path was verified by recording 4.02 seconds of a live match,
switching to replay state `0x20000`, playing through the complete buffer, and
returning to gameplay. The former invalid callback jump did not recur. A later
AI-shot divide-by-zero was unrelated and is tracked in `ISSUES.md`.

Auditing the tree against the patch stack afterwards turned up four more
instances of the same class, none of them in replay code:
`patches/decomp-late/107` widens sound handles (a `SFXEmitter*` returned as
`unsigned long` through a virtual chain), `108` widens the script VM's stack so
calls and string loads stop returning to half addresses, `109` stops
`nlAsyncLoadFileToVirtualMemory` truncating its user parameter, and `110` fixes
an animation-trigger cast that silently kept every trigger in the game from ever
matching -- which is what left the goalie save table full of zeroes.

The lesson worth keeping: three of those four never crashed. A truncated pointer
that happens to land on mapped memory, or a comparison that simply never matches,
just makes a feature quietly do nothing. Grep the build warnings before assuming
a subsystem is unimplemented.

### Characters floated because a mirrored blend read a dead stack slot

Outfield players and goalkeepers used to rise off the pitch during normal play.
The ball carrier did too, and the ball kept trailing along the ground behind
them -- which is the whole diagnosis in one sentence, because it says the drawn
skeleton and `m_v3Position` had come apart. Gameplay, physics and the ball all
follow `m_v3Position`; only the pose was moving.

`cPoseAccumulator::BlendTrans` handles a mirrored animation by rewriting the
incoming translation into a local and repointing its parameter at it:

```cpp
if (bMirror) {
    nlVector3 vtemp;          // declared inside the block
    ...
    pTrans = &vtemp;
}                             // vtemp's lifetime ends here
...
e->t.z = inv * e->t.z + t * pTrans->z;   // read afterwards
```

`BlendRot`, immediately above it, declares its `qtemp` *outside* the block.
That is the correct shape; this one had drifted. MWCC left the slot alone and
the retail game worked by accident, but GCC reuses it, so every mirrored blend
picked up whatever the compiler had since put there. On a limb node that is a
small wrong offset. On the animated root node, whose translation is the
character's hip height, it is the whole skeleton's height.
`patches/decomp-late/115-mirrored-blend-trans-dangling-local.patch`.

R "causing" the hover was a red herring twice over. Gameplay *does* read R --
`cAIPad::IsTurboPressed` calls `GetPressure(0x14, true)`, and `0x14` is
`PAD_TURBO` written as a literal, which is why grepping for the enum name found
nothing. R plus a deflected stick is turbo, and the turbo run states force
mirror swaps (`mActionRunningWBTurboVars.bForcedMirrorSwap`), so holding R with
the ball reliably put a mirrored controller in the pose tree. The button was
never doing anything to the physics.

**Two lessons worth more than the fix.**

*Measure the layer, not the symptom.* "Floating" could be a bad vertical
position or a bad pose, and those live in different code. Logging the lower
foot's posed world height next to `m_v3Position.z` separated them in one run:
`m_v3Position.z` never left zero. A second pass walked the pose tree and
re-derived what the blend should have produced -- every animation feeding the
root node read about 0.29 while the accumulator held 0.84. A convex blend cannot
do that, which is what named the accumulator rather than the animations.

*Undefined behaviour moves when you look at it.* Adding an **inert** branch to
`cSAnim::BlendTrans` -- code that could not change behaviour -- made the float
disappear entirely, twice, because a different tenant landed in the stack slot.
If a symptom vanishes when you add a probe that cannot possibly matter, stop
treating it as noise. Diagnostics for this class have to live in cold code; the
ones that worked hung off `cCharacter::PostPhysicsUpdate` and touched nothing in
the animation path.

Verified by instrumenting the lower foot's posed height: before the fix a
two-minute match logged 46-93 sustained lifts, feet held about 0.87 above the
pitch for up to 320 frames. After it, none, with or without R held. The one
detection left is a *far* goalkeeper holding a frozen pose, which is the
original game's own optimisation -- `cCharacter::PrePhysicsUpdate` and
`PostPhysicsUpdate` both skip posing a keeper while the ball is on the other
half, so the node matrices simply stay where they were. The give-away is that
every joint height is bit-identical frame to frame while the animation id keeps
changing.

### The in-stadium screens showed a stale animation

Super Mario Strikers has screens set into the stadium wall below the crowd, at
both ends of the pitch. They play a 16-frame animation -- one set for kickoff,
one for a goal, one for a win -- and the presentation script drives them:
opcode 5 loads a bundle, 13 plays it, 25 stops it, 28 unloads it. The screens
are model packets carrying object-creation flag bit 16; `DrawableModel::DrawModel`
swaps `Jumbotron::m_CurrentTexture` onto texmap 0 for those packets.

They were showing the wrong animation, and the cause was in aurora rather than
in the game.

`Jumbotron::Initialize` creates sixteen `PlatTexture`s over one fixed buffer,
zero-fills it, and calls `Prepare()` -- which is `GXInitTexObj`. Every later
bundle load DMAs new pixels into that same buffer and calls `GXInvalidateTexAll`.
It never re-creates the texture objects, because on real hardware it does not
need to: GX reads texture pixels straight out of main memory, so new bytes plus
an invalidate is all it takes.

Aurora keys its upload cache on texture-object identity -- `texObjId` plus
`texDataVersion` -- and neither changes when only the bytes behind the object
change. `GXInvalidateTexAll` was `// no-op?`. So the first animation to play
pinned all sixteen frames, and every later bundle was invisible to the renderer:
a goal replayed the kickoff animation. The crowd atlas
(`CrowdManager::Initialize`) uses the identical pattern, so crowd mood changes
never reached the GPU either.

Honouring the call means dropping the object-identity cache; the content cache
is keyed on a hash of the source bytes and is left alone, so a redundant
invalidate costs a re-hash rather than a re-upload.
`patches/aurora/010-honour-gxinvalidatetexall.patch`.

**Measured, not assumed.** Two 90-second matches with
`OPENSTRIKERS_TEXTURE_DUMPS=1`, dumps cleared between runs: the fixed run
uploads 24 more 128x128 CMPR textures -- a second and third jumbotron frame set
-- and a second 256x512 crowd atlas, none of which the unfixed run uploads at
all. It did this while loading *fewer* jumbotron bundles (2 against 3), so it is
not a matter of one run simply getting further.

**The lesson for this port.** Anywhere the game refills a buffer that a live
`GXTexObj` points at, aurora needs telling. `GXInvalidateTexAll` is the game
telling it. Grep for the pattern -- `CreateWithMemory` followed by
`glBeginLoadTextureBundle` on the same base address -- when a texture looks
correct once and then stops updating. `glplatTextureReplace` is the *other* way
the game refills a texture, and that one is safe: it re-runs `GXInitTexObj`, so
the object identity changes and aurora re-uploads on its own.

**Diagnostics.** `OPENSTRIKERS_JUMBO_DEBUG=1` traces the state machine, the
frame the game selected, the bytes actually in that frame's buffer, and
per-frame counts of how many flagged models were submitted, drew, and took the
animation texture -- plus, when one did not, which early return in `DrawModel`
ate it. `OPENSTRIKERS_JUMBO_FORCE=1` keeps every flagged surface opaque and
hands it a known-good frame, which is how the screens were located on screen in
the first place. `patches/decomp-late/116-jumbotron-screen-diagnostics.patch`.

Note that the screens are *deliberately* transparent in the gameplay camera:
`DoTranslucency` in `world.cpp` forces translucency to zero for creation flags
0x2000 and 0x8000 while the gameplay camera is active. A trace showing the
screens dropped for `translucency == 0` through a whole match is correct
behaviour, not a fault -- they are meant to appear only in presentation cameras.

### The ball flashed white and magenta with MSAA on

With anti-aliasing on, the ball and the Palace's dugout canopies flashed
white and magenta, four frames each. That is the game's own missing-texture
stand-in: when `glx_SwitchTexture` cannot find a packet's texture it draws
`global/white` and `global/magenta` in turn (`(frame & 4) >> 2`), and
`OPENSTRIKERS_TRACE_MISSING_TEXTURES=1` names the handles it could not find.

They were not missing. Some texture handles are `PlatTexture` pointers, and
`glx_GetTex` recognised a pointer by a 0x80 high byte -- the first 16MB of
MEM1, where the GameCube's heap always put them. The host's MEM1 is 128MB and
its heap is laid out differently; with MSAA on, Aurora allocates more of it, the
headers land at 0x81000000 and up, and the test sent them to the hash lookup,
which failed. Without MSAA the same headers sit just below 0x81000000, so any
other growth in host allocations would have tripped it too.
`patches/decomp-late/154-texture-pointer-handles-anywhere-in-mem1.patch`
accepts any aligned address inside the host's MEM1; the `PTEX` tag check still
guards the dereference.

How it was found, since every renderer-side suspect (EFB copies, resolves,
blending, dual-source alpha, depth of field) checked out identical: record one
straight-to-match session with `OPENSTRIKERS_SKIP_FE=1` and
`OPENSTRIKERS_NET_RECORD`, replay it with and without `--msaa` under
`OPENSTRIKERS_NET_REPLAY`, and the same frame numbers are the same picture
(the replay confirms the game state is identical throughout). With that A/B,
masking one shader at a time found the draw, texture dumps showed it bound to
a `global.glt` texture instead of `ball.glt`'s, and a backtrace on that bind
led to the missing-texture fallback.

### The match rendering fix

The enormous triangles and flat bands in the match are fixed. Stadium geometry,
the ball and skinned characters now render at the correct scale and continue to
animate during a long capture. The remaining visible defect is separate: some
character materials are overbright or flat yellow/lime, so lighting and material
translation still need work.

The corruption came from how shared vertex arrays were sized for Aurora. Packets
within a model share array pointers, and `glx_SwitchStreams` does not rebind an
unchanged stream for every packet. The first packet could therefore upload only
858 bytes (143 positions), while later packets using that same binding referenced
indices as high as 2014 or 2684. Those later draws read beyond the uploaded GPU
buffer even though their CPU-side positions and transforms were valid.

Patch 095 replaced the original `numVertices * stride` estimate with a packet's
maximum index, which was necessary but still too small for shared arrays. Patch
097 scans all packets before their raw index buffers become display lists, stores
the model-wide maximum index in host-only `DisplayList` metadata, and sizes the
first array upload from that high-water mark. Standalone display-list callers
retain a packet-local maximum as a safe default.

Useful diagnostics from the investigation:

- Draws were being submitted (735 calls and about 1.34 MB of vertices per frame),
  and viewport, scissor, camera and little-endian vertex decoding were correct.
- Both rigid stadium packets and skinned character packets failed independently,
  which ruled out skinning as the shared cause.
- For camera logging, sample periodically rather than inspecting only the first
  few view switches. Early initialization intentionally contains a zero view
  matrix and a projection made from the clamped 1-degree placeholder FOV.

### Model loading

That load is now well past the model files. `glxLoadModelFromMemory`
(`extern/decomp/src/NL/glx/glxLoadModel.cpp`) parses `.bmd`/`.glg` correctly:
`Environment/the_palace/the_palace.glg` yields 342 models, `gameplay/ball.glg`
one, `gameplay/powerups.glg` eight, and drawables are built for them. The load
then moves on to the `.wld` object data, which is where it currently sits.

Porting that pipeline meant a specific split, and it is worth stating because
the two halves pull in opposite directions:

- **Structs keep their GameCube layout.** `glModel` is 0x10, `glModelPacket`
  0x4A, `glModelStream` 0x6, `glStateBundle` 0x36. The loader walks these arrays
  with hard-coded strides and with counts derived from the chunk size on disc, so
  a struct that grows behind a 64-bit pointer desynchronises the whole walk.
  On-disc pointers became `nlGCPtr32<T>`, `unsigned long` became `u32`, and every
  one of them carries an `NL_GC_LAYOUT` assertion.
- **Scalars are swapped once at load and are native afterwards** — the same shape
  as `.loc` and `.fen`. The exception is `nlChunk`, which is re-read out of the
  raw file buffer at every level of the tree and so is typed `beu32`.
- **Display lists and index data stay big-endian**, because aurora's FIFO reader
  eats them in GameCube format. Vertex *arrays* are the exception: `glxSend.cpp`
  passes `le = true` to `GXSetArray`, so those are swapped at load, deduped by
  start offset and sorted, since packets share arrays and a per-stream pass would
  swap a shared array twice.

Four bugs surfaced on the way through, and the last two are new classes:

- The optional outer BMD header was tested with a raw `*(u32*)` read, so
  `80 01 b1 00` compared as `0x00b10180` and never matched. A `.glg` that carried
  the wrapper had its first group header parsed as a chunk and yielded **zero**
  models — which is exactly what stadium geometry did.
- **Anything read back out of a GX display list on the CPU has to be swapped.**
  The list is deliberately left big-endian for the FIFO, so
  `GetAABBDimensions`' `*pVert` came back ~256x too large and the vertex fetch
  left MEM1 at `0x88003b65`, just past the 128 MB top.
- **A pointer field that is really two `u16`s is a 64-bit trap.** `DisplayList`'s
  last word was decompiled as `unsigned short* indices`, but nothing dereferences
  it: `dlMakeDisplayList` writes two `u16`s there and every reader aliases them
  back as `((u16*)&list->indices)[0/1]`. Same four bytes on the GameCube; here the
  pointer is eight bytes and 8-aligned, so the writer stored at 0x14 and the
  readers looked at 0x18. The struct now spells `numStreams`/`hasColorStream`,
  and `dlMakeDisplayList` allocates `sizeof(DisplayList)` rather than a literal
  `0x10` that no longer covered the stores it made.
- `.wld` chunk payloads are read in place too. `World::LoadPhysicsPrimitives`
  took its element count with a native read, got a number in the tens of
  millions, and asserted out of `MemoryAllocator::Allocate`. The four
  `World*ChunkData` structs and `CharacterPhysicsElement` now keep GC layout and
  have their non-name words swapped once at the point the chunk is identified.
- **A `void` function installed as a value-returning callback used to work.**
  `SidelineExplodableTextureLoadCallback` is declared `void f(unsigned long)` and
  cast to `glxTextureLoadCallback_t`, which returns the remapped texture handle.
  MWCC left the incoming argument in `r3`, the empty body never touched it, and
  the caller read the id straight back — the stub was a pass-through by
  coincidence of the PowerPC ABI. On x86-64 the caller reads `RAX`, which still
  held the callback's own address, so `newHash` came out as the low 32 bits of a
  host function pointer (`0x61c4d030`), `glplatTextureReplace` was handed the
  wrong `PlatTexture`, and a 256-entry palette was copied onto a texture that had
  none. The stub now returns `textureId`. `SidekickTexture_cb` next to it
  returned `s32` through the same `unsigned long`-returning pointer, which is
  only the same width away from LP64; it is spelled `unsigned long` now.
- **Hand-rolled byte-offset walks through what used to be a 4-byte pointer.**
  `NetMeshVertex::GetPosition/GetNormal/GetTextureCoord` reach the packet's
  vertex streams as `*(u8**)((char*)mpPacket + 0x0C)` and then
  `*(s8**)(layout + 0x00 / 0x06 / 0x12)` — the stream table pointer and the three
  stream base addresses, all 4 bytes on the GameCube. Each of those host reads
  takes eight bytes and swallows the field after it, so the first one came back
  as `0xffffffffffffffff`. They index `glModelStream` now; the literal offsets
  were streams 0, 1 and 3.
- **A literal `8` for a two-pointer list node.** `PhysicsLoader::Construct-
  StaticPhysicsPrimitives` allocates its `ListEntry<PhysicsObject*>` nodes with
  `nlMalloc(8, 8, false)` — a 4-byte `next` and a 4-byte payload on the
  GameCube. Both are 8 bytes here, so each node's two stores ran eight bytes past
  its block and corrupted the allocator's free list; the fault surfaced later, in
  `nlRingCheck`. Ten such literals across `Physics.cpp`, `NPCManager.cpp` and
  `plataudio.cpp` are now `sizeof(T)`. This is the same class as the slot-pool
  literals in `patches/decomp/084`, and it is worth grepping for whenever a crash
  lands inside the allocator rather than at the code that caused it.

Then the load moved past the world and the physics into the NPC/animation
inventory, which turned out to be a third file family with the same shape as
`.bmd`: `cSHierarchy`, `cSAnim` and `AnimRetargetList` are not constructed at
all. Each one **is** the first chunk of its `.shr`/`.sanim` file, and
`Initialize()` fills its pointer fields in with the addresses of the chunks that
follow — the object is a header in the file image being relocated in place. So
all three keep GameCube layout (`cIdentifier` 0x8, `cSHierarchy` 0x34, `cSAnim`
0x48, `AnimRetarget` 0xC, `AnimRetargetList` 0x10) with `nlGCPtr32<T>` slots,
including the *arrays of pointers* they point at — `cSHierarchy::m_pChildren`
and `cSAnim`'s three key tables are themselves chunks the loader writes 4-byte
pointers into, so their element type is `nlGCPtr32<T>` too. The scalars beside
them are swapped once, at the point each chunk is claimed, with the element
width the chunk actually holds: 32-bit for node ids, parents, child counts,
mirror tables, translation offsets, morph counts and node properties; 16-bit for
packed rotations, packed scales and bone maps; and nothing at all for the byte
arrays (`m_pPreserveBoneLength`, `m_pMorphKeys`) or for the pointer tables the
loader is about to overwrite.

The skin meshes after that added one more rule, and it is the sharpest edge in
the whole "swap once at load" design:

- **Swap where the data is claimed, not where it is parsed.** The `.bmd` SKIN
  chunk is copied out of the file once and kept in `glInventory`; then
  `glx_MakeSkinMesh` runs over that same copy again for *every instance* that
  shares the model. Swapping inside the parser looked right and worked for the
  first NPC — and handed the second one the original big-endian words back,
  because the block had been swapped twice. The stitching chunk's packet count
  came back as `0x01000000`, and `nlMalloc(16777216 * 8)` walked the whole free
  list and asserted. The pass now runs once, at the `memcpy` that makes the
  copy. A swap that is not idempotent must happen exactly once, so it belongs at
  the point a blob is taken ownership of, never at the point it is read.

Within that chunk the widths matter individually: 32-bit for the bone matrices,
the bone map and the whole morph chunk; 16-bit for `SkinPair`; only the first
twelve bytes of each `SkinVertex` because the packed normal that follows is four
signed bytes; and only the two leading counts of the stitching chunk because the
rest is per-vertex bytes.

Characters then loaded — `chainchomp`, `bowser`, `luigi`, `luigi_blend` — and
the next failure was not a crash at all. The allocator's own validator
(`patches/decomp/077`) caught a **corrupt free ring** at an unrelated
allocation, long after the damage. That is a different debugging problem, and
the technique that cracked it is worth recording: the ring is checked on every
allocator entry *and* exit, so the bad write necessarily happened between two
consecutive allocations. Dumping a backtrace of whoever is allocating *now*
therefore names the loop that was running when it landed, even though the
writer is long gone from the stack. That pointed at the animation bundle, and
the cause was:

```c
m_pSAnims = (cSAnim**)nlMalloc(m_nNumProperties << 2, 8, 0);
```

`<< 2` is four bytes per `cSAnim*` — a GameCube pointer. They are eight here, so
the array was half the size the loop that fills it needs, and it overran into
the middle of the heap. A sweep found every other pointer-array allocation
already spelled `sizeof(T*)`; the two remaining `* 4` sites are genuine `u32`
and `int` arrays.

The `.trg` animation-trigger files after that took the *other* endian treatment.
`FILE_HEADER`, `ANIM_RECORD` and `TRIGGER_RECORD` are used strictly where they
land — nothing copies them out — so they are typed `beu16`/`beu32`/`bef32`
rather than swapped in place. That is not just a style choice: the records are
sorted by hash and searched with `nlBSearch`, and a byte-reversed read of a
big-endian sorted array is not sorted in either direction, so swapping the key
instead would have broken the search. **When a file is searched or compared
rather than merely read, the typed-big-endian form is the only correct one.**

Getting from "front end runs" to "match loading" took five more fixes, each of
which hid the next:

- With no save present, aurora's `CardGciFolder` returned `NOCARD` for a file it
  could not find. That sends the game down its "no memory card in Slot A" path,
  whose **RETRY** re-opens the same absent file forever. It now returns `NOFILE`,
  like hardware, and the two stubbed `deleteFile` overloads are implemented
  (`patches/aurora/006`).
- Creating the save then smashed the caller's stack. `MemCardFunctor` stores its
  callback in a 24-byte inline buffer, which was exactly a vtable pointer, a
  `void*`, a 4-byte pointer-to-member and a `this` on the GameCube. The Itanium
  ABI's pointer-to-member-function is 16 bytes, so the object needs 40
  (`patches/decomp/080`).
- Writing the save banner then dereferenced ~`0x93fb5f80`. The TPL structs in
  `dolphin/charPipeline/texPalette.h` overlay a raw big-endian `.tpl` and were
  declared host-width and little-endian (`patches/decomp/081`, `082`).
- The write itself then went to `0xcdcdcdcdcdcdcdcd`. `CARDSetStatusAsync` called
  its completion inline, so `MemCard::SetStatusDone` read `m_pFileCB`/`m_pDataCB`
  before `WriteFileIconData` had assigned them — the same hazard `CARDMountAsync`
  was already fixed for. Every `CARD*Async` entry point now queues its callback
  (`patches/aurora/005`).
- The save then wrote cleanly and the game crashed anyway, inside a cold block of
  `SaveCallbacks::DoSave`. `FileWriteIconCB` falls off the end of a non-`void`
  function; MWCC returned whatever `DoSave` left in `r3`, GCC treats the path as
  unreachable and lets control run into the next basic block
  (`patches/decomp/082`).

After that the front end ran to team select and stopped on
**"AT LEAST ONE PLAYER MUST CHOOSE A SIDE"**, and three more fell out:

- `IChooseSide::UpdateForFE` marks a player ready through
  `((TLInstance**)&mPlayingSides[i])[9]` — a 4-byte-stride walk from an `int*`
  into the pointer array 0x14 bytes later (`patches/decomp/083`).
- `IChooseCaptain::~IChooseCaptain` walks `mAsyncImage[2][3]` by stepping `this`
  0xC bytes per row and 4 per column (`patches/decomp/083`).
- Tearing down the front end then freed a wild pointer.
  `AudioLoader::UnloadFE` passes a literal `0x24` as the slot size to
  `SlotPoolBase::BaseFreeBlocks`, but the pool was filled with `sizeof(T)`, and
  `T` is bigger here. Six such literals are now `sizeof(T)`
  (`patches/decomp/084`).

Before any of that, eight things had to be fixed to get the front end up at all,
each of which also hid the next:

- The loading screen used to report **"Localization Table Not Found"**. `.loc`
  files are big-endian, so `LOCHeader::Version` read back as `0x01000000` and
  `nlLocalization::Load` rejected every language. See the porting notes.
- It then parked forever on "Checking the MemoryCard in SlotA", because
  aurora's `CARDMountAsync` returned success without ever calling its
  `attachCallback`.
- Delivering that callback inline then crashed the save/load path, which starts
  a mount from inside a scene update and is not re-entrant. It is now queued and
  delivered once per host frame, which is what the hardware's EXI interrupt
  effectively did.
- The legal screen then dereferenced a NULL `TLImageInstance`, because every
  lookup in `CrossFaderScene::SceneCreated` returned nothing: the decomp calls
  the scene-graph finder through a function-pointer union that is only valid
  under the Metrowerks PPC ABI. See the porting notes.
- The audio preload then had aurora's DVD worker write outside MEM1.
  `GCAudioStreaming::AudioBufferMgr::GetADPCMHdr` aligns its buffer by casting
  through `unsigned long`, and that buffer is a static in the executable image
  rather than in MEM1, so the read destination was the low half of its address.
- The stream header callback then read through a truncated pointer of its own.
  `nlReadAsync` carries callback user data in an `unsigned long`, which every
  caller stuffs a host pointer into. That channel is now `nlUserParam`
  (`uintptr_t`) end to end. See the porting notes.
- The audio stream then wrote gigabytes through the middle of MEM1, wrecking
  the heap. `sDSPADPCM` and `INTERLEAVED_ADPCM_HEADER` are read off the disc and
  used where they land, so they are big-endian; `Interleave` and `StreamLength`
  become the length and offset of the *next* DVD read, so a byte-swapped
  `0x6A40` read back as `0x406A0000` is a one-gigabyte transfer.
- Removing a fade from the audio track manager then wrote through a bogus
  pointer. `RemoveFade` recovers the list node from the value with a hard-coded
  `- 8` — two GameCube link pointers. Ours are eight bytes each.

Before that it segfaulted about a minute in. That was the health-and-safety
screen's 60-second auto-advance (`SHHealthWarning.cpp`) reaching a text path
that stored a host stack pointer in a 32-bit slot; see the porting notes.

---
