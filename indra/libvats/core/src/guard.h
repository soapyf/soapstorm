// Viewport Avatar Toolset - shared limits and a last line of defence for the file and packet readers.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Files and network packets are untrusted. Readers validate what they read; `guarded` makes sure that
// an allocation or length failure they missed becomes an error message instead of ending the program.
#pragma once

#include <cmath>
#include <exception>
#include <new>
#include <string>

namespace vats {

// Most samples (frames x joints) a reader builds: 10 minutes at 30 fps for 440 joints.
inline constexpr double kMaxSamples = 8'000'000;
// Longest animation a reader samples, in seconds.
inline constexpr double kMaxSeconds = 600;

// Frames covering `seconds` at `fps` (both assumed positive), at least 1 and at most kMaxSeconds worth.
inline int sample_count(double seconds, double fps) {
    double n = std::isfinite(seconds) && seconds > 0 ? std::floor(std::min(seconds, kMaxSeconds) * fps + 1e-6) : 0;
    return int(n) + 1;
}

template <class F>
bool guarded(std::string& err, F&& body) {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        err = "the input needs more memory than is available";
    } catch (const std::exception& e) {
        err = std::string("unreadable input (") + e.what() + ")";
    }
    return false;
}

}  // namespace vats
