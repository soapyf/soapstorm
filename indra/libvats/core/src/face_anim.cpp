// Viewport Avatar Toolset - face animation by hand: expression sliders, the blink/saccade/look-at layer, look-at.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/face_anim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

#include "vats/dynamics.h"
#include "vats/edit.h"
#include "vats/mocap.h"

namespace vats {

// Millimetres, so a millimetre weighs like a degree in the read-back fit.
std::vector<double> face_bone_values(const Clip& clip, const FaceTable& table, double frame, bool positions) {
    std::vector<double> v;
    for (const std::string& bone : table.bones()) {
        const Vec3 e = curve_euler(clip, bone, frame);
        v.insert(v.end(), {e.x, e.y, e.z});
        if (positions) {
            const Vec3 o = curve_offset(clip, bone, frame) * 1000.0;
            v.insert(v.end(), {o.x, o.y, o.z});
        }
    }
    return v;
}

namespace {

// The eyes turn at most this far from straight ahead towards a look-at target (a cone). ponytail: one cone for
// every direction; human eyes reach about 35-45 degrees sideways and less upwards.
constexpr double kEyeTurn = 30;

std::vector<double> face_values(const FaceTable& table, const std::map<std::string, double>& weights, bool positions) {
    Clip c;
    key_face_weights(c, table, weights, positions, 0);
    return face_bone_values(c, table, 0, positions);
}

// SplitMix64 and our own uniform and normal draws: std's distributions differ between standard libraries, and a
// seed must bake the same face in the app and the viewer on every platform.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return double(next() >> 11) * 0x1.0p-53; }  // [0, 1)
    double normal() {  // Box-Muller
        const double u = 1 - uniform(), v = uniform();
        return std::sqrt(-2 * std::log(u)) * std::cos(2 * kPi * v);
    }
};

// Eyes Alive (Lee, Badler & Badler, SIGGRAPH 2002): a saccade of A degrees lasts D0 + d * A, with D0 = 25 ms and
// d = 2.4 ms per degree.
double saccade_duration(double amplitude) { return 0.025 + 0.0024 * amplitude; }

// One stretch of the timeline [a, b] seconds. loop: blinks keep lo apart across the seam from b back to a.
void generate(const FaceLayer& L, double a, double b, bool loop, std::uint64_t stream, FaceEvents& ev) {
    if (b - a <= 0) return;
    Rng r{(std::uint64_t(L.seed) << 8) ^ stream};
    if (L.blinks) {
        const double lo = std::max(0.5, std::min(L.blink_min, L.blink_max)), hi = std::max(lo, L.blink_max);
        const double len = std::clamp(L.blink_length, 0.05, 1.0);
        std::vector<double> seg;
        for (double t = a + r.uniform() * lo; t + len <= b; t += lo + (hi - lo) * r.uniform()) seg.push_back(t);
        if (loop)
            while (!seg.empty() && (b - seg.back()) + (seg.front() - a) < lo) seg.pop_back();
        ev.blinks.insert(ev.blinks.end(), seg.begin(), seg.end());
    }
    const double limit = L.eye_limit;
    if (!L.saccades || !(limit > 0)) return;
    const double median = std::clamp(L.saccade_interval, 0.1, 10.0);
    // Directions every 45 degrees from the avatar's left (yaw +) round through up; straight moves twice as likely as
    // diagonal ones. ponytail: approximates the direction histogram of Eyes Alive, whose exact table we don't use.
    static constexpr double kWeight[8] = {2, 1, 2, 1, 2, 1, 2, 1};
    double yaw = 0, pitch = 0, t = a, last_end = a;
    for (;;) {
        // Fixations last a log-normal time around the median.
        t += std::clamp(median * std::exp(0.5 * r.normal()), 0.1, 10 * median);
        // Eyes Alive's magnitude fit: P(A) falls off as exp(-A / 6.9), so A is exponential with mean 6.9 degrees.
        const double amp = -6.9 * std::log(1 - r.uniform());
        double ny = 0, np = 0;
        for (int tries = 0; tries < 8; ++tries) {
            double pick = r.uniform() * 12, dir = 0;
            for (int k = 0; k < 8; ++k)
                if ((pick -= kWeight[k]) < 0) {
                    dir = k * kPi / 4;
                    break;
                }
            ny = yaw + amp * std::cos(dir), np = pitch - amp * std::sin(dir);  // up = negative pitch
            if (std::hypot(ny, np) <= limit) break;
        }
        if (const double m = std::hypot(ny, np); m > limit) ny *= limit / m, np *= limit / m;  // never past the limit
        const double d = saccade_duration(std::hypot(ny - yaw, np - pitch));
        if (t + d + saccade_duration(std::hypot(ny, np)) + 0.1 > b) break;  // room to look back before b
        ev.saccades.push_back({t, d, ny, np});
        yaw = ny, pitch = np, t = last_end = t + d;
    }
    if (yaw != 0 || pitch != 0) {  // back to where the eyes look, just before the end
        const double d = saccade_duration(std::hypot(yaw, pitch));
        ev.saccades.push_back({std::max(last_end, b - d - 0.05), d, 0, 0});
    }
}

// The shortest rotation taking unit vector a onto unit vector b.
Quat arc(const Vec3& a, const Vec3& b) {
    const double d = a.dot(b);
    if (d < -0.999999) {
        Vec3 axis = a.cross({0, 0, 1});
        if (axis.length() < 1e-6) axis = a.cross({0, 1, 0});
        return Quat::axis_angle(axis, kPi);
    }
    const Vec3 c = a.cross(b);
    return Quat{1 + d, c.x, c.y, c.z}.normalized();
}

Quat add_euler(const Quat& q, const Vec3& deg) { return euler_to_quat(quat_to_euler(q) + deg); }

// The nodes every bake keys: the gaze eyes and lids and the blink shapes' bones (mHead only with a look-at).
std::vector<int> layer_nodes(const Skeleton& skel, const FaceTable& table) {
    std::set<int> n;
    auto add = [&](const std::string& name) {
        if (int i = skel.find(name); i > 0) n.insert(i);
    };
    for (const FaceTable::Gaze& g : table.gaze) {
        for (auto& e : g.eyes) add(e);
        for (auto& [lid, k] : g.lids) add(lid);
    }
    for (const char* s : {"eyeBlinkLeft", "eyeBlinkRight"})
        if (auto it = table.shapes.find(s); it != table.shapes.end())
            for (auto& m : it->second) add(m.bone);
    return {n.begin(), n.end()};
}

void restore_source(Clip& clip, const Skeleton& skel, const FaceLayer& L, const std::vector<int>& nodes) {
    for (int n : nodes) {
        auto it = L.source.find(skel[n].name);
        if (it == L.source.end()) clip.curves.erase(skel[n].name);
        else clip.curves[it->first] = it->second;
    }
}

}  // namespace

