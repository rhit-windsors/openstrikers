#include "net/lockstep.h"

#include <algorithm>
#include <cstdio>

namespace net {
namespace {

constexpr uint32_t kMagic = 0x4E4B534F; // "OSKN"
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxPacket = 1200;     // stays under any real path MTU
constexpr uint32_t kNoHash = UINT32_MAX;

enum MessageType : uint8_t {
    kHello = 1,   // client -> host: build id, name
    kWelcome = 2, // host -> client: admitted, here is your machine number
    kReject = 3,  // host -> client: reason
    kStart = 4,   // host -> client: seed, delay, slot owners; the game may start
    kInput = 5,   // client -> host: our unacknowledged input, plus a hash
    kFrames = 6,  // host -> client: unacknowledged assembled frames, plus a hash
    kBye = 7,     // either way: leaving
    kDesync = 8,  // either way: the game states differ; the host relays it

};

class Writer {
public:
    explicit Writer(uint8_t type) {
        u32(kMagic);
        u8(kVersion);
        u8(type);
    }
    void u8(uint8_t v) { m_buf.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) m_buf.push_back(uint8_t(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        u32(uint32_t(v));
        u32(uint32_t(v >> 32));
    }
    void str(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 32);
        u8(uint8_t(n));
        m_buf.insert(m_buf.end(), s.begin(), s.begin() + n);
    }
    void pad(const PadInput& p) {
        u8(uint8_t(p.buttons));
        u8(uint8_t(p.buttons >> 8));
        u8(uint8_t(p.stickX));
        u8(uint8_t(p.stickY));
        u8(uint8_t(p.substickX));
        u8(uint8_t(p.substickY));
        u8(p.triggerLeft);
        u8(p.triggerRight);
    }
    size_t size() const { return m_buf.size(); }
    const std::vector<uint8_t>& bytes() const { return m_buf; }

private:
    std::vector<uint8_t> m_buf;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : m_p(data), m_end(data + size) {}
    bool ok() const { return m_ok; }
    uint8_t u8() {
        if (m_p >= m_end) {
            m_ok = false;
            return 0;
        }
        return *m_p++;
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(u8()) << (8 * i);
        return v;
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | (uint64_t(u32()) << 32);
    }
    std::string str() {
        const uint8_t n = u8();
        if (size_t(m_end - m_p) < n) {
            m_ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_p), n);
        m_p += n;
        return s;
    }
    PadInput pad() {
        PadInput p;
        p.buttons = u8();
        p.buttons |= uint16_t(u8()) << 8;
        p.stickX = int8_t(u8());
        p.stickY = int8_t(u8());
        p.substickX = int8_t(u8());
        p.substickY = int8_t(u8());
        p.triggerLeft = u8();
        p.triggerRight = u8();
        return p;
    }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_ok = true;
};

} // namespace

Lockstep::Lockstep(Transport& transport, const LockstepConfig& config)
    : m_transport(transport), m_config(config), m_status(config.host ? Status::Waiting : Status::Connecting) {
    std::memcpy(m_slotOwner, config.slotOwner, sizeof(m_slotOwner));
    for (auto& f : m_ourHashFrame) f = kNoHash;
    if (config.host) {
        m_localPeer = 0;
        m_numPeers = std::clamp(config.numPeers, 1, kMaxPeers);
        m_seed = config.seed;
        m_delay = std::max<uint8_t>(config.inputDelay, 1);
        for (int s = 0; s < kMaxSlots; ++s) {
            if (m_slotOwner[s] >= m_numPeers) m_slotOwner[s] = -1;
        }
        m_peers[0].joined = true;
        m_peers[0].name = config.name;
        m_matchConfig = config.matchConfig;
        if (m_matchConfig.size() > kMaxMatchConfig) m_matchConfig.resize(kMaxMatchConfig);
    }
}

int Lockstep::findPeer(const Endpoint& from) const {
    for (int i = 1; i < kMaxPeers; ++i) {
        if (m_peers[i].joined && m_peers[i].endpoint == from) return i;
    }
    return -1;
}

void Lockstep::fail(Status status, const std::string& message) {
    if (m_status == Status::Desynced || m_status == Status::Disconnected || m_status == Status::Rejected) return;
    m_status = status;
    m_message = message;
    if (status == Status::Desynced) {
        // Everyone has to stop, not only whoever noticed: tell the others,
        // repeatedly for a while since any one packet may be lost.
        m_desyncNoticeUntil = m_now + 1000;
        m_lastSendMs = 0;
    }
}

