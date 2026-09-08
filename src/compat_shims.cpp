#include "compat_shims.h"
#include "NL/gl/glPlat.h"

#include <aurora/aurora.h>
#include <dolphin/os.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <aurora/event.h>
#include <dolphin/pad.h>
#include <dolphin/card.h>
#include <SDL3/SDL_scancode.h>

namespace aurora {
extern AuroraConfig g_config;
}

// On GameCube hardware the runtime calls OSInit() from __start *before* static
// constructors run, and the decomp depends on that ordering: static SlotPool
// objects (e.g. Powerups.cpp) call nlMalloc -> nlInitMemory, which reads the OS
// arena. Aurora never calls OSInit() itself and aurora_initialize() only runs
// once we are already in main(), so without this MEM1 is never allocated, the
// arena is null, and nlInitMemory computes a 0-sized heap and segfaults.
//
// The ELF runtime runs prioritized constructors before all default-priority
// ones, so priority 101 restores the hardware's ordering. OSInit() is
// idempotent, so aurora_initialize()'s later setup is unaffected.
#if defined(_WIN32)
// The interesting crashes here only happen when no debugger is attached: a
// pointer that has lost its top 32 bits lands on whatever the loader mapped at
// the truncated address, and that layout changes under a debugger. gdb also
// cannot attach to this process after the fact on this setup, so the process
// has to report its own fault.
//
// Symbolize the frames with:
//   addr2line -e build/openstrikers.exe -f -C -p <rva+base printed below>
// Reads OptionalHeader.ImageBase out of the executable file rather than out of
// the mapped image, which the loader has already relocated. Falls back to the
// in-memory value so the output is still shaped like addresses if this fails.
static uintptr_t osReadLinkTimeImageBase(uintptr_t loaded) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)loaded;
    const IMAGE_NT_HEADERS* mapped = (const IMAGE_NT_HEADERS*)(loaded + dos->e_lfanew);
    uintptr_t base = (uintptr_t)mapped->OptionalHeader.ImageBase;

    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return base;
    }
    FILE* f = _wfopen(path, L"rb");
    if (f == nullptr) {
        return base;
    }
    IMAGE_DOS_HEADER fileDos;
    IMAGE_NT_HEADERS fileNt;
    if (std::fread(&fileDos, sizeof(fileDos), 1, f) == 1 && fileDos.e_magic == IMAGE_DOS_SIGNATURE &&
        std::fseek(f, fileDos.e_lfanew, SEEK_SET) == 0 &&
        std::fread(&fileNt, sizeof(fileNt), 1, f) == 1 && fileNt.Signature == IMAGE_NT_SIGNATURE) {
        base = (uintptr_t)fileNt.OptionalHeader.ImageBase;
    }
    std::fclose(f);
    return base;
}

static LONG WINAPI osCrashHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    std::fprintf(stderr, "%s", "openstrikers: FATAL EXCEPTION" "\n");
    std::fprintf(stderr, "  code    0x%08lx" "\n", (unsigned long)rec->ExceptionCode);
    std::fprintf(stderr, "  at      %p" "\n", rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::fprintf(stderr, "  %s address 0x%llx" "\n",
                     rec->ExceptionInformation[0] == 0 ? "reading" :
                     rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
                     (unsigned long long)rec->ExceptionInformation[1]);
    }

    // addr2line wants link-time addresses, so undo the loader's relocation.
    // The in-memory OptionalHeader.ImageBase is no help: the loader rewrites it
    // to wherever it actually mapped us, so subtracting it is a no-op and every
    // frame comes back "?? ??:0". The link-time value only survives on disk.
    const uintptr_t loaded = (uintptr_t)GetModuleHandleW(nullptr);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)loaded;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(loaded + dos->e_lfanew);
    const uintptr_t linked = osReadLinkTimeImageBase(loaded);
    std::fprintf(stderr, "  module  loaded at 0x%llx, linked at 0x%llx" "\n",
                 (unsigned long long)loaded, (unsigned long long)linked);

    osDumpBacktrace("stack", 0);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

