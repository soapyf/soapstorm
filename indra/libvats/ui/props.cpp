// Viewport Avatar Toolset - props: imported COLLADA meshes, static or rigged.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/04 sections 2.7 and 2.8, docs/spec/03 sections 2.5, 3.3 and 3.6, docs/spec/06 section 4.2.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

#include "app.h"
#include "imgui_internal.h"  // BeginDragDropTargetCustom
#include "dock_layout.h"
#include "vats/fbx.h"
#include "vats/gif.h"
#include "vats/pose_presets.h"

namespace vats {
namespace {

std::string sl_vector(const Vec3& v, int decimals) {
    auto clean = [&](double x) { return std::fabs(x) < 0.5 * std::pow(10.0, -decimals) ? 0.0 : x; };  // no "-0.000"
    char b[128];
    std::snprintf(b, sizeof b, "<%.*f, %.*f, %.*f>", decimals, clean(v.x), decimals, clean(v.y), decimals, clean(v.z));
    return b;
}

}  // namespace

// IO-42: names of props whose mesh file is missing, for the warning after a project opens.
std::vector<std::string> App::missing_prop_meshes() {
    std::vector<std::string> out;
    for (const Prop& p : doc_.clip().props)
        if (!prop_model(p.path)) out.push_back(p.name + " (" + p.path + ")");
    return out;
}

const DaeModel* App::prop_model(const std::string& path) {
    auto it = prop_models_.find(path);
    if (it != prop_models_.end()) return it->second.get();
    auto model = std::make_unique<DaeModel>();
    DaeReport report;
    std::string err;
    bool ok = false;
    try {
        ok = load_mesh_file(path, skel_, *model, report, err);
    } catch (const std::exception&) {  // a broken file is treated like a missing one (the box placeholder)
    }
    if (!ok) model.reset();  // missing: a placeholder later
    return (prop_models_[path] = std::move(model)).get();
}

// World transform of a static prop's mesh (without scale): parent x (rotation, position), vats::prop_frame.
Xform App::prop_frame(const Prop& p, const std::vector<Xform>* globals, const Xform& world) const {
    return vats::prop_frame(p, skel_, globals ? *globals : globals_, world);
}

namespace {

// A rigged mesh that binds the pelvis, 20 or more body bones, or both the head and the torso is most likely a
// whole avatar body (a devkit), not something to hold or wear.
bool looks_like_body(const DaeModel& m, const Skeleton& skel) {
    if (!m.rigged) return false;
    std::vector<char> used(skel.size(), 0);
    for (size_t i = 0; i < m.joints.size() && i < m.weights.size(); ++i)
        if (m.weights[i] > 0 && m.joints[i] >= 0 && m.joints[i] < skel.joint_count()) used[m.joints[i]] = 1;
    auto has = [&](const char* name) { int j = skel.find(name); return j >= 0 && used[j]; };
    int bones = 0;
    for (int j = 0; j < skel.joint_count(); ++j) bones += used[j];
    return has("mPelvis") || bones >= 20 || (has("mHead") && (has("mChest") || has("mTorso")));
}

}  // namespace

void App::import_prop(const std::string& path, bool as_prop) {
    DaeModel model;
    DaeReport report;
    std::string err;
    if (!load_mesh_file(path, skel_, model, report, err)) {
        message("Could not import the mesh", path + "\n\n" + err);
        return;
    }
    if (!as_prop && !headless_ && looks_like_body(model, skel_)) {
        body_prompt_path_ = path;  // draw_body_prompt asks: body or prop
        return;
    }
    Prop p;
    p.path = path;
    p.name = path.substr(path.find_last_of('/') + 1);
    p.name = p.name.substr(0, p.name.rfind('.'));
    p.rigged = model.rigged;
    p.lib_id = add_to_prop_library(p);
    edit("Import Prop", [&](Clip& c) { c.props.push_back(p); });
    clear_selection();
    if (!p.rigged) selected_prop_ = int(doc_.clip().props.size()) - 1;
    prop_models_.erase(path);  // reload next draw

    std::string text = std::to_string(report.triangles) + " triangles, " +
                       (report.rigged ? "rigged: it follows the avatar." : "static: attach it to a bone in Properties.");
    char scale[64];
    std::snprintf(scale, sizeof scale, "\nScale %.4g, up axis %s", report.scale, report.up_axis.c_str());
    text += scale;
    if (report.skipped_joint_nodes) text += "\n" + std::to_string(report.skipped_joint_nodes) + " joint nodes skipped";
    if (!report.unmapped_joints.empty()) {
        text += "\nJoints that are not SL bones (they stay still):";
        for (auto& j : report.unmapped_joints) text += "\n- " + j;
    }
    for (auto& t : report.missing_textures) text += "\nMissing texture: " + t;
    for (auto& u : report.unsupported) text += "\nNot supported: " + u;
    status("Imported " + p.name);
    message("Imported " + p.name, text);
}

void App::draw_props(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices) {
    for (const Prop& p : doc_.clip().props)
        if (p.visible) draw_prop(p, verts, indices);
}

void App::draw_prop(const Prop& p, std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices,
                    const std::vector<Xform>* globals, const Shape* shape_override, float dim, const Xform& world,
                    const float* tint) {
    const DaeModel* m = prop_model(p.path);
    if (!m) return;
    verts.clear();
    indices.clear();
    const std::vector<float>* pos = &m->positions;
    const std::vector<float>* nrm = &m->normals;
    Xform frame;
    if (p.rigged) {
        skin_prop(*m, skel_, globals ? *globals : globals_, globals ? shape_override : shape(), prop_skin_pos_,
                  prop_skin_nrm_);
        pos = &prop_skin_pos_;
        nrm = &prop_skin_nrm_;
    } else {
        frame = prop_frame(p, globals, world);
    }
    for (const DaeGroup& g : m->groups) {
        const DaeMaterial& mat = m->materials[g.material];
        for (std::uint32_t v = g.first_vertex; v < g.first_vertex + g.vertex_count; ++v) {
            Vec3 x{(*pos)[v * 3], (*pos)[v * 3 + 1], (*pos)[v * 3 + 2]}, n{(*nrm)[v * 3], (*nrm)[v * 3 + 1], (*nrm)[v * 3 + 2]};
            if (!p.rigged) {  // re-centred on the bounding box, scaled, then placed (VP-81)
                x = frame.apply(prop_local(p, *m, x));
                n = frame.rot.rotate(Vec3{n.x / p.scale.x, n.y / p.scale.y, n.z / p.scale.z}).normalized();
            }
            if (tint)
                verts.push_back({{float(x.x), float(x.y), float(x.z)}, {float(n.x), float(n.y), float(n.z)},
                                 {tint[0], tint[1], tint[2], tint[3]}});
            else
                verts.push_back({{float(x.x), float(x.y), float(x.z)}, {float(n.x), float(n.y), float(n.z)},
                                 {mat.rgba[0] * dim, mat.rgba[1] * dim, mat.rgba[2] * dim, mat.blend ? mat.rgba[3] : 1.f}});
        }
        indices.insert(indices.end(), m->indices.begin() + g.first_index,
                       m->indices.begin() + g.first_index + g.index_count);
    }
    scene_triangles(verts, indices, true, 0.15f, tint != nullptr);  // gathered by the Picker's render
}

// The nearest static prop under the cursor: the ray against each mesh's box in prop space (VP-22).
int App::pick_prop(ImVec2 m) const {
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    int best = -1;
    double best_t = 1e30;
    const auto& props = doc_.clip().props;
    for (int i = 0; i < int(props.size()); ++i) {
        const Prop& p = props[i];
        if (!p.visible || p.rigged) continue;
        auto it = prop_models_.find(p.path);
        const DaeModel* mdl = it != prop_models_.end() ? it->second.get() : nullptr;
        Xform inv = prop_frame(p).inverse();
        // A missing mesh is picked by its 10 cm placeholder box (IO-42).
        Vec3 lo = mdl ? prop_local(p, *mdl, mdl->bounds_min) : Vec3{-0.05, -0.05, -0.05};
        Vec3 hi = mdl ? prop_local(p, *mdl, mdl->bounds_max) : Vec3{0.05, 0.05, 0.05};
        Vec3 lo2{std::min(lo.x, hi.x), std::min(lo.y, hi.y), std::min(lo.z, hi.z)};
        Vec3 hi2{std::max(lo.x, hi.x), std::max(lo.y, hi.y), std::max(lo.z, hi.z)};
        Vec3 ro = inv.apply(o), rd = inv.rot.rotate(d);
        double t0 = 0, t1 = 1e30;
        bool hit = true;
        for (int a = 0; a < 3 && hit; ++a) {
            if (std::fabs(rd[a]) < 1e-12) {
                hit = ro[a] >= lo2[a] && ro[a] <= hi2[a];
                continue;
            }
            double ta = (lo2[a] - ro[a]) / rd[a], tb = (hi2[a] - ro[a]) / rd[a];
            t0 = std::max(t0, std::min(ta, tb));
            t1 = std::min(t1, std::max(ta, tb));
            hit = t0 <= t1;
        }
        if (hit && t0 < best_t) best_t = t0, best = i;
    }
    return best;
}

void App::draw_prop_section() {
    auto& props = doc_.clip().props;
    if (selected_prop_ < 0 || selected_prop_ >= int(props.size())) return;
    Prop& p = props[selected_prop_];
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(p.rigged ? "rigged" : "static");
    if (p.rigged) {
        ImGui::TextDisabled("Rigged meshes follow the avatar and have no transform of their own.");
    } else {
        const float label_w = ImGui::GetFontSize() * 5.5f;
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(-1);
        };
        auto track = [&](const char* step) {
            if (ImGui::IsItemActivated()) doc_.history.begin(doc_.clip());
            if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, doc_.clip())) mark_dirty();
        };
        label("Parent");
        std::string current = !p.point.empty() ? p.point : !p.bone.empty() ? p.bone : "World";
        if (ImGui::BeginCombo("##parent", current.c_str(), ImGuiComboFlags_HeightLarge)) {
            auto choose = [&](const std::string& bone, const std::string& point) {
                std::string b = bone, pt = point;
                edit("Prop Parent", [&](Clip& c) {
                    Prop& q = c.props[selected_prop_];
                    q.bone = b, q.point = pt, q.pos = {}, q.rot = {};  // snaps to the new parent (VP-82)
                });
            };
            if (ImGui::Selectable("World", current == "World")) choose("", "");
            ImGui::SeparatorText("Attachment points");
            for (int i = skel_.joint_count(); i < skel_.size(); ++i)
                if (ImGui::Selectable(skel_[i].name.c_str(), current == skel_[i].name)) choose("", skel_[i].name);
            ImGui::SeparatorText("Bones");
            for (int i = 0; i < skel_.joint_count(); ++i)
                if (ImGui::Selectable(skel_[i].name.c_str(), current == skel_[i].name)) choose(skel_[i].name, "");
            ImGui::EndCombo();
        }
        float v[3];
        auto vec = [&](const char* name, const char* id, Vec3& x, float speed, const char* fmt, const char* step) {
            v[0] = float(x.x), v[1] = float(x.y), v[2] = float(x.z);
            label(name);
            if (ImGui::DragFloat3(id, v, speed, 0, 0, fmt)) x = {v[0], v[1], v[2]};
            track(step);
        };
        vec("Position", "##ppos", p.pos, 0.002f, "%.3f", "Move Prop");
        vec("Rotation", "##prot", p.rot, 0.25f, "%.1f°", "Rotate Prop");
        vec("Scale", "##pscale", p.scale, 0.005f, "%.3f", "Scale Prop");
        ImGui::SetCursorPosX(label_w);
        bool visible = p.visible;
        if (ImGui::Checkbox("Visible", &visible)) edit("Prop Visibility", [&](Clip& c) { c.props[selected_prop_].visible = visible; });
        // SL build-window vectors (VP-84).
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Copy for SL");
        ImGui::SameLine(label_w);
        const char* labels[3] = {"Position", "Rotation", "Size"};
        for (int k = 0; k < 3; ++k) {
            if (k) ImGui::SameLine();
            bool clicked = ImGui::SmallButton(labels[k]);
            ImGui::SetItemTooltip("Copy as a vector to paste into the SL build window");
            if (clicked) {
                std::string v = prop_sl_string(p, k);
                ImGui::SetClipboardText(v.c_str());
                status("Copied " + v);
            }
        }
    }
    if (!prop_model(p.path)) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Mesh file not found: %s", p.path.c_str());
    if (ImGui::Button("Remove Prop")) {
        int i = selected_prop_;
        selected_prop_ = -1;
        edit("Remove Prop", [&](Clip& c) { c.props.erase(c.props.begin() + i); });
    }
}

