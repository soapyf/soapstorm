// Viewport Avatar Toolset - rigging a model from scratch. See vats/auto_rig.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The guesses read the model's shape through cross-sections: a plane cuts the surface, the cut is drawn into a small
// grid and filled, and each filled piece is a limb, a trunk or a tail where the plane crosses it. Horizontal cuts find
// the legs, the crotch and the neck; cuts square to a limb, stepped from its tip, follow its centre line until it joins
// the body. Proportions from SL's default skeleton fill in what the shape cannot say, with a low confidence.
#include "vats/auto_rig.h"

#include <algorithm>
#include <set>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <unordered_map>

namespace vats {

namespace {

const std::vector<RigMarkerDef> kMarkers = {
    {"chin", "chin", "mHead", "", ""},
    {"neck", "neck base", "mNeck", "", ""},
    {"shoulder_l", "left shoulder", "mShoulderLeft", "shoulder_r", ""},
    {"shoulder_r", "right shoulder", "mShoulderRight", "shoulder_l", ""},
    {"elbow_l", "left elbow", "mElbowLeft", "elbow_r", ""},
    {"elbow_r", "right elbow", "mElbowRight", "elbow_l", ""},
    {"wrist_l", "left wrist", "mWristLeft", "wrist_r", ""},
    {"wrist_r", "right wrist", "mWristRight", "wrist_l", ""},
    {"hip_l", "left hip", "mHipLeft", "hip_r", ""},
    {"hip_r", "right hip", "mHipRight", "hip_l", ""},
    {"knee_l", "left knee", "mKneeLeft", "knee_r", ""},
    {"knee_r", "right knee", "mKneeRight", "knee_l", ""},
    {"ankle_l", "left ankle", "mAnkleLeft", "ankle_r", ""},
    {"ankle_r", "right ankle", "mAnkleRight", "ankle_l", ""},
    {"toe_l", "left toes", "mToeLeft", "toe_r", ""},
    {"toe_r", "right toes", "mToeRight", "toe_l", ""},
    {"groin", "groin", "mGroin", "", "groin"},
    {"tail_tip", "tail tip", "mTail6", "", "tail"},
    {"wing_l", "left wing tip", "mWing4Left", "wing_r", "wings"},
    {"wing_r", "right wing tip", "mWing4Right", "wing_l", "wings"},
    {"hind_l", "left hind foot", "mHindLimb4Left", "hind_r", "hind"},
    {"hind_r", "right hind foot", "mHindLimb4Right", "hind_l", "hind"},
    {"ear_l", "left ear tip", "mFaceEar2Left", "ear_r", "ears"},
    {"ear_r", "right ear tip", "mFaceEar2Right", "ear_l", "ears"},
    {"eye_l", "left eye", "mEyeLeft", "eye_r", "face"},
    {"eye_r", "right eye", "mEyeRight", "eye_l", "face"},
    {"jaw", "jaw hinge", "mFaceJaw", "", "face"},
    // SL's own skeleton has its lip corners crossed: mFaceLipCornerRight sits on the avatar's left.
    {"mouth_l", "left mouth corner", "mFaceLipCornerRight", "mouth_r", "face"},
    {"mouth_r", "right mouth corner", "mFaceLipCornerLeft", "mouth_l", "face"},
};

const std::vector<RigGroupDef> kGroups = {
    {"fingers", "Fingers", "Bento fingers along each hand; off for a mitten or a paw: the hand stays on the wrist"},
    {"groin", "Groin", "mGroin at the groin marker"},
    {"tail", "Tail", "mTail1..6 along the tail, from its tip to where it joins the body"},
    {"wings", "Wings", "mWing1..4 along each wing, from its tip to the back"},
    {"hind", "Hind limbs", "mHindLimb1..4 along a second pair of legs (a taur's), from each hind foot"},
    {"ears", "Ears", "mFaceEar1..2 along each ear, from its tip"},
    {"face", "Face", "the Bento face bones, fitted to the eyes, the jaw hinge and the mouth corners"},
};

constexpr double kSlHeight = 1.865;  // SL's default avatar, soles to the top of the head (kSlAvatarHeight)

// The model as one surface: every part, its vertices and triangles.
struct Surface {
    std::vector<Vec3> p;
    std::vector<std::array<int, 3>> t;
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    double height() const { return hi.z - lo.z; }
};

Surface surface_of(const DaeModel& m) {
    Surface s;
    s.p.reserve(m.positions.size() / 3);
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
        const Vec3 v{m.positions[i], m.positions[i + 1], m.positions[i + 2]};
        s.p.push_back(v);
        for (int a = 0; a < 3; ++a) s.lo[a] = std::min(s.lo[a], v[a]), s.hi[a] = std::max(s.hi[a], v[a]);
    }
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        if (m.indices[i] < s.p.size() && m.indices[i + 1] < s.p.size() && m.indices[i + 2] < s.p.size())
            s.t.push_back({int(m.indices[i]), int(m.indices[i + 1]), int(m.indices[i + 2])});
    return s;
}

// A filled piece of a cross-section.
struct Blob {
    Vec3 centre;
    double area = 0;
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;  // its extent along the plane's u and v axes, from the plane's origin
    bool edge = false;                      // it reaches the edge of the cut: bigger than the cut looked
    double radius() const { return std::sqrt(area / kPi); }
};

// The pieces where the plane through o (normal n, in-plane axes u and v) cuts the surface, within half of o along u and
// v, drawn at cell size. dilate: cells one apart join (fingers held together read as a hand).
std::vector<Blob> cut(const Surface& s, const Vec3& o, const Vec3& n, const Vec3& u, const Vec3& v, double half, double cell,
                      bool dilate = false) {
    int g = int(std::ceil(2 * half / cell)) + 1;
    if (g > 400) g = 400, cell = 2 * half / (g - 1);
    std::vector<char> mark(size_t(g) * size_t(g), 0);
    auto put = [&](double a, double b) {
        const int i = int(std::floor((a + half) / cell)), j = int(std::floor((b + half) / cell));
        if (i >= 0 && j >= 0 && i < g && j < g) mark[size_t(j) * size_t(g) + size_t(i)] = 1;
    };
    const double reach = half * 1.75;  // a triangle farther than the cut's corners cannot reach it
    for (const auto& t : s.t) {
        const Vec3 &a = s.p[size_t(t[0])], &b = s.p[size_t(t[1])], &c = s.p[size_t(t[2])];
        double d[3] = {(a - o).dot(n), (b - o).dot(n), (c - o).dot(n)};
        if ((d[0] > 0 && d[1] > 0 && d[2] > 0) || (d[0] < 0 && d[1] < 0 && d[2] < 0)) continue;
        if ((a - o).length() > reach + (b - a).length() + (c - a).length()) continue;
        const Vec3* q[3] = {&a, &b, &c};
        Vec3 ends[2];
        int k = 0;
        for (int e = 0; e < 3 && k < 2; ++e) {
            double d0 = d[e], d1 = d[(e + 1) % 3];
            if (d0 == 0) d0 = 1e-12;
            if (d1 == 0) d1 = 1e-12;
            if ((d0 > 0) == (d1 > 0)) continue;
            ends[k++] = *q[e] + (*q[(e + 1) % 3] - *q[e]) * (d0 / (d0 - d1));
        }
        if (k < 2) continue;
        const double a0 = (ends[0] - o).dot(u), b0 = (ends[0] - o).dot(v), a1 = (ends[1] - o).dot(u), b1 = (ends[1] - o).dot(v);
        if ((a0 < -half && a1 < -half) || (a0 > half && a1 > half) || (b0 < -half && b1 < -half) || (b0 > half && b1 > half)) continue;
        const int steps = std::max(1, int(std::ceil(std::hypot(a1 - a0, b1 - b0) / (cell * 0.5))));
        for (int i = 0; i <= steps; ++i) put(a0 + (a1 - a0) * i / steps, b0 + (b1 - b0) * i / steps);
    }
    if (dilate) {
        std::vector<char> grown = mark;
        for (int j = 0; j < g; ++j)
            for (int i = 0; i < g; ++i)
                if (mark[size_t(j) * size_t(g) + size_t(i)])
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di)
                            if (i + di >= 0 && j + dj >= 0 && i + di < g && j + dj < g) grown[size_t(j + dj) * size_t(g) + size_t(i + di)] = 1;
        mark.swap(grown);
    }
    // Outside: everything the grid's border reaches without crossing the cut's lines.
    std::vector<char> outside(mark.size(), 0);
    std::vector<int> stack;
    for (int i = 0; i < g; ++i)
        for (int c : {i, (g - 1) * g + i, i * g, i * g + g - 1})
            if (!mark[size_t(c)] && !outside[size_t(c)]) outside[size_t(c)] = 1, stack.push_back(c);
    while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        const int i = c % g, j = c / g;
        for (const auto& [di, dj] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int x = i + di, y = j + dj;
            if (x < 0 || y < 0 || x >= g || y >= g) continue;
            const int e = y * g + x;
            if (!mark[size_t(e)] && !outside[size_t(e)]) outside[size_t(e)] = 1, stack.push_back(e);
        }
    }
    std::vector<int> label(mark.size(), -1);
    std::vector<Blob> out;
    for (int start = 0; start < g * g; ++start) {
        if (outside[size_t(start)] || label[size_t(start)] >= 0) continue;
        Blob b;
        b.u0 = b.v0 = 1e300, b.u1 = b.v1 = -1e300;
        double su = 0, sv = 0;
        int cells = 0;
        label[size_t(start)] = int(out.size());
        stack.push_back(start);
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            const int i = c % g, j = c / g;
            const double a = -half + (i + 0.5) * cell, bb = -half + (j + 0.5) * cell;
            su += a, sv += bb, ++cells;
            b.u0 = std::min(b.u0, a - cell / 2), b.u1 = std::max(b.u1, a + cell / 2);
            b.v0 = std::min(b.v0, bb - cell / 2), b.v1 = std::max(b.v1, bb + cell / 2);
            if (i == 0 || j == 0 || i == g - 1 || j == g - 1) b.edge = true;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int x = i + di, y = j + dj;
                    if (x < 0 || y < 0 || x >= g || y >= g) continue;
                    const int e = y * g + x;
                    if (!outside[size_t(e)] && label[size_t(e)] < 0) label[size_t(e)] = int(out.size()), stack.push_back(e);
                }
        }
        b.area = cells * cell * cell;
        b.centre = o + u * (su / cells) + v * (sv / cells);
        out.push_back(b);
    }
    return out;
}

