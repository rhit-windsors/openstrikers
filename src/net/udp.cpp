#include "net/udp.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET // not in mingw-w64's headers
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using socket_t = SOCKET;
static constexpr socket_t kNoSocket = INVALID_SOCKET;
static int lastSocketError() { return WSAGetLastError(); }
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kNoSocket = -1;
static int lastSocketError() { return errno; }
static void closeSocket(socket_t s) { ::close(s); }
#endif

#include <cstdio>
#include <cstring>

namespace net {
namespace {

bool startSockets(std::string& error) {
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "Windows sockets would not start";
            return false;
        }
        started = true;
    }
#else
    (void)error;
#endif
    return true;
}

socket_t asSocket(intptr_t s) { return static_cast<socket_t>(s); }

} // namespace

UdpTransport::~UdpTransport() { close(); }

void UdpTransport::close() {
    if (asSocket(m_socket) != kNoSocket && m_socket != -1) {
        closeSocket(asSocket(m_socket));
    }
    m_socket = -1;
}

bool UdpTransport::open(uint16_t port, std::string& error) {
    close();
    if (!startSockets(error)) return false;
    const socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        error = "could not create a UDP socket (error " + std::to_string(lastSocketError()) + ")";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "could not use UDP port " + std::to_string(port) + " (error " + std::to_string(lastSocketError()) +
                "); is another copy already hosting?";
        closeSocket(s);
        return false;
    }
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // Windows reports an ICMP "port unreachable" for an earlier send as a
    // failed receive (WSAECONNRESET) on this socket. A peer that has not
    // started yet must not look like a broken socket.
    BOOL reportReset = FALSE;
    DWORD unused = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &unused, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    m_socket = static_cast<intptr_t>(s);
    return true;
}

void UdpTransport::send(const Endpoint& to, const uint8_t* data, size_t size) {
    if (m_socket == -1 || to.size == 0) return;
    ::sendto(asSocket(m_socket), reinterpret_cast<const char*>(data), int(size), 0,
             reinterpret_cast<const sockaddr*>(to.data), int(to.size));
}

bool UdpTransport::receive(Endpoint& from, std::vector<uint8_t>& data) {
    if (m_socket == -1) return false;
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int n = int(::recvfrom(asSocket(m_socket), reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&addr), &len));
        if (n < 0) {
#ifdef _WIN32
            if (lastSocketError() == WSAECONNRESET) continue;
#endif
            return false;
        }
        from = Endpoint{};
        std::memcpy(from.data, &addr, sizeof(addr));
        from.size = sizeof(addr);
        data.assign(buf, buf + n);
        return true;
    }
}

bool UdpTransport::resolve(const std::string& address, uint16_t defaultPort, Endpoint& out, std::string& error) {
    if (!startSockets(error)) return false;
    std::string host = address;
    std::string port = std::to_string(defaultPort);
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        host = address.substr(0, colon);
        port = address.substr(colon + 1);
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
        error = "cannot find the address " + address;
        return false;
    }
    out = Endpoint{};
    std::memcpy(out.data, result->ai_addr, sizeof(sockaddr_in));
    out.size = sizeof(sockaddr_in);
    freeaddrinfo(result);
    return true;
}

std::string UdpTransport::describe(const Endpoint& endpoint) {
    if (endpoint.size != sizeof(sockaddr_in)) return "?";
    sockaddr_in addr;
    std::memcpy(&addr, endpoint.data, sizeof(addr));
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace net