// --- Prop library (03 section 3.6, IO-39) ---

std::string App::library_dir() const {
    std::string p = library_path();
    return p.substr(0, p.find_last_of('/') + 1);
}

void App::save_prop_library() {
    std::string path = library_dir() + "library.json", tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << vats::save_prop_library(prop_library_);
        if (!f) return message("Could not save the prop library", path);
    }
    std::rename(tmp.c_str(), path.c_str());
}

std::string App::add_to_prop_library(const Prop& p) {
    Prop stored = p;
    stored.lib_id.clear();
    stored.visible = true;
    for (PropLibraryItem& it : prop_library_) {
        if (it.prop.path != p.path) continue;
        // Re-imported: refresh the item and render its thumbnail again.
        it.prop.rigged = p.rigged;
        forget_thumbnail(it.id);
        prop_models_.erase(p.path);  // the file changed: read it again for the new thumbnail
        render_thumbnail(it.prop, thumb_png(library_dir() + it.id));  // now, not when the Inventory is next shown
        save_prop_library();
        return it.id;
    }
    prop_library_.push_back({new_library_id(), stored});
    render_thumbnail(stored, thumb_png(library_dir() + prop_library_.back().id));
    save_prop_library();
    return prop_library_.back().id;
}

const PropLibraryItem* App::find_prop_item(const std::string& id) const {
    for (auto* list : {&prop_library_, &starter_props_})
        for (const PropLibraryItem& it : *list)
            if (it.id == id) return &it;
    return nullptr;
}

