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

#include "gltf_common.h"
#include "guard.h"
#include "vats/retarget.h"

namespace vats {
namespace {

using gltf::inum;
using gltf::num;

}  // namespace

static bool read_gltf(const std::vector<std::uint8_t>& bytes, const std::string& dir, SourceAnim& out, std::string& err) {
    out = SourceAnim{};
    Json doc;
    std::vector<std::vector<std::uint8_t>> buffers;
    if (!gltf::open(bytes, dir, doc, buffers, err)) return false;
    gltf::Reader rd{doc, std::move(buffers), {}};
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
