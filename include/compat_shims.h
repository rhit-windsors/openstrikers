#pragma once
#include "compat_shims/runtime.h"
#include "compat_shims/dolphin_stubs.h"
#include "compat_shims/musyx_types.h"

// GameCube memory sizes, shared between main.cpp's AuroraConfig and the
// early OSInit() in compat_shims.cpp -- they must agree.
#define OPENSTRIKERS_MEM1_SIZE (128 * 1024 * 1024)
#define OPENSTRIKERS_MEM2_SIZE ARAM_DEFAULT_SIZE

// --- host frame driving -----------------------------------------------------
// The decomp's frame boundary is glplatSendFrame(): everything between
// glxSwapPre and glxSwapPost is one GameCube frame. Aurora needs the host frame
// opened before any GX command is recorded and closed after the copy-to-display,
// and it needs its event pump run once a frame or the window never responds and
// PAD input never updates. These live in compat_shims.cpp so the decomp does not
// have to include aurora headers.
//
// Returns false when the host cannot present right now (window minimized, or
// paused on focus loss). The caller must then skip the frame *and* not call
// osHostFrameEnd().
bool osHostFrameBegin();
void osHostFrameEnd();

// --- host switches ----------------------------------------------------------
// OPENSTRIKERS_SKIP_FE=1 starts an exhibition match directly instead of running
// the front end. It is read in three places -- the initial task state, the
// GameInfoManager defaults it implies, and whether the front end counts as
// already booted when a match is quit -- so the parse lives here rather than
// being spelled out at each of them.
#ifdef __cplusplus
#include <cstdlib>
inline bool osHostSkipFrontEnd() {
    const char* v = std::getenv("OPENSTRIKERS_SKIP_FE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}
#endif

// --- diagnostics ------------------------------------------------------------
// Print a backtrace to stderr, translated to link-time addresses so addr2line
// can resolve it (see "Crashes that only happen without a debugger" in the
// README). The crash handler uses this, and so does anything that needs to
// answer "who called this?" in a build where a debugger changes the layout
// enough to hide the bug. `skip` drops that many innermost frames.
extern "C" void osDumpBacktrace(const char* tag, unsigned skip);
