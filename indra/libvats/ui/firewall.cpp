// Viewport Avatar Toolset - network addresses and firewall help for motion capture senders.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "firewall.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/wait.h>
#endif

namespace vats {

bool is_virtual_interface(const std::string& name) {
    for (const char* p : {"lo", "docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "tun", "tap", "wg",
                          "tailscale", "zt", "mullvad", "utun", "podman", "cni", "flannel", "kube"})
        if (name.rfind(p, 0) == 0) return true;
    return false;
}

std::string subnet_of(const std::string& ipv4, int prefix) {
    unsigned a, b, c, d;
    char tail;
    if (prefix < 0 || prefix > 32 || std::sscanf(ipv4.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
        return "";
    const unsigned ip = a << 24 | b << 16 | c << 8 | d;
    const unsigned mask = prefix == 0 ? 0u : ~0u << (32 - prefix);
    const unsigned net = ip & mask;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u/%d", net >> 24, net >> 16 & 255, net >> 8 & 255, net & 255, prefix);
    return buf;
}

std::vector<LanAddress> lan_addresses() {
    std::vector<LanAddress> out;
    if (const char* fake = std::getenv("VATS_FAKE_LAN")) {  // scripted screenshots: never this computer's address
        out.push_back({"wlan0", fake, subnet_of(fake, 24)});
        return out;
    }
#if !defined(_WIN32)
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK) || is_virtual_interface(i->ifa_name))
            continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, ip, sizeof ip);
        if (std::string(ip).rfind("169.254.", 0) == 0) continue;  // link-local: no router handed one out
        int prefix = 24;
        if (i->ifa_netmask)
            prefix = __builtin_popcount(ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr));
        out.push_back({i->ifa_name, ip, subnet_of(ip, prefix)});
    }
    freeifaddrs(list);
#endif
    // ponytail: Windows lists nothing here yet (GetAdaptersAddresses); the app then shows no address row.
    return out;
}

CommandRunner system_runner() {
    return [](const std::string& command, std::string& out) {
        out.clear();
#if defined(_WIN32)
        (void)command;
        return -1;
#else
        FILE* p = popen((command + " 2>/dev/null").c_str(), "r");
        if (!p) return -1;
        char buf[256];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        const int status = pclose(p);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    };
}

Firewall detect_firewall(const CommandRunner& run, const std::string& ufw_conf) {
    if (const char* fake = std::getenv("VATS_FAKE_FIREWALL")) {
        const std::string f = fake;
        return f == "ufw" ? Firewall::Ufw : f == "firewalld" ? Firewall::Firewalld : Firewall::Off;
    }
#if defined(__linux__)
    std::string out;
    if (run("systemctl is-active ufw", out) == 0 && out.rfind("active", 0) == 0) {
        // The service can run with filtering switched off; its config says which, and is world readable.
        std::ifstream conf(ufw_conf);
        std::string line;
        bool enabled = !conf;  // unreadable: trust the running service
        while (std::getline(conf, line))
            if (line.rfind("ENABLED=", 0) == 0) enabled = line.find("yes") != std::string::npos;
        if (enabled) return Firewall::Ufw;
    }
    if (run("firewall-cmd --state", out) == 0 && out.rfind("running", 0) == 0) return Firewall::Firewalld;
    return Firewall::Off;
#else
    (void)run, (void)ufw_conf;
    return Firewall::Unknown;
#endif
}

const char* firewall_name(Firewall f) {
    switch (f) {
        case Firewall::Ufw: return "ufw";
        case Firewall::Firewalld: return "firewalld";
        case Firewall::Off: return "none";
        default: return "unknown";
    }
}

std::string allow_command(Firewall f, int port, const std::string& subnet, const std::string& elevate) {
    if (subnet.empty() || port < 1 || port > 65535) return "";
    const std::string p = std::to_string(port);
    if (f == Firewall::Ufw) return elevate + " ufw allow from " + subnet + " to any port " + p + " proto udp";
    if (f == Firewall::Firewalld)  // one password for both steps; the rule only admits the home network
        return elevate + " sh -c 'firewall-cmd --permanent --add-rich-rule=\"rule family=ipv4 source address=" +
               subnet + " port port=" + p + " protocol=udp accept\" && firewall-cmd --reload'";
    return "";
}

}  // namespace vats
