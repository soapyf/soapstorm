// Viewport Avatar Toolset - COLLADA (.dae) import for props and rigged meshes.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/dae.h"
#include "guard.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "vats/xml.h"

namespace vats {
namespace {

namespace fs = std::filesystem;

// Affine 3x4 matrix, row-major, column vectors (p' = M p), as COLLADA writes them.
struct Mat {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    Mat operator*(const Mat& o) const {
        Mat r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j] + (j == 3 ? m[i][3] : 0);
        return r;
    }
    Vec3 dir(const Vec3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Vec3 origin() const { return {m[0][3], m[1][3], m[2][3]}; }
    Vec3 point(const Vec3& v) const { return dir(v) + origin(); }

    // Cofactor matrix of the 3x3 part: det * inverse-transpose.
    Mat cofactor() const {
        auto& a = m;
        Mat c;
        c.m[0][0] = a[1][1] * a[2][2] - a[1][2] * a[2][1];
        c.m[0][1] = a[1][2] * a[2][0] - a[1][0] * a[2][2];
        c.m[0][2] = a[1][0] * a[2][1] - a[1][1] * a[2][0];
        c.m[1][0] = a[0][2] * a[2][1] - a[0][1] * a[2][2];
        c.m[1][1] = a[0][0] * a[2][2] - a[0][2] * a[2][0];
        c.m[1][2] = a[0][1] * a[2][0] - a[0][0] * a[2][1];
        c.m[2][0] = a[0][1] * a[1][2] - a[0][2] * a[1][1];
        c.m[2][1] = a[0][2] * a[1][0] - a[0][0] * a[1][2];
        c.m[2][2] = a[0][0] * a[1][1] - a[0][1] * a[1][0];
        c.m[0][3] = c.m[1][3] = c.m[2][3] = 0;
        return c;
    }
    double det() const {
        Mat c = cofactor();
        return m[0][0] * c.m[0][0] + m[0][1] * c.m[0][1] + m[0][2] * c.m[0][2];
    }
    // Normals: the inverse-transpose, so its sign follows the determinant.
    Mat normal_matrix() const {
        double d = det();
        if (!(std::fabs(d) > 1e-300)) return *this;
        Mat c = cofactor();
        for (auto& row : c.m)
            for (double& e : row) e /= d;
        return c;
    }
    // A singular matrix gives identity.
    Mat inverse() const {
        double d = det();
        if (!(std::fabs(d) > 1e-300)) return {};
        Mat c = cofactor(), r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = c.m[j][i] / d;
        Vec3 t = r.dir(origin());
        r.m[0][3] = -t.x, r.m[1][3] = -t.y, r.m[2][3] = -t.z;
        return r;
    }
};

Mat scaling(const Vec3& s) {
    Mat r;
    r.m[0][0] = s.x, r.m[1][1] = s.y, r.m[2][2] = s.z;
    return r;
}

Mat from_rows(const double* f) {  // 16 values, row-major; the last row is ignored
    Mat r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = f[i * 4 + j];
    return r;
}

Mat from_xform(const Xform& x) {
    const Quat& q = x.rot;
    double f[16] = {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.w * q.z), 2 * (q.x * q.z + q.w * q.y), x.pos.x,
                    2 * (q.x * q.y + q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.w * q.x), x.pos.y,
                    2 * (q.x * q.z - q.w * q.y), 2 * (q.y * q.z + q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y), x.pos.z,
                    0, 0, 0, 1};
    return from_rows(f);
}

// The rotation of the 3x3 part after Gram-Schmidt on its columns (a mirror becomes a proper rotation).
Quat orthonormal_rotation(const Mat& a) {
    Vec3 x = Vec3{a.m[0][0], a.m[1][0], a.m[2][0]}.normalized();
    Vec3 c1{a.m[0][1], a.m[1][1], a.m[2][1]};
    Vec3 y = (c1 - x * x.dot(c1)).normalized();
    if (x.length() == 0 || y.length() == 0) return {};
    Vec3 z = x.cross(y);
    double r[3][3] = {{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}};
    Quat q;
    double tr = r[0][0] + r[1][1] + r[2][2];
    if (tr > 0) {
        double s = std::sqrt(tr + 1) * 2;
        q = {s / 4, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        double s = std::sqrt(1 + r[0][0] - r[1][1] - r[2][2]) * 2;
        q = {(r[2][1] - r[1][2]) / s, s / 4, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s};
    } else if (r[1][1] > r[2][2]) {
        double s = std::sqrt(1 + r[1][1] - r[0][0] - r[2][2]) * 2;
        q = {(r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, s / 4, (r[1][2] + r[2][1]) / s};
    } else {
        double s = std::sqrt(1 + r[2][2] - r[0][0] - r[1][1]) * 2;
        q = {(r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, s / 4};
    }
    return q.normalized();
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// Whitespace-separated numbers. False when a token is not a number. Out-of-range values become max().
template <class T>
bool parse_list(std::string_view s, std::vector<T>& out) {
    out.clear();
    const char* p = s.data();
    const char* end = p + s.size();
    for (;;) {
        while (p < end && is_space(*p)) ++p;
        if (p == end) return true;
        if (*p == '+') ++p;
        T v{};
        auto r = std::from_chars(p, end, v);
        if (r.ptr == p || (r.ptr < end && !is_space(*r.ptr))) return false;
        if (r.ec == std::errc::result_out_of_range) v = std::numeric_limits<T>::max();
        out.push_back(v);
        p = r.ptr;
    }
}

std::vector<std::string> tokens(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) ++i;
        size_t b = i;
        while (i < s.size() && !is_space(s[i])) ++i;
        if (i > b) out.emplace_back(s.substr(b, i - b));
    }
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

void add_unique(std::vector<std::string>& list, std::string s) {
    if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(std::move(s));
}

// An accessor over a float_array: element i is data[offset + i * stride ...].
struct Source {
    std::vector<double> data;
    size_t stride = 1, offset = 0, count = 0;

    // Reads n components (missing ones are 0). False when i is out of range.
    bool get(long long i, int n, double* out) const {
        if (i < 0 || static_cast<unsigned long long>(i) >= count) return false;
        size_t b = offset + static_cast<size_t>(i) * stride;
        for (int k = 0; k < n; ++k) out[k] = static_cast<size_t>(k) < stride ? data[b + k] : 0.0;
        return true;
    }
};

struct Influences {
    std::array<int, 4> joint;
    std::array<float, 4> weight;
};

struct Skin {
    Mat bind_shape;
    std::vector<int> node;  // SK-40 index per joint; mRoot when unmapped
    std::vector<bool> has_ibm;
    std::vector<Mat> ibm;
    std::vector<Influences> vertex;  // by position index
};

struct Instance {
    const XmlNode* geometry;
    const XmlNode* skin;  // null for a static instance_geometry
    Mat world;
    std::vector<std::pair<std::string, std::string>> materials;  // symbol -> material id
};

struct Build {
    std::vector<float> pos, nrm, uv;
    std::vector<std::uint32_t> idx;
    std::vector<int> joints;
    std::vector<float> weights;
};

class Loader {
public:
    Loader(const Skeleton& s, const std::string& d, DaeModel& m, DaeReport& r) : skel(s), dir(d), model(m), rep(r) {}

    bool run(std::string_view text, std::string& err);

private:
    const Skeleton& skel;
    const std::string& dir;
    DaeModel& model;
    DaeReport& rep;

    std::unordered_map<std::string_view, const XmlNode*> ids, node_sids;
    std::unordered_map<const XmlNode*, Source> sources;
    std::unordered_map<const XmlNode*, Skin> skins;
    std::unordered_map<std::string, int> material_index;
    std::vector<Build> builds;  // one per material
    std::vector<Instance> instances;
    Mat up;
    double unit = 1, rig_scale = 1;
    int visits = 0;
    long long bad_triangles = 0;

    void index(const XmlNode& n) {
        if (auto* id = n.attr("id")) ids.emplace(*id, &n);
        if (n.name == "node")
            if (auto* sid = n.attr("sid")) node_sids.emplace(*sid, &n);
        for (auto& c : n.children) index(c);
    }

    const XmlNode* resolve(std::string_view url) {
        if (url.empty()) return nullptr;
        if (url[0] != '#') {
            add_unique(rep.unsupported, "external reference " + std::string(url));
            return nullptr;
        }
        auto it = ids.find(url.substr(1));
        if (it == ids.end()) {
            add_unique(rep.warnings, "missing " + std::string(url));
            return nullptr;
        }
        return it->second;
    }

    const Source* source(std::string_view url) {
        const XmlNode* n = resolve(url);
        if (!n || n->name != "source") return nullptr;
        auto [it, fresh] = sources.try_emplace(n);
        Source& s = it->second;
        if (!fresh) return s.count ? &s : nullptr;
        const XmlNode* arr = n->child("float_array");
        if (!arr || !parse_list(arr->text, s.data)) {
            add_unique(rep.warnings, "unreadable source " + std::string(url));
            return nullptr;
        }
        long long stride = 1, offset = 0, count = -1;
        if (auto* tc = n->child("technique_common"))
            if (auto* acc = tc->child("accessor")) {
                std::vector<long long> v;
                if (parse_list(acc->attr_or("stride", "1"), v) && v.size() == 1) stride = v[0];
                if (parse_list(acc->attr_or("offset", "0"), v) && v.size() == 1) offset = v[0];
                if (parse_list(acc->attr_or("count", "-1"), v) && v.size() == 1) count = v[0];
            }
        s.stride = static_cast<size_t>(std::clamp<long long>(stride, 1, 1 << 20));
        s.offset = static_cast<size_t>(std::clamp<long long>(offset, 0, static_cast<long long>(s.data.size())));
        s.count = (s.data.size() - s.offset) / s.stride;
        if (count >= 0) s.count = std::min(s.count, static_cast<size_t>(count));
        return s.count ? &s : nullptr;
    }

    Mat local_transform(const XmlNode& n) {
        Mat m;
        std::vector<double> f;
        for (auto& c : n.children) {
            bool t = c.name == "matrix" || c.name == "translate" || c.name == "rotate" || c.name == "scale";
            if (c.name == "lookat" || c.name == "skew") add_unique(rep.unsupported, c.name);
            if (!t) continue;
            if (!parse_list(c.text, f)) {
                add_unique(rep.warnings, "unreadable <" + c.name + ">");
                continue;
            }
            if (c.name == "matrix" && f.size() >= 16) {
                m = m * from_rows(f.data());
            } else if (c.name == "translate" && f.size() >= 3) {
                Mat tr;
                tr.m[0][3] = f[0], tr.m[1][3] = f[1], tr.m[2][3] = f[2];
                m = m * tr;
            } else if (c.name == "rotate" && f.size() >= 4) {
                Vec3 axis{f[0], f[1], f[2]};
                if (axis.length() > 0) m = m * from_xform({Quat::axis_angle(axis, f[3] * kDegToRad), {}});
            } else if (c.name == "scale" && f.size() >= 3) {
                m = m * scaling({f[0], f[1], f[2]});
            }
        }
        return m;
    }

    static int count_joints(const XmlNode& n) {
        int k = n.attr_or("type") == "JOINT";
        for (auto& c : n.children)
            if (c.name == "node") k += count_joints(c);
        return k;
    }

    void add_instance(const XmlNode& inst, const XmlNode* geometry, const XmlNode* skin, const Mat& world) {
        if (!geometry) return;
        if (geometry->name != "geometry") {
            add_unique(rep.unsupported, geometry->name == "controller" ? "morph" : geometry->name);
            return;
        }
        Instance in{geometry, skin, world, {}};
        if (auto* bm = inst.child("bind_material"))
            if (auto* tc = bm->child("technique_common"))
                for (auto& im : tc->children)
                    if (im.name == "instance_material") {
                        std::string target = im.attr_or("target");
                        if (!target.empty() && target[0] == '#') target.erase(0, 1);
                        in.materials.emplace_back(im.attr_or("symbol"), target);
                    }
        instances.push_back(std::move(in));
    }

    void visit(const XmlNode& n, const Mat& parent, int depth) {
        if (depth > 64 || ++visits > 100000) {
            add_unique(rep.warnings, "node hierarchy too deep or too large; the rest is skipped");
            return;
        }
        if (n.attr_or("type") == "JOINT") {
            rep.skipped_joint_nodes += count_joints(n);
            return;
        }
        Mat world = parent * local_transform(n);
        for (auto& c : n.children) {
            if (c.name == "node") {
                visit(c, world, depth + 1);
            } else if (c.name == "instance_node") {
                if (auto* t = resolve(c.attr_or("url"))) visit(*t, world, depth + 1);
            } else if (c.name == "instance_geometry") {
                add_instance(c, resolve(c.attr_or("url")), nullptr, world);
            } else if (c.name == "instance_controller") {
                const XmlNode* ctl = resolve(c.attr_or("url"));
                if (!ctl) continue;
                const XmlNode* skin = ctl->child("skin");
                if (!skin) {
                    add_unique(rep.unsupported, ctl->child("morph") ? "morph" : "controller without skin");
                    continue;
                }
                add_instance(c, resolve(skin->attr_or("source")), skin, world);
            } else if (c.name == "instance_camera" || c.name == "instance_light") {
                add_unique(rep.unsupported, c.name);
            }
        }
    }

    int map_name(const std::string& name) {
        int i = map_skin_joint(skel, name);
        if (i < 0) {  // step 5: a node whose sid is the name
            auto it = node_sids.find(name);
            if (it != node_sids.end()) i = map_skin_joint(skel, it->second->attr_or("name"));
        }
        if (i < 0) add_unique(rep.unmapped_joints, name);
        return i < 0 ? dae_root(skel) : i;
    }

    void read_skin(const XmlNode& s, Skin& k) {
        std::vector<double> f;
        if (auto* b = s.child("bind_shape_matrix")) {
            if (parse_list(b->text, f) && f.size() >= 16)
                k.bind_shape = from_rows(f.data());
            else
                add_unique(rep.warnings, "unreadable bind_shape_matrix");
        }
        const Source* ibm = nullptr;
        if (auto* joints = s.child("joints"))
            for (auto& in : joints->children) {
                if (in.name != "input") continue;
                std::string sem = in.attr_or("semantic");
                if (sem == "INV_BIND_MATRIX") ibm = source(in.attr_or("source"));
                if (sem != "JOINT") continue;
                const XmlNode* src = resolve(in.attr_or("source"));
                if (!src) continue;
                bool idref = false;
                const XmlNode* arr = src->child("Name_array");
                if (!arr) idref = (arr = src->child("IDREF_array")) != nullptr;
                if (!arr) continue;
                for (std::string& t : tokens(arr->text)) {
                    std::string name = t;
                    if (idref)
                        if (auto it = ids.find(t); it != ids.end())
                            name = it->second->attr_or("name", it->second->attr_or("sid", t));
                    k.node.push_back(map_name(name));
                }
            }
        size_t nj = k.node.size();
        k.has_ibm.assign(nj, false);
        k.ibm.resize(nj);
        double buf[16];
        for (size_t j = 0; j < nj && ibm; ++j)
            if (ibm->get(static_cast<long long>(j), 16, buf) && ibm->stride >= 16) {
                k.ibm[j] = from_rows(buf);
                k.has_ibm[j] = true;
            }

        // Weights: summed per bone, the four largest kept and renormalised; nothing -> 100 % root.
        const XmlNode* vw = s.child("vertex_weights");
        if (!vw) return;
        long long jo = -1, wo = -1, stride = 1;
        const Source* wsrc = nullptr;
        std::vector<long long> v;
        for (auto& in : vw->children) {
            if (in.name != "input") continue;
            long long off = 0;
            if (parse_list(in.attr_or("offset", "0"), v) && v.size() == 1) off = v[0];
            if (off < 0 || off > 64) continue;
            stride = std::max(stride, off + 1);
            if (in.attr_or("semantic") == "JOINT") jo = off;
            if (in.attr_or("semantic") == "WEIGHT") wo = off, wsrc = source(in.attr_or("source"));
        }
        std::vector<long long> vcount;
        auto* vc = vw->child("vcount");
        auto* vv = vw->child("v");
        if (jo < 0 || !wsrc || !vc || !vv || !parse_list(vc->text, vcount) || !parse_list(vv->text, v)) {
            add_unique(rep.warnings, "unreadable vertex_weights");
            return;
        }
        int root = dae_root(skel);
        size_t at = 0;
        std::vector<std::pair<int, double>> sum;
        for (long long n : vcount) {
            sum.clear();
            for (long long e = 0; e < n && at + stride <= v.size(); ++e, at += stride) {
                long long j = v[at + jo];
                double w = 0;
                if (!wsrc->get(v[at + wo], 1, &w) || !(w > 0) || !std::isfinite(w)) continue;
                int node = j == -1 ? root : j >= 0 && static_cast<size_t>(j) < nj ? k.node[j] : -1;
                if (node < 0) continue;
                auto it = std::find_if(sum.begin(), sum.end(), [&](auto& p) { return p.first == node; });
                if (it != sum.end())
                    it->second += w;
                else
                    sum.emplace_back(node, w);
            }
            std::stable_sort(sum.begin(), sum.end(), [](auto& a, auto& b) { return a.second > b.second; });
            if (sum.size() > 4) sum.resize(4);
            double total = 0;
            for (auto& p : sum) total += p.second;
            Influences inf{{root, root, root, root}, {1, 0, 0, 0}};
            if (total > 0 && std::isfinite(total))
                for (size_t i = 0; i < sum.size(); ++i) {
                    inf.joint[i] = sum[i].first;
                    inf.weight[i] = static_cast<float>(sum[i].second / total);
                }
            k.vertex.push_back(inf);
            if (n < 0 || at + stride > v.size()) break;
        }
    }

    // Rig scale (§3.3.4): median of rest distance / bind distance over bones > 0.3 m out, snapped.
    void measure_rig_scale(const std::vector<Xform>& rest) {
        std::vector<double> ratios;
        for (auto& in : instances) {
            if (!in.skin) continue;
            const Skin& k = skins[in.skin];
            for (size_t j = 0; j < k.node.size(); ++j) {
                if (!k.has_ibm[j] || k.node[j] == dae_root(skel)) continue;
                double r = rest[k.node[j]].pos.length(), b = k.ibm[j].inverse().origin().length();
                if (r > 0.3 && b > 1e-12 && std::isfinite(b)) ratios.push_back(r / b);
            }
        }
        if (!ratios.empty()) rig_scale = rig_scale_of(ratios, rep.measured_scale, unit);
    }

    std::string find_texture(std::string raw) {
        std::string p;
        for (size_t i = 0; i < raw.size(); ++i) {
            int hi, lo;
            auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) ? (c <= '9' ? c - '0' : (c | 32) - 'a' + 10) : -1; };
            if (raw[i] == '%' && i + 2 < raw.size() && (hi = hex(raw[i + 1])) >= 0 && (lo = hex(raw[i + 2])) >= 0) {
                p += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                p += raw[i] == '\\' ? '/' : raw[i];
            }
        }
        if (p.rfind("file://", 0) == 0) p.erase(0, 7);
        if (p.size() >= 3 && p[0] == '/' && std::isalpha(static_cast<unsigned char>(p[1])) && p[2] == ':') p.erase(0, 1);
        std::error_code ec;
        std::vector<fs::path> tries;
        if (fs::path(p).is_absolute()) tries.push_back(p);
        if (!dir.empty()) {
            tries.push_back(fs::path(dir) / p);
            tries.push_back(fs::path(dir) / p.substr(p.find_last_of('/') + 1));
        }
        for (auto& t : tries)
            if (fs::is_regular_file(t, ec)) return t.lexically_normal().string();
        add_unique(rep.missing_textures, raw);
        return {};
    }

    static const XmlNode* find_newparam(const XmlNode& n, std::string_view sid) {
        for (auto& c : n.children) {
            if (c.name == "newparam" && c.attr_or("sid") == sid) return &c;
            if (c.name == "technique")
                if (auto* r = find_newparam(c, sid)) return r;
        }
        return nullptr;
    }

    // diffuse/texture: sampler newparam -> surface newparam -> image, or sampler2D/instance_image (1.5).
    std::string texture_path(const XmlNode& profile, std::string_view sampler) {
        const XmlNode* image = nullptr;
        if (auto* np = find_newparam(profile, sampler)) {
            if (auto* s2 = np->child("sampler2D")) {
                if (auto* ii = s2->child("instance_image")) {
                    image = resolve(ii->attr_or("url"));
                } else if (auto* src = s2->child("source")) {
                    if (auto* np2 = find_newparam(profile, trim(src->text)))
                        if (auto* surf = np2->child("surface"))
                            if (auto* init = surf->child("init_from")) image = resolve("#" + std::string(trim(init->text)));
                }
            }
        } else {
            image = resolve("#" + std::string(sampler));  // many exporters name the image directly
        }
        if (!image || image->name != "image") return {};
        const XmlNode* init = image->child("init_from");
        if (!init) return {};
        const XmlNode* ref = init->child("ref");
        std::string_view path = trim(ref ? ref->text : init->text);
        return path.empty() ? std::string() : find_texture(std::string(path));
    }

    DaeMaterial read_material(const std::string& id) {
        DaeMaterial m;
        m.name = id;
        m.double_sided = true;  // until a profile_COMMON technique says otherwise
        auto it = ids.find(id);
        if (it == ids.end() || it->second->name != "material") return m;
        auto* ie = it->second->child("instance_effect");
        const XmlNode* fx = ie ? resolve(ie->attr_or("url")) : nullptr;
        const XmlNode* profile = fx ? fx->child("profile_COMMON") : nullptr;
        const XmlNode* tech = profile ? profile->child("technique") : nullptr;
        const XmlNode* shading = nullptr;
        for (const char* s : {"phong", "lambert", "blinn", "constant"})
            if (!shading && tech) shading = tech->child(s);
        if (!shading) {
            if (fx) add_unique(rep.unsupported, "effect without a profile_COMMON phong/lambert/blinn/constant technique");
            return m;
        }
        m.double_sided = false;
        std::vector<double> f;
        if (auto* d = shading->child("diffuse")) {
            if (auto* c = d->child("color"); c && parse_list(c->text, f) && f.size() >= 3) {
                for (size_t i = 0; i < 4; ++i) m.rgba[i] = static_cast<float>(i < f.size() ? f[i] : 1.0);
            } else if (auto* t = d->child("texture")) {
                m.texture = texture_path(*profile, t->attr_or("texture"));
                if (!m.texture.empty()) m.rgba = {1, 1, 1, 1};
            }
        }
        if (auto* t = shading->child("transparency"))
            if (auto* fl = t->child("float"); fl && parse_list(fl->text, f) && f.size() == 1 && f[0] < 0.999)
                m.rgba[3] *= static_cast<float>(std::max(0.0, f[0]));
        m.blend = m.rgba[3] < 0.999f;
        return m;
    }

    int material_for(const std::string& id) {
        auto [it, fresh] = material_index.try_emplace(id, static_cast<int>(model.materials.size()));
        if (fresh) {
            model.materials.push_back(read_material(id));
            builds.emplace_back();
        }
        return it->second;
    }

    void build(const Instance& in);
};

void Loader::build(const Instance& in) {
    const XmlNode* mesh = in.geometry->child("mesh");
    if (!mesh) {
        for (auto& c : in.geometry->children)
            if (c.name != "asset" && c.name != "extra") add_unique(rep.unsupported, c.name);
        return;
    }
    const Skin* skin = in.skin ? &skins[in.skin] : nullptr;
    bool rigged_skin = skin && model.rigged;
    Mat x = rigged_skin ? scaling({rig_scale, rig_scale, rig_scale}) * up * skin->bind_shape
                        : skin ? in.world * skin->bind_shape : in.world;
    Mat nm = x.normal_matrix();
    bool flip = x.det() < 0;
    int root = dae_root(skel);

    std::map<std::tuple<int, long long, long long, long long>, std::uint32_t> vmap;
    std::map<std::pair<int, long long>, Vec3> smooth;
    std::vector<std::tuple<int, std::uint32_t, long long>> generated;  // material, vertex, position index
    std::vector<long long> p, vcount;

    for (auto& prim : mesh->children) {
        const std::string& kind = prim.name;
        if (kind == "lines" || kind == "linestrips") add_unique(rep.unsupported, kind);
        if (kind != "triangles" && kind != "polylist" && kind != "polygons" && kind != "tristrips" && kind != "trifans")
            continue;

        // Inputs: VERTEX (its vertices/POSITION, and NORMAL/TEXCOORD there per vertex), NORMAL, TEXCOORD.
        const Source *pos = nullptr, *nrm = nullptr, *uv = nullptr;
        long long vo = -1, no = -1, to = -1, stride = 1;
        bool uv_set0 = false, bad_input = false;
        auto take_uv = [&](const XmlNode& input, long long off) {
            bool set0 = input.attr_or("set") == "0";
            if (uv && (uv_set0 || !set0)) return;
            if (const Source* s = source(input.attr_or("source"))) uv = s, to = off, uv_set0 = set0;
        };
        std::vector<long long> v;
        for (auto& input : prim.children) {
            if (input.name != "input") continue;
            long long off = 0;
            if (!parse_list(input.attr_or("offset", "0"), v) || v.size() != 1 || v[0] < 0 || v[0] > 64) {
                bad_input = true;
                continue;
            }
            off = v[0];
            stride = std::max(stride, off + 1);
            std::string sem = input.attr_or("semantic");
            if (sem == "VERTEX") {
                vo = off;
                const XmlNode* verts = resolve(input.attr_or("source"));
                if (!verts) continue;
                for (auto& vi : verts->children) {
                    if (vi.name != "input") continue;
                    std::string vs = vi.attr_or("semantic");
                    if (vs == "POSITION") pos = source(vi.attr_or("source"));
                    if (vs == "NORMAL" && !nrm) nrm = source(vi.attr_or("source")), no = -1;
                    if (vs == "TEXCOORD") take_uv(vi, -1);
                }
            } else if (sem == "NORMAL") {
                if (const Source* s = source(input.attr_or("source"))) nrm = s, no = off;
            } else if (sem == "TEXCOORD") {
                take_uv(input, off);
            }
        }
        if (!pos || vo < 0 || bad_input) {
            add_unique(rep.warnings, "<" + kind + "> without usable positions skipped");
            continue;
        }

        std::string symbol = prim.attr_or("material"), mat_id;
        for (auto& [s, t] : in.materials)
            if (s == symbol) mat_id = t;
        if (auto it = ids.find(symbol); mat_id.empty() && it != ids.end() && it->second->name == "material") mat_id = symbol;
        int g = material_for(mat_id);
        Build& b = builds[g];

        struct Corner {
            long long vi, ni, ti;
            Vec3 p, n;
            double t[2];
        };
        auto corner = [&](size_t c, Corner& out) {
            size_t at = c * static_cast<size_t>(stride);
            if (at + stride > p.size()) return false;
            double f[3];
            out.vi = p[at + vo];
            if (!pos->get(out.vi, 3, f)) return false;
            out.p = {f[0], f[1], f[2]};
            out.ni = nrm ? p[at + (no < 0 ? vo : no)] : -1;
            if (nrm && !nrm->get(out.ni, 3, f)) return false;
            out.n = nrm ? Vec3{f[0], f[1], f[2]} : Vec3{};
            out.ti = uv ? p[at + (to < 0 ? vo : to)] : -1;
            out.t[0] = out.t[1] = 0;
            return !uv || uv->get(out.ti, 2, out.t);
        };
        auto vertex = [&](const Corner& c) {
            auto [it, fresh] = vmap.try_emplace({g, c.vi, c.ni, c.ti}, static_cast<std::uint32_t>(b.pos.size() / 3));
            if (!fresh) return it->second;
            Vec3 wp = x.point(c.p), wn = nm.dir(c.n).normalized();
            b.pos.insert(b.pos.end(), {float(wp.x), float(wp.y), float(wp.z)});
            b.nrm.insert(b.nrm.end(), {float(wn.x), float(wn.y), float(wn.z)});
            b.uv.insert(b.uv.end(), {float(c.t[0]), float(1 - c.t[1])});
            if (model.rigged) {
                Influences f{{root, root, root, root}, {1, 0, 0, 0}};
                if (rigged_skin && c.vi < static_cast<long long>(skin->vertex.size())) f = skin->vertex[c.vi];
                b.joints.insert(b.joints.end(), f.joint.begin(), f.joint.end());
                b.weights.insert(b.weights.end(), f.weight.begin(), f.weight.end());
            }
            if (!nrm) generated.emplace_back(g, it->second, c.vi);
            return it->second;
        };
        auto tri = [&](size_t c0, size_t c1, size_t c2) {
            Corner c[3];
            if (!corner(c0, c[0]) || !corner(c1, c[1]) || !corner(c2, c[2])) {
                ++bad_triangles;
                return;
            }
            if (flip) std::swap(c[1], c[2]);
            std::uint32_t k[3];
            for (int i = 0; i < 3; ++i) k[i] = vertex(c[i]);
            b.idx.insert(b.idx.end(), k, k + 3);
            if (nrm) return;
            auto at = [&](int i) { return Vec3{b.pos[k[i] * 3], b.pos[k[i] * 3 + 1], b.pos[k[i] * 3 + 2]}; };
            Vec3 fn = (at(1) - at(0)).cross(at(2) - at(0));  // area-weighted
            for (auto& cc : c) smooth[{g, cc.vi}] += fn;
        };
        auto fan = [&](size_t first, size_t n) {
            for (size_t i = 1; i + 1 < n; ++i) tri(first, first + i, first + i + 1);
        };

        bool parsed = true;
        if (kind == "triangles" || kind == "polylist") {
            const XmlNode* pn = prim.child("p");
            parsed = pn && parse_list(pn->text, p);
            size_t corners = parsed ? p.size() / stride : 0;
            if (kind == "triangles") {
                for (size_t c = 0; c + 3 <= corners; c += 3) tri(c, c + 1, c + 2);
            } else if (const XmlNode* vc = prim.child("vcount"); parsed && vc && parse_list(vc->text, vcount)) {
                size_t at = 0;
                for (long long n : vcount) {
                    if (n < 0 || static_cast<size_t>(n) > corners - at) {
                        ++bad_triangles;
                        break;
                    }
                    fan(at, static_cast<size_t>(n));
                    at += static_cast<size_t>(n);
                }
            } else {
                parsed = false;
            }
        } else {
            for (auto& pc : prim.children) {
                if (pc.name == "ph") add_unique(rep.unsupported, "ph");
                if (pc.name != "p") continue;
                if (!parse_list(pc.text, p)) {
                    parsed = false;
                    continue;
                }
                size_t n = p.size() / stride;
                if (kind == "tristrips") {
                    for (size_t i = 0; i + 2 < n; ++i) i % 2 ? tri(i + 1, i, i + 2) : tri(i, i + 1, i + 2);
                } else {
                    fan(0, n);
                }
            }
        }
        if (!parsed) add_unique(rep.warnings, "unreadable index list in <" + kind + ">");
    }

    for (auto& [g, vert, vi] : generated) {
        Vec3 n = smooth[{g, vi}].normalized();
        if (n.length() == 0) n = {0, 0, 1};
        float* o = &builds[g].nrm[vert * 3];
        o[0] = float(n.x), o[1] = float(n.y), o[2] = float(n.z);
    }
}

bool Loader::run(std::string_view text, std::string& err) {
    XmlNode root;
    if (!parse_xml(text, root, err)) return false;
    if (root.name != "COLLADA") {
        err = "not a COLLADA document";
        return false;
    }
    index(root);

    std::vector<double> f;
    if (auto* asset = root.child("asset")) {
        if (auto* u = asset->child("unit"))
            if (parse_list(u->attr_or("meter", "1"), f) && f.size() == 1 && f[0] > 0 && std::isfinite(f[0])) unit = f[0];
        if (auto* ua = asset->child("up_axis")) {
            std::string_view axis = trim(ua->text);
            if (axis == "Y_UP") {  // (x, y, z) -> (x, -z, y)
                up.m[1][1] = 0, up.m[1][2] = -1, up.m[2][1] = 1, up.m[2][2] = 0;
                rep.up_axis = "Y_UP";
            } else if (axis == "X_UP") {  // (x, y, z) -> (-y, -z, x)
                up = {};
                up.m[0][0] = 0, up.m[0][1] = -1, up.m[1][1] = 0, up.m[1][2] = -1, up.m[2][0] = 1, up.m[2][2] = 0;
                rep.up_axis = "X_UP";
            } else if (axis != "Z_UP" && !axis.empty()) {
                add_unique(rep.warnings, "unknown up_axis " + std::string(axis) + ", using Z_UP");
            }
        }
    }

    const XmlNode* scene = nullptr;
    if (auto* s = root.child("scene"))
        if (auto* ivs = s->child("instance_visual_scene")) scene = resolve(ivs->attr_or("url"));
    if (!scene || scene->name != "visual_scene")
        if (auto* lib = root.child("library_visual_scenes")) scene = lib->child("visual_scene");
    if (!scene) {
        err = "no visual scene";
        return false;
    }
    Mat top = scaling({unit, unit, unit}) * up;
    for (auto& n : scene->children)
        if (n.name == "node") visit(n, top, 0);

    // Skins, then IO-37: rigged when any joint maps to a skeleton node or collision volume.
    for (auto& in : instances)
        if (in.skin && !skins.count(in.skin)) read_skin(*in.skin, skins[in.skin]);
    for (auto& [node, k] : skins)
        for (int i : k.node) model.rigged |= i != dae_root(skel);
    rep.rigged = model.rigged;
    rep.skins_as_static = !skins.empty() && !model.rigged;
    rep.scale = unit;

    std::vector<bool> bound;
    if (model.rigged) {
        std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
        rest.push_back({});  // mRoot
        for (auto& v : skel.volumes()) rest.push_back(rest[v.joint] * Xform{v.rot, v.pos});
        measure_rig_scale(rest);
        rep.scale = rig_scale;
        model.binds = rest;
        bound.assign(rest.size(), false);
        for (auto& in : instances) {
            if (!in.skin) continue;
            const Skin& k = skins[in.skin];
            for (size_t j = 0; j < k.node.size(); ++j) {
                int n = k.node[j];
                // ponytail: first bind of a node wins; controllers disagreeing on one joint's bind are rare.
                if (!k.has_ibm[j] || n == dae_root(skel) || bound[n]) continue;
                Mat b = up * k.ibm[j].inverse();
                model.binds[n] = {orthonormal_rotation(b), b.origin() * rig_scale};
                bound[n] = true;
            }
        }
    }

    for (auto& in : instances) build(in);
    if (bad_triangles)
        add_unique(rep.warnings, std::to_string(bad_triangles) + " polygons with out-of-range indices skipped");

    for (size_t g = 0; g < builds.size(); ++g) {
        Build& b = builds[g];
        if (b.idx.empty()) continue;
        DaeGroup grp{static_cast<int>(g), static_cast<std::uint32_t>(model.vertex_count()),
                     static_cast<std::uint32_t>(b.pos.size() / 3), static_cast<std::uint32_t>(model.indices.size()),
                     static_cast<std::uint32_t>(b.idx.size())};
        for (std::uint32_t i : b.idx) model.indices.push_back(grp.first_vertex + i);
        model.positions.insert(model.positions.end(), b.pos.begin(), b.pos.end());
        model.normals.insert(model.normals.end(), b.nrm.begin(), b.nrm.end());
        model.uvs.insert(model.uvs.end(), b.uv.begin(), b.uv.end());
        model.joints.insert(model.joints.end(), b.joints.begin(), b.joints.end());
        model.weights.insert(model.weights.end(), b.weights.begin(), b.weights.end());
        model.groups.push_back(grp);
    }
    settle_rig(model, skel, bound, rep);
    rep.triangles = model.triangle_count();
    if (!rep.triangles) {
        err = "no triangles in the scene";
        if (!rep.warnings.empty()) err += " (" + rep.warnings.front() + ")";
        return false;
    }
    double inf = std::numeric_limits<double>::infinity();
    Vec3 lo{inf, inf, inf}, hi = -lo;
    for (size_t i = 0; i < model.positions.size(); ++i) {
        lo[i % 3] = std::min(lo[i % 3], double(model.positions[i]));
        hi[i % 3] = std::max(hi[i % 3], double(model.positions[i]));
    }
    model.bounds_min = lo;
    model.bounds_max = hi;
    return true;
}

}  // namespace

double rig_scale_of(std::vector<double> ratios, double& measured, double declared) {
    std::sort(ratios.begin(), ratios.end());
    const size_t h = ratios.size() / 2;
    measured = ratios.size() % 2 ? ratios[h] : (ratios[h - 1] + ratios[h]) / 2;
    const double units[] = {1.0, 0.01, 0.001, 0.0254, 0.1, 10.0, 100.0};
    auto near_unit = [](double r, double u) { return std::fabs(std::log(r / u)) < 0.05; };
    // A body bound in another pose than SL's rest (an A-pose) has its arms and hands nearer the origin, and they
    // are a good third of its joints: the median lands between units. A unit a third of the joints agree on wins.
    double best = 0;
    size_t votes = 0;
    for (double u : units) {
        const size_t n = std::count_if(ratios.begin(), ratios.end(), [&](double r) { return near_unit(r, u); });
        if (n > votes) votes = n, best = u;
    }
    if (votes * 3 >= ratios.size()) return best;
    for (double u : units)
        if (near_unit(measured, u)) return u;
    // No unit fits because the rig has its own proportions (a creature with moved joints): SL uploads it at the
    // unit the file declares, so shrinking it to the median would draw it smaller than it is in-world.
    if (declared > 0 && std::fabs(std::log(measured / declared)) < std::log(2.0)) return declared;
    return measured;
}

int map_skin_joint(const Skeleton& skel, std::string_view name) {
    int root = dae_root(skel);
    size_t cut = name.find_last_of(":|");
    std::string_view suffix = cut == std::string_view::npos ? name : name.substr(cut + 1);
    for (std::string_view n : {name, suffix})  // 1: collision volume
        for (const std::string& q : {std::string(n), upper(n)})
            if (int v = skel.find_volume(q); v >= 0) return dae_volume(skel, v);
    if (name == "mRoot") return root;  // 2
    if (int i = skel.find(name); i >= 0) return i;  // 3: SK-7 with aliases
    cut = name.find_last_of(":|_");  // 4: the part after the last prefix separator
    if (cut != std::string_view::npos) {
        std::string_view tail = name.substr(cut + 1);
        if (tail == "mRoot") return root;
        if (int i = skel.find(tail); i >= 0 && !tail.empty()) return i;
    }
    return -1;
}

static bool load_dae_text(std::string_view xml_text, const std::string& dae_dir, const Skeleton& skel, DaeModel& out,
                          DaeReport& report, std::string& err) {
    out = DaeModel();
    report = DaeReport();
    return Loader(skel, dae_dir, out, report).run(xml_text, err);
}

void skin_prop(const DaeModel& model, const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
               std::vector<float>& positions, std::vector<float>& normals) {
    positions = model.positions;
    normals = model.normals;
    int count = dae_index_count(skel), root = dae_root(skel);
    size_t nv = model.positions.size() / 3;
    if (!model.rigged || globals.size() < static_cast<size_t>(skel.size()) ||
        model.binds.size() != static_cast<size_t>(count) || model.joints.size() < nv * 4 || model.weights.size() < nv * 4)
        return;
    auto scaled = [&](int node) {
        Vec3 s = shape && static_cast<size_t>(node) < shape->scale.size() ? shape->scale[node] : Vec3{1, 1, 1};
        return from_xform(globals[node]) * scaling(s);
    };
    // SK-36 conventions: current global x own shape scale x inverse bind.
    std::vector<Mat> mats(count);
    for (int i = 0; i < count; ++i) {
        Mat g;
        if (i < root) g = scaled(i);
        if (i > root) {
            const CollisionVolume& v = skel.volumes()[i - root - 1];
            if (v.node >= 0 && static_cast<size_t>(v.node) < globals.size()) {
                // Animated volume (README decision 12): its own node, relative to the joint, with the joint's
                // shape scale between the translation and the rotation. At rest this is the line below.
                Xform local = globals[v.joint].inverse() * globals[v.node];
                Vec3 s = shape && static_cast<size_t>(v.joint) < shape->scale.size() ? shape->scale[v.joint] : Vec3{1, 1, 1};
                g = from_xform(globals[v.joint]) * from_xform({Quat{}, local.pos}) * scaling(s) * from_xform({local.rot, {}});
            } else {
                g = scaled(v.joint) * from_xform({v.rot, v.pos});
            }
        }
        mats[i] = g * from_xform(model.binds[i].inverse());
    }
    for (size_t v = 0; v < nv; ++v) {
        Mat m;
        for (auto& row : m.m)
            for (double& e : row) e = 0;
        double total = 0;
        int main_joint = -1;
        for (int k = 0; k < 4; ++k) {
            int j = model.joints[v * 4 + k];
            double w = model.weights[v * 4 + k];
            if (!(w > 0) || j < 0 || j >= count) continue;
            if (main_joint < 0) main_joint = j;  // influences are stored largest first
            total += w;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) m.m[r][c] += w * mats[j].m[r][c];
        }
        const float* p = &model.positions[v * 3];
        const float* n = &model.normals[v * 3];
        if (total < 1e-6) continue;  // no usable weight: stays where it was bound, never at the origin
        const Vec3 bind_p{p[0], p[1], p[2]};
        Vec3 po = m.point(bind_p), no = m.dir({n[0], n[1], n[2]}).normalized();
        // Never fling a vertex: a non-finite result, or one far further from its main joint than it was bound
        // (a broken bind, say), follows that joint rigidly instead.
        const Vec3 jn = mats[main_joint].point(model.binds[main_joint].pos);
        const double bound_d = (bind_p - model.binds[main_joint].pos).length();
        if (!std::isfinite(po.x) || !std::isfinite(po.y) || !std::isfinite(po.z) || !std::isfinite(no.x) ||
            (po - jn).length() > 2 * bound_d + 0.25) {
            po = mats[main_joint].point(bind_p);
            no = mats[main_joint].dir({n[0], n[1], n[2]}).normalized();
            if (!std::isfinite(po.x) || !std::isfinite(po.y) || !std::isfinite(po.z)) continue;
        }
        for (int i = 0; i < 3; ++i) {
            positions[v * 3 + i] = static_cast<float>(po[i]);
            normals[v * 3 + i] = static_cast<float>(no[i]);
        }
    }
}

