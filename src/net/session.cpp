#include "net/session.h"

#include "net/lockstep.h"
#include "net/state_hash.h"
#include "net/udp.h"

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace net {
namespace {

enum class Mode { Off, Record, Replay, Online };

// One frame of a recording: the state hash as the frame's input was sampled,
// then the input itself. The pads are stored as PADStatus verbatim, so a
// recording only replays on the build that made it.
struct FrameRecord {
    uint32_t frame;
    StateHash hash;
    PADStatus pads[kMaxSlots];
};

struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t seed;
    uint32_t frameRecordSize;
    uint32_t matchSize;
    uint8_t match[64]; // the match settings, as sent to network peers
};

constexpr char kMagic[8] = {'O', 'S', 'K', 'R', 'E', 'C', '1', '\0'};
constexpr uint32_t kVersion = 3; // 2: eight controller slots; 3: match settings
// A GameCube frame: the delta every task sees while a session is active.
constexpr float kFrameMs = 1000.0f / 60.0f;
constexpr int kReportedMismatches = 20;
// Default input delay: 4 frames (67 ms) covers a LAN or a nearby internet
// peer. --net-delay raises it for distant ones.
constexpr unsigned kDefaultDelay = 4;

Mode s_mode = Mode::Off;
uint32_t s_seed = 0;
std::FILE* s_file = nullptr;
uint32_t s_frame = 0;
uint32_t s_jitterMs = 0;
uint32_t s_mismatches = 0;
int64_t s_firstMismatch = -1;

// The match to play when the front end is skipped: the command line's offline,
// the host's online.
MatchSettings s_match;

// --player-names: what to draw over each local port's player offline.
std::string s_localNames[PAD_MAX_CONTROLLERS];

std::unique_ptr<UdpTransport> s_transport;
std::unique_ptr<Lockstep> s_lockstep;
uint64_t s_clockStart = 0;

const char* envValue(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' ? v : nullptr;
}

uint64_t nowMs() { return SDL_GetTicks() - s_clockStart + 1; }

void flushRecording() {
    if (s_file != nullptr) {
        std::fflush(s_file);
    }
}

[[noreturn]] void finishReplay() {
    if (s_mismatches == 0) {
        std::fprintf(stderr, "openstrikers: replay: %u frames, state identical throughout\n", s_frame);
    } else {
        std::fprintf(stderr, "openstrikers: replay: %u frames, %u mismatched, first at frame %lld\n", s_frame,
                     s_mismatches, (long long)s_firstMismatch);
    }
    std::fflush(stderr);
    aurora_shutdown();
    std::exit(s_mismatches == 0 ? 0 : 3);
}

void compareHash(const FrameRecord& rec, const StateHash& now) {
    if (rec.hash.combined == now.combined) {
        return;
    }
    if (s_firstMismatch < 0) {
        s_firstMismatch = rec.frame;
    }
    if (++s_mismatches > kReportedMismatches) {
        return;
    }
    std::fprintf(stderr, "openstrikers: replay: frame %u differs:", rec.frame);
    for (int i = 0; i < kHashComponents; ++i) {
        if (rec.hash.component[i] != now.component[i]) {
            std::fprintf(stderr, " %s (%08x, recorded %08x)", stateHashComponentName(i), now.component[i],
                         rec.hash.component[i]);
        }
    }
    std::fprintf(stderr, "\n");
    if (s_mismatches == kReportedMismatches) {
        std::fprintf(stderr, "openstrikers: replay: further mismatches are counted, not listed\n");
    }
}

// --- match settings -------------------------------------------------------------

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// The test switches OPENSTRIKERS_MATCH_SECONDS and OPENSTRIKERS_AI_SKILL
// change the game, so they are part of the match: fold them in on the host...
void foldEnvIntoMatch(MatchSettings& m) {
    if (m.matchSeconds < 0) {
        if (const char* v = envValue("OPENSTRIKERS_MATCH_SECONDS")) m.matchSeconds = std::atoi(v);
    }
    if (m.skill < 0) {
        if (const char* v = envValue("OPENSTRIKERS_AI_SKILL")) m.skill = std::atoi(v);
    }
}

// ...and make every machine's environment say what the host's does, since the
// game reads them from there (decomp-late/125).
void applyMatchToEnv(const MatchSettings& m) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", m.matchSeconds);
    setEnv("OPENSTRIKERS_MATCH_SECONDS", m.matchSeconds > 0 ? buf : nullptr);
    std::snprintf(buf, sizeof(buf), "%d", m.skill);
    setEnv("OPENSTRIKERS_AI_SKILL", m.skill >= 0 ? buf : nullptr);
}

