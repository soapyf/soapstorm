// Viewport Avatar Toolset - foot-contact clean-up (spec 07 RT-9, 08 FC).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/footlock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace vats {

namespace {

bool is_leg(const LimbInfo& l, bool left, bool right) {
    return (left && l.name == "LegLeft") || (right && l.name == "LegRight");
}

std::vector<int> legs(const Rig& rig, const FootLockOptions& opt) {
    std::vector<int> out;
    for (int i = 0; i < int(rig.limbs().size()); ++i)
        if (is_leg(rig.limbs()[i], opt.left, opt.right)) out.push_back(i);
    return out;
}

int last_frame(const Clip& clip, const FootLockOptions& opt) {
    return opt.to < 0 ? clip.end_frame : std::min(opt.to, clip.end_frame);
}

// A leg's toe point, or -1 (heel only).
int toe_of(const Rig& rig, int limb, const FootLockOptions& opt) {
    return opt.heel_toe ? rig.skeleton().find(rig.limbs()[limb].name == "LegLeft" ? "mToeLeft" : "mToeRight") : -1;
}

// The rest pose's floor (its lowest ankle, foot or toe, as Animation Check takes it) and each point's height
// above it at rest: a point is on the ground when it is that high above it.
struct Floor {
    double z = 0;
    std::vector<double> clearance;  // per node

    Floor(const Skeleton& s, const Shape* shape) {
        const std::vector<Xform> g = s.global_pose(Pose(s.size()), shape);
        z = std::numeric_limits<double>::max();
        for (const char* n : {"mAnkleLeft", "mAnkleRight", "mFootLeft", "mFootRight", "mToeLeft", "mToeRight"})
            if (int i = s.find(n); i >= 0) z = std::min(z, g[i].pos.z);
        if (z == std::numeric_limits<double>::max()) z = 0;
        for (auto& x : g) clearance.push_back(x.pos.z - z);
    }
};

std::vector<std::vector<Xform>> sample(const Rig& rig, const Clip& clip, int a, int b, const Shape* shape) {
    std::vector<std::vector<Xform>> g;
    for (int f = a; f <= b; ++f) g.push_back(evaluate(rig, clip, f, shape).globals);
    return g;
}

// The lowest sole point over the samples (absolute z).
double ground_z(const Rig& rig, const FootLockOptions& opt, const Floor& fl, const std::vector<std::vector<Xform>>& g) {
    double z = std::numeric_limits<double>::max();
    for (int limb : legs(rig, opt))
        for (int p : {rig.limbs()[limb].end, toe_of(rig, limb, opt)})
            if (p >= 0)
                for (auto& x : g) z = std::min(z, x[p].pos.z - fl.clearance[p]);
    return z == std::numeric_limits<double>::max() ? fl.z : z;
}

// Moves every key of a track's channel by dz (values and handles), or keys dz at frame 0 when it has none.
void shift_channel(Clip& c, const std::string& track, const char* channel, double dz, bool add_if_missing) {
    auto t = c.curves.find(track);
    if (t == c.curves.end() && !add_if_missing) return;
    FCurve& z = c.curves[track][channel];
    if (z.empty()) {
        if (add_if_missing) z.set_key(0, dz);
        return;
    }
    for (Key& k : z.keys) k.value += dz, k.ly += dz, k.ry += dz;
}

// One stance of one leg: frames from..to where a point is down, ramps rin/rout either side.
struct Stance {
    int from, to, rin, rout;
};

}  // namespace

