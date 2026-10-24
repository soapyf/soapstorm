// Viewport Avatar Toolset - hands against the body's own mesh (see vats/mesh_contact.h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/mesh_contact.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>

#include "vats/edit.h"

namespace vats {
namespace {

constexpr double kCell = 0.04;    // m: the grid the body's vertices are sorted into
constexpr double kReach = 0.12;   // m: a body vertex further than this from a hand vertex says nothing about it
constexpr double kMargin = 0.005; // m: a pushed hand ends this far outside
constexpr int kPasses = 6;        // keying moves the curves around the keys, and the hand's new place can touch elsewhere
constexpr double kShallow = 0.03; // m: deeper than this, the nearest surface may be the far side the hand went through
const char* kSide[2] = {"Left", "Right"};

// The skeleton node a skin index rides with: a joint, the root, or a collision volume's joint.
int node_of(const Skeleton& skel, int j) {
    if (j >= 0 && j < skel.size()) return j;
    const int v = j - skel.size() - 1;
    return v >= 0 && v < int(skel.volumes().size()) ? skel.volumes()[size_t(v)].joint : -1;
}

double distance_to_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const double len2 = ab.dot(ab);
    const double t = len2 > 1e-12 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
    return (p - (a + ab * t)).length();
}

bool below(const Skeleton& skel, int node, int top) {
    for (int n = node; n >= 0; n = skel[n].parent)
        if (n == top) return true;
    return false;
}

std::int64_t cell_key(const Vec3& p) {
    const auto c = [](double x) { return std::int64_t(std::floor(x / kCell)) + (1 << 20); };
    return (c(p.x) << 42) | (c(p.y) << 21) | c(p.z);
}

}  // namespace

MeshContactCheck::MeshContactCheck(const Skeleton& skel, const std::vector<const DaeModel*>& parts, const Shape* shape)
    : skel_(skel), shape_(shape) {
    int collar[2], elbow[2];
    for (int s = 0; s < 2; ++s) {
        collar[s] = skel.find(std::string("mCollar") + kSide[s]), elbow[s] = skel.find(std::string("mElbow") + kSide[s]);
        if (collar[s] < 0 || elbow[s] < 0) return;
    }
    for (const DaeModel* m : parts) {  // the vertices of all rigged parts, one after another
        const size_t n = m ? m->positions.size() / 3 : 0;
        if (!m || !m->rigged || m->joints.size() < n * 4 || m->normals.size() != n * 3) continue;
        parts_.push_back(m);
        for (size_t v = 0; v < n; ++v) {
            const int node = node_of(skel, m->joints[v * 4]);  // the largest influence
            signed char own = -1, arm = -1;
            for (int s = 0; s < 2 && node >= 0; ++s) {
                if (below(skel, node, collar[s])) own = static_cast<signed char>(s);
                if (below(skel, node, elbow[s])) arm = static_cast<signed char>(s);
            }
            own_.push_back(own), arm_.push_back(arm), node_.push_back(node);
        }
    }
    usable_ = !parts_.empty();
}