void App::add_library_prop(const PropLibraryItem& it, const std::string& bone, const std::string& point, bool keep_offset) {
    if (auto m = prop_models_.find(it.prop.path); m != prop_models_.end() && !m->second) prop_models_.erase(m);  // retry
    if (!prop_model(it.prop.path))
        return message("Could not add " + it.prop.name, "The mesh file is missing or could not be read:\n" + it.prop.path);
    Prop p = it.prop;
    p.lib_id = it.id;
    p.visible = true;
    p.extra = Json::object();  // starter metadata (slug, category...) stays in the library
    if (!p.rigged && !keep_offset) p.bone = bone, p.point = point, p.pos = {}, p.rot = {};
    if (p.rigged) p.bone.clear(), p.point.clear();
    edit("Add Prop", [&](Clip& c) { c.props.push_back(p); });
    clear_selection();
    if (!p.rigged) selected_prop_ = int(doc_.clip().props.size()) - 1;
    std::string where = p.rigged ? "" : !p.point.empty() ? " on " + p.point : !p.bone.empty() ? " on " + p.bone : " in the world";
    status("Added " + p.name + where);
}

// --- Thumbnails (VP-90, VP-I12) ---

namespace {

constexpr int kThumbSize = 128;

// The thumbnail format and renderer version, in every thumbnail's file name ("starter-mug.t2.png"). Bump it when
// thumbnails already written are wrong, and each renders once more under the new name. The one older file of the same
// thumbnail (the name without the version, or with an older one) is the app's own and is removed once the new one is
// written; nothing else in the library folder is touched. Hash-named ones (pose-, file-) whose item has gone stay, as
// before. t2: the viewer's in-world thumbnails had alpha 0 (its build 25) or leftover alpha (build 24).
constexpr int kThumbVersion = 2;

// Frames a sphere from a front, slightly-high three-quarter direction through a 30 degree lens (VP-90).
Mat4 frame_sphere(Camera& cam, const Vec3& centre, double radius, double yaw) {
    const double fov = 30 * kDegToRad;
    cam.target = centre;
    cam.yaw = yaw;
    cam.pitch = 0.35;
    cam.distance = radius / std::sin(fov / 2) * 1.05;
    return perspective(fov, 1, std::max(cam.distance - radius * 1.5, cam.distance * 0.01), cam.distance + radius * 1.5);
}

}  // namespace