// Horizontal cuts at height z: u along X, v along Y.
std::vector<Blob> slice(const Surface& s, double z, double cell) {
    const Vec3 o{(s.lo.x + s.hi.x) / 2, (s.lo.y + s.hi.y) / 2, z};
    const double half = std::max(s.hi.x - s.lo.x, s.hi.y - s.lo.y) / 2 + 2 * cell;
    return cut(s, o, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, half, cell);
}

// Two axes square to n.
void plane_axes(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 any = std::fabs(n.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    u = n.cross(any).normalized();
    v = n.cross(u).normalized();
}

// A limb's centre line, from its tip towards a point in the body: the centres of cuts square to it, stepped along it,
// until the cut swells into the body (merged) or the line reaches the point.
struct Limb {
    std::vector<Vec3> line;    // from the tip
    std::vector<double> r;     // each cut's radius (as a circle of its area)
    std::vector<double> at;    // distance along the line from the tip
    bool merged = false;
};

Limb trace(const Surface& s, const Vec3& tip, const Vec3& toward, double step, double cell, double max_r, double min_travel) {
    Limb limb;
    Vec3 dir = (toward - tip).normalized();
    if (dir.length() < 0.5) return limb;
    Vec3 p = tip + dir * (step * 0.5);
    double travelled = 0;
    const int max_steps = int((toward - tip).length() / step) + 20;
    for (int i = 0; i < max_steps; ++i) {
        Vec3 u, v;
        plane_axes(dir, u, v);
        const std::vector<Blob> blobs = cut(s, p, dir, u, v, max_r, cell, true);
        const Blob* best = nullptr;
        double best_d = 1e300;
        const double limit = limb.r.empty() ? max_r * 0.5 : std::max(1.5 * limb.r.back() + 2 * cell, 3 * cell);
        for (const Blob& b : blobs) {
            const bool holds = b.u0 <= 0 && b.u1 >= 0 && b.v0 <= 0 && b.v1 >= 0;
            const double d = holds ? 0 : (b.centre - p).length();
            if (d < best_d && d < limit) best_d = d, best = &b;
        }
        if (!best) break;
        const double r = best->radius();
        if (limb.line.size() >= 3 && travelled > min_travel) {
            std::vector<double> recent(limb.r.end() - std::min<long>(8, long(limb.r.size())), limb.r.end());
            std::nth_element(recent.begin(), recent.begin() + long(recent.size() / 2), recent.end());
            const double med = recent[recent.size() / 2];
            if (best->edge || (r > 1.8 * med && r - med > 2 * cell)) {
                limb.merged = true;
                break;
            }
        }
        const Vec3 c = best->centre;
        if (!limb.line.empty()) travelled += (c - limb.line.back()).length();
        limb.line.push_back(c), limb.r.push_back(r), limb.at.push_back(travelled);
        const Vec3 local = limb.line.size() >= 2 ? (c - limb.line[limb.line.size() - 2]).normalized() : dir;
        dir = (local * 0.75 + (toward - c).normalized() * 0.25).normalized();
        p = c + dir * step;
        if ((toward - c).length() < step) break;
    }
    return limb;
}

// The point at distance d along a polyline (clamped to its ends).
Vec3 along(const std::vector<Vec3>& line, double d) {
    if (line.empty()) return {};
    for (size_t i = 1; i < line.size(); ++i) {
        const double len = (line[i] - line[i - 1]).length();
        if (d <= len) return line[i - 1] + (line[i] - line[i - 1]) * (len > 0 ? d / len : 0);
        d -= len;
    }
    return line.back();
}
double length_of(const std::vector<Vec3>& line) {
    double l = 0;
    for (size_t i = 1; i < line.size(); ++i) l += (line[i] - line[i - 1]).length();
    return l;
}

Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

// SL's default skeleton at rest: global positions by name.
std::map<std::string, Vec3> sl_rest(const Skeleton& skel) {
    const std::vector<Xform> g = skel.global_pose(Pose(size_t(skel.size())));
    std::map<std::string, Vec3> out;
    for (int i = 0; i < skel.joint_count(); ++i) out[skel[i].name] = g[size_t(i)].pos;
    return out;
}

// The blob of a horizontal cut nearest the body's middle line (x, y), and so most likely the trunk, neck or head.
const Blob* middle_blob(const std::vector<Blob>& blobs, double y = 0) {
    const Blob* best = nullptr;
    double best_d = 1e300;
    for (const Blob& b : blobs) {
        const double d = b.v0 <= y && b.v1 >= y ? -b.area : std::min(std::fabs(b.v0 - y), std::fabs(b.v1 - y));
        if (d < best_d) best_d = d, best = &b;
    }
    return best;
}

bool overlaps(const Blob& a, const Blob& b) { return a.v0 < b.v1 && b.v0 < a.v1 && a.u0 < b.u1 && b.u0 < a.u1; }

}  // namespace

const std::vector<RigMarkerDef>& rig_marker_defs() { return kMarkers; }

const RigMarkerDef* find_rig_marker(std::string_view id) {
    for (const RigMarkerDef& d : kMarkers)
        if (id == d.id) return &d;
    return nullptr;
}

const std::vector<RigGroupDef>& rig_group_defs() { return kGroups; }

void place_marker(ScratchRig& rig, const std::string& id, const Vec3& pos, bool mirror) {
    const RigMarkerDef* d = find_rig_marker(id);
    if (!d) return;
    Vec3 p = pos;
    if (mirror && !d->mirror[0]) p.y = 0;
    rig.markers[id] = {p, 100, ""};
    if (mirror && d->mirror[0]) rig.markers[d->mirror] = {{p.x, -p.y, p.z}, 100, ""};
}

bool mesh_middle_on_ray(const DaeModel& m, const Vec3& o, const Vec3& d, Vec3& out) {
    std::vector<double> hits;
    auto at = [&](std::uint32_t i) { return Vec3{m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]}; };
    const size_t nv = m.positions.size() / 3;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        if (m.indices[t] >= nv || m.indices[t + 1] >= nv || m.indices[t + 2] >= nv) continue;
        const Vec3 a = at(m.indices[t]), e1 = at(m.indices[t + 1]) - a, e2 = at(m.indices[t + 2]) - a, pv = d.cross(e2);
        const double det = e1.dot(pv);
        if (std::fabs(det) < 1e-12) continue;
        const Vec3 tv = o - a;
        const double u = tv.dot(pv) / det;
        if (u < 0 || u > 1) continue;
        const Vec3 qv = tv.cross(e1);
        const double v = d.dot(qv) / det, dist = e2.dot(qv) / det;
        if (v < 0 || u + v > 1 || dist <= 0) continue;
        hits.push_back(dist);
    }
    if (hits.empty()) return false;
    std::sort(hits.begin(), hits.end());
    // The exit: the next hit past the entry that is not the same surface again (a seam's twin triangles).
    const double scale = std::max((m.bounds_max - m.bounds_min).length(), 1e-6);
    double exit = hits[0];
    for (double h : hits)
        if (h > hits[0] + scale * 1e-4) {
            exit = h;
            break;
        }
    out = o + d * ((hits[0] + exit) / 2);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Standing it up

Vec3 ScratchPlacement::apply(const Vec3& p) const {
    return Quat::axis_angle({0, 0, 1}, turn * kPi / 2).rotate(p) * scale + offset;
}

Vec3 ScratchPlacement::undo(const Vec3& p) const {
    return Quat::axis_angle({0, 0, 1}, -turn * kPi / 2).rotate((p - offset) * (1 / scale));
}

ScratchPlacement scratch_placement(const DaeModel& as_is, int turn, double height) {
    ScratchPlacement pl;
    pl.turn = ((turn % 4) + 4) % 4;
    const Quat q = Quat::axis_angle({0, 0, 1}, pl.turn * kPi / 2);
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    std::vector<double> xs, ys;
    for (size_t i = 0; i + 2 < as_is.positions.size(); i += 3) {
        const Vec3 v = q.rotate({as_is.positions[i], as_is.positions[i + 1], as_is.positions[i + 2]});
        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v[a]), hi[a] = std::max(hi[a], v[a]);
        xs.push_back(v.x), ys.push_back(v.y);
    }
    if (xs.empty()) return pl;
    const double h = hi.z - lo.z;
    pl.scale = height > 0 && h > 1e-9 ? height / h : 1;
    // Its lowest point on the ground, and over the origin: the median of its vertices, which a long scarf or tail does
    // not pull aside as it would the middle of its bounds.
    std::nth_element(xs.begin(), xs.begin() + long(xs.size() / 2), xs.end());
    std::nth_element(ys.begin(), ys.begin() + long(ys.size() / 2), ys.end());
    pl.offset = Vec3{-xs[xs.size() / 2], -ys[ys.size() / 2], -lo.z} * pl.scale;
    return pl;
}