extern "C" unsigned long osCurrentThreadId(void) {
#if defined(_WIN32)
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

extern "C" void osDumpBacktrace(const char* tag, unsigned skip) {
#if defined(_WIN32)
    const uintptr_t loaded = (uintptr_t)GetModuleHandleW(nullptr);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)loaded;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(loaded + dos->e_lfanew);
    const uintptr_t linked = osReadLinkTimeImageBase(loaded);

    void* frames[62];
    // +1 skips this function itself, so callers count from their own frame.
    const USHORT n = RtlCaptureStackBackTrace((ULONG)skip + 1, 62, frames, nullptr);
    std::fprintf(stderr, "  %s (%u frames, link-time addresses):" "\n", tag, (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        const uintptr_t a = (uintptr_t)frames[i];
        if (a >= loaded && a < loaded + nt->OptionalHeader.SizeOfImage) {
            std::fprintf(stderr, "    0x%llx" "\n", (unsigned long long)(a - loaded + linked));
        } else {
            std::fprintf(stderr, "    0x%llx (outside the executable)" "\n", (unsigned long long)a);
        }
    }
    std::fflush(stderr);
#else
    (void)tag;
    (void)skip;
#endif
}

__attribute__((constructor(101))) static void openstrikers_early_os_init() {
    aurora::g_config.mem1Size = OPENSTRIKERS_MEM1_SIZE;
    aurora::g_config.mem2Size = OPENSTRIKERS_MEM2_SIZE;
#if defined(_WIN32)
    SetUnhandledExceptionFilter(osCrashHandler);
#endif
    OSInit();
}

extern "C" int __float_max[] = { 0x7F7FFFFF };
extern "C" float __float_min[] = { 0x00800000 };
extern "C" int __float_nan[] = { 0x7FFFFFFF };
extern "C" int __float_huge[] = { 0x7F800000 };

// GX render mode stub
extern "C" GXRenderModeObj GXNtsc480Prog = {};
// MSL __lower_map (case conversion table)

extern "C" const unsigned short __lower_map[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x5B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

// MSL's ctype table. The MSL ctype.c that carries the real one is not in the
// build, and its is<class>() inlines and the game's own direct reads --
// world.cpp tests `__ctype_map[c] & __digit` -- index this array, so a table
// of zeroes silently answers "no" for every character class. Flags are MSL's:
// 0x01 control, 0x02 motion, 0x04 space, 0x08 punctuation, 0x10 digit,
// 0x20 hex digit, 0x40 lower case, 0x80 upper case. Entries above 0x7F stay
// zero, exactly as the GameCube table has them.
extern "C" unsigned char __ctype_map[256] = {
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x04, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0xA0, 0xA0, 0xA0, 0xA0, 0xA0, 0xA0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x08, 0x08, 0x08, 0x08, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

extern "C" {

void C_MTX44Identity(Mtx44 m) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
    m[3][0] = 0.0f; m[3][1] = 0.0f; m[3][2] = 0.0f; m[3][3] = 1.0f;
}

void C_MTX44Concat(const Mtx44 a, const Mtx44 b, Mtx44 ab) {
    Mtx44 tmp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j]
                      + a[i][2]*b[2][j] + a[i][3]*b[3][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            ab[i][j] = tmp[i][j];
}

void C_MTX44Transpose(const Mtx44 src, Mtx44 xPose) {
    Mtx44 tmp;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            tmp[j][i] = src[i][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            xPose[i][j] = tmp[i][j];
}

void C_MTX44Scale(Mtx44 m, f32 xS, f32 yS, f32 zS) {
    C_MTX44Identity(m);
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

u32 C_MTX44Inverse(const Mtx44 src, Mtx44 inv) {
    f32 a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) a[i][j] = src[i][j];
        for (int j = 0; j < 4; j++) a[i][j+4] = (i == j) ? 1.0f : 0.0f;
    }

    for (int col = 0; col < 4; col++) {
        int pivot = col;
        f32 maxVal = fabsf(a[col][col]);
        for (int r = col + 1; r < 4; r++) {
            if (fabsf(a[r][col]) > maxVal) { maxVal = fabsf(a[r][col]); pivot = r; }
        }
        if (maxVal == 0) return 0;

        if (pivot != col) {
            for (int j = 0; j < 8; j++) std::swap(a[col][j], a[pivot][j]);
        }

        f32 d = a[col][col];
        for (int j = 0; j < 8; j++) a[col][j] /= d;

        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            f32 f = a[r][col];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[col][j];
        }
    }

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            inv[i][j] = a[i][j+4];

    return 1;
}

// --- Cache / PPC stubs ---
void PPCSync() {}
void DCFlushRangeNoSync(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
void DCStoreRange(void*, u32) {}

void DCFlushRange(void*, u32) {}
// Not a cache hint: on hardware this is a dcbz loop, so it really does zero
// the range, and nlZeroMemory delegates its whole aligned middle to it. As a
// no-op it left everything but the unaligned head and tail untouched.
void DCZeroRange(void* addr, u32 nBytes) { std::memset(addr, 0, nBytes); }
void DCInvalidateRange(void*, u32) {}


// --- VI stubs ---

void VISetBlack(int) {}
void VIWaitForRetrace() {}

u32 VIGetRetraceCount() { return 0; }
void VISetNextFrameBuffer(void*) {}
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback) { return NULL; }
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) { return NULL; }
u32 VIGetDTVStatus() { return 0; }



// --- OS stubs ---

void OSYieldThread() {}
u32 OSGetConsoleType() { return 0; }

u32 OSGetResetCode() { return 0; }
void OSResetSystem(int, u32, BOOL) {}
u32 OSGetSoundMode() { return 0; }

void OSSetSoundMode(u32) {}
u8 OSGetLanguage() { return 0; }
u32 OSGetProgressiveMode() { return 0; }

void OSSetProgressiveMode(u32) {}
u32 OSGetEuRgb60Mode() { return 0; }
void OSSetEuRgb60Mode(u32) {}

BOOL OSGetResetButtonState() { return 0; }
void OSClearStack(u8) {}

// aurora declares OSReport DECL_WEAK but its definition in lib/dolphin/os/
// OSReport.cpp is #if 0'd out, so the reference resolves to address 0 rather
// than failing to link -- nlInitMemory()'s first OSReport() call then jumps to
// null. The game is expected to supply this itself.
void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fflush(stderr);
}

