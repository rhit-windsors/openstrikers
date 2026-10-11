#pragma once

#include <cstdio>
#include <cstring>
#include <initializer_list>

// How the picture is shaped. Auto follows the game's own Options > Aspect
// setting; Standard and Widescreen force it (and the output) to 4:3 or 16:9;
// Stretch follows the game's setting but fills the window regardless.
enum class AspectMode { Auto, Standard, Widescreen, Stretch };

// Netplay: host a session on a UDP port, or join one.
enum class NetRole { None, Host, Join };
constexpr unsigned kDefaultNetPort = 52320;

// The match to play when the front end is skipped (network play, and
// OPENSTRIKERS_SKIP_FE). -1 leaves a field at the game's default. Values are
// the game's own: eTeamID, eSidekickID, eStadiumID, GameplaySettings::eSkillLevel.
struct MatchSettings {
    int homeTeam = -1, awayTeam = -1;
    int homeSidekick = -1, awaySidekick = -1;
    int stadium = -1;
    int skill = -1;
    int matchSeconds = -1;
    int items = -1, bowser = -1, superStrikes = -1; // 0 off, 1 on
    // The game's own 16:9 mode. It moves the camera, and what is on screen
    // decides which effects draw random numbers, so every machine in a
    // network match uses the host's.
    int widescreen = -1;
};

inline int parse_name(const char* value, std::initializer_list<const char*> names) {
    int i = 0;
    for (const char* name : names) {
        if (!std::strcmp(value, name)) return i;
        ++i;
    }
    return -1;
}

// In eTeamID order.
inline int parse_team(const char* v) {
    return parse_name(v, {"daisy", "dk", "luigi", "mario", "peach", "waluigi", "wario", "yoshi"});
}
// In eSidekickID order.
inline int parse_sidekick(const char* v) { return parse_name(v, {"toad", "koopa", "hammerbros", "birdo"}); }
// In eStadiumID order.
inline int parse_stadium(const char* v) {
    return parse_name(v, {"mario", "peach", "dk", "wario", "yoshi", "super", "forbidden"});
}
inline int parse_on_off(const char* v) { return parse_name(v, {"off", "on"}); }

// Kept independent of SDL so argument validation needs no graphics device.
struct LaunchOptions {
    const char* disc = nullptr;
    unsigned renderWidth = 0, renderHeight = 0;
    unsigned windowWidth = 1280, windowHeight = 960;
    unsigned display = 0;
    AspectMode aspect = AspectMode::Auto;
    bool fullscreen = false, vsync = false, help = false;
    NetRole netRole = NetRole::None;
    unsigned netPort = kDefaultNetPort; // host: port to listen on
    const char* netAddress = nullptr;   // join: host[:port]
    unsigned netPlayers = 2;            // host: machines in the session, itself included
    unsigned netDelay = 0;              // host: input delay in frames; 0 picks a default
    const char* netName = nullptr;
    MatchSettings match;
};

inline bool parse_unsigned(const char* value, unsigned low, unsigned high, unsigned& out) {
    unsigned v = 0;
    char tail = 0;
    if (std::sscanf(value, "%u%c", &v, &tail) != 1 || v < low || v > high) return false;
    out = v;
    return true;
}

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
            } else if (!std::strcmp(arg, "--net-host")) {
                options.netRole = NetRole::Host;
                // The port is optional.
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    if (!parse_unsigned(argv[++i], 1, 65535, options.netPort)) return false;
                }
            } else if (!std::strcmp(arg, "--net-join")) {
                if (++i == argc || argv[i][0] == '\0') return false;
                options.netRole = NetRole::Join;
                options.netAddress = argv[i];
            } else if (!std::strcmp(arg, "--net-players")) {
                if (++i == argc || !parse_unsigned(argv[i], 2, 8, options.netPlayers)) return false;
            } else if (!std::strcmp(arg, "--net-delay")) {
                if (++i == argc || !parse_unsigned(argv[i], 1, 30, options.netDelay)) return false;
            } else if (!std::strcmp(arg, "--net-name")) {
                if (++i == argc) return false;
                options.netName = argv[i];
            } else if (!std::strcmp(arg, "--home") || !std::strcmp(arg, "--away")) {
                if (++i == argc) return false;
                int& team = arg[2] == 'h' ? options.match.homeTeam : options.match.awayTeam;
                if ((team = parse_team(argv[i])) < 0) return false;
            } else if (!std::strcmp(arg, "--home-sidekick") || !std::strcmp(arg, "--away-sidekick")) {
                if (++i == argc) return false;
                int& sidekick = arg[2] == 'h' ? options.match.homeSidekick : options.match.awaySidekick;
                if ((sidekick = parse_sidekick(argv[i])) < 0) return false;
            } else if (!std::strcmp(arg, "--stadium")) {
                if (++i == argc || (options.match.stadium = parse_stadium(argv[i])) < 0) return false;
            } else if (!std::strcmp(arg, "--skill")) {
                unsigned skill = 0;
                if (++i == argc || !parse_unsigned(argv[i], 0, 4, skill)) return false;
                options.match.skill = int(skill);
            } else if (!std::strcmp(arg, "--match-minutes")) {
                unsigned minutes = 0;
                if (++i == argc || !parse_unsigned(argv[i], 1, 30, minutes)) return false;
                options.match.matchSeconds = int(minutes * 60);
            } else if (!std::strcmp(arg, "--items") || !std::strcmp(arg, "--bowser") ||
                       !std::strcmp(arg, "--super-strikes")) {
                if (++i == argc) return false;
                int& flag = arg[2] == 'i' ? options.match.items
                          : arg[2] == 'b' ? options.match.bowser : options.match.superStrikes;
                if ((flag = parse_on_off(argv[i])) < 0) return false;
            } else return false;
        } else {
            if (options.disc) return false;
            options.disc = arg;
        }
    }
    return options.help || options.disc != nullptr;
}