std::vector<FootContact> find_foot_contacts(const Rig& rig, const Clip& clip, const FootLockOptions& opt) {
    const int a = std::max(0, opt.from), b = last_frame(clip, opt);
    std::vector<FootContact> out;
    if (b <= a) return out;
    const Floor fl(rig.skeleton(), opt.shape);
    const auto g = sample(rig, clip, a, b, opt.shape);
    const double ground = ground_z(rig, opt, fl, g);
    const double fps = std::max(clip.fps, 1);
    const int n = int(g.size());
    for (int limb : legs(rig, opt))
        for (int p : {rig.limbs()[limb].end, toe_of(rig, limb, opt)}) {
            if (p < 0) continue;
            const bool toe = p != rig.limbs()[limb].end;
            bool on = false;
            int start = 0;
            auto close = [&](int end) {
                if (end - start + 1 >= opt.min_frames) out.push_back({limb, a + start, a + end, toe});
            };
            for (int i = 0; i < n; ++i) {
                const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, n - 1);
                const double speed = i1 > i0 ? (g[i1][p].pos - g[i0][p].pos).length() / (i1 - i0) * fps : 0;
                const double h = g[i][p].pos.z - fl.clearance[p] - ground;
                if (!on && h <= opt.height && speed <= opt.speed) on = true, start = i;
                else if (on && (h > opt.height * 1.5 || speed > opt.speed * 2)) on = false, close(i - 1);
            }
            if (on) close(n - 1);
        }
    return out;
}

double foot_ground(const Rig& rig, const Clip& clip, const FootLockOptions& opt) {
    const int a = std::max(0, opt.from), b = std::max(a, last_frame(clip, opt));
    const Floor fl(rig.skeleton(), opt.shape);
    return ground_z(rig, opt, fl, sample(rig, clip, a, b, opt.shape)) - fl.z;
}

