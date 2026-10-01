// Viewport Avatar Toolset - glTF 2.0 / GLB animation reader for retargeting (spec 07 RT-2.2).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Reads the node tree, the first skin's joints and the first animation, sampled at 30 fps. The rest
// pose is the nodes' own TRS (glTF exporters write the bind pose there). Float and normalised
// integer accessors are read; CUBICSPLINE samplers use their key values, linearly.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>

#include "vats/json.h"
#include "vats/retarget.h"
#include "guard.h"

namespace vats {
namespace {

std::vector<std::uint8_t> base64(std::string_view s) {
    std::vector<std::uint8_t> out;
    int val = 0, bits = -8;
    for (char c : s) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
        if (d < 0) continue;
        val = (val << 6) | d;
        if ((bits += 6) >= 0) {
            out.push_back(std::uint8_t((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

double num(const Json* j, double fallback = 0) { return j && j->is_number() ? j->num : fallback; }
// An index or count from the file; out of range (or not a number) gives the fallback.
int inum(const Json* j, int fallback = -1) {
    double v = num(j, fallback);
    return std::isfinite(v) && v >= -1e9 && v <= 1e9 ? int(v) : fallback;
}

struct Reader {
    const Json& doc;
    std::vector<std::vector<std::uint8_t>> buffers;
    std::string err;
    double budget = kMaxSamples * 8;  // floats all accessors may read together (256 MB)

    // Accessor as floats, `width` per element (normalised integers scaled to -1..1 / 0..1).
    bool floats(int index, std::vector<float>& out, int& width) {
        const Json* acc = doc.find("accessors");
        if (!acc || index < 0 || index >= int(acc->arr.size())) return err = "bad accessor", false;
        const Json& a = acc->arr[index];
        static const std::pair<const char*, int> types[] = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
        width = 0;
        if (auto* t = a.find("type"); t && t->is_string())
            for (auto& [n, w] : types)
                if (t->str == n) width = w;
        const int ct = inum(a.find("componentType"), 0), count = inum(a.find("count"), -1);
        const bool normalized = a.find("normalized") && a.find("normalized")->b;
        int size = ct == 5126 ? 4 : ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 0;
        if (!width || !size || count < 0 || double(count) * width > budget) return err = "unsupported accessor", false;
        budget -= double(count) * width;
        const Json* bv_index = a.find("bufferView");
        if (!bv_index) {  // all zeros (sparse accessors are not read)
            out.assign(size_t(count) * width, 0.f);
            return true;
        }
        const Json* views = doc.find("bufferViews");
        int vi = inum(bv_index);
        if (!views || vi < 0 || vi >= int(views->arr.size())) return err = "bad bufferView", false;
        const Json& v = views->arr[vi];
        int bi = inum(v.find("buffer"));
        if (bi < 0 || bi >= int(buffers.size())) return err = "bad buffer", false;
        const auto& buf = buffers[bi];
        // Offsets and stride are checked as doubles before any size_t arithmetic, so nothing wraps.
        const size_t elem = size_t(size) * width;
        const double view_off = num(v.find("byteOffset")), acc_off = num(a.find("byteOffset"));
        const double off = view_off + acc_off, st = num(v.find("byteStride"), double(elem));
        const double n = double(buf.size());
        if (!std::isfinite(off) || !std::isfinite(st) || view_off < 0 || acc_off < 0 || off > n || st < double(elem) || st > n)
            return err = "accessor runs past its buffer", false;
        const size_t base = size_t(off), stride = size_t(st);
        if (count > 0 && (base + elem > buf.size() || size_t(count - 1) > (buf.size() - base - elem) / stride))
            return err = "accessor runs past its buffer", false;
        out.assign(size_t(count) * width, 0.f);
        for (int e = 0; e < count; ++e)
            for (int c = 0; c < width; ++c) {
                size_t at = base + size_t(e) * stride + size_t(c) * size;
                const std::uint8_t* p = buf.data() + at;
                double x = 0;
                switch (ct) {
                    case 5126: { float f; std::memcpy(&f, p, 4); x = f; break; }
                    case 5120: x = normalized ? std::max(std::int8_t(*p) / 127.0, -1.0) : std::int8_t(*p); break;
                    case 5121: x = normalized ? *p / 255.0 : *p; break;
                    case 5122: { std::int16_t s; std::memcpy(&s, p, 2); x = normalized ? std::max(s / 32767.0, -1.0) : s; break; }
                    case 5123: { std::uint16_t s; std::memcpy(&s, p, 2); x = normalized ? s / 65535.0 : s; break; }
                }
                out[size_t(e) * width + c] = float(x);
            }
        return true;
    }
};

}  // namespace

static bool read_gltf(const std::vector<std::uint8_t>& bytes, const std::string& dir, SourceAnim& out, std::string& err) {
    out = SourceAnim{};
    std::string_view json_text;
    std::vector<std::uint8_t> bin;
    auto u32 = [&](size_t at) { std::uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v; };
    if (bytes.size() >= 20 && std::memcmp(bytes.data(), "glTF", 4) == 0) {  // GLB container
        size_t at = 12;
        while (at + 8 <= bytes.size()) {
            std::uint32_t len = u32(at), type = u32(at + 4);
            if (at + 8 + len > bytes.size()) return err = "truncated GLB", false;
            if (type == 0x4E4F534A) json_text = std::string_view(reinterpret_cast<const char*>(bytes.data() + at + 8), len);
            if (type == 0x004E4942) bin.assign(bytes.begin() + at + 8, bytes.begin() + at + 8 + len);
            at += 8 + len;
        }
    } else {
        json_text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    Json doc;
    if (!parse_json(json_text, doc, err)) return false;
    Reader rd{doc, {}, {}};
    if (auto* bufs = doc.find("buffers"))
        for (auto& b : bufs->arr) {
            const Json* uri = b.find("uri");
            if (!uri || !uri->is_string()) {
                rd.buffers.push_back(bin);
            } else if (uri->str.rfind("data:", 0) == 0) {
                rd.buffers.push_back(base64(std::string_view(uri->str).substr(uri->str.find(',') + 1)));
            } else {
                // Only a regular file beside the .gltf (or below it), and not a huge one.
                std::filesystem::path rel(uri->str);
                bool up = false;
                for (auto& part : rel) up = up || part == "..";
                std::error_code ec;
                const std::filesystem::path file = std::filesystem::path(dir) / rel;
                if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory() || up ||
                    !std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > (1u << 30) || ec)
                    return err = "buffer file must be a file beside the .gltf: " + uri->str, false;
                std::ifstream f(file, std::ios::binary);
                if (!f) return err = "missing buffer file " + uri->str, false;
                rd.buffers.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
        }
    const Json* nodes = doc.find("nodes");
    if (!nodes || nodes->arr.empty()) return err = "the glTF file has no nodes", false;
    const int nn = int(nodes->arr.size());

    // Names: VRM humanoid bone names win (0.x and 1.0), so the VRM table maps them.
    std::vector<std::string> names(nn);
    for (int i = 0; i < nn; ++i) {
        const Json* n = nodes->arr[i].find("name");
        names[i] = n && n->is_string() ? n->str : "node" + std::to_string(i);
    }
    if (const Json* ext = doc.find("extensions")) {
        if (const Json* vrm = ext->find("VRM"); vrm && vrm->find("humanoid"))
            if (const Json* hb = vrm->find("humanoid")->find("humanBones"))
                for (auto& b : hb->arr) {
                    int node = inum(b.find("node"));
                    if (node >= 0 && node < nn && b.find("bone")) names[node] = b.find("bone")->str;
                }
        if (const Json* vrm = ext->find("VRMC_vrm"); vrm && vrm->find("humanoid"))
            if (const Json* hb = vrm->find("humanoid")->find("humanBones"))
                for (auto& [bone, v] : hb->obj) {
                    int node = inum(v.find("node"));
                    if (node >= 0 && node < nn) names[node] = bone;
                }
    }

    // Joints: the first skin's joints and their ancestors; without a skin, every node.
    std::vector<int> parent(nn, -1);
    for (int i = 0; i < nn; ++i)
        if (const Json* ch = nodes->arr[i].find("children"))
            for (auto& c : ch->arr)
                if (int k = inum(&c); k >= 0 && k < nn) parent[k] = i;
    std::set<int> keep;
    const Json* skins = doc.find("skins");
    if (skins && !skins->arr.empty() && skins->arr[0].find("joints")) {
        for (auto& j : skins->arr[0].find("joints")->arr)
            for (int k = inum(&j); k >= 0 && k < nn && !keep.count(k); k = parent[k]) keep.insert(k);
    } else {
        for (int i = 0; i < nn; ++i) keep.insert(i);
        out.notes.push_back("no skin: every node is treated as a joint");
    }
    // Parents before children, walked without recursion; a cycle in the node tree is refused.
    std::vector<int> order, index(nn, -1), chain;
    std::vector<char> on_chain(nn, 0);
    for (int i : keep) {
        chain.clear();
        for (int k = i; k >= 0 && keep.count(k) && index[k] < 0; k = parent[k]) {
            if (on_chain[k]) return err = "the glTF node tree has a cycle", false;
            on_chain[k] = 1;
            chain.push_back(k);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            on_chain[*it] = 0;
            index[*it] = int(order.size());
            order.push_back(*it);
        }
    }

    auto vec = [&](const Json* a, int n, std::vector<double> d) {
        if (a && a->is_array() && int(a->arr.size()) == n)
            for (int i = 0; i < n; ++i) d[i] = num(&a->arr[i]);
        return d;
    };
    for (int i : order) {
        const Json& n = nodes->arr[i];
        SourceJoint j;
        j.name = names[i];
        j.parent = parent[i] >= 0 ? index[parent[i]] : -1;
        auto t = vec(n.find("translation"), 3, {0, 0, 0});
        auto r = vec(n.find("rotation"), 4, {0, 0, 0, 1});
        auto s = vec(n.find("scale"), 3, {1, 1, 1});
        if (const Json* m = n.find("matrix"); m && m->is_array() && m->arr.size() == 16) {  // column-major TRS
            auto e = vec(m, 16, std::vector<double>(16, 0));
            t = {e[12], e[13], e[14]};
            Vec3 c0{e[0], e[1], e[2]}, c1{e[4], e[5], e[6]}, c2{e[8], e[9], e[10]};
            s = {c0.length(), c1.length(), c2.length()};
            c0 = c0 * (1 / s[0]), c1 = c1 * (1 / s[1]), c2 = c2 * (1 / s[2]);
            double w = std::sqrt(std::max(0.0, 1 + c0.x + c1.y + c2.z)) / 2;
            if (w > 1e-6) r = {(c1.z - c2.y) / (4 * w), (c2.x - c0.z) / (4 * w), (c0.y - c1.x) / (4 * w), w};
        }
        j.offset = {t[0], t[1], t[2]};
        j.rot = Quat{r[3], r[0], r[1], r[2]}.normalized();
        j.scale = (s[0] + s[1] + s[2]) / 3;  // ponytail: uniform scale only; non-uniform armature scales are averaged
        out.joints.push_back(j);
    }

    // The first animation, sampled at 30 fps.
    const Json* anims = doc.find("animations");
    if (!anims || anims->arr.empty()) return err = "the glTF file has no animation", false;
    const Json& anim = anims->arr[0];
    const Json *channels = anim.find("channels"), *samplers = anim.find("samplers");
    if (!channels || !samplers) return err = "malformed animation", false;
    struct Chan {
        int joint, path;  // 0 translation, 1 rotation
        std::vector<float> in, val;
        int width;
        bool step, cubic;
    };
    std::vector<Chan> chans;
    double duration = 0;
    for (auto& c : channels->arr) {
        const Json* target = c.find("target");
        int node = target ? inum(target->find("node")) : -1;
        const Json* path = target ? target->find("path") : nullptr;
        if (node < 0 || node >= nn || index[node] < 0 || !path || !path->is_string()) continue;
        int p = path->str == "translation" ? 0 : path->str == "rotation" ? 1 : -1;
        if (p < 0) continue;
        int si = inum(c.find("sampler"));
        if (si < 0 || si >= int(samplers->arr.size())) continue;
        const Json& s = samplers->arr[si];
        Chan ch{index[node], p, {}, {}, 0, false, false};
        int w_in = 0;
        if (!rd.floats(inum(s.find("input")), ch.in, w_in) || !rd.floats(inum(s.find("output")), ch.val, ch.width))
            return err = rd.err, false;
        const Json* interp = s.find("interpolation");
        ch.step = interp && interp->str == "STEP";
        ch.cubic = interp && interp->str == "CUBICSPLINE";
        if (ch.in.empty() || ch.width != (p ? 4 : 3)) continue;
        duration = std::max(duration, double(ch.in.back()));
        chans.push_back(std::move(ch));
    }
    if (chans.empty()) out.notes.push_back("the animation moves none of the joints");
    const int frames = sample_count(duration, 30);
    if (double(frames) * double(out.joints.size()) > kMaxSamples)
        return err = "the animation is too large (" + std::to_string(out.joints.size()) + " joints x " + std::to_string(frames) +
                     " frames)", false;
    out.fps = 30;
    out.rot.assign(frames, {});
    out.pos.assign(frames, {});
    for (int f = 0; f < frames; ++f) {
        for (auto& j : out.joints) out.rot[f].push_back(j.rot), out.pos[f].push_back(j.offset);
        const double t = f / 30.0;
        for (const Chan& ch : chans) {
            auto value = [&](size_t k) -> const float* {  // cubic: in-tangent, value, out-tangent
                return ch.val.data() + (ch.cubic ? (k * 3 + 1) : k) * ch.width;
            };
            size_t n = ch.in.size();
            size_t k = size_t(std::upper_bound(ch.in.begin(), ch.in.end(), float(t)) - ch.in.begin());
            size_t a = k == 0 ? 0 : k - 1, b = std::min(k, n - 1);
            if ((ch.cubic ? 3 : 1) * n * ch.width > ch.val.size()) continue;
            double u = b == a || ch.step ? 0 : std::clamp((t - ch.in[a]) / (ch.in[b] - ch.in[a]), 0.0, 1.0);
            const float *va = value(a), *vb = value(b);
            if (ch.path == 1)
                out.rot[f][ch.joint] = nlerp(Quat{va[3], va[0], va[1], va[2]}, Quat{vb[3], vb[0], vb[1], vb[2]}, u);
            else
                out.pos[f][ch.joint] = Vec3{va[0], va[1], va[2]} + (Vec3{vb[0], vb[1], vb[2]} - Vec3{va[0], va[1], va[2]}) * u;
        }
    }
    return true;
}

bool read_gltf_source(const std::vector<std::uint8_t>& bytes, const std::string& dir, SourceAnim& out, std::string& err) {
    return guarded(err, [&] { return read_gltf(bytes, dir, out, err); });
}

}  // namespace vats