void Lockstep::sendDesync() {
    Writer w(kDesync);
    w.str(m_message);
    if (m_config.host) {
        for (int i = 1; i < m_numPeers; ++i) {
            if (m_peers[i].joined) m_transport.send(m_peers[i].endpoint, w.bytes().data(), w.size());
        }
    } else {
        m_transport.send(m_config.hostEndpoint, w.bytes().data(), w.size());
    }
}

void Lockstep::setInput(uint32_t frame, int slot, const PadInput& in) {
    FrameInput& f = m_inputs[frame % kRing];
    uint8_t& known = m_known[frame % kRing];
    if (known & (1u << slot)) return; // first one wins: input never changes once sent
    f.pad[slot] = in;
    known |= uint8_t(1u << slot);
}

// Frames [0, delay) are neutral on every machine; nobody sends them.
static void neutralFrames(FrameInput* inputs, uint8_t* known, uint32_t ring, uint32_t delay) {
    for (uint32_t f = 0; f < delay; ++f) {
        inputs[f % ring] = FrameInput{};
        known[f % ring] = 0xFF;
    }
}

void Lockstep::submitLocal(uint32_t frame, const PadInput* pads) {
    if (!running() || frame < m_localNext || frame < m_delay) return;
    // Frames must arrive in order; fill any gap with the same input.
    for (uint32_t f = std::max(m_localNext, uint32_t(m_delay)); f <= frame; ++f) {
        // Never write so far ahead that the ring would overwrite an unused frame.
        if (f >= m_assembled + kRing / 2) return;
        int k = 0;
        for (int s = 0; s < kMaxSlots; ++s) {
            if (ownsSlot(s)) setInput(f, s, pads[k++]);
        }
        m_localNext = f + 1;
    }
    m_dirty = true;
    if (m_config.host) assembleFrames();
}

void Lockstep::assembleFrames() {
    uint8_t occupied = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (slotOccupied(s)) occupied |= uint8_t(1u << s);
    }
    while (true) {
        const uint8_t known = m_known[m_assembled % kRing];
        if ((known & occupied) != occupied) break;
        // Empty slots are neutral.
        FrameInput& f = m_inputs[m_assembled % kRing];
        for (int s = 0; s < kMaxSlots; ++s) {
            if (!(occupied & (1u << s))) f.pad[s] = PadInput{};
        }
        ++m_assembled;
        // The next frame through this ring position starts empty.
        const uint32_t reuse = (m_assembled + kRing / 2) % kRing;
        m_known[reuse] = 0;
        m_dirty = true;
    }
}

bool Lockstep::frameReady(uint32_t frame) const { return running() && frame < m_assembled; }

const FrameInput& Lockstep::frameInput(uint32_t frame) const { return m_inputs[frame % kRing]; }

void Lockstep::reportHash(uint32_t frame, uint32_t hash) {
    if (frame % kHashInterval != 0) return;
    const uint32_t i = (frame / kHashInterval) % kRing;
    m_ourHash[i] = hash;
    m_ourHashFrame[i] = frame;
    m_lastHashFrame = frame;
    m_lastHash = hash;
    // Compare against anything that arrived before we got here.
    std::vector<PendingHash> pending;
    pending.swap(m_pendingHashes);
    for (const PendingHash& p : pending) checkHash(p.frame, p.hash, p.peer);
}

void Lockstep::checkHash(uint32_t frame, uint32_t theirs, int fromPeer) {
    if (frame == kNoHash || frame % kHashInterval != 0) return;
    const uint32_t i = (frame / kHashInterval) % kRing;
    if (m_ourHashFrame[i] != frame) {
        // Not there yet (or long past): keep it for when we are.
        if (m_lastHashFrame == UINT32_MAX || frame > m_lastHashFrame) {
            if (m_pendingHashes.size() < 64) m_pendingHashes.push_back({frame, theirs, fromPeer});
        }
        return;
    }
    if (m_ourHash[i] != theirs) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "game state differs from %s at frame %u (%08x here, %08x there)",
                      fromPeer == 0 ? "the host" : m_peers[fromPeer].name.c_str(), frame, m_ourHash[i], theirs);
        fail(Status::Desynced, msg);
    }
}

