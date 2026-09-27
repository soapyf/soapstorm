// Viewport Avatar Toolset - the 3D view: scene, navigation, picking and gizmo edits.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "imgui_internal.h"  // the dockspace's central node (world view)
#include "profile.h"
#include "vats/edit.h"
#include "theme.h"

namespace vats {
namespace {

const Rgb kCategoryColour[kCategoryCount] = {{0.95f, 0.62f, 0.25f}, {0.55f, 0.80f, 0.35f}, {0.90f, 0.45f, 0.60f},
                                             {0.45f, 0.65f, 0.95f}, {0.70f, 0.50f, 0.95f}, {0.35f, 0.80f, 0.80f},
                                             {0.80f, 0.80f, 0.80f}, {1.00f, 1.00f, 1.00f}, {0.78f, 0.74f, 0.90f}};
const Rgb kVolume{0.80f, 0.78f, 1.00f};  // pale lavender-grey (VP-10)
const Rgb kSelected{1.0f, 0.95f, 0.2f};

Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

void push_triangle(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c, Rgb col) {
    Vec3 n = (b - a).cross(c - a).normalized();
    for (const Vec3* p : {&a, &b, &c})
        out.push_back({{float(p->x), float(p->y), float(p->z)}, {float(n.x), float(n.y), float(n.z)}, {col.r, col.g, col.b, 1}});
}

// A four-sided double pyramid from head to tail, widest (9 % of the length) at 15 % along (spec VP-6).
// rot is the bone's display frame (SK-21), so its X (or Z) axis gives the roll.
void bone_glyph(std::vector<Vertex>& out, const Vec3& head, const Vec3& tail, const Quat& rot, Rgb col) {
    Vec3 d = tail - head;
    double len = d.length();
    Vec3 dir = d * (1.0 / len);
    Vec3 roll = rot.rotate({1, 0, 0});
    if (std::fabs(roll.dot(dir)) > 0.9) roll = rot.rotate({0, 0, 1});
    Vec3 u = (roll - dir * roll.dot(dir)).normalized(), v = dir.cross(u);
    Vec3 mid = head + dir * (0.15 * len);
    double w = 0.09 * len;
    Vec3 ring[4] = {mid + u * w, mid + v * w, mid - u * w, mid - v * w};
    for (int k = 0; k < 4; ++k) {
        push_triangle(out, head, ring[(k + 1) % 4], ring[k], col);
        push_triangle(out, tail, ring[k], ring[(k + 1) % 4], col);
    }
}

double segment_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
    double abx = b.x - a.x, aby = b.y - a.y, apx = p.x - a.x, apy = p.y - a.y;
    double t = std::clamp((apx * abx + apy * aby) / std::max(abx * abx + aby * aby, 1e-9), 0.0, 1.0);
    return std::hypot(apx - abx * t, apy - aby * t);
}

// Handle colours by side (VP-12).
ImU32 side_colour(const LimbInfo& l) {
    if (l.spine) return IM_COL32(140, 230, 115, 255);
    return l.name.find("Left") != std::string::npos ? IM_COL32(89, 166, 255, 255) : IM_COL32(255, 115, 102, 255);
}

// A unit sphere as a triangle list, counter-clockwise from outside.
const std::vector<Vec3>& unit_sphere() {
    static const std::vector<Vec3> tris = [] {
        constexpr int kRings = 10, kSides = 16;
        auto at = [](int r, int s) {
            double th = kPi * r / kRings, ph = 2 * kPi * s / kSides;
            return Vec3{std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)};
        };
        std::vector<Vec3> out;
        for (int r = 0; r < kRings; ++r)
            for (int s = 0; s < kSides; ++s) {
                Vec3 a = at(r, s), b = at(r + 1, s), c = at(r + 1, s + 1), d = at(r, s + 1);
                if (r > 0) out.insert(out.end(), {a, b, d});
                if (r < kRings - 1) out.insert(out.end(), {b, c, d});
            }
        return out;
    }();
    return tris;
}

}  // namespace

// Collision volumes as translucent ellipsoids on their (animated) nodes, semi-axes scaled by the owning
// joint's shape scale (VP-10, SK-I5).
void App::draw_collision_volumes(std::vector<Vertex>& verts) {
    verts.clear();
    const Shape* sh = shape();
    for (const CollisionVolume& v : skel_.volumes()) {
        if (!node_visible(v.node)) continue;
        Vec3 axes = sh ? v.scale.mul(sh->scale[v.joint]) : v.scale;
        bool sel = std::find(selection_.begin(), selection_.end(), v.node) != selection_.end();
        Rgb c = v.node == primary() ? kSelected : sel ? mix(kSelected, kVolume, 0.45f) : v.node == hover_bone_ ? mix(kVolume, {1, 1, 1}, 0.5f) : kVolume;
        float a = sel || v.node == hover_bone_ ? 0.32f : 0.2f;
        const Xform& g = globals_[v.node];
        for (const Vec3& u : unit_sphere()) {
            Vec3 p = g.apply(u.mul(axes)), n = g.rot.rotate(Vec3{u.x / axes.x, u.y / axes.y, u.z / axes.z}).normalized();
            verts.push_back({{float(p.x), float(p.y), float(p.z)}, {float(n.x), float(n.y), float(n.z)}, {c.r, c.g, c.b, a}});
        }
    }
}

bool App::handle_shown(int limb, bool pole) const {
    const LimbInfo& l = rig_->limbs()[limb];
    if (pole && l.spine) return false;
    if (!node_visible(l.end)) return false;
    return limb_states_[limb].ik_on || std::find(handles_.begin(), handles_.end(), HandleRef{limb, pole}) != handles_.end();
}

bool App::handle_screen(int limb, bool pole, ImVec2& out) const {
    const LimbState& s = limb_states_[limb];
    double x, y;
    if (!projector_.to_screen(pole ? s.pole : s.target.pos, x, y)) return false;
    out = ImVec2(float(x), float(y));
    return true;
}

int App::pick_handle(ImVec2 m, bool& pole) const {
    int best = -1;
    double best_d = 12;
    for (int l = 0; l < int(limb_states_.size()); ++l)
        for (bool p : {false, true}) {
            ImVec2 s;
            if (!handle_shown(l, p) || !handle_screen(l, p, s)) continue;
            double d = std::hypot(s.x - m.x, s.y - m.y);
            if (d < best_d) best_d = d, best = l, pole = p;
        }
    return best;
}