void place_model(DaeModel& m, const ScratchPlacement& p) {
    const Quat q = Quat::axis_angle({0, 0, 1}, p.turn * kPi / 2);
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
        const Vec3 v = p.apply({m.positions[i], m.positions[i + 1], m.positions[i + 2]});
        m.positions[i] = float(v.x), m.positions[i + 1] = float(v.y), m.positions[i + 2] = float(v.z);
    }
    for (size_t i = 0; i + 2 < m.normals.size(); i += 3) {
        const Vec3 n = q.rotate({m.normals[i], m.normals[i + 1], m.normals[i + 2]});
        m.normals[i] = float(n.x), m.normals[i + 1] = float(n.y), m.normals[i + 2] = float(n.z);
    }
    for (DaeShapeKey& k : m.shape_keys) {
        for (size_t i = 0; i + 2 < k.dpos.size(); i += 3) {
            const Vec3 d = q.rotate({k.dpos[i], k.dpos[i + 1], k.dpos[i + 2]}) * p.scale;
            k.dpos[i] = float(d.x), k.dpos[i + 1] = float(d.y), k.dpos[i + 2] = float(d.z);
        }
        for (size_t i = 0; i + 2 < k.dnrm.size(); i += 3) {
            const Vec3 d = q.rotate({k.dnrm[i], k.dnrm[i + 1], k.dnrm[i + 2]});
            k.dnrm[i] = float(d.x), k.dnrm[i + 1] = float(d.y), k.dnrm[i + 2] = float(d.z);
        }
    }
    m.bounds_min = {1e300, 1e300, 1e300}, m.bounds_max = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
        for (int a = 0; a < 3; ++a)
            m.bounds_min[a] = std::min(m.bounds_min[a], double(m.positions[i + size_t(a)])),
            m.bounds_max[a] = std::max(m.bounds_max[a], double(m.positions[i + size_t(a)]));
}