// --- Face panel ---------------------------------------------------------------------------------

void key_face_weights(Clip& clip, const FaceTable& table, const std::map<std::string, double>& weights,
                      bool positions, double frame, const std::map<std::string, double>* from) {
    auto keyed = [&](const std::map<std::string, double>& w, Clip& out, double at) {
        VmcState s;
        for (auto& [name, v] : w)
            if (v > 0) s.blend[name] = float(std::min(v, 1.0));
        FaceSettings fs;
        fs.positions = positions;
        fs.head = false;
        key_face(out, table, s, fs, at);
    };
    if (!from) return keyed(weights, clip, frame);
    Clip a, b;
    keyed(*from, a, 0);
    keyed(weights, b, 0);
    for (const std::string& bone : table.bones()) {
        const Vec3 dr = curve_euler(b, bone, 0) - curve_euler(a, bone, 0), dp = curve_offset(b, bone, 0) - curve_offset(a, bone, 0);
        if (dr.length() > 1e-9) key_euler(clip, bone, frame, curve_euler(clip, bone, frame) + dr);
        if (dp.length() > 1e-12) key_offset(clip, bone, frame, curve_offset(clip, bone, frame) + dp);
    }
}

bool face_weights_match(const Clip& clip, const FaceTable& table, const std::map<std::string, double>& weights,
                        bool positions, double frame) {
    const std::vector<double> want = face_values(table, weights, positions), have = face_bone_values(clip, table, frame, positions);
    for (size_t i = 0; i < want.size(); ++i)
        if (std::fabs(want[i] - have[i]) > 0.01) return false;
    return true;
}