std::vector<std::string> lock_feet(Clip& clip, const Rig& rig, const FootLockOptions& opt) {
    std::vector<std::string> report;
    char buf[160];
    const std::vector<FootContact> contacts = find_foot_contacts(rig, clip, opt);
    const int a = std::max(0, opt.from), b = std::max(a, last_frame(clip, opt));
    const Floor fl(rig.skeleton(), opt.shape);
    // The pose as animated, before any keys change: poles (the knee keeps pointing where it did), the foot's
    // turn and the hips' height come from it.
    auto fk = sample(rig, clip, 0, b, opt.shape);
    double ground = ground_z(rig, opt, fl, {fk.begin() + a, fk.end()});
    std::snprintf(buf, sizeof buf, "ground: %.1f cm %s the floor", std::fabs(ground - fl.z) * 100,
                  ground >= fl.z ? "above" : "below");
    std::string ground_line = buf;
    if (opt.to_ground && std::fabs(ground - fl.z) > 1e-4) {
        // As Animation Check's Drop the Hips: the pelvis, and leg IK targets keyed in avatar space.
        const double dz = fl.z - ground;
        shift_channel(clip, "mPelvis", "pos_z", dz, true);
        for (int limb : legs(rig, opt))
            if (uses_ik(clip, rig.limbs()[limb]))
                for (const char* ch : {"pos_z", "pole_z"}) shift_channel(clip, "ik." + rig.limbs()[limb].name, ch, dz, false);
        std::snprintf(buf, sizeof buf, "; feet put on the ground (pelvis moved %s %.1f cm)", dz < 0 ? "down" : "up",
                      std::fabs(dz) * 100);
        ground_line += buf;
        ground = fl.z;
        fk = sample(rig, clip, 0, b, opt.shape);
    }

    // Per leg, per frame: the ankle target that holds the planted point (held = keyed).
    struct Plan {
        int limb;
        std::vector<Stance> stances;
        std::vector<Vec3> pos;
        std::vector<char> held;  // 2 in a contact, 1 in a ramp
    };
    std::vector<Plan> plans;
    for (int limb : legs(rig, opt)) {
        const LimbInfo& l = rig.limbs()[limb];
        std::vector<char> heel(b + 1, 0), toe(b + 1, 0);
        int heels = 0, toes = 0;
        for (auto& c : contacts)
            if (c.limb == limb) {
                for (int f = c.from; f <= c.to; ++f) (c.toe ? toe : heel)[f] = 1;
                ++(c.toe ? toes : heels);
            }
        if (!heels && !toes) continue;
        // IK the user keyed in the range wins; blend keys elsewhere (an earlier take's lock) are fine.
        auto has_blend = [&] {
            auto t = clip.curves.find("ik." + l.name);
            if (t == clip.curves.end() || !t->second.count("blend")) return false;
            for (const Key& k : t->second.at("blend").keys)
                if (k.frame >= opt.from - opt.blend && k.frame <= b + opt.blend) return true;
            return false;
        };
        if (has_blend()) {
            report.push_back(l.label + ": already uses IK here, left as it is");
            continue;
        }
        Plan plan{limb, {}, std::vector<Vec3>(b + 1), std::vector<char>(b + 1, 0)};
        for (int f = a; f <= b; ++f) {
            if (!heel[f] && !toe[f]) continue;
            int e = f;
            while (e < b && (heel[e + 1] || toe[e + 1])) ++e;
            plan.stances.push_back({f, e, 0, 0});
            f = e;
        }
        // Ramps shrink to fit the gaps between stances.
        for (size_t s = 0; s < plan.stances.size(); ++s) {
            Stance& st = plan.stances[s];
            const int gap_before = s ? st.from - plan.stances[s - 1].to - 1 : st.from - a;
            const int gap_after = s + 1 < plan.stances.size() ? plan.stances[s + 1].from - st.to - 1 : b - st.to;
            st.rin = std::clamp(gap_before / 2, 0, opt.blend), st.rout = std::clamp(gap_after / 2, 0, opt.blend);
        }
        const int ankle = l.end, tp = toe_of(rig, limb, opt);
        auto off = [&](int f) { return tp >= 0 ? fk[f][tp].pos - fk[f][ankle].pos : Vec3{}; };
        auto snap = [&](Vec3 p, int node) { return p.z = ground + fl.clearance[node], p; };
        auto sole = [&](int f, int node) { return fk[f][node].pos.z - fl.clearance[node]; };
        // The ankle target that holds the heel (the ankle itself) or the toe at p while the foot turns as animated.
        auto place = [&](bool heel_held, const Vec3& p, int f) { return heel_held ? p : p - off(f); };
        for (const Stance& st : plan.stances) {
            // One point holds the foot at a time: the one down, or with both down the lower (1 mm in favour of
            // the one holding), so a heel-toe roll hands over from heel to toe. The point taking over is planted
            // where the other one's hold has it, so the hand-over does not jump (and is not snapped again).
            bool heel_held = true, first_heel = true;
            Vec3 plant, first_plant;
            for (int f = st.from; f <= st.to; ++f) {
                const bool h = heel[f], t = toe[f];
                const bool want = h && (!t || (heel_held ? sole(f, ankle) <= sole(f, tp) + 0.001
                                                         : sole(f, ankle) < sole(f, tp) - 0.001));
                if (f == st.from) {
                    plant = snap(want ? fk[f][ankle].pos : fk[f][tp].pos, want ? ankle : tp);
                    first_heel = want, first_plant = plant;
                } else if (want != heel_held) {
                    // Where the old hold had the new point a frame ago, while the foot still turned about the old one.
                    const Vec3 held = place(heel_held, plant, f - 1);
                    plant = want ? held : held + off(f - 1);
                }
                heel_held = want;
                plan.pos[f] = place(heel_held, plant, f);
                plan.held[f] = 2;
            }
            // The ramps keep the first and last hold.
            for (int f = st.from - st.rin; f < st.from; ++f) plan.pos[f] = place(first_heel, first_plant, f), plan.held[f] = 1;
            for (int f = st.to + 1; f <= st.to + st.rout; ++f) plan.pos[f] = place(heel_held, plant, f), plan.held[f] = 1;
        }
        if (tp >= 0) std::snprintf(buf, sizeof buf, "%s: %d heel and %d toe contacts held still", l.label.c_str(), heels, toes);
        else std::snprintf(buf, sizeof buf, "%s: %d foot contact%s held still", l.label.c_str(), heels, heels == 1 ? "" : "s");
        report.push_back(buf);
        plans.push_back(std::move(plan));
    }
    if (report.empty()) report.push_back("no foot contacts found");
    report.push_back(ground_line);
    if (plans.empty()) return report;

    // Out of reach: how far the hips must come down in each contact frame for the knee to stay bent at least
    // kMinKneeBendDeg. (Ramps only part blend the IK in, so they ask for nothing.)
    std::vector<double> need(b + 1, 0);
    const Skeleton& sk = rig.skeleton();
    const Pose rest(sk.size());
    for (const Plan& p : plans) {
        const LimbInfo& l = rig.limbs()[p.limb];
        // The hip-ankle distance at that bend, as the solver bends the knee: about its hinge from rest.
        const Vec3 e = sk.local_xform(l.mid, rest, opt.shape).pos, w = sk.local_xform(l.end, rest, opt.shape).pos;
        const double reach = (e + Quat::axis_angle(l.hinge, kMinKneeBendDeg * kDegToRad).rotate(w)).length();
        for (int f = 0; f <= b; ++f) {
            if (p.held[f] != 2) continue;
            const Vec3 hip = fk[f][l.root].pos, t = p.pos[f];
            const double reach2 = reach * reach;
            const double h2 = (hip.x - t.x) * (hip.x - t.x) + (hip.y - t.y) * (hip.y - t.y);
            need[f] = std::max(need[f], hip.z - t.z - std::sqrt(std::max(reach2 - h2, 0.0)));
        }
    }
    // Smoothed: the running maximum, then the running mean, over the same window, so it never falls below need.
    const int r = std::max(2, clip.fps / 6);
    std::vector<double> peak(b + 1, 0), drop(b + 1, 0);
    for (int f = 0; f <= b; ++f)
        for (int i = std::max(0, f - r); i <= std::min(b, f + r); ++i) peak[f] = std::max(peak[f], need[i]);
    for (int f = 0; f <= b; ++f) {
        for (int i = f - r; i <= f + r; ++i) drop[f] += i >= 0 && i <= b ? peak[i] : 0;
        drop[f] /= 2 * r + 1;
    }
    // Where the smoothing spills past the need it does not take a foot that is not down through the floor.
    for (int f = 0; f <= b; ++f) {
        double room = std::numeric_limits<double>::max();
        for (int limb : legs(rig, opt)) {
            bool down = false;
            for (const Plan& p : plans) down |= p.limb == limb && p.held[f] == 2;
            if (down) continue;
            for (int pt : {rig.limbs()[limb].end, toe_of(rig, limb, opt)})
                if (pt >= 0) room = std::min(room, std::max(0.0, fk[f][pt].pos.z - fl.clearance[pt] - ground));
        }
        drop[f] = std::max(need[f], std::min(drop[f], room));
    }
    double deepest = 0;
    int lowered = 0;
    {
        FCurve before = clip.curves["mPelvis"]["pos_z"];
        FCurve& z = clip.curves["mPelvis"]["pos_z"];
        for (int f = 0; f <= b; ++f) {
            if (drop[f] > 1e-5) {
                z.set_key(f, before.evaluate(f) - drop[f]);
                deepest = std::max(deepest, drop[f]), ++lowered;
            } else if ((f > 0 && drop[f - 1] > 1e-5) || (f < b && drop[f + 1] > 1e-5)) {
                z.set_key(f, before.evaluate(f));  // anchors the curve either side
            }
        }
        if (z.empty()) clip.curves["mPelvis"].erase("pos_z");
    }
    if (lowered) {
        std::snprintf(buf, sizeof buf, "pelvis lowered by up to %.1f cm on %d frames where a leg could not reach",
                      deepest * 100, lowered);
        report.push_back(buf);
        fk = sample(rig, clip, 0, b, opt.shape);
    } else {
        report.push_back("pelvis not lowered: every held foot was in reach");
    }

    for (const Plan& p : plans) {
        const LimbInfo& l = rig.limbs()[p.limb];
        for (int f = 0; f <= b; ++f) {
            if (!p.held[f]) continue;
            key_limb_target(clip, rig, f, p.limb, {fk[f][l.end].rot, p.pos[f]}, opt.shape);
            key_limb_pole(clip, rig, f, p.limb, derive_pole(rig, p.limb, fk[f]), opt.shape);
        }
        FCurve& blend = clip.curves["ik." + l.name]["blend"];
        for (const Stance& st : p.stances) {
            if (st.from - st.rin > 0) blend.set_key(st.from - st.rin - (st.rin ? 0 : 1), 0, Interp::Linear);
            blend.set_key(st.from, 1, Interp::Linear);
            blend.set_key(st.to, 1, Interp::Linear);
            if (st.to + st.rout < clip.end_frame) blend.set_key(st.to + st.rout + (st.rout ? 0 : 1), 0, Interp::Linear);
        }
    }
    return report;
}

}  // namespace vats