void App::draw_handles(ImDrawList* dl) const {
    for (int l = 0; l < int(limb_states_.size()); ++l) {
        const LimbInfo& info = rig_->limbs()[l];
        const LimbState& s = limb_states_[l];
        for (bool pole : {false, true}) {
            if (!handle_shown(l, pole)) continue;
            HandleRef ref{l, pole};
            bool sel = std::find(handles_.begin(), handles_.end(), ref) != handles_.end();
            bool prim = primary_handle() && *primary_handle() == ref;
            ImU32 c = side_colour(info);
            if (prim) c = IM_COL32(255, 242, 51, 255);
            else if (sel) c = IM_COL32(255, 200, 90, 255);
            if (!s.ik_on) c = (c & 0x00FFFFFF) | (102u << 24);  // FK limb, shown only while selected
            if (hover_handle_ == l && hover_handle_pole_ == pole && !prim) c = (c & 0xFF000000) | 0x00F0F0F0;
            if (!pole) {  // wire cube in the target's orientation
                double h = info.spine ? 0.11 : info.finger ? 0.012 : 0.055;
                ImVec2 p[8];
                bool ok = true;
                for (int k = 0; k < 8; ++k) {
                    Vec3 corner{(k & 1 ? h : -h), (k & 2 ? h : -h), (k & 4 ? h : -h)};
                    double x = 0, y = 0;
                    ok = ok && projector_.to_screen(Xform{s.target.rot * skel_.bone_frame(info.end), s.target.pos}.apply(corner), x, y);
                    p[k] = ImVec2(float(x), float(y));
                }
                if (!ok) continue;
                static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                                 {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& e : edges) dl->AddLine(p[e[0]], p[e[1]], c, prim ? 2.5f : 1.8f);
            } else {
                ImVec2 d, mid;
                double x, y;
                if (!handle_screen(l, true, d) || !projector_.to_screen(globals_[info.mid].pos, x, y)) continue;
                mid = ImVec2(float(x), float(y));
                float len = std::hypot(d.x - mid.x, d.y - mid.y);
                for (float t = 0; t < len; t += 12)  // dashed line from the middle joint
                    dl->AddLine(ImVec2(mid.x + (d.x - mid.x) * t / len, mid.y + (d.y - mid.y) * t / len),
                                ImVec2(mid.x + (d.x - mid.x) * std::min(t + 6, len) / len,
                                       mid.y + (d.y - mid.y) * std::min(t + 6, len) / len),
                                (c & 0x00FFFFFF) | (178u << 24));
                float r = prim ? 7 : 5.5f;
                dl->AddQuadFilled(ImVec2(d.x, d.y - r), ImVec2(d.x + r, d.y), ImVec2(d.x, d.y + r), ImVec2(d.x - r, d.y), c);
            }
        }
    }
}

// The skinned body for a pose, in the view or a thumbnail renderer.
void App::draw_avatar(bool view, const std::vector<Xform>& globals, const SceneColours& colours) {
    static std::vector<Vertex> verts;
    static std::vector<std::uint32_t> skin_idx, eye_idx;
    static std::vector<float> other_pos, other_nrm;  // thumbnails: skin_pos_ stays the view's, for picking
    static const AvatarMesh* built_for = nullptr;
    static Body built_body = Body::SkeletonOnly;
    if (built_for != &mesh_ || built_body != body_) {  // split the index list by material once per body
        skin_idx.clear();
        eye_idx.clear();
        for (auto& part : mesh_.parts()) {
            auto& dst = part.material == Material::Eye ? eye_idx : skin_idx;
            dst.insert(dst.end(), mesh_.indices().begin() + part.first_index,
                       mesh_.indices().begin() + part.first_index + part.index_count);
        }
        built_for = &mesh_;
        built_body = body_;
    }
    std::vector<float>& pos = view ? skin_pos_ : other_pos;
    std::vector<float>& nrm = view ? skin_nrm_ : other_nrm;
    mesh_.skin(globals, shape(), pos, nrm);
    verts.resize(pos.size() / 3);
    for (auto& part : mesh_.parts()) {
        Rgb c = part.material == Material::Eye ? colours.eye : colours.body;
        for (std::uint32_t i = part.first_vertex; i < part.first_vertex + part.vertex_count; ++i)
            verts[i] = {{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}, {nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]},
                        {c.r, c.g, c.b, 1}};
    }
    host_.scene_triangles(verts, skin_idx, true, 0.04f);
    host_.scene_triangles(verts, eye_idx, true, 0.6f);
}

// Onion skin (08 ON-1..3): ghosts of the pose at nearby frames, earlier ones cool, later ones warm, fading
// with distance. Drawn see-through and never picked; nothing is evaluated while they are off or playing.
void App::draw_onion(const SceneColours& colours) {
    const OnionView v = onion_view();
    if (!v.on || playing_ || !rig_) return;
    const Shape* sh = shape();
    const auto ghosts = onion_ghosts(*rig_, doc_.clip(), frame_, sh, v.s);
    if (ghosts.empty()) return;
    const Rgb cool = mix({0.35f, 0.62f, 1.0f}, colours.body, 0.2f), warm = mix({1.0f, 0.58f, 0.28f}, colours.body, 0.2f);
    static std::vector<Vertex> verts;
    static std::vector<std::uint32_t> idx;
    static std::vector<float> pos, nrm;
    const bool bones = v.bones_only || (body_ == Body::SkeletonOnly && !mesh_body());
    for (auto it = ghosts.rbegin(); it != ghosts.rend(); ++it) {  // farthest first
        const OnionGhost& g = *it;
        const Rgb c = g.at.offset < 0 ? cool : warm;
        const float alpha = 0.12f + 0.28f * g.at.weight;
        if (bones) {
            verts.clear();
            for (int i = 0; i < skel_.joint_count(); ++i) {
                if (!node_visible(i)) continue;
                Vec3 end = sh ? skel_[i].end.mul(sh->scale[i]) : skel_[i].end;
                if (end.length() < 1e-5) continue;
                bone_glyph(verts, g.globals[i].pos, g.globals[i].apply(end), g.globals[i].rot * skel_.bone_frame(i), c);
            }
            for (Vertex& vx : verts) vx.c[3] = alpha;
            host_.scene_triangles(verts, {}, true, 0.1f, true);
        } else if (const MeshBody* mb = mesh_body()) {
            const float tint[4] = {c.r, c.g, c.b, alpha};
            for (const std::string& path : mb->parts) {
                Prop part;
                part.path = path;
                part.rigged = true;
                draw_prop(part, verts, idx, &g.globals, sh, 1.f, {}, tint);
            }
        } else {
            mesh_.skin(g.globals, sh, pos, nrm);
            verts.resize(pos.size() / 3);
            for (size_t i = 0; i < verts.size(); ++i)
                verts[i] = {{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}, {nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]},
                            {c.r, c.g, c.b, alpha}};
            host_.scene_triangles(verts, mesh_.indices(), true, 0.05f, true);
        }
    }
}