void Lockstep::leave() {
    if (m_config.host) {
        for (int i = 1; i < kMaxPeers; ++i) {
            if (m_peers[i].joined) sendBye(m_peers[i].endpoint);
        }
    } else {
        sendBye(m_config.hostEndpoint);
    }
    fail(Status::Disconnected, "left the session");
}

void Lockstep::sendBye(const Endpoint& to) {
    Writer w(kBye);
    m_transport.send(to, w.bytes().data(), w.size());
}

void Lockstep::sendHello() {
    Writer w(kHello);
    w.u64(m_config.buildId);
    w.str(m_config.name);
    m_transport.send(m_config.hostEndpoint, w.bytes().data(), w.size());
}

void Lockstep::sendWelcome(int peer) {
    Writer w(kWelcome);
    w.u8(uint8_t(peer));
    m_transport.send(m_peers[peer].endpoint, w.bytes().data(), w.size());
}

void Lockstep::sendStart(int peer) {
    Writer w(kStart);
    w.u8(uint8_t(peer));
    w.u8(uint8_t(m_numPeers));
    w.u8(m_delay);
    w.u32(m_seed);
    for (int s = 0; s < kMaxSlots; ++s) w.u8(uint8_t(m_slotOwner[s]));
    for (int p = 0; p < m_numPeers; ++p) w.str(m_peers[p].name);
    w.u8(uint8_t(m_matchConfig.size()));
    for (uint8_t b : m_matchConfig) w.u8(b);
    m_transport.send(m_peers[peer].endpoint, w.bytes().data(), w.size());
}

void Lockstep::sendInputs() {
    int owned = 0;
    for (int s = 0; s < kMaxSlots; ++s) owned += ownsSlot(s);
    Writer w(kInput);
    w.u32(m_assembled);
    w.u32(m_lastHashFrame == UINT32_MAX ? kNoHash : m_lastHashFrame);
    w.u32(m_lastHash);
    const uint32_t first = std::max(m_inputAcked, uint32_t(m_delay));
    const size_t perFrame = size_t(owned) * sizeof(PadInput);
    uint32_t count = 0;
    if (owned > 0 && m_localNext > first) {
        count = std::min<uint32_t>(m_localNext - first, uint32_t((kMaxPacket - w.size() - 5) / perFrame));
        count = std::min<uint32_t>(count, 255);
    }
    w.u32(first);
    w.u8(uint8_t(count));
    for (uint32_t f = first; f < first + count; ++f) {
        const FrameInput& in = m_inputs[f % kRing];
        for (int s = 0; s < kMaxSlots; ++s) {
            if (ownsSlot(s)) w.pad(in.pad[s]);
        }
    }
    m_transport.send(m_config.hostEndpoint, w.bytes().data(), w.size());
}

void Lockstep::sendFrames(int peer) {
    Peer& p = m_peers[peer];
    int occupied = 0;
    for (int s = 0; s < kMaxSlots; ++s) occupied += slotOccupied(s);
    // What this client has sent us, contiguous: everything it owns is known.
    uint32_t inputAck = m_assembled;
    while (inputAck < m_assembled + kRing / 2) {
        const uint8_t known = m_known[inputAck % kRing];
        bool all = true;
        for (int s = 0; s < kMaxSlots; ++s) {
            if (m_slotOwner[s] == peer && !(known & (1u << s))) all = false;
        }
        if (!all) break;
        ++inputAck;
    }
    Writer w(kFrames);
    w.u32(inputAck);
    w.u32(m_lastHashFrame == UINT32_MAX ? kNoHash : m_lastHashFrame);
    w.u32(m_lastHash);
    const uint32_t first = std::max(p.ackedFrames, uint32_t(m_delay));
    const size_t perFrame = std::max<size_t>(1, size_t(occupied) * sizeof(PadInput));
    uint32_t count = 0;
    if (m_assembled > first) {
        count = std::min<uint32_t>(m_assembled - first, uint32_t((kMaxPacket - w.size() - 5) / perFrame));
        count = std::min<uint32_t>(count, 255);
    }
    w.u32(first);
    w.u8(uint8_t(count));
    for (uint32_t f = first; f < first + count; ++f) {
        const FrameInput& in = m_inputs[f % kRing];
        for (int s = 0; s < kMaxSlots; ++s) {
            if (slotOccupied(s)) w.pad(in.pad[s]);
        }
    }
    m_transport.send(p.endpoint, w.bytes().data(), w.size());
}