std::map<std::string, double> read_face_weights(const Clip& clip, const FaceTable& table, double frame, bool positions) {
    // min |A w - b|^2 + 0.001 sum(w) over 0 <= w <= 1 by coordinate descent (projected Gauss-Seidel; convex, so it
    // converges). The small cost per shape prefers few shapes: a blink stays a blink rather than half a blink
    // plus a wide eye and a squint cancelling out. Each column is what one shape at full weight keys; key_face is
    // linear in the weights inside the eye limits.
    const std::vector<double> b = face_bone_values(clip, table, frame, positions);
    std::vector<std::string> names;
    std::vector<std::vector<double>> cols;
    std::vector<double> norm;
    for (auto& [shape, motions] : table.shapes) {
        names.push_back(shape);
        cols.push_back(face_values(table, {{shape, 1.0}}, positions));
        double n2 = 0;
        for (double x : cols.back()) n2 += x * x;
        norm.push_back(n2);
    }
    std::vector<double> w(names.size(), 0.0), r = b;  // r = b - A w
    for (int sweep = 0; sweep < 1000; ++sweep) {
        double moved = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            if (norm[i] < 1e-12) continue;
            double g = 0;
            for (size_t k = 0; k < r.size(); ++k) g += cols[i][k] * r[k];
            const double nw = std::clamp(w[i] + (g - 0.0005) / norm[i], 0.0, 1.0), dw = nw - w[i];
            if (dw == 0) continue;
            for (size_t k = 0; k < r.size(); ++k) r[k] -= cols[i][k] * dw;
            w[i] = nw;
            moved = std::max(moved, std::fabs(dw));
        }
        if (moved < 1e-7) break;
    }
    // Coordinate descent crawls along valleys where shapes nearly cancel (a blink against a wide eye). Polish: solve
    // the normal equations on the shapes strictly inside 0..1, pinning any that leave the range, and keep the
    // result when it costs no more.
    auto cost = [&](const std::vector<double>& x) {
        std::vector<double> res = b;
        double c = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            for (size_t k = 0; k < res.size(); ++k) res[k] -= cols[i][k] * x[i];
            c += 0.001 * x[i];
        }
        for (double v : res) c += v * v;
        return c;
    };
    std::vector<double> x = w;
    std::vector<size_t> inside;
    for (size_t i = 0; i < w.size(); ++i)
        if (norm[i] > 1e-12 && w[i] > 1e-9 && w[i] < 1 - 1e-9) inside.push_back(i);
    while (!inside.empty()) {
        const size_t n = inside.size();
        std::vector<double> r0 = b;  // what the shapes held at a bound leave to explain
        for (size_t i = 0; i < x.size(); ++i)
            if (std::find(inside.begin(), inside.end(), i) == inside.end())
                for (size_t k = 0; k < r0.size(); ++k) r0[k] -= cols[i][k] * x[i];
        std::vector<std::vector<double>> m(n, std::vector<double>(n + 1, 0.0));
        for (size_t a = 0; a < n; ++a) {
            for (size_t c = 0; c < n; ++c)
                for (size_t k = 0; k < r0.size(); ++k) m[a][c] += cols[inside[a]][k] * cols[inside[c]][k];
            for (size_t k = 0; k < r0.size(); ++k) m[a][n] += cols[inside[a]][k] * r0[k];
            m[a][n] -= 0.0005;
        }
        bool singular = false;
        for (size_t c = 0; c < n && !singular; ++c) {  // Gauss-Jordan with partial pivoting
            size_t p = c;
            for (size_t a = c + 1; a < n; ++a)
                if (std::fabs(m[a][c]) > std::fabs(m[p][c])) p = a;
            if (std::fabs(m[p][c]) < 1e-9) singular = true;
            if (singular) break;
            std::swap(m[p], m[c]);
            for (size_t a = 0; a < n; ++a) {
                if (a == c) continue;
                const double f = m[a][c] / m[c][c];
                for (size_t k = c; k <= n; ++k) m[a][k] -= f * m[c][k];
            }
        }
        if (singular) break;  // shapes that cannot be told apart: keep the descent's answer
        size_t worst = n;
        double out_by = 0;
        for (size_t a = 0; a < n; ++a) {
            const double v = m[a][n] / m[a][a], by = std::max(-v, v - 1);
            x[inside[a]] = v;
            if (by > out_by) out_by = by, worst = a;
        }
        if (worst == n) break;
        x[inside[worst]] = std::clamp(x[inside[worst]], 0.0, 1.0);
        inside.erase(inside.begin() + std::ptrdiff_t(worst));
    }
    for (double& v : x) v = std::clamp(v, 0.0, 1.0);
    if (cost(x) <= cost(w)) w = x;
    std::map<std::string, double> out;
    for (size_t i = 0; i < names.size(); ++i) out[names[i]] = w[i] < 1e-4 ? 0 : w[i];
    return out;
}

