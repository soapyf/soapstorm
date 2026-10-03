// Viewport Avatar Toolset - the SL rig export. See vats/rig_export.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/rig_export.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <array>
#include <map>
#include <set>

#include "affine.h"
#include "vats/rig_map.h"
#include "vats/xml.h"

namespace vats {
namespace {

constexpr int kMaxJoints = 110;          // lljoint.h LL_MAX_JOINTS_PER_MESH_OBJECT
constexpr double kThresholdM = 0.0001;   // LL_JOINT_TRESHOLD_POS_OFFSET: 0.1 mm
constexpr double kNoiseM = 0.001;        // up to 1 mm: a false offset, most likely
constexpr double kNudgeM = 0.00011;      // RG-10: just over the threshold, so the joint is listed and its scale locks
constexpr double kSoleTolM = 0.01;       // floating or sinking under 1 cm goes unseen
constexpr int kMaxFaces = 8;             // LL_SCULPT_MESH_MAX_FACES: more are split into sub-models
constexpr int kMaxFaceVertices = 65534;  // lldaeloader's 16-bit index limit: the uploader refuses the model

// The model's materials in first-use order: each is one face of the upload, whatever parts its triangles are in.
std::vector<int> materials_used(const DaeModel& m) {
    std::vector<int> out;
    for (const DaeGroup& g : m.groups)
        if (std::find(out.begin(), out.end(), g.material) == out.end()) out.push_back(g.material);
    return out;
}

std::vector<bool> weighted_nodes(const Skeleton& skel, const DaeModel& model) {
    const int count = dae_index_count(skel), root = dae_root(skel);
    std::vector<bool> w(count, false);
    for (size_t k = 0; k + 1 <= model.joints.size() && k < model.weights.size(); ++k) {
        const int j = model.joints[k];
        if (model.weights[k] > 0 && j >= 0 && j < count && j != root) w[j] = true;
    }
    return w;
}

// Shortest-arc rotation taking unit u onto unit v.
Quat arc(const Vec3& u, const Vec3& v) {
    const double d = u.dot(v);
    if (d < -0.999999) return Quat::axis_angle(std::fabs(u.x) < 0.9 ? u.cross({1, 0, 0}) : u.cross({0, 1, 0}), kPi);
    const Vec3 c = u.cross(v);
    return Quat{1 + d, c.x, c.y, c.z}.normalized();
}

// The bound joints the bone of j points at: the next weighted, bound joints down each branch.
std::vector<int> next_weighted(const Skeleton& skel, const DaeModel& m, const std::vector<bool>& weighted, int j) {
    std::vector<int> out;
    std::function<void(int)> walk = [&](int n) {
        for (int c : skel[n].children) {
            if (c >= skel.joint_count()) continue;
            if (c < int(m.bound.size()) && m.bound[c] && weighted[c]) out.push_back(c);
            else walk(c);
        }
    };
    walk(j);
    return out;
}

// The turn (in the joint's bind frame) from SL's rest direction of j's bone onto the file's, when j's bone points at
// one weighted joint; identity otherwise.
Quat bone_swing(const Skeleton& skel, const DaeModel& m, const std::vector<bool>& weighted, const std::vector<Xform>& rest, int j) {
    const std::vector<int> next = next_weighted(skel, m, weighted, j);
    if (next.size() != 1) return {};
    const int c = next[0];
    const Vec3 want = rest[j].rot.conj().rotate(rest[c].pos - rest[j].pos);
    const Vec3 have = m.binds[j].rot.conj().rotate(m.binds[c].pos - m.binds[j].pos);
    if (want.length() < 0.005 || have.length() < 0.005) return {};
    return arc(want.normalized(), have.normalized());
}

// RG-9 "Stand it in SL's rest pose": every bound joint's bind turns by the swing from SL's rest direction of its bone
// onto the file's, and a joint whose bone does not point at one weighted joint (a wrist, the chest, the head, a leaf)
// turns with its nearest bound ancestor. The joint positions written then stand the bones along SL's rest directions
// (a T-pose) at the file's lengths, and the inverse binds carry the pose it was modelled in (an A-pose), as for an
// SL-named body bound in an A-pose: SL skins the mesh into its own rest pose, and animations made for it fit.
std::vector<Xform> rest_pose_binds(const Skeleton& skel, const DaeModel& m, const std::vector<Xform>& rest) {
    std::vector<Xform> b = m.binds;
    const std::vector<bool> weighted = weighted_nodes(skel, m);
    auto bound = [&](int i) { return i < int(m.bound.size()) && m.bound[i]; };
    std::vector<Quat> turn(size_t(skel.joint_count()));  // the global turn each joint's bind takes
    for (int j = 0; j < skel.joint_count(); ++j) {
        const int par = skel[j].parent;
        turn[j] = par >= 0 ? turn[par] : Quat{};
        if (!bound(j)) continue;
        const Quat s = bone_swing(skel, m, weighted, rest, j);
        if (s.angle() > 1e-9) turn[j] = m.binds[j].rot * s * m.binds[j].rot.conj();
        b[j].rot = (turn[j] * m.binds[j].rot).normalized();
    }
    for (size_t v = 0; v < skel.volumes().size(); ++v)
        if (const int i = dae_volume(skel, int(v)); bound(i)) b[i].rot = (turn[skel.volumes()[v].joint] * m.binds[i].rot).normalized();
    return b;
}

// RG-11: the uploader keeps skin weights by vertex position, not per vertex (lldaeloader.cpp stores
// mSkinWeights[position]; LLModel::getJointInfluences hands each vertex the first weights within 1e-5 of it, after
// normalizeVolumeFaces has scaled the model's bounds to a unit cube, per axis). Vertices that close together but
// weighted differently get one another's weights. Per vertex: 0, or the rank (1, 2...) of its weights among the
// different weights at that position; empty when none clash. size gets the model's bounds.
constexpr double kUploaderEps = 1e-5;
std::vector<int> weight_clashes(const DaeModel& m, int root, Vec3& size) {
    const size_t nv = std::min(m.positions.size() / 3, std::min(m.joints.size(), m.weights.size()) / 4);
    if (!nv) return {};
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    auto at = [&](size_t v) { return Vec3{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]}; };
    for (size_t v = 0; v < nv; ++v)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], at(v)[k]), hi[k] = std::max(hi[k], at(v)[k]);
    for (int k = 0; k < 3; ++k) size[k] = hi[k] - lo[k] < 1e-5 ? 1 : hi[k] - lo[k];  // as normalizeVolumeFaces
    auto unit = [&](size_t v) { const Vec3 p = at(v) - lo; return Vec3{p.x / size.x, p.y / size.y, p.z / size.z}; };
    // The weights as written: normalised over the real joints.
    auto weights = [&](size_t v) {
        std::vector<std::pair<int, float>> w;
        float total = 0;
        for (int k = 0; k < 4; ++k)
            if (const int j = m.joints[v * 4 + k]; j >= 0 && j != root && m.weights[v * 4 + k] > 0) w.push_back({j, m.weights[v * 4 + k]}), total += m.weights[v * 4 + k];
        for (auto& x : w) x.second /= total;
        std::sort(w.begin(), w.end());
        return w;
    };
    auto same = [](const std::vector<std::pair<int, float>>& a, const std::vector<std::pair<int, float>>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].first != b[i].first || std::fabs(a[i].second - b[i].second) > 1e-3f) return false;
        return true;
    };
    // Neighbours within the uploader's distance, through a grid of that cell size.
    std::map<std::array<long long, 3>, std::vector<int>> grid;
    auto cell = [&](const Vec3& u) {
        return std::array<long long, 3>{(long long)std::floor(u.x / kUploaderEps), (long long)std::floor(u.y / kUploaderEps),
                                        (long long)std::floor(u.z / kUploaderEps)};
    };
    for (size_t v = 0; v < nv; ++v) grid[cell(unit(v))].push_back(int(v));
    std::vector<int> group(nv);
    for (size_t v = 0; v < nv; ++v) group[v] = int(v);
    std::function<int(int)> top = [&](int v) { return group[v] == v ? v : group[v] = top(group[v]); };
    bool close_pairs = false;
    for (size_t v = 0; v < nv; ++v) {
        const Vec3 u = unit(v);
        const auto c = cell(u);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                for (long long dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find({c[0] + dx, c[1] + dy, c[2] + dz});
                    if (it == grid.end()) continue;
                    for (int o : it->second)
                        if (o > int(v) && (unit(size_t(o)) - u).length() < kUploaderEps) group[top(o)] = top(int(v)), close_pairs = true;
                }
    }
    if (!close_pairs) return {};
    // Within each group of close vertices, the first weights keep rank 0; each different set of weights the next rank.
    std::map<int, std::vector<std::vector<std::pair<int, float>>>> seen;
    std::vector<int> rank(nv, 0);
    bool any = false;
    for (size_t v = 0; v < nv; ++v) {
        auto& sets = seen[top(int(v))];
        const auto w = weights(v);
        size_t r = 0;
        while (r < sets.size() && !same(sets[r], w)) ++r;
        if (r == sets.size()) sets.push_back(w);
        rank[v] = int(r);
        any |= r > 0;
    }
    if (!any) return {};
    // Every vertex of a group with more than one set counts, rank 0 included (-1 marks those).
    for (size_t v = 0; v < nv; ++v)
        if (rank[v] == 0 && seen[top(int(v))].size() > 1) rank[v] = -1;
    return rank;
}

