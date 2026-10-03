// Viewport Avatar Toolset - glTF 2.0 / GLB rigged mesh import. See vats/gltf_mesh.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/gltf_mesh.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "affine.h"
#include "gltf_common.h"
#include "guard.h"
#include "mesh_parts.h"
#include "source_bones.h"

namespace vats {
namespace {

using gltf::inum;
using gltf::num;

void add_unique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

std::vector<double> nums(const Json* a, int n, std::vector<double> d) {
    if (a && a->is_array() && int(a->arr.size()) == n)
        for (int i = 0; i < n; ++i) d[i] = num(&a->arr[i]);
    return d;
}

// A node's local transform: TRS, or its matrix.
Affine local_of(const Json& n) {
    if (const Json* m = n.find("matrix"); m && m->is_array() && m->arr.size() == 16) {
        float f[16];
        for (int i = 0; i < 16; ++i) f[i] = float(num(&m->arr[i]));
        return Affine::from_columns(f);
    }
    auto t = nums(n.find("translation"), 3, {0, 0, 0});
    auto r = nums(n.find("rotation"), 4, {0, 0, 0, 1});
    auto s = nums(n.find("scale"), 3, {1, 1, 1});
    return Affine::from_xform({Quat{r[3], r[0], r[1], r[2]}.normalized(), {t[0], t[1], t[2]}}) * Affine::scaling({s[0], s[1], s[2]});
}

struct Build {
    std::vector<float> pos, nrm, uv, weights;
    std::vector<int> joints;
    std::vector<std::uint32_t> idx;
    std::vector<char> smooth;
    std::map<std::string, KeyBuild> keys;
};

bool load(const std::vector<std::uint8_t>& bytes, const std::string& dir, const Skeleton& skel, DaeModel& model, DaeReport& rep,
          std::string& err, const SkinRemap* remap) {
    model = DaeModel();
    rep = DaeReport();
    rep.up_axis = "Y_UP";
    Json doc;
    std::vector<std::vector<std::uint8_t>> buffers;
    if (!gltf::open(bytes, dir, doc, buffers, err)) return false;
    gltf::Reader rd{doc, std::move(buffers), {}};
    const Json* nodes = doc.find("nodes");
    if (!nodes || !nodes->is_array() || nodes->arr.empty()) return err = "the glTF file has no nodes", false;
    const int nn = int(nodes->arr.size()), root = dae_root(skel), count = dae_index_count(skel);

    // Node tree: parents, names, globals (parents before children, without recursion).
    std::vector<int> parent(nn, -1);
    std::vector<std::string> names(nn);
    for (int i = 0; i < nn; ++i) {
        const Json& n = nodes->arr[i];
        const Json* nm = n.find("name");
        names[i] = nm && nm->is_string() ? nm->str : "node" + std::to_string(i);
        if (const Json* ch = n.find("children"))
            for (auto& c : ch->arr)
                if (int k = inum(&c); k >= 0 && k < nn && k != i) parent[k] = i;
    }
    std::vector<Affine> global(nn);
    std::vector<char> done(nn, 0);
    for (int i = 0; i < nn; ++i) {
        std::vector<int> chain;
        for (int k = i; k >= 0 && !done[k]; k = parent[k]) {
            if (int(chain.size()) > nn) return err = "the glTF node tree has a cycle", false;
            chain.push_back(k);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const Affine l = local_of(nodes->arr[*it]);
            global[*it] = parent[*it] >= 0 ? global[parent[*it]] * l : l;
            done[*it] = 1;
        }
    }

    // Skins: each joint's SL node and bind (the inverse of its inverse bind matrix; the identity without them).
    struct SkinInfo {
        std::vector<int> node;       // SK-40 index per joint, mRoot when unmapped or dropped by a remap
        std::vector<int> joint;      // the node index per joint, -1 when out of range
        std::vector<Affine> bind;    // scene space, glTF axes
        std::vector<bool> has_bind;
        // Skin space (where the vertices and the inverse bind matrices are) to scene space. Nodes above the armature
        // (a "Z_UP" node turning a Z-up export into glTF's Y up) sit between the two: the skin's root joint, posed in
        // the scene at its bind, says how.
        Affine to_scene;
        bool maps = false;
    };
    std::vector<SkinInfo> skins;
    if (const Json* sk = doc.find("skins"); sk && sk->is_array())
        for (const Json& s : sk->arr) {
            SkinInfo info;
            std::vector<float> ibm;
            int w = 0;
            const bool have_ibm = s.find("inverseBindMatrices") && rd.floats(inum(s.find("inverseBindMatrices")), ibm, w) && w == 16;
            if (const Json* joints = s.find("joints"); joints && joints->is_array())
                for (size_t j = 0; j < joints->arr.size(); ++j) {
                    const int ni = inum(&joints->arr[j]);
                    int mapped = -1;
                    if (remap && ni >= 0 && ni < nn) {  // the mapping decides; a bone it leaves out is dropped
                        if (auto it = remap->joints.find(names[ni]); it != remap->joints.end()) mapped = it->second;
                    } else if (!remap) {
                        bool loose = false;
                        mapped = ni >= 0 && ni < nn ? map_skin_joint(skel, names[ni], &loose) : -1;
                        if (loose) add_unique(rep.warnings, loose_joint_warning(names[ni]));
                        if (mapped < 0) add_unique(rep.unmapped_joints, ni >= 0 && ni < nn ? names[ni] : "joint " + std::to_string(j));
                    }
                    info.joint.push_back(ni >= 0 && ni < nn ? ni : -1);
                    info.node.push_back(mapped < 0 ? root : mapped);
                    info.maps |= mapped >= 0 && mapped != root;
                    const bool has = have_ibm && (j + 1) * 16 <= ibm.size();
                    info.has_bind.push_back(has);
                    info.bind.push_back(has ? Affine::from_columns(ibm.data() + j * 16).inverse() : Affine{});
                }
            for (size_t j = 0; j < info.joint.size(); ++j) {  // the first joint whose parent is no joint of this skin
                const int ni = info.joint[j];
                if (ni < 0 || !info.has_bind[j] || (parent[ni] >= 0 && std::count(info.joint.begin(), info.joint.end(), parent[ni]))) continue;
                // ponytail: assumes the file's scene pose is its bind pose at the root joint (true of exporters' rest
                // files); a file saved mid-animation would carry that root motion into the bind.
                info.to_scene = global[ni] * info.bind[j].inverse();
                for (Affine& b : info.bind) b = info.to_scene * b;
                break;
            }
            if (!have_ibm) add_unique(rep.warnings, "a skin has no inverse bind matrices: its joints are taken as bound at the origin");
            skins.push_back(std::move(info));
        }
    for (const SkinInfo& s : skins) model.rigged |= s.maps;
    rep.rigged = model.rigged;
    rep.skins_as_static = !skins.empty() && !model.rigged;
    rep.remapped = remap != nullptr;

    // The armature (SourceBone): every skin joint and its ancestors, bound where its skin says, else where the node
    // tree puts it against its parent. Their weights are summed as the meshes are read.
    Affine up = Affine::y_up();
    std::vector<int> bone_of(nn, -1);
    {
        std::vector<char> keep(nn, 0);
        for (const SkinInfo& s : skins)
            for (int ni : s.joint)
                for (int k = ni; k >= 0 && !keep[k]; k = parent[k]) keep[k] = 1;  // the tree has no cycle (checked above)
        std::vector<Xform> scene;
        std::vector<char> placed(nn, 0);
        for (int i = 0; i < nn; ++i) {  // parents first, without recursion
            std::vector<int> chain;
            for (int k = i; k >= 0 && keep[k] && !placed[k]; k = parent[k]) chain.push_back(k);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                placed[*it] = 1;
                bone_of[*it] = int(rep.bones.size());
                rep.bones.push_back({names[*it], parent[*it] >= 0 ? bone_of[parent[*it]] : -1, {}, false, 0});
                const Affine g = up * global[*it];
                scene.push_back({g.rotation(), g.origin()});
            }
        }
        for (const SkinInfo& s : skins)
            for (size_t j = 0; j < s.joint.size(); ++j)
                if (SourceBone* b = s.joint[j] >= 0 ? &rep.bones[bone_of[s.joint[j]]] : nullptr; b && s.has_bind[j] && !b->skinned) {
                    const Affine m = up * s.bind[j];
                    b->bind = {m.rotation(), m.origin()};
                    b->skinned = true;
                }
        place_unskinned_bones(rep.bones, scene);
    }

