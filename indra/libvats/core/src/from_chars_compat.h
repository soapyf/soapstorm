// Viewport Avatar Toolset - from_chars compatibility header.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <charconv>
#include <cmath>
#include <system_error>
#include <type_traits>

namespace vats {

#if defined(__APPLE__)
// Apple Clang's libc++ marks floating-point std::from_chars as deleted when
// targeting macOS < 13.3 (-mmacosx-version-min=11 in the viewer).
template <typename T>
typename std::enable_if<std::is_floating_point<T>::value, std::from_chars_result>::type
from_chars(const char* first, const char* last, T& value) {
    if (first >= last) return {first, std::errc::invalid_argument};
    const char* p = first;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }
    if (p >= last) return {first, std::errc::invalid_argument};

    double int_part = 0.0;
    bool has_digits = false;
    while (p < last && *p >= '0' && *p <= '9') {
        int_part = int_part * 10.0 + (*p - '0');
        has_digits = true;
        ++p;
    }

    double frac_part = 0.0;
    double frac_scale = 0.1;
    if (p < last && *p == '.') {
        ++p;
        while (p < last && *p >= '0' && *p <= '9') {
            frac_part += (*p - '0') * frac_scale;
            frac_scale *= 0.1;
            has_digits = true;
            ++p;
        }
    }

    if (!has_digits) return {first, std::errc::invalid_argument};

    double result = int_part + frac_part;

    if (p < last && (*p == 'e' || *p == 'E')) {
        const char* exp_start = p;
        ++p;
        bool exp_neg = false;
        if (p < last && *p == '-') {
            exp_neg = true;
            ++p;
        } else if (p < last && *p == '+') {
            ++p;
        }
        int exp_val = 0;
        bool has_exp_digits = false;
        while (p < last && *p >= '0' && *p <= '9') {
            exp_val = exp_val * 10 + (*p - '0');
            has_exp_digits = true;
            ++p;
        }
        if (!has_exp_digits) {
            p = exp_start;
        } else {
            if (exp_neg && exp_val > 308) {
                value = static_cast<T>(neg ? -0.0 : 0.0);
                return {p, std::errc::result_out_of_range};
            }
            result *= std::pow(10.0, exp_neg ? -exp_val : exp_val);
        }
    }

    if (std::isinf(result)) {
        value = static_cast<T>(neg ? -result : result);
        return {p, std::errc::result_out_of_range};
    }

    value = static_cast<T>(neg ? -result : result);
    return {p, std::errc()};
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value, std::from_chars_result>::type
from_chars(const char* first, const char* last, T& value) {
    return std::from_chars(first, last, value);
}

#else

using std::from_chars;

#endif

} // namespace vats