ImTextureID App::render_scene(int w, int h) {
    const SceneColours& colours = scene_colours();
    { VATS_PROFILE("vp begin+ground");
    if (!host_.scene_begin(ui::SceneTarget::View, w, h, camera_, colours)) return ImTextureID{};  // the viewer: the world is the view
    host_.scene_ground(globals_.empty() ? Vec3{} : globals_[0].pos); }

    static std::vector<Vertex> prop_verts;
    static std::vector<std::uint32_t> prop_indices;
    {
        VATS_PROFILE("vp body");
        if (mesh_body()) draw_mesh_body(prop_verts, prop_indices);  // replaces the Linden mesh (BD-2)
        else if (body_ != Body::SkeletonOnly) draw_avatar(true, globals_, colours);
    }
    { VATS_PROFILE("vp other actors"); draw_other_actors(colours); }  // couples and groups (GR-1)
    { VATS_PROFILE("vp onion"); draw_onion(colours); }  // ghosts (08 ON)
    { VATS_PROFILE("vp props"); draw_props(prop_verts, prop_indices); }

    VATS_PROFILE("vp volumes+bones+end");
    static std::vector<Vertex> volumes;
    draw_collision_volumes(volumes);
    host_.scene_triangles(volumes, {}, !xray_, 0.1f, true);

    static std::vector<Vertex> bones;
    bones.clear();
    const Shape* sh = shape();
    for (int i = 0; i < skel_.size(); ++i) {
        if (!node_visible(i)) continue;
        Vec3 end = sh ? skel_[i].end.mul(sh->scale[i]) : skel_[i].end;
        if (end.length() < 1e-5) continue;
        Rgb c = kCategoryColour[int(skel_[i].category)];
        bool sel = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        if (i == primary()) c = kSelected;
        else if (sel) c = mix(kSelected, c, 0.45f);
        else if (i == hover_bone_) c = mix(c, {1, 1, 1}, 0.5f);
        bone_glyph(bones, globals_[i].pos, globals_[i].apply(end), local_axes(i), c);
    }
    host_.scene_triangles(bones, {}, !xray_, 0.25f);

    // Other actors shown as a skeleton (GR): the body bones only, dimmed towards their colour.
    bones.clear();
    for (const auto& [g, colour] : other_skeletons_)
        for (int i = 0; i < skel_.joint_count(); ++i) {
            if (!node_visible(i) || skel_[i].end.length() < 1e-5) continue;
            Rgb c = mix(kCategoryColour[int(skel_[i].category)], {colour[0], colour[1], colour[2]}, 0.5f);
            c = {c.r * 0.7f, c.g * 0.7f, c.b * 0.7f};
            bone_glyph(bones, g[i].pos, g[i].apply(skel_[i].end), g[i].rot * skel_.bone_frame(i), c);
        }
    host_.scene_triangles(bones, {}, true, 0.25f);
    return host_.scene_end();
}

int App::pick_bone(ImVec2 m, std::vector<int>* ranked) const { return pick_node(m, ranked, false); }

// with_points: attachment points count even while hidden (Inventory drops, VP-83).
int App::pick_node(ImVec2 m, std::vector<int>* ranked, bool with_points) const {
    const Shape* sh = shape();
    std::vector<std::pair<double, int>> hits;
    for (int i = 0; i < skel_.size(); ++i) {
        if (!node_visible(i) && !(with_points && skel_[i].attachment && !skel_[i].volume)) continue;
        Vec3 end = sh ? skel_[i].end.mul(sh->scale[i]) : skel_[i].end;
        if (end.length() < 1e-5) continue;
        double hx, hy, tx, ty;
        if (!projector_.to_screen(globals_[i].pos, hx, hy) || !projector_.to_screen(globals_[i].apply(end), tx, ty))
            continue;
        ImVec2 a{float(hx), float(hy)}, b{float(tx), float(ty)};
        double d = segment_distance(m, a, b);
        if (skel_[i].attachment) d = std::min(d, std::max(0.0, double(std::hypot(m.x - a.x, m.y - a.y)) - 6.0));  // the dot
        if (d > 14) continue;
        // Shorter bones win where segments overlap (spec VP-21).
        hits.emplace_back(d + 0.04 * std::min(std::hypot(tx - hx, ty - hy), 200.0), i);
    }
    std::sort(hits.begin(), hits.end());
    if (ranked) {
        ranked->clear();
        for (auto& h : hits) ranked->push_back(h.second);
    }
    return hits.empty() ? -1 : hits.front().second;
}

bool App::pick_surface(ImVec2 m, Vec3& point) const {
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    double best = 1e30;
    if (body_ != Body::SkeletonOnly && !skin_pos_.empty()) best = ray_triangles(o, d, skin_pos_, mesh_.indices());
    for (size_t i = 0; i < actor_pick_pos_.size(); ++i)  // other actors (GR)
        if (actor_pick_idx_[i]) best = std::min(best, ray_triangles(o, d, actor_pick_pos_[i], *actor_pick_idx_[i]));
    if (best < 1e30) {
        point = o + d * best;
        return true;
    }
    if (d.z < -1e-3 && -o.z / d.z < 20) {
        point = o + d * (-o.z / d.z);
        return true;
    }
    return false;
}

Tool App::effective_tool() const {
    const ImGuiIO& io = ImGui::GetIO();
    if (settings_.preset != Preset::SecondLife || !io.KeyCtrl || io.KeyAlt) return tool_;
    return io.KeyShift ? Tool::Scale : Tool::Rotate;  // SL build tool: Ctrl rotates, Ctrl+Shift scales
}