// Where the file's joints are written: the "as uploaded" globals, computed the way shape_from_binds places a body.
struct Placement {
    std::vector<Xform> g;          // as-written global of every SK-40 index (rest rotations)
    std::vector<Vec3> local;       // the translation written on each joint node
    std::vector<Vec3> def;         // SL's default local
    std::vector<bool> positioned;  // its bind position is written (bound, and the options allow it)
    std::vector<Xform> bind;       // the bind the inverse bind matrix inverts (a volume's without its scale)
    std::vector<bool> nudged;      // RG-10: moved kNudgeM off its default only so its scale locks
};

Placement place(const Skeleton& skel, const DaeModel& model, const RigExportOptions& opt) {
    const int n = skel.size(), root = dae_root(skel), count = dae_index_count(skel);
    Placement p;
    p.g.assign(count, {});
    p.local.assign(count, {});
    p.def.assign(count, {});
    p.positioned.assign(count, false);
    std::vector<Xform> rest = skel.global_pose(Pose(n));
    rest.push_back({});
    for (auto& v : skel.volumes()) rest.push_back(rest[v.joint] * Xform{v.rot, v.pos});
    p.bind = rest;
    const bool ok = model.rigged && model.binds.size() == size_t(count);
    const std::vector<Xform> binds = ok && opt.rest_pose && opt.joint_positions && !opt.bind_pose_only ? rest_pose_binds(skel, model, rest) : model.binds;
    auto bound = [&](int i) { return ok && i >= 0 && i < int(model.bound.size()) && model.bound[i]; };
    auto positions = [&](int i) { return bound(i) && opt.joint_positions && !opt.bind_pose_only && (i != 0 || opt.pelvis_offset); };
    // RG-10 shape-proof: SL locks a joint's scale against the shape sliders only when the mesh moves it over 0.1 mm
    // (LLVOAvatar::addAttachmentOverridesForObject, with Lock scale if joint position defined). So every joint the
    // file lists and every joint above one get a position, kNudgeM off the default where they sit on it.
    p.nudged.assign(count, false);
    std::vector<bool> lock;
    if (ok && opt.shape_proof && opt.joint_positions && !opt.bind_pose_only) {
        RigExportOptions plain = opt;
        plain.shape_proof = false;
        const Placement first = place(skel, model, plain);
        const std::vector<bool> w = weighted_nodes(skel, model);
        lock.assign(size_t(skel.joint_count()), false);
        for (int j = skel.joint_count() - 1; j >= 0; --j)
            if (w[j] || (first.local[j] - first.def[j]).length() > kThresholdM)
                for (int a = j; a >= 0 && !lock[a]; a = skel[a].parent) lock[a] = true;
    }
    // The global a bound node is written at: its offset from its nearest bound ancestor, in that ancestor's bind
    // frame, re-applied in the ancestor's as-written (rest) frame. A body modelled in an A-pose keeps its bone
    // lengths along SL's rest axes, as SL reads joint positions.
    auto anchored_at = [&](int node, int first_ancestor) {
        int a = first_ancestor;
        while (a >= 0 && !bound(a)) a = skel[a].parent;
        const Vec3 b = binds[node].pos;
        if (a < 0) return b;
        return p.g[a].apply(binds[a].rot.conj().rotate(b - binds[a].pos));
    };
    for (int j = 0; j < skel.joint_count(); ++j) {
        const Node& node = skel[j];
        const int par = node.parent;
        p.def[j] = node.pos;
        p.positioned[j] = positions(j);
        Vec3 local = node.pos;
        if (p.positioned[j]) {
            const Vec3 at = anchored_at(j, par);
            local = par >= 0 ? p.g[par].inverse().apply(at) : at;
        }
        if (!lock.empty() && lock[j] && (local - node.pos).length() <= kThresholdM) {
            const Vec3 dir = node.pos.length() > 1e-9 ? node.pos.normalized() : Vec3{0, 0, 1};
            local = node.pos + dir * kNudgeM;
            p.positioned[j] = p.nudged[j] = true;
        }
        p.local[j] = local;
        const Xform l{node.rest, local};
        p.g[j] = par >= 0 ? p.g[par] * l : l;
        if (bound(j)) p.bind[j] = binds[j];
    }
    for (int j = skel.joint_count(); j < n; ++j) p.g[j] = rest[j];  // attachment points: never rigged to
    p.g[root] = {};
    const auto& vols = skel.volumes();
    for (size_t v = 0; v < vols.size(); ++v) {
        const int i = dae_volume(skel, int(v)), jv = vols[v].joint;
        p.def[i] = vols[v].pos;
        p.positioned[i] = positions(i);
        Vec3 local = vols[v].pos;
        if (p.positioned[i]) local = p.g[jv].inverse().apply(anchored_at(i, jv));
        p.local[i] = local;
        p.g[i] = p.g[jv] * Xform{vols[v].rot, local};
        if (bound(i)) p.bind[i] = binds[i];
    }
    return p;
}