int guess_scratch_turn(const DaeModel& as_is, std::string& why) {
    const Surface s = surface_of(as_is);
    const double h = s.height();
    if (s.p.empty() || h <= 0) return why = "nothing to go by", 0;
    // Feet: the lowest few percent of the model against the legs above them. Toes point ahead of the ankles.
    Vec3 low, above;
    int nl = 0, na = 0;
    for (const Vec3& p : s.p) {
        const double z = (p.z - s.lo.z) / h;
        if (z < 0.03) low += p, ++nl;
        else if (z > 0.08 && z < 0.2) above += p, ++na;
    }
    if (nl > 3 && na > 3) {
        const Vec3 d = low * (1.0 / nl) - above * (1.0 / na);
        const double horiz = std::hypot(d.x, d.y);
        if (horiz > 0.015 * h) {
            // The quarter turn that takes d onto +X.
            const double angle = std::atan2(d.y, d.x);
            const int turn = ((int(std::lround(-angle / (kPi / 2))) % 4) + 4) % 4;
            why = "its feet point that way";
            return turn;
        }
    }
    // Arms out to the sides: the upper body is widest across, and a body faces across its widest side.
    double wx = 0, wy = 0;
    for (const Vec3& p : s.p)
        if ((p.z - s.lo.z) / h > 0.6) wx = std::max(wx, std::fabs(p.x - (s.lo.x + s.hi.x) / 2)), wy = std::max(wy, std::fabs(p.y - (s.lo.y + s.hi.y) / 2));
    if (wx > 1.3 * wy) {
        why = "its arms reach out along the file's front-to-back axis; check which way it faces";
        return 1;
    }
    why = wy > 1.3 * wx ? "its arms reach out to its sides; check it faces you in the front view"
                        : "nothing in its shape says which way it faces: check it faces you in the front view";
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Guessing the markers

std::map<std::string, RigMarker> guess_markers(const DaeModel& placed, const std::set<std::string>& groups) {
    std::map<std::string, RigMarker> out;
    const Surface s = surface_of(placed);
    const double H = s.height();
    if (s.t.empty() || H <= 0) return out;
    const double cell = H / 140;
    auto put = [&](const std::string& id, const Vec3& p, int conf, const std::string& why) { out[id] = {p, std::clamp(conf, 0, 99), why}; };
    // Horizontal cuts every 1/160 of the height.
    const int N = 160;
    std::vector<std::vector<Blob>> cuts(N);
    for (int i = 0; i < N; ++i) cuts[size_t(i)] = slice(s, s.lo.z + H * (i + 0.5) / N, cell);
    auto row = [&](double z) { return std::clamp(int((z - s.lo.z) / H * N), 0, N - 1); };
    auto zrow = [&](int i) { return s.lo.z + H * (i + 0.5) / N; };

    // Legs: the two largest pieces at mid-shin, one each side, followed up until one piece holds both for good (thighs
    // that touch a while lower down part again): the crotch.
    std::array<std::vector<const Blob*>, 2> leg;  // per side (0 left, 1 right), per row
    leg[0].assign(N, nullptr), leg[1].assign(N, nullptr);
    double crotch = 0.47 * H;
    bool legs = false, parted = false;
    {
        const int r0 = row(0.22 * H);
        std::vector<const Blob*> big;
        for (const Blob& b : cuts[size_t(r0)]) big.push_back(&b);
        std::sort(big.begin(), big.end(), [](const Blob* a, const Blob* b) { return a->area > b->area; });
        if (big.size() >= 2 && (big[0]->centre.y > 0) != (big[1]->centre.y > 0) && big[1]->area > 0.3 * big[0]->area) {
            legs = true;
            leg[0][size_t(r0)] = big[0]->centre.y > 0 ? big[0] : big[1];
            leg[1][size_t(r0)] = big[0]->centre.y > 0 ? big[1] : big[0];
            const int hold = std::max(2, int(std::ceil(0.04 * N)));  // rows a join must last to be the crotch
            for (int dir : {-1, 1}) {
                const Blob* last[2] = {leg[0][size_t(r0)], leg[1][size_t(r0)]};
                int joined = 0;
                for (int r = r0 + dir; r >= 0 && r < N; r += dir) {
                    const Blob* found[2] = {nullptr, nullptr};
                    for (int side = 0; side < 2; ++side)
                        for (const Blob& b : cuts[size_t(r)])
                            if (overlaps(b, *last[side]) && (!found[side] || b.area > found[side]->area)) found[side] = &b;
                    if (!found[0] || !found[1]) break;
                    if (found[0] == found[1]) {
                        if (dir < 0) break;
                        if (++joined >= hold) {
                            crotch = zrow(r - hold + 1) - H / N / 2;
                            parted = true;
                            break;
                        }
                        continue;
                    }
                    joined = 0;
                    leg[0][size_t(r)] = last[0] = found[0], leg[1][size_t(r)] = last[1] = found[1];
                }
            }
        }
    }
    const int crotch_conf = parted && crotch > 0.3 * H && crotch < 0.65 * H ? 75 : legs ? 45 : 20;
    const std::string crotch_why = parted ? "where the legs join" : "from SL's proportions (the legs could not be told apart)";
    // The trunk's middle at a height.
    auto trunk = [&](double z) -> const Blob* { return middle_blob(cuts[size_t(row(z))]); };
    for (int side = 0; side < 2; ++side) {
        const double sy = side == 0 ? 1 : -1;
        const char* tag = side == 0 ? "_l" : "_r";
        auto leg_at = [&](double z) -> const Blob* { return leg[size_t(side)][size_t(row(z))]; };
        // Ankle: the slimmest the leg is front to back above the foot.
        Vec3 ankle{0, sy * 0.069 * H, 0.036 * H};
        int ankle_conf = 25;
        std::string ankle_why = "from SL's proportions";
        if (legs) {
            double best = 1e300, foot = 0;
            for (int r = row(0.0); r <= row(0.012 * H); ++r)
                if (leg[size_t(side)][size_t(r)]) foot = std::max(foot, leg[size_t(side)][size_t(r)]->u1 - leg[size_t(side)][size_t(r)]->u0);
            for (int r = row(0.025 * H); r <= row(0.10 * H); ++r) {
                const Blob* b = leg[size_t(side)][size_t(r)];
                if (b && b->u1 - b->u0 < best) best = b->u1 - b->u0, ankle = {b->centre.x, b->centre.y, zrow(r) - 0.012 * H};
            }
            if (best < 1e300) {
                ankle_conf = best < 0.8 * foot ? 70 : 50;
                ankle_why = best < 0.8 * foot ? "the slimmest the leg is above the foot" : "the slimmest the leg is near the floor";
            }
        }
        put(std::string("ankle") + tag, ankle, ankle_conf, ankle_why);
        // Toes: the front of the foot on the floor, on this side (feet may touch).
        Vec3 toe{ankle.x + 0.118 * H, ankle.y, 0.2 * ankle.z};
        int toe_conf = 25;
        double front = -1e300;
        for (const Vec3& p : s.p)
            if (p.z < s.lo.z + 0.03 * H && (p.y - ankle.y) * sy > -0.05 * H && std::fabs(p.y - ankle.y) < 0.08 * H) front = std::max(front, p.x);
        if (front > ankle.x + 0.03 * H) {
            toe = {front - 0.1 * (front - ankle.x), ankle.y, 0.2 * ankle.z};
            toe_conf = 65;
        }
        put(std::string("toe") + tag, toe, toe_conf, toe_conf > 50 ? "the front of the foot" : "ahead of the ankle by SL's proportions");
        // Hip: in line with the top of the leg, a hand's width above the crotch (SL's own hips sit higher still).
        Vec3 hip{0, sy * 0.069 * H, crotch + 0.075 * H};
        const Blob* top_leg = nullptr;
        for (int r = row(crotch - 0.03 * H); legs && r >= 0 && !top_leg; --r) top_leg = leg[size_t(side)][size_t(r)];
        if (top_leg) hip.x = top_leg->centre.x, hip.y = top_leg->centre.y;
        else if (const Blob* t = trunk(crotch + 0.02 * H)) hip.x = t->centre.x;
        put(std::string("hip") + tag, hip, std::min(crotch_conf, 70), crotch_why == "where the legs join" ? "in the leg, above where the legs join" : crotch_why);
        // Knee: up the leg as SL's is (49% from the ankle to the hip), in the middle of the leg there.
        const double kz = ankle.z + 0.488 * (hip.z - ankle.z);
        Vec3 knee{lerp(ankle, hip, 0.488).x, lerp(ankle, hip, 0.488).y, kz};
        if (const Blob* b = legs ? leg_at(kz) : nullptr) knee.x = b->centre.x, knee.y = b->centre.y;
        put(std::string("knee") + tag, knee, legs ? 55 : 25, "half way up the leg, in its middle (SL's proportions)");
    }
    // Groin: the front of the trunk just above the crotch.
    if (const Blob* t = trunk(crotch + 0.03 * H)) {
        const double front = (s.lo.x + s.hi.x) / 2 + t->u1;
        put("groin", {t->centre.x + 0.6 * (front - t->centre.x), t->centre.y, crotch + 0.03 * H}, crotch_conf - 10,
            "the front of the body where the legs join");
    }

    // Arms: from each hand's farthest point, along the arm to where it joins the body (wings, behind, are not arms).
    std::array<double, 2> shoulder_z{0.816 * H, 0.816 * H};
    const Blob* upper = trunk(0.72 * H);
    const double chest_back = upper ? (s.lo.x + s.hi.x) / 2 + upper->u0 : -0.1 * H;
    const bool wings = groups.count("wings") > 0;
    for (int side = 0; side < 2; ++side) {
        const double sy = side == 0 ? 1 : -1;
        const char* tag = side == 0 ? "_l" : "_r";
        int tip = -1;
        for (size_t i = 0; i < s.p.size(); ++i)
            if (s.p[i].z > 0.3 * H && (!wings || s.p[i].x > chest_back) && (tip < 0 || s.p[i].y * sy > s.p[size_t(tip)].y * sy))
                tip = int(i);
        const Vec3 sl_shoulder{0, sy * 0.088 * H, 0.816 * H};
        const Blob* chest = trunk(0.75 * H);
        const double chest_half = chest ? std::max(std::fabs(chest->v0), std::fabs(chest->v1)) : 0.1 * H;
        const bool out_wide = tip >= 0 && std::fabs(s.p[size_t(tip)].y) > chest_half + 0.12 * H;
        Limb arm;
        if (tip >= 0) arm = trace(s, s.p[size_t(tip)], {0, sy * 0.05 * H, 0.78 * H}, 0.012 * H, H / 160, 0.12 * H, 0.2 * H);
        Vec3 wrist = {0, sy * 0.331 * H, 0.816 * H}, shoulder = sl_shoulder;
        int wrist_conf = 20, shoulder_conf = 20;
        std::string wrist_why = "from SL's proportions", shoulder_why = "from SL's proportions";
        if (arm.line.size() >= 6) {
            double best = 1e300, palm = 0;
            size_t wi = 0;
            for (size_t i = 0; i < arm.line.size(); ++i) {
                if (arm.at[i] < 0.05 * H) palm = std::max(palm, arm.r[i]);
                if (arm.at[i] >= 0.055 * H && arm.at[i] <= 0.15 * H && arm.r[i] < best) best = arm.r[i];
            }
            // The first place as slim as that, from the hand: a forearm as slim all along keeps the wrist by the hand.
            for (size_t i = 0; i < arm.line.size() && best < 1e300; ++i)
                if (arm.at[i] >= 0.055 * H && arm.r[i] <= best * 1.03) {
                    wi = i;
                    break;
                }
            if (best < 1e300) {
                wrist = arm.line[wi];
                wrist_conf = best < 0.85 * palm ? 70 : 50;
                wrist_why = best < 0.85 * palm ? "the slimmest the arm is past the hand" : "a hand's length up the arm";
            }
            // The shoulder: the upper arm's line carried in to the side of the chest (SL's shoulder sits just inside it),
            // else where the arm swells past its girth into the body.
            const double a0 = arm.at[wi], a1 = arm.at.back();
            std::vector<double> upper_r;
            Vec3 from, to;
            for (size_t i = wi; i < arm.line.size(); ++i) {
                if (arm.at[i] < a0 + 0.25 * (a1 - a0) || arm.at[i] > a0 + 0.6 * (a1 - a0)) continue;
                upper_r.push_back(arm.r[i]);
                if (upper_r.size() == 1) from = arm.line[i];
                to = arm.line[i];
            }
            size_t si = arm.line.size() - 1;
            if (!upper_r.empty()) {
                std::nth_element(upper_r.begin(), upper_r.begin() + long(upper_r.size() / 2), upper_r.end());
                const double girth = upper_r[upper_r.size() / 2];
                for (size_t i = wi; i < arm.line.size(); ++i)
                    if (arm.at[i] > a0 + 0.6 * (a1 - a0) && arm.r[i] > 1.25 * girth) {
                        si = i;
                        break;
                    }
            }
            shoulder = arm.line[si];
            shoulder_conf = arm.merged ? (out_wide ? 60 : 45) : 30;
            shoulder_why = arm.merged ? "where the arm swells into the body" : "the end of what could be followed of the arm";
            const Vec3 d = (to - from).normalized();
            if (upper && std::fabs(d.y) > 0.3) {
                const double oy = (s.lo.y + s.hi.y) / 2, edge = oy + (sy > 0 ? upper->v1 : upper->v0);
                const double side_y = upper->centre.y + 0.93 * (edge - upper->centre.y);
                const double t = (side_y - from.y) / d.y;
                if (t > 0 && t < 3 * (a1 - a0)) {
                    shoulder = from + d * t;
                    shoulder_why = "the upper arm's line where it meets the side of the chest";
                }
            }
        }
        shoulder_z[size_t(side)] = shoulder.z;
        put(std::string("wrist") + tag, wrist, out_wide ? wrist_conf : std::min(wrist_conf, 40), wrist_why);
        put(std::string("shoulder") + tag, shoulder, shoulder_conf, shoulder_why);
        // Elbow: 45% of the way from the wrist to the shoulder along the arm, as SL's forearm and upper arm are.
        Vec3 elbow = lerp(wrist, shoulder, 0.453);
        if (arm.line.size() >= 6) {
            std::vector<Vec3> up;
            bool on = false;
            for (const Vec3& p : arm.line) {
                if (!on && (p - wrist).length() < 1e-9) on = true;
                if (!on) continue;
                if (up.size() >= 2 && (p - shoulder).length() > (up.back() - shoulder).length()) break;  // past the shoulder
                up.push_back(p);
            }
            up.push_back(shoulder);
            if (up.size() >= 2) elbow = along(up, 0.453 * length_of(up));
        }
        put(std::string("elbow") + tag, elbow, std::min(wrist_conf, shoulder_conf) - 5,
            "between the wrist and the shoulder as SL's arm is (45% from the wrist)");
    }

    // Neck: the narrowest the middle is above the shoulders; chin: where the face juts out above the throat.
    const double sz = (shoulder_z[0] + shoulder_z[1]) / 2;
    double top = s.hi.z;
    double narrow = 1e300, nz = sz + 0.05 * H;
    for (int r = row(sz + 0.02 * H); r <= row(std::min(sz + 0.12 * H, top - 0.05 * H)); ++r)
        if (const Blob* b = middle_blob(cuts[size_t(r)]); b && b->v1 - b->v0 < narrow) narrow = b->v1 - b->v0, nz = zrow(r);
    const bool found_neck = narrow < 1e300;
    const double neck_z = sz + 0.62 * (nz - sz);
    const Blob* neck_blob = trunk(neck_z);
    put("neck", {neck_blob ? neck_blob->centre.x : 0, 0, neck_z}, found_neck ? 60 : 25,
        found_neck ? "below the narrowest point of the neck" : "from SL's proportions");
    double chin_z = nz + 0.02 * H, jump = -1e300;
    auto front_at = [&](int r) {
        const Blob* b = middle_blob(cuts[size_t(r)]);
        return b ? (s.lo.x + s.hi.x) / 2 + b->u1 : -1e300;
    };
    for (int r = row(nz - 0.01 * H) + 1; r <= row(std::min(nz + 0.07 * H, top - 0.03 * H)); ++r)
        if (const double d = front_at(r) - front_at(r - 1); d > jump) jump = d, chin_z = zrow(r);
    const double chin_front = front_at(row(chin_z));
    put("chin", {chin_front - 0.012 * H, 0, chin_z}, jump > 0.004 * H ? 55 : 30,
        jump > 0.004 * H ? "where the face juts out above the throat" : "above the neck by SL's proportions");
    const double range = std::max(top - chin_z, 0.05 * H);
    // Face: from the head's proportions as SL's are.
    if (groups.count("face")) {
        auto front = [&](double z) { return front_at(row(z)); };
        const double ez = chin_z + 0.5 * range, mz = chin_z + 0.14 * range, jz = chin_z + 0.26 * range;
        const Blob* jb = trunk(jz);
        for (int side = 0; side < 2; ++side) {
            const double sy = side == 0 ? 1 : -1;
            put(side == 0 ? "eye_l" : "eye_r", {front(ez) - 0.1 * range, sy * 0.175 * range, ez}, 35, "from the head's proportions: put it on the eye");
            put(side == 0 ? "mouth_l" : "mouth_r", {front(mz) - 0.06 * range, sy * 0.092 * range, mz}, 30,
                "from the head's proportions: put it on the corner of the mouth");
        }
        put("jaw", {jb ? jb->centre.x : 0, 0, jz}, 30, "from the head's proportions: put it where the jaw hinges, below the ear");
    }
    if (groups.count("ears")) {
        for (int side = 0; side < 2; ++side) {
            const double sy = side == 0 ? 1 : -1;
            int best = -1;
            double score = -1e300;
            for (size_t i = 0; i < s.p.size(); ++i) {
                const Vec3& p = s.p[i];
                if (p.z < chin_z + 0.3 * range || p.y * sy < 0.1 * range) continue;
                const double sc = (p.z - chin_z) + 1.5 * p.y * sy;
                if (sc > score) score = sc, best = int(i);
            }
            if (best >= 0) put(side == 0 ? "ear_l" : "ear_r", s.p[size_t(best)], 40, "the farthest point of the head on that side, high up");
        }
    }
    // Tail: the farthest point behind the hips.
    const Blob* hips = trunk(crotch + 0.03 * H);
    const double back = hips ? (s.lo.x + s.hi.x) / 2 + hips->u0 : -0.1 * H;
    if (groups.count("tail")) {
        int best = -1;
        for (size_t i = 0; i < s.p.size(); ++i)
            if (s.p[i].z > 0.1 * H && s.p[i].z < 0.85 * H && std::fabs(s.p[i].y) < 0.25 * H && (best < 0 || s.p[i].x < s.p[size_t(best)].x))
                best = int(i);
        const bool sticks = best >= 0 && back - s.p[size_t(best)].x > 0.08 * H;
        put("tail_tip", best >= 0 ? s.p[size_t(best)] : Vec3{back - 0.3 * H, 0, crotch}, sticks ? 65 : 20,
            sticks ? "the farthest point behind the hips" : "nothing sticks out behind: put it on the tail's tip");
    }
    // Wings: the farthest points out to each side behind the shoulders.
    if (wings) {
        for (int side = 0; side < 2; ++side) {
            const double sy = side == 0 ? 1 : -1;
            int best = -1;
            for (size_t i = 0; i < s.p.size(); ++i)
                if (s.p[i].x < chest_back + 0.02 * H && s.p[i].z > crotch && (best < 0 || s.p[i].y * sy > s.p[size_t(best)].y * sy))
                    best = int(i);
            const bool wide = best >= 0 && s.p[size_t(best)].y * sy > 0.2 * H;
            put(side == 0 ? "wing_l" : "wing_r", wide ? s.p[size_t(best)] : Vec3{chest_back - 0.2 * H, sy * 0.45 * H, 0.85 * H}, wide ? 50 : 20,
                wide ? "the farthest point out behind the shoulders" : "nothing spreads out behind: put it on the wing's tip");
        }
    }
    // Hind feet: pieces on the floor behind the feet.
    if (groups.count("hind")) {
        const auto& floor = cuts[size_t(row(0.01 * H))];
        const double feet_x = (out["toe_l"].pos.x + out["toe_r"].pos.x) / 2;
        for (int side = 0; side < 2; ++side) {
            const double sy = side == 0 ? 1 : -1;
            const Blob* best = nullptr;
            for (const Blob& b : floor)
                if (b.centre.y * sy > 0 && b.centre.x < feet_x - 0.2 * H && (!best || b.centre.x < best->centre.x)) best = &b;
            put(side == 0 ? "hind_l" : "hind_r", best ? Vec3{best->centre.x, best->centre.y, 0.02 * H} : Vec3{back - 0.4 * H, sy * 0.07 * H, 0.02 * H},
                best ? 55 : 20, best ? "a foot on the floor behind the others" : "no second pair of feet: put it on the hind foot");
        }
    }
    return out;
}

std::set<std::string> guess_rig_groups(const DaeModel& placed, std::vector<std::string>* why) {
    std::set<std::string> groups{"fingers", "groin"};
    auto found = [&](const char* group, const std::string& how) {
        groups.insert(group);
        if (why) why->push_back(how);
    };
    // The tail, wings and hind feet by the guesses' own tests: a confident guess is one the shape showed.
    const auto m = guess_markers(placed, {"tail", "wings", "hind"});
    auto sure = [&](const char* id) {
        const auto it = m.find(id);
        return it != m.end() && it->second.confidence >= 50;
    };
    if (sure("tail_tip")) found("tail", "Tail (something sticks out behind the hips)");
    if (sure("wing_l") && sure("wing_r")) found("wings", "Wings (something spreads out behind the shoulders)");
    if (sure("hind_l") && sure("hind_r")) found("hind", "Hind limbs (a second pair of feet stands behind the first)");
    // Ears: going down from the top, the head is two points apart, one each side, for a while before it is one piece.
    const Surface s = surface_of(placed);
    const double H = s.height();
    if (s.t.empty() || H <= 0) return groups;
    const double side = 0.015 * H;
    int apart = 0;
    for (int r = 0; r < 16; ++r) {  // the top tenth, in rows of 1/160 of the height
        bool l = false, rt = false, mid = false;
        for (const Blob& b : slice(s, s.hi.z - H * (r + 0.5) / 160, H / 140))
            (b.centre.y > side ? l : b.centre.y < -side ? rt : mid) = true;
        if (mid) break;
        if (l && rt) ++apart;
        else if (apart) break;  // above both tips (one ear a little taller), nothing yet to count
    }
    if (apart >= 3) found("ears", "Ears (the top of the head parts into two points)");
    return groups;
}

// ---------------------------------------------------------------------------------------------
// Fitting SL's skeleton

namespace {

// A chain of joints spaced along a polyline from its base (line.front()) to its tip, at fractions of its length.
void chain_along(const std::vector<Vec3>& line, const std::vector<std::string>& joints, const std::vector<double>& fraction,
                 std::map<std::string, Vec3>& out) {
    const double len = length_of(line);
    for (size_t i = 0; i < joints.size() && i < fraction.size(); ++i) out[joints[i]] = along(line, fraction[i] * len);
}

// The line from a tip back into the body, base first: the traced limb's centres, else straight. reach: how far toward
// the body (a share of the way) it runs before a swelling counts as the body: a fluffy tail or a feathered wing swells
// and thins all along, an ear joins the head soon.
std::vector<Vec3> base_to_tip(const Surface& s, const Vec3& tip, const Vec3& toward, double H, double reach) {
    Limb l = trace(s, tip, toward, 0.012 * H, H / 160, 0.1 * H, reach * (toward - tip).length());
    std::vector<Vec3> line;
    if (l.line.size() >= 3) {
        line.assign(l.line.rbegin(), l.line.rend());
        if (!l.merged) line.insert(line.begin(), toward);  // never joined: the chain runs on to where it was heading
    } else {
        line = {toward};
    }
    line.push_back(tip);
    // It starts where it leaves the body: the first surface from the body's point out to where the trace stopped (a
    // tail thick at its root swells into the body some way out).
    const Vec3 d = line.front() - toward;
    if (const double len = d.length(); len > 1e-6) {
        const Vec3 dir = d * (1 / len);
        double first = len;
        for (const auto& t : s.t) {
            const Vec3 &a = s.p[size_t(t[0])], e1 = s.p[size_t(t[1])] - a, e2 = s.p[size_t(t[2])] - a, pv = dir.cross(e2);
            const double det = e1.dot(pv);
            if (std::fabs(det) < 1e-15) continue;
            const Vec3 tv = toward - a;
            const double u = tv.dot(pv) / det;
            if (u < 0 || u > 1) continue;
            const Vec3 qv = tv.cross(e1);
            const double v = dir.dot(qv) / det, at = e2.dot(qv) / det;
            if (v >= 0 && u + v <= 1 && at > 1e-6 && at < first) first = at;
        }
        if (first < len * 0.98) line.insert(line.begin(), toward + dir * first);
    }
    return line;
}

// SL's segment lengths along a chain, as fractions of its whole length (plus a tip after the last joint).
std::vector<double> fractions(const Skeleton& skel, const std::vector<std::string>& joints, double tip) {
    std::vector<double> len;
    double total = 0;
    for (size_t i = 1; i < joints.size(); ++i) {
        const int n = skel.find(joints[i]);
        len.push_back(n >= 0 ? skel[n].pos.length() : 0.1), total += len.back();
    }
    total += tip;
    std::vector<double> f{0};
    for (double l : len) f.push_back(f.back() + l / total);
    return f;
}

}  // namespace

void place_pin(ScratchRig& rig, const std::string& joint, const Vec3& pos, bool mirror) {
    rig.pins[joint] = pos;
    if (!mirror) return;
    std::string other = joint;
    if (const size_t at = other.find("Left"); at != std::string::npos) other.replace(at, 4, "Right");
    else if (const size_t at2 = other.find("Right"); at2 != std::string::npos) other.replace(at2, 5, "Left");
    if (other != joint) rig.pins[other] = Vec3{pos.x, -pos.y, pos.z};
}

ScratchFit fit_scratch_joints(const Skeleton& skel, const DaeModel& placed, const ScratchRig& rig) {
    ScratchFit fit;
    const Surface s = surface_of(placed);
    const double H = std::max(s.height(), 1e-6);
    const std::map<std::string, Vec3> rest = sl_rest(skel);
    auto m = [&](const char* id) -> Vec3 {
        const auto it = rig.markers.find(id);
        return it != rig.markers.end() ? it->second.pos : Vec3{};
    };
    auto& j = fit.joints;
    const Vec3 hip_l = m("hip_l"), hip_r = m("hip_r"), ankle_l = m("ankle_l"), ankle_r = m("ankle_r"), neck = m("neck");
    // The body's size against SL's, from the neck to the ankles.
    const double sl_span = rest.at("mNeck").z - (rest.at("mAnkleLeft").z + rest.at("mAnkleRight").z) / 2;
    const double k = std::clamp((neck.z - (ankle_l.z + ankle_r.z) / 2) / sl_span, 0.05, 50.0);
    const Vec3 sl_pelvis = rest.at("mPelvis");
    const Vec3 hip_mid = (hip_l + hip_r) * 0.5, sl_hip_mid = (rest.at("mHipLeft") + rest.at("mHipRight")) * 0.5;
    const Vec3 pelvis = hip_mid + (sl_pelvis - sl_hip_mid) * k;
    j["mPelvis"] = pelvis;
    // The spine as SL spaces it from the pelvis to the neck (a straight line).
    const double sl_neck = rest.at("mNeck").z - sl_pelvis.z;
    j["mTorso"] = lerp(pelvis, neck, (rest.at("mTorso").z - sl_pelvis.z) / sl_neck);
    j["mChest"] = lerp(pelvis, neck, (rest.at("mChest").z - sl_pelvis.z) / sl_neck);
    j["mNeck"] = neck;
    // The head above the chin as SL's sits above its chin; the skull half way to the top of the head.
    const Vec3 chin = m("chin");
    double top = chin.z;
    for (const Vec3& p : s.p)
        if (std::hypot(p.x - chin.x, p.y - chin.y) < 0.12 * H && p.z > chin.z) top = std::max(top, p.z);
    const double range = std::max(top - chin.z, 0.02 * H);
    const Vec3 head{lerp(neck, chin, 0.1).x, neck.y, chin.z + 0.117 * range};
    j["mHead"] = head;
    j["mSkull"] = {head.x, head.y, chin.z + 0.5 * range};
    fit.tips["mSkull"] = {head.x, head.y, top};
    // A head is a ball, not a stick: its bone also reaches to the middle of the head's own bounds (a snout, a muzzle),
    // so the face is the head's, not the neck's.
    Vec3 hlo{1e300, 1e300, 1e300}, hhi{-1e300, -1e300, -1e300};
    for (const Vec3& p : s.p)
        if (p.z > chin.z && std::hypot(p.x - head.x, p.y - head.y) < 0.15 * H)
            for (int a = 0; a < 3; ++a) hlo[a] = std::min(hlo[a], p[a]), hhi[a] = std::max(hhi[a], p[a]);
    if (hlo.x < hhi.x) fit.tips["mHead"] = (hlo + hhi) * 0.5;
    // Arms.
    for (const char* side : {"Left", "Right"}) {
        const bool left = side[0] == 'L';
        const Vec3 shoulder = m(left ? "shoulder_l" : "shoulder_r"), elbow = m(left ? "elbow_l" : "elbow_r"),
                   wrist = m(left ? "wrist_l" : "wrist_r");
        const Vec3 chest = j["mChest"];
        j[std::string("mCollar") + side] = {shoulder.x, chest.y + 0.52 * (shoulder.y - chest.y), shoulder.z};
        j[std::string("mShoulder") + side] = shoulder;
        j[std::string("mElbow") + side] = elbow;
        j[std::string("mWrist") + side] = wrist;
        // The hand: the mesh past the wrist, its long axis, its palm.
        const Vec3 fore = (wrist - elbow).normalized();
        Vec3 mean;
        std::vector<Vec3> hand;
        for (const Vec3& p : s.p)
            if ((p - wrist).dot(fore) > 0 && (p - wrist).length() < 0.16 * H) hand.push_back(p), mean += p;
        double length = 0.115 * H;
        Vec3 axis = fore, normal{0, 0, -1};
        bool flat = false;
        if (hand.size() >= 8) {
            mean = mean * (1.0 / double(hand.size()));
            // The covariance's main axis by power iteration, then the flattest across it.
            double c[3][3] = {};
            for (const Vec3& p : hand)
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) c[a][b] += (p - wrist)[a] * (p - wrist)[b];
            auto power = [&](Vec3 v, const Vec3* skip) {
                for (int it = 0; it < 50; ++it) {
                    Vec3 w{c[0][0] * v.x + c[0][1] * v.y + c[0][2] * v.z, c[1][0] * v.x + c[1][1] * v.y + c[1][2] * v.z,
                           c[2][0] * v.x + c[2][1] * v.y + c[2][2] * v.z};
                    if (skip) w = w - *skip * w.dot(*skip);
                    v = w.normalized();
                    if (v.length() < 0.5) break;
                }
                return v;
            };
            axis = power(fore, nullptr);
            if (axis.dot(fore) < 0) axis = -axis;
            if (axis.dot(fore) < 0.5) axis = fore;  // a hand wider than long (a fist, a paw): along the forearm
            const Vec3 across = power(Vec3{0, 0, 1}.cross(axis).normalized(), &axis);
            normal = axis.cross(across).normalized();
            // A hand as thick as it is wide (a mitten, a paw, a rounded blob) has no palm to go by: SL's way, below.
            auto spread = [&](const Vec3& v) {
                double sum = 0;
                for (const Vec3& p : hand) sum += ((p - wrist).dot(v)) * ((p - wrist).dot(v));
                return sum;
            };
            flat = spread(across) > 1.5 * spread(normal);
            double reach = 0;
            for (const Vec3& p : hand) reach = std::max(reach, (p - wrist).dot(axis));
            if (reach > 0.03 * H) length = reach;
        }
        // SL's palm faces down as its hand reaches out: the palm's normal the side nearer SL's turned by the arm's swing.
        const Vec3 sl_axis{0, left ? 1.0 : -1.0, 0};
        const Vec3 rot_axis = sl_axis.cross(axis);
        const double angle = std::atan2(rot_axis.length(), sl_axis.dot(axis));
        const Quat swing = rot_axis.length() > 1e-9 ? Quat::axis_angle(rot_axis, angle) : Quat{};
        if (!flat) normal = swing.rotate({0, 0, -1});
        if (normal.dot(swing.rotate({0, 0, -1})) < 0) normal = -normal;
        normal = (normal - axis * normal.dot(axis)).normalized();
        if (normal.length() < 0.5) normal = swing.rotate({0, 0, -1});
        const Vec3 thumb = left ? normal.cross(axis) : axis.cross(normal);
        // SL's hand frame (long axis, palm normal, thumb side) onto the mesh's.
        const Vec3 sl_thumb{1, 0, 0}, sl_normal{0, 0, -1};
        auto to_mesh = [&](const Vec3& o) {
            return axis * o.dot(sl_axis) + normal * o.dot(sl_normal) + thumb * o.dot(sl_thumb);
        };
        const double sl_length = std::fabs(rest.at(std::string("mHandMiddle3") + side).y - rest.at(std::string("mWrist") + side).y) + 0.025;
        const double scale = length / sl_length;
        if (rig.groups.count("fingers")) {
            for (const char* f : {"Thumb", "Index", "Middle", "Ring", "Pinky"}) {
                Vec3 last, before;
                for (int seg = 1; seg <= 3; ++seg) {
                    const std::string name = std::string("mHand") + f + std::to_string(seg) + side;
                    const Vec3 o = rest.at(name) - rest.at(std::string("mWrist") + side);
                    before = last;
                    last = wrist + to_mesh(o) * scale;
                    j[name] = last;
                }
                fit.tips[std::string("mHand") + f + "3" + side] = last + (last - before) * 0.8;
            }
        } else {
            fit.tips[std::string("mWrist") + side] = wrist + axis * length;
        }
        // Legs.
        const char* t = left ? "_l" : "_r";
        const Vec3 hip = m((std::string("hip") + t).c_str()), knee = m((std::string("knee") + t).c_str()),
                   ankle = m((std::string("ankle") + t).c_str()), toe = m((std::string("toe") + t).c_str());
        j[std::string("mHip") + side] = hip;
        j[std::string("mKnee") + side] = knee;
        j[std::string("mAnkle") + side] = ankle;
        const Vec3 foot{lerp(ankle, toe, 0.507).x, lerp(ankle, toe, 0.507).y, toe.z};
        j[std::string("mFoot") + side] = foot;
        j[std::string("mToe") + side] = toe;
        fit.tips[std::string("mToe") + side] = toe + (toe - foot) * 0.3;
    }
    if (rig.groups.count("groin")) j["mGroin"] = m("groin");
    // Face: SL's face joints scaled per axis to the face markers (least squares), moved onto them.
    if (rig.groups.count("face")) {
        const std::vector<std::pair<const char*, std::string>> pairs = {
            {"eye_l", "mEyeLeft"}, {"eye_r", "mEyeRight"}, {"mouth_l", "mFaceLipCornerRight"}, {"mouth_r", "mFaceLipCornerLeft"}, {"jaw", "mFaceJaw"}};
        Vec3 ms, rs;
        for (const auto& [id, joint] : pairs) ms += m(id), rs += rest.at(joint);
        ms = ms * (1.0 / double(pairs.size())), rs = rs * (1.0 / double(pairs.size()));
        Vec3 scale;
        int axes = 0;
        double sum_scale = 0;
        for (int a = 0; a < 3; ++a) {
            double num = 0, den = 0;
            for (const auto& [id, joint] : pairs) num += (rest.at(joint)[a] - rs[a]) * (m(id)[a] - ms[a]), den += (rest.at(joint)[a] - rs[a]) * (rest.at(joint)[a] - rs[a]);
            scale[a] = den > 1e-12 ? num / den : -1;
            if (scale[a] > 0) sum_scale += scale[a], ++axes;
        }
        for (int a = 0; a < 3; ++a) scale[a] = std::clamp(scale[a] > 0 ? scale[a] : axes ? sum_scale / axes : k, 0.3 * k, 3 * k);
        const int head_node = skel.find("mHead");
        for (int n = 0; n < skel.joint_count(); ++n) {
            const std::string& name = skel[n].name;
            bool under_head = false;
            for (int a = skel[n].parent; a >= 0; a = skel[a].parent) under_head = under_head || a == head_node;
            if (!under_head || name == "mSkull" || name.rfind("mFaceEar", 0) == 0) continue;
            j[name] = ms + (rest.at(name) - rs).mul(scale);
        }
        for (const auto& [id, joint] : pairs) j[joint] = m(id);  // exactly where they were put
        fit.notes.push_back("face: SL's face scaled to the markers, " + std::to_string(int(std::lround(scale.x / k * 100))) + "% deep, " +
                            std::to_string(int(std::lround(scale.y / k * 100))) + "% wide, " +
                            std::to_string(int(std::lround(scale.z / k * 100))) + "% tall against the body");
    }
    // Ears, tail, wings and hind limbs along the mesh from their tips.
    if (rig.groups.count("ears"))
        for (const char* side : {"Left", "Right"}) {
            const Vec3 tip = m(side[0] == 'L' ? "ear_l" : "ear_r");
            const Vec3 centre{head.x, head.y, chin.z + 0.5 * range};
            const std::vector<Vec3> line = base_to_tip(s, tip, centre, H, 0.3);
            chain_along(line, {std::string("mFaceEar1") + side, std::string("mFaceEar2") + side}, {0, 0.5}, j);
            fit.tips[std::string("mFaceEar2") + side] = tip;
        }
    if (rig.groups.count("tail")) {
        const Vec3 tip = m("tail_tip");
        const std::vector<std::string> names = {"mTail1", "mTail2", "mTail3", "mTail4", "mTail5", "mTail6"};
        const std::vector<Vec3> line = base_to_tip(s, tip, pelvis, H, 0.6);
        chain_along(line, names, fractions(skel, names, 0.08), j);
        fit.tips["mTail6"] = tip;
    }
    if (rig.groups.count("wings")) {
        const Vec3 chest = j["mChest"];
        j["mWingsRoot"] = chest + (rest.at("mWingsRoot") - rest.at("mChest")) * k;
        for (const char* side : {"Left", "Right"}) {
            const bool left = side[0] == 'L';
            const Vec3 tip = m(left ? "wing_l" : "wing_r");
            const Vec3 base = chest + Vec3{-0.06 * H, (left ? 1 : -1) * 0.05 * H, 0.05 * H};
            const std::vector<std::string> names = {std::string("mWing1") + side, std::string("mWing2") + side,
                                                    std::string("mWing3") + side, std::string("mWing4") + side};
            const std::vector<Vec3> line = base_to_tip(s, tip, base, H, 0.5);
            chain_along(line, names, fractions(skel, names, 0.2), j);
            j[std::string("mWing4Fan") + side] = j[std::string("mWing4") + side];
            fit.tips[std::string("mWing4") + side] = tip;
        }
    }
    if (rig.groups.count("hind")) {
        Vec3 roots;
        for (const char* side : {"Left", "Right"}) {
            const bool left = side[0] == 'L';
            const Vec3 tip = m(left ? "hind_l" : "hind_r");
            const Vec3 base = pelvis + Vec3{-0.2 * k, (left ? 1 : -1) * 0.1 * k, 0};
            const std::vector<std::string> names = {std::string("mHindLimb1") + side, std::string("mHindLimb2") + side,
                                                    std::string("mHindLimb3") + side, std::string("mHindLimb4") + side};
            const std::vector<Vec3> line = base_to_tip(s, tip, base, H, 0.5);
            chain_along(line, names, fractions(skel, names, 0.109), j);
            fit.tips[std::string("mHindLimb4") + side] = tip;
            roots += j[names[0]];
        }
        j["mHindLimbsRoot"] = roots * 0.5 + (rest.at("mHindLimbsRoot") - rest.at("mHindLimb1Left")).mul({1, 0, 1}) * k;
    }
    // Pins last, parents first: a pinned joint goes where you put it and the joints below it move with it, so dragging a
    // knuckle brings its finger and a pinned fingertip below it still lands on its own pin. The carry stops at a joint a
    // marker places: a pinned pelvis brings the spine, but the hips stay on their markers.
    std::set<std::string> marked;
    for (const RigMarkerDef& d : rig_marker_defs())
        if (rig.markers.count(d.id) && (!d.group[0] || rig.groups.count(d.group))) marked.insert(d.joint);
    for (int n = 0; n < skel.size(); ++n) {
        const auto pin = rig.pins.find(skel[n].name);
        const auto at = j.find(skel[n].name);
        if (pin == rig.pins.end() || at == j.end()) continue;
        const Vec3 delta = pin->second - at->second;
        std::vector<int> todo{n};
        while (!todo.empty()) {
            const int node = todo.back();
            todo.pop_back();
            if (node != n && marked.count(skel[node].name)) continue;
            if (auto it = j.find(skel[node].name); it != j.end()) it->second += delta;
            if (auto it = fit.tips.find(skel[node].name); it != fit.tips.end()) it->second += delta;
            for (int c : skel[node].children) todo.push_back(c);
        }
    }
    return fit;
}

