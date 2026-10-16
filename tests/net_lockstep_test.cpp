// Lockstep over a simulated network: latency, jitter, loss, duplication and
// reordering, machines running at different speeds. Every machine must run
// exactly the same input for every frame, and each player's input must land in
// its own slot, delayed by the agreed number of frames.

#include "net/lockstep.h"
#include "net/udp.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <vector>

using namespace net;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

struct Rng {
    uint64_t s;
    uint32_t next(uint32_t range) {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return range ? uint32_t(z % range) : 0;
    }
};

struct NetConditions {
    uint32_t latencyMs = 20;
    uint32_t jitterMs = 10;
    uint32_t lossPercent = 10;
    uint32_t duplicatePercent = 5;
};

Endpoint endpointFor(int id) {
    Endpoint e;
    e.data[0] = uint8_t(id);
    e.size = 1;
    return e;
}

struct Packet {
    uint64_t deliverAt;
    Endpoint from;
    std::vector<uint8_t> data;
};

class SimNetwork {
public:
    SimNetwork(NetConditions c, uint64_t seed) : m_c(c), m_rng{seed} {}
    void post(int from, const Endpoint& to, const uint8_t* data, size_t size, uint64_t now) {
        if (m_rng.next(100) < m_c.lossPercent) return;
        const int copies = m_rng.next(100) < m_c.duplicatePercent ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            Packet p{now + m_c.latencyMs + m_rng.next(m_c.jitterMs + 1), endpointFor(from), {data, data + size}};
            m_inbox[to.data[0]].push_back(std::move(p)); // jitter reorders them
        }
    }
    bool take(int id, uint64_t now, Endpoint& from, std::vector<uint8_t>& data) {
        auto& box = m_inbox[id];
        for (auto it = box.begin(); it != box.end(); ++it) {
            if (it->deliverAt <= now) {
                from = it->from;
                data = std::move(it->data);
                box.erase(it);
                return true;
            }
        }
        return false;
    }
    uint64_t now = 0;

private:
    NetConditions m_c;
    Rng m_rng;
    std::map<int, std::deque<Packet>> m_inbox;
};

class SimTransport : public Transport {
public:
    SimTransport(SimNetwork& net, int id) : m_net(net), m_id(id) {}
    void send(const Endpoint& to, const uint8_t* data, size_t size) override {
        if (!muted) m_net.post(m_id, to, data, size, m_net.now);
    }
    bool receive(Endpoint& from, std::vector<uint8_t>& data) override {
        return !muted && m_net.take(m_id, m_net.now, from, data);
    }
    bool muted = false;

private:
    SimNetwork& m_net;
    int m_id;
};

PadInput inputFor(int peer, uint32_t frame) {
    PadInput p;
    p.buttons = uint16_t(frame * 7 + peer * 1000);
    p.stickX = int8_t(peer * 10 + 1);
    p.stickY = int8_t(frame & 0x3F);
    p.triggerRight = uint8_t(peer);
    return p;
}

struct Machine {
    std::unique_ptr<SimTransport> transport;
    std::unique_ptr<Lockstep> lockstep;
    uint32_t frame = 0;          // next frame to run
    uint32_t frameMs = 16;       // how long this machine takes per frame
    uint64_t nextFrameAt = 0;
    uint32_t hash = 0x12345678;  // stands in for the game state
    std::vector<FrameInput> ran; // what it ran, frame by frame
    int corruptAtFrame = -1;     // pretend to desync here
    bool frozen = false;         // stops polling: a crashed or unplugged machine
};

struct Run {
    std::vector<Machine> machines;
    std::unique_ptr<SimNetwork> net;
};

Run makeRun(int peers, NetConditions c, uint64_t seed, uint8_t delay, uint64_t clientBuild = 42) {
    Run run;
    run.net = std::make_unique<SimNetwork>(c, seed);
    run.machines.resize(peers);
    for (int i = 0; i < peers; ++i) {
        Machine& m = run.machines[i];
        m.transport = std::make_unique<SimTransport>(*run.net, i);
        LockstepConfig cfg;
        cfg.host = i == 0;
        cfg.numPeers = peers;
        cfg.inputDelay = delay;
        cfg.seed = 777;
        cfg.hostEndpoint = endpointFor(0);
        cfg.buildId = i == 0 ? 42 : clientBuild;
        cfg.name = "m" + std::to_string(i);
        cfg.timeoutMs = 3000;
        m.lockstep = std::make_unique<Lockstep>(*m.transport, cfg);
        m.frameMs = 16 + (i % 3); // slightly different speeds
        // Clients start up at different times.
        m.nextFrameAt = uint64_t(i) * 137;
    }
    return run;
}

