// Viewport Avatar Toolset - posing assists: live mirror, scratch pose, propagate pose and the graph's curve buffer.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/pose_tools.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vats/edit.h"
#include "vats/footlock.h"
#include "vats/pose_ops.h"
#include "vats/ragdoll.h"
#include "vats/rig.h"

namespace vats {
namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

Quat mirror_q(const Quat& q) { return {q.w, -q.x, q.y, -q.z}; }
Vec3 mirror_v(const Vec3& v) { return {v.x, -v.y, v.z}; }

// A finger segment's curl axis: across the palm from the bone (the thumb's palm side slants toward the fingers).
Vec3 curl_axis(const Skeleton& skel, int node) {
    const bool thumb = skel[node].name.find("Thumb") != std::string::npos;
    const Vec3 palm = thumb ? Vec3{-0.7, 0, -1}.normalized() : Vec3{0, 0, -1};
    return skel[node].end.normalized().cross(palm).normalized();
}

}  // namespace

Quat finger_segment_rotation(const Skeleton& skel, int node, const Quat& start, double curl_deg, double spread_deg,
                             bool first, const JointLimit* limit, const Shape* shape) {
    // Past half a turn a curl reads as a bend the other way (a long drag down came out bent back), so it stops short.
    curl_deg = std::clamp(curl_deg, -170.0, 170.0), spread_deg = std::clamp(spread_deg, -90.0, 90.0);
    Quat q = start * Quat::axis_angle(curl_axis(skel, node), curl_deg * kDegToRad);  // curl in the segment's own frame
    if (first && spread_deg != 0) {
        const bool right = skel[node].name.ends_with("Right");
        const Vec3 side = right ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
        q = Quat::axis_angle(skel[node].end.normalized().cross(side), spread_deg * kDegToRad) * q;
    }
    // Respect Joint Limits: a long drag folded a finger back through the hand.
    return limit ? clamp_joint_rotation(*limit, q.normalized(), shape, node) : q;
}

double finger_curl_degrees(const Skeleton& skel, const Clip& clip, const std::vector<std::string>& segments, double frame) {
    double total = 0;
    for (const std::string& bone : segments) {
        const int node = skel.find(bone);
        if (node < 0) continue;
        const Vec3 axis = curl_axis(skel, node);
        Quat swing, twist;
        decompose_swing_twist(euler_to_quat(curve_euler(clip, bone, frame)), axis, swing, twist);
        double a = 2 * std::atan2(twist.x * axis.x + twist.y * axis.y + twist.z * axis.z, twist.w) * kRadToDeg;
        if (a > 180) a -= 360;
        if (a < -180) a += 360;
        total += a;
    }
    return total;
}

namespace {

// The lowest point the body sits on at a pose: the bottom of the pelvis's and thighs' capsules.
double seat_bottom(const Skeleton& skel, const std::vector<Xform>& globals) {
    double low = 1e300;
    for (const RagdollCapsule& c : ragdoll_capsules(skel, globals)) {
        const std::string& n = skel[c.node].name;
        if (n != "mPelvis" && n != "mHipLeft" && n != "mHipRight") continue;
        for (const auto& [a, b] : c.segments) low = std::min(low, std::min(a.z, b.z) - c.radius);
    }
    return low;
}

}  // namespace

std::vector<Vec3> thigh_points(const Rig& rig, const Clip& clip, double frame, const Shape* shape) {
    const Skeleton& skel = rig.skeleton();
    const Evaluation e = evaluate(rig, clip, frame, shape);
    std::vector<Vec3> out;
    for (const char* side : {"Left", "Right"}) {
        const int hip = skel.find(std::string("mHip") + side), knee = skel.find(std::string("mKnee") + side);
        if (hip >= 0 && knee >= 0)
            for (double t : {0.0, 0.25, 0.5, 0.75}) {
                const Vec3 a = e.globals[size_t(hip)].pos, b = e.globals[size_t(knee)].pos;
                out.push_back(a + (b - a) * t);
            }
    }
    return out;
}

bool sit_on_seat(Clip& clip, const Rig& rig, double frame, double seat_z, const Shape* shape, std::string& report) {
    const Skeleton& skel = rig.skeleton();
    auto raise = [&](double dz) { key_offset(clip, "mPelvis", frame, curve_offset(clip, "mPelvis", frame) + Vec3{0, 0, dz}); };
    // The feet onto the floor first, as the Animation Check's Drop the Hips does.
    const double drop = sole_height(skel, evaluate(rig, clip, frame, shape).globals) - sole_floor(skel, shape);
    raise(-drop);
    // Held there, so the hips can move and the knees bend (Hold in World from Here). A foot already pinned stays so.
    for (const char* ankle : {"mAnkleLeft", "mAnkleRight"}) {
        const int node = skel.find(ankle);
        if (node >= 0 && pin_at(clip, rig, node, frame) < 0 && !pin_here(clip, rig, frame, node, -1, shape, report))
            return false;
    }
    // The thighs onto the seat.
    const double up = seat_z - seat_bottom(skel, evaluate(rig, clip, frame, shape).globals);
    raise(up);
    char buf[160];
    std::snprintf(buf, sizeof buf, "the hips %s %.0f cm onto the seat at %.0f cm, the feet held on the floor",
                  up - drop < 0 ? "down" : "up", std::fabs(up - drop) * 100, (seat_z - sole_floor(skel, shape)) * 100);
    report = buf;
    return true;
}