HandContacts MeshContactCheck::measure(const std::vector<Xform>& globals) const {
    HandContacts out;
    if (!usable_) return out;
    std::vector<float> pos, nrm, skin_pos, skin_nrm;
    for (const DaeModel* m : parts_) {
        skin_prop(*m, skel_, globals, shape_, skin_pos, skin_nrm);
        pos.insert(pos.end(), skin_pos.begin(), skin_pos.end()), nrm.insert(nrm.end(), skin_nrm.begin(), skin_nrm.end());
    }
    const size_t n = arm_.size();
    auto at = [&](const std::vector<float>& a, size_t v) { return Vec3{a[v * 3], a[v * 3 + 1], a[v * 3 + 2]}; };
    for (int s = 0; s < 2; ++s) {
        // The body near this arm: its vertices inside the arm's box grown by the reach, sorted into grid cells.
        Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
        bool any = false;
        for (size_t v = 0; v < n; ++v)
            if (arm_[v] == s) {
                const Vec3 p = at(pos, v);
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                any = true;
            }
        if (!any) continue;
        std::vector<std::pair<std::int64_t, std::uint32_t>> grid;
        for (size_t v = 0; v < n; ++v) {
            if (own_[v] == s) continue;
            const Vec3 p = at(pos, v);
            if (p.x < lo.x - kReach || p.y < lo.y - kReach || p.z < lo.z - kReach || p.x > hi.x + kReach ||
                p.y > hi.y + kReach || p.z > hi.z + kReach)
                continue;
            grid.emplace_back(cell_key(p), std::uint32_t(v));
        }
        if (grid.empty()) continue;
        std::sort(grid.begin(), grid.end());
        auto nearest = [&](const Vec3& q, int rings) {  // the closest body vertex within rings cells, or -1
            long best = -1;
            double best_d = 1e30;
            for (int dx = -rings; dx <= rings; ++dx)
                for (int dy = -rings; dy <= rings; ++dy)
                    for (int dz = -rings; dz <= rings; ++dz) {
                        const std::int64_t key = cell_key({q.x + dx * kCell, q.y + dy * kCell, q.z + dz * kCell});
                        auto it = std::lower_bound(grid.begin(), grid.end(), std::make_pair(key, std::uint32_t(0)));
                        for (; it != grid.end() && it->first == key; ++it) {
                            const double d = (at(pos, it->second) - q).length();
                            if (d < best_d) best_d = d, best = long(it->second);
                        }
                    }
            return best_d <= kReach ? best : -1;
        };
        // Each forearm and hand vertex behind the surface of the body vertex nearest it is inside, that far.
        std::vector<std::pair<double, Vec3>> hits;  // depth, the body's normal there
        for (size_t v = 0; v < n; ++v) {
            if (arm_[v] != s) continue;
            const Vec3 q = at(pos, v);
            // The nearest within one cell is certain; one found further out may have a nearer one a ring beyond.
            long b = nearest(q, 1);
            const double d1 = b >= 0 ? (at(pos, size_t(b)) - q).length() : kReach;
            if (b < 0 || d1 > kCell) b = nearest(q, int(std::ceil(d1 / kCell)));
            if (b < 0) continue;
            const Vec3 vb = at(pos, size_t(b)), nb = at(nrm, size_t(b)).normalized();
            const double depth = (vb - q).dot(nb);
            if (depth <= 0) continue;
            // Inside the body there: nearer the bone that carries the surface than the surface itself is.
            const int bone = node_[size_t(b)];
            if (bone < 0) continue;
            const Vec3 a = globals[size_t(bone)].pos, e = bone_tail(skel_, globals, shape_, bone);
            if (distance_to_segment(q, a, e) >= distance_to_segment(vb, a, e)) continue;
            hits.emplace_back(depth, nb);
        }
        if (hits.empty()) continue;
        // The deepest few say how deep and which way out (one stray vertex does not decide the direction).
        std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        Vec3 dir;
        const size_t k = std::min<size_t>(hits.size(), 8);
        for (size_t i = 0; i < k; ++i) dir = dir + hits[i].second * hits[i].first;
        out.side[s].depth = hits.front().first;
        out.side[s].push = dir.length() > 1e-9 ? dir.normalized() : hits.front().second;
    }
    return out;
}

