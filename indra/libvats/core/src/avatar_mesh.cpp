// Viewport Avatar Toolset - the SL system avatar body mesh (.llm), morphed and skinned on the CPU.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/avatar_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace vats {
namespace {

constexpr char kLlmHeader[] = "Linden Binary Mesh 1.0";

class Reader {
public:
    const std::vector<std::uint8_t>& in;
    size_t at = 0;
    bool ok = true;

    size_t left() const { return in.size() - at; }
    bool need(size_t n) {
        if (ok && n <= left()) return true;
        ok = false;
        at = in.size();
        return false;
    }
    template <class T>
    T get() {
        T v{};
        if (!need(sizeof(T))) return v;
        std::memcpy(&v, in.data() + at, sizeof(T));  // little-endian hosts only (x86, ARM)
        at += sizeof(T);
        return v;
    }
    void floats(size_t n, std::vector<float>& out) {
        if (!need(n * sizeof(float))) return;
        out.resize(n);
        std::memcpy(out.data(), in.data() + at, n * sizeof(float));
        at += n * sizeof(float);
    }
    void skip(size_t n) {
        if (need(n)) at += n;
    }
    std::string name64() {  // char[64], NUL-terminated
        if (!need(64)) return {};
        const char* p = reinterpret_cast<const char*>(in.data() + at);
        at += 64;
        const void* nul = std::memchr(p, 0, 64);
        return std::string(p, nul ? static_cast<const char*>(nul) - p : 64);
    }
};

bool read_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// SK-35: w = palette index + blend towards the next entry.
Influence decode_weight(float w, const std::vector<int>& palette) {
    if (palette.empty()) return {};
    int last = static_cast<int>(palette.size()) - 1;
    int j = std::clamp(static_cast<int>(std::floor(w)), 0, last);
    return {palette[j], palette[std::min(j + 1, last)], std::clamp(w - static_cast<float>(j), 0.0f, 1.0f)};
}

// SK-37: re-weight vertices bound fully to a wrist between the nearest finger joint and its parent.
void reweight_fingers(const Skeleton& skel, const std::vector<Xform>& rest, const std::vector<float>& coords,
                      std::vector<Influence>& inf, size_t count) {
    for (const char* wrist_name : {"mWristLeft", "mWristRight"}) {
        int wrist = skel.find(wrist_name);
        if (wrist < 0) continue;
        struct Segment {
            int node;
            Vec3 head, dir;
            double len2;
        };
        std::vector<Segment> segs;
        for (int i = wrist; i < skel.size(); ++i) {
            int p = i;
            while (p > wrist) p = skel[p].parent;
            if (p != wrist || skel[i].attachment) continue;
            Vec3 tail = rest[i].apply(skel[i].end);
            segs.push_back({i, rest[i].pos, tail - rest[i].pos, (tail - rest[i].pos).dot(tail - rest[i].pos)});
        }
        for (size_t v = 0; v < count; ++v) {
            Influence& f = inf[v];
            int dominant = f.blend <= 0.001f ? f.a : f.blend >= 0.999f ? f.b : -1;
            if (dominant != wrist) continue;
            Vec3 p{coords[v * 3], coords[v * 3 + 1], coords[v * 3 + 2]};
            const Segment* best = nullptr;
            double best_d = 0, best_t = 0;
            for (const Segment& s : segs) {
                double t = s.len2 > 1e-12 ? (p - s.head).dot(s.dir) / s.len2 : 0.0;
                double d = (p - (s.head + s.dir * std::clamp(t, 0.0, 1.0))).length();
                if (!best || d < best_d) best = &s, best_d = d, best_t = t;
            }
            if (!best || best->node == wrist) continue;
            double w;
            if (best_t >= 0) {
                double s = std::min(best_t / 0.3, 1.0);
                w = 0.5 + 0.5 * s * s * (3 - 2 * s);
            } else {
                w = 0.5 * std::max(0.0, 1.0 + best_t * std::sqrt(best->len2) / 0.015);
            }
            f = {skel[best->node].parent, best->node, static_cast<float>(w)};
        }
    }
}

}  // namespace

const LlmMorph* LlmMesh::find_morph(std::string_view name) const {
    for (auto& m : morphs)
        if (m.name == name) return &m;
    return nullptr;
}