// The one skeleton the file carries. When the parts disagree on a joint (each bound it elsewhere), the first part's
// position is written (the uploader takes one skeleton).
std::vector<Vec3> written_locals(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt) {
    std::vector<Vec3> local(dae_index_count(skel));
    for (int j = 0; j < skel.size(); ++j) local[j] = skel[j].pos;
    for (size_t v = 0; v < skel.volumes().size(); ++v) local[dae_volume(skel, int(v))] = skel.volumes()[v].pos;
    std::vector<bool> have(local.size(), false);
    for (const RigPart& part : parts) {
        if (!part.model) continue;
        const Placement p = place(skel, *part.model, opt);
        for (size_t i = 0; i < local.size(); ++i)
            if (!have[i] && p.positioned[i]) local[i] = p.local[i], have[i] = true;
    }
    return local;
}


std::vector<RigJoint> joints_of(const Skeleton& skel, const DaeModel& model, const Placement& p) {
    std::vector<RigJoint> out;
    if (!model.rigged || model.binds.size() != size_t(dae_index_count(skel))) return out;
    const std::vector<bool> w = weighted_nodes(skel, model);
    auto row = [&](int i, const std::string& name) {
        RigJoint r;
        r.node = i;
        r.name = name;
        r.bound = i < int(model.bound.size()) && model.bound[i];
        r.weighted = w[i];
        r.local = p.local[i];
        r.offset = p.local[i] - p.def[i];
        r.offset_mm = r.offset.length() * 1000;
        const bool moved = r.offset.length() > kThresholdM;
        r.listed = r.weighted || moved;
        r.uploads = r.listed && moved;
        r.nudged = p.nudged[i];
        r.noise = moved && r.offset.length() <= kNoiseM && !r.nudged;
        out.push_back(r);
    };
    for (int j = 0; j < skel.joint_count(); ++j) row(j, skel[j].name);
    const auto& vols = skel.volumes();
    for (size_t v = 0; v < vols.size(); ++v) row(dae_volume(skel, int(v)), vols[v].name);
    return out;
}

std::string num(double v, const char* fmt = "%.7g") {
    char buf[64];
    if (!std::isfinite(v)) v = 0;
    if (std::fabs(v) < 1e-12) v = 0;  // no "-0"
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string id_of(std::string s) {
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) c = '_';
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) s = "n" + s;
    return s;
}

std::string mm(double m) { return num(m * 1000, "%.1f") + " mm"; }

std::string join(const std::vector<std::string>& v, size_t max = 6) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > max) s += " and " + std::to_string(v.size() - max) + " more";
    return s;
}

}  // namespace

std::vector<RigJoint> rig_joints(const Skeleton& skel, const DaeModel& model, const RigExportOptions& opt) {
    return joints_of(skel, model, place(skel, model, opt));
}

RigHeight rig_in_world_height(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt) {
    RigHeight h;
    const int count = dae_index_count(skel), skull = skel.find("mSkull");
    std::vector<Placement> places(parts.size());
    std::vector<bool> listed(size_t(count), false);
    bool skull_weighted = false;
    int pelvis_part = -1;  // the first part that bound mPelvis
    for (size_t k = 0; k < parts.size(); ++k) {
        const DaeModel* m = parts[k].model;
        if (!m || !m->rigged || m->binds.size() != size_t(count)) continue;
        places[k] = place(skel, *m, opt);
        for (const RigJoint& j : joints_of(skel, *m, places[k])) listed[size_t(j.node)] = listed[size_t(j.node)] || j.listed;
        skull_weighted |= skull >= 0 && weighted_nodes(skel, *m)[size_t(skull)];
        // The skull's position is written from the first part that positions it (written_locals).
        if (h.skull_part < 0 || (skull >= 0 && places[k].positioned[skull] && !places[h.skull_part].positioned[skull])) h.skull_part = int(k);
        if (pelvis_part < 0 && !m->bound.empty() && m->bound[0]) pelvis_part = int(k);
        h.valid = true;
    }
    if (!h.valid) return h;
    if (skull_weighted || !opt.joint_positions || opt.bind_pose_only || skull < 0) h.skull_part = -1;
    const std::vector<Vec3> local = written_locals(skel, parts, opt);
    // The joint positions the viewer applies (LLVOAvatar::addAttachmentOverridesForObject): in the joint list and over
    // 0.1 mm from default.
    Pose live(skel.size());
    for (int j = 0; j < skel.joint_count(); ++j)
        if (listed[j] && (local[j] - skel[j].pos).length() > kThresholdM) live.offset[j] = local[j] - skel[j].pos;
    const SlBodySize s = sl_body_size(skel), l = sl_body_size(skel, &live);
    h.body = l.height, h.default_body = s.height;
    // The file's ground (z 0 as it was bound: a body stands on it) hangs off the pelvis as it was bound, and the
    // viewer draws the pelvis at the root whatever its written position.
    const double pelvis_bind = pelvis_part >= 0 ? parts[size_t(pelvis_part)].model->binds[0].pos.z : skel[0].pos.z;
    h.sole = skel[0].pos.z - pelvis_bind + 0.5 * (s.height - l.height) + (l.pelvis_to_foot - s.pelvis_to_foot);
    return h;
}

bool set_written_local(const Skeleton& skel, DaeModel& model, int node, const Vec3& local, const RigExportOptions& opt) {
    const int count = dae_index_count(skel), root = dae_root(skel);
    if (!model.rigged || node < 0 || node >= count || node == root || model.binds.size() != size_t(count)) return false;
    model.bound.resize(size_t(count), false);
    // The bind position that writes local: the local from the as-written parent, carried back into the nearest
    // bound ancestor's bind frame (place() reads it there).
    RigExportOptions all = opt;
    all.joint_positions = true, all.bind_pose_only = false, all.pelvis_offset = true;
    const Placement p = place(skel, model, all);
    const bool volume = node > root;
    const int parent = volume ? skel.volumes()[node - root - 1].joint : skel[node].parent;
    const Vec3 at = parent >= 0 ? p.g[parent].apply(local) : local;
    int a = parent;
    while (a >= 0 && !model.bound[a]) a = skel[a].parent;
    model.binds[node].pos = a < 0 ? at : p.bind[a].pos + p.bind[a].rot.rotate(p.g[a].inverse().apply(at));
    model.bound[node] = true;
    return true;
}

