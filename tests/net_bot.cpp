// A stand-in network player: joins a session and plays random input at 60
// frames a second, without running the game. With one real game hosting
// (--net-host) and this joining, the game's whole netplay path -- handshake,
// input delay, assembled frames, stalls, leaving -- runs on one machine without
// the cost of a second game. It sends no state hashes, having no game state.
//
//   openstrikers-net-bot HOST[:PORT] [--frames N] [--seed S] [--latency MS]
//                        [--loss PERCENT] [--freeze-at FRAME]
//
// --latency and --loss delay and drop this bot's own packets (both ways), to
// see the game cope with a poor connection. --freeze-at stops it answering, as
// a crashed or unplugged player would.

#include "net/lockstep.h"
#include "net/udp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>

using namespace net;

namespace {

uint64_t g_rng = 1;
uint32_t rnd(uint32_t range) {
    g_rng += 0x9E3779B97F4A7C15ull;
    uint64_t z = g_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return uint32_t(z % range);
}

uint64_t nowMs() {
    static const auto start = std::chrono::steady_clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count()) + 1;
}

// Wraps the socket to add latency and loss in both directions.
class BadLink : public Transport {
public:
    BadLink(UdpTransport& udp, uint32_t latencyMs, uint32_t lossPercent)
        : m_udp(udp), m_latency(latencyMs), m_loss(lossPercent) {}
    void send(const Endpoint& to, const uint8_t* data, size_t size) override {
        if (rnd(100) < m_loss) return;
        m_out.push_back({nowMs() + m_latency / 2, to, {data, data + size}});
        flush();
    }
    bool receive(Endpoint& from, std::vector<uint8_t>& data) override {
        flush();
        Endpoint e;
        std::vector<uint8_t> d;
        while (m_udp.receive(e, d)) {
            if (rnd(100) < m_loss) continue;
            m_in.push_back({nowMs() + m_latency / 2, e, d});
        }
        if (!m_in.empty() && m_in.front().at <= nowMs()) {
            from = m_in.front().who;
            data = std::move(m_in.front().data);
            m_in.pop_front();
            return true;
        }
        return false;
    }

private:
    struct Delayed {
        uint64_t at;
        Endpoint who;
        std::vector<uint8_t> data;
    };
    void flush() {
        while (!m_out.empty() && m_out.front().at <= nowMs()) {
            m_udp.send(m_out.front().who, m_out.front().data.data(), m_out.front().data.size());
            m_out.pop_front();
        }
    }
    UdpTransport& m_udp;
    uint32_t m_latency, m_loss;
    std::deque<Delayed> m_out, m_in;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s HOST[:PORT] [--frames N] [--seed S] [--latency MS] [--loss PERCENT] "
                             "[--freeze-at FRAME]\n", argv[0]);
        return 2;
    }
    uint32_t frames = 36000, latency = 0, loss = 0, freezeAt = UINT32_MAX;
    for (int i = 2; i + 1 < argc; i += 2) {
        const uint32_t v = uint32_t(std::strtoul(argv[i + 1], nullptr, 0));
        if (!std::strcmp(argv[i], "--frames")) frames = v;
        else if (!std::strcmp(argv[i], "--seed")) g_rng = v;
        else if (!std::strcmp(argv[i], "--latency")) latency = v;
        else if (!std::strcmp(argv[i], "--loss")) loss = v;
        else if (!std::strcmp(argv[i], "--freeze-at")) freezeAt = v;
    }

    UdpTransport udp;
    std::string error;
    LockstepConfig config;
    config.name = "bot";
    if (!udp.open(0, error) || !UdpTransport::resolve(argv[1], 52320, config.hostEndpoint, error)) {
        std::fprintf(stderr, "net-bot: %s\n", error.c_str());
        return 2;
    }
    // The bot has no executable or disc to vouch for; ask the host to accept
    // any build (the game hosts with OPENSTRIKERS_NET_ACCEPT_ANY_BUILD=1).
    config.buildId = 0;
    BadLink link(udp, latency, loss);
    Lockstep ls(link, config);

    std::printf("net-bot: joining %s\n", UdpTransport::describe(config.hostEndpoint).c_str());
    uint32_t frame = 0;
    uint64_t nextFrameAt = 0;
    uint64_t waitedMs = 0;
    PadInput held;
    uint32_t holdUntil = 0;
    while (frame < frames) {
        const uint64_t now = nowMs();
        ls.poll(now);
        if (ls.status() != Lockstep::Status::Connecting && ls.status() != Lockstep::Status::Running) {
            std::printf("net-bot: session ended at frame %u: %s\n", frame, ls.statusMessage().c_str());
            return ls.status() == Lockstep::Status::Disconnected ? 0 : 1;
        }
        if (frame >= freezeAt) {
            std::printf("net-bot: freezing at frame %u\n", frame);
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
        if (ls.running() && now >= nextFrameAt) {
            if (ls.frameReady(frame)) {
                if (frame >= holdUntil) {
                    held = PadInput{};
                    const float a = float(rnd(628)) / 100.0f;
                    if (rnd(100) < 85) {
                        held.stickX = int8_t(72 * std::cos(a));
                        held.stickY = int8_t(72 * std::sin(a));
                    }
                    const uint32_t roll = rnd(100);
                    if (roll < 30) held.buttons = 0x0100;      // A: pass, and every prompt
                    else if (roll < 50) held.buttons = 0x0200; // B: shoot
                    holdUntil = frame + 6 + rnd(40);
                }
                ls.submitLocal(frame + ls.inputDelay(), &held);
                ++frame;
                nextFrameAt = (nextFrameAt == 0 ? now : nextFrameAt) + 16;
                if (frame % 600 == 0) {
                    std::printf("net-bot: frame %u (stalled %llu ms so far)\n", frame, (unsigned long long)waitedMs);
                    std::fflush(stdout);
                }
            } else {
                ++waitedMs;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ls.leave();
    ls.poll(nowMs());
    std::printf("net-bot: played %u frames, stalled %llu ms\n", frame, (unsigned long long)waitedMs);
    return 0;
}