constexpr uint8_t kMatchConfigVersion = 2; // 2: widescreen

std::vector<uint8_t> encodeMatch(const MatchSettings& m) {
    const int fields[] = {m.homeTeam, m.awayTeam, m.homeSidekick, m.awaySidekick, m.stadium,
                          m.skill, m.matchSeconds, m.items, m.bowser, m.superStrikes, m.widescreen};
    std::vector<uint8_t> out{kMatchConfigVersion};
    for (int f : fields) {
        out.push_back(uint8_t(f & 0xFF));
        out.push_back(uint8_t((f >> 8) & 0xFF));
    }
    return out;
}

bool decodeMatch(const std::vector<uint8_t>& in, MatchSettings& m) {
    int* fields[] = {&m.homeTeam, &m.awayTeam, &m.homeSidekick, &m.awaySidekick, &m.stadium,
                     &m.skill, &m.matchSeconds, &m.items, &m.bowser, &m.superStrikes, &m.widescreen};
    constexpr size_t kCount = sizeof(fields) / sizeof(fields[0]);
    if (in.size() != 1 + 2 * kCount || in[0] != kMatchConfigVersion) return false;
    for (size_t i = 0; i < kCount; ++i) {
        *fields[i] = int16_t(in[1 + 2 * i] | (in[2 + 2 * i] << 8));
    }
    return true;
}

// --- online -------------------------------------------------------------------

// Every machine must run the same executable on the same disc: the game's
// floating point is only bit-identical between identical binaries.
uint64_t buildId() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    std::FILE* exe = nullptr;
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0) exe = _wfopen(path, L"rb");
#else
    exe = std::fopen("/proc/self/exe", "rb");
#endif
    if (exe != nullptr) {
        static unsigned char buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), exe)) > 0) mix(buf, n);
        std::fclose(exe);
    }
    if (const DVDDiskID* id = DVDGetCurrentDiskID()) {
        mix(id->gameName, sizeof(id->gameName));
        mix(id->company, sizeof(id->company));
        mix(&id->gameVersion, sizeof(id->gameVersion));
    }
    return h;
}

[[noreturn]] void endOnline(const char* title, const std::string& message, int code) {
    std::fprintf(stderr, "openstrikers: netplay: %s: %s\n", title, message.c_str());
    std::fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message.c_str(), nullptr);
    if (s_lockstep) {
        s_lockstep->leave();
        s_lockstep->poll(nowMs());
    }
    aurora_shutdown();
    std::exit(code);
}

void checkOnline() {
    switch (s_lockstep->status()) {
    case Lockstep::Status::Desynced:
        endOnline("Netplay desync", "The game went out of sync between players: " + s_lockstep->statusMessage(), 4);
    case Lockstep::Status::Disconnected:
        endOnline("Netplay ended", s_lockstep->statusMessage(), 5);
    case Lockstep::Status::Rejected:
        endOnline("Could not join", s_lockstep->statusMessage(), 6);
    default:
        break;
    }
}

// Keeps the window and the network alive while we wait on other players.
// The game window's title says what we are waiting for: nothing is drawn
// while the game is held up.
void setWindowTitle(const std::string& title) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows != nullptr && count > 0) {
        SDL_SetWindowTitle(windows[0], title.c_str());
    }
    SDL_free(windows);
}

// "OpenStrikers - online: you are home sidekick 1 (Sam, Alex, ...)"
std::string runningTitle() {
    static const char* const kRoles[] = {"captain", "sidekick 1", "sidekick 2", "sidekick 3"};
    std::string mine;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (s_lockstep->slotOwner(s) != s_lockstep->localPeer()) continue;
        mine += mine.empty() ? "" : ", ";
        mine += std::string(s < 4 ? "home " : "away ") + kRoles[s % 4];
    }
    std::string players;
    for (int p = 0; p < s_lockstep->numPeers(); ++p) {
        players += (p ? ", " : "") + s_lockstep->peerName(p);
    }
    return "OpenStrikers - online: you are " + (mine.empty() ? std::string("watching") : mine) + " (" + players + ")";
}