// Advance the virtual clock 1 ms at a time until every machine has run
// `frames` frames or `limitMs` passes.
void simulate(Run& run, uint32_t frames, uint64_t limitMs) {
    for (uint64_t t = 0; t < limitMs; ++t) {
        run.net->now = t;
        bool allDone = true;
        for (Machine& m : run.machines) {
            if (m.frozen) {
                m.transport->muted = true;
                continue;
            }
            m.lockstep->poll(t);
            Lockstep& ls = *m.lockstep;
            if (ls.running() && t >= m.nextFrameAt && m.frame < frames && ls.frameReady(m.frame)) {
                const FrameInput in = ls.frameInput(m.frame);
                m.ran.push_back(in);
                for (int s = 0; s < kMaxSlots; ++s) {
                    m.hash = m.hash * 31 + in.pad[s].buttons + uint8_t(in.pad[s].stickX) * 7;
                }
                uint32_t reported = m.hash;
                if (m.corruptAtFrame >= 0 && m.frame >= uint32_t(m.corruptAtFrame)) reported ^= 1;
                ls.reportHash(m.frame, reported);
                // Input sampled now plays `delay` frames later.
                PadInput local[kMaxSlots];
                int k = 0;
                for (int s = 0; s < kMaxSlots; ++s) {
                    if (ls.slotOwner(s) == ls.localPeer()) local[k++] = inputFor(ls.localPeer(), m.frame + ls.inputDelay());
                }
                ls.submitLocal(m.frame + ls.inputDelay(), local);
                ++m.frame;
                m.nextFrameAt = t + m.frameMs;
            }
            if (m.frame < frames) allDone = false;
        }
        if (allDone) return;
    }
}

void testIdenticalFrames(int peers, NetConditions c, uint64_t seed, uint8_t delay) {
    Run run = makeRun(peers, c, seed, delay);
    const uint32_t kFrames = 3000;
    simulate(run, kFrames, 600000);
    for (Machine& m : run.machines) {
        CHECK(m.lockstep->running());
        CHECK(m.frame == kFrames);
        CHECK(m.lockstep->numPeers() == peers);
        CHECK(m.lockstep->inputDelay() == delay);
        CHECK(m.lockstep->seed() == 777);
    }
    const Machine& host = run.machines[0];
    for (size_t i = 1; i < run.machines.size(); ++i) {
        const Machine& m = run.machines[i];
        CHECK(m.ran.size() == host.ran.size());
        for (size_t f = 0; f < m.ran.size() && f < host.ran.size(); ++f) {
            for (int s = 0; s < kMaxSlots; ++s) {
                if (!(m.ran[f].pad[s] == host.ran[f].pad[s])) {
                    std::fprintf(stderr, "machine %zu frame %zu slot %d differs from host\n", i, f, s);
                    ++g_failures;
                    return;
                }
            }
        }
    }
    // Slot k carries machine k's input from frame `delay` on; empty slots are neutral.
    for (uint32_t f = 0; f < host.ran.size(); ++f) {
        for (int s = 0; s < kMaxSlots; ++s) {
            const PadInput expect = (s < peers && f >= delay) ? inputFor(s, f) : PadInput{};
            if (!(host.ran[f].pad[s] == expect)) {
                std::fprintf(stderr, "frame %u slot %d has the wrong input\n", f, s);
                ++g_failures;
                return;
            }
        }
    }
}

void testDesyncDetected() {
    Run run = makeRun(3, NetConditions{}, 5, 3);
    run.machines[2].corruptAtFrame = 600;
    simulate(run, 1500, 120000);
    // Every machine stops, told by whoever noticed, soon after the bad frame.
    for (Machine& m : run.machines) {
        CHECK(m.lockstep->status() == Lockstep::Status::Desynced);
        CHECK(m.frame < 600 + 3 * kHashInterval + 10);
    }
}

void testDisconnect() {
    Run run = makeRun(2, NetConditions{}, 9, 2);
    simulate(run, 300, 60000);
    CHECK(run.machines[1].frame == 300);
    run.machines[1].frozen = true;
    simulate(run, 2000, 20000);
    CHECK(run.machines[0].lockstep->status() == Lockstep::Status::Disconnected);
    // It stalled at the first frame missing the other machine's input.
    CHECK(run.machines[0].frame < 300 + 10);
}

void testBuildMismatchRejected() {
    Run run = makeRun(2, NetConditions{}, 11, 2, /*clientBuild=*/43);
    simulate(run, 10, 5000);
    CHECK(run.machines[1].lockstep->status() == Lockstep::Status::Rejected);
    CHECK(run.machines[0].lockstep->status() == Lockstep::Status::Waiting);
}

