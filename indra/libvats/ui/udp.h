// Viewport Avatar Toolset - a non-blocking UDP receiver, polled once per frame (motion capture).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <cstdint>
#include <string>

namespace vats {

class UdpReceiver {
public:
    UdpReceiver() = default;
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    ~UdpReceiver() { close(); }

    // Binds port on 127.0.0.1, or on every interface when lan is true (senders on other devices).
    bool open(int port, bool lan, std::string& err);
    void close();
    bool is_open() const { return sock_ != kNone; }
    // The next waiting datagram's size, or 0 when none is waiting. from gets "address:port".
    int receive(std::uint8_t* buf, int cap, std::string* from = nullptr);
    // Sends one datagram from this socket to an IPv4 address (iFacialMocap needs a hello to start).
    bool send_to(const std::string& ipv4, int port, const std::string& data, std::string& err);

private:
    static constexpr std::intptr_t kNone = -1;
    std::intptr_t sock_ = kNone;
};

}  // namespace vats