void Lockstep::handle(const Endpoint& from, const uint8_t* data, size_t size, uint64_t nowMs) {
    Reader r(data, size);
    if (r.u32() != kMagic || r.u8() != kVersion) return;
    const uint8_t type = r.u8();
    if (!r.ok()) return;

    if (m_config.host) {
        int peer = findPeer(from);
        if (type == kHello) {
            const uint64_t build = r.u64();
            const std::string name = r.str();
            if (!r.ok()) return;
            if (build != m_config.buildId && !m_config.acceptAnyBuild) {
                Writer w(kReject);
                w.str("different game build or disc: everyone needs the same openstrikers and ISO");
                m_transport.send(from, w.bytes().data(), w.size());
                return;
            }
            if (peer < 0) {
                for (int i = 1; i < m_numPeers; ++i) {
                    if (!m_peers[i].joined) {
                        peer = i;
                        break;
                    }
                }
                if (peer < 0 || m_status != Status::Waiting) {
                    Writer w(kReject);
                    w.str("the session is full");
                    m_transport.send(from, w.bytes().data(), w.size());
                    return;
                }
                m_peers[peer].joined = true;
                m_peers[peer].endpoint = from;
                m_peers[peer].name = name.empty() ? "player " + std::to_string(peer + 1) : name;
                m_peers[peer].ackedFrames = m_delay;
            }
            m_peers[peer].lastHeardMs = nowMs;
            sendWelcome(peer);
            if (m_status == Status::Running) sendStart(peer);
            return;
        }
        if (peer < 0) return;
        Peer& p = m_peers[peer];
        p.lastHeardMs = nowMs;
        if (type == kDesync) {
            fail(Status::Desynced, r.str());
            return;
        }
        if (type == kBye) {
            fail(Status::Disconnected, p.name + " left the session");
            return;
        }
        if (type != kInput || m_status != Status::Running) return;
        p.started = true;
        const uint32_t acked = r.u32();
        const uint32_t hashFrame = r.u32();
        const uint32_t hash = r.u32();
        const uint32_t first = r.u32();
        const uint8_t count = r.u8();
        if (!r.ok()) return;
        p.ackedFrames = std::max(p.ackedFrames, std::min(acked, m_assembled));
        for (uint32_t f = first; f < first + count; ++f) {
            for (int s = 0; s < kMaxSlots; ++s) {
                if (m_slotOwner[s] != peer) continue;
                const PadInput in = r.pad();
                if (!r.ok()) return;
                if (f >= m_assembled && f < m_assembled + kRing / 2) setInput(f, s, in);
            }
        }
        checkHash(hashFrame, hash, peer);
        assembleFrames();
        return;
    }

    // Client: only the host talks to us.
    if (!(from == m_config.hostEndpoint)) return;
    m_peers[0].lastHeardMs = nowMs;
    switch (type) {
    case kWelcome:
        m_localPeer = r.u8();
        if (m_message.empty()) m_message = "joined; waiting for the other players";
        break;
    case kReject:
        fail(Status::Rejected, r.str());
        break;
    case kBye:
        fail(Status::Disconnected, "the host left the session");
        break;
    case kDesync:
        fail(Status::Desynced, r.str());
        break;
    case kStart: {
        if (m_status != Status::Connecting) break;
        const uint8_t peer = r.u8();
        const uint8_t numPeers = r.u8();
        const uint8_t delay = r.u8();
        const uint32_t seed = r.u32();
        int8_t owners[kMaxSlots];
        for (int s = 0; s < kMaxSlots; ++s) owners[s] = int8_t(r.u8());
        std::string names[kMaxPeers];
        for (int p = 0; p < numPeers && p < kMaxPeers; ++p) names[p] = r.str();
        std::vector<uint8_t> match(r.u8());
        for (uint8_t& b : match) b = r.u8();
        if (!r.ok() || numPeers > kMaxPeers || peer >= numPeers || delay == 0) break;
        m_matchConfig = std::move(match);
        m_localPeer = peer;
        m_numPeers = numPeers;
        m_delay = delay;
        m_seed = seed;
        std::memcpy(m_slotOwner, owners, sizeof(m_slotOwner));
        for (int p = 0; p < numPeers; ++p) {
            m_peers[p].joined = true;
            m_peers[p].name = names[p];
        }
        neutralFrames(m_inputs, m_known, kRing, m_delay);
        m_assembled = m_delay;
        m_localNext = m_delay;
        m_inputAcked = m_delay;
        m_status = Status::Running;
        m_message.clear();
        m_startedMs = nowMs;
        m_dirty = true;
        break;
    }
    case kFrames: {
        if (m_status != Status::Running) break;
        const uint32_t inputAck = r.u32();
        const uint32_t hashFrame = r.u32();
        const uint32_t hash = r.u32();
        const uint32_t first = r.u32();
        const uint8_t count = r.u8();
        if (!r.ok()) break;
        m_inputAcked = std::max(m_inputAcked, std::min(inputAck, m_localNext));
        for (uint32_t f = first; f < first + count; ++f) {
            FrameInput in{};
            for (int s = 0; s < kMaxSlots; ++s) {
                if (slotOccupied(s)) in.pad[s] = r.pad();
            }
            if (!r.ok()) break;
            if (f != m_assembled) continue; // old (or, never, from the future)
            // The host's copy of our own slots is what we sent it, so taking
            // the whole frame from it changes nothing here.
            m_inputs[f % kRing] = in;
            m_known[f % kRing] = 0xFF;
            ++m_assembled;
            m_known[(m_assembled + kRing / 2) % kRing] = 0;
            m_dirty = true;
        }
        checkHash(hashFrame, hash, 0);
        break;
    }
    default:
        break;
    }
}

