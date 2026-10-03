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

// A tool window's row: its label in a fixed column on the left, then the next item (a field, a slider, a combo)
// filling the rest, or `width` ems of it. One column width for every tool window, so their fields line up; a
// window whose labels are longer passes its own. A label wider than the column pushes the item along.
inline constexpr float kLabelEm = 6.5f;
float label_column(float em = kLabelEm);  // the column's width in pixels, for SetCursorPosX under a row
void labelled_row(const char* label, float em = kLabelEm, float width = 0);

// A tool window's title: its full name as a floating window ("Map Rig to Second Life"), a short one on a docked
// tab ("Map Rig") so a strip of four reads; id ("map-rig") keeps it one window. After Begin, tab_tooltip() names it
// in full on the tab.
std::string dock_title(const char* full, const char* tab, const char* id);
void tab_tooltip(const char* full);

// A list or panel with nothing in it yet: one dim sentence centred in the space left (or in `height`, a child's
// height in pixels), with the action that fills it under the sentence. True when the action is pressed.
bool empty_state(const char* text, const char* action = nullptr, float height = 0);

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

// An imported body's name (Inventory > Bodies): one file by its name, several by the folder they share
// ("Reborn" for Reborn/head.dae, Reborn/upper.dae), else the first file's name. Paths take / or \.
inline std::string body_name(const std::vector<std::string>& paths) {
    auto slash = [](const std::string& p) { return p.find_last_of("/\\"); };
    auto dir = [&](const std::string& p) { return slash(p) == std::string::npos ? std::string() : p.substr(0, slash(p)); };
    auto last = [&](const std::string& p) { return slash(p) == std::string::npos ? p : p.substr(slash(p) + 1); };
    if (paths.empty()) return "Body";
    const std::string folder = dir(paths[0]);
    bool shared = paths.size() > 1 && !folder.empty();
    for (const std::string& p : paths) shared = shared && dir(p) == folder;
    if (shared) return last(folder);
    const std::string file = last(paths[0]);
    return file.substr(0, file.find_last_of('.'));
}

// The same files, in any order: importing them again is the body already in the Inventory.
inline bool same_files(std::vector<std::string> a, std::vector<std::string> b) {
    std::sort(a.begin(), a.end()), std::sort(b.begin(), b.end());
    return !a.empty() && a == b;
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
// stack down the right edge of the work area first, then the left.
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

// The area a tool window first opens in: the 3D view, so it never lands on a panel (Properties, Bones) and reads as
// part of it, when the view has room for its width and min_h of its height; else the whole work area.
inline Box tool_window_area(const Box& work, const Box& view, float w, float min_h, float margin) {
    const bool fits = view.w() >= w + 2 * margin && view.h() >= min_h + 2 * margin;
    return fits ? view : work;
}

// Where a tool window that works on the view (the Hand Poser) first opens, so it covers none of the avatar: beside the
// view, over the panel on its right (side 1) or else its left (-1), shrunk by scale (down to 0.6) to fit there; else
// inside the view's bottom-right corner (0), shrunk to half the view's width (down to 0.45). w: its full width.
struct BesidePlace {
    int side = 0;
    float scale = 1;
};
inline BesidePlace beside_view(const Box& work, const Box& view, float w, float margin) {
    const float right = work.x1 - view.x1 - 2 * margin, left = view.x0 - work.x0 - 2 * margin;
    if (right >= 0.6f * w) return {1, std::min(1.f, right / w)};
    if (left >= 0.6f * w) return {-1, std::min(1.f, left / w)};
    return {0, std::clamp((0.5f * view.w() - 2 * margin) / w, 0.45f, 1.f)};
}

// A window kept wholly inside work (a fixed-size one, such as the Hands pad): its top-left corner moved in.
inline Box clamp_window(const Box& work, const Box& win) {
    const float x = std::clamp(win.x0, work.x0, std::max(work.x0, work.x1 - win.w()));
    const float y = std::clamp(win.y0, work.y0, std::max(work.y0, work.y1 - win.h()));
    return {x, y, x + win.w(), y + win.h()};
}

// Every filter or search box: ImGui's text box with a × at its right end while it holds text, which empties it.
// True when the text changed (typed or cleared). The item after it (IsItemHovered and the rest) is the box.
bool filter_input(const char* id, const char* hint, char* buf, std::size_t size);

// The Animation Check's finding icon (lint_ui.cpp): a red cross, an amber triangle or a ring with an i.
enum class LintSeverity;
void severity_icon(LintSeverity s);

}  // namespace vats
