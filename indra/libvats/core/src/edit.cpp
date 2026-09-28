// Viewport Avatar Toolset - editing operations on a clip.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/edit.h"

#include <algorithm>

namespace vats {
namespace {

Vec3 channels(const Clip& clip, const std::string& track, double frame, const char* const (&names)[3]) {
    Vec3 v;
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return v;
    for (int a = 0; a < 3; ++a) {
        auto c = t->second.find(names[a]);
        if (c != t->second.end()) v[a] = c->second.evaluate(frame);
    }
    return v;
}

void set_channels(Clip& clip, const std::string& track, double frame, const char* const (&names)[3], const Vec3& v) {
    Track& t = clip.curves[track];
    for (int a = 0; a < 3; ++a) t[names[a]].set_key(frame, v[a]);
}

}  // namespace

Vec3 curve_euler(const Clip& clip, const std::string& track, double frame) {
    return channels(clip, track, frame, kRotChannels);
}

Vec3 curve_offset(const Clip& clip, const std::string& track, double frame) {
    return channels(clip, track, frame, kPosChannels);
}

void key_rotation(Clip& clip, const std::string& track, double frame, const Quat& rotation) {
    key_euler(clip, track, frame, nearest_euler(rotation, curve_euler(clip, track, frame)));
}

void key_euler(Clip& clip, const std::string& track, double frame, const Vec3& euler_deg) {
    set_channels(clip, track, frame, kRotChannels, euler_deg);
}

void key_offset(Clip& clip, const std::string& track, double frame, const Vec3& offset) {
    set_channels(clip, track, frame, kPosChannels, offset);
}

void key_current(Clip& clip, const Skeleton& skel, int node, double frame) {
    const std::string& name = skel[node].name;
    bool pos = skel[node].attachment || clip.has_channels(name, kPosChannels);
    Vec3 e = curve_euler(clip, name, frame), o = curve_offset(clip, name, frame);
    key_euler(clip, name, frame, e);
    if (pos) key_offset(clip, name, frame, o);
}

int delete_keys_at(Clip& clip, const std::string& track, double frame) {
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return 0;
    int n = 0;
    for (auto& [ch, curve] : t->second) n += curve.remove_key(frame);
    prune(clip);
    return n;
}

int delete_keys_at_all(Clip& clip, double frame) {
    int n = 0;
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, curve] : track) n += curve.remove_key(frame);
    prune(clip);
    return n;
}

void reset_bone(Clip& clip, const std::string& track, double frame) {
    bool pos = clip.has_channels(track, kPosChannels);
    key_euler(clip, track, frame, {});
    if (pos) key_offset(clip, track, frame, {});
}

std::vector<double> key_frames(const Clip& clip, const std::string& track) {
    std::vector<double> out;
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return out;
    for (auto& [ch, curve] : t->second)
        for (auto& k : curve.keys) out.push_back(k.frame);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return same_frame(a, b); }), out.end());
    return out;
}

bool has_key_at(const Clip& clip, const std::string& track, double frame) {
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return false;
    for (auto& [ch, curve] : t->second)
        if (curve.find(frame) >= 0) return true;
    return false;
}

void prune(Clip& clip) {
    for (auto t = clip.curves.begin(); t != clip.curves.end();) {
        for (auto c = t->second.begin(); c != t->second.end();) c = c->second.empty() ? t->second.erase(c) : std::next(c);
        t = t->second.empty() ? clip.curves.erase(t) : std::next(t);
    }
}

void keep_outer_handles(Clip& clip, const Clip& before, const std::string& track, int a, int b) {
    auto was = before.curves.find(track);
    auto now = clip.curves.find(track);
    if (was == before.curves.end() || now == clip.curves.end()) return;
    for (auto& [name, curve] : now->second) {
        auto old = was->second.find(name);
        if (old == was->second.end()) continue;
        const int ia = old->second.find(a), ib = old->second.find(b), ja = curve.find(a), jb = curve.find(b);
        if (ia >= 0 && ja >= 0) {
            const Key& k = old->second.keys[ia];
            Key& n = curve.keys[ja];
            n.left = Handle::Free, n.lx = k.lx, n.ly = k.ly;
            if (jb != ja) n.right = Handle::AutoClamped;
        }
        if (ib >= 0 && jb >= 0) {
            const Key& k = old->second.keys[ib];
            Key& n = curve.keys[jb];
            n.right = Handle::Free, n.rx = k.rx, n.ry = k.ry;
            if (jb != ja) n.left = Handle::AutoClamped;
        }
        curve.recompute_handles();
    }
}

}  // namespace vats