// A lobby seats the machines and says which fielders each drives; the order
// they connect in must not matter.
void testLobbySeats() {
    Run run = makeRun(3, NetConditions{20, 10, 5, 5}, 15, 3);
    LockstepConfig hc;
    hc.host = true;
    hc.numPeers = 3;
    hc.inputDelay = 3;
    hc.seed = 777;
    hc.buildId = 42;
    hc.name = "host";
    for (int s = 0; s < kMaxSlots; ++s) hc.slotOwner[s] = -1;
    hc.slotOwner[0] = 0; // host: home captain
    hc.slotOwner[1] = 1; // seat 1: home sidekick
    hc.slotOwner[4] = 2; // seat 2: away captain
    hc.slotOwner[5] = 2; //         and an away sidekick
    run.machines[0].lockstep = std::make_unique<Lockstep>(*run.machines[0].transport, hc);
    for (int i = 1; i < 3; ++i) {
        LockstepConfig cc;
        cc.hostEndpoint = endpointFor(0);
        cc.buildId = 42;
        cc.name = i == 1 ? "second" : "first";
        cc.seat = uint8_t(3 - i); // machine 1 takes seat 2, machine 2 seat 1
        run.machines[i].lockstep = std::make_unique<Lockstep>(*run.machines[i].transport, cc);
    }
    simulate(run, 600, 60000);
    for (Machine& m : run.machines) CHECK(m.frame == 600);
    CHECK(run.machines[1].lockstep->localPeer() == 2);
    CHECK(run.machines[2].lockstep->localPeer() == 1);
    CHECK(run.machines[1].lockstep->peerName(1) == "first");
    CHECK(run.machines[0].lockstep->slotOwner(5) == 2);
    CHECK(!run.machines[0].lockstep->slotOccupied(2));
    // Slot 5 is seat 2's second controller.
    const FrameInput& late = run.machines[0].ran[500];
    CHECK(late.pad[4] == inputFor(2, 500) && late.pad[5] == inputFor(2, 500));
    CHECK(late.pad[1] == inputFor(1, 500));
}

void testSeatTaken() {
    Run run = makeRun(3, NetConditions{0, 0, 0, 0}, 17, 2);
    for (int i = 1; i < 3; ++i) {
        LockstepConfig cc;
        cc.hostEndpoint = endpointFor(0);
        cc.buildId = 42;
        cc.seat = 1;
        run.machines[i].lockstep = std::make_unique<Lockstep>(*run.machines[i].transport, cc);
    }
    simulate(run, 10, 3000);
    const int rejected = (run.machines[1].lockstep->status() == Lockstep::Status::Rejected) +
                         (run.machines[2].lockstep->status() == Lockstep::Status::Rejected);
    CHECK(rejected == 1);
}

void testLeave() {
    Run run = makeRun(2, NetConditions{0, 0, 0, 0}, 13, 2);
    simulate(run, 100, 20000);
    run.machines[1].lockstep->leave();
    simulate(run, 200, 1000);
    CHECK(run.machines[0].lockstep->status() == Lockstep::Status::Disconnected);
}

// The same over real UDP sockets on 127.0.0.1, with the wall clock.
void testUdpLoopback() {
    UdpTransport hostSocket, clientSocket;
    std::string error;
    const uint16_t port = 52399;
    if (!hostSocket.open(port, error) || !clientSocket.open(0, error)) {
        std::fprintf(stderr, "UDP loopback skipped: %s\n", error.c_str());
        return;
    }
    LockstepConfig hc;
    hc.host = true;
    hc.numPeers = 2;
    hc.inputDelay = 2;
    hc.seed = 99;
    hc.buildId = 7;
    LockstepConfig cc;
    cc.buildId = 7;
    CHECK(UdpTransport::resolve("127.0.0.1:" + std::to_string(port), 0, cc.hostEndpoint, error));
    Lockstep host(hostSocket, hc), client(clientSocket, cc);
    Lockstep* ls[2] = {&host, &client};
    uint32_t frame[2] = {0, 0};
    std::vector<FrameInput> ran[2];
    const auto start = std::chrono::steady_clock::now();
    while (frame[0] < 300 || frame[1] < 300) {
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count()) + 1;
        if (now > 10000) break;
        for (int i = 0; i < 2; ++i) {
            ls[i]->poll(now);
            if (frame[i] < 300 && ls[i]->frameReady(frame[i])) {
                ran[i].push_back(ls[i]->frameInput(frame[i]));
                const PadInput local = inputFor(i, frame[i] + 2);
                ls[i]->submitLocal(frame[i] + 2, &local);
                ++frame[i];
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(frame[0] == 300 && frame[1] == 300);
    CHECK(client.seed() == 99);
    for (size_t f = 0; f < ran[0].size() && f < ran[1].size(); ++f) {
        for (int s = 0; s < kMaxSlots; ++s) CHECK(ran[0][f].pad[s] == ran[1][f].pad[s]);
    }
}

} // namespace

int main() {
    testUdpLoopback();
    testIdenticalFrames(2, NetConditions{0, 0, 0, 0}, 1, 2);
    testIdenticalFrames(2, NetConditions{}, 2, 3);
    testIdenticalFrames(4, NetConditions{40, 30, 20, 10}, 3, 5);
    testIdenticalFrames(8, NetConditions{60, 40, 25, 10}, 4, 6);
    testDesyncDetected();
    testDisconnect();
    testBuildMismatchRejected();
    testLobbySeats();
    testSeatTaken();
    testLeave();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("net lockstep: all tests passed\n");
    return 0;
}
