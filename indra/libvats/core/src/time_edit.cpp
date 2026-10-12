// Viewport Avatar Toolset - editing time (spec 08 TE).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/time_edit.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "vats/curve_ops.h"
#include "vats/pose_ops.h"

namespace vats {
namespace {

bool wanted(const std::vector<std::string>& tracks, const std::string& name) {
    return tracks.empty() || std::find(tracks.begin(), tracks.end(), name) != tracks.end();
}

// Moves every edited key through `map`; keys for which `gone` is true are deleted first.
void remap(Clip& clip, const std::vector<std::string>& tracks, const std::function<double(double)>& map,
           const std::function<bool(double)>& gone) {
    for_each_track_map(clip, [&](std::map<std::string, Track>& curves) {
        for (auto& [name, track] : curves) {
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
    });
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

// Automatic handles take their slope from the neighbours; frozen, they stay as they are when a neighbour goes.
static void freeze_handles(Key& k) {
    for (Handle* h : {&k.left, &k.right}) {
        if (*h == Handle::Vector) *h = Handle::Free;
        else if (*h != Handle::Aligned && *h != Handle::Free) *h = Handle::Aligned;
    }
}

// A key at frame on c that leaves the curve's shape as it was once the keys beyond it are gone: insert_on_curve
// keeps the shape and freezes the neighbours; a key already there is frozen too.
static void key_keeping_shape(FCurve& c, double frame) {
    if (const int i = c.find(frame); i >= 0) return freeze_handles(c.keys[size_t(i)]);
    insert_on_curve(c, frame);
}

void crop_time(Clip& clip, int a, int b) {
    a = std::max(a, 0), b = std::min(b, clip.end_frame);
    if (b <= a) return;
    for_each_track_map(clip, [&](std::map<std::string, Track>& curves) {
        for (auto& [name, track] : curves)
            for (auto& [ch, c] : track)
                if (!c.empty()) key_keeping_shape(c, a), key_keeping_shape(c, b);
    });
    std::vector<Pin> pins = std::move(clip.pins);  // remap would drop a pin held across the whole of a..b
    clip.pins.clear();
    // The edge test matches find()'s (same_frame), so a key taken as the one at a or b stays.
    remap(clip, {}, [&](double x) { return x - a; },
          [&](double x) { return (x < a && !same_frame(x, a)) || (x > b && !same_frame(x, b)); });
    const int end = b - a;
    clip.end_frame = end;
    clip.loop_in = std::clamp(clip.loop_in, 0, end);
    clip.loop_out = std::clamp(clip.loop_out, clip.loop_in, end);
    // Pins held at some frame of a..b stay, clipped to it; a helper key VATs made outside is gone with its frame.
    for (Pin& p : pins) {
        if (p.from > b || (p.to >= 0 && p.to < a)) continue;
        p.from = std::max(p.from - a, 0);
        if (p.to >= 0) p.to = std::min(p.to - a, end);
        for (int* k : {&p.start_key, &p.release_key}) *k = *k < a || *k > b ? -1 : *k - a;
        clip.pins.push_back(std::move(p));
    }
    // Everything else placed on the timeline moves with the keys.
    if (clip.audio) clip.audio->offset -= double(a) / std::max(clip.fps, 1);
    if (clip.reference) clip.reference->frame_offset -= a;
    if (clip.ragdoll) {
        Ragdoll& r = *clip.ragdoll;
        const int from = std::clamp(r.start - a, 0, end), to = std::clamp(r.start + r.frames - a, 0, end);
        r.start = from, r.frames = to - from;
    }
    if (clip.lip_sync && (clip.lip_sync->to < a || clip.lip_sync->from > b)) clip.lip_sync.reset();  // all cut away
    if (clip.lip_sync) {  // each cue holds until the next: the one holding at a starts the cropped track
        LipSync& ls = *clip.lip_sync;
        std::vector<LipSync::Cue> cues;
        for (const LipSync::Cue& q : ls.cues) {
            if (q.frame > b) break;
            if (q.frame <= a) cues.clear();
            cues.push_back({std::max(q.frame - a, 0), q.shape});
        }
        ls.cues = std::move(cues);
        const int from = std::clamp(ls.from, a, b), to = std::clamp(ls.to, from, b);
        if (!ls.level.empty()) {  // one value per frame from..to
            const size_t i = size_t(from - ls.from), n = size_t(to - from + 1);
            ls.level = i < ls.level.size()
                           ? std::vector<double>(ls.level.begin() + i, ls.level.begin() + std::min(ls.level.size(), i + n))
                           : std::vector<double>{};
        }
        ls.from = from - a, ls.to = to - a;
    }
}

void scale_time(Clip& clip, int a, int b, int length, const std::vector<std::string>& tracks) {
    if (length < 1) return;
    scale_time_to(clip, a, b, a + length, tracks);
}

void scale_time_to(Clip& clip, double a, double b, double to, const std::vector<std::string>& tracks) {
    if (b <= a || to <= a) return;
    const double s = (to - a) / (b - a), d = to - b;
    if (tracks.empty()) clip.end_frame = std::max(1, int(std::lround(clip.end_frame + d)));
    remap(clip, tracks, [&](double x) { return x > b + 1e-6 ? x + d : x >= a - 1e-6 ? a + (x - a) * s : x; }, never);
}

void scale_length_with_keys(Clip& clip, const Clip& at_press, double pivot, double sx) {
    auto map = [&](int f) { return std::max(0, int(std::lround(pivot + (f - pivot) * sx))); };
    clip.end_frame = std::max(1, map(at_press.end_frame));
    clip.loop_out = std::min(map(at_press.loop_out), clip.end_frame);
    clip.loop_in = std::min(map(at_press.loop_in), clip.loop_out);
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