bool face_shape_keys(const FaceTable& table, const std::string& shape, bool positions) {
    auto it = table.shapes.find(shape);
    if (it == table.shapes.end()) return false;
    for (auto& m : it->second)
        if ((m.has_rot && m.rot != Vec3{}) || (positions && m.has_pos && m.pos != Vec3{})) return true;
    return false;
}

LibraryItem make_face_pose(const Clip& clip, const FaceTable& table, double frame, bool positions) {
    LibraryItem it;
    it.id = new_item_id();
    it.kind = "face";
    for (const std::string& bone : table.bones()) it.bones[bone] = curve_euler(clip, bone, frame);
    if (positions)
        for (auto& [shape, motions] : table.shapes)
            for (auto& m : motions)
                if (m.has_pos) it.offsets[m.bone] = curve_offset(clip, m.bone, frame);
    return it;
}

// --- Layer --------------------------------------------------------------------------------------

FaceEvents face_layer_events(const Clip& clip, const FaceLayer& layer) {
    FaceEvents ev;
    const double fps = std::max(clip.fps, 1);
    if (clip.loop) {
        const int in = std::clamp(clip.loop_in, 0, clip.end_frame), out = std::clamp(clip.loop_out, in, clip.end_frame);
        generate(layer, 0, in / fps, false, 1, ev);
        generate(layer, in / fps, out / fps, true, 2, ev);
    } else {
        generate(layer, 0, clip.end_frame / fps, false, 0, ev);
    }
    return ev;
}

double blink_weight(const FaceEvents& ev, double t, double blink_length) {
    const double len = std::clamp(blink_length, 0.05, 1.0);
    double w = 0;
    for (double s : ev.blinks) {
        const double u = (t - s) / len;
        if (u < 0 || u >= 1) continue;
        w = std::max(w, u < 0.3 ? u / 0.3 : u < 0.45 ? 1.0 : 1 - (u - 0.45) / 0.55);
    }
    return w;
}