bool parse_llm(const std::vector<std::uint8_t>& bytes, LlmMesh& out, std::string& err) {
    out = LlmMesh();
    if (bytes.size() < 64 || std::memcmp(bytes.data(), kLlmHeader, sizeof(kLlmHeader) - 1) != 0) {
        err = "not a Linden binary mesh";
        return false;
    }
    Reader r{bytes, 24};
    bool has_weights = r.get<std::uint8_t>() != 0;
    bool has_detail = r.get<std::uint8_t>() != 0;
    r.skip(37);  // position, rotation, rotation order, scale: all ignored
    size_t nv = r.get<std::uint16_t>();
    r.floats(nv * 3, out.coords);
    r.floats(nv * 3, out.normals);
    r.skip(nv * 12 + nv * 8 + (has_detail ? nv * 8 : 0));  // binormals, UVs, detail UVs
    if (has_weights) r.floats(nv, out.weights);
    size_t nf = r.get<std::uint16_t>();
    if (r.need(nf * 6)) {
        out.faces.resize(nf * 3);
        std::memcpy(out.faces.data(), bytes.data() + r.at, nf * 6);
        r.at += nf * 6;
    }
    for (std::uint16_t i : out.faces)
        if (i >= nv) {
            err = "face index out of range";
            return false;
        }
    if (has_weights) {
        size_t nj = r.get<std::uint16_t>();
        for (size_t i = 0; i < nj && r.ok; ++i) out.skin_joints.push_back(r.name64());
    }
    if (!r.ok) {
        err = "truncated mesh";
        return false;
    }

    while (r.left() > 0) {
        std::string name = r.name64();
        if (!r.ok || name == "End Morphs") break;
        std::int32_t n = r.get<std::int32_t>();
        if (!r.ok || n < 0 || static_cast<size_t>(n) > r.left() / 48) {
            r.ok = false;
            break;
        }
        LlmMorph m;
        m.name = std::move(name);
        for (std::int32_t k = 0; k < n; ++k) {
            std::uint32_t v = r.get<std::uint32_t>();
            float d[6];
            for (float& x : d) x = r.get<float>();
            r.skip(20);  // binormal and UV deltas
            if (v >= nv) continue;  // bad index: skip the entry, keep the morph
            m.index.push_back(v);
            m.coord.insert(m.coord.end(), d, d + 3);
            m.normal.insert(m.normal.end(), d + 3, d + 6);
        }
        out.morphs.push_back(std::move(m));
    }
    if (r.ok && r.left() > 0) {  // the remap count may be absent
        std::int32_t n = r.get<std::int32_t>();
        if (!r.ok || n < 0 || static_cast<size_t>(n) > r.left() / 8) {
            r.ok = false;
        } else {
            for (std::int32_t k = 0; k < n; ++k) {
                std::int32_t src = r.get<std::int32_t>(), dst = r.get<std::int32_t>();
                if (src >= 0 && dst >= 0 && static_cast<size_t>(src) < nv && static_cast<size_t>(dst) < nv)
                    out.remaps.push_back({src, dst});
            }
        }
    }
    if (!r.ok) {
        err = "truncated mesh";
        return false;
    }
    return true;
}

void morph_mesh(const LlmMesh& mesh, const std::vector<std::pair<std::string, float>>& morphs,
                std::vector<float>& coords, std::vector<float>& normals) {
    coords = mesh.coords;
    normals = mesh.normals;
    for (auto& [name, weight] : morphs) {
        const LlmMorph* m = mesh.find_morph(name);
        if (!m) continue;
        for (size_t k = 0; k < m->index.size(); ++k)
            for (size_t c = 0; c < 3; ++c) {
                coords[m->index[k] * 3 + c] += weight * m->coord[k * 3 + c];
                normals[m->index[k] * 3 + c] += weight * m->normal[k * 3 + c];
            }
    }
    for (auto [src, dst] : mesh.remaps)
        for (int c = 0; c < 3; ++c) {
            coords[src * 3 + c] = coords[dst * 3 + c];
            normals[src * 3 + c] = normals[dst * 3 + c];
        }
    for (size_t v = 0; v + 2 < normals.size(); v += 3) {
        float* n = &normals[v];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0)
            for (int c = 0; c < 3; ++c) n[c] /= l;
    }
}

