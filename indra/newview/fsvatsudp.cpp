// Viewport Avatar Toolset - a non-blocking UDP receiver.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
// COPY of Viewport Avatar Toolset's app/udp.cpp (the app's file is the source; keep the logic identical and
// re-copy it from there when it changes). Used by the viewer's motion capture (VATs spec 09, stage 6h).
#include "llviewerprecompiledheaders.h"

#include "fsvatsudp.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vats {

bool UdpReceiver::open(int port, bool lan, std::string& err) {
    close();
#ifdef _WIN32
    static bool started = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!started) return err = "Windows sockets could not start", false;
#endif
    auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return err = "cannot create a socket", false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    if (s < 0) return err = std::string("cannot create a socket: ") + std::strerror(errno), false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
#ifdef _WIN32
    // No address reuse: with it bind succeeds while another app owns the port and VATs would hear nothing.
    // Exclusive use also stops another program taking the port from VATs.
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof yes);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(std::uint16_t(port));
    a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    sock_ = std::intptr_t(s);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
#ifdef _WIN32
        err = "port " + std::to_string(port) + " is in use or not allowed";
#else
        err = errno == EADDRINUSE ? "port " + std::to_string(port) + " is in use by another program"
                                  : "port " + std::to_string(port) + ": " + std::strerror(errno);
#endif
        close();
        return false;
    }
    return true;
}

void UdpReceiver::close() {
    if (sock_ == kNone) return;
#ifdef _WIN32
    closesocket(SOCKET(sock_));
#else
    ::close(int(sock_));
#endif
    sock_ = kNone;
}

bool UdpReceiver::send_to(const std::string& ipv4, int port, const std::string& data, std::string& err) {
    if (sock_ == kNone) return err = "not listening", false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(std::uint16_t(port));
    if (inet_pton(AF_INET, ipv4.c_str(), &a.sin_addr) != 1) return err = "\"" + ipv4 + "\" is not an IPv4 address", false;
#ifdef _WIN32
    const int n = ::sendto(SOCKET(sock_), data.data(), int(data.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
#else
    const int n = int(::sendto(int(sock_), data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof a));
#endif
    if (n != int(data.size())) return err = "could not send to " + ipv4, false;
    return true;
}

int UdpReceiver::receive(std::uint8_t* buf, int cap, std::string* from) {
    if (sock_ == kNone) return 0;
    sockaddr_in a{};
    socklen_t len = sizeof a;
#ifdef _WIN32
    int n = ::recvfrom(SOCKET(sock_), reinterpret_cast<char*>(buf), cap, 0, reinterpret_cast<sockaddr*>(&a), &len);
#else
    int n = int(::recvfrom(int(sock_), buf, size_t(cap), 0, reinterpret_cast<sockaddr*>(&a), &len));
#endif
    if (n <= 0) return 0;  // nothing waiting (or an error, which reads the same to a poller)
    if (from) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
        *from = std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
    }
    return n;
}

}  // namespace vats
