// Viewport Avatar Toolset - frame timing for --bench: named scopes collected only while benchmarking.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace vats {

struct Profile {
    static inline bool on = false;
    static inline std::map<std::string, std::vector<double>> ms;  // section -> one sample per frame
    static void add(const char* name, double v) { ms[name].push_back(v); }
};

struct ProfileScope {
    const char* name;
    std::chrono::steady_clock::time_point t0;
    explicit ProfileScope(const char* n) : name(n), t0(Profile::on ? std::chrono::steady_clock::now() : decltype(t0){}) {}
    ~ProfileScope() {
        if (Profile::on) Profile::add(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
};

}  // namespace vats

#define VATS_PROFILE_CAT2(a, b) a##b
#define VATS_PROFILE_CAT(a, b) VATS_PROFILE_CAT2(a, b)
#define VATS_PROFILE(name) ::vats::ProfileScope VATS_PROFILE_CAT(profile_scope_, __LINE__)(name)