// Renders the prop alone, re-centred, on a transparent background, and saves a 128 x 128 PNG.
bool App::render_thumbnail(const Prop& src, const std::string& png) {
    const DaeModel* m = prop_model(src.path);
    if (!m) return false;
    Prop p;  // world, no offset: the mesh centre sits at the origin; rigged meshes at their bind shape
    p.path = src.path;
    p.scale = src.scale;
    double radius = (m->bounds_max - m->bounds_min).mul(p.scale).length() * 0.5;
    if (!(radius > 1e-6)) return false;
    Camera cam;
    Mat4 proj = frame_sphere(cam, {}, radius, -0.6);  // front-right: the avatar faces +X, its right is -Y
    if (!host_.scene_begin(ui::SceneTarget::Thumbnail, kThumbSize, kThumbSize, cam, SceneColours{}, &proj)) return false;
    std::vector<Vertex> verts;
    std::vector<std::uint32_t> indices;
    draw_prop(p, verts, indices);
    host_.scene_end();
    return host_.save_thumbnail_png(png);
}

// VP-I12: the avatar in the pose (a clip at its middle frame), framed on the bones the item moves.
bool App::render_pose_thumbnail(const LibraryItem& it, const std::string& png) {
    if (body_ == Body::SkeletonOnly || mesh_.parts().empty()) return false;
    Clip c;
    double frame = 0;
    if (it.clip) c.curves = it.curves, c.end_frame = int(std::ceil(it.length)), frame = std::round(it.length / 2);
    else apply_pose(c, skel_, it, 0, false);
    Evaluation e = vats::evaluate(*rig_, c, frame, shape());
    // Hands and other small parts fill the picture; whole poses show the body.
    std::vector<int> nodes;
    if (it.kind != "pose" && it.kind != "selection")
        for (auto& [name, v] : it.bones)
            if (int n = skel_.find(name); n >= 0) nodes.push_back(n);
    if (it.clip && it.kind != "pose")
        for (auto& [name, t] : it.curves)
            if (int n = skel_.find(name); n >= 0) nodes.push_back(n);
    if (nodes.size() < 2 || nodes.size() > 60)
        for (int n = 0; n < skel_.joint_count(); ++n)
            if (skel_[n].category == Category::Body) nodes.push_back(n);
    Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    const Shape* sh = shape();
    for (int n : nodes)  // each bone's head and tail, so fingertips and the hands of a whole pose stay in
        for (const Vec3& q : {e.globals[n].pos, e.globals[n].apply(sh ? skel_[n].end.mul(sh->scale[n]) : skel_[n].end)})
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
    Vec3 centre = (lo + hi) * 0.5;
    double radius = std::max((hi - lo).length() * 0.5, 0.04) * 1.1 + 0.02;  // room for the skin around the bones
    Camera cam;
    Mat4 proj = frame_sphere(cam, centre, radius, it.kind == "hand" ? 0.6 : -0.6);  // hand poses are left hands
    if (!host_.scene_begin(ui::SceneTarget::Thumbnail, kThumbSize, kThumbSize, cam, SceneColours{}, &proj)) return false;
    draw_avatar(false, e.globals, SceneColours{});
    host_.scene_end();
    return host_.save_thumbnail_png(png);
}