// Events are only peeked at: aurora's own pump must still see every key and
// button change, or one released during a stall would stay held.
void pumpWhileWaiting() {
    SDL_PumpEvents();
    if (SDL_HasEvent(SDL_EVENT_QUIT) || SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
        endOnline("Netplay ended", "you closed the game", 0);
    }
    s_lockstep->poll(nowMs());
    checkOnline();
}

void leaveOnExit() {
    if (s_lockstep && s_lockstep->running()) {
        s_lockstep->leave();
        s_lockstep->poll(nowMs());
    }
}

void startOnline(const LaunchOptions& options) {
    s_clockStart = SDL_GetTicks();
    s_transport = std::make_unique<UdpTransport>();
    LockstepConfig config;
    config.host = options.netRole == NetRole::Host;
    config.numPeers = int(options.netPlayers);
    config.inputDelay = uint8_t(options.netDelay != 0 ? options.netDelay : kDefaultDelay);
    config.seed = uint32_t(SDL_GetTicksNS() ^ (SDL_GetTicksNS() >> 29));
    config.buildId = buildId();
    // For tests/net_bot.cpp, which joins without being a game.
    config.acceptAnyBuild = envValue("OPENSTRIKERS_NET_ACCEPT_ANY_BUILD") != nullptr;
    // A lobby (--net-slots) says who drives which fielder. Without one,
    // machines alternate teams in join order, each taking the next fielder:
    // host home captain, first guest away captain, then home sidekick 1, away
    // sidekick 1, and so on.
    for (int s = 0; s < kMaxSlots; ++s) config.slotOwner[s] = -1;
    if (options.netSlotsSet) {
        for (int s = 0; s < kMaxSlots; ++s) config.slotOwner[s] = int8_t(options.netSlots[s]);
    } else {
        for (int peer = 0; peer < int(options.netPlayers); ++peer) {
            config.slotOwner[(peer % 2) * 4 + peer / 2] = int8_t(peer);
        }
    }
    config.seat = uint8_t(options.netSeat);
    if (config.host) {
        foldEnvIntoMatch(s_match);
        config.matchConfig = encodeMatch(s_match);
    }
    config.name = options.netName != nullptr ? options.netName : (config.host ? "host" : "");

    std::string error;
    if (!s_transport->open(config.host ? uint16_t(options.netPort) : 0, error) ||
        (!config.host && !UdpTransport::resolve(options.netAddress, uint16_t(kDefaultNetPort), config.hostEndpoint, error))) {
        endOnline("Netplay", error, 2);
    }
    s_lockstep = std::make_unique<Lockstep>(*s_transport, config);
    if (config.host) {
        std::fprintf(stderr, "openstrikers: netplay: hosting on UDP port %u for %u players\n", options.netPort,
                     options.netPlayers);
    } else {
        std::fprintf(stderr, "openstrikers: netplay: joining %s\n",
                     UdpTransport::describe(config.hostEndpoint).c_str());
    }

    // The game does not start until everyone is in: then every machine boots
    // from the same point and runs frame 0 together.
    uint64_t lastReport = 0;
    while (!s_lockstep->running()) {
        pumpWhileWaiting();
        if (nowMs() - lastReport >= 2000) {
            lastReport = nowMs();
            std::fprintf(stderr, "openstrikers: netplay: waiting for players%s%s\n",
                         s_lockstep->statusMessage().empty() ? "" : ": ", s_lockstep->statusMessage().c_str());
            setWindowTitle(config.host ? "OpenStrikers - hosting, waiting for players to join"
                                       : "OpenStrikers - joining " + UdpTransport::describe(config.hostEndpoint));
        }
        SDL_Delay(5);
    }
    s_seed = s_lockstep->seed();
    if (!decodeMatch(s_lockstep->matchConfig(), s_match)) {
        endOnline("Could not join", "the host is running a different version of openstrikers", 6);
    }
    applyMatchToEnv(s_match);
    // A network match always starts straight from the menus' match setup.
    setEnv("OPENSTRIKERS_SKIP_FE", "1");
    std::fprintf(stderr, "openstrikers: netplay: started as player %d of %d, input delay %u frames, seed %u\n",
                 s_lockstep->localPeer() + 1, s_lockstep->numPeers(), s_lockstep->inputDelay(), s_seed);
    setWindowTitle(runningTitle());
    std::atexit(leaveOnExit);
}

