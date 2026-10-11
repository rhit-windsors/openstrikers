#include "launch_options.h"
#include <initializer_list>
#include <cstdlib>

static LaunchOptions parse(std::initializer_list<const char*> args, bool expected = true) {
    char* argv[32]{};
    int argc = 0;
    argv[argc++] = const_cast<char*>("openstrikers");
    for (auto arg : args) argv[argc++] = const_cast<char*>(arg);
    LaunchOptions options;
    if (parse_launch_options(argc, argv, options) != expected) {
        std::fprintf(stderr, "Unexpected argument validation result\n");
        std::exit(1);
    }
    return options;
}

static void check(bool valid) { if (!valid) std::exit(1); }
int main() {
    auto legacy = parse({"C:/My Games/disc.iso"});
    check(!legacy.fullscreen && !legacy.vsync && !legacy.renderWidth &&
          legacy.aspect == AspectMode::Auto);
    auto fourK = parse({"--fullscreen", "--resolution", "3840x2160", "--vsync",
                        "--display", "2", "C:/My Games/disc.iso"});
    check(fourK.fullscreen && fourK.vsync && fourK.renderWidth == 3840 && fourK.renderHeight == 2160 &&
          fourK.display == 2 && !std::strcmp(fourK.disc, "C:/My Games/disc.iso"));
    auto override = parse({"game.iso", "--fullscreen", "--windowed", "--stretch", "--original-aspect",
                           "--vsync", "--no-vsync", "--resolution", "native", "--window-size", "1920x1080"});
    check(!override.fullscreen && !override.vsync &&
          override.aspect == AspectMode::Standard && !override.renderWidth &&
          override.windowWidth == 1920 && override.windowHeight == 1080);
    check(!std::strcmp(parse({"--", "-disc.iso"}).disc, "-disc.iso"));
    check(parse({"--help"}).help);
    auto wide = parse({"--widescreen", "--resolution", "3840x2160", "game.iso"});
    check(wide.aspect == AspectMode::Widescreen && wide.renderWidth == 3840);
    check(parse({"--widescreen", "--original-aspect", "game.iso"}).aspect == AspectMode::Standard);
    check(parse({"--widescreen", "--stretch", "game.iso"}).aspect == AspectMode::Stretch);
    check(parse({"--stretch", "--widescreen", "game.iso"}).aspect == AspectMode::Widescreen);
    check(parse({"--aspect", "16:9", "game.iso"}).aspect == AspectMode::Widescreen);
    check(parse({"--widescreen", "--aspect", "auto", "game.iso"}).aspect == AspectMode::Auto);
    check(parse({"--aspect", "4:3", "game.iso"}).aspect == AspectMode::Standard);
    check(parse({"--aspect", "stretch", "game.iso"}).aspect == AspectMode::Stretch);
    parse({"game.iso", "--aspect"}, false);
    parse({"game.iso", "--aspect", "21:9"}, false);
    parse({}, false);
    parse({"game.iso", "another.iso"}, false);
    parse({"game.iso", "--unknown"}, false);
    parse({"game.iso", "--resolution"}, false);
    parse({"game.iso", "--display", "64"}, false);
    parse({"game.iso", "--display", "-1"}, false);
    parse({"game.iso", "--display", "1junk"}, false);
    for (auto bad : {"0x0", "3840x0", "8000x4320", "3840x5000", "3840x2160junk", "3840", "abc"})
        parse({"game.iso", "--resolution", bad}, false);
    check(legacy.netRole == NetRole::None);
    auto host = parse({"--net-host", "game.iso"});
    check(host.netRole == NetRole::Host && host.netPort == kDefaultNetPort && !std::strcmp(host.disc, "game.iso"));
    auto host4 = parse({"--net-host", "40000", "--net-players", "4", "--net-delay", "5", "--net-name", "Riley", "game.iso"});
    check(host4.netPort == 40000 && host4.netPlayers == 4 && host4.netDelay == 5 && !std::strcmp(host4.netName, "Riley"));
    auto join = parse({"game.iso", "--net-join", "192.168.1.20:40000"});
    check(join.netRole == NetRole::Join && !std::strcmp(join.netAddress, "192.168.1.20:40000"));
    parse({"game.iso", "--net-join"}, false);
    parse({"game.iso", "--net-host", "70000"}, false);
    parse({"game.iso", "--net-players", "9"}, false);
    parse({"game.iso", "--net-players", "1"}, false);
    parse({"game.iso", "--net-delay", "0"}, false);
    check(legacy.match.homeTeam == -1 && legacy.match.stadium == -1 && legacy.match.items == -1);
    auto match = parse({"--home", "peach", "--away", "dk", "--home-sidekick", "birdo", "--away-sidekick", "toad",
                        "--stadium", "forbidden", "--skill", "3", "--match-minutes", "4", "--items", "off",
                        "--bowser", "on", "--super-strikes", "off", "game.iso"});
    check(match.match.homeTeam == 4 && match.match.awayTeam == 1 && match.match.homeSidekick == 3 &&
          match.match.awaySidekick == 0 && match.match.stadium == 6 && match.match.skill == 3 &&
          match.match.matchSeconds == 240 && match.match.items == 0 && match.match.bowser == 1 &&
          match.match.superStrikes == 0);
    parse({"game.iso", "--home", "bowser"}, false);
    parse({"game.iso", "--stadium", "moon"}, false);
    parse({"game.iso", "--skill", "5"}, false);
    parse({"game.iso", "--items", "maybe"}, false);
    parse({"game.iso", "--match-minutes", "0"}, false);
    std::puts("Launch argument compatibility and validation passed.");
}