Vec3 saccade_offset(const FaceEvents& ev, double t) {
    double yaw = 0, pitch = 0;
    for (const FaceEvents::Saccade& s : ev.saccades) {
        if (t < s.start) break;
        if (t < s.start + s.duration) {
            double u = (t - s.start) / s.duration;
            u = u * u * (3 - 2 * u);
            return {0, pitch + (s.pitch - pitch) * u, yaw + (s.yaw - yaw) * u};
        }
        yaw = s.yaw, pitch = s.pitch;
    }
    return {0, pitch, yaw};
}

void bake_face_layer(Clip& clip, const Rig& rig, const Shape* shape, const FaceTable& table, bool positions,
                     const LookTarget& look) {
    if (!clip.face_layer) return;
    const Skeleton& skel = rig.skeleton();
    FaceLayer& L = *clip.face_layer;
    const std::vector<int> owned = layer_nodes(skel, table);
    const int head = skel.find("mHead");
    const bool turn_head = look && L.head_share > 0 && head > 0;
    if (L.baked) {  // back to the source before re-baking over it
        restore_source(clip, skel, L, owned);
        if (L.head_baked) restore_source(clip, skel, L, {head});
    } else {
        L.source.clear();
        for (int n : owned)
            if (auto it = clip.curves.find(skel[n].name); it != clip.curves.end()) L.source[it->first] = it->second;
    }
    // mHead's pre-bake keys are kept only while a bake turns the head, so head keys set after a bake without a
    // look-at are never replaced by a later one.
    if (head > 0) {
        const std::string& name = skel[head].name;
        if (!turn_head) {
            L.source.erase(name);
        } else if (!(L.baked && L.head_baked)) {
            L.source.erase(name);
            if (auto it = clip.curves.find(name); it != clip.curves.end()) L.source[name] = it->second;
        }
    }
    const FaceEvents ev = face_layer_events(clip, L);
    const double fps = std::max(clip.fps, 1), share = std::clamp(L.head_share, 0.0, 1.0);
    std::vector<int> with_position;
    std::vector<Pose> frames;
    for (int f = 0; f <= clip.end_frame; ++f) {
        const Evaluation e = evaluate(rig, clip, f, shape);
        Pose p = e.pose;
        std::vector<Xform> g = e.globals;
        const double t = f / fps;
        Vec3 target;
        const bool aim = look && look(f, target);
        if (aim && turn_head) {
            p.rot[head] = nlerp(p.rot[head], aim_rotation(skel, g, head, target, L.head_max), share);
            g = skel.global_pose(p, shape);
        }
        const Vec3 off = saccade_offset(ev, t);
        for (const FaceTable::Gaze& gz : table.gaze) {
            const int first = gz.eyes.empty() ? -1 : skel.find(gz.eyes[0]);
            if (first <= 0) continue;
            const double pitch_before = quat_to_euler(p.rot[first]).y;
            for (const std::string& eye : gz.eyes) {
                const int n = skel.find(eye);
                if (n <= 0) continue;
                if (aim) p.rot[n] = aim_rotation(skel, g, n, target, kEyeTurn);
                p.rot[n] = add_euler(p.rot[n], off);
            }
            // The lids follow the eyes' pitch by the table's fractions (looking down lowers the upper lid).
            const double dp = quat_to_euler(p.rot[first]).y - pitch_before;
            for (auto& [lid, k] : gz.lids)
                if (int n = skel.find(lid); n > 0) p.rot[n] = add_euler(p.rot[n], {0, k * dp, 0});
        }
        if (const double w = L.blinks ? blink_weight(ev, t, L.blink_length) : 0; w > 0)
            for (const char* s : {"eyeBlinkLeft", "eyeBlinkRight"})
                if (auto it = table.shapes.find(s); it != table.shapes.end())
                    for (const FaceTable::Motion& m : it->second) {
                        const int n = skel.find(m.bone);
                        if (n <= 0) continue;
                        if (m.has_rot) p.rot[n] = add_euler(p.rot[n], m.rot * w);
                        if (m.has_pos && positions) p.offset[n] += m.pos * w;
                    }
        frames.push_back(std::move(p));
    }
    if (positions)
        for (const char* s : {"eyeBlinkLeft", "eyeBlinkRight"})
            if (auto it = table.shapes.find(s); it != table.shapes.end())
                for (auto& m : it->second)
                    if (int n = skel.find(m.bone); n > 0 && m.has_pos) with_position.push_back(n);
    std::vector<int> nodes = owned;
    if (turn_head) nodes.push_back(head);
    bake_samples(clip, skel, nodes, frames, 0.1, 0.0001, with_position);
    L.baked = true;
    L.head_baked = turn_head;
}