bool scratch_weighted_joint(const Skeleton& skel, const std::string& joint, const std::set<std::string>& groups) {
    static const std::set<std::string> body = {
        "mPelvis", "mTorso", "mChest", "mNeck", "mHead", "mCollarLeft", "mCollarRight", "mShoulderLeft", "mShoulderRight",
        "mElbowLeft", "mElbowRight", "mWristLeft", "mWristRight", "mHipLeft", "mHipRight", "mKneeLeft", "mKneeRight",
        "mAnkleLeft", "mAnkleRight", "mFootLeft", "mFootRight", "mToeLeft", "mToeRight"};
    if (body.count(joint)) return true;
    const int n = skel.find(joint);
    if (n < 0 || n >= skel.joint_count()) return false;
    auto starts = [&](const char* p) { return joint.rfind(p, 0) == 0; };
    if (joint == "mGroin") return groups.count("groin") > 0;
    if (starts("mHand")) return groups.count("fingers") > 0;
    if (starts("mTail")) return groups.count("tail") > 0;
    if (starts("mWing")) return groups.count("wings") > 0 && joint != "mWingsRoot" && !starts("mWing4Fan");
    if (starts("mHindLimb")) return groups.count("hind") > 0 && joint != "mHindLimbsRoot";
    if (starts("mFaceEar")) return groups.count("ears") > 0;
    if (starts("mEye") || starts("mFace"))
        return groups.count("face") > 0 && joint != "mFaceRoot" && joint != "mFaceJawShaper" && !starts("mFaceEyeAlt");
    return false;
}