bool snap_joint_to_default(const Skeleton& skel, DaeModel& model, int node, const RigExportOptions& opt) {
    const int count = dae_index_count(skel), root = dae_root(skel);
    if (node < 0 || node >= count || node == root || node >= int(model.bound.size()) || !model.bound[node]) return false;
    const bool volume = node > root;
    return set_written_local(skel, model, node, volume ? skel.volumes()[node - root - 1].pos : skel[node].pos, opt);
}

const std::vector<RigRule>& rig_export_rules() {
    static const std::vector<RigRule> rules = {
        {"not_rigged", "Part is not rigged"},
        {"too_many_joints", "Over 110 joints in one mesh"},
        {"unknown_joints", "Weights to unknown joint names"},
        {"zero_weight_vertices", "Vertices with no weights"},
        {"unweighted_moved", "Moved joints with no weights"},
        {"false_offsets", "Joint positions under 1 mm"},
        {"scale_mismatch", "Rig scale differs from the measured one"},
        {"bind_pose", "Bound in another pose than SL's rest"},
        {"rest_pose", "Arms bound off SL's rest pose"},
        {"pelvis", "mPelvis position"},
        {"shared_positions", "Vertices sharing a position, weighted differently"},
        {"shape_proof", "Shape-proof"},
        {"spare_chains", "Parts on spare chains"},
        {"volumes", "Collision volumes"},
        {"faces", "Materials over the uploader's 8"},
        {"face_vertices", "Over 65,534 vertices in one material"},
        {"triangles", "Triangle count and LODs"},
        {"in_world_height", "Floats or sinks in-world"},
    };
    return rules;
}