PushOutReport push_hands_out(Clip& clip, const Rig& rig, const std::vector<const DaeModel*>& parts, const Shape* shape,
                             int from, int to, const RigConstraints* constraints, double tol, double ease_s) {
    PushOutReport rep;
    const Skeleton& skel = rig.skeleton();
    const MeshContactCheck check(skel, parts, shape);
    from = std::max(from, 0), to = std::min(to, clip.end_frame);
    if (!check.usable() || to < from) return rep;
    const int ease = std::max(1, int(std::lround(ease_s * std::max(clip.fps, 1))));
    std::vector<bool> moved_on[2] = {std::vector<bool>(size_t(to - from + 1)), std::vector<bool>(size_t(to - from + 1))};
    for (int pass = 0; pass < kPasses; ++pass) {
        // Measure: 20 times a second (the pushes are eased over more than that), the frames shared out to threads (a
        // large body skinned every time costs tens of milliseconds); a frame between two samples takes the deeper.
        const int step = std::max(1, int(std::lround(std::max(clip.fps, 1) / 20.0)));
        std::vector<int> samples;
        for (int f = from; f <= to; f += step) samples.push_back(f);
        if (samples.back() != to) samples.push_back(to);
        std::vector<HandContacts> measured(samples.size());
        {
            // Cross-actor pins resolve through the caller (Rig::external), which may fill caches: one thread then.
            const unsigned threads = rig.external ? 1u : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
            std::atomic<size_t> next{0};
            auto work = [&] {
                for (size_t i; (i = next++) < samples.size();)
                    measured[i] = check.measure(skel.global_pose(evaluate(rig, clip, samples[i], shape, constraints).pose, shape));
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
            work();
            for (std::thread& t : pool) t.join();
        }
        std::vector<HandContacts> c(size_t(to - from + 1));
        for (size_t i = 0; i < samples.size(); ++i) {
            const int lo = samples[i], hi = i + 1 < samples.size() ? samples[i + 1] : samples[i];
            for (int f = lo; f <= hi; ++f)
                for (int s = 0; s < 2; ++s) {
                    const HandDepth& a = measured[i].side[s];
                    const HandDepth& b = measured[std::min(i + 1, samples.size() - 1)].side[s];
                    HandDepth& out = c[size_t(f - from)].side[s];
                    const HandDepth& deeper = f == lo ? a : f == hi ? b : (a.depth >= b.depth ? a : b);
                    if (deeper.depth > out.depth) out = deeper;
                }
        }
        bool any = false;
        for (size_t i = 0; i < samples.size(); ++i)
            for (int s = 0; s < 2; ++s) {
                const double d = measured[i].side[s].depth;
                any = any || d > tol;
                if (pass == 0 && d > rep.deepest) rep.deepest = d, rep.deepest_frame = samples[i], rep.deepest_side = s;
            }
        if (!any) break;
        for (int s = 0; s < 2; ++s) {
            const int limb = rig.find_limb(std::string("Arm") + kSide[s]);
            if (limb < 0) continue;
            // A hand deep in the body may be nearer the far side than the side it came in by, and pushing it on through
            // the body is wrong: in each contact, the deep frames go out the way the nearest shallow frame (where the
            // hand went in, or comes out) says.
            for (int f0 = from; f0 <= to;) {
                if (c[size_t(f0 - from)].side[s].depth <= tol) { ++f0; continue; }
                int f1 = f0;
                while (f1 + 1 <= to && c[size_t(f1 + 1 - from)].side[s].depth > tol) ++f1;
                for (int f = f0; f <= f1; ++f) {
                    HandDepth& h = c[size_t(f - from)].side[s];
                    if (h.depth <= kShallow) continue;
                    int best = -1;
                    for (int d = 1; best < 0 && (f - d >= f0 || f + d <= f1); ++d)
                        for (int g : {f - d, f + d})
                            if (best < 0 && g >= f0 && g <= f1 && c[size_t(g - from)].side[s].depth <= kShallow) best = g;
                    if (best >= 0) h.push = c[size_t(best - from)].side[s].push;
                }
                f0 = f1 + 1;
            }
            const LimbInfo& l = rig.limbs()[size_t(limb)];
            // The push each frame takes: its own, or a neighbour's eased off with the distance, whichever is larger,
            // so the hand starts moving before a contact and settles back after it.
            std::vector<Vec3> push(c.size());
            for (int f = from; f <= to; ++f) {
                double best = 0;
                for (int g = std::max(from, f - ease); g <= std::min(to, f + ease); ++g) {
                    const HandDepth& h = c[size_t(g - from)].side[s];
                    if (h.depth <= tol) continue;
                    const double t = 1.0 - double(std::abs(f - g)) / (ease + 1);
                    const double w = t * t * (3 - 2 * t), amount = (h.depth + kMargin) * w;
                    if (amount > best) best = amount, push[size_t(f - from)] = h.push * amount;
                }
            }
            // Move the hand through the arm: its target keyed in IK; in FK, the rotations IK finds, from one copy. Each
            // frame's hand and arm come from the clip as it was (a key just made would carry into the next frame's
            // reading and the pushes would add up).
            const Clip orig = clip;
            Clip ik = clip;
            bool fk_any = false;
            std::vector<int> fk_frames;
            for (int f = from; f <= to; ++f) {
                const Vec3& d = push[size_t(f - from)];
                if (d.length() < 0.001) continue;
                const Evaluation e = evaluate(rig, orig, f, shape, constraints);
                Xform target = e.globals[size_t(l.end)];
                target.pos = target.pos + d;
                if (e.limbs[size_t(limb)].ik_on) {
                    key_limb_target(clip, rig, f, limb, target, shape, constraints);
                } else {
                    // The copy's earlier frames are in IK now (a held blend key): this frame starts from its own FK pose.
                    key_blend(ik, rig, f, limb, 0);
                    switch_to_ik(ik, rig, f, limb, shape);
                    key_limb_target(ik, rig, f, limb, target, shape, constraints);
                    fk_frames.push_back(f);
                    fk_any = true;
                }
                if (!moved_on[s][size_t(f - from)]) moved_on[s][size_t(f - from)] = true, ++rep.frames[s];
            }
            if (fk_any)
                for (int f : fk_frames) {
                    const Pose p = evaluate(rig, ik, f, shape, constraints).pose;
                    for (int j : {l.root, l.mid, l.end}) key_rotation(clip, skel[j].name, f, p.rot[size_t(j)]);
                }
        }
    }
    return rep;
}

}  // namespace vats