std::vector<HeatBone> scratch_heat_bones(const Skeleton& skel, const ScratchFit& fit, const std::set<std::string>& groups) {
    std::vector<HeatBone> bones;
    for (const auto& [name, at] : fit.joints) {
        if (!scratch_weighted_joint(skel, name, groups)) continue;
        const int node = skel.find(name);
        if (node < 0 || node >= skel.joint_count()) continue;
        // Its placed child joints, looking through the joints the rig does not place (the spine's extra ones).
        std::vector<Vec3> ends;
        std::function<void(int)> down = [&](int n) {
            for (int c : skel[n].children) {
                if (c >= skel.joint_count()) continue;
                const auto it = fit.joints.find(skel[c].name);
                if (it != fit.joints.end()) ends.push_back(it->second);
                else down(c);
            }
        };
        down(node);
        if (const auto tip = fit.tips.find(name); tip != fit.tips.end()) ends.push_back(tip->second);
        if (ends.empty()) {
            // A leaf with no tip: half its parent's bone again along it, or a point (a face joint).
            Vec3 tail = at;
            for (int p = skel[node].parent; p >= 0; p = skel[p].parent)
                if (const auto it = fit.joints.find(skel[p].name); it != fit.joints.end()) {
                    if (skel[node].category != Category::Face) tail = at + (at - it->second) * 0.5;
                    break;
                }
            ends.push_back(tail);
        }
        for (const Vec3& e : ends) bones.push_back({node, at, e});
    }
    return bones;
}