bool AvatarMesh::load(const Skeleton& skel, const std::string& dir, std::string& err) {
    static const char* names[FileCount] = {"avatar_head.llm", "avatar_upper_body.llm", "avatar_lower_body.llm",
                                           "avatar_eyelashes.llm", "avatar_eye.llm"};
    *this = AvatarMesh();
    skel_ = &skel;
    for (int f = 0; f < FileCount; ++f) {
        std::vector<std::uint8_t> bytes;
        std::string path = dir + "/" + names[f];
        if (!read_bytes(path, bytes)) {
            err = "cannot read " + path;
            return false;
        }
        if (!parse_llm(bytes, files_[f], err)) {
            err = std::string(names[f]) + ": " + err;
            return false;
        }
        // SK-34: joints in skeleton order, each preceded by its base ancestor unless that came last.
        const auto& joints = files_[f].skin_joints;
        for (int i = 0; i < skel.joint_count(); ++i) {
            if (std::find(joints.begin(), joints.end(), skel[i].name) == joints.end()) continue;
            int anc = skel[i].parent;
            while (anc >= 0 && !skel[anc].base) anc = skel[anc].parent;
            if (palettes_[f].empty() || palettes_[f].back() != anc) palettes_[f].push_back(anc);
            palettes_[f].push_back(i);
        }
    }
    std::vector<std::uint8_t> lad;
    if (!read_bytes(dir + "/avatar_lad.xml", lad)) {
        err = "cannot read " + dir + "/avatar_lad.xml";
        return false;
    }
    if (!parse_avatar_params(std::string_view(reinterpret_cast<const char*>(lad.data()), lad.size()), params_, err))
        return false;
    for (int male = 0; male < 2; ++male) sl_default_[male] = sl_default_shape(skel, params_, male);
    return true;
}

const Shape* AvatarMesh::shape(Body body) const {
    if (body == Body::SLDefault || body == Body::SLDefaultMale) return &sl_default_[body == Body::SLDefaultMale].shape;
    return skel_ ? body_shape(*skel_, body) : nullptr;
}

void AvatarMesh::build(Body body, bool finger_weights) {
    if (body == Body::SLDefault || body == Body::SLDefaultMale)
        return build(sl_default_[body == Body::SLDefaultMale], finger_weights);
    BodyShape morphs;  // Female: base meshes
    if (body == Body::Male) {  // SK-28
        morphs.morphs[Head] = {{"Male_Head", 1.0f}};
        morphs.morphs[UpperBody] = {{"Male_Torso", 1.0f}};
        morphs.morphs[LowerBody] = {{"Male_Legs", 1.0f}};
    }
    build(body == Body::SkeletonOnly ? nullptr : &morphs, shape(body), finger_weights);
}

void AvatarMesh::build(const BodyShape& body, bool finger_weights) {
    build(&body, body.shape.offset.empty() ? nullptr : &body.shape, finger_weights);
}

void AvatarMesh::build(const BodyShape* shape, const Shape* own, bool finger_weights) {
    parts_.clear();
    indices_.clear();
    influences_.clear();
    coords_.clear();
    normals_.clear();
    slots_.clear();
    vslot_.clear();
    vblend_.clear();
    if (!skel_ || !shape) return;
    const BodyShape& body = *shape;
    const Skeleton& skel = *skel_;
    std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));

    struct PartDef {
        const char* name;
        File file;
        const char* eye;  // rigid eye joint, or null for a skinned part
    };
    static const PartDef defs[] = {{"head", Head, nullptr},           {"upper_body", UpperBody, nullptr},
                                   {"lower_body", LowerBody, nullptr}, {"eyelashes", Eyelashes, nullptr},
                                   {"eye_left", Eye, "mEyeLeft"},      {"eye_right", Eye, "mEyeRight"}};
    static_assert(int(ShapeMeshCount) == int(Eye), "BodyShape::morphs follows File");
    const std::vector<std::pair<std::string, float>> none;
    std::vector<float> c, n;
    for (const PartDef& d : defs) {
        const LlmMesh& m = files_[d.file];
        morph_mesh(m, d.file < Eye ? body.morphs[d.file] : none, c, n);
        MeshPart part{d.name, d.eye ? Material::Eye : Material::Skin, static_cast<std::uint32_t>(vertex_count()),
                      static_cast<std::uint32_t>(m.vertex_count()), static_cast<std::uint32_t>(indices_.size()),
                      static_cast<std::uint32_t>(m.faces.size())};
        coords_.insert(coords_.end(), c.begin(), c.end());
        normals_.insert(normals_.end(), n.begin(), n.end());
        for (std::uint16_t i : m.faces) indices_.push_back(part.first_vertex + i);
        int eye = d.eye ? skel.find(d.eye) : -1;
        for (int v = 0; v < m.vertex_count(); ++v)
            influences_.push_back(d.eye ? Influence{eye, eye, 0}
                                  : m.weights.empty() ? Influence{} : decode_weight(m.weights[v], palettes_[d.file]));
        parts_.push_back(part);
    }
    if (finger_weights) reweight_fingers(skel, rest, coords_, influences_, influences_.size());

    // Precompute each vertex's two transform slots, so skin() only blends matrices.
    auto slot_for = [&](int node, bool rigid) {
        for (size_t s = 0; s < slots_.size(); ++s)
            if (slots_[s].node == node && slots_[s].shaped != rigid) return static_cast<std::uint16_t>(s);
        Slot sl{node, node >= 0 && !rigid ? rest[node].pos : Vec3{}, !rigid, -1, {}};
        if (rigid && node >= 0) {  // an eye rides its parent (the head) at this body's own eye position
            sl.parent = skel[node].parent;
            sl.local = skel[node].pos + (own && size_t(node) < own->offset.size() ? own->offset[node] : Vec3{});
        }
        slots_.push_back(sl);
        return static_cast<std::uint16_t>(slots_.size() - 1);
    };
    for (const MeshPart& p : parts_)
        for (std::uint32_t v = p.first_vertex; v < p.first_vertex + p.vertex_count; ++v) {
            bool rigid = p.material == Material::Eye;
            vslot_.push_back(slot_for(influences_[v].a, rigid));
            vslot_.push_back(slot_for(influences_[v].b, rigid));
            vblend_.push_back(influences_[v].blend);
        }
}

