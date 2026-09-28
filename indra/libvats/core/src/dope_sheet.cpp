// Viewport Avatar Toolset - the dope sheet's rows.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/dope_sheet.h"

#include <algorithm>
#include <climits>

#include "vats/pose_ops.h"

namespace vats {
namespace {

// A track's part and where it sorts: its bone's node index, or for an IK track its part's first bone.
BodyPart part_of(const Skeleton& skel, const std::string& track, int& order) {
    order = INT_MAX;
    std::string name = track.rfind("pin:", 0) == 0 ? track.substr(4) : track;
    if (name.rfind("ik.", 0) == 0) {
        for (PartKind k : {PartKind::Hand, PartKind::Arm, PartKind::Leg, PartKind::Wing, PartKind::HindLeg, PartKind::Torso})
            for (const char* side : {"Left", "Right", ""}) {
                if ((k == PartKind::Torso) != (*side == 0)) continue;
                BodyPart p = body_part(skel, k, side);
                if (std::find(p.ik_tracks.begin(), p.ik_tracks.end(), name) == p.ik_tracks.end()) continue;
                if (!p.bones.empty()) order = *std::min_element(p.bones.begin(), p.bones.end());
                return p;
            }
        return {};
    }
    int n = skel.find(name);
    if (n < 0) return {};
    order = n;
    return body_part_of(skel, n);
}

}  // namespace

std::vector<std::string> with_limb_controls(const Rig& rig, const Clip& clip, std::vector<std::string> tracks) {
    const Skeleton& skel = rig.skeleton();
    auto has = [&](const std::string& t) { return std::find(tracks.begin(), tracks.end(), t) != tracks.end(); };
    for (const LimbInfo& l : rig.limbs()) {
        const std::string ik = "ik." + l.name;
        if (has(ik) || !clip.curves.count(ik)) continue;
        for (int b : {l.root, l.mid, l.end})
            if (b >= 0 && has(skel[b].name)) {
                tracks.push_back(ik);
                break;
            }
    }
    return tracks;
}

std::vector<DopeRow> dope_rows(const Skeleton& skel, const std::vector<std::string>& tracks) {
    struct Entry {
        std::string part, track;
        int part_order, order;
    };
    std::vector<Entry> entries;
    for (const std::string& t : tracks) {
        int order = INT_MAX;
        BodyPart p = part_of(skel, t, order);
        entries.push_back({p.label.empty() ? "Other" : p.label, t, 0, order});
    }
    for (Entry& e : entries) {  // a part sorts by its earliest track
        e.part_order = INT_MAX;
        for (const Entry& o : entries)
            if (o.part == e.part) e.part_order = std::min(e.part_order, o.order);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.part_order != b.part_order) return a.part_order < b.part_order;
        if (a.part != b.part) return a.part < b.part;
        const bool ia = a.track.rfind("ik.", 0) == 0, ib = b.track.rfind("ik.", 0) == 0;  // IK controls after the bones
        if (ia != ib) return ib;
        return a.order != b.order ? a.order < b.order : a.track < b.track;
    });
    std::vector<DopeRow> rows;
    for (const Entry& e : entries) {
        if (rows.empty() || rows.back().label != e.part) rows.push_back({e.part, {}});
        if (std::find(rows.back().tracks.begin(), rows.back().tracks.end(), e.track) == rows.back().tracks.end())
            rows.back().tracks.push_back(e.track);
    }
    return rows;
}

std::vector<double> keyed_frames(const Clip& clip, const std::vector<std::string>& tracks) {
    std::vector<double> out;
    for (const std::string& t : tracks)
        if (auto it = clip.curves.find(t); it != clip.curves.end())
            for (const auto& [ch, curve] : it->second)
                for (const Key& k : curve.keys) out.push_back(k.frame);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return same_frame(a, b); }), out.end());
    return out;
}

std::vector<KeyRef> keys_between(const Clip& clip, const std::vector<std::string>& tracks, double f0, double f1) {
    std::vector<KeyRef> out;
    for (const std::string& t : tracks)
        if (auto it = clip.curves.find(t); it != clip.curves.end())
            for (const auto& [ch, curve] : it->second)
                for (int i = 0; i < int(curve.keys.size()); ++i) {
                    const double f = curve.keys[i].frame;
                    if ((f >= f0 || same_frame(f, f0)) && (f <= f1 || same_frame(f, f1))) out.push_back({t, ch, i});
                }
    return out;
}

}  // namespace vats
