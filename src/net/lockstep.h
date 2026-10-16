#pragma once

// Delay-based lockstep over an unreliable datagram transport.
//
// Every machine runs the whole game, frame by frame, from the same input. A
// frame may only run once every player's controller input for it is known, so
// each machine plays its own input a few frames late (the input delay) and
// sends it ahead to the others. The topology is a star: clients send their
// input to the host, the host assembles each frame's complete input once it has
// everyone's and sends that to all clients, and every machine -- the host too --
// runs a frame only from an assembled one. Only the host needs a reachable port.
//
// Packets are lost, duplicated and reordered freely; each one carries every
// frame the other side has not acknowledged yet, so nothing is ever resent on
// its own. Each side also reports a state hash every so often, and a mismatch
// stops the session instead of letting two machines play different games.
//
// Nothing here knows about the game, sockets or time sources: the caller owns
// the Transport and passes the clock in, so tests can drive it deterministically.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

constexpr int kMaxSlots = 8;   // controller slots in a match: one per fielder
constexpr int kMaxPeers = 8;   // machines in a session
constexpr uint32_t kHashInterval = 30; // frames between state hash checks
constexpr size_t kMaxMatchConfig = 200;

// One controller's state for one frame, as sent on the wire.
struct PadInput {
    uint16_t buttons = 0;
    int8_t stickX = 0, stickY = 0, substickX = 0, substickY = 0;
    uint8_t triggerLeft = 0, triggerRight = 0;
    bool operator==(const PadInput& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(PadInput) == 8);

struct FrameInput {
    PadInput pad[kMaxSlots];
};

// An opaque address. UDP stores a sockaddr; tests and a future relay store
// whatever names a peer for them.
struct Endpoint {
    uint8_t data[32] = {};
    uint32_t size = 0;
    bool operator==(const Endpoint& o) const { return size == o.size && std::memcmp(data, o.data, size) == 0; }
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const Endpoint& to, const uint8_t* data, size_t size) = 0;
    // Non-blocking. Returns false when nothing is waiting.
    virtual bool receive(Endpoint& from, std::vector<uint8_t>& data) = 0;
};

struct LockstepConfig {
    bool host = false;
    // Host: how many machines take part, the host included.
    int numPeers = 2;
    // Host: frames each machine's input is played late. Sent to the clients.
    uint8_t inputDelay = 3;
    // Host: the game's random seed. Sent to the clients.
    uint32_t seed = 0;
    // Host: which machine drives each slot (-1: nobody, slot is AI). By default
    // machine k drives slot k.
    int8_t slotOwner[kMaxSlots] = {0, 1, 2, 3, 4, 5, 6, 7};
    // Client: where the host is.
    Endpoint hostEndpoint;
    // Client: the machine number to take (1..numPeers-1), as a lobby assigned
    // it, or 0 for the first free one.
    uint8_t seat = 0;
    // Everyone: identifies the executable and disc; must match the host's.
    uint64_t buildId = 0;
    // Host: admit any build. Only for test peers that run no game.
    bool acceptAnyBuild = false;
    // Host: the match everyone plays (teams, stadium, rules), opaque here and
    // sent with Start. At most kMaxMatchConfig bytes.
    std::vector<uint8_t> matchConfig;
    // Everyone: shown to the other players.
    std::string name;
    // Milliseconds without hearing from a machine before giving up on it.
    uint32_t timeoutMs = 10000;
};

class Lockstep {
public:
    enum class Status {
        Connecting, // client: waiting for the host to admit us and start
        Waiting,    // host: waiting for every client to join
        Running,
        Desynced,
        Disconnected,
        Rejected,
    };

    Lockstep(Transport& transport, const LockstepConfig& config);

    // Handle incoming packets and send what is due. Call often, with a
    // monotonic millisecond clock.
    void poll(uint64_t nowMs);

    Status status() const { return m_status; }
    const std::string& statusMessage() const { return m_message; }
    bool running() const { return m_status == Status::Running; }