namespace {

Quat quarter_turn(int k) { return Quat::axis_angle({0, 0, 1}, k * kPi / 2); }

void turn_binds(DaeModel& model, int k) {
    const Quat q = quarter_turn(k);
    for (size_t j = 0; j < model.binds.size(); ++j)
        if (j < model.bound.size() && model.bound[j]) {
            model.binds[j] = {(q * model.binds[j].rot).normalized(), q.rotate(model.binds[j].pos)};
            if (j < model.rig_axes.size()) model.rig_axes[j] = (q * model.rig_axes[j]).normalized();
        }
    model.turn_binds = (model.turn_binds + k) % 4;
}

void turn_vertices(DaeModel& model, int k) {
    const Quat q = quarter_turn(k);
    const size_t nv = model.positions.size() / 3;
    for (size_t v = 0; v < nv; ++v) {
        float* p = &model.positions[v * 3];
        float* m = &model.normals[v * 3];
        Vec3 a = q.rotate({p[0], p[1], p[2]}), b = q.rotate({m[0], m[1], m[2]});
        for (int i = 0; i < 3; ++i) p[i] = float(a[i]), m[i] = float(b[i]);
    }
    model.turn_vertices = (model.turn_vertices + k) % 4;
    // Keep the box in step with the vertices (it places static props).
    if (nv) {
        Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (size_t v = 0; v < nv; ++v)
            for (int i = 0; i < 3; ++i) lo[i] = std::min(lo[i], double(model.positions[v * 3 + i])),
                                        hi[i] = std::max(hi[i], double(model.positions[v * 3 + i]));
        model.bounds_min = lo, model.bounds_max = hi;
    }
}

}  // namespace