void Lockstep::poll(uint64_t nowMs) {
    m_now = nowMs;
    if (m_startedMs == 0 && m_lastHelloMs == 0) {
        m_lastHelloMs = nowMs ? nowMs : 1;
        if (!m_config.host) sendHello();
        for (int i = 0; i < kMaxPeers; ++i) m_peers[i].lastHeardMs = nowMs;
    }

    Endpoint from;
    std::vector<uint8_t> buf;
    while (m_transport.receive(from, buf)) handle(from, buf.data(), buf.size(), nowMs);

    if (m_status == Status::Desynced && nowMs < m_desyncNoticeUntil && nowMs - m_lastSendMs >= 100) {
        m_lastSendMs = nowMs;
        sendDesync();
    }
    if (m_status == Status::Desynced || m_status == Status::Disconnected || m_status == Status::Rejected) return;

    if (m_config.host && m_status == Status::Waiting) {
        bool all = true;
        for (int i = 1; i < m_numPeers; ++i) all = all && m_peers[i].joined;
        if (all) {
            neutralFrames(m_inputs, m_known, kRing, m_delay);
            m_assembled = 0;
            m_localNext = m_delay;
            assembleFrames();
            m_status = Status::Running;
            m_startedMs = nowMs;
            for (int i = 1; i < m_numPeers; ++i) {
                m_peers[i].lastHeardMs = nowMs;
                sendStart(i);
            }
        }
        return;
    }

    if (!m_config.host && m_status == Status::Connecting) {
        if (nowMs - m_lastHelloMs >= 250) {
            m_lastHelloMs = nowMs;
            sendHello();
        }
        if (nowMs - m_peers[0].lastHeardMs > uint64_t(m_config.timeoutMs) * 6) {
            fail(Status::Disconnected, "no answer from the host");
        }
        return;
    }

    // Running.
    if (m_config.host) {
        for (int i = 1; i < m_numPeers; ++i) {
            if (nowMs - m_peers[i].lastHeardMs > m_config.timeoutMs) {
                fail(Status::Disconnected, "lost contact with " + m_peers[i].name);
                return;
            }
        }
    } else if (nowMs - m_peers[0].lastHeardMs > m_config.timeoutMs) {
        fail(Status::Disconnected, "lost contact with the host");
        return;
    }

    const uint64_t since = nowMs - m_lastSendMs;
    if ((m_dirty && since >= 4) || since >= 16) {
        m_lastSendMs = nowMs;
        m_dirty = false;
        if (m_config.host) {
            for (int i = 1; i < m_numPeers; ++i) {
                // Until the client shows it has Start, keep repeating it.
                if (!m_peers[i].started) sendStart(i);
                sendFrames(i);
            }
        } else {
            sendInputs();
        }
    }
}

} // namespace net