std::vector<RigFinding> check_rig_export(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt) {
    std::vector<RigFinding> out;
    auto add = [&](const char* rule, RigSeverity sev, int part, std::string msg, std::vector<std::string> joints = {}) {
        RigFinding f;
        f.rule = rule, f.severity = sev, f.part = part, f.message = std::move(msg), f.joints = std::move(joints);
        out.push_back(std::move(f));
        return &out.back();
    };
    const int root = dae_root(skel), count = dae_index_count(skel);
    long long triangles = 0;
    std::vector<std::string> volumes_weighted;
    for (int pi = 0; pi < int(parts.size()); ++pi) {
        const RigPart& part = parts[pi];
        const std::string label = part.name.empty() ? "mesh " + std::to_string(pi + 1) : part.name;
        const DaeModel* m = part.model;
        if (!m || !m->rigged || m->binds.size() != size_t(count)) {
            add("not_rigged", RigSeverity::Error, pi, label + " is not rigged to the SL skeleton: nothing to upload as rigged mesh");
            continue;
        }
        triangles += m->triangle_count();
        const Placement p = place(skel, *m, opt);
        const std::vector<RigJoint> joints = joints_of(skel, *m, p);
        std::vector<std::string> listed, moved_unweighted, noise, unknown = part.unmapped_joints;
        int volumes = 0;
        for (const RigJoint& j : joints) {
            if (j.listed) listed.push_back(j.name);
            if (j.uploads && !j.weighted) moved_unweighted.push_back(j.name);
            if (j.noise && j.listed) noise.push_back(j.name);
            if (j.weighted && j.node > root) ++volumes, volumes_weighted.push_back(j.name);
        }
        if (listed.size() > size_t(kMaxJoints))
            add("too_many_joints", RigSeverity::Error, pi,
                label + " lists " + std::to_string(listed.size()) + " joints; SL allows " + std::to_string(kMaxJoints) +
                    " per mesh, and skins none of it over that" +
                    (moved_unweighted.empty() ? "" : " (" + std::to_string(moved_unweighted.size()) +
                                                         " of them are moved but carry no weights: " + join(moved_unweighted) + ")"),
                listed);
        if (!unknown.empty())
            add("unknown_joints", RigSeverity::Warning, pi,
                label + ": weights to " + std::to_string(unknown.size()) + " joint name" + (unknown.size() == 1 ? "" : "s") +
                    " VATs does not know were dropped when the file was read (" + join(unknown) +
                    "); the vertices keep their other weights. Rename the bones to SL's before exporting from the modelling tool if they were meant",
                unknown);
        // Vertices weighted to nothing: SL divides by their zero total.
        std::vector<int> orphans;
        const size_t nv = m->positions.size() / 3;
        for (size_t v = 0; v < nv && v * 4 + 3 < m->joints.size() && v * 4 + 3 < m->weights.size(); ++v) {
            float total = 0;
            for (int k = 0; k < 4; ++k)
                if (m->joints[v * 4 + k] != root && m->joints[v * 4 + k] >= 0) total += m->weights[v * 4 + k];
            if (!(total > 0)) orphans.push_back(int(v));
        }
        if (!orphans.empty()) {
            RigFinding* f = add("zero_weight_vertices", RigSeverity::Error, pi,
                                label + ": " + std::to_string(orphans.size()) + " vertices carry no weight to any SL joint" +
                                    (unknown.empty() ? "" : " (weighted to unknown names, most likely)") +
                                    "; SL would put them at the origin. Weight them, or let VATs weight each to the nearest bone the mesh rigs to");
            f->fix_label = "Weight to the nearest bone";
            const std::vector<bool> w = weighted_nodes(skel, *m);
            std::vector<int> candidates;  // the bones the file rigs to: bound or weighted
            for (int i = 0; i < count; ++i)
                if (i != root && (w[i] || (i < int(m->bound.size()) && m->bound[i]))) candidates.push_back(i);
            if (!candidates.empty())
                f->fix = [orphans, candidates, root](DaeModel& model) {
                    for (int v : orphans) {
                        if (size_t(v) * 4 + 3 >= model.joints.size() || size_t(v) * 3 + 2 >= model.positions.size()) continue;
                        const Vec3 pos{model.positions[v * 3], model.positions[v * 3 + 1], model.positions[v * 3 + 2]};
                        int best = candidates[0];
                        double lo = 1e300;
                        for (int c : candidates)
                            if (c < int(model.binds.size()))
                                if (double d = (model.binds[c].pos - pos).length(); d < lo) lo = d, best = c;
                        model.joints[v * 4] = best, model.weights[v * 4] = 1;
                        for (int k = 1; k < 4; ++k) model.joints[v * 4 + k] = root, model.weights[v * 4 + k] = 0;
                    }
                };
        }
        if (!moved_unweighted.empty())
            add("unweighted_moved", RigSeverity::Info, pi,
                label + ": " + std::to_string(moved_unweighted.size()) + (moved_unweighted.size() == 1 ? " moved joint carries" : " moved joints carry") +
                    " no weights (" + join(moved_unweighted) + "); they are listed with no weight so their positions upload",
                moved_unweighted);
        if (!noise.empty()) {
            RigFinding* f = add("false_offsets", RigSeverity::Warning, pi,
                                label + ": " + std::to_string(noise.size()) + (noise.size() == 1 ? " joint sits" : " joints sit") +
                                    " between 0.1 and 1 mm from default (" + join(noise) +
                                    "): SL uploads such a position and it overrides the wearer's body there for nothing. Float noise from an export, most likely",
                                noise);
            f->fix_label = "Snap to default";
            std::vector<int> nodes;
            for (const RigJoint& j : joints)
                if (j.noise && j.listed) nodes.push_back(j.node);
            f->fix = [nodes, &skel](DaeModel& model) {
                for (int n : nodes) snap_joint_to_default(skel, model, n);
            };
        }
        if (part.measured_scale > 0 && part.scale > 0 && std::fabs(std::log(part.measured_scale / part.scale)) > 0.02)
            add("scale_mismatch", RigSeverity::Warning, pi,
                label + " was read at scale " + num(part.scale, "%.4g") + " but its bones measure " + num(part.measured_scale, "%.4g") +
                    " against SL's: its proportions are its own (a creature), or the file's unit is off. It uploads at the size shown; check it in the uploader's preview");
        // Bind pose: bind rotations far from rest.
        std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
        int posed = 0;
        double worst = 0;
        for (int j = 0; j < skel.joint_count(); ++j)
            if (j < int(m->bound.size()) && m->bound[j]) {
                const double deg = (rest[j].rot.conj() * m->binds[j].rot).angle() * kRadToDeg;
                if (deg > 5) ++posed, worst = std::max(worst, deg);
            }
        if (posed)
            add("bind_pose", RigSeverity::Info, pi,
                label + " is bound in another pose than SL's rest (" + std::to_string(posed) + " joints turned, up to " + num(worst, "%.0f") +
                    " degrees; an A-pose, say). The inverse binds carry that pose and SL skins the mesh back onto its rest skeleton" +
                    (opt.bind_pose_only ? ". Bind pose only: no joint positions are written"
                                        : "; joint positions keep each bone's length along SL's rest axes"));
        // RG-9: arms bound hanging (an A-pose) on SL's unturned joints: SL animations, made for its T-pose rest, turn
        // them that much too far. Stand it in SL's rest pose writes the T-pose instead.
        if (opt.joint_positions && !opt.bind_pose_only && !opt.rest_pose) {
            const std::vector<bool> w = weighted_nodes(skel, *m);
            double arms = 0;
            for (const char* n : {"mShoulderLeft", "mShoulderRight"}) {
                const int j = skel.find(n);
                const std::vector<int> next = j >= 0 && m->bound[j] ? next_weighted(skel, *m, w, j) : std::vector<int>{};
                // An arm still reaching out sideways (a quadruped's front leg on the arm points down: no hint).
                if (next.size() == 1 && std::fabs((m->binds[next[0]].pos - m->binds[j].pos).normalized().y) > 0.35)
                    arms = std::max(arms, bone_swing(skel, *m, w, rest, j).angle() * kRadToDeg);
            }
            if (arms > 20)
                add("rest_pose", RigSeverity::Info, pi,
                    label + "'s arms are bound " + num(arms, "%.0f") + " degrees off SL's rest pose (arms out level): SL animations are made "
                            "for that rest and would turn them " + num(arms, "%.0f") + " degrees too far. For an upright humanoid, tick "
                            "Stand it in SL's rest pose");
        }
        // The pelvis: its position changes the avatar's height and hover.
        if (m->bound[0]) {
            const double d = (m->binds[0].pos - rest[0].pos).length();
            if (d > kThresholdM && opt.joint_positions && !opt.bind_pose_only) {
                const Vec3 o = m->binds[0].pos - rest[0].pos;
                if (opt.pelvis_offset)
                    add("pelvis", RigSeverity::Warning, pi,
                        label + " writes mPelvis " + mm(d) + " from default (x " + num(o.x * 1000, "%.1f") + ", y " + num(o.y * 1000, "%.1f") +
                            ", z " + num(o.z * 1000, "%.1f") + " mm): the wearer's pelvis-to-foot height and hover change with it, on every animation",
                        {"mPelvis"});
                else
                    add("pelvis", RigSeverity::Info, pi,
                        label + " bound mPelvis " + mm(d) + " from default; it is not written (mPelvis offset is off), so the wearer keeps their hover and the other joints keep their offsets from the pelvis",
                        {"mPelvis"});
            }
        }
        if (opt.shape_proof && opt.joint_positions && !opt.bind_pose_only) {
            int nudged = 0;
            for (const RigJoint& j : joints) nudged += j.nudged;
            add("shape_proof", RigSeverity::Info, pi,
                label + ": shape-proof, " + std::to_string(listed.size()) + " joints listed with their positions" +
                    (nudged ? " (" + std::to_string(nudged) + " of them moved 0.11 mm off SL's default only so they count)" : "") +
                    ". Tick \"Lock scale if joint position defined\" when you upload, or the wearer's shape sliders still scale it");
        }
        // RM-8: parts on spare chains. Their joints sit far from SL's defaults, along the part, and only joint positions
        // put them there; anything else that animates those joints moves the part too.
        for (const SpareSlot& slot : spare_slots()) {
            std::vector<std::string> placed;
            std::string held;
            for (const std::string& name : slot.joints) {
                const auto it = m->labels.find(name);
                if (it == m->labels.end()) continue;
                held = it->second;
                for (const RigJoint& j : joints)
                    if (j.name == name) placed.push_back(name + " " + num(j.offset_mm / 10, "%.1f") + " cm off");
            }
            if (held.empty()) continue;
            const bool positions = opt.joint_positions && !opt.bind_pose_only;
            add("spare_chains", positions ? RigSeverity::Info : RigSeverity::Warning, pi,
                label + ": the " + held + " rides SL's " + slot.name + " (" + join(placed) + ")" +
                    (positions ? ". Its joint positions are written: upload with \"Include joint positions\"" +
                                     std::string(opt.shape_proof ? ", and tick \"Lock scale if joint position defined\""
                                                                 : "; tick Shape-proof and \"Lock scale if joint position defined\" "
                                                                   "so the wearer's shape sliders do not stretch it")
                               : ". No joint positions are written, so the " + held + " would be wrapped round SL's own " +
                                     slot.name + ": write joint positions") +
                    ". Whatever animates these joints at a higher priority wins: key them at a priority above your AO's" +
                    (slot.worn.empty() ? "" : "; " + slot.worn + " worn with it fight it"),
                slot.joints);
        }
        Vec3 size;
        if (const std::vector<int> clash = weight_clashes(*m, root, size); !clash.empty()) {
            const long long n = std::count_if(clash.begin(), clash.end(), [](int r) { return r != 0; });
            const double nudge_mm = 2 * kUploaderEps * std::max({size.x, size.y, size.z}) * 1000;
            RigFinding* f = add("shared_positions", RigSeverity::Warning, pi,
                                label + ": " + std::to_string(n) + " vertices share a position with differently weighted ones. The uploader keeps "
                                        "one set of weights per position, so it gives them each other's: touching plates or lips on "
                                        "different bones tear apart or stick together in-world. Nudge them apart, up to " +
                                        num(nudge_mm * std::max(1, *std::max_element(clash.begin(), clash.end())), "%.2f") + " mm");
            f->fix_label = "Nudge apart";
            f->fix = [root](DaeModel& model) {
                Vec3 sz;
                const std::vector<int> r = weight_clashes(model, root, sz);
                for (size_t v = 0; v < r.size(); ++v) {
                    if (r[v] <= 0) continue;
                    Vec3 nrm = v * 3 + 2 < model.normals.size() ? Vec3{model.normals[v * 3], model.normals[v * 3 + 1], model.normals[v * 3 + 2]}.normalized() : Vec3{};
                    if (nrm.length() < 0.5) nrm = {0, 0, 1};
                    // Twice the uploader's distance per rank, measured as it measures (scaled to the unit cube).
                    const double d = 2 * kUploaderEps * r[v] / Vec3{nrm.x / sz.x, nrm.y / sz.y, nrm.z / sz.z}.length();
                    for (int k = 0; k < 3; ++k) model.positions[v * 3 + k] += float(nrm[k] * d);
                }
            };
        }
        if (volumes)
            add("volumes", RigSeverity::Info, pi,
                label + " is weighted to " + std::to_string(volumes) + " collision volume" + (volumes == 1 ? "" : "s") +
                    " (fitted mesh); their inverse binds carry SL's volume rotation and scale, as the wearer's volumes have them",
                volumes_weighted);
        // The writer puts every part's triangles of one material in one <triangles>: one uploader face per material.
        const std::vector<int> mats = materials_used(*m);
        if (mats.size() > size_t(kMaxFaces))
            add("faces", RigSeverity::Warning, pi,
                label + " has " + std::to_string(mats.size()) + " materials; the uploader takes 8 per mesh and splits the rest into more meshes (a linkset)");
        for (int material : mats) {
            std::uint32_t face_verts = 0;
            for (const DaeGroup& g : m->groups) face_verts += g.material == material ? g.vertex_count : 0;
            if (face_verts <= unsigned(kMaxFaceVertices)) continue;
            // The uploader shares corners with the same position, normal and UV, so count those, not our vertices.
            std::set<std::array<float, 8>> unique;
            for (const DaeGroup& g : m->groups)
                for (std::uint32_t v = g.first_vertex; g.material == material && v < g.first_vertex + g.vertex_count && v * 3 + 2 < m->positions.size() &&
                                                       unique.size() <= size_t(kMaxFaceVertices); ++v) {
                    std::array<float, 8> key{m->positions[v * 3], m->positions[v * 3 + 1], m->positions[v * 3 + 2], 0, 0, 0, 0, 0};
                    if (v * 3 + 2 < m->normals.size()) key[3] = m->normals[v * 3], key[4] = m->normals[v * 3 + 1], key[5] = m->normals[v * 3 + 2];
                    if (v * 2 + 1 < m->uvs.size()) key[6] = m->uvs[v * 2], key[7] = m->uvs[v * 2 + 1];
                    unique.insert(key);
                }
            const size_t n = m->positions.empty() ? count : unique.size();
            if (n <= size_t(kMaxFaceVertices)) continue;
            const std::string mat = material >= 0 && material < int(m->materials.size()) && !m->materials[material].name.empty()
                                        ? m->materials[material].name : "material " + std::to_string(material + 1);
            add("face_vertices", RigSeverity::Error, pi,
                label + ": " + mat + " has over 65,534 vertices (corners with their own position, normal and UV; " + std::to_string(count) +
                    " here); the uploader's faces hold 65,534 (16-bit indices) and it refuses the mesh. Split the material or the mesh");
        }
    }
    // Spec 08 RG-8: where SL will stand it.
    if (const RigHeight h = rig_in_world_height(skel, parts, opt); h.valid && std::fabs(h.sole) > kSoleTolM) {
        const bool floats = h.sole > 0;
        const double z_offset = std::clamp(-h.sole, -3.0, 3.0);  // the uploader's spinner takes -3..3 m
        std::string msg = std::string(floats ? "Floats " : "Sinks ") + num(std::fabs(h.sole) * 100, "%.1f") + " cm " +
                          (floats ? "above the ground" : "into the ground") +
                          " in Second Life (the default shape). SL takes the wearer's height from the left leg, torso, chest, neck, "
                          "head and skull joints: with these joint positions it comes to " + num(h.body * 100, "%.0f") + " cm against the default's " +
                          num(h.default_body * 100, "%.0f") + " cm, and the viewer stands the mesh by that. ";
        if (h.skull_part >= 0) msg += "Fix: an unweighted mSkull position that evens the height out (nothing shows). Or ";
        else msg += std::string(opt.joint_positions && !opt.bind_pose_only ? "(mSkull carries weights here, so it cannot even it out.) " : "") + "Fix: ";
        msg += "on the uploader's Upload options tab, set \"Z offset (raise or lower avatar)\" to " + num(z_offset, "%.3f") +
               " (it applies only with Include joint positions ticked)";
        RigFinding* f = add("in_world_height", RigSeverity::Warning, h.skull_part, msg, {"mSkull"});
        if (h.skull_part >= 0) {
            // computeBodySize counts mSkull's Z times sqrt(2) times the head's scale (1 for the default shape), and the
            // mesh moves half of what the height changes: sqrt(2) * sole of skull Z puts the soles on the ground.
            const int skull = skel.find("mSkull");
            const Vec3 local = written_locals(skel, parts, opt)[skull] + Vec3{0, 0, std::sqrt(2.0) * h.sole};
            f->fix_label = "Even out with mSkull";
            f->fix = [&skel, skull, local, opt](DaeModel& model) { set_written_local(skel, model, skull, local, opt); };
        }
    }
    if (triangles)
        add("triangles", RigSeverity::Info, -1,
            std::to_string(triangles) + " triangles at the highest LOD. The uploader makes the lower LODs itself (or takes your own files); a rigged mesh's land impact comes from its LODs, and a body over 50,000 triangles will need its own reduced LODs to stay reasonable");
    return out;
}