// Remembers the selection's state at the start of a gizmo drag or modal transform.
void App::capture_edit_start() {
    Clip& clip = doc_.clip();
    const Prop* sp = selected_prop_ >= 0 && selected_prop_ < int(clip.props.size()) ? &clip.props[selected_prop_] : nullptr;
    const HandleRef* ph = primary_handle();
    int p = primary();
    if (sp) {
        drag_start_prop_ = *sp;
        drag_start_global_ = prop_frame(*sp);
        int parent = !sp->point.empty() ? skel_.find(sp->point) : !sp->bone.empty() ? skel_.find(sp->bone) : -1;
        drag_parent_global_ = parent >= 0 ? globals_[parent] : Xform{};
    } else if (ph) {
        drag_start_target_ = limb_states_[ph->limb].target;
        drag_start_pole_ = limb_states_[ph->limb].pole;
    } else if (p >= 0) {
        drag_start_global_ = globals_[p];
        drag_parent_global_ = skel_[p].parent >= 0 ? globals_[skel_[p].parent] : Xform{};
        drag_start_offset_ = pose_.offset[p];
        drag_start_euler_ = curve_euler(clip, skel_[p].name, frame_);
    }
}

void App::apply_gizmo_drag(ImVec2 m, bool snap) {
    Quat r;
    Vec3 t;
    gizmo_.drag(m, snap, snap_deg_, r, t);
    const Gizmo::Part part = gizmo_.drag_part();
    Clip& clip = doc_.clip();
    if (drag_tool_ == Tool::Scale) {  // props only, in the prop's own axes (VP-42, VP-47)
        if (selected_prop_ >= 0 && selected_prop_ < int(clip.props.size()))
            clip.props[selected_prop_].scale = drag_start_prop_.scale.mul(gizmo_.scale());
        return;
    }
    bool gimbal = drag_tool_ == Tool::Rotate && orientation_ == Orientation::Gimbal && part >= Gizmo::AxisX && part <= Gizmo::AxisZ;
    apply_delta(r, t, gimbal ? part - Gizmo::AxisX : -1, gizmo_.last_angle());
}

// Applies a world-space rotation r / translation t (from the edit's start) to the selection.
void App::apply_delta(const Quat& r, const Vec3& t, int gimbal_axis, double gimbal_angle) {
    Clip& clip = doc_.clip();
    if (selected_prop_ >= 0 && selected_prop_ < int(clip.props.size())) {
        Prop& p = clip.props[selected_prop_];
        p = drag_start_prop_;
        Quat inv_parent = drag_parent_global_.rot.conj();
        if (drag_tool_ == Tool::Rotate) {
            Quat local = (inv_parent * r * drag_start_global_.rot).normalized();
            p.rot = nearest_euler(local, drag_start_prop_.rot);  // no flips (VP-50)
        } else {
            p.pos = drag_start_prop_.pos + inv_parent.rotate(t);
        }
        return;
    }
    if (const HandleRef* h = primary_handle()) {
        if (h->pole) {
            key_limb_pole(clip, *rig_, frame_, h->limb, drag_start_pole_ + t, shape());
        } else {
            Xform target = drag_start_target_;
            if (drag_tool_ == Tool::Rotate) target.rot = (r * target.rot).normalized();
            else target.pos = target.pos + t;
            key_limb_target(clip, *rig_, frame_, h->limb, target, shape());
        }
        return;
    }
    int p = primary();
    if (p < 0) return;
    const Node& n = skel_[p];
    if (pin_at(clip, *rig_, p, frame_) >= 0) {  // a pinned point keys its pin offset instead (AM-86)
        Xform world = drag_start_global_;
        if (drag_tool_ == Tool::Rotate) world.rot = (r * world.rot).normalized();
        else world.pos = world.pos + t;
        key_pinned_point(clip, *rig_, frame_, p, world, shape());
        return;
    }
    if (gimbal_axis >= 0) {
        Vec3 e = drag_start_euler_;  // one channel changes, the other two stay exactly as keyed
        e[gimbal_axis] += gimbal_angle * kRadToDeg;
        key_euler(clip, n.name, frame_, e);
    } else if (drag_tool_ == Tool::Rotate) {
        // New global rotation, expressed back in the bone's frame after its parent and rest rotation.
        Quat global = (r * drag_start_global_.rot).normalized();
        Quat local = (n.rest.conj() * drag_parent_global_.rot.conj() * global).normalized();
        key_rotation(clip, n.name, frame_, local);
    } else {
        Vec3 local = drag_parent_global_.rot.conj().rotate(t);
        const Shape* sh = shape();
        if (sh && n.parent >= 0) {
            Vec3 s = sh->scale[n.parent];
            local = {local.x / s.x, local.y / s.y, local.z / s.z};
        }
        key_offset(clip, n.name, frame_, drag_start_offset_ + local);
    }
}