void OSVReport(const char* msg, va_list list) {
    vfprintf(stderr, msg, list);
    fflush(stderr);
}

// --- GX stubs ---
void GXPeekARGB(u16, u16, u32* val) { *val = 0; }

void GXPokeColorUpdate(GXBool) {}
void GXPokeBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) {}

void GXPokeARGB(u16, u16, u32) {}
f32 GXGetYScaleFactor(u16, u16) { return 1.0f; }

// --- VM stubs ---

void VMInit(uintptr_t baseAddr, size_t initialCommitSize, uintptr_t limitAddr) {}

// nlInitMemory() hands the "virtual" (ARAM-paged) allocator a hardcoded
// GameCube address -- VMAlloc(0x7E000000, 0x900000) -- and then writes to it, so
// this cannot stay a no-op or the first write faults. Map the region where the
// game asks for it; nlFree() tells the two heaps apart by testing bit 31 of the
// pointer, so the address genuinely matters.
void VMAlloc(uintptr_t address, size_t size) {
    void* const desired = reinterpret_cast<void*>(address);
#if defined(_WIN32)
    void* const ptr = VirtualAlloc(desired, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (ptr != desired) {
        if (ptr != nullptr) {
            VirtualFree(ptr, 0, MEM_RELEASE);
        }
        fprintf(stderr, "openstrikers: VMAlloc could not reserve %zu bytes at %p\n", size, desired);
    }
#else
    void* const ptr = mmap(desired, size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ptr == MAP_FAILED || ptr != desired) {
        if (ptr != MAP_FAILED) {
            munmap(ptr, size);
        }
        fprintf(stderr, "openstrikers: VMAlloc could not map %zu bytes at %p\n", size, desired);
    }
#endif
}
void VMSetLogStatsCallback(VMLogStatsCallback cb) {}

void LCEnable() {}
void LCDisable() {}
 
BOOL THPInit() {
    return TRUE;
}
 

static void DummyAIDCallback() {}
 
void* AIRegisterDMACallback(void* callback) {
    (void)callback;
    static void* sPrevCallback = (void*)DummyAIDCallback;
    void* prev = sPrevCallback;
    sPrevCallback = callback ? callback : (void*)DummyAIDCallback;
    return prev;
}
 
void AIInitDMA(u32 addr, u32 size) {
    (void)addr;
    (void)size;
}
 
void AIStartDMA() {}
 
u32 AIGetDMAStartAddr() {
    return 0;
}
 
u32 AIGetDSPSampleRate() {
    return 0;
}
 
BOOL OSEnableInterrupts() {
    return TRUE;
}
 
BOOL OSDisableInterrupts() {
    return TRUE;
}
 
BOOL OSRestoreInterrupts(BOOL state) {
    return state;
}

#ifdef GOLDEN_DISABLE_AUDIO
u32 THPAudioDecode(void*, void*, long) { return 0; }
s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) { return 0; }
#endif

} // extern "C"

