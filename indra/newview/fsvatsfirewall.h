// Viewport Avatar Toolset - letting a phone or headset reach motion capture: this computer's network
// addresses, whether a firewall is on, and the one rule that lets the home network in.
// COPY of Viewport Avatar Toolset's app/firewall.h (the app's file is the source; keep the logic identical and
// re-copy it from there when it changes). Used by the viewer's motion capture (VATs spec 09, stage 6h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Plain C++ and POSIX with no UI, so the viewer's motion capture (spec 09 stage 6h) can use it as is.
// Detection never needs root; only allow_command's pkexec form asks for a password, and only when the
// user presses the button.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace vats {

struct LanAddress {
    std::string name;    // interface, e.g. "enp39s0"
    std::string ip;      // "192.168.68.77"
    std::string subnet;  // "192.168.68.0/22"
};
// IPv4 addresses other devices can reach: no loopback, link-local, Docker/VM bridges or VPN tunnels.
std::vector<LanAddress> lan_addresses();
// "192.168.68.77", 22 -> "192.168.68.0/22"; "" for a bad address or prefix.
std::string subnet_of(const std::string& ipv4, int prefix);
// Interfaces whose addresses a phone on the home network cannot reach.
bool is_virtual_interface(const std::string& name);

enum class Firewall { Unknown, Off, Ufw, Firewalld };  // not "None": X11 defines None as a macro (the viewer copies this file)
// Runs a shell command and returns its exit status, with stdout in out (-1 when it cannot run).
using CommandRunner = std::function<int(const std::string& command, std::string& out)>;
CommandRunner system_runner();
// Linux: ufw or firewalld when either is filtering; Off when neither is; Unknown on other systems.
// VATS_FAKE_FIREWALL=ufw|firewalld|none overrides it (for screenshots and tests, changes nothing).
Firewall detect_firewall(const CommandRunner& run, const std::string& ufw_conf = "/etc/ufw/ufw.conf");
const char* firewall_name(Firewall f);
// The command that opens UDP port to subnet, starting with elevate ("pkexec" to run it from the app,
// "sudo" to show for typing). "" when there is nothing to run.
std::string allow_command(Firewall f, int port, const std::string& subnet, const std::string& elevate);

}  // namespace vats