    // Rig scale and binds, as load_dae measures them: SL rest distance over bind distance for bones > 0.3 m out.
    double rig_scale = 1;
    std::vector<bool> bound;
    if (model.rigged && remap) {
        rig_scale = 1;
        remap_binds(skel, *remap, model, bound);
        up = Affine::from_xform({Quat::axis_angle({0, 0, 1}, remap->turn * kPi / 2), {}}) * up;  // the vertices' turn
    } else if (model.rigged) {
        std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
        rest.push_back({});
        for (auto& v : skel.volumes()) rest.push_back(rest[v.joint] * Xform{v.rot, v.pos});
        std::vector<double> ratios;
        for (const SkinInfo& s : skins)
            for (size_t j = 0; j < s.node.size(); ++j) {
                if (!s.has_bind[j] || s.node[j] == root) continue;
                const double r = rest[s.node[j]].pos.length(), b = (up * s.bind[j]).origin().length();
                if (r > 0.3 && b > 1e-12 && std::isfinite(b)) ratios.push_back(r / b);
            }
        if (!ratios.empty()) rig_scale = rig_scale_of(ratios, rep.measured_scale, 1);
        model.binds = rest;
        bound.assign(count, false);
        for (const SkinInfo& s : skins)
            for (size_t j = 0; j < s.node.size(); ++j) {
                const int n = s.node[j];
                if (!s.has_bind[j] || n == root || bound[n]) continue;  // first bind of a joint wins, as in load_dae
                const Affine b = up * s.bind[j];
                model.binds[n] = {b.rotation(), b.origin() * rig_scale};
                bound[n] = true;
            }
    }
    rep.scale = rig_scale;