PadInput toPadInput(const PADStatus& s) {
    PadInput p;
    p.buttons = s.button;
    p.stickX = s.stickX;
    p.stickY = s.stickY;
    p.substickX = s.substickX;
    p.substickY = s.substickY;
    p.triggerLeft = s.triggerLeft;
    p.triggerRight = s.triggerRight;
    return p;
}

// Every machine builds the game's pads from the agreed frame alone, its own
// slots included, so all of them see exactly the same PADStatus.
void fromFrameInput(const FrameInput& in, PADStatus* status) {
    std::memset(status, 0, sizeof(PADStatus) * kMaxSlots);
    for (int port = 0; port < kMaxSlots; ++port) {
        if (!s_lockstep->slotOccupied(port)) {
            status[port].err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        const PadInput& p = in.pad[port];
        status[port].button = p.buttons;
        status[port].stickX = p.stickX;
        status[port].stickY = p.stickY;
        status[port].substickX = p.substickX;
        status[port].substickY = p.substickY;
        status[port].triggerLeft = p.triggerLeft;
        status[port].triggerRight = p.triggerRight;
        status[port].err = PAD_ERR_NONE;
    }
}

void onlineFrame(PADStatus* status) {
    Lockstep& ls = *s_lockstep;
    const uint32_t frame = s_frame;
    ls.reportHash(frame, computeStateHash().combined);

    // This machine's controllers, played `delay` frames from now. Local port
    // k drives this machine's k-th slot.
    PADStatus local[PAD_MAX_CONTROLLERS];
    PADRead(local);
    PadInput mine[kMaxSlots];
    int k = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (ls.slotOwner(s) == ls.localPeer()) {
            mine[k] = k < PAD_MAX_CONTROLLERS && local[k].err == PAD_ERR_NONE ? toPadInput(local[k]) : PadInput{};
            ++k;
        }
    }
    ls.submitLocal(frame + ls.inputDelay(), mine);
    ls.poll(nowMs());
    checkOnline();

    if (!ls.frameReady(frame)) {
        const uint64_t since = nowMs();
        bool reported = false;
        while (!ls.frameReady(frame)) {
            pumpWhileWaiting();
            if (!reported && nowMs() - since > 1000) {
                reported = true;
                std::fprintf(stderr, "openstrikers: netplay: waiting for other players at frame %u\n", frame);
                setWindowTitle("OpenStrikers - waiting for the other players...");
            }
            SDL_Delay(1);
        }
        if (reported) setWindowTitle(runningTitle());
    }
    fromFrameInput(ls.frameInput(frame), status);
}

} // namespace

bool sessionDeterministic() { return s_mode != Mode::Off; }

int sessionWidescreen() { return s_mode == Mode::Off ? -1 : (s_match.widescreen > 0 ? 1 : 0); }

