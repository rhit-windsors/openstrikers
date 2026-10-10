#pragma once

#include <cstdio>
#include <cstring>

// How the picture is shaped. Auto follows the game's own Options > Aspect
// setting; Standard and Widescreen force it (and the output) to 4:3 or 16:9;
// Stretch follows the game's setting but fills the window regardless.
enum class AspectMode { Auto, Standard, Widescreen, Stretch };

// Kept independent of SDL so argument validation needs no graphics device.
struct LaunchOptions {
    const char* disc = nullptr;
    unsigned renderWidth = 0, renderHeight = 0;
    unsigned windowWidth = 1280, windowHeight = 960;
    unsigned display = 0;
    AspectMode aspect = AspectMode::Auto;
    bool fullscreen = false, vsync = false, help = false;
};

inline bool parse_aspect(const char* value, AspectMode& aspect) {
    if (!std::strcmp(value, "auto")) aspect = AspectMode::Auto;
    else if (!std::strcmp(value, "4:3")) aspect = AspectMode::Standard;
    else if (!std::strcmp(value, "16:9")) aspect = AspectMode::Widescreen;
    else if (!std::strcmp(value, "stretch")) aspect = AspectMode::Stretch;
    else return false;
    return true;
}

inline bool parse_resolution(const char* value, unsigned& width, unsigned& height) {
    unsigned w = 0, h = 0;
    char tail = 0;
    if (std::sscanf(value, "%ux%u%c", &w, &h, &tail) != 2 ||
        w < 640 || h < 480 || w > 7680 || h > 4320) return false;
    width = w;
    height = h;
    return true;
}

inline bool parse_launch_options(int argc, char* argv[], LaunchOptions& options) {
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && std::strcmp(arg, "--") == 0) { positional = true; continue; }
        if (!positional && arg[0] == '-') {
            if (!std::strcmp(arg, "--help") || !std::strcmp(arg, "-h")) options.help = true;
            else if (!std::strcmp(arg, "--fullscreen")) options.fullscreen = true;
            else if (!std::strcmp(arg, "--windowed")) options.fullscreen = false;
            else if (!std::strcmp(arg, "--vsync")) options.vsync = true;
            else if (!std::strcmp(arg, "--no-vsync")) options.vsync = false;
            else if (!std::strcmp(arg, "--original-aspect")) options.aspect = AspectMode::Standard;
            else if (!std::strcmp(arg, "--widescreen")) options.aspect = AspectMode::Widescreen;
            else if (!std::strcmp(arg, "--stretch")) options.aspect = AspectMode::Stretch;
            else if (!std::strcmp(arg, "--aspect")) {
                if (++i == argc || !parse_aspect(argv[i], options.aspect)) return false;
            }
            else if (!std::strcmp(arg, "--resolution") || !std::strcmp(arg, "--window-size")) {
                const bool render = !std::strcmp(arg, "--resolution");
                if (++i == argc) return false;
                if (render && !std::strcmp(argv[i], "native")) {
                    options.renderWidth = options.renderHeight = 0;
                } else if (!parse_resolution(argv[i], render ? options.renderWidth : options.windowWidth,
                                             render ? options.renderHeight : options.windowHeight)) return false;
            } else if (!std::strcmp(arg, "--display")) {
                if (++i == argc) return false;
                unsigned display = 0;
                char tail = 0;
                if (std::sscanf(argv[i], "%u%c", &display, &tail) != 1 || display > 63) return false;
                options.display = display;
            } else return false;
        } else {
            if (options.disc) return false;
            options.disc = arg;
        }
    }
    return options.help || options.disc != nullptr;
}