std::string App::thumb_png(const std::string& stem) { return stem + ".t" + std::to_string(kThumbVersion) + ".png"; }

// VP-90 queue: loads and renders share ~20 ms a frame; the rest wait for the next frame, which the pushed
// event brings even while the app idles. 0 = not yet, or it cannot be made (then it is not retried). A cached
// picture that is fully transparent drew nothing and is rendered again.
ImTextureID App::thumbnail(const std::string& key, const std::string& png, const std::function<bool()>& render) {
    if (auto t = thumbs_.find(key); t != thumbs_.end()) return t->second;
    static int frame = -1;
    static std::uint64_t frame_start = 0;
    const std::uint64_t now = host_.ticks_ns() / 1000000;
    if (frame != ImGui::GetFrameCount()) frame = ImGui::GetFrameCount(), frame_start = now;
    if (now - frame_start > 20) {
        host_.wake();
        return 0;
    }
    std::ifstream f(u8path(png), std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ImTextureID tex = bytes.empty() || vats::png_blank(bytes.data(), bytes.size()) ? 0 : host_.load_texture(png);
    if (!tex && render()) {
        tex = host_.load_texture(png);
        const std::string stem = png.substr(0, png.size() - thumb_png("").size());
        for (int v = 1; v < kThumbVersion; ++v)  // v1 had no version in the name
            std::remove((v == 1 ? stem + ".png" : stem + ".t" + std::to_string(v) + ".png").c_str());
    }
    return thumbs_[key] = tex;
}

void App::forget_thumbnail(const std::string& key) {
    if (auto t = thumbs_.find(key); t != thumbs_.end()) {
        if (t->second) host_.free_texture(t->second);
        thumbs_.erase(t);
    }
}

ImTextureID App::prop_thumbnail(const PropLibraryItem& it) {
    const std::string png = thumb_png(library_dir() + it.id);
    return thumbnail(it.id, png, [&] { return render_thumbnail(it.prop, png); });
}

// Cached by content (VP-I12): the item as saved plus the body, so an edited built-in pose renders afresh.
// ponytail: old pose-*.png files are never swept; add a sweep if the folder grows.
ImTextureID App::pose_thumbnail(const LibraryItem& it) {
    Library one;
    one.items.push_back(it);
    one.items[0].id.clear(), one.items[0].name.clear();
    size_t h = std::hash<std::string>{}(vats::save_library(one) + kBodyIds[int(body_)]);
    char name[40];
    std::snprintf(name, sizeof name, "pose-%016zx", h);
    std::string png = thumb_png(library_dir() + name);
    return thumbnail(name, png, [&] { return render_pose_thumbnail(it, png); });
}

void App::free_thumbnails() {
    for (auto& [key, tex] : thumbs_)
        if (tex) host_.free_texture(tex);
    thumbs_.clear();
}

// --- SL build-window vectors (VP-84, VP-85) ---

std::string App::prop_sl_string(const Prop& p, int what) {
    if (what == 0) return sl_vector(p.pos, 5);
    if (what == 1) return sl_vector(p.rot, 3);
    const DaeModel* m = prop_model(p.path);
    return sl_vector(m ? (m->bounds_max - m->bounds_min).mul(p.scale) : Vec3{}, 5);
}

namespace {
const Prop* static_prop(const Clip& clip, int i) {
    return i >= 0 && i < int(clip.props.size()) && !clip.props[i].rigged ? &clip.props[i] : nullptr;
}
}  // namespace

bool App::prop_sl_copy() {
    if (!static_prop(doc_.clip(), selected_prop_)) return false;
    prop_sl_popup_ = 1;
    return true;
}

bool App::prop_sl_paste() {
    if (!static_prop(doc_.clip(), selected_prop_)) return false;
    const char* text = ImGui::GetClipboardText();
    bool ok = text && parse_sl_vector(text, prop_sl_value_);
    if (!ok) status("Paste needs an SL vector like <1.0, 2.0, 3.0> on the clipboard");
    else prop_sl_popup_ = 2;
    return true;
}

void App::draw_prop_sl_popup() {
    if (prop_sl_popup_ && !ImGui::IsPopupOpen("##slvector")) ImGui::OpenPopup("##slvector");
    if (!ImGui::BeginPopup("##slvector")) {
        prop_sl_popup_ = 0;
        return;
    }
    const Prop* p = static_prop(doc_.clip(), selected_prop_);
    const bool copy = prop_sl_popup_ == 1;
    if (!p) {
        prop_sl_popup_ = 0;
        ImGui::CloseCurrentPopup();
        return ImGui::EndPopup();
    }
    ImGui::TextDisabled("%s", copy ? "Copy for SL" : ("Paste " + sl_vector(prop_sl_value_, 5) + " as").c_str());
    const char* what[3] = {"Position", "Rotation", "Size"};
    for (int k = 0; k < 3; ++k) {
        if (!ImGui::Selectable(what[k])) continue;
        prop_sl_popup_ = 0;
        if (copy) {
            std::string v = prop_sl_string(*p, k);
            ImGui::SetClipboardText(v.c_str());
            status("Copied " + v);
            continue;
        }
        const DaeModel* m = prop_model(p->path);
        Vec3 extent = m ? m->bounds_max - m->bounds_min : Vec3{1, 1, 1};
        const Vec3 v = prop_sl_value_;
        const int i = selected_prop_;
        edit(std::string("Paste ") + what[k], [&](Clip& c) {
            Prop& q = c.props[i];
            if (k == 0) q.pos = v;
            else if (k == 1) q.rot = v;
            else q.scale = {v.x / std::max(extent.x, 1e-5), v.y / std::max(extent.y, 1e-5), v.z / std::max(extent.z, 1e-5)};
        });
        status(std::string("Pasted ") + what[k]);
    }
    ImGui::EndPopup();
}

// --- Drag and drop from the Inventory into the 3D view (VP-83) ---

void App::viewport_drop_target(ImVec2 origin, ImVec2 size) {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!is_view_drop(payload)) return;
    if (!ImGui::BeginDragDropTargetCustom(ImRect(origin, ImVec2(origin.x + size.x, origin.y + size.y)),
                                          ImGui::GetID("##view_drop")))
        return;
    std::string id(static_cast<const char*>(payload->Data));
    ImVec2 m = ImGui::GetIO().MousePos;
    std::string hint;
    if (payload->IsDataType("VATS_PROP")) {
        const PropLibraryItem* it = find_prop_item(id);
        int bone = it && !it->prop.rigged ? pick_node(m, nullptr, true) : -1;
        // Over the bone that carries the item's own attachment point, or a finger of it (a hand for a mug):
        // use that point, with the item's offset.
        int carrier = bone;
        while (carrier >= 0 && skel_[carrier].category == Category::Hands) carrier = skel_[carrier].parent;
        if (int own = it && !it->prop.point.empty() ? skel_.find(it->prop.point) : -1; own >= 0 && carrier >= 0 && skel_[own].parent == carrier)
            bone = own;
        hover_bone_ = bone;  // highlighted by render_scene
        std::string target = bone >= 0 ? skel_[bone].name : "";
        if (it) hint = it->prop.rigged ? "Add " + it->prop.name + " (rigged: follows the avatar)"
                     : bone >= 0      ? "Attach " + it->prop.name + " to " + target
                                      : "Place " + it->prop.name + " in the world";
        if (ImGui::AcceptDragDropPayload("VATS_PROP", ImGuiDragDropFlags_AcceptNoDrawDefaultRect) && it) {
            bool point = bone >= 0 && skel_[bone].attachment;
            std::string b = point ? "" : target, pt = point ? target : "";
            Vec3 ground;
            if (bone < 0 && !it->prop.rigged && pick_surface(m, ground)) {  // world: where it was dropped
                PropLibraryItem placed = *it;
                bool world = it->prop.bone.empty() && it->prop.point.empty();  // keeps a world item's height and turn
                placed.prop.pos = ground + (world ? it->prop.pos : Vec3{});
                if (!world) placed.prop.rot = {};
                placed.prop.bone.clear(), placed.prop.point.clear();
                add_library_prop(placed, "", "", true);
            } else {
                add_library_prop(*it, b, pt, !it->prop.rigged && b == it->prop.bone && pt == it->prop.point);
            }
        }
    } else if (payload->IsDataType("VATS_FILE")) {
        file_drop(payload, hint);  // Projects and Animations (file_library_ui.cpp)
    } else {
        const LibraryItem* pose = nullptr;
        for (const LibraryItem& it : library_.items)
            if (it.id == id) pose = &it;
        for (const LibraryItem& it : builtin_poses(skel_))
            if (!pose && it.id == id) pose = &it;
        if (pose) hint = (pose->clip ? "Paste " : "Apply ") + pose->name + " at frame " + std::to_string(int(std::round(frame_)));
        if (ImGui::AcceptDragDropPayload("VATS_POSE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect) && pose)
            apply_library_item(*pose, apply_mirrored_);
    }
    ImGui::EndDragDropTarget();
    if (hint.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize(hint.c_str()), at(m.x + 14, m.y - ts.y - 14);  // above: the drag tooltip sits below
    dl->AddRectFilled(ImVec2(at.x - 5, at.y - 3), ImVec2(at.x + ts.x + 5, at.y + ts.y + 3), IM_COL32(12, 13, 16, 220), 4);
    dl->AddText(at, IM_COL32(255, 238, 170, 255), hint.c_str());
}

void App::draw_body_prompt() {
    if (body_prompt_path_.empty()) return;
    const char* title = "This Looks Like an Avatar Body";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::string name = body_prompt_path_.substr(body_prompt_path_.find_last_of('/') + 1);
    ImGui::Text("%s is rigged to most of the skeleton.", name.c_str());
    ImGui::TextUnformatted("Use it as your body instead? It then replaces the Linden body and shapes the avatar.");
    ImGui::Spacing();
    const std::string path = body_prompt_path_;
    auto done = [&] {
        body_prompt_path_.clear();
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("Use as Body")) {
        done();
        guarded(path, [&] { import_body({path}); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Add as Prop")) {
        done();
        guarded(path, [&] { import_prop(path, true); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
    ImGui::EndPopup();
}

}  // namespace vats