void rig_from_scratch(const Skeleton& skel, DaeModel& m, const ScratchRig& rig) {
    const int count = dae_index_count(skel), root = dae_root(skel);
    std::vector<Xform> rest = skel.global_pose(Pose(size_t(skel.size())));
    rest.push_back({});
    for (const CollisionVolume& v : skel.volumes()) rest.push_back(rest[size_t(v.joint)] * Xform{v.rot, v.pos});
    m.binds = rest;
    m.bound.assign(size_t(count), false);
    for (const auto& [name, at] : rig.joints) {
        const int n = skel.find(name);
        if (n < 0 || n >= skel.joint_count()) continue;
        m.binds[size_t(n)] = {Quat{}, at};
        m.bound[size_t(n)] = true;
    }
    // Each collision volume keeps SL's place against its joint, wherever the joint now is.
    for (size_t v = 0; v < skel.volumes().size(); ++v) {
        const CollisionVolume& cv = skel.volumes()[v];
        if (!m.bound[size_t(cv.joint)]) continue;
        const int i = dae_volume(skel, int(v));
        m.binds[size_t(i)] = {rest[size_t(i)].rot, m.binds[size_t(cv.joint)].pos + (rest[size_t(i)].pos - rest[size_t(cv.joint)].pos)};
        m.bound[size_t(i)] = true;
    }
    m.rigged = true;
    m.rig_axes.clear();
    m.turn_binds = m.turn_vertices = 0;
    m.turn_decided = true;
    const int nv = m.vertex_count();
    if (rig.weighted(nv)) {
        m.joints = rig.wjoints;
        m.weights = rig.weights;
        return;
    }
    // No weights yet: each vertex on its nearest bone, for a preview.
    ScratchFit fit;
    fit.joints = rig.joints;
    const std::vector<HeatBone> bones = scratch_heat_bones(skel, fit, rig.groups);
    m.joints.assign(size_t(nv) * 4, root);
    m.weights.assign(size_t(nv) * 4, 0.f);
    if (bones.empty()) {
        const int pelvis = skel.find("mPelvis");
        for (int v = 0; v < nv; ++v) m.joints[size_t(v) * 4] = pelvis, m.weights[size_t(v) * 4] = 1;
        return;
    }
    for (int v = 0; v < nv; ++v) {
        const Vec3 p{m.positions[size_t(v) * 3], m.positions[size_t(v) * 3 + 1], m.positions[size_t(v) * 3 + 2]};
        double best = 1e300;
        for (const HeatBone& b : bones)
            if (const double d = (closest_on_segment(p, b.a, b.b) - p).length(); d < best) best = d, m.joints[size_t(v) * 4] = b.node;
        m.weights[size_t(v) * 4] = 1;
    }
}

