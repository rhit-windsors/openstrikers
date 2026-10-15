#pragma once
#include "compat_shims/runtime.h"
#include "compat_shims/dolphin_stubs.h"
// With audio off, the game only needs MusyX's types; with it on, the real
// header (same include guard) brings the API with them.
#ifdef GOLDEN_DISABLE_AUDIO
#include "compat_shims/musyx_types.h"
#else
#include "musyx/musyx.h"
#endif

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

// Called once a frame with the game's Aspect option (VisualSettings::
// mIsWidescreen). Locks the host output to 16:9 or 4:3 to match, and returns
// the value the game should use -- the same one, unless the command line forced
// an aspect, in which case the caller writes the forced value back.
bool osHostSyncWidescreen(bool gameWidescreen);

// --- determinism (input replay and netplay) ---------------------------------
// Lockstep netplay sends only controller input, so every machine has to turn
// the same input into the same game. These are the places the game would
// otherwise take something from the outside world; the host owns them (see
// src/net/session.cpp). With no session active each one behaves as before.
#ifdef __cplusplus
struct PADStatus;
// The constant per-frame delta in milliseconds that nlTask uses instead of
// the wall clock, or 0 to use the wall clock.
float osHostFixedStepMs();
// The seed for nlDefaultSeed; `fallback` is what the game would have used.
unsigned int osHostRandomSeed(unsigned int fallback);
// Completes queued disc reads (aurora's deferred DVD mode). Called from
// nlServiceFileSystem.
void osHostServiceDisc();
// Fills all four ports for this frame and returns true when the host supplies
// input (replayed or agreed over the network); returns false to have the
// caller read the local controllers with PADRead. `frameSample` is false for
// the one-off read at pad initialisation.
bool osHostReadPads(PADStatus* status, bool frameSample);

// Trigonometry with the same bits on every machine (src/net/detmath.cpp), for
// the simulation: the C runtime's can differ between CPUs and Windows versions.
extern "C" float osDetSinf(float x);
extern "C" float osDetCosf(float x);
extern "C" float osDetTanf(float x);
extern "C" float osDetAtan2f(float y, float x);
extern "C" double osDetPowi(double x, int n);

// --- network slots ------------------------------------------------------------
// A network match has a controller slot per fielder: slots 0-3 are the home
// captain and three sidekicks, 4-7 the same for the away team.
//
// The team a pad plays for, -1 for none, or OS_HOST_PAD_SIDE_GAME to use the
// game's own setting (no network match).
#define OS_HOST_PAD_SIDE_GAME (-2)
int osHostPadSide(int pad);
// The fielder (0 captain, 1-3 sidekicks) a pad controls, or -1 to leave it to
// the game.
int osHostPadFielder(int pad);
// True for a team whose four fielders all have a human: nobody swaps.
bool osHostTeamLocked(int side);
// Rumble for a slot, on whichever controller of this machine drives it.
void osHostControlMotor(int pad, unsigned int command);
// The name to draw over the player a slot controls, or NULL to keep the
// controller number: online, the name of the machine driving the slot
// (numbered when one machine drives several); offline, the --player-names
// entry for that port. Drawing only -- never feed it into the simulation.
const char* osHostPadName(int pad);

// The match the skip-the-menus path sets up: the host's in a network match,
// else the command line's. -1 keeps the game's own value. Teams, sidekicks and
// stadium are eTeamID / eSidekickID / eStadiumID; the rest 0 off, 1 on.
struct OsHostMatch {
    int homeTeam, awayTeam, homeSidekick, awaySidekick, stadium;
    int items, bowser, superStrikes;
};
bool osHostMatch(OsHostMatch* out);
#endif

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