void apply_rig_turn(DaeModel& model, int turn_b, int turn_v) {
    if (!model.rigged || model.turn_decided) return;
    if (int k = ((turn_b - model.turn_binds) % 4 + 4) % 4) turn_binds(model, k);
    if (int k = ((turn_v - model.turn_vertices) % 4 + 4) % 4) turn_vertices(model, k);
}

void settle_rig(DaeModel& model, const Skeleton& skel, const std::vector<bool>& bound, DaeReport& rep) {
    const int root = dae_root(skel), count = dae_index_count(skel);
    if (!model.rigged || model.binds.size() != static_cast<size_t>(count) || bound.size() != static_cast<size_t>(count))
        return;
    model.bound = bound;
    std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
    rest.push_back({});  // mRoot
    for (auto& v : skel.volumes()) rest.push_back(rest[v.joint] * Xform{v.rot, v.pos});
    auto turn = [](int k) { return Quat::axis_angle({0, 0, 1}, k * kPi / 2); };
    auto sq = [](const Vec3& d) { return d.dot(d); };
    // A quarter turn is only judged when the evidence reaches well off the vertical axis (arms, shoulders),
    // and only a clear win moves the file.
    auto best_turn = [&](auto&& cost, double spread) {
        if (spread < 0.2) return 0;
        int best = 0;
        double lo = cost(0);
        for (int k = 1; k < 4; ++k)
            if (double c = cost(k); c < lo * 0.25) lo = c, best = k;
        return best;
    };
    auto off_axis = [](const Vec3& p) { return std::hypot(p.x, p.y); };
    // 1a. Binds: bound joints against the SL rest joints.
    double bind_spread = 0;
    for (int j = 0; j < root; ++j)
        if (bound[j] && !skel[j].attachment) bind_spread = std::max(bind_spread, off_axis(rest[j].pos));
    const int kb = best_turn([&](int k) {
        double c = 0;
        for (int j = 0; j < root; ++j)
            if (bound[j] && !skel[j].attachment) c += sq(turn(k).rotate(model.binds[j].pos) - rest[j].pos);
        return c;
    }, bind_spread);
    model.turn_decided = bind_spread >= 0.2;
    if (kb) {
        turn_binds(model, kb);
        add_unique(rep.warnings, "the rig was turned " + std::to_string(kb * 90) + " degrees about Z to face SL's +X");
    }
    // 1a'. Mixed files: Blender's COLLADA exporter writes stored SL bind data for some joints and Blender's
    // own bone positions (its axes) for others, often the face. A joint whose bind is far from its SL rest
    // but lands on it under a quarter turn takes that turn on its own. Never a half turn: Blender's axes are a
    // quarter off SL's, and a half turn only flips a joint near the centre line front to back, which is how a
    // body that moved its CHEST volume a few centimetres back used to get its chest turned inside out.
    int mixed = 0;
    for (int j = 0; j < count; ++j) {
        if (!bound[j] || j == root) continue;
        // Only the horizontal part: a turn about Z cannot change height, and a devkit's head may sit
        // well above or below SL's.
        auto flat = [&](const Vec3& a) { const Vec3 d = a - rest[j].pos; return d.x * d.x + d.y * d.y; };
        const Vec3 b = model.binds[j].pos;
        const double here = flat(b);
        if (here < 0.01 * 0.01) continue;  // already within a centimetre
        int best = 0;
        double lo = here;
        for (int k = 1; k < 4; k += 2)
            if (double c = flat(turn(k).rotate(b)); c < lo * 0.25) lo = c, best = k;
        if (best) {
            model.binds[j] = {(turn(best) * model.binds[j].rot).normalized(), turn(best).rotate(b)};
            ++mixed;
        }
    }
    if (mixed)
        add_unique(rep.warnings, std::to_string(mixed) + " joints were bound in other axes than the rest of the rig and were turned to match");
    // 1b. Vertices: each bound joint's own vertices (its heaviest weight) sit around its bind.
    std::vector<Vec3> sum(count);
    std::vector<int> n(count, 0);
    const size_t nv = model.positions.size() / 3;
    for (size_t v = 0; v < nv && v * 4 < model.joints.size(); ++v) {
        int j = model.joints[v * 4];
        if (j < 0 || j >= count || !bound[j] || model.weights[v * 4] < 0.5f) continue;
        sum[j] += Vec3{model.positions[v * 3], model.positions[v * 3 + 1], model.positions[v * 3 + 2]};
        ++n[j];
    }
    double vert_spread = 0;
    for (int j = 0; j < count; ++j)
        if (n[j]) vert_spread = std::max(vert_spread, off_axis(model.binds[j].pos));
    const int kv = best_turn([&](int k) {
        double c = 0;
        for (int j = 0; j < count; ++j)
            if (n[j]) c += n[j] * sq(turn(k).rotate(sum[j] * (1.0 / n[j])) - model.binds[j].pos);
        return c;
    }, vert_spread);
    model.turn_decided = model.turn_decided && vert_spread >= 0.2;
    if (kv) {
        turn_vertices(model, kv);
        add_unique(rep.warnings, "the mesh was turned " + std::to_string(kv * 90) +
                                     " degrees about Z to line up with its skeleton");
    }
    // 2. Bone-orientation binds. A bind rotation far from the SL rest is either a pose the body was modelled in
    // (an A-pose: SL skins it back onto the rest skeleton through the inverse bind, so it is kept) or a tool's
    // bone axes (Blender's Y along the bone: meaningless in SL). In SL's own frames a joint sits in its nearest
    // bound ancestor's frame in the direction of its SL rest offset, whatever the pose; in bone axes it mostly
    // does not. With no bound pair to judge by, any bind far (> 5 degrees) from its rest counts.
    int pairs = 0, agree = 0;
    for (int j = 0; j < root; ++j) {
        int a = skel[j].parent;
        while (a >= 0 && !bound[a]) a = skel[a].parent;
        if (!bound[j] || a < 0 || skel[j].attachment) continue;
        const Vec3 want = rest[a].rot.conj().rotate(rest[j].pos - rest[a].pos);
        const Vec3 d = model.binds[a].rot.conj().rotate(model.binds[j].pos - model.binds[a].pos);
        if (want.length() < 0.01 || d.length() < 0.005) continue;
        ++pairs;
        agree += d.normalized().dot(want.normalized()) > std::cos(15 * kDegToRad);
    }
    bool oriented = pairs && agree * 2 < pairs;
    for (int j = 0; j < count && !oriented && !pairs; ++j)
        if (bound[j] && j != root) oriented = std::fabs((rest[j].rot.conj() * model.binds[j].rot).w) < std::cos(2.5 * kDegToRad);
    if (oriented) {
        model.rig_axes.assign(count, Quat{});  // the file's own axes stay for posing (rig_axes_from_parts)
        for (int j = 0; j < count; ++j)
            if (bound[j]) model.rig_axes[j] = model.binds[j].rot, model.binds[j].rot = rest[j].rot;
    }
}

