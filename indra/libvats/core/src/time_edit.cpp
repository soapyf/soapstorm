// Viewport Avatar Toolset - editing time (spec 08 TE).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/time_edit.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "vats/pose_ops.h"

namespace vats {
namespace {

bool wanted(const std::vector<std::string>& tracks, const std::string& name) {
    return tracks.empty() || std::find(tracks.begin(), tracks.end(), name) != tracks.end();
}

// Moves every edited key through `map`; keys for which `gone` is true are deleted first.
void remap(Clip& clip, const std::vector<std::string>& tracks, const std::function<double(double)>& map,
           const std::function<bool(double)>& gone) {
    for (auto& [name, track] : clip.curves) {
        if (!wanted(tracks, name)) continue;
        for (auto& [ch, c] : track) {
            std::vector<Key> keys;
            for (Key k : c.keys) {
                if (gone(k.frame)) continue;
                k.frame = map(k.frame), k.lx = map(k.lx), k.rx = map(k.rx);
                keys.push_back(k);
            }
            std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
            c.keys.clear();
            for (const Key& k : keys) {  // a squash can land two keys on one frame: the later one wins
                if (!c.keys.empty() && same_frame(c.keys.back().frame, k.frame)) c.keys.back() = k;
                else c.keys.push_back(k);
            }
            c.recompute_handles();
        }
    }
    const bool all = tracks.empty();
    auto whole = [&](int f) { return f < 0 ? f : int(std::lround(map(f))); };  // -1 = none stays -1
    if (all) {
        clip.loop_in = whole(clip.loop_in);
        clip.loop_out = std::max(whole(clip.loop_out), clip.loop_in);
    }
    for (auto it = clip.pins.begin(); it != clip.pins.end();) {
        Pin& p = *it;
        if (!all && !wanted(tracks, p.joint) && !wanted(tracks, "pin:" + p.joint)) {
            ++it;
            continue;
        }
        const bool inside = gone(p.from) && p.to >= 0 && gone(p.to);  // held only in removed frames
        if (inside) {
            it = clip.pins.erase(it);
            continue;
        }
        p.from = whole(p.from), p.to = whole(p.to);
        p.start_key = whole(p.start_key), p.release_key = whole(p.release_key);
        ++it;
    }
    // Keys that ran past the end extend the clip.
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track)
            if (!c.keys.empty()) clip.end_frame = std::max(clip.end_frame, int(std::ceil(c.keys.back().frame - 1e-6)));
}

auto never = [](double) { return false; };

}  // namespace

void insert_time(Clip& clip, int at, int frames, const std::vector<std::string>& tracks) {
    if (frames <= 0) return;
    if (tracks.empty()) clip.end_frame += frames;
    remap(clip, tracks, [&](double x) { return x >= at - 1e-6 ? x + frames : x; }, never);
}

void remove_time(Clip& clip, int a, int b, const std::vector<std::string>& tracks) {
    if (b <= a) return;
    const int n = b - a;
    if (tracks.empty()) clip.end_frame = std::max(1, clip.end_frame - n);
    remap(clip, tracks, [&](double x) { return x >= b - 1e-6 ? x - n : x >= a ? double(a) : x; },
          [&](double x) { return x >= a - 1e-6 && x < b - 1e-6; });
}

void scale_time(Clip& clip, int a, int b, int length, const std::vector<std::string>& tracks) {
    if (b <= a || length < 1) return;
    const double s = double(length) / (b - a);
    if (tracks.empty()) clip.end_frame = std::max(1, clip.end_frame + length - (b - a));
    remap(clip, tracks,
          [&](double x) { return x > b + 1e-6 ? x + (length - (b - a)) : x >= a - 1e-6 ? a + (x - a) * s : x; }, never);
}

KeyRange copy_range(const Clip& clip, int a, int b, const std::vector<std::string>& tracks) {
    KeyRange r;
    r.length = std::max(0, b - a);
    for (auto& [name, track] : clip.curves) {
        if (!wanted(tracks, name)) continue;
        for (auto& [ch, c] : track)
            for (Key k : c.keys)
                if (k.frame >= a - 1e-6 && k.frame <= b + 1e-6) {
                    k.frame -= a, k.lx -= a, k.rx -= a;
                    r.tracks[name][ch].keys.push_back(k);
                }
    }
    return r;
}

void paste_range(Clip& clip, const KeyRange& range, int at, bool insert, const Skeleton* mirror_with) {
    std::map<std::string, Track> src = range.tracks;
    if (mirror_with) {  // mirror the way whole clips are mirrored, so every channel rule is shared
        Clip tmp;
        tmp.curves = src;
        src = mirrored_clip(*mirror_with, tmp).curves;
    }
    std::vector<std::string> names;
    for (auto& [name, t] : src) names.push_back(name);
    if (insert) insert_time(clip, at, range.length + 1, names);
    for (auto& [name, track] : src)
        for (auto& [ch, keys] : track) {
            FCurve& c = clip.curves[name][ch];
            std::erase_if(c.keys, [&](const Key& k) { return k.frame >= at - 1e-6 && k.frame <= at + range.length + 1e-6; });
            for (Key k : keys.keys) {
                k.frame += at, k.lx += at, k.rx += at;
                c.keys.push_back(k);
            }
            std::stable_sort(c.keys.begin(), c.keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
            c.recompute_handles();
        }
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track)
            if (!c.keys.empty()) clip.end_frame = std::max(clip.end_frame, int(std::ceil(c.keys.back().frame - 1e-6)));
}

}  // namespace vats