bool write_rig_dae(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt, std::string& out,
                   std::string& err) {
    out.clear();
    const std::vector<RigFinding> findings = check_rig_export(skel, parts, opt);
    for (const RigFinding& f : findings)
        if (f.severity == RigSeverity::Error) return err = f.message, false;
    if (parts.empty()) return err = "nothing to export", false;
    const int root = dae_root(skel);

    std::string images, effects, materials, geometries, controllers, mesh_nodes;
    int image_n = 0;
    for (int pi = 0; pi < int(parts.size()); ++pi) {
        const RigPart& part = parts[pi];
        const DaeModel& m = *part.model;
        const std::string id = "mesh" + std::to_string(pi), name = xml_escape(part.name.empty() ? id : part.name);
        const Placement p = place(skel, m, opt);
        const std::vector<RigJoint> joints = joints_of(skel, m, p);

        // Materials.
        std::string bind_material;
        std::map<int, std::string> symbol;
        for (const DaeGroup& g : m.groups) {
            if (symbol.count(g.material)) continue;
            const DaeMaterial* dm = g.material >= 0 && g.material < int(m.materials.size()) ? &m.materials[g.material] : nullptr;
            const std::string mid = id + "-mat" + std::to_string(g.material), sym = mid + "-sym";
            const std::string mname = xml_escape(dm && !dm->name.empty() ? dm->name : "Material" + std::to_string(g.material + 1));
            symbol[g.material] = sym;
            std::string diffuse, params;
            if (dm && !dm->texture.empty()) {
                const std::string img = "image" + std::to_string(image_n++);
                const std::string file = dm->texture.substr(dm->texture.find_last_of("/\\") + 1);
                images += "    <image id=\"" + img + "\" name=\"" + img + "\"><init_from>" + xml_escape(file) + "</init_from></image>\n";
                params = "        <newparam sid=\"" + img + "-surface\"><surface type=\"2D\"><init_from>" + img + "</init_from></surface></newparam>\n"
                         "        <newparam sid=\"" + img + "-sampler\"><sampler2D><source>" + img + "-surface</source></sampler2D></newparam>\n";
                diffuse = "<texture texture=\"" + img + "-sampler\" texcoord=\"UVMap\"/>";
            } else {
                const auto c = dm ? dm->rgba : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1.f};
                diffuse = "<color sid=\"diffuse\">" + num(c[0], "%.4g") + " " + num(c[1], "%.4g") + " " + num(c[2], "%.4g") + " " + num(c[3], "%.4g") + "</color>";
            }
            effects += "    <effect id=\"" + mid + "-fx\">\n      <profile_COMMON>\n" + params +
                       "        <technique sid=\"common\"><phong><diffuse>" + diffuse + "</diffuse><specular><color>0 0 0 1</color></specular>"
                       "<shininess><float>10</float></shininess></phong></technique>\n      </profile_COMMON>\n    </effect>\n";
            materials += "    <material id=\"" + mid + "\" name=\"" + mname + "\"><instance_effect url=\"#" + mid + "-fx\"/></material>\n";
            bind_material += "            <instance_material symbol=\"" + sym + "\" target=\"#" + mid + "\"><bind_vertex_input semantic=\"UVMap\" input_semantic=\"TEXCOORD\" input_set=\"0\"/></instance_material>\n";
        }

        // Geometry: one source each for positions, normals and UVs, one <triangles> per material.
        const size_t nv = m.positions.size() / 3;
        std::string pos, nrm, uv;
        pos.reserve(nv * 30), nrm.reserve(nv * 24), uv.reserve(nv * 20);
        for (size_t v = 0; v < nv; ++v) {
            for (int k = 0; k < 3; ++k) pos += num(m.positions[v * 3 + k]) + " ";
            for (int k = 0; k < 3; ++k) nrm += num(v * 3 + k < m.normals.size() ? m.normals[v * 3 + k] : 0, "%.5g") + " ";
            uv += num(v * 2 < m.uvs.size() ? m.uvs[v * 2] : 0, "%.6g") + " " + num(v * 2 + 1 < m.uvs.size() ? 1 - m.uvs[v * 2 + 1] : 0, "%.6g") + " ";
        }
        auto source = [&](const std::string& sid, const std::string& data, size_t n, int stride, const char* params) {
            return "        <source id=\"" + sid + "\"><float_array id=\"" + sid + "-array\" count=\"" + std::to_string(n * stride) + "\">" + data +
                   "</float_array><technique_common><accessor source=\"#" + sid + "-array\" count=\"" + std::to_string(n) + "\" stride=\"" +
                   std::to_string(stride) + "\">" + params + "</accessor></technique_common></source>\n";
        };
        const char* xyz = "<param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/>";
        geometries += "    <geometry id=\"" + id + "\" name=\"" + name + "\">\n      <mesh>\n" +
                      source(id + "-positions", pos, nv, 3, xyz) + source(id + "-normals", nrm, nv, 3, xyz) +
                      source(id + "-uvs", uv, nv, 2, "<param name=\"S\" type=\"float\"/><param name=\"T\" type=\"float\"/>") +
                      "        <vertices id=\"" + id + "-vertices\"><input semantic=\"POSITION\" source=\"#" + id + "-positions\"/></vertices>\n";
        for (const int material : materials_used(m)) {  // one <triangles> per material, over every part
            std::string idx;
            std::uint32_t count = 0;
            for (const DaeGroup& g : m.groups) {
                if (g.material != material) continue;
                count += g.index_count;
                for (std::uint32_t k = g.first_index; k < g.first_index + g.index_count && k < m.indices.size(); ++k) {
                    const std::string i = std::to_string(m.indices[k]);
                    idx += i + " " + i + " " + i + " ";
                }
            }
            geometries += "        <triangles material=\"" + symbol[material] + "\" count=\"" + std::to_string(count / 3) + "\">"
                          "<input semantic=\"VERTEX\" source=\"#" + id + "-vertices\" offset=\"0\"/><input semantic=\"NORMAL\" source=\"#" + id +
                          "-normals\" offset=\"1\"/><input semantic=\"TEXCOORD\" source=\"#" + id + "-uvs\" offset=\"2\" set=\"0\"/><p>" + idx +
                          "</p></triangles>\n";
        }
        geometries += "      </mesh>\n    </geometry>\n";

        // Skin: the listed joints, their inverse binds, and 4 weights at most per vertex.
        std::vector<int> slot(dae_index_count(skel), -1);
        std::string names, ibms;
        int nj = 0;
        for (const RigJoint& j : joints) {
            if (!j.listed) continue;
            slot[j.node] = nj++;
            names += j.name + " ";
            Affine b = Affine::from_xform(p.bind[j.node]);
            if (j.node > root) b = b * Affine::scaling(skel.volumes()[j.node - root - 1].scale);
            const Affine inv = b.inverse();
            for (auto& row : inv.m)
                for (double e : row) ibms += num(e) + " ";
            ibms += "0 0 0 1 ";
        }
        std::string weights, vcount, v;
        weights.reserve(nv * 40), vcount.reserve(nv * 2), v.reserve(nv * 32);
        int wn = 0;
        for (size_t vi = 0; vi < nv; ++vi) {
            int n = 0;
            float total = 0;
            for (int k = 0; k < 4 && vi * 4 + k < m.joints.size(); ++k)
                if (m.joints[vi * 4 + k] >= 0 && m.joints[vi * 4 + k] < int(slot.size()) && slot[m.joints[vi * 4 + k]] >= 0 && m.weights[vi * 4 + k] > 0)
                    total += m.weights[vi * 4 + k];
            for (int k = 0; k < 4 && vi * 4 + k < m.joints.size(); ++k) {
                const int j = m.joints[vi * 4 + k];
                const float w = m.weights[vi * 4 + k];
                if (j < 0 || j >= int(slot.size()) || slot[j] < 0 || !(w > 0) || !(total > 0)) continue;
                weights += num(w / total, "%.6g") + " ";
                v += std::to_string(slot[j]) + " " + std::to_string(wn++) + " ";
                ++n;
            }
            vcount += std::to_string(n) + " ";
        }
        controllers += "    <controller id=\"" + id + "-skin\" name=\"" + name + "-skin\">\n      <skin source=\"#" + id + "\">\n"
                       "        <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>\n"
                       "        <source id=\"" + id + "-joints\"><Name_array id=\"" + id + "-joints-array\" count=\"" + std::to_string(nj) + "\">" + names +
                       "</Name_array><technique_common><accessor source=\"#" + id + "-joints-array\" count=\"" + std::to_string(nj) +
                       "\" stride=\"1\"><param name=\"JOINT\" type=\"name\"/></accessor></technique_common></source>\n"
                       "        <source id=\"" + id + "-bind_poses\"><float_array id=\"" + id + "-bind_poses-array\" count=\"" + std::to_string(nj * 16) + "\">" + ibms +
                       "</float_array><technique_common><accessor source=\"#" + id + "-bind_poses-array\" count=\"" + std::to_string(nj) +
                       "\" stride=\"16\"><param name=\"TRANSFORM\" type=\"float4x4\"/></accessor></technique_common></source>\n" +
                       source(id + "-weights", weights, size_t(wn), 1, "<param name=\"WEIGHT\" type=\"float\"/>") +
                       "        <joints><input semantic=\"JOINT\" source=\"#" + id + "-joints\"/><input semantic=\"INV_BIND_MATRIX\" source=\"#" + id + "-bind_poses\"/></joints>\n"
                       "        <vertex_weights count=\"" + std::to_string(nv) + "\"><input semantic=\"JOINT\" source=\"#" + id + "-joints\" offset=\"0\"/>"
                       "<input semantic=\"WEIGHT\" source=\"#" + id + "-weights\" offset=\"1\"/><vcount>" + vcount + "</vcount><v>" + v + "</v></vertex_weights>\n"
                       "      </skin>\n    </controller>\n";
        mesh_nodes += "      <node id=\"" + id + "-node\" name=\"" + name + "\" type=\"NODE\">\n        <instance_controller url=\"#" + id + "-skin\">\n"
                      "          <skeleton>#" + id_of(skel[0].name) + "</skeleton>\n          <bind_material><technique_common>\n" + bind_material +
                      "          </technique_common></bind_material>\n        </instance_controller>\n      </node>\n";
    }

    // The skeleton: every SL joint as a pure translation, collision volumes under their joints. When the parts disagree
    // on a joint (each bound it elsewhere), the first part's position is written (the uploader takes one skeleton).
    const std::vector<Vec3> local = written_locals(skel, parts, opt);
    std::vector<std::vector<int>> volumes_of(skel.joint_count());
    for (size_t v = 0; v < skel.volumes().size(); ++v) volumes_of[skel.volumes()[v].joint].push_back(int(v));
    std::string skeleton;
    auto translate = [&](const Vec3& t) { return "<translate sid=\"translate\">" + num(t.x) + " " + num(t.y) + " " + num(t.z) + "</translate>"; };
    std::function<void(int, int)> write_joint = [&](int j, int depth) {
        const std::string pad(size_t(6 + depth * 2), ' '), jid = id_of(skel[j].name);
        skeleton += pad + "<node id=\"" + jid + "\" name=\"" + xml_escape(skel[j].name) + "\" sid=\"" + jid + "\" type=\"JOINT\">" + translate(local[j]) + "\n";
        for (int v : volumes_of[j]) {
            const std::string vid = id_of(skel.volumes()[v].name);
            skeleton += pad + "  <node id=\"" + vid + "\" name=\"" + xml_escape(skel.volumes()[v].name) + "\" sid=\"" + vid + "\" type=\"JOINT\">" +
                        translate(local[dae_volume(skel, v)]) + "</node>\n";
        }
        for (int c : skel[j].children)
            if (c < skel.joint_count()) write_joint(c, depth + 1);
        skeleton += pad + "</node>\n";
    };
    for (int j = 0; j < skel.joint_count(); ++j)
        if (skel[j].parent < 0) write_joint(j, 0);

    char date[32] = "2026-01-01T00:00:00";
    std::time_t now = std::time(nullptr);
    if (std::tm* t = std::gmtime(&now)) std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", t);
    out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          "<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">\n"
          "  <asset>\n    <contributor><authoring_tool>Viewport Avatar Toolset</authoring_tool></contributor>\n    <created>" + std::string(date) +
          "</created>\n    <modified>" + date + "</modified>\n    <unit name=\"meter\" meter=\"1\"/>\n    <up_axis>Z_UP</up_axis>\n  </asset>\n"
          "  <library_images>\n" + images + "  </library_images>\n  <library_effects>\n" + effects + "  </library_effects>\n"
          "  <library_materials>\n" + materials + "  </library_materials>\n  <library_geometries>\n" + geometries + "  </library_geometries>\n"
          "  <library_controllers>\n" + controllers + "  </library_controllers>\n  <library_visual_scenes>\n    <visual_scene id=\"Scene\" name=\"Scene\">\n" +
          skeleton + mesh_nodes + "    </visual_scene>\n  </library_visual_scenes>\n  <scene><instance_visual_scene url=\"#Scene\"/></scene>\n</COLLADA>\n";
    return true;
}