// Places the gizmo on the selection for the current camera; false when there is none. Runs again just before
// drawing, so a camera that moved during input (a navigation drag returns early) never leaves it behind.
bool App::place_gizmo() {
    int p = primary();
    const HandleRef* ph = primary_handle();
    const Tool tool = dragging_gizmo_ ? drag_tool_ : effective_tool();
    bool gizmo_on = false;
    const Prop* sp = selected_prop_ >= 0 && selected_prop_ < int(doc_.clip().props.size()) ? &doc_.clip().props[selected_prop_] : nullptr;
    if (sp && !sp->rigged && (tool == Tool::Move || tool == Tool::Rotate || tool == Tool::Scale)) {
        gizmo_on = true;
        Xform f = prop_frame(*sp);
        Quat axes = orientation_ == Orientation::World && tool != Tool::Scale ? Quat{} : f.rot;  // Scale: own axes
        gizmo_.place(tool == Tool::Rotate ? GizmoKind::Rotate : tool == Tool::Scale ? GizmoKind::Scale : GizmoKind::Move,
                     f.pos, axes, camera_, projector_, gizmo_size_);
    } else if (ph) {  // IK handles: move targets and poles, rotate targets (VP-40)
        const LimbState& s = limb_states_[ph->limb];
        gizmo_on = tool == Tool::Move || (tool == Tool::Rotate && !ph->pole);
        if (gizmo_on) {
            // SK-21: the target's local axes follow the end bone's frame; rotate drags still use target.rot.
            Quat axes = orientation_ == Orientation::Local && !ph->pole
                            ? s.target.rot * skel_.bone_frame(rig_->limbs()[ph->limb].end) : Quat{};
            gizmo_.place(tool == Tool::Rotate ? GizmoKind::Rotate : GizmoKind::Move, ph->pole ? s.pole : s.target.pos,
                         axes, camera_, projector_, gizmo_size_);
        }
    } else if (p >= 0 && node_visible(p) && (tool == Tool::Rotate || tool == Tool::Move)) {
        gizmo_on = true;
        Quat axes = orientation_ == Orientation::World ? Quat{} : local_axes(p);
        gizmo_.place(tool == Tool::Rotate ? GizmoKind::Rotate : GizmoKind::Move, globals_[p].pos, axes, camera_,
                     projector_, gizmo_size_);
        if (orientation_ == Orientation::Gimbal && tool == Tool::Rotate) {
            // Z ring in the parent-rest frame, Y turned by the Z angle, X by Z then Y (spec AM-37).
            Quat frame = (skel_[p].parent >= 0 ? globals_[skel_[p].parent].rot : Quat{}) * skel_[p].rest;
            Vec3 e = curve_euler(doc_.clip(), skel_[p].name, frame_);
            Quat rz = Quat::axis_angle({0, 0, 1}, e.z * kDegToRad), ry = Quat::axis_angle({0, 1, 0}, e.y * kDegToRad);
            Vec3 axes3[3] = {(frame * rz * ry).rotate({1, 0, 0}), (frame * rz).rotate({0, 1, 0}), frame.rotate({0, 0, 1})};
            gizmo_.set_axes(axes3);
        }
    }
    gizmo_on = gizmo_on && gizmo_.visible();
    return gizmo_on;
}

// The viewer's keyboard camera: Alt + Left/Right orbits and Alt + Up/Down zooms; with Ctrl, Up/Down orbits
// up and down; with Ctrl + Shift the arrows pan. Works wherever the pointer is, like the viewer.
void App::keyboard_camera() {
    ImGuiIO& io = ImGui::GetIO();
    cam_keys_held_ = false;
    if (settings_.preset != Preset::SecondLife || !io.KeyAlt || io.WantTextInput) return;
    const int x = ImGui::IsKeyDown(ImGuiKey_RightArrow) - ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    const int y = ImGui::IsKeyDown(ImGuiKey_UpArrow) - ImGui::IsKeyDown(ImGuiKey_DownArrow);
    if (!x && !y) return;
    cam_keys_held_ = true;
    cam_anim_t_ = -1;
    const double px = 150 * io.DeltaTime;  // about 70 degrees a second, like a steady 150 px/s mouse drag
    if (io.KeyCtrl && io.KeyShift) camera_.pan(-x * px, y * px);  // the camera goes the way the arrow points
    else if (io.KeyCtrl) camera_.orbit(x * px, -y * px);
    else camera_.orbit(x * px, 0), camera_.zoom(std::exp(-0.005 * y * px));
}

