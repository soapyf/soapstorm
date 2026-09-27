// Viewport Avatar Toolset - FBX import through ufbx. See vats/fbx.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Mesh conventions follow load_dae exactly (up-axis turn, measured rig scale, SK-40 binds), so
// skin_prop and the rest of the app cannot tell an FBX part from a COLLADA one.
#include "vats/fbx.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "guard.h"

#ifdef VATS_FBX
#include "ufbx.h"
#endif

namespace vats {

bool load_mesh_file(const std::string& path, const Skeleton& skel, DaeModel& out, DaeReport& report, std::string& err) {
    return guarded(err, [&] {
    std::ifstream f(path, std::ios::binary);
    if (!f) return err = "cannot read " + path, false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string dir = path.substr(0, path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\"));
    std::string ext = path.substr(path.find_last_of('.') + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "fbx") return load_fbx_mesh(bytes, dir, skel, out, report, err);
    return load_dae(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), dir, skel, out, report,
                    err);
    });
}

#ifndef VATS_FBX

bool read_fbx_source(const std::vector<std::uint8_t>&, SourceAnim&, std::string& err) {
    return err = "this build has no FBX support (VATS_FBX=OFF)", false;
}
bool load_fbx_mesh(const std::vector<std::uint8_t>&, const std::string&, const Skeleton&, DaeModel&, DaeReport&,
                   std::string& err) {
    return err = "this build has no FBX support (VATS_FBX=OFF)", false;
}

#else

namespace {

struct Scene {
    ufbx_scene* s = nullptr;
    ~Scene() { ufbx_free_scene(s); }
};

bool load(const std::vector<std::uint8_t>& bytes, Scene& scene, std::string& err) {
    ufbx_load_opts opts{};
    opts.node_depth_limit = 512;  // the readers below walk the tree recursively
    ufbx_error e;
    scene.s = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &e);
    if (scene.s) return true;
    char buf[512];
    ufbx_format_error(buf, sizeof buf, &e);
    err = std::string("not a readable FBX file: ") + buf;
    if (auto nl = err.find('\n'); nl != std::string::npos) err.resize(nl);
    return false;
}

std::string str(ufbx_string s) { return std::string(s.data, s.length); }
Vec3 vec(ufbx_vec3 v) { return {v.x, v.y, v.z}; }
Quat quat(ufbx_quat q) { return Quat{q.w, q.x, q.y, q.z}.normalized(); }

ufbx_matrix scaling(double s) {
    ufbx_matrix m{};
    m.m00 = m.m11 = m.m22 = s;
    return m;
}

// load_dae's up-axis turn: Y up (x, y, z) -> (x, -z, y); X up (x, y, z) -> (-y, -z, x).
ufbx_matrix up_turn(const ufbx_scene* s, std::string& name) {
    ufbx_matrix m = scaling(1);
    name = "Z_UP";
    switch (s->settings.axes.up) {
        case UFBX_COORDINATE_AXIS_POSITIVE_Y:
            m.m11 = 0, m.m12 = -1, m.m21 = 1, m.m22 = 0;
            name = "Y_UP";
            break;
        case UFBX_COORDINATE_AXIS_POSITIVE_X:
            m = ufbx_matrix{};
            m.m01 = -1, m.m12 = -1, m.m20 = 1;
            name = "X_UP";
            break;
        default: break;
    }
    return m;
}

void add_unique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

}  // namespace