    // Materials: base colour, its texture (a file beside the .gltf), alpha blending, double sided.
    std::map<int, int> mat_index;
    auto material = [&](int mi) {
        auto it = mat_index.find(mi);
        if (it != mat_index.end()) return it->second;
        DaeMaterial d;
        const Json* mats = doc.find("materials");
        const Json* m = mats && mi >= 0 && mi < int(mats->arr.size()) ? &mats->arr[mi] : nullptr;
        if (m) {
            if (const Json* nm = m->find("name"); nm && nm->is_string()) d.name = nm->str;
            if (d.name.empty()) d.name = "material" + std::to_string(mi);
            if (const Json* pbr = m->find("pbrMetallicRoughness")) {
                auto c = nums(pbr->find("baseColorFactor"), 4, {1, 1, 1, 1});
                d.rgba = {float(c[0]), float(c[1]), float(c[2]), float(c[3])};
                if (const Json* bt = pbr->find("baseColorTexture")) {
                    const Json *textures = doc.find("textures"), *images = doc.find("images");
                    const int ti = inum(bt->find("index"));
                    const Json* tex = textures && ti >= 0 && ti < int(textures->arr.size()) ? &textures->arr[ti] : nullptr;
                    const int si = tex ? inum(tex->find("source")) : -1;
                    const Json* img = images && si >= 0 && si < int(images->arr.size()) ? &images->arr[si] : nullptr;
                    const Json* uri = img ? img->find("uri") : nullptr;
                    std::string raw = uri && uri->is_string() ? uri->str : "";
                    std::error_code ec;
                    const std::filesystem::path file = std::filesystem::path(dir) / raw;
                    if (!raw.empty() && raw.rfind("data:", 0) != 0 && std::filesystem::is_regular_file(file, ec)) {
                        d.texture = file.lexically_normal().string();
                        d.rgba = {1, 1, 1, d.rgba[3]};
                    } else {
                        add_unique(rep.missing_textures, raw.empty() ? "an embedded texture" : raw);
                    }
                }
            }
            if (const Json* am = m->find("alphaMode"); am && am->is_string() && am->str == "BLEND") d.blend = true;
            if (const Json* ds = m->find("doubleSided"); ds && ds->is_bool()) d.double_sided = ds->b;
            d.blend = d.blend || d.rgba[3] < 0.999f;
        } else {
            d.double_sided = true;
        }
        model.materials.push_back(d);
        return mat_index[mi] = int(model.materials.size()) - 1;
    };