void unbake_face_layer(Clip& clip, const Skeleton& skel, const FaceTable& table) {
    if (!clip.face_layer || !clip.face_layer->baked) return;
    FaceLayer& L = *clip.face_layer;
    restore_source(clip, skel, L, layer_nodes(skel, table));
    if (int head = skel.find("mHead"); head > 0 && L.head_baked) restore_source(clip, skel, L, {head});
    L.source.clear();
    L.baked = L.head_baked = false;
}

// --- Look-at ------------------------------------------------------------------------------------

Quat aim_rotation(const Skeleton& skel, const std::vector<Xform>& g, int node, const Vec3& target, double max_deg) {
    const Node& n = skel[node];
    const Quat base = ((n.parent >= 0 ? g[n.parent].rot : Quat{}) * n.rest).normalized();  // the node at no rotation
    const Quat cur = g[node].rot.normalized();
    Vec3 d = (target - g[node].pos).normalized();
    if (d.length() == 0) return (base.conj() * cur).normalized();
    const Vec3 ahead = base.rotate({1, 0, 0});
    const double lim = std::max(0.0, max_deg) * kDegToRad;
    if (std::acos(std::clamp(ahead.dot(d), -1.0, 1.0)) > lim) {  // as far towards the target as the limit allows
        Vec3 axis = ahead.cross(d);
        if (axis.length() < 1e-9) axis = base.rotate({0, 0, 1});
        d = Quat::axis_angle(axis, lim).rotate(ahead);
    }
    return (base.conj() * arc(cur.rotate({1, 0, 0}), d) * cur).normalized();
}

bool look_at_bake(Clip& clip, const Rig& rig, const std::vector<int>& nodes_in, const LookTarget& target,
                  const LookAtOptions& opt, const Shape* shape, std::string& why) {
    const Skeleton& skel = rig.skeleton();
    if (!target) return why = "Choose something to look at", false;
    std::vector<int> nodes;
    for (int n : nodes_in)
        if (n > 0 && n < skel.joint_count()) nodes.push_back(n);
    std::sort(nodes.begin(), nodes.end());  // parents come before their children in the skeleton
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    if (nodes.empty()) return why = "Select the head or the eyes (not the hips or an attachment point)", false;
    const int f0 = std::clamp(opt.from, 0, clip.end_frame);
    const int f1 = opt.to < 0 ? clip.end_frame : std::clamp(opt.to, f0, clip.end_frame);
    const double w = std::clamp(opt.weight, 0.0, 1.0);
    const Clip src = clip;  // every frame aims from the animation as it was, not from the frames keyed before it
    int keyed = 0;
    for (int f = f0; f <= f1; ++f) {
        Vec3 t;
        if (!target(f, t)) continue;
        const Evaluation e = evaluate(rig, src, f, shape);
        Pose p = e.pose;
        std::vector<Xform> g = e.globals;
        for (int n : nodes) {
            p.rot[n] = nlerp(p.rot[n], aim_rotation(skel, g, n, t, opt.max_turn), w);
            key_rotation(clip, skel[n].name, f, p.rot[n]);
            g = skel.global_pose(p, shape);
        }
        ++keyed;
    }
    if (!keyed) return why = "The target could not be found on any frame", false;
    return true;
}

}  // namespace vats