static bool read_fbx(const std::vector<std::uint8_t>& bytes, SourceAnim& out, std::string& err) {
    out = SourceAnim();
    Scene scene;
    if (!load(bytes, scene, err)) return false;
    const ufbx_scene* s = scene.s;

    // Joints: bone nodes, the bones skins bind to, and their ancestors (not the scene root).
    std::set<const ufbx_node*> keep;
    auto keep_up = [&](const ufbx_node* n) {
        for (; n && !n->is_root && !keep.count(n); n = n->parent) keep.insert(n);
    };
    for (size_t i = 0; i < s->nodes.count; ++i)
        if (s->nodes.data[i]->bone) keep_up(s->nodes.data[i]);
    for (size_t i = 0; i < s->skin_clusters.count; ++i) keep_up(s->skin_clusters.data[i]->bone_node);
    if (keep.empty()) {
        for (size_t i = 0; i < s->nodes.count; ++i)
            if (!s->nodes.data[i]->is_root && !s->nodes.data[i]->mesh) keep.insert(s->nodes.data[i]);
        out.notes.push_back("no bones marked in the file: every non-mesh node is treated as a joint");
    }
    std::vector<const ufbx_node*> order;
    std::map<const ufbx_node*, int> index;
    std::function<void(const ufbx_node*)> visit = [&](const ufbx_node* n) {
        if (index.count(n) || !keep.count(n)) return;
        if (n->parent && keep.count(n->parent)) visit(n->parent);
        index[n] = int(order.size());
        order.push_back(n);
    };
    // Depth first from the root, children in file order, so joints come out in the file's order.
    std::function<void(const ufbx_node*)> walk = [&](const ufbx_node* n) {
        visit(n);
        for (size_t i = 0; i < n->children.count; ++i) walk(n->children.data[i]);
    };
    walk(s->root_node);
    for (const ufbx_node* n : order) {
        SourceJoint j;
        j.name = str(n->name);
        j.parent = n->parent && index.count(n->parent) ? index[n->parent] : -1;
        j.offset = vec(n->local_transform.translation);
        j.rot = quat(n->local_transform.rotation);
        ufbx_vec3 sc = n->local_transform.scale;
        j.scale = (sc.x + sc.y + sc.z) / 3;  // ponytail: uniform only, like the glTF reader
        out.joints.push_back(j);
    }
    if (out.joints.empty()) return err = "the FBX file has no skeleton", false;

    if (!s->anim_stacks.count) return err = "the FBX file has no animation", false;
    const ufbx_anim_stack* stack = s->anim_stacks.data[0];
    if (s->anim_stacks.count > 1)
        out.notes.push_back("the file has " + std::to_string(s->anim_stacks.count) + " takes; reading \"" +
                            str(stack->name) + "\"");
    const double file_fps = s->settings.frames_per_second;
    out.fps = std::isfinite(file_fps) && file_fps >= 1 && file_fps <= 1000 ? file_fps : 30;
    double begin = stack->time_begin, end = stack->time_end;
    if (end <= begin && s->anim_curves.count) {  // no take range in the file: the span of the keys
        begin = 1e300, end = -1e300;
        for (size_t i = 0; i < s->anim_curves.count; ++i)
            if (s->anim_curves.data[i]->keyframes.count)
                begin = std::min(begin, s->anim_curves.data[i]->min_time), end = std::max(end, s->anim_curves.data[i]->max_time);
        if (end < begin) begin = end = 0;
    }
    if (!std::isfinite(begin)) begin = 0;
    const int frames = sample_count(end - begin, out.fps);
    if (double(frames) * double(order.size()) > kMaxSamples)
        return err = "the animation is too large (" + std::to_string(order.size()) + " joints x " + std::to_string(frames) +
                     " frames)", false;
    out.rot.assign(frames, {});
    out.pos.assign(frames, {});
    for (int f = 0; f < frames; ++f) {
        double t = begin + f / out.fps;
        for (const ufbx_node* n : order) {
            ufbx_transform x = ufbx_evaluate_transform(stack->anim, n, t);
            out.rot[f].push_back(quat(x.rotation));
            out.pos[f].push_back(vec(x.translation));
        }
    }
    return true;
}