void sessionStart(const LaunchOptions& options) {
    if (const char* v = envValue("OPENSTRIKERS_NET_JITTER_MS")) {
        s_jitterMs = (uint32_t)std::strtoul(v, nullptr, 0);
    }
    if (options.playerNames != nullptr) {
        int port = 0;
        for (const char* p = options.playerNames; *p != '\0' && port < PAD_MAX_CONTROLLERS; ++p) {
            if (*p == ',') {
                ++port;
            } else {
                s_localNames[port] += *p;
            }
        }
    }
    s_match = options.match;
    if (s_match.widescreen < 0) {
        // The host's (or recorder's) picture choice decides the game's 16:9
        // mode for everyone; with no save loaded the game's own default is 4:3.
        s_match.widescreen = options.aspect == AspectMode::Widescreen ? 1 : 0;
    }
    if (s_match.matchSeconds > 0 || s_match.skill >= 0) {
        // The command line wins over the environment switches.
        MatchSettings folded = s_match;
        foldEnvIntoMatch(folded);
        applyMatchToEnv(folded);
    }

    if (options.netRole != NetRole::None) {
        startOnline(options);
        s_mode = Mode::Online;
    } else if (const char* path = envValue("OPENSTRIKERS_NET_REPLAY")) {
        s_file = std::fopen(path, "rb");
        FileHeader header{};
        if (s_file == nullptr || std::fread(&header, sizeof(header), 1, s_file) != 1 ||
            std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion) {
            std::fprintf(stderr, "openstrikers: replay: %s is not a recording\n", path);
            std::exit(2);
        }
        if (header.frameRecordSize != sizeof(FrameRecord)) {
            std::fprintf(stderr, "openstrikers: replay: %s was recorded by a different build\n", path);
            std::exit(2);
        }
        s_mode = Mode::Replay;
        s_seed = header.seed;
        // The match the recording played, whatever this command line says.
        if (header.matchSize > sizeof(header.match) ||
            !decodeMatch(std::vector<uint8_t>(header.match, header.match + header.matchSize), s_match)) {
            std::fprintf(stderr, "openstrikers: replay: %s was recorded by a different build\n", path);
            std::exit(2);
        }
        applyMatchToEnv(s_match);
        std::fprintf(stderr, "openstrikers: replaying %s (seed %u)\n", path, s_seed);
    } else if (const char* path = envValue("OPENSTRIKERS_NET_RECORD")) {
        s_file = std::fopen(path, "wb");
        if (s_file == nullptr) {
            std::fprintf(stderr, "openstrikers: record: cannot write %s\n", path);
            std::exit(2);
        }
        s_mode = Mode::Record;
        s_seed = (uint32_t)SDL_GetTicksNS() ^ (uint32_t)(SDL_GetTicksNS() >> 32);
        FileHeader header{};
        std::memcpy(header.magic, kMagic, sizeof(kMagic));
        header.version = kVersion;
        header.seed = s_seed;
        header.frameRecordSize = sizeof(FrameRecord);
        foldEnvIntoMatch(s_match);
        const std::vector<uint8_t> match = encodeMatch(s_match);
        header.matchSize = uint32_t(match.size());
        std::memcpy(header.match, match.data(), match.size());
        std::fwrite(&header, sizeof(header), 1, s_file);
        std::atexit(flushRecording);
        std::fprintf(stderr, "openstrikers: recording input to %s (seed %u)\n", path, s_seed);
    }

    if (s_mode != Mode::Off) {
        aurora_dvd_set_deferred(true);
    }
}

} // namespace net

float osHostFixedStepMs() {
    if (net::sessionDeterministic()) {
        return net::kFrameMs;
    }
    const char* v = std::getenv("OPENSTRIKERS_FIXED_STEP");
    return v != nullptr ? (float)std::atof(v) : 0.0f;
}

unsigned int osHostRandomSeed(unsigned int fallback) {
    return net::sessionDeterministic() ? net::s_seed : fallback;
}

void osHostServiceDisc() {
    if (net::sessionDeterministic()) {
        aurora_dvd_service_deferred();
    }
}

bool osHostReadPads(PADStatus* status, bool frameSample) {
    using namespace net;
    if (s_mode == Mode::Off) {
        return false;
    }
    if (!frameSample) {
        // The read at pad initialisation happens before any frame; give every
        // run the same answer rather than whatever is plugged in. A recording
        // or replay sees a single-player boot: port 0 present and idle, the
        // others empty. (Reporting all four present makes the skip-FE match
        // put an idle "human" on every port, two per team, and nobody ever
        // scores.) Online, the ports with a player behind them are present.
        std::memset(status, 0, sizeof(PADStatus) * kMaxSlots);
        for (int i = 0; i < kMaxSlots; ++i) {
            const bool present = s_mode == Mode::Online ? s_lockstep->slotOccupied(i) : i == 0;
            status[i].err = present ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
        }
        return true;
    }

    if (s_jitterMs != 0) {
        SDL_Delay((Uint32)(std::rand() % (int)(s_jitterMs + 1)));
    }

    if (s_mode == Mode::Online) {
        onlineFrame(status);
        if (s_frame % 600 == 0) {
            std::fprintf(stderr, "openstrikers: net frame %u state %08x\n", s_frame, computeStateHash().combined);
        }
        ++s_frame;
        return true;
    }

    FrameRecord rec{};
    const StateHash now = computeStateHash();
    if (s_mode == Mode::Record) {
        PADRead(status); // the four GameCube ports
        for (int i = PAD_MAX_CONTROLLERS; i < kMaxSlots; ++i) {
            std::memset(&status[i], 0, sizeof(PADStatus));
            status[i].err = PAD_ERR_NO_CONTROLLER;
        }
        rec.frame = s_frame;
        rec.hash = now;
        std::memcpy(rec.pads, status, sizeof(rec.pads));
        std::fwrite(&rec, sizeof(rec), 1, s_file);
        if (s_frame % 600 == 0) {
            flushRecording();
        }
    } else {
        if (std::fread(&rec, sizeof(rec), 1, s_file) != 1) {
            finishReplay();
        }
        if (rec.frame != s_frame) {
            std::fprintf(stderr, "openstrikers: replay: recording is corrupt at frame %u\n", s_frame);
            std::exit(2);
        }
        compareHash(rec, now);
        std::memcpy(status, rec.pads, sizeof(rec.pads));
    }
    if (s_frame % 600 == 0) {
        std::fprintf(stderr, "openstrikers: net frame %u state %08x\n", s_frame, now.combined);
    }
    ++s_frame;
    return true;
}