void App::viewport_input(const ImVec2& origin, const ImVec2& size, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    viewport_hovered_ = hovered;
    if (modal_input(m)) return;  // a modal transform owns the mouse and keys (VP-51)
    {
        // VP-40: Scale never applies to bones; say so when the tool is picked with no static prop selected.
        const Tool now = effective_tool();
        const auto& props = doc_.clip().props;
        bool prop = selected_prop_ >= 0 && selected_prop_ < int(props.size()) && !props[selected_prop_].rigged;
        if (now == Tool::Scale && last_effective_tool_ != Tool::Scale && !prop)
            status("Scale works on static props only: SL animations store no bone scale");
        last_effective_tool_ = now;
    }
    keyboard_camera();
    static int nav_button = -1, nav_mode = 0;  // mode: 0 orbit, 1 pan, 2 zoom
    static bool nav_moved = false;
    const Preset preset = settings_.preset;

    // Camera drags per control preset (spec 04 section 3.1); the wheel zooms in every preset.
    if (nav_button >= 0) {
        if (!ImGui::IsMouseDown(nav_button)) {
            // QAvimator: a click on empty space without dragging clears the selection.
            if (preset == Preset::QAvimator && nav_button == 0 && !nav_moved && !io.KeyShift) clear_selection();
            nav_button = -1;
        } else {
            ImVec2 d = io.MouseDelta;
            nav_moved = nav_moved || d.x != 0 || d.y != 0;
            if (nav_moved && cam_anim_t_ >= 0) update_camera_animation(1);  // a drag finishes the focus swing first
            if (nav_mode == 0) camera_.orbit(d.x, d.y);
            else if (nav_mode == 1) camera_.pan(d.x, d.y);
            else if (nav_mode == 3) camera_.orbit(d.x, 0), camera_.zoom(std::exp(0.005 * d.y));  // SL focus drag
            else camera_.zoom(std::exp(0.005 * (d.y - d.x)));
        }
        return;
    }
    if (hovered && io.MouseWheel != 0) camera_.zoom(std::pow(0.9, io.MouseWheel));
    // The viewer's world view: its own camera controls get those clicks (spec 09 U3).
    if (hovered && !dragging_gizmo_ && euler_drag_bone_ < 0 && !host_.world_view()) {
        auto start = [&](int button, int mode) {
            nav_button = button, nav_mode = mode, nav_moved = false;
        };
        if (preset == Preset::Industry && io.KeyAlt) {
            if (ImGui::IsMouseClicked(0)) start(0, 0);
            else if (ImGui::IsMouseClicked(2)) start(2, 1);
            else if (ImGui::IsMouseClicked(1)) start(1, 2);
        } else if (preset == Preset::Blender) {
            int b = ImGui::IsMouseClicked(2) ? 2 : (settings_.emulate_3_button && io.KeyAlt && ImGui::IsMouseClicked(0)) ? 0 : -1;
            if (b >= 0) start(b, io.KeyShift ? 1 : io.KeyCtrl ? 2 : 0);
        } else if (preset == Preset::QAvimator && ImGui::IsMouseClicked(2)) {
            start(2, 1);
        } else if (preset == Preset::SecondLife && io.KeyAlt && ImGui::IsMouseClicked(0)) {
            if (io.KeyCtrl) {
                start(0, io.KeyShift ? 1 : 0);  // Ctrl+Alt orbit, Ctrl+Alt+Shift pan
            } else {
                // Alt+click: look at what was clicked, then drag sideways to orbit and up/down to zoom.
                Vec3 hit;
                if (pick_surface(m, hit)) focus_camera_on(hit);
                start(0, 3);
            }
        }
        if (preset == Preset::SecondLife && ImGui::IsMouseClicked(2)) start(2, 1);
        if (nav_button >= 0) return;
    }

    // QAvimator: Shift/Ctrl/Alt + drag on a bone turns one Euler channel (spec 04 section 3.1).
    if (euler_drag_bone_ >= 0) {
        if (!ImGui::IsMouseDown(0)) {
            if (doc_.history.commit("Rotate " + skel_[euler_drag_bone_].name, doc_.clip())) mark_dirty();
            euler_drag_bone_ = -1;
        } else {
            double dx = m.x - euler_drag_press_.x, dy = m.y - euler_drag_press_.y;
            Vec3 e = euler_drag_start_;
            if (euler_drag_axis_ == 1) e.y += dy;
            else if (euler_drag_axis_ == 0) e.x -= dx;
            else e.z += dx;
            key_euler(doc_.clip(), skel_[euler_drag_bone_].name, frame_, e);
        }
        return;
    }

    // VP-26: a bone pressed with the Rotate tool turns about the view axis through its head once the cursor
    // has moved 4 px; the angle is the change of the cursor's screen angle around the projected head.
    if (bone_drag_ >= 0) {
        if (!ImGui::IsMouseDown(0)) {
            if (bone_drag_started_ && doc_.history.commit("Rotate " + skel_[bone_drag_].name, doc_.clip())) {
                mark_dirty();
                status("Rotated " + skel_[bone_drag_].name + " at frame " + std::to_string(int(std::round(frame_))));
            }
            bone_drag_ = -1;
            return;
        }
        if (bone_drag_started_ && (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            doc_.clip() = doc_.history.cancel();
            bone_drag_ = -1;
            skip_shortcuts_ = true;
            status("Cancelled");
            return;
        }
        if (!bone_drag_started_) {
            if (std::hypot(m.x - bone_drag_press_.x, m.y - bone_drag_press_.y) < 4) return;
            bone_drag_started_ = true;
            doc_.history.begin(doc_.clip());
            drag_tool_ = Tool::Rotate;
            capture_edit_start();
        }
        double hx, hy;
        if (!projector_.to_screen(drag_start_global_.pos, hx, hy)) return;
        double angle = std::remainder(std::atan2(m.y - hy, m.x - hx) -
                                          std::atan2(bone_drag_press_.y - hy, bone_drag_press_.x - hx), 2 * kPi);
        if (preset == Preset::SecondLife ? snap_on_ : io.KeyCtrl) {
            double step = snap_deg_ * kDegToRad;
            angle = std::round(angle / step) * step;
        }
        apply_delta(Quat::axis_angle(camera_.forward(), angle), {});
        return;
    }

    if (actor_gizmo_input(m, hovered)) return;  // GR: placing another actor

    // Gizmo on the primary bone.
    int p = primary();
    const HandleRef* ph = primary_handle();
    const Tool tool = dragging_gizmo_ ? drag_tool_ : effective_tool();
    const Prop* sp = selected_prop_ >= 0 && selected_prop_ < int(doc_.clip().props.size()) ? &doc_.clip().props[selected_prop_] : nullptr;
    bool gizmo_on = place_gizmo();

    if (dragging_gizmo_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            doc_.clip() = doc_.history.cancel();  // back to the value at the press
            gizmo_.end_drag();
            dragging_gizmo_ = false;
            skip_shortcuts_ = true;
            status("Cancelled");
            return;
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            apply_gizmo_drag(m, preset == Preset::SecondLife ? snap_on_ : io.KeyCtrl);
        } else {
            const char* label = tool == Tool::Rotate ? "Rotate" : tool == Tool::Scale ? "Scale" : "Move";
            std::string what = sp ? sp->name : ph ? rig_->limbs()[ph->limb].label + (ph->pole ? " pole" : " IK") : skel_[p].name;
            if (doc_.history.commit(std::string(label) + " " + what, doc_.clip())) {
                mark_dirty();
                status(std::string(tool == Tool::Rotate ? "Rotated " : tool == Tool::Scale ? "Scaled " : "Moved ") + what + " at frame " +
                       std::to_string(int(std::round(frame_))));
            }
            gizmo_.end_drag();
            dragging_gizmo_ = false;
        }
        return;
    }

    gizmo_hover_ = hovered && gizmo_on ? gizmo_.hit(m) : Gizmo::None;
    std::vector<int> ranked;
    hover_handle_ = hovered ? pick_handle(m, hover_handle_pole_) : -1;
    hover_bone_ = hovered && hover_handle_ < 0 ? pick_bone(m, &ranked) : -1;
    // The free-rotate disk only takes the click when no other bone is under the cursor (VP-24).
    if (gizmo_hover_ == Gizmo::Free && hover_bone_ >= 0 && hover_bone_ != p) gizmo_hover_ = Gizmo::None;

    // The world view: a right-click off the bones is the viewer's own (its pie menu).
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && (preset != Preset::Industry || !io.KeyAlt) &&
        (!host_.world_view() || hover_bone_ >= 0)) {
        open_context_menu(hover_bone_);
        return;
    }
    // AM-62: double-clicking a bone toggles its limb's IK, in every preset.
    if (hovered && hover_bone_ >= 0 && gizmo_hover_ == Gizmo::None && ImGui::IsMouseDoubleClicked(0)) {
        int limb = rig_->limb_of_bone(hover_bone_);
        if (limb < 0) return status(skel_[hover_bone_].name + " is not part of an IK limb");
        select(hover_bone_, false);
        run_action("ik_toggle");
        return;
    }
    if (preset == Preset::QAvimator && hovered && ImGui::IsMouseClicked(0) && gizmo_hover_ == Gizmo::None) {
        if (hover_bone_ < 0 && hover_handle_ < 0) {  // empty space: orbit, Shift pan, Alt zoom
            if (!host_.world_view()) nav_button = 0, nav_mode = io.KeyShift ? 1 : io.KeyAlt ? 2 : 0, nav_moved = false;
            return;
        }
        int limb = hover_bone_ >= 0 ? rig_->limb_of_bone(hover_bone_) : -1;
        bool ik_limb = limb >= 0 && limb_states_[limb].ik_on;
        if (hover_bone_ >= 0 && !ik_limb && (io.KeyShift || io.KeyCtrl || io.KeyAlt)) {
            select(hover_bone_, false);
            euler_drag_bone_ = hover_bone_;
            euler_drag_axis_ = io.KeyAlt ? 2 : io.KeyShift ? 1 : 0;
            euler_drag_press_ = m;
            euler_drag_start_ = curve_euler(doc_.clip(), skel_[hover_bone_].name, frame_);
            doc_.history.begin(doc_.clip());
            return;
        }
    }
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ((preset == Preset::Industry || preset == Preset::SecondLife || host_.world_view()) && io.KeyAlt))
        return;
    if (gizmo_hover_ != Gizmo::None) {
        doc_.history.begin(doc_.clip());
        drag_tool_ = tool;
        gizmo_.begin_drag(gizmo_hover_, m);
        dragging_gizmo_ = true;
        capture_edit_start();
        return;
    }
    if (hover_handle_ >= 0) {
        select_handle({hover_handle_, hover_handle_pole_}, io.KeyShift);
    } else if (hover_bone_ >= 0) {
        // A bone of a limb in IK selects the limb's target instead (VP-25).
        int limb = rig_->limb_of_bone(hover_bone_);
        if (limb >= 0 && limb_states_[limb].ik_on) {
            select_handle({limb, false}, io.KeyShift);
            last_click_ = m;
            return;
        }
        int pick = hover_bone_;
        // Clicking the same spot again steps through bones stacked under the cursor (VP-23).
        bool again = std::hypot(m.x - last_click_.x, m.y - last_click_.y) <= 4;
        auto cur = std::find(ranked.begin(), ranked.end(), p);
        if (again && ranked.size() > 1 && cur != ranked.end()) pick = ranked[(cur - ranked.begin() + 1) % ranked.size()];
        select(pick, io.KeyShift);
        status(skel_[pick].name);
        if (preset != Preset::QAvimator && tool == Tool::Rotate && primary() == pick)
            bone_drag_ = pick, bone_drag_started_ = false, bone_drag_press_ = m;  // VP-26
    } else if (int prop = pick_prop(m); prop >= 0) {
        clear_selection();
        selected_prop_ = prop;
        status(doc_.clip().props[prop].name);
    } else if (int actor = pick_actor(m); actor >= 0) {
        activate_actor(actor);  // clicking another actor's body edits it (GR-1)
    } else if (!io.KeyShift) {
        clear_selection();
    }
    last_click_ = m;
    (void)origin;
    (void)size;
}

