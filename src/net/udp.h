#pragma once

#include "net/lockstep.h"

#include <string>

namespace net {

// IPv4 UDP, non-blocking. The address-is-a-peer transport: a relay would
// implement the same interface with room codes instead.
class UdpTransport : public Transport {
public:
    UdpTransport() = default;
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // port 0 takes any free port (a client); a host binds the one it was given.
    bool open(uint16_t port, std::string& error);
    void close();

    void send(const Endpoint& to, const uint8_t* data, size_t size) override;
    bool receive(Endpoint& from, std::vector<uint8_t>& data) override;

    // "host", "host:port", "1.2.3.4:port". defaultPort when none is given.
    static bool resolve(const std::string& address, uint16_t defaultPort, Endpoint& out, std::string& error);
    static std::string describe(const Endpoint& endpoint);

private:
    intptr_t m_socket = -1;
};

} // namespace net