void AvatarMesh::skin(const std::vector<Xform>& globals, const Shape* shape, std::vector<float>& positions,
                      std::vector<float>& normals) const {
    // SK-36: current global x own shape scale (local axes) x inverse bind, as a 3x4 row-major matrix per slot.
    mats_.resize(slots_.size() * 12);
    for (size_t s = 0; s < slots_.size(); ++s) {
        float* m = &mats_[s * 12];
        const Slot& sl = slots_[s];
        if (sl.node < 0) {
            const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            std::memcpy(m, id, sizeof(id));
            continue;
        }
        Xform g = globals[sl.node];
        if (sl.parent >= 0) {  // the eyes: the skin's shape may carry another body's eye positions (a worn mesh head)
            const Xform& h = globals[sl.parent];
            g.pos = h.apply(shape ? sl.local.mul(shape->scale[sl.parent]) : sl.local);
        }
        const Quat& q = g.rot;
        double r[3][3] = {{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.w * q.z), 2 * (q.x * q.z + q.w * q.y)},
                          {2 * (q.x * q.y + q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.w * q.x)},
                          {2 * (q.x * q.z - q.w * q.y), 2 * (q.y * q.z + q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)}};
        Vec3 sc = sl.shaped && shape ? shape->scale[sl.node] : Vec3{1, 1, 1};
        for (int i = 0; i < 3; ++i) {
            double t = g.pos[i];
            for (int k = 0; k < 3; ++k) {
                double e = r[i][k] * sc[k];
                m[i * 4 + k] = static_cast<float>(e);
                t -= e * sl.bind[k];
            }
            m[i * 4 + 3] = static_cast<float>(t);
        }
    }

    size_t nv = vblend_.size();
    positions.resize(nv * 3);
    normals.resize(nv * 3);
    for (size_t v = 0; v < nv; ++v) {
        const float* a = &mats_[vslot_[v * 2] * 12];
        const float* b = &mats_[vslot_[v * 2 + 1] * 12];
        float f = vblend_[v];
        float blended[12];
        const float* m = a;
        if (a != b && f != 0) {
            for (int k = 0; k < 12; ++k) blended[k] = a[k] + (b[k] - a[k]) * f;
            m = blended;
        }
        const float* p = &coords_[v * 3];
        const float* n = &normals_[v * 3];
        float* po = &positions[v * 3];
        float* no = &normals[v * 3];
        for (int i = 0; i < 3; ++i) {
            po[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3];
            no[i] = m[i * 4] * n[0] + m[i * 4 + 1] * n[1] + m[i * 4 + 2] * n[2];
        }
        float l = std::sqrt(no[0] * no[0] + no[1] * no[1] + no[2] * no[2]);
        if (l > 0)
            for (int i = 0; i < 3; ++i) no[i] /= l;
    }
}

}  // namespace vats
