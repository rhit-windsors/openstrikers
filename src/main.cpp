#include <cstdlib>
#include <aurora/main.h>   // DEVE venire prima della tua definizione di main,
                            // altrimenti la macro non si applica e hai un
                            // vero secondo main() -> conflitto di link
#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <aurora/dvd.h>
#include <dolphin/vi.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_filesystem.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif
#include "compat_shims.h"
#include "launch_options.h"
#include "net/session.h"

#include <cstdio>

int game_main(); // dichiarato qui, definito nel decomp rinominato

static AspectMode s_aspect = AspectMode::Auto;

// The game renders 16:9 by widening its projection into the same EFB and
// leaving the TV to stretch it, so here the output aspect is the whole job:
// Aurora scales the logical EFB onto a render target of the locked aspect.
bool osHostSyncWidescreen(bool gameWidescreen) {
    static int s_applied = -1;
    bool widescreen = gameWidescreen;
    // Netplay and replay fix it for everyone: 16:9 moves the game's camera.
    if (const int fixed = net::sessionWidescreen(); fixed >= 0) widescreen = fixed != 0;
    else if (s_aspect == AspectMode::Standard) widescreen = false;
    else if (s_aspect == AspectMode::Widescreen) widescreen = true;
    if (s_applied != static_cast<int>(widescreen)) {
        s_applied = widescreen;
        if (s_aspect != AspectMode::Stretch) {
            VILockAspectRatio(widescreen ? 16 : 4, widescreen ? 9 : 3);
        }
        fprintf(stderr, "openstrikers: game aspect %s\n", widescreen ? "16:9" : "4:3");
    }
    return widescreen;
}

#ifdef _WIN32
// The console CRT supplies ANSI argv. SDL and disc I/O expect UTF-8 paths.
struct Utf8Arguments {
    int count = 0;
    char** values = nullptr;
    Utf8Arguments() {
        LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide) std::exit(2);
        values = static_cast<char**>(std::calloc(count + 1, sizeof(char*)));
        if (!values) std::exit(2);
        for (int i = 0; i < count; ++i) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            values[i] = static_cast<char*>(std::malloc(bytes));
            if (!values[i]) std::exit(2);
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, values[i], bytes, nullptr, nullptr);
        }
        LocalFree(wide);
    }
    ~Utf8Arguments() {
        for (int i = 0; i < count; ++i) std::free(values[i]);
        std::free(values);
    }
};
#endif