// The world as the view (the viewer, spec 09 U3): the host draws no scene, so the bones are lines over it.
void App::draw_bone_lines(ImDrawList* dl) const {
    const Shape* sh = shape();
    for (int i = 0; i < skel_.volume_start(); ++i) {
        if (!node_visible(i) || skel_[i].attachment) continue;  // attachment points get their dots below
        Vec3 end = sh ? skel_[i].end.mul(sh->scale[i]) : skel_[i].end;
        if (end.length() < 1e-5) continue;
        double hx, hy, tx, ty;
        if (!projector_.to_screen(globals_[i].pos, hx, hy) || !projector_.to_screen(globals_[i].apply(end), tx, ty)) continue;
        Rgb c = kCategoryColour[int(skel_[i].category)];
        const bool sel = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        if (i == primary()) c = kSelected;
        else if (sel) c = mix(kSelected, c, 0.45f);
        else if (i == hover_bone_) c = mix(c, {1, 1, 1}, 0.5f);
        const ImU32 col = IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), 235);
        const ImVec2 a{float(hx), float(hy)}, b{float(tx), float(ty)};
        const float width = i == primary() ? 3.5f : sel || i == hover_bone_ ? 3.f : 2.f;
        dl->AddLine(a, b, IM_COL32(10, 12, 14, 150), width + 2);  // a dark edge keeps them readable on any backdrop
        dl->AddLine(a, b, col, width);
        dl->AddCircleFilled(a, width + 0.5f, col);
    }
}

