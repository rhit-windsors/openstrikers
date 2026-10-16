# Architecture and porting reference

OpenStrikers builds the patched game decompilation as a native host binary.
[Aurora](https://github.com/encounter/aurora) supplies GameCube system APIs
on top of SDL3 and WebGPU/Dawn. The root project supplies the entry point,
compatibility shims, network transport, host audio/movie decoding and build.

See [Building and patching](BUILDING.md) for the patch workflow and
[porting history](archive/porting-history.md) for individual investigations.
Paths below are relative to the repository root.

## Layout

```
CMakeLists.txt          the whole build
apply_patches.sh        resets submodules and applies patches/
justfile                Linux convenience targets
src/main.cpp            entry point: aurora_initialize, open the disc, game_main()
src/compat_shims.cpp    libc/MSL gaps the decomp expects
src/musyx_host.c        host DSP stand-in and SDL audio output
src/thp_host.c          host THP movie decoder
src/net/               lockstep transport, deterministic math and state hashes
include/compat_shims.h  force-included into every decomp TU (-include)
include/launch_options.h command-line and match-option parsing
launcher/              Electron launcher, lobby and co-op cup management
tests/                 native unit tests and PowerShell integration checks
patches/decomp/         numbered patches against the decompilation source
patches/aurora/         numbered patches against aurora
patches/musyx/          audio-library host fixes
patches/tmp/            patches that are known workarounds, not real fixes
patches/decomp-late/    decomp patches applied after patches/tmp/ -- see
                        "Patch stages, and why the order matters"
extern/decomp/          submodule: the game's source
extern/aurora/          submodule: the GameCube system library re-implementation
```

Note the include order in `CMakeLists.txt`: `extern/aurora/include` comes
*before* `extern/decomp/include`, so Aurora's `dolphin/...` headers win over
the decomp's copies. That is deliberate.

### How a frame gets driven

On hardware there was no "frame" for the CPU to open: the GP consumed the FIFO
continuously and VI scanned out whatever `GXCopyDisp` had written. Aurora needs
an explicit one. The decomp's frame boundary is `glplatSendFrame()`
(`extern/decomp/src/NL/gl/glPlat.cpp`), so that function calls
`osHostFrameBegin()` / `osHostFrameEnd()` — thin wrappers in
`src/compat_shims.cpp` around `aurora_update()`, `aurora_begin_frame()` and
`aurora_end_frame()`. `aurora_update()` is also what pumps SDL, so without it
the window never responds and pad input never refreshes.

The host frame is also the **pad sampling tick**. On hardware, VI's retrace
kicked off an SI transfer and the callback registered with
`PADSetSamplingCallback()` ran off that interrupt; nothing here generates it, so
`osHostFrameBegin()` calls the stored callback once per frame. That callback is
the decomp's `VBlankPadUpdate()`, which is what actually calls `PADRead`. It
used to be a no-op stub, so the pad state the game reads stayed zeroed forever.

Sampling happens after the event pump (so the keyboard state is this frame's)
and before `aurora_begin_frame()` (so input keeps flowing on frames that are
skipped because the window is minimized).

The host frame is also where **queued memory-card completions are delivered**,
via `CARDServicePendingCallbacks()`. On hardware a card mount finished off the
EXI interrupt, well after `CARDMountAsync` had returned; delivering it inline
instead re-enters the caller mid-setup. See the porting notes.

---

## Porting notes

A handful of bug classes account for most of the work, and the first two are
worth knowing before you touch anything.

**LP64 vs LLP64 vs the GameCube.** `unsigned long` is 4 bytes on the GameCube
and on Windows, but 8 on Linux. Code that assumed `sizeof(long) == 4` breaks
differently on each host. The fix is always to spell the width: `u32`/`s32`.

**Serialized structs contain 4-byte pointers.** The `.fen` scene packages are
relocated object graphs written by the GameCube toolchain: pointer fields are
32 bits and every scalar is big-endian. Growing those fields to 64-bit host
pointers would shift every offset in the file. Instead, MEM1 is mapped at
`0x80000000` (see `patches/aurora/003`) so a host pointer truncates to 32 bits
losslessly, and serialized structs declare their fields as:

* `nlGCPtr32<T>` (from `extern/decomp/include/NL/nlGCPtr.h`) for pointers, and
* `beu32` / `bes32` / `beu16` / `bef32` / `nlBEEnum<E>` (from
  `extern/decomp/include/NL/nlEndian.h`) for scalars.

Every converted struct carries `NL_GC_LAYOUT(Type, size)` and, where a field's
placement matters, `NL_GC_FIELD(Type, field, offset)`. These are
`static_assert`s: if a layout drifts, the build fails instead of the game
reading garbage.

The corollary bites harder than the rule: **anything one of those 32-bit slots
points at must live in MEM1.** A host stack address or the address of a static
in the executable does not fit, and the store truncates in silence. On hardware
this never came up because the stack was in MEM1 too, so the decomp is full of
`__alloca` scratch buffers and addresses-of-locals that were perfectly good
32-bit pointers then and are not now. Two of those were live bugs:
`FERender::RenderTextInstance` handed `TLTextInstance::SetMatrix` a stack local,
and `TLTextInstance::Render` built its character buffer with `__alloca`; both now
use MEM1 scratch (`patches/decomp/068`). Build with `-DOPENSTRIKERS_CHECK_GCPTR=ON`
to catch the next one at the store.

The same truncation happens without an `nlGCPtr32` in sight, wherever the code
casts a pointer through `unsigned long` — which is 32 bits on Windows.
`AudioBufferMgr::GetADPCMHdr()` returned
`(void*)(((unsigned long)m_ADPCMHdrMem + 0x1F) & ~0x1F)`, and `m_ADPCMHdrMem`
lives inside `g_BufferMgr`, a static in the executable image rather than in
MEM1, so the DVD worker read a 96-byte ADPCM header into the low half of its
address (`patches/decomp/074`). Cast through `uintptr_t` instead. Note that
this one is invisible to the `nlGCPtr32` check, so the pattern is worth
grepping for directly.

**When `unsigned long` is a channel, widen the channel.** Grepping for the cast
finds one site at a time; sometimes the truncation is structural. `nlReadAsync`
and everything downstream of it carried callback user data in an `unsigned
long` — `AsyncEntry::m_uParam`, `GameCubeReadAsync`, `nlReadAsyncToVirtualMemory`
and the fourth parameter of every `ReadAsyncCallback` — and essentially every
caller stuffs a host pointer in there: `BundleFile` passes `(unsigned long)this`,
`nlLoadEntireFileAsync` passes its bookkeeping struct, `GCStream` passes a
`READ_CB_INFO` out of a static pool. Fixing the one callback that happened to
crash (`AudioStream::_HdrReadCB`) would have left the rest waiting. The channel
is now `nlUserParam` — a `uintptr_t` typedef in `NL/nlFile.h` — declared once
and used end to end, including `FileReadAsyncCallback` in `nlBundleFile.h`
(`patches/decomp/075`).

One caution if you do this: **`-fpermissive` will not find the stragglers for
you.** A callback still declared with `unsigned long` converts to the widened
function-pointer type with a warning rather than an error, and the mismatch then
truncates silently at the call. Grep the log for `invalid conversion from 'void
(*)('` after the change and expect the count to be zero; a build that merely
succeeds proves nothing.

**Hand-written `container_of` is a hard-coded pointer size.** `DLListEntry<T>`
is `{next, prev, entry}`, so the decomp recovers the node from a value with
`(Entry*)((char*)value - 8)` — two 32-bit links. Ours are eight bytes each, so
that constant lands in the middle of the node and the next unlink writes through
whatever it finds there. `nlDLEntryFromValue<T>` in `NL/nlDLRing.h` asks the type
instead (`patches/decomp/078`). Worth grepping for the shape: `- 4)` and `- 8)`
immediately after a cast.

There is a nastier relative of this that is *not* fixed: `feManager.cpp:199` and
`SHLessonSelect.cpp:406` both do `scene = (BaseSceneHandler*)((char*)scene - 4)`,
which is a hard-coded multiple-inheritance base adjustment. The right spelling is
a `static_cast` and letting the compiler compute the offset. Neither is on the
boot path, so neither has been exercised yet.

**Audio stream headers are big-endian too, and they are not a display bug.**
`sDSPADPCM` and `INTERLEAVED_ADPCM_HEADER` (`Game/Sys/GCStream.h`) are the first
bytes of a stream on the disc, read with a DVD transfer and used where they
land. `Interleave` and `StreamLength` then become the length and offset of the
*next* DVD read — so a byte-swapped `0x6A40` read back as `0x406A0000` is a
one-gigabyte transfer through the middle of MEM1 (`patches/decomp/076`). This is
the general shape to watch for: a missed byte swap on a field that is only a
number is a wrong number, but on a field that is a length or an offset it is
memory corruption a long way from the mistake.

**Recognising a truncated static.** A truncated *stack* address looks like a
stack address with its top bytes gone. A truncated *static* is harder, because
the executable is ASLR'd: the fault address changes every run, which reads like
a race rather than a deterministic bug. The tell is that the low bits stay
identical across runs (`0x940c02e0`, `0x54fa02e0`, `0xd6202e0`). To name the
object, take the loaded base the crash handler prints, subtract it from the
faulting address, add the link-time base, and look the result up:

```bash
nm -C --defined-only build/openstrikers.exe | sort   # find the symbol at or below it
```

**Big-endian applies to loose files too, not just `.fen`.** Anything read off
the disc and used in place needs the same treatment. `.loc` localization tables
were the case that bit: `LOCHeader` and `nlLocalization::StringLookup` are read
straight out of the file, so `Version` came back as `0x01000000`, the language
id never matched, and `Load` silently returned 0 for every language — which the
front end renders as "Localization Table Not Found". Both structs now use
`beu32` (`patches/decomp/069`).

The same applies to `.tpl` textures. `TEXPalette`, `TEXDescriptor`, `TEXHeader`
and `CLUTHeader` (`dolphin/charPipeline/texPalette.h`) are the raw first bytes of
the file, and `descriptorArray` / `textureHeader` / `data` are offsets into it —
so a byte-swapped offset is not a wrong picture, it is a wild pointer. The save
banner path did `(u8*)tpl + tpl->descriptorArray` and landed around
`0x93fb5f80`. All four structs are now `beu*` with `NL_GC_LAYOUT`
(`patches/decomp/081`). Note that `TEXDescriptor::textureHeader` was declared
`TEXHeaderPtr` because the SDK header declares it that way, but the file stores
a `u32` offset and every caller immediately adds it to the buffer base; it is
typed `u32` now (`patches/decomp/082`).

The `.loc` strings themselves are UTF-16 **big-endian**, and those cannot be wrapped:
every consumer takes a bare `const unsigned short*` and compares against native
literals like `LocalizationTableNotFound`. `Load` byte-swaps the whole string
block once, in place, and everything downstream stays native. `StringOffset`
counts characters from `m_FirstString`, not bytes.

**Not every class here is a width or an endianness bug: some are undefined
behaviour that MWCC happened to tolerate.** `cPoseAccumulator::BlendTrans`
declared a local *inside* the `if (bMirror)` block that fills it, repointed its
`pTrans` parameter at that local, and dereferenced it after the block closed.
MWCC left the stack slot alone, so the retail game worked; GCC reuses it, and
every mirrored blend read whatever the compiler had since put there. On the
animated root node that value is the character's height, which is why players
floated off the pitch (`patches/decomp-late/115`). `BlendRot`, ten lines above,
declares its `qtemp` outside the block and is correct -- so the grep shape is a
`p = &local;` on the last line of a block, where `p` outlives it.

This class behaves differently from the rest and that matters more than the
fix. A width bug is deterministic; this one **moves when you measure it**.
Adding an inert branch to a *neighbouring* function made the float disappear
twice without changing any behaviour, because a different tenant landed in the
slot. If a symptom vanishes when you add a probe that cannot possibly matter,
that is evidence, not noise -- and the diagnostics have to move to cold code
well away from the path being measured.

**A struct passed by value is not always passed by value.** `SHCrossFader.cpp`
and `SHChooseCup.cpp` reach the scene-graph finder through a union of two
function-pointer types — one taking `InlineHasher` by value, the other taking
`InlineHasher&` — assigning the first and calling through the second. That is a
decompilation artifact of the Metrowerks PPC ABI, which passed a class with a
user-declared constructor by hidden reference, so the two signatures compiled to
the same calling sequence. They do not on x86-64: `InlineHasher` is a 4-byte
trivially-copyable struct and is passed by value in a register, so the pun handed
the callee the *address* of each `volatile unsigned long` temporary and it read
the low half of a stack pointer as the hash. Every lookup missed and returned
NULL, silently — the legal screen then wrote a colour through the NULL it got
back. The unions are gone; the finder is called through a plain function pointer
(`patches/decomp/071`). Anywhere else the decomp punts between two function
types to reproduce PPC codegen deserves the same suspicion.

**Arrays of pointers are twice as wide now.** Distinct from the `sizeof` bullet
below, because the multiplier is spelled as a literal `4` rather than as a type:
`nlMalloc(count * 4, ...)` cast to `T**` allocates half of what it needs on a
64-bit host, and every element past the first overruns the block. Seven of these
were live (`patches/decomp/072`): the crossfader's image table, the captain-grid
instance table, and the AVL-tree walk stacks in `EmissionManager`,
`NetMeshModelLoader` and `StaticModelExplodable`. Spell it `sizeof(T*)`.

**`nlZeroMemory` was zeroing almost nothing.** It hands its aligned middle to
`DCZeroRange`, which is a `dcbz` loop on hardware and really does zero the
range — but was stubbed out as a no-op here alongside the genuine cache hints,
so every call of 0x80 bytes or more left everything but the unaligned head and
tail untouched. It also held the address in an `unsigned long`, truncating the
host stack addresses its callers pass (`nlZeroMemory(&t, sizeof t)`) and turning
the head/tail loops into wild writes. Both are fixed (`patches/decomp/073` and
`DCZeroRange` in `src/compat_shims.cpp`). Worth remembering that a Dolphin
cache routine is not always safe to stub: `DCFlushRange` is, `DCZeroRange` is
not.

**Async callbacks in aurora are not free.** The Dolphin `*Async` APIs promise to
call you back; several of aurora's implementations do the work synchronously and
call the callback before returning, and `CARDMountAsync` used not to call it at
all. Both halves matter. A dropped callback parks the game forever (this is what
the "Checking the MemoryCard" hang was), and an inline callback re-enters a
caller that has not finished setting itself up — `SaveLoad::StartLoad` starts a
mount from inside a scene update and crashed when the completion arrived before
it returned.

**Every** `CARD*Async` entry point now queues its completion for
`CARDServicePendingCallbacks()`, called once per host frame from
`osHostFrameBegin` (`patches/aurora/005`). Fixing only `CARDMountAsync` was not
enough, and the second one cost a session on its own: `MemCard::WriteFileIconData`
calls `CARDSetStatusAsync` and *then* assigns the `m_pFileCB` / `m_pDataCB` that
`MemCard::SetStatusDone` reads, so an inline completion wrote the save from
`0xcdcdcdcdcdcdcdcd`. The whole state machine is written for completions that
arrive after the call returns, because that is what an EXI interrupt does.
Anything else in aurora that grows a completion should do the same rather than
calling inline.

**Inline storage sized for a GameCube object.** `MemCardFunctor` (`Game/Sys/gcmemcard.h`)
keeps its callback in a fixed `m_FunctorMem[24]` and placement-news a small
polymorphic object into it. On the GameCube that was a vtable pointer, a
`void*`, a 4-byte MWCC pointer-to-member-function and a `this` — exactly 24. The
Itanium ABI's pointer-to-member-function is **16 bytes** (function address plus
`this`-adjustment), so the object needs 40, and every `MemCardFunctor functor;`
is a stack local: the placement-new wrote 16 bytes past it into the caller's
frame. The buffer is now sized from a layout probe struct and guarded with a
`static_assert` in `MCMemberFunctor::Call` (`patches/decomp/080`). Any other
fixed-size inline buffer holding a member-function pointer has the same problem;
size it from the type, never from the original number.

**Hand-walking a struct with hard-coded strides.** Related to the `container_of`
note above, but the shape is a loop rather than a subtraction: MWCC would step
`this` by a literal byte count and read one member off the end of it, because on
the GameCube the arrays it was walking were 4 bytes per element and packed one
after another. Two were live and both are on the match-setup path
(`patches/decomp/083`):

* `IChooseSide::UpdateForFE` does `((TLInstance**)&mPlayingSides[i])[9]`.
  `mPlayingSides` is `int[4]` at offset 0 and `mInstanceTable` starts at 0x14, so
  `i*4 + 36` was `mInstanceTable[i + 4]` — which is what the matching
  "not ready any more" branch a few lines up spells directly. With 8-byte
  pointers `[9]` is 72 bytes out and misaligned.
* `IChooseCaptain::~IChooseCaptain` walks `mAsyncImage[2][3]` by advancing `this`
  0xC bytes per row and 4 per column, always reading `mAsyncImage[0][0]`, so on a
  64-bit host it `delete`s six pointers that are not the ones it allocated.

Both fixes are the same: index the array. Grep for a cast to `T**` applied to the
address of something that is not a `T*` array.

**Falling off the end of a non-`void` function is not harmless here.**
`SaveCallbacks::FileWriteIconCB` ends with a bare `DoSave(Slot);` and no
`return`. Under MWCC that returned `DoSave`'s result, because it was already in
`r3`. GCC treats the path as unreachable, emits no `ret`, and lets control run
straight into whatever it laid out next — in this case a cold block of the
inlined `DoSave` that dereferences `m_pSaveFile` with a clobbered register, so
the crash lands on a line that never executed. This one is free to find:
`-Wreturn-type` is on by default, so grep the build log for `return-type` and
expect zero. `patches/decomp/082` returns the value the original returned.

**The `.bmd` model pipeline is still big-endian and unported.** This is the
current blocker, and it is the largest remaining piece.
`glxLoadModelFromMemory` reads `nlChunk::m_ID` / `m_Size` natively out of a file
that stores them big-endian, so the `0x8001B100` magic test fails and the chunk
walk immediately leaves the buffer. Beyond the chunk headers, `glModel`,
`glModelPacket` (packed, 0x4A, with `glStateBundle` inline), `glModelStream`,
`GLMaterialEntry` and the texture/vertex-anim headers are all `memcpy`'d
straight out of the file and used as native structs, and `glModel::packets` /
`glModelPacket::streams` are 4-byte on-disc pointers. The conversion is the same
one `.fen` and `.loc` already had — `beu32` scalars, `nlGCPtr32<T>` pointers,
`NL_GC_LAYOUT` assertions — but it touches the whole render path. Note that the
display-list, index and vertex blobs must **stay** big-endian: aurora's GX
consumes GameCube-format data.

A few smaller traps that have each cost a debugging session:

* `#pragma push` / `#pragma pop` are Metrowerks spellings; GCC silently ignores
  them, leaving a `#pragma pack(1)` unterminated for the rest of the
  translation unit. Guard them with `#ifdef __MWERKS__` and use
  `#pragma pack(push, 1)` / `#pragma pack(pop)` otherwise.
* Metrowerks emits a 0x10-byte array cookie for `new T[]`; the Itanium ABI
  emits none for trivially-destructible types. Code that hand-computes the
  allocation and then frees the base pointer needs the offset spelled out.
* GameCube RAM was fully writable, so `static const` globals that the game
  writes through a cast-away-const worked. On a hosted platform they land in
  `.rdata` and fault. Drop the `const`.
* Address 0 was readable OS-globals memory on the GameCube, so a NULL-ish
  dereference was benign. Lookups that can miss need real NULL guards now.
* Hard-coded GameCube `sizeof` values passed to `nlMalloc` under-allocate on a
  64-bit host and quietly overrun the next free-list node. Use `sizeof(T)`.
  The same literals turn up as the `slotSize` argument to
  `SlotPoolBase::BaseAddNewBlock` / `BaseFreeBlocks`, where they are worse than
  an under-allocation: `BaseFreeBlocks` recovers the block base with
  `(u8*)block - slotSize * count`, so a stale literal hands `free()` a pointer
  that was never allocated. `AudioLoader::UnloadFE` passing `0x24` for a
  `SlotPool<SFXPlaySet>` is what crashed the front-end teardown when a match
  starts (`patches/decomp/084`).
* A packed colour word read through `((u8*)&word)[0..3]` spells `r,g,b,a` only
  on a big-endian host. Extract channels with shifts.
* The game draws in *logical* EFB coordinates: since Aurora 40cf48f,
  `GXAdjustForOverscan` leaves the GameCube's 640x448 EFB alone, and Aurora
  scales viewports, scissors, copy rects and the copies themselves onto its
  render target, whose size and aspect are the host's. "Whole frame" rects must
  therefore use the configured EFB (`glplatGetEFBSize`), never
  `AuroraGetRenderSize`. Before 40cf48f the EFB itself was host-sized, which
  is why `patches/decomp-late/118` exists and why `patches/aurora/012` widens
  the scissor past GX's 11-bit fields.
* `GXSetArray` takes an `le` flag that aurora does not have on hardware. Vertex
  arrays filled by the game at runtime are host little-endian, so it must be
  `true`; passing `false` byte-swaps every position and the geometry vanishes.
