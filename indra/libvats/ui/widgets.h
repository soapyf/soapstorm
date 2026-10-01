// Viewport Avatar Toolset - the shared slider, and small layout and wording helpers.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Every slider in the editor is slider_float() or slider_int(): it drags from where the value is (a click never
// jumps it), a double-click or Ctrl+click types a value, Shift drags faster and Alt slower. The inline helpers
// below need no ImGui, so the tests use them directly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace vats {

// How a slider maps the pointer to its value. Log spreads values that span orders of magnitude (0.01 to 5)
// evenly along the drag.
enum class SliderCurve { Linear, Log };

// px: how far the pointer travels over the whole range; 0 = 1.5 times the slider's width (ints: about 24 px a
// step within half to 1.5 widths). fmt is printf-style, as ImGui's.
bool slider_float(const char* label, float* v, float lo, float hi, const char* fmt = "%.3f", float px = 0,
                  SliderCurve curve = SliderCurve::Linear);
bool slider_int(const char* label, int* v, int lo, int hi, const char* fmt = "%d", float px = 0);

// Pointer travel over a slider's whole range for its width w: px when set, else 1.5 w; for ints about 24 px a
// step, within half to 1.5 widths.
inline float slider_travel(float w, float px, int steps = 0) {
    if (px > 0) return px;
    if (steps > 0) return std::clamp(24.f * float(steps), 0.5f * w, 1.5f * w);
    return 1.5f * w;
}

// "1 bone", "3 bones", "0 bones". many defaults to one + "s".
inline std::string count_noun(std::size_t n, const std::string& one, const std::string& many = "") {
    return std::to_string(n) + " " + (n == 1 ? one : many.empty() ? one + "s" : many);
}

// Widens [lo, hi] about its middle to at least min_span (a flat or near-flat curve still fills the view).
inline void ensure_span(double& lo, double& hi, double min_span) {
    if (hi - lo >= min_span) return;
    const double mid = (lo + hi) / 2;
    lo = mid - min_span / 2, hi = mid + min_span / 2;
}

// Zooms [lo, hi] by k about the value at, which stays where it is on screen.
inline void zoom_about(double& lo, double& hi, double at, double k) {
    const double span = hi - lo, r = span != 0 ? (at - lo) / span : 0.5;
    lo = at - span * k * r, hi = lo + span * k;
}

struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

// Where a tool window of size w x h first opens, as its top-left corner: inside work (shrunk to fit), off the
// avatar's centre line avoid_x where it can, and over the other open windows as little as it can. The windows
// stack down the right edge of the work area first (beside the view, over the side panels), then the left.
inline Box place_window(const Box& work, float w, float h, float avoid_x, const std::vector<Box>& others, float margin,
                        float step) {
    w = std::min(w, work.w() - 2 * margin), h = std::min(h, work.h() - 2 * margin);
    auto overlap = [](const Box& a, const Box& b) {
        return std::max(0.f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0)) *
               std::max(0.f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
    };
    Box best;
    float best_cost = 1e30f;
    const float xs[2] = {work.x1 - margin - w, work.x0 + margin};
    for (int row = 0; row < 12; ++row) {
        const float y = std::min(work.y0 + margin + row * step, work.y1 - margin - h);
        for (float x : xs) {
            const Box b{x, y, x + w, y + h};
            float cost = row * 1e-3f * w * h;  // higher up wins a tie
            if (b.x0 < avoid_x && avoid_x < b.x1) cost += 0.5f * w * h;  // over the avatar: worse than half covered
            for (const Box& o : others) cost += overlap(b, o);
            if (cost < best_cost) best_cost = cost, best = b;
        }
    }
    return best;
}

}  // namespace vats