static bool load_fbx(const std::vector<std::uint8_t>& bytes, const std::string& dir, const Skeleton& skel, DaeModel& model,
                   DaeReport& rep, std::string& err) {
    model = DaeModel();
    rep = DaeReport();
    Scene scene;
    if (!load(bytes, scene, err)) return false;
    const ufbx_scene* s = scene.s;
    const ufbx_matrix up = up_turn(s, rep.up_axis);
    const int root = dae_root(skel), count = dae_index_count(skel);
    const double unit = s->settings.unit_meters > 0 ? s->settings.unit_meters : 0.01;

    // Clusters that bind to SL joints make the model rigged (IO-37).
    std::map<const ufbx_skin_cluster*, int> target;
    for (size_t i = 0; i < s->skin_clusters.count; ++i) {
        const ufbx_skin_cluster* c = s->skin_clusters.data[i];
        std::string name = c->bone_node ? str(c->bone_node->name) : str(c->name);
        int n = map_skin_joint(skel, name);
        if (n < 0) add_unique(rep.unmapped_joints, name);
        target[c] = n;
        model.rigged |= n >= 0 && n != root;
    }
    rep.rigged = model.rigged;
    rep.skins_as_static = s->skin_clusters.count && !model.rigged;

    double rig_scale = unit;
    if (model.rigged) {
        std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
        rest.push_back({});  // mRoot
        for (auto& v : skel.volumes()) rest.push_back(rest[v.joint] * Xform{v.rot, v.pos});
        // Rig scale as in load_dae: median rest / bind distance over bones > 0.3 m out, snapped.
        std::vector<double> ratios;
        for (auto& [c, n] : target) {
            if (n < 0 || n == root) continue;
            ufbx_matrix b = ufbx_matrix_mul(&up, &c->bind_to_world);
            double r = rest[n].pos.length(), d = vec(b.cols[3]).length();
            if (r > 0.3 && d > 1e-12 && std::isfinite(d)) ratios.push_back(r / d);
        }
        if (!ratios.empty()) {
            std::sort(ratios.begin(), ratios.end());
            size_t h = ratios.size() / 2;
            double m = ratios.size() % 2 ? ratios[h] : (ratios[h - 1] + ratios[h]) / 2;
            rep.measured_scale = rig_scale = m;
            for (double u : {1.0, 0.01, 0.001, 0.0254, 0.1, 10.0, 100.0})
                if (std::fabs(std::log(m / u)) < 0.05) {
                    rig_scale = u;
                    break;
                }
        }
        model.binds = rest;
        std::vector<bool> bound(count, false);
        for (auto& [c, n] : target) {
            if (n < 0 || n == root || bound[n]) continue;  // ponytail: first bind of a joint wins, as in load_dae
            ufbx_matrix b = ufbx_matrix_mul(&up, &c->bind_to_world);
            model.binds[n] = {quat(ufbx_matrix_to_transform(&b).rotation), vec(b.cols[3]) * rig_scale};
            bound[n] = true;
        }
    }
    rep.scale = rig_scale;

    // Materials: one DaeMaterial per ufbx material, plus the default one for faces without any.
    std::map<const ufbx_material*, int> mat_index;
    auto material = [&](const ufbx_material* m) {
        auto it = mat_index.find(m);
        if (it != mat_index.end()) return it->second;
        DaeMaterial d;
        if (m) {
            d.name = str(m->name);
            const ufbx_material_map& c = m->fbx.diffuse_color;
            if (c.has_value) d.rgba = {float(c.value_vec4.x), float(c.value_vec4.y), float(c.value_vec4.z), 1.f};
            // ponytail: transparency is ignored; exporters disagree on what TransparencyFactor means.
            if (c.texture) {
                std::error_code ec;
                for (std::string p : {str(c.texture->absolute_filename), dir + "/" + str(c.texture->relative_filename)})
                    if (!p.empty() && std::filesystem::exists(p, ec)) {
                        d.texture = p;
                        break;
                    }
                if (d.texture.empty()) add_unique(rep.missing_textures, str(c.texture->filename));
                else d.rgba = {1, 1, 1, d.rgba[3]};
            }
        } else {
            d.double_sided = true;
        }
        model.materials.push_back(d);
        return mat_index[m] = int(model.materials.size()) - 1;
    };

    struct Build {
        std::vector<float> pos, nrm, uv, weights;
        std::vector<int> joints;
        std::vector<std::uint32_t> idx;
        std::vector<char> smooth;  // the file has no normal for this vertex: smoothed from its faces below
        // A corner is shared when it has the same position, UV and normal (by value: generated normals get
        // one index per corner).
        std::map<std::tuple<const void*, std::uint32_t, std::uint32_t, double, double, double>, std::uint32_t> seen;
    };
    std::map<int, Build> builds;
    std::vector<std::uint32_t> tri;
    for (size_t ni = 0; ni < s->nodes.count; ++ni) {
        const ufbx_node* node = s->nodes.data[ni];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh) continue;
        const ufbx_skin_deformer* skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
        const ufbx_skin_cluster* first = nullptr;
        for (size_t i = 0; skin && i < skin->clusters.count && !first; ++i)
            if (target[skin->clusters.data[i]] >= 0) first = skin->clusters.data[i];
        const bool rigged = model.rigged && first;
        // Geometry to world: at the bind pose for skinned parts, else where the node sits.
        ufbx_matrix g2w = rigged ? ufbx_matrix_mul(&first->bind_to_world, &first->geometry_to_bone) : node->geometry_to_world;
        ufbx_matrix su = scaling(rigged ? rig_scale : unit), t = ufbx_matrix_mul(&up, &g2w);
        ufbx_matrix x = ufbx_matrix_mul(&su, &t), nx = ufbx_matrix_for_normals(&x);
        const bool flip = ufbx_matrix_determinant(&x) < 0;
        if (model.rigged && !rigged) add_unique(rep.warnings, "mesh " + str(node->name) + " is not skinned: it stays still");

        tri.resize(mesh->max_face_triangles * 3);
        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            ufbx_face face = mesh->faces.data[fi];
            if (face.num_indices < 3) continue;
            std::uint32_t mi = mesh->face_material.count ? mesh->face_material.data[fi] : 0;
            const ufbx_material* m = mi < mesh->materials.count ? mesh->materials.data[mi] : nullptr;
            Build& b = builds[material(m)];
            std::uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (std::uint32_t k = 0; k < nt * 3; ++k) {
                std::uint32_t c = tri[flip ? k / 3 * 3 + 2 - k % 3 : k];
                std::uint32_t vi = mesh->vertex_indices.data[c];
                std::uint32_t pi = mesh->vertex_position.indices.data[c];
                std::uint32_t nn = mesh->vertex_normal.exists ? mesh->vertex_normal.indices.data[c] : 0;
                std::uint32_t ui = mesh->vertex_uv.exists ? mesh->vertex_uv.indices.data[c] : 0;
                ufbx_vec3 nv = mesh->vertex_normal.exists ? mesh->vertex_normal.values.data[nn] : ufbx_vec3{};
                auto key = std::make_tuple(static_cast<const void*>(mesh), pi, ui, nv.x, nv.y, nv.z);
                auto [it, fresh] = b.seen.try_emplace(key, std::uint32_t(b.pos.size() / 3));
                b.idx.push_back(it->second);
                if (!fresh) continue;
                Vec3 p = vec(ufbx_transform_position(&x, mesh->vertex_position.values.data[pi]));
                Vec3 n = mesh->vertex_normal.exists ? vec(ufbx_transform_direction(&nx, nv)).normalized() : Vec3{};
                b.smooth.push_back(!mesh->vertex_normal.exists);
                for (int i = 0; i < 3; ++i) b.pos.push_back(float(p[i])), b.nrm.push_back(float(n[i]));
                ufbx_vec2 uv = mesh->vertex_uv.exists ? mesh->vertex_uv.values.data[ui] : ufbx_vec2{};
                b.uv.push_back(float(uv.x)), b.uv.push_back(float(1 - uv.y));
                // Weights: summed per SL joint, four largest renormalised; nothing -> 100 % root.
                int j4[4] = {root, root, root, root};
                float w4[4] = {1, 0, 0, 0};
                if (rigged && vi < skin->vertices.count) {
                    std::vector<std::pair<int, double>> sum;
                    const ufbx_skin_vertex& sv = skin->vertices.data[vi];
                    for (std::uint32_t w = 0; w < sv.num_weights; ++w) {
                        const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + w];
                        int node_i = sw.cluster_index < skin->clusters.count ? target[skin->clusters.data[sw.cluster_index]] : -1;
                        if (node_i < 0 || !(sw.weight > 0)) continue;
                        auto at = std::find_if(sum.begin(), sum.end(), [&](auto& e) { return e.first == node_i; });
                        if (at != sum.end()) at->second += sw.weight;
                        else sum.emplace_back(node_i, sw.weight);
                    }
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
                if (model.rigged) b.joints.insert(b.joints.end(), j4, j4 + 4), b.weights.insert(b.weights.end(), w4, w4 + 4);
            }
        }
    }

    for (auto& [mat, b] : builds) {
        if (b.idx.empty()) continue;
        // Area-weighted smooth normals where the file gave none (corners of one position share a vertex).
        auto at = [&](std::uint32_t v) { return Vec3{b.pos[v * 3], b.pos[v * 3 + 1], b.pos[v * 3 + 2]}; };
        std::vector<Vec3> acc(b.smooth.size());
        for (size_t t = 0; t + 2 < b.idx.size(); t += 3) {
            Vec3 fn = (at(b.idx[t + 1]) - at(b.idx[t])).cross(at(b.idx[t + 2]) - at(b.idx[t]));
            for (int k = 0; k < 3; ++k) acc[b.idx[t + k]] += fn;
        }
        for (size_t v = 0; v < b.smooth.size(); ++v)
            if (b.smooth[v]) {
                Vec3 n = acc[v].length() > 1e-20 ? acc[v].normalized() : Vec3{0, 0, 1};
                for (int i = 0; i < 3; ++i) b.nrm[v * 3 + i] = float(n[i]);
            }
        DaeGroup g{mat, std::uint32_t(model.vertex_count()), std::uint32_t(b.pos.size() / 3),
                   std::uint32_t(model.indices.size()), std::uint32_t(b.idx.size())};
        for (std::uint32_t i : b.idx) model.indices.push_back(g.first_vertex + i);
        model.positions.insert(model.positions.end(), b.pos.begin(), b.pos.end());
        model.normals.insert(model.normals.end(), b.nrm.begin(), b.nrm.end());
        model.uvs.insert(model.uvs.end(), b.uv.begin(), b.uv.end());
        model.joints.insert(model.joints.end(), b.joints.begin(), b.joints.end());
        model.weights.insert(model.weights.end(), b.weights.begin(), b.weights.end());
        model.groups.push_back(g);
    }
    rep.triangles = model.triangle_count();
    if (!rep.triangles) return err = "no triangles in the FBX file", false;
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

bool read_fbx_source(const std::vector<std::uint8_t>& bytes, SourceAnim& out, std::string& err) {
    return guarded(err, [&] { return read_fbx(bytes, out, err); });
}

bool load_fbx_mesh(const std::vector<std::uint8_t>& bytes, const std::string& dir, const Skeleton& skel, DaeModel& model,
                   DaeReport& rep, std::string& err) {
    return guarded(err, [&] { return load_fbx(bytes, dir, skel, model, rep, err); });
}

#endif

}  // namespace vats