bool shape_from_binds(const Skeleton& skel, const std::vector<const DaeModel*>& parts, const Shape* base, Shape& out,
                      double tol_m) {
    const int n = skel.size();
    out = base ? *base : Shape{std::vector<Vec3>(n, Vec3{1, 1, 1}), std::vector<Vec3>(n, Vec3{}), {}, {}};
    const std::vector<Xform> rest = skel.global_pose(Pose(n));
    std::vector<const Vec3*> target(n, nullptr);
    // A pinned node is placed relative to its nearest ancestor bound in the same part, in that ancestor's bound
    // frame: a body modelled in another pose (an A-pose) keeps its bone lengths on SL's rest directions, as SL
    // reads joint positions (parent-relative), and a hand volume stays on a wrist the body did not bind.
    std::vector<int> anchor(n, -1);
    std::vector<const Xform*> anchor_bind(n, nullptr);
    bool any = false;
    for (const DaeModel* m : parts)
        if (m && m->rigged && m->binds.size() >= static_cast<size_t>(n))
        {
            // Every joint the file bound is pinned to its bind, even one that matches SL's rest, so a moved
            // parent does not carry it off; without the bound flags, only joints that differ are moved.
            auto bound = [&](int i) { return i < int(m->bound.size()) && m->bound[i]; };
            auto pin = [&](int node, int b, int a) {
                target[node] = &m->binds[b].pos;
                while (a >= 0 && !bound(a)) a = skel[a].parent;
                if (a >= 0) anchor[node] = a, anchor_bind[node] = &m->binds[a];
            };
            for (int j = 0; j < skel.joint_count(); ++j) {
                if (target[j]) continue;
                const bool moved = (m->binds[j].pos - rest[j].pos).length() > tol_m;
                if (moved || bound(j)) pin(j, j, skel[j].parent);
                any |= moved;
            }
            // Collision volumes too: fitted mesh is weighted to them, so they must sit where the body bound them.
            const auto& vols = skel.volumes();
            for (size_t v = 0; v < vols.size(); ++v) {
                const int node = vols[v].node, b = dae_volume(skel, int(v));
                if (node < 0 || node >= n || static_cast<size_t>(b) >= m->binds.size() || target[node]) continue;
                const bool moved = (m->binds[b].pos - rest[node].pos).length() > tol_m;
                if (moved || bound(b)) pin(node, b, vols[v].joint);
                any |= moved;
            }
        }
    if (!any) return false;
    // Parents come first, so each joint's parent frame is final when the joint is placed.
    std::vector<Xform> g(n);
    for (int j = 0; j < n; ++j) {
        const Node& node = skel[j];
        const int p = node.parent;
        if (target[j]) {
            const Vec3 at = anchor[j] >= 0 ? g[anchor[j]].apply(anchor_bind[j]->inverse().apply(*target[j])) : *target[j];
            const Vec3 local = p >= 0 ? g[p].inverse().apply(at) : at;
            Vec3 s = p >= 0 ? out.scale[p] : Vec3{1, 1, 1};
            out.offset[j] = Vec3{local.x / s.x, local.y / s.y, local.z / s.z} - node.pos;
        }
        Vec3 t = node.pos + out.offset[j];
        if (p >= 0) t = t.mul(out.scale[p]);
        Xform l{node.rest, t};
        g[j] = p >= 0 ? g[p] * l : l;
    }
    // Keep the feet where the base had them (SK-28), unless the body places the pelvis itself.
    const int foot = skel.find("mFootLeft");
    if (foot >= 0 && !target[0]) {
        Pose zero(n);
        out.offset[0].z += skel.global_pose(zero, base)[foot].pos.z - skel.global_pose(zero, &out)[foot].pos.z;
    }
    return true;
}