    // Meshes: every primitive of every mesh node, triangles only. Each mesh node is a part (SK-1), its builds one per
    // material.
    std::map<std::pair<int, int>, Build> builds;
    double key_budget = kMaxSamples * 32;  // floats every morph target may read together (1 GB, each read dropped after)
    const Json* meshes = doc.find("meshes");
    for (int ni = 0; ni < nn; ++ni) {
        const Json& n = nodes->arr[ni];
        const int mi = inum(n.find("mesh"));
        if (!meshes || mi < 0 || mi >= int(meshes->arr.size())) continue;
        const int si = inum(n.find("skin"));
        const SkinInfo* skin = si >= 0 && si < int(skins.size()) ? &skins[si] : nullptr;
        const bool rigged = model.rigged && skin && skin->maps;
        if (model.rigged && !rigged) add_unique(rep.warnings, "mesh " + names[ni] + " is not skinned: it stays still");
        // A skinned mesh is in its skin's (scene) space, whatever its node says; a static one sits where its node is.
        const Affine x = rigged ? Affine::scaling({rig_scale, rig_scale, rig_scale}) * up * skin->to_scene : up * global[ni];
        const Affine nx = x.normal_matrix();
        const bool flip = x.det() < 0;
        const Json* prims = meshes->arr[mi].find("primitives");
        if (!prims || !prims->is_array()) continue;
        // SK-2: shape key names from the exporters' extras.targetNames (Blender's); values from the node's weights, else
        // the mesh's.
        const Json* extras = meshes->arr[mi].find("extras");
        const Json* key_names = extras ? extras->find("targetNames") : nullptr;
        const Json* key_values = n.find("weights") ? n.find("weights") : meshes->arr[mi].find("weights");
        for (const Json& prim : prims->arr) {
            const int mode = inum(prim.find("mode"), 4);
            if (mode != 4) {
                add_unique(rep.unsupported, mode == 5 ? "triangle strips" : mode == 6 ? "triangle fans" : "points or lines");
                continue;
            }
            const Json* attrs = prim.find("attributes");
            if (!attrs) continue;
            std::vector<float> pos, nrm, uv, j0, w0, j1, w1, idxf;
            int w = 0;
            if (!attrs->find("POSITION") || !rd.floats(inum(attrs->find("POSITION")), pos, w) || w != 3) {
                add_unique(rep.warnings, "a primitive without usable positions was skipped" + (rd.err.empty() ? "" : " (" + rd.err + ")"));
                rd.err.clear();
                continue;
            }
            const size_t nv = pos.size() / 3;
            if (attrs->find("NORMAL") && !(rd.floats(inum(attrs->find("NORMAL")), nrm, w) && w == 3 && nrm.size() == nv * 3)) nrm.clear();
            if (attrs->find("TEXCOORD_0") && !(rd.floats(inum(attrs->find("TEXCOORD_0")), uv, w) && w == 2 && uv.size() == nv * 2)) uv.clear();
            if (skin) {
                if (attrs->find("JOINTS_0") && !(rd.floats(inum(attrs->find("JOINTS_0")), j0, w) && w == 4 && j0.size() == nv * 4)) j0.clear();
                if (attrs->find("WEIGHTS_0") && !(rd.floats(inum(attrs->find("WEIGHTS_0")), w0, w) && w == 4 && w0.size() == nv * 4)) w0.clear();
                if (attrs->find("JOINTS_1") && !(rd.floats(inum(attrs->find("JOINTS_1")), j1, w) && w == 4 && j1.size() == nv * 4)) j1.clear();
                if (attrs->find("WEIGHTS_1") && !(rd.floats(inum(attrs->find("WEIGHTS_1")), w1, w) && w == 4 && w1.size() == nv * 4)) w1.clear();
                if (rigged && (j0.empty() || w0.empty()))
                    add_unique(rep.warnings, "mesh " + names[ni] + " has no skin weights: it follows nothing");
                auto sum = [&](const std::vector<float>& js, const std::vector<float>& ws) {  // the bones' weights
                    for (size_t k = 0; k < js.size() && k < ws.size(); ++k)
                        if (ws[k] > 0 && js[k] >= 0 && js[k] < float(skin->joint.size()))
                            if (const int bn = skin->joint[size_t(js[k])]; bn >= 0 && bone_of[bn] >= 0) rep.bones[bone_of[bn]].weight += ws[k];
                };
                sum(j0, w0);
                sum(j1, w1);
            }
            rd.err.clear();
            std::vector<std::uint32_t> idx;
            if (prim.find("indices")) {
                if (!rd.floats(inum(prim.find("indices")), idxf, w) || w != 1) {
                    add_unique(rep.warnings, "a primitive with unreadable indices was skipped");
                    rd.err.clear();
                    continue;
                }
                idx.reserve(idxf.size());
                for (float f : idxf) idx.push_back(f >= 0 && f < float(nv) ? std::uint32_t(f) : std::numeric_limits<std::uint32_t>::max());
            } else {
                for (size_t i = 0; i < nv; ++i) idx.push_back(std::uint32_t(i));
            }
            Build& b = builds[{ni, material(inum(prim.find("material")))}];
            const std::uint32_t base = std::uint32_t(b.pos.size() / 3);
            for (size_t v = 0; v < nv; ++v) {
                const Vec3 p = x.point({pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]});
                const Vec3 nv3 = nrm.empty() ? Vec3{} : nx.dir({nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]}).normalized();
                for (int i = 0; i < 3; ++i) b.pos.push_back(float(p[i])), b.nrm.push_back(float(nv3[i]));
                b.smooth.push_back(nrm.empty());
                b.uv.push_back(uv.empty() ? 0.f : uv[v * 2]);
                b.uv.push_back(uv.empty() ? 0.f : 1 - uv[v * 2 + 1]);
                if (!model.rigged) continue;
                // Weights: summed per SL joint, the four largest renormalised; nothing -> 100 % root.
                int j4[4] = {root, root, root, root};
                float w4[4] = {1, 0, 0, 0};
                if (rigged && !j0.empty() && !w0.empty()) {
                    std::vector<std::pair<int, double>> sum;
                    auto take = [&](const std::vector<float>& js, const std::vector<float>& ws) {
                        if (js.empty() || ws.empty()) return;
                        for (int k = 0; k < 4; ++k) {
                            const float jf = js[v * 4 + k], wf = ws[v * 4 + k];
                            const int ji = jf >= 0 && jf < float(skin->node.size()) ? int(jf) : -1;
                            if (ji < 0 || !(wf > 0) || skin->node[ji] == root) continue;
                            const int node = skin->node[ji];
                            auto at = std::find_if(sum.begin(), sum.end(), [&](auto& e) { return e.first == node; });
                            if (at != sum.end()) at->second += wf;
                            else sum.emplace_back(node, wf);
                        }
                    };
                    take(j0, w0);
                    take(j1, w1);
                    std::stable_sort(sum.begin(), sum.end(), [](auto& a, auto& c) { return a.second > c.second; });
                    if (sum.size() > 4) sum.resize(4);
                    double total = 0;
                    for (auto& e : sum) total += e.second;
                    if (total > 0 && std::isfinite(total))
                        for (size_t i = 0; i < 4; ++i) {
                            j4[i] = i < sum.size() ? sum[i].first : root;
                            w4[i] = i < sum.size() ? float(sum[i].second / total) : 0.f;
                        }
                }
                b.joints.insert(b.joints.end(), j4, j4 + 4);
                b.weights.insert(b.weights.end(), w4, w4 + 4);
            }
            // SK-2: the primitive's morph targets, one at a time, kept where they move a vertex. A body's dozens of keys
            // are read whole and dropped, so they draw on a budget of their own and never starve the meshes'.
            if (const Json* ts = prim.find("targets"); ts && ts->is_array())
                for (size_t k = 0; k < ts->arr.size(); ++k) {
                    const Json* nm = key_names && key_names->is_array() && k < key_names->arr.size() ? &key_names->arr[k] : nullptr;
                    const std::string name = nm && nm->is_string() ? nm->str : "Key " + std::to_string(k + 1);
                    const Json& tj = ts->arr[k];
                    std::vector<float> dp, dn;
                    std::swap(rd.budget, key_budget);
                    if (tj.find("POSITION") && !(rd.floats(inum(tj.find("POSITION")), dp, w) && w == 3 && dp.size() == nv * 3)) dp.clear();
                    if (tj.find("NORMAL") && !nrm.empty() && !(rd.floats(inum(tj.find("NORMAL")), dn, w) && w == 3 && dn.size() == nv * 3)) dn.clear();
                    std::swap(rd.budget, key_budget);
                    if (!rd.err.empty()) add_unique(rep.warnings, "shape key " + name + " of " + names[ni] + " could not be read (" + rd.err + ")");
                    rd.err.clear();
                    if (dp.empty() && dn.empty()) continue;
                    KeyBuild& kb = b.keys[name];
                    kb.initial = key_values && key_values->is_array() && k < key_values->arr.size() ? num(&key_values->arr[k]) : 0;
                    for (size_t v = 0; v < nv; ++v) {
                        const Vec3 d = dp.empty() ? Vec3{} : x.dir({dp[v * 3], dp[v * 3 + 1], dp[v * 3 + 2]});
                        if (dn.empty()) {
                            kb.add(base + std::uint32_t(v), d, nullptr);
                            continue;
                        }
                        // In the unit normal's scale: glTF adds the offset to the normal before normalising (shown_model).
                        const double len = nx.dir({nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]}).length();
                        const Vec3 ndir = nx.dir({dn[v * 3], dn[v * 3 + 1], dn[v * 3 + 2]}) * (len > 1e-12 ? 1 / len : 0.0);
                        kb.add(base + std::uint32_t(v), d, &ndir);
                    }
                }
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                std::uint32_t k[3] = {idx[t], idx[t + 1], idx[t + 2]};
                if (k[0] >= nv || k[1] >= nv || k[2] >= nv) continue;
                if (flip) std::swap(k[1], k[2]);
                for (std::uint32_t i : k) b.idx.push_back(base + i);
            }
        }
    }

    int open = -1;
    for (auto& [key, b] : builds) {
        const auto [ni, mat] = key;
        if (b.idx.empty()) continue;
        auto at = [&](std::uint32_t v) { return Vec3{b.pos[v * 3], b.pos[v * 3 + 1], b.pos[v * 3 + 2]}; };
        std::vector<Vec3> acc(b.smooth.size());
        for (size_t t = 0; t + 2 < b.idx.size(); t += 3) {
            const Vec3 fn = (at(b.idx[t + 1]) - at(b.idx[t])).cross(at(b.idx[t + 2]) - at(b.idx[t]));
            for (int k = 0; k < 3; ++k) acc[b.idx[t + k]] += fn;
        }
        for (size_t v = 0; v < b.smooth.size(); ++v)
            if (b.smooth[v]) {
                const Vec3 n = acc[v].length() > 1e-20 ? acc[v].normalized() : Vec3{0, 0, 1};
                for (int i = 0; i < 3; ++i) b.nrm[v * 3 + i] = float(n[i]);
            }
        if (std::exchange(open, ni) != ni) begin_part(model, names[ni]);
        append_group(model, mat, b, b.keys);
    }
    settle_rig(model, skel, bound, rep, remap != nullptr);
    rep.triangles = model.triangle_count();
    if (!rep.triangles) return err = "no triangles in the glTF file", false;
    const double inf = std::numeric_limits<double>::infinity();
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

bool load_gltf_mesh(const std::vector<std::uint8_t>& bytes, const std::string& dir, const Skeleton& skel, DaeModel& out,
                    DaeReport& report, std::string& err, const SkinRemap* remap) {
    return guarded(err, [&] { return load(bytes, dir, skel, out, report, err, remap); });
}

}  // namespace vats