int osHostPadSide(int pad) {
    using namespace net;
    if (s_mode != Mode::Online) {
        return pad < PAD_MAX_CONTROLLERS ? OS_HOST_PAD_SIDE_GAME : -1;
    }
    if (pad < 0 || pad >= kMaxSlots || !s_lockstep->slotOccupied(pad)) {
        return -1;
    }
    return pad / 4;
}

int osHostPadFielder(int pad) {
    using namespace net;
    if (s_mode != Mode::Online || pad < 0 || pad >= kMaxSlots || !s_lockstep->slotOccupied(pad)) {
        return -1;
    }
    return pad % 4;
}

bool osHostTeamLocked(int side) {
    using namespace net;
    if (s_mode != Mode::Online || side < 0 || side > 1) {
        return false;
    }
    for (int s = side * 4; s < side * 4 + 4; ++s) {
        if (!s_lockstep->slotOccupied(s)) {
            return false;
        }
    }
    return true;
}

void osHostControlMotor(int pad, unsigned int command) {
    using namespace net;
    if (s_mode != Mode::Online) {
        if (pad >= 0 && pad < PAD_MAX_CONTROLLERS) {
            PADControlMotor(pad, command);
        }
        return;
    }
    // Only this machine's own slots rumble, on the controller that drives them:
    // its k-th slot is its local port k.
    if (pad < 0 || pad >= kMaxSlots || s_lockstep->slotOwner(pad) != s_lockstep->localPeer()) {
        return;
    }
    int port = 0;
    for (int s = 0; s < pad; ++s) {
        port += s_lockstep->slotOwner(s) == s_lockstep->localPeer();
    }
    if (port < PAD_MAX_CONTROLLERS) {
        PADControlMotor(port, command);
    }
}

const char* osHostPadName(int pad) {
    using namespace net;
    static std::string s_names[kMaxSlots];
    if (pad < 0 || pad >= kMaxSlots) {
        return nullptr;
    }
    std::string& name = s_names[pad];
    if (s_mode == Mode::Online) {
        if (!s_lockstep->slotOccupied(pad)) {
            return nullptr;
        }
        const int owner = s_lockstep->slotOwner(pad);
        int index = 0, count = 0;
        for (int s = 0; s < kMaxSlots; ++s) {
            if (s_lockstep->slotOccupied(s) && s_lockstep->slotOwner(s) == owner) {
                index += s < pad;
                ++count;
            }
        }
        name = s_lockstep->peerName(owner);
        if (count > 1) {
            name += " " + std::to_string(index + 1);
        }
    } else {
        name = pad < PAD_MAX_CONTROLLERS ? s_localNames[pad] : std::string();
    }
    return name.empty() ? nullptr : name.c_str();
}

bool osHostMatch(OsHostMatch* out) {
    const MatchSettings& m = net::s_match;
    out->homeTeam = m.homeTeam;
    out->awayTeam = m.awayTeam;
    out->homeSidekick = m.homeSidekick;
    out->awaySidekick = m.awaySidekick;
    out->stadium = m.stadium;
    out->items = m.items;
    out->bowser = m.bowser;
    out->superStrikes = m.superStrikes;
    return true;
}