bool rig_axes_from_parts(const Skeleton& skel, const std::vector<const DaeModel*>& parts, Shape& out) {
    const int n = skel.size(), joints = skel.joint_count();
    std::vector<const DaeModel*> from(n, nullptr);  // the part that gives each joint its axes
    bool any = false;
    for (const DaeModel* m : parts)
        if (m && m->rigged && m->rig_axes.size() >= static_cast<size_t>(n) && m->bound.size() >= static_cast<size_t>(n))
            for (int j = 0; j < joints; ++j)
                if (!from[j] && m->bound[j] && !skel[j].attachment) from[j] = m, any = true;
    if (!any) return false;
    auto axes = [&](int j) { return from[j]->rig_axes[j]; };  // at the bind, in SL space
    auto at = [&](int j) { return from[j]->binds[j].pos; };
    auto kids = [&](int j) {  // the child joints bound in the same part
        std::vector<int> out_kids;
        for (int c : skel[j].children)
            if (c < joints && from[c] == from[j]) out_kids.push_back(c);
        return out_kids;
    };
    // The rig's bone axis: the signed axis that points at the child joints on most bones (Blender: +Y).
    int votes[6] = {};
    for (int j = 0; j < joints; ++j)
        for (int c : from[j] ? kids(j) : std::vector<int>{}) {
            const Vec3 d = axes(j).conj().rotate(at(c) - at(j));
            if (d.length() < 1e-4) continue;
            int k = 0;
            for (int i = 1; i < 3; ++i)
                if (std::fabs(d[i]) > std::fabs(d[k])) k = i;
            if (std::fabs(d[k]) > 0.9 * d.length()) ++votes[k * 2 + (d[k] < 0)];
        }
    const int best = int(std::max_element(votes, votes + 6) - votes);
    Vec3 bone;
    bone[votes[best] ? best / 2 : 1] = votes[best] && best % 2 ? -1 : 1;
    const std::vector<Xform> rest = skel.global_pose(Pose(n));  // a shape never turns a joint
    out.axes.assign(n, Quat{});
    out.tails.assign(n, Vec3{});
    for (int j = 0; j < joints; ++j) {
        if (!from[j]) continue;
        out.axes[j] = (rest[j].rot.conj() * axes(j)).normalized();
        const Vec3 along = axes(j).rotate(bone);  // at the bind
        double len = 0, best_cos = 0.9;
        for (int c : kids(j)) {  // the child joint the bone points at
            const Vec3 d = at(c) - at(j);
            if (const double l = d.length(); l > 1e-4 && d.dot(along) > best_cos * l) best_cos = d.dot(along) / l, len = d.dot(along);
        }
        const DaeModel& m = *from[j];
        const bool to_child = len > 0;
        for (size_t v = 0; !to_child && v < m.positions.size() / 3; ++v)  // an end bone: as far as its own vertices reach
            if (v * 4 < m.joints.size() && m.joints[v * 4] == j && m.weights[v * 4] >= 0.5f) {
                const Vec3 p{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
                len = std::max(len, (p - at(j)).dot(along));
            }
        // A bone with neither: as long as the SL bone.
        if (len < 0.005) len = skel[j].end.length() > 1e-5 ? skel[j].end.length() : 0.05;
        out.tails[j] = out.axes[j].rotate(bone) * len;
    }
    return true;
}

bool load_dae(std::string_view xml_text, const std::string& dae_dir, const Skeleton& skel, DaeModel& out,
              DaeReport& report, std::string& err) {
    return guarded(err, [&] { return load_dae_text(xml_text, dae_dir, skel, out, report, err); });
}

}  // namespace vats
