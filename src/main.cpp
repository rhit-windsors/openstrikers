#include <cstdlib>
#include <aurora/main.h>   // DEVE venire prima della tua definizione di main,
                            // altrimenti la macro non si applica e hai un
                            // vero secondo main() -> conflitto di link
#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <aurora/dvd.h>
#include "compat_shims.h"

#include <cstdio>

int game_main(); // dichiarato qui, definito nel decomp rinominato

static const char* disc_path(int argc, char* argv[]) {
  if (argc > 1) {
    return argv[1];
  }
  return nullptr;
}

int main(int argc, char* argv[])   // <-- diventa aurora_main via macro
{
    // The game's own diagnostics go to stdout through nlPrintf, and when the
    // process dies on a fault a block-buffered stdout takes the last few
    // thousand characters with it -- exactly the ones that say why. Both
    // streams are unbuffered so a crash log is complete up to the fault.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    const char* disc;
    disc = disc_path(argc, argv);
    // Check the disc before bringing anything up. aurora_dvd_open() failing
    // after aurora_initialize() leaves a half-built graphics stack that
    // aurora_shutdown() faults on, so the useful message is followed by a
    // crash. Validating here keeps a missing or mistyped path an ordinary
    // error exit.
    if (disc == nullptr) {
        fprintf(stderr, "usage: %s <disc image>\n",
                argc > 0 ? argv[0] : "openstrikers");
        return 2;
    }
    if (FILE* f = fopen(disc, "rb")) {
        fclose(f);
    } else {
        fprintf(stderr, "openstrikers: cannot read disc image %s\n", disc);
        return 2;
    }
    const AuroraConfig config = {
        .appName = "openstrikers",
        .vsync = false,
        .startFullscreen = false,
        // OPENSTRIKERS_TEXTURE_DUMPS=1 writes every uploaded texture to
        // %APPDATA%/openstrikers/texture_dumps as DDS, named by its source key
        // (format, dimensions, hash). It answers "is the right asset being
        // decoded?" directly, and it is worth being able to turn on without a
        // rebuild -- the runs it is useful for are the ones you are already
        // repeating.
        .allowTextureDumps = std::getenv("OPENSTRIKERS_TEXTURE_DUMPS") != nullptr,
        .mem1Size = OPENSTRIKERS_MEM1_SIZE,
        .mem2Size = OPENSTRIKERS_MEM2_SIZE,
    };
    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    // AuroraSetDisplayAspect(4/3);
    if (!aurora_dvd_open(disc)) {
        fprintf(stderr, "openstrikers: failed to open disc image %s\n", disc);
        aurora_shutdown();
        return 1;
    }
    return game_main();
}