// --- pad sampling -----------------------------------------------------------

namespace {
PADSamplingCallback sPadSamplingCallback = nullptr;

// Aurora ships every keyboard binding as PAD_KEY_INVALID and leaves the keyboard
// inactive, so with no gamepad plugged in PADRead reports PAD_ERR_NO_CONTROLLER
// on all four ports and cPlatPad::IsConnected() is false forever. Install a
// Dolphin-style layout on port 0 so the game is playable without a controller;
// a real gamepad still takes precedence, since PADRead merges both sources.
//
// This must run after PADInit(), which overwrites the button mapping with the
// (blank) defaults, so it is deferred to the first sampling tick.
void installDefaultKeyboardBindings() {
    static PADKeyButtonBinding buttons[PAD_BUTTON_COUNT] = {
        {SDL_SCANCODE_X, PAD_BUTTON_A},
        {SDL_SCANCODE_Z, PAD_BUTTON_B},
        {SDL_SCANCODE_C, PAD_BUTTON_X},
        {SDL_SCANCODE_S, PAD_BUTTON_Y},
        {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
        {SDL_SCANCODE_D, PAD_TRIGGER_Z},
        {SDL_SCANCODE_Q, PAD_TRIGGER_L},
        {SDL_SCANCODE_W, PAD_TRIGGER_R},
        {SDL_SCANCODE_T, PAD_BUTTON_UP},
        {SDL_SCANCODE_G, PAD_BUTTON_DOWN},
        {SDL_SCANCODE_F, PAD_BUTTON_LEFT},
        {SDL_SCANCODE_H, PAD_BUTTON_RIGHT},
    };
    // influence is ignored by aurora's keyboard path: a pressed key contributes
    // a full-scale deflection on its axis.
    static PADKeyAxisBinding axes[PAD_AXIS_COUNT] = {
        {SDL_SCANCODE_RIGHT, PAD_AXIS_LEFT_X_POS, 0},
        {SDL_SCANCODE_LEFT, PAD_AXIS_LEFT_X_NEG, 0},
        {SDL_SCANCODE_UP, PAD_AXIS_LEFT_Y_POS, 0},
        {SDL_SCANCODE_DOWN, PAD_AXIS_LEFT_Y_NEG, 0},
        {SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 0},
        {SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 0},
        {SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 0},
        {SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 0},
        {SDL_SCANCODE_Q, PAD_AXIS_TRIGGER_L, 0},
        {SDL_SCANCODE_W, PAD_AXIS_TRIGGER_R, 0},
    };

    PADSetKeyButtonBindings(0, buttons);
    PADSetKeyAxisBindings(0, axes);
    PADSetKeyboardActive(0, TRUE);
}
} // namespace

void PADSetSamplingCallback(PADSamplingCallback callback) {
    sPadSamplingCallback = callback;
}

// --- host frame driving -----------------------------------------------------

// The GameCube's frame cadence was the VI retrace: glplatSendFrame() ran once
// per field, 60 times a second, and everything written against "once per frame"
// inherited that rate -- the frame allocator, and in particular the pad
// sampling callback, whose UpdateButtonStateTime() credits a held button with a
// flat 1/targetFPS seconds per call. Nothing here reproduced that. Unpaced, this
// loop runs at whatever the host manages (~2800fps on this machine), so held
// time accrued about 47x too fast: a menu's auto-repeat initial delay and repeat
// rate both expired almost immediately and the d-pad scrolled uncontrollably.
//
// Pace the frame instead of correcting the individual symptoms, because it is
// the invariant the decomp was written against and the divergence is not
// specific to input. Set OPENSTRIKERS_UNCAPPED=1 to run flat out again.
namespace {
bool uncappedFrameRate() {
    static const bool sUncapped = [] {
        const char* v = std::getenv("OPENSTRIKERS_UNCAPPED");
        return v != nullptr && v[0] != ' ' && v[0] != '0';
    }();
    return sUncapped;
}

double hostSeconds() {
#if defined(_WIN32)
    static const double sScale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * sScale;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

// Sleep until `deadline`. Sleep()'s default granularity is ~15ms -- most of a
// frame -- so a plain sleep would overshoot every time; ask for a
// high-resolution timer and spin only the last fraction of a millisecond.
void sleepUntil(double deadline) {
#if defined(_WIN32)
    static HANDLE sTimer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    double remaining = deadline - hostSeconds();
    if (sTimer != nullptr && remaining > 0.001) {
        LARGE_INTEGER due;
        // Negative means relative, in 100ns units. Stop a little short and let
        // the spin below take up the slack rather than risk sleeping past.
        due.QuadPart = -(LONGLONG)((remaining - 0.0005) * 1e7);
        if (SetWaitableTimer(sTimer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(sTimer, INFINITE);
        }
    }
#endif
    while (hostSeconds() < deadline) {
    }
}

void paceFrame() {
    if (uncappedFrameRate()) {
        return;
    }
    const u32 targetFPS = glx_GetTargetFPS();
    if (targetFPS == 0) {
        return;
    }
    const double period = 1.0 / (double)targetFPS;
    static double sNextFrame = 0.0;

    const double now = hostSeconds();
    if (sNextFrame == 0.0) {
        sNextFrame = now;
    }
    sNextFrame += period;
    if (now > sNextFrame + period) {
        // More than a whole frame behind -- a load, a stall, or a breakpoint.
        // Start counting again from here instead of trying to catch up, which
        // would run a burst of unpaced frames to "repay" time that is gone.
        sNextFrame = now + period;
        return;
    }
    sleepUntil(sNextFrame);
}

// Report the achieved frame rate once a second when OPENSTRIKERS_LOG_FPS is set.
// Worth having permanently: "the game is running too fast" does not look like a
// frame-rate bug from the outside -- it looks like menus that scroll too
// quickly, or timers that expire early -- and this answers it in one run.
void logFrameRate() {
    static const bool sEnabled = [] {
        const char* v = std::getenv("OPENSTRIKERS_LOG_FPS");
        return v != nullptr && v[0] != ' ' && v[0] != '0';
    }();
    if (!sEnabled) {
        return;
    }
    static unsigned sFrames = 0;
    static double sSince = 0.0;
    const double now = hostSeconds();
    if (sSince == 0.0) {
        sSince = now;
    }
    ++sFrames;
    const double elapsed = now - sSince;
    if (elapsed >= 1.0) {
        std::fprintf(stderr, "openstrikers: %.1f fps (target %u)\n", sFrames / elapsed,
                     glx_GetTargetFPS());
        sFrames = 0;
        sSince = now;
    }
}

// --- scripted input ---------------------------------------------------------

// Several of the remaining bugs live behind a sequence of button presses --
// pausing and quitting a match, the pre-match intro, a replay -- and none of
// them is reachable with OPENSTRIKERS_AUTO_A, which only knows how to hold A.
// Driving the window from outside with PostMessage works, but its presses are
// timed against wall-clock guesses; this runs off the same frame counter the
// game does, so a sequence repeats run to run.
//
//   OPENSTRIKERS_INPUT="600:START;660:DUP;690:A;750:DUP;780:A"
//
// Entries are `frame:BUTTONS[:holdFrames]`, separated by `;` or `,`. Frames are
// host frames since process start; BUTTONS is one or more names joined by `+`.
// The default hold is 6 frames -- long enough for an edge-triggered
// JustPressed, short enough not to trip a menu's auto-repeat.
//
// Setting OPENSTRIKERS_INPUT also stands OPENSTRIKERS_AUTO_A down at the first
// scripted frame: auto-A gets the run to kickoff, and would otherwise keep
// mashing A through whatever menu the script opens.
struct ScriptedPress {
    u32 frame;
    u32 hold;
    u16 buttons;
};

u16 padButtonByName(const char* name, std::size_t len) {
    struct Entry {
        const char* name;
        u16 mask;
    };
    static const Entry kButtons[] = {
        {"A", PAD_BUTTON_A},        {"B", PAD_BUTTON_B},
        {"X", PAD_BUTTON_X},        {"Y", PAD_BUTTON_Y},
        {"Z", PAD_TRIGGER_Z},       {"L", PAD_TRIGGER_L},
        {"R", PAD_TRIGGER_R},       {"START", PAD_BUTTON_START},
        {"DUP", PAD_BUTTON_UP},     {"DDOWN", PAD_BUTTON_DOWN},
        {"DLEFT", PAD_BUTTON_LEFT}, {"DRIGHT", PAD_BUTTON_RIGHT},
    };
    for (const Entry& e : kButtons) {
        if (std::strlen(e.name) == len && std::strncmp(e.name, name, len) == 0) {
            return e.mask;
        }
    }
    std::fprintf(stderr, "openstrikers: OPENSTRIKERS_INPUT: unknown button '%.*s'\n", (int)len,
                 name);
    return 0;
}

// Parsed once. A malformed entry is reported and skipped rather than fatal:
// this is a debugging aid, and losing a whole run to a typo in the tail of a
// long sequence is worse than losing the one press.
const std::vector<ScriptedPress>& scriptedInput() {
    static const std::vector<ScriptedPress> sScript = [] {
        std::vector<ScriptedPress> script;
        const char* spec = std::getenv("OPENSTRIKERS_INPUT");
        if (spec == nullptr || *spec == '\0') {
            return script;
        }
        const std::string text(spec);
        std::size_t pos = 0;
        while (pos <= text.size()) {
            std::size_t end = text.find_first_of(";,", pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string entry = text.substr(pos, end - pos);
            pos = end + 1;
            if (entry.empty()) {
                continue;
            }

            const std::size_t firstColon = entry.find(':');
            if (firstColon == std::string::npos) {
                std::fprintf(stderr, "openstrikers: OPENSTRIKERS_INPUT: no ':' in '%s'\n",
                             entry.c_str());
                continue;
            }
            const std::size_t secondColon = entry.find(':', firstColon + 1);

            ScriptedPress press{};
            press.frame = (u32)std::strtoul(entry.c_str(), nullptr, 10);
            press.hold = 6;
            if (secondColon != std::string::npos) {
                press.hold = (u32)std::strtoul(entry.c_str() + secondColon + 1, nullptr, 10);
                if (press.hold == 0) {
                    press.hold = 1;
                }
            }

            const std::size_t namesEnd =
                (secondColon == std::string::npos) ? entry.size() : secondColon;
            std::size_t namePos = firstColon + 1;
            while (namePos < namesEnd) {
                std::size_t plus = entry.find('+', namePos);
                if (plus == std::string::npos || plus > namesEnd) {
                    plus = namesEnd;
                }
                press.buttons |= padButtonByName(entry.c_str() + namePos, plus - namePos);
                namePos = plus + 1;
            }
            if (press.buttons == 0) {
                continue;
            }
            script.push_back(press);
        }
        for (const ScriptedPress& press : script) {
            std::fprintf(stderr,
                         "openstrikers: scripted input: frame %u buttons 0x%04x hold %u\n",
                         press.frame, press.buttons, press.hold);
        }
        return script;
    }();
    return sScript;
}

// The frame at which the script first does something, so auto-A can get out of
// the way before it. 0 when there is no script.
u32 scriptedInputFirstFrame() {
    static const u32 sFirst = [] {
        u32 first = 0;
        for (const ScriptedPress& press : scriptedInput()) {
            if (first == 0 || press.frame < first) {
                first = press.frame;
            }
        }
        return first;
    }();
    return sFirst;
}

// Buttons the script wants held on `frame`, or 0. Overlapping entries merge.
u16 scriptedInputButtons(u32 frame) {
    u16 buttons = 0;
    for (const ScriptedPress& press : scriptedInput()) {
        if (frame >= press.frame && frame < press.frame + press.hold) {
            buttons |= press.buttons;
        }
    }
    return buttons;
}
} // namespace

bool osHostFrameBegin() {
    // Hold the frame to the target rate before doing anything else, so a frame
    // skipped below (minimized, unfocused) is paced too rather than spinning.
    paceFrame();
    logFrameRate();

    // Pump SDL. Without this the window never processes messages (Windows marks
    // it "Not Responding") and aurora's input state -- which is what PADRead
    // reads from -- is never refreshed.
    for (const AuroraEvent* ev = aurora_update(); ev != nullptr && ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_EXIT) {
            aurora_shutdown();
            std::exit(0);
        }
    }

    // The host frame is our sampling tick. Run it after the event pump so the
    // keyboard state PADRead samples is this frame's, and before begin_frame so
    // it still happens on a frame we are going to skip (minimized/paused) --
    // otherwise input would freeze whenever the window is not being drawn.
    // OPENSTRIKERS_AUTO_A=1 pulses A on port 0 so an unattended run gets past
    // the prompts that wait on it. The kickoff is the one that matters: with
    // OPENSTRIKERS_SKIP_FE the match loads by itself but still waits for A, so
    // a run left alone sits on the loading transition and every captured frame
    // comes out black. It is pulsed rather than held because those prompts are
    // edge triggered, and merged rather than substituted by aurora, so a real
    // keyboard or gamepad keeps working alongside it.
    {
        static const bool sAutoA = [] {
            const char* v = std::getenv("OPENSTRIKERS_AUTO_A");
            return v != nullptr && *v != '0';
        }();
        static u32 sFrame = 0;
        const u32 frame = sFrame++;

        const u16 scripted = scriptedInputButtons(frame);
        const u32 scriptStart = scriptedInputFirstFrame();
        // Auto-A stands down once the script is due, so it cannot mash A
        // through the menus the script exists to navigate.
        const bool autoAActive = sAutoA && (scriptStart == 0 || frame < scriptStart);

        if (sAutoA || scriptStart != 0) {
            PADStatus st{};
            st.err = PAD_ERR_NONE;
            st.button = scripted;
            if (autoAActive && ((frame / 15) % 2) == 0) {
                st.button |= PAD_BUTTON_A;
            }
            // Written every frame, including when nothing is pressed: leaving
            // the last status latched would hold a button down forever.
            PADSetVirtualStatus(0, &st);
        }

        if (scripted != 0 && (frame == 0 || scriptedInputButtons(frame - 1) != scripted)) {
            std::fprintf(stderr, "openstrikers: frame %u: scripted buttons 0x%04x\n", frame,
                         scripted);
        }
    }

    if (sPadSamplingCallback != nullptr) {
        static bool sBindingsInstalled = false;
        if (!sBindingsInstalled) {
            sBindingsInstalled = true;
            installDefaultKeyboardBindings();
        }
        sPadSamplingCallback();
    }

    // Every card operation completes off the EXI interrupt on hardware, so aurora
    // queues the completion rather than re-entering the caller inline, and this is
    // what delivers it. It sits outside the pad block above, which does nothing
    // until the game installs a sampling callback.
    CARDServicePendingCallbacks();

    return aurora_begin_frame();
}

void osHostFrameEnd() {
    aurora_end_frame();
}
