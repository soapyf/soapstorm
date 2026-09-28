// Viewport Avatar Toolset - scalar animation curves.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/02 sections 2.3 and 3.1-3.3.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace vats {

// How the segment from a key to the next one is evaluated.
enum class Interp : std::uint8_t { Constant = 0, Linear = 1, Bezier = 2 };

// Handle types, stored as 0-6 in project files.
enum class Handle : std::uint8_t { AutoClamped = 0, Auto, Vector, Aligned, Free, Flat, Plateau };

// A key's role in the pose-to-pose workflow (spec 08 KT-1), saved as one byte per key (0-3).
enum class KeyTag : std::uint8_t { None = 0, Extreme, Breakdown, Hold };

// UI tangent commands.
enum class Tangent { Auto, Spline, Plateau, Linear, Flat, Stepped, Break, Unify };

struct Key {
    double frame = 0, value = 0;
    Interp interp = Interp::Bezier;
    Handle left = Handle::AutoClamped, right = Handle::AutoClamped;
    double lx = 0, ly = 0, rx = 0, ry = 0;  // handle points, absolute (frame, value)
    KeyTag tag = KeyTag::None;

    bool operator==(const Key&) const = default;
};

// Two frames count as the same key frame within a relative 1e-5.
bool same_frame(double a, double b);

class FCurve {
public:
    std::vector<Key> keys;  // sorted by frame

    bool operator==(const FCurve&) const = default;

    bool empty() const { return keys.empty(); }
    double evaluate(double frame) const;

    // Index of the key at frame, or -1.
    int find(double frame) const;
    // Sets or inserts a key; an existing key keeps its handles' offsets. Returns its index.
    int set_key(double frame, double value, std::optional<Interp> interp = std::nullopt);
    bool remove_key(double frame);
    void apply_tangent(int index, Tangent t);

    // Recomputes every automatic handle. Aligned and Free handles are left as they are.
    void recompute_handles();
};

}  // namespace vats