void mirror_live(Clip& clip, const Skeleton& skel, double frame, const std::vector<std::string>& tracks,
                 bool centre_in_place) {
    // No partner written is itself an edited track, so every source reads the same before and after (no copy).
    const Clip& before = clip;
    auto edited = [&](const std::string& t) { return std::find(tracks.begin(), tracks.end(), t) != tracks.end(); };
    for (const std::string& src : tracks) {
        if (starts_with(src, "ik.")) {
            const std::string dst = Skeleton::mirror_name(src);
            if (dst == src || edited(dst)) continue;
            auto t = before.curves.find(src);
            if (t == before.curves.end()) continue;
            for (auto& [ch, c] : t->second)  // pos and pole per channel; blend stays the partner's own
                if (!c.empty() && (starts_with(ch, "pos_") || starts_with(ch, "pole_")))
                    clip.curves[dst][ch].set_key(frame, mirror_flips(ch) ? -c.evaluate(frame) : c.evaluate(frame));
            if (before.has_channels(src, kRotChannels))
                key_rotation(clip, dst, frame, mirror_q(euler_to_quat(curve_euler(before, src, frame))));
            continue;
        }
        const int s = skel.find(src);
        if (s < 0 || skel.reused(s)) continue;  // pins and unknown tracks; a reused bone keeps its own motion
        const int d = skel.mirror(s);
        const Quat rot = euler_to_quat(curve_euler(before, src, frame));
        const Vec3 off = curve_offset(before, src, frame);
        const bool pos = before.has_channels(src, kPosChannels);
        if (d == s) {
            if (!centre_in_place) continue;
            key_rotation(clip, src, frame, nlerp(rot, mirror_rotation(skel, s, s, rot), 0.5));
            if (pos) key_offset(clip, src, frame, {off.x, 0, off.z});
            continue;
        }
        const std::string& dn = skel[d].name;
        if (edited(dn)) continue;
        key_rotation(clip, dn, frame, mirror_rotation(skel, s, d, rot));
        if (pos || skel[d].attachment || before.has_channels(dn, kPosChannels)) key_offset(clip, dn, frame, mirror_v(off));
    }
}

std::vector<std::string> scratch_tracks(const Clip& base, const Clip& working) {
    std::vector<std::string> out;
    for (auto& [name, track] : working.curves) {
        auto b = base.curves.find(name);
        if (b == base.curves.end() || b->second != track) out.push_back(name);
    }
    for (auto& [name, track] : base.curves)
        if (!working.curves.count(name)) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

Clip scratch_commit(const Clip& base, const Clip& working, double frame, bool only_existing) {
    Clip out = working;
    out.curves = base.curves;
    for (auto& [name, track] : working.curves)
        for (auto& [ch, c] : track) {
            const FCurve* was = nullptr;
            if (auto t = base.curves.find(name); t != base.curves.end())
                if (auto k = t->second.find(ch); k != t->second.end()) was = &k->second;
            if (c.empty() || (was && *was == c) || (only_existing && (!was || was->empty()))) continue;
            out.curves[name][ch].set_key(frame, c.evaluate(frame));
        }
    return out;
}

int propagate_pose(Clip& clip, const std::vector<std::string>& tracks, double frame, PropagateTo to, double a,
                   double b) {
    int n = 0;
    for (const std::string& name : tracks) {
        auto t = clip.curves.find(name);
        if (t == clip.curves.end()) continue;
        double lo = std::max(a, frame), hi = b;
        if (to != PropagateTo::Range) {
            lo = frame, hi = 1e300;
            if (to == PropagateTo::NextKey) {
                const std::vector<double> frames = key_frames(clip, name);
                auto next = std::find_if(frames.begin(), frames.end(), [&](double f) { return f > frame && !same_frame(f, frame); });
                if (next == frames.end()) continue;
                hi = *next;
            }
        }
        for (auto& [ch, c] : t->second) {
            if (c.empty() || ch == "blend") continue;  // an ik track's IK/FK state is not pose
            const double v = c.evaluate(frame);
            std::vector<double> targets;
            for (const Key& k : c.keys)
                if (k.frame >= lo - 1e-9 && k.frame <= hi + 1e-9 && !same_frame(k.frame, frame) && k.value != v)
                    targets.push_back(k.frame);
            for (double f : targets) c.set_key(f, v);
            n += int(targets.size());
        }
    }
    return n;
}

const FCurve* CurveBuffer::curve(const std::string& track, const std::string& channel) const {
    if (!curves) return nullptr;
    auto t = curves->find(track);
    if (t == curves->end()) return nullptr;
    auto c = t->second.find(channel);
    return c == t->second.end() ? nullptr : &c->second;
}

}  // namespace vats