int main(int argc, char* argv[])   // <-- diventa aurora_main via macro
{
    // The game's own diagnostics go to stdout through nlPrintf, and when the
    // process dies on a fault a block-buffered stdout takes the last few
    // thousand characters with it -- exactly the ones that say why. Both
    // streams are unbuffered so a crash log is complete up to the fault.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

#ifdef _WIN32
    Utf8Arguments unicodeArgs;
    argc = unicodeArgs.count;
    argv = unicodeArgs.values;
#endif

    LaunchOptions options;
    const bool valid = parse_launch_options(argc, argv, options);
    if (!valid || options.help) {
        fprintf(valid ? stdout : stderr,
                "usage: %s [options] <disc image>\n"
                "  --fullscreen / --windowed\n"
                "  --resolution native|WIDTHxHEIGHT   Internal rendering, up to 7680x4320\n"
                "  --window-size WIDTHxHEIGHT         Windowed size (default 1280x960)\n"
                "  --display N                       Monitor index, starting at 0\n"
                "  --vsync / --no-vsync\n"
                "  --msaa 1|2|4|8                     Anti-aliasing samples per pixel (default 1, off)\n"
                "  --aspect auto|4:3|16:9|stretch     Picture shape (default auto: the\n"
                "                                     game's Options > Aspect setting)\n"
                "  --original-aspect / --widescreen / --stretch   Same as 4:3 / 16:9 / stretch\n"
                "  --net-host [PORT]                  Host a network match (UDP, default 52320)\n"
                "  --net-players N                    Machines in the match, host included (2-8)\n"
                "  --net-delay FRAMES                 Input delay; default 4 (raise it for distant players)\n"
                "  --net-join HOST[:PORT]             Join a network match\n"
                "  --net-name NAME                    Your name, shown over your player's head\n"
                "  --net-seat N                       Join: the machine number a lobby gave you\n"
                "  --net-slots A,B,...                Host: machine driving each fielder (home\n"
                "                                     captain, 3 sidekicks, then away), - = computer\n"
                "  --player-names A,B,...             Offline: names over ports 1-4's players\n"
                "Match (network play, or OPENSTRIKERS_SKIP_FE; a host's apply to everyone):\n"
                "  --home TEAM / --away TEAM          daisy dk luigi mario peach waluigi wario yoshi\n"
                "  --home-sidekick SK / --away-sidekick SK   toad koopa hammerbros birdo\n"
                "  --stadium NAME                     mario peach dk wario yoshi super forbidden\n"
                "  --match-minutes N / --skill 0-4    Length; computer skill (Training..Legend)\n"
                "  --items on|off / --bowser on|off / --super-strikes on|off\n"
                "  --end-after-match                  Exit once the match is over (the launcher's lobby)\n"
                "  F11 or Alt+Enter toggles fullscreen while playing.\n",
                argc > 0 ? argv[0] : "openstrikers");
        return valid ? 0 : 2;
    }
    const char* disc = options.disc;
    s_aspect = options.aspect;
    // Check the disc before bringing anything up. aurora_dvd_open() failing
    // after aurora_initialize() leaves a half-built graphics stack that
    // aurora_shutdown() faults on, so the useful message is followed by a
    // crash. Validating here keeps a missing or mistyped path an ordinary
    // error exit.
    if (SDL_IOStream* file = SDL_IOFromFile(disc, "rb")) {
        SDL_CloseIO(file);
    } else {
        fprintf(stderr, "openstrikers: cannot read disc image %s\n", disc);
        return 2;
    }
    const AuroraConfig config = {
        .appName = "openstrikers",
        // Useful for automated runs whose process environment prevents SDL
        // from discovering its normal per-user preference directory.
        .userPath = std::getenv("OPENSTRIKERS_CACHE_PATH"),
        .cachePath = std::getenv("OPENSTRIKERS_CACHE_PATH"),
        // Aurora looks for initial_pipeline_cache.db here, which the build
        // copies next to the executable. Left unset it is resolved against
        // the working directory, so launching from anywhere else on a fresh
        // machine would skip the seed and drop draws while shaders compile.
        .resourcesPath = SDL_GetBasePath(),
        .msaa = options.msaa,
        .vsync = options.vsync,
        // Position the window on the selected monitor before entering fullscreen.
        .startFullscreen = false,
        // OPENSTRIKERS_TEXTURE_DUMPS=1 writes every uploaded texture to
        // %APPDATA%/openstrikers/texture_dumps as DDS, named by its source key
        // (format, dimensions, hash). It answers "is the right asset being
        // decoded?" directly, and it is worth being able to turn on without a
        // rebuild -- the runs it is useful for are the ones you are already
        // repeating.
        .allowTextureDumps = std::getenv("OPENSTRIKERS_TEXTURE_DUMPS") != nullptr,
        .windowPosX = SDL_WINDOWPOS_CENTERED,
        .windowPosY = SDL_WINDOWPOS_CENTERED,
        .windowWidth = options.windowWidth,
        .windowHeight = options.windowHeight,
        .mem1Size = OPENSTRIKERS_MEM1_SIZE,
        .mem2Size = OPENSTRIKERS_MEM2_SIZE,
    };
    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    // The game called PADInit from nlInitMemory, but on the host that runs
    // during static initialisation, before Aurora's input router exists, and
    // the input layer PADInit registers would be lost (decomp-late/148).
    PADInit();
    int displayCount = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
    if (displays && options.display < static_cast<unsigned>(displayCount)) {
        SDL_SetWindowPosition(info.window, SDL_WINDOWPOS_CENTERED_DISPLAY(displays[options.display]),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(displays[options.display]));
    } else {
        fprintf(stderr, "openstrikers: selected monitor unavailable; using default monitor\n");
    }
    SDL_free(displays);
    VISetWindowFullscreen(options.fullscreen);
    // The game draws into a GameCube-sized EFB that Aurora scales onto the
    // render target, so the target's aspect is the TV's: lock one before the
    // first frame. osHostSyncWidescreen() re-locks it once the game's own
    // Aspect option is known.
    const bool stretch = options.aspect == AspectMode::Stretch;
    if (!stretch) {
        const bool widescreen = options.aspect == AspectMode::Widescreen;
        VILockAspectRatio(widescreen ? 16 : 4, widescreen ? 9 : 3);
    }
    AuroraSetViewportPolicy(stretch ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
    VISetRenderResolution(options.renderWidth, options.renderHeight);
    SDL_SyncWindow(info.window);
    u32 renderWidth = 0, renderHeight = 0;
    AuroraGetRenderSize(&renderWidth, &renderHeight);
    fprintf(stderr, "openstrikers: requested render %ux%u, %s, vsync %s, msaa %ux\n", renderWidth, renderHeight,
            options.fullscreen ? "fullscreen" : "windowed", options.vsync ? "on" : "off", options.msaa);
    if (!aurora_dvd_open(disc)) {
        fprintf(stderr, "openstrikers: failed to open disc image %s\n", disc);
        aurora_shutdown();
        return 1;
    }
    // Input replay or netplay, if asked for: must be set up before the game's
    // first task frame reads the fixed step, the seed or the pads.
    net::sessionStart(options);
    return game_main();
}