    // Valid once running.
    int localPeer() const { return m_localPeer; }
    int numPeers() const { return m_numPeers; }
    uint32_t seed() const { return m_seed; }
    uint8_t inputDelay() const { return m_delay; }
    int slotOwner(int slot) const { return m_slotOwner[slot]; }
    const std::vector<uint8_t>& matchConfig() const { return m_matchConfig; }
    const std::string& peerName(int peer) const { return m_peers[peer].name; }
    bool slotOccupied(int slot) const { return m_slotOwner[slot] >= 0; }

    // This machine's input for `frame` (frame >= inputDelay; the frames before
    // that are neutral). One PadInput per slot this machine owns, in slot
    // order. Submitting a frame twice keeps the first.
    void submitLocal(uint32_t frame, const PadInput* padsForOwnedSlots);

    // The frame every machine runs next is complete. Frames are taken in order.
    bool frameReady(uint32_t frame) const;
    const FrameInput& frameInput(uint32_t frame) const;

    // This machine's state hash after running `frame`. Every kHashInterval-th
    // frame is compared with the other machines'.
    void reportHash(uint32_t frame, uint32_t hash);

    // Tell the others this machine is leaving.
    void leave();

private:
    static constexpr uint32_t kRing = 512;

    struct Peer {
        Endpoint endpoint;
        bool joined = false;
        std::string name;
        uint64_t lastHeardMs = 0;
        // Host: frames assembled that this client has acknowledged.
        uint32_t ackedFrames = 0;
        // Host: true once the client has shown it got Start.
        bool started = false;
    };

    void handle(const Endpoint& from, const uint8_t* data, size_t size, uint64_t nowMs);
    void sendHello();
    void sendWelcome(int peer);
    void sendStart(int peer);
    void sendInputs();
    void sendFrames(int peer);
    void sendBye(const Endpoint& to);
    void sendDesync();
    void assembleFrames();
    void checkHash(uint32_t frame, uint32_t theirs, int fromPeer);
    void fail(Status status, const std::string& message);
    int findPeer(const Endpoint& from) const;
    bool ownsSlot(int slot) const { return m_slotOwner[slot] == m_localPeer; }
    void setInput(uint32_t frame, int slot, const PadInput& in);

    Transport& m_transport;
    LockstepConfig m_config;
    Status m_status;
    std::string m_message;

    int m_localPeer = -1;
    int m_numPeers = 0;
    uint32_t m_seed = 0;
    uint8_t m_delay = 0;
    int8_t m_slotOwner[kMaxSlots];
    std::vector<uint8_t> m_matchConfig;
    Peer m_peers[kMaxPeers];

    // Per frame (mod kRing): every slot's input and which slots are known.
    FrameInput m_inputs[kRing];
    uint8_t m_known[kRing] = {};
    // Frames [0, m_assembled) are complete. The host assembles them; clients
    // learn them from the host.
    uint32_t m_assembled = 0;
    // Local input submitted for frames [0, m_localNext).
    uint32_t m_localNext = 0;
    // Host: for each slot, input known for frames [0, m_slotNext[slot]).
    uint32_t m_slotNext[kMaxSlots] = {};
    // Client: the host has acknowledged our input up to here.
    uint32_t m_inputAcked = 0;

    // Hashes, by frame / kHashInterval (mod kRing): ours, and the last one heard.
    uint32_t m_ourHash[kRing] = {};
    uint32_t m_ourHashFrame[kRing] = {};
    uint32_t m_lastHashFrame = UINT32_MAX;
    uint32_t m_lastHash = 0;
    struct PendingHash {
        uint32_t frame, hash;
        int peer;
    };
    std::vector<PendingHash> m_pendingHashes;

    uint64_t m_now = 0;
    uint64_t m_lastSendMs = 0;
    uint64_t m_lastHelloMs = 0;
    uint64_t m_startedMs = 0;
    uint64_t m_desyncNoticeUntil = 0;
    bool m_dirty = false;
};

} // namespace net