std::map<std::string, std::string> suggest_transfers(const DaeModel& m) {
    std::map<std::string, std::string> out;
    const double reach = 0.02 * (m.bounds_max.z - m.bounds_min.z);
    for (const DaePart& p : m.parts) {
        double best = 0.9;
        for (const DaePart& q : m.parts) {
            if (&q == &p || q.vertex_count <= p.vertex_count || q.name == p.name) continue;
            if (const double c = part_coverage(m.positions, m.indices, q, p, reach); c >= best) best = c, out[p.name] = q.name;
        }
    }
    // A chain of copies (a patch on a vest on a body) copies from the end of the chain.
    for (auto& [part, from] : out)
        for (int i = 0; i < 8 && out.count(from); ++i) from = out[from];
    return out;
}

void share_with_volumes(const Skeleton& skel, DaeModel& m) {
    const int root = dae_root(skel), nv = m.vertex_count();
    if (!m.rigged || m.joints.size() != size_t(nv) * 4) return;
    std::vector<std::vector<int>> of(size_t(skel.joint_count()));  // per joint, its volumes (SK-40)
    for (size_t v = 0; v < skel.volumes().size(); ++v)
        if (skel.volumes()[v].joint < skel.joint_count() && m.bound[size_t(dae_volume(skel, int(v)))])
            of[size_t(skel.volumes()[v].joint)].push_back(dae_volume(skel, int(v)));
    for (int v = 0; v < nv; ++v) {
        const Vec3 p{m.positions[size_t(v) * 3], m.positions[size_t(v) * 3 + 1], m.positions[size_t(v) * 3 + 2]};
        std::vector<std::pair<int, double>> w;
        for (int k = 0; k < 4; ++k) {
            const int j = m.joints[size_t(v) * 4 + size_t(k)];
            const double x = m.weights[size_t(v) * 4 + size_t(k)];
            if (x <= 0) continue;
            if (j < 0 || j >= skel.joint_count() || of[size_t(j)].empty()) {
                w.push_back({j, x});
                continue;
            }
            std::vector<std::pair<int, double>> touching;
            double most = 0, sum = 0;
            for (int vol : of[size_t(j)]) {
                const CollisionVolume& cv = skel.volumes()[size_t(vol - root - 1)];
                const Vec3 local = m.binds[size_t(vol)].inverse().apply(p);
                const double r = Vec3{local.x / cv.scale.x, local.y / cv.scale.y, local.z / cv.scale.z}.length();
                const double f = std::clamp(2 - r, 0.0, 1.0);
                if (f > 0) touching.push_back({vol, f}), most = std::max(most, f), sum += f;
            }
            w.push_back({j, x * (1 - most)});
            for (const auto& [vol, f] : touching) w.push_back({vol, x * most * f / sum});
        }
        keep_four(w, root, 0.005, &m.joints[size_t(v) * 4], &m.weights[size_t(v) * 4]);
    }
}

bool weigh_scratch_rig(const Skeleton& skel, DaeModel& m, ScratchRig& rig, const BoneHeatOptions& opt, ScratchWeighReport& report) {
    const auto t0 = std::chrono::steady_clock::now();
    report = {};
    const ScratchFit fit = fit_scratch_joints(skel, m, rig);
    rig.joints = fit.joints;
    ScratchRig unweighted = rig;
    unweighted.wjoints.clear(), unweighted.weights.clear();
    rig_from_scratch(skel, m, unweighted);
    const std::vector<HeatBone> bones = scratch_heat_bones(skel, fit, rig.groups);
    if (bones.empty()) return report.lines.push_back("no joints to weigh to"), false;
    const int root = dae_root(skel), nv = m.vertex_count();
    // How far the body reaches from its bones: three times the median vertex's distance to its nearest bone, and at
    // least an eighth of the height. Beyond it a vertex is out in the air (a streamer), not body (see BoneHeatOptions).
    double reach = 0.12 * (m.bounds_max.z - m.bounds_min.z);
    if (opt.reach == 0) {
        std::vector<double> d;
        for (int v = 0; v < nv; v += std::max(1, nv / 20000)) {
            const Vec3 p{m.positions[size_t(v) * 3], m.positions[size_t(v) * 3 + 1], m.positions[size_t(v) * 3 + 2]};
            double best = 1e300;
            for (const HeatBone& b : bones) best = std::min(best, (closest_on_segment(p, b.a, b.b) - p).length());
            d.push_back(best);
        }
        if (!d.empty()) {
            std::nth_element(d.begin(), d.begin() + long(d.size() / 2), d.end());
            reach = std::max(reach, 3 * d[d.size() / 2]);
        }
    } else {
        reach = opt.reach;
    }
    std::vector<DaePart> parts = m.parts;
    if (parts.empty()) parts.push_back({"", 0, std::uint32_t(nv), 0, std::uint32_t(m.indices.size())});
    // The part p copies its weights from: the end of its chain of copies (a patch on a vest on a body), never one
    // without triangles of its own to copy from (then p is weighed by heat itself).
    auto transfer_of = [&](const DaePart& p) -> const DaePart* {
        std::string from = p.name;
        for (int hops = 0; hops < 8; ++hops) {
            const auto it = rig.transfer.find(from);
            if (it == rig.transfer.end() || it->second.empty() || it->second == from) break;
            from = it->second;
        }
        if (from == p.name) return nullptr;
        for (const DaePart& q : parts)
            if (q.name == from && q.index_count >= 3 && &q != &p) return &q;
        return nullptr;
    };
    // Parts that share a seam (a vertex at one place) are solved together.
    std::vector<int> group(parts.size());
    std::iota(group.begin(), group.end(), 0);
    auto find = [&](int i) {
        while (group[size_t(i)] != i) i = group[size_t(i)] = group[size_t(group[size_t(i)])];
        return i;
    };
    {
        const double q = std::max((m.bounds_max - m.bounds_min).length(), 1e-9) * 1e-6;
        std::map<std::array<long long, 3>, int> owner;
        for (size_t pi = 0; pi < parts.size(); ++pi) {
            if (transfer_of(parts[pi])) continue;
            for (std::uint32_t v = parts[pi].first_vertex; v < parts[pi].first_vertex + parts[pi].vertex_count && v < std::uint32_t(nv); ++v) {
                const std::array<long long, 3> key{std::llround(m.positions[v * 3] / q), std::llround(m.positions[v * 3 + 1] / q),
                                                   std::llround(m.positions[v * 3 + 2] / q)};
                const auto [it, fresh] = owner.try_emplace(key, int(pi));
                if (!fresh && find(it->second) != find(int(pi))) group[size_t(find(int(pi)))] = find(it->second);
            }
        }
    }
    m.joints.assign(size_t(nv) * 4, root);
    m.weights.assign(size_t(nv) * 4, 0.f);
    double done = 0;
    for (size_t pi = 0; pi < parts.size(); ++pi) {
        if (transfer_of(parts[pi]) || find(int(pi)) != int(pi)) continue;
        std::vector<std::uint32_t> piece;
        std::string names;
        for (size_t pj = 0; pj < parts.size(); ++pj) {
            if (transfer_of(parts[pj]) || find(int(pj)) != int(pi)) continue;
            names += (names.empty() ? "" : " + ") + (parts[pj].name.empty() ? std::string("the mesh") : parts[pj].name);
            for (std::uint32_t v = parts[pj].first_vertex; v < parts[pj].first_vertex + parts[pj].vertex_count && v < std::uint32_t(nv); ++v)
                piece.push_back(v);
        }
        if (piece.empty()) continue;
        BoneHeatOptions o = opt;
        o.reach = reach;
        const double share = double(piece.size()) / std::max(nv, 1);
        o.progress = [&, names, share](double f, const std::string& what) {
            if (opt.progress) opt.progress(done + share * f, names + ": " + what);
        };
        BoneHeatStats st;
        if (!bone_heat(m.positions, m.indices, piece, bones, root, o, m.joints, m.weights, &st)) {
            if (opt.cancel && opt.cancel->load()) return report.lines.push_back("cancelled"), false;
            report.lines.push_back(names + ": not weighed");
            continue;
        }
        done += share;
        report.stats.push_back(st);
        std::string line = names + ": bone heat, " + std::to_string(st.vertices) + " vertices in " + std::to_string(st.pieces) +
                           (st.pieces == 1 ? " piece" : " pieces");
        if (st.hidden) line += "; " + std::to_string(st.hidden) + " behind a fold or a layer took their weights from around them";
        if (st.out_of_reach) line += "; " + std::to_string(st.out_of_reach) + " out in the air (streamers, hems) took them from where they hang";
        if (st.unreached)
            line += "; " + std::to_string(st.unreached) +
                    " in pieces no bone can see (claws, eyes, buckles, shut inside) were warmed by their nearest bones regardless";
        if (st.failed) line += "; the solve failed, so every vertex went to its nearest bone";
        report.lines.push_back(line);
    }
    for (const DaePart& p : parts)
        if (const DaePart* from = transfer_of(p)) {
            if (transfer_weights(m.positions, m.indices, *from, p, root, m.joints, m.weights))
                report.lines.push_back(p.name + ": weights copied from " + from->name + " (the nearest point of its surface)");
            else
                report.lines.push_back(p.name + ": could not copy from " + from->name + " (it has no triangles)");
        }
    if (rig.fitted) {
        share_with_volumes(skel, m);
        report.lines.push_back("fitted mesh: shared with the collision volumes");
    }
    rig.wjoints = m.joints;
    rig.weights = m.weights;
    rig.painted = false;
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (opt.progress) opt.progress(1, "done");
    return true;
}

}  // namespace vats