std::vector<std::pair<std::string, Vec3>> uploader_joint_translations(std::string_view dae_text) {
    std::vector<std::pair<std::string, Vec3>> out;
    XmlNode root;
    std::string err;
    if (!parse_xml(dae_text, root, err)) return out;
    auto floats = [](std::string_view s, std::vector<double>& f) {
        f.clear();
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            size_t b = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i > b) f.push_back(std::atof(std::string(s.substr(b, i - b)).c_str()));
        }
    };
    std::function<void(const XmlNode&)> walk = [&](const XmlNode& n) {
        if (n.name == "node" && n.attr_or("type") == "JOINT" && n.attr("name")) {
            // lldaeloader: <translate> by sid "translate", then "location", then the first <translate>, then <matrix>.
            const XmlNode* t = nullptr;
            for (const char* sid : {"translate", "location"})
                for (auto& c : n.children)
                    if (!t && c.name == "translate" && c.attr_or("sid") == sid) t = &c;
            for (auto& c : n.children)
                if (!t && c.name == "translate") t = &c;
            std::vector<double> f;
            Vec3 pos;
            if (t) {
                floats(t->text, f);
                if (f.size() >= 3) pos = {f[0], f[1], f[2]};
            } else {
                for (auto& c : n.children)
                    if (c.name == "matrix") {
                        floats(c.text, f);
                        if (f.size() >= 16) pos = {f[3], f[7], f[11]};
                        break;
                    }
            }
            out.emplace_back(n.attr_or("name"), pos);
        }
        for (auto& c : n.children) walk(c);
    };
    walk(root);
    return out;
}

}  // namespace vats