void App::draw_viewport() {
    const bool world = host_.world_view();
    ImVec2 origin, size;
    int w = 1, h = 1;
    bool hovered = false;
    if (world) {
        // No panel: the dockspace's empty centre shows the world, and the pointer counts as over the view only
        // where no window, popup or viewer floater is.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);
        origin = central ? central->Pos : vp->WorkPos;
        size = central ? central->Size : vp->WorkSize;
        if (size.x < 8 || size.y < 8) return;
        viewport_max_ = ImVec2(origin.x + size.x, origin.y + size.y);
        const ImVec2 m = ImGui::GetIO().MousePos;
        hovered = m.x >= origin.x && m.y >= origin.y && m.x < viewport_max_.x && m.y < viewport_max_.y &&
                  !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                  !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && host_.pointer_on_world();
        projector_ = host_.projector(origin, size);
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        bool open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (!open) return ImGui::End();

        origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        if (size.x < 8 || size.y < 8) return ImGui::End();
        viewport_max_ = ImVec2(origin.x + size.x, origin.y + size.y);
        ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        w = std::max(1, int(size.x * scale.x)), h = std::max(1, int(size.y * scale.y));

        projector_ = host_.projector(origin, size);

        ImGui::InvisibleButton("##view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                   ImGuiButtonFlags_MouseButtonMiddle);
        hovered = ImGui::IsItemHovered();
    }
    // The view cube takes the pointer while it is over the cube or dragging it.
    ImVec2 vmax(origin.x + size.x, origin.y + size.y);
    const float cube = settings_.view_cube_size;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool over_cube = mp.x >= vmax.x - 8 - cube && mp.x <= vmax.x - 8 && mp.y >= origin.y + 8 && mp.y <= origin.y + 8 + cube;
    { VATS_PROFILE("vp input"); viewport_input(origin, size, hovered && !over_cube && cube_drag_ == 0); }
    viewport_drop_target(origin, size);  // props agent: Inventory drops (VP-83); after hover, before the highlight
    projector_ = host_.projector(origin, size);  // input may have moved the camera

    ImTextureID scene = 0;
    if (!world) { VATS_PROFILE("vp render_scene"); scene = render_scene(w, h); }
    ImDrawList* dl = world ? ImGui::GetBackgroundDrawList() : ImGui::GetWindowDrawList();  // world: behind every panel
    if (scene) dl->AddImage(scene, origin, ImVec2(origin.x + size.x, origin.y + size.y), ImVec2(0, 1), ImVec2(1, 0));

    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    if (world) draw_bone_lines(dl);
    // Attachment points get a dot so they can be seen and clicked (their glyphs are only 4 cm).
    for (int i = skel_.joint_count(); i < skel_.volume_start(); ++i) {
        if (!node_visible(i) && i != hover_bone_) continue;  // a hidden point shows while a drop targets it
        double x, y;
        if (!projector_.to_screen(globals_[i].pos, x, y)) continue;
        bool sel = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        ImU32 c = i == primary() ? IM_COL32(255, 242, 51, 255) : sel ? IM_COL32(255, 200, 90, 255)
                  : i == hover_bone_ ? IM_COL32(220, 255, 225, 255) : ui::kAttachment;
        dl->AddCircleFilled(ImVec2(float(x), float(y)), 4.5f, c);
        dl->AddCircle(ImVec2(float(x), float(y)), 4.5f, IM_COL32(10, 12, 14, 200), 0, 1.2f);
    }
    draw_handles(dl);
    // IO-42: a prop whose mesh is missing is a dashed-looking orange box, so it can still be found and picked.
    for (int k = 0; k < int(doc_.clip().props.size()); ++k) {
        const Prop& p = doc_.clip().props[k];
        if (!p.visible || p.rigged || prop_model(p.path)) continue;
        Xform f = prop_frame(p);
        ImVec2 q[8];
        bool ok = true;
        for (int c = 0; c < 8; ++c) {
            Vec3 corner{(c & 1 ? 0.05 : -0.05), (c & 2 ? 0.05 : -0.05), (c & 4 ? 0.05 : -0.05)};
            double x = 0, y = 0;
            ok = ok && projector_.to_screen(f.apply(corner), x, y);
            q[c] = ImVec2(float(x), float(y));
        }
        if (!ok) continue;
        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        ImU32 c = k == selected_prop_ ? IM_COL32(255, 242, 51, 255) : IM_COL32(255, 150, 90, 220);
        for (auto& e : edges) dl->AddLine(q[e[0]], q[e[1]], c, 1.5f);
    }
    if ((primary() >= 0 || primary_handle() || selected_prop_ >= 0) && gizmo_.visible() && bone_drag_ < 0 &&
        (effective_tool() == Tool::Rotate || effective_tool() == Tool::Move || effective_tool() == Tool::Scale || dragging_gizmo_) &&
        place_gizmo())
        gizmo_.draw(dl, gizmo_hover_);
    if (place_actor_gizmo()) actor_gizmo_.draw(dl, actor_gizmo_hover_);  // GR: placing another actor

    // Hover label.
    if (hovered && (hover_bone_ >= 0 || hover_handle_ >= 0) && gizmo_hover_ == Gizmo::None && !dragging_gizmo_) {
        ImVec2 m = ImGui::GetIO().MousePos;
        std::string label = hover_handle_ >= 0 ? rig_->limbs()[hover_handle_].label + (hover_handle_pole_ ? " IK pole" : " IK")
                                               : skel_[hover_bone_].name;
        const char* name = label.c_str();
        ImVec2 ts = ImGui::CalcTextSize(name), at(m.x + 14, m.y + 22);
        dl->AddRectFilled(ImVec2(at.x - 5, at.y - 3), ImVec2(at.x + ts.x + 5, at.y + ts.y + 3), IM_COL32(12, 13, 16, 220), 4);
        dl->AddText(at, IM_COL32(255, 238, 170, 255), name);
    }

    // Axis marker (VP-66): a 72 px widget 6 px in from the bottom left, drawn back to front.
    {
        const float widget = 72, len = widget * 0.36f;
        ImVec2 c(origin.x + 6 + widget / 2, origin.y + size.y - 6 - widget / 2);
        const char* names[3] = {"X", "Y", "Z"};
        const ImU32 cols[3] = {IM_COL32(242, 77, 77, 255), IM_COL32(102, 230, 89, 255), IM_COL32(89, 140, 255, 255)};
        const Vec3 toward = -camera_.forward();
        int order[3] = {0, 1, 2};
        std::sort(order, order + 3, [&](int a, int b) { return toward[a] < toward[b]; });  // far first
        auto tip = [&](int a, double k) {
            Vec3 axis;
            axis[a] = k;
            return ImVec2(c.x + float(axis.dot(camera_.right()) * len), c.y - float(axis.dot(camera_.up()) * len));
        };
        for (int a = 0; a < 3; ++a) dl->AddLine(c, tip(a, -0.45), (cols[a] & 0x00FFFFFF) | (77u << 24), 2.5f);
        for (int a : order) {
            ImVec2 e = tip(a, 1);
            dl->AddLine(c, e, cols[a], 2.5f);
            dl->AddCircleFilled(e, 7, cols[a]);
            ImVec2 ts = ImGui::CalcTextSize(names[a]);
            dl->AddText(ImVec2(e.x - ts.x / 2, e.y - ts.y / 2), IM_COL32(20, 22, 26, 255), names[a]);
        }
    }
    if (modal_ != Modal::None) {  // the modal readout, bottom left
        ImVec2 at(origin.x + 16, vmax.y - 20 - ImGui::GetFontSize());
        dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), modal_readout_.c_str());
        dl->AddText(at, IM_COL32(255, 255, 255, 255), modal_readout_.c_str());
    }
    draw_view_cube(dl, origin, vmax, hovered);
    dl->PopClipRect();
    draw_prop_sl_popup();
    draw_context_menu();
    if (!world) return ImGui::End();
    // The world view takes the pointer only over what the editor draws, so every other click reaches the world.
    const bool taken = dragging_gizmo_ || bone_drag_ >= 0 || euler_drag_bone_ >= 0 || modal_ != Modal::None ||
                       actor_dragging_ || cube_drag_ != 0 ||
                       (hovered && (hover_bone_ >= 0 || hover_handle_ >= 0 || gizmo_hover_ != Gizmo::None ||
                                    actor_gizmo_hover_ != Gizmo::None || over_cube));
    if (taken) ImGui::SetNextFrameWantCaptureMouse(true);
}

}  // namespace vats
