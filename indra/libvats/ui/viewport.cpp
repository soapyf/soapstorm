// Viewport Avatar Toolset - the 3D view: scene, navigation, picking and gizmo edits.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "widgets.h"
#include "imgui_internal.h"  // the dockspace's central node (world view)
#include "box_select.h"
#include "dock_layout.h"
#include "profile.h"
#include "vats/skeleton.h"
#include "vats/edit.h"
#include "vats/height_variant.h"
#include "vats/fluid_pose.h"
#include "theme.h"

namespace vats {
namespace {

const Rgb kCategoryColour[kCategoryCount] = {{0.95f, 0.62f, 0.25f}, {0.55f, 0.80f, 0.35f}, {0.90f, 0.45f, 0.60f},
                                             {0.45f, 0.65f, 0.95f}, {0.70f, 0.50f, 0.95f}, {0.35f, 0.80f, 0.80f},
                                             {0.80f, 0.80f, 0.80f}, {1.00f, 1.00f, 1.00f}, {0.78f, 0.74f, 0.90f}};
const Rgb kSelected{1.0f, 0.95f, 0.2f};
const Rgb kContact{0.92f, 0.31f, 0.27f};  // in a self-contact finding at this frame (08 SX)

Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

void push_triangle(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c, Rgb col) {
    Vec3 n = (b - a).cross(c - a).normalized();
    for (const Vec3* p : {&a, &b, &c})
        out.push_back({{float(p->x), float(p->y), float(p->z)}, {float(n.x), float(n.y), float(n.z)}, {col.r, col.g, col.b, 1}});
}

void stick_mesh(std::vector<Vec3>& tris, const Vec3& a, const Vec3& b, double radius = 0.008) {
    const Vec3 d = b - a;
    const double len = d.length();
    if (len < 1e-5) return;
    const Vec3 dir = d * (1.0 / len);
    Vec3 u = (std::fabs(dir.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0}).cross(dir).normalized();
    const Vec3 v = dir.cross(u);
    const Vec3 r[4] = {u * radius, v * radius, -u * radius, -v * radius};
    for (int k = 0; k < 4; ++k) {
        int k1 = (k + 1) % 4;
        tris.insert(tris.end(), {a + r[k], b + r[k], b + r[k1], a + r[k], b + r[k1], a + r[k1]});
    }
}

// Handle colours by side (VP-12).
ImU32 side_colour(const LimbInfo& l) {
    if (l.spine) return IM_COL32(140, 230, 115, 255);
    return l.name.find("Left") != std::string::npos ? IM_COL32(89, 166, 255, 255) : IM_COL32(255, 115, 102, 255);
}

// A unit sphere as a triangle list, counter-clockwise from outside.
const std::vector<Vec3>& unit_sphere() {
    static const std::vector<Vec3> tris = [] {
        constexpr int kRings = 14, kSides = 24;  // fine enough for a smooth rim
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

// Collision volumes as shells: see-through ellipsoids at SL's size and place (volume_shell), clearest at their rims as a
// glass bubble is, so the body reads through them and they read as bodies rather than more bones (VP-10, SK-I5). The
// hovered and selected ones are stronger.
void App::draw_collision_volumes(std::vector<Vertex>& verts) {
    verts.clear();
    const Shape* sh = shape();
    const Rgb base = scene_colours().shell;
    const Vec3 eye = camera_.eye(), fwd = camera_.forward();
    for (const CollisionVolume& v : skel_.volumes()) {
        if (!node_visible(v.node)) continue;
        const VolumeShell s = volume_shell(globals_, sh, v);
        const bool sel = std::find(selection_.begin(), selection_.end(), v.node) != selection_.end();
        const Rgb c = v.node == primary() ? kSelected : sel ? mix(kSelected, base, 0.45f) : hot(v.node) ? mix(base, {1, 1, 1}, 0.45f) : base;
        const float face = sel ? 0.16f : hot(v.node) ? 0.12f : 0.06f, rim = sel ? 0.6f : hot(v.node) ? 0.55f : 0.4f;
        for (const Vec3& u : unit_sphere()) {
            const Vec3 p = s.frame.apply(u.mul(s.axes));
            const Vec3 n = s.frame.rot.rotate(Vec3{u.x / s.axes.x, u.y / s.axes.y, u.z / s.axes.z}).normalized();
            const Vec3 view = camera_.ortho ? fwd : (p - eye).normalized();
            const double edge = 1 - std::fabs(n.dot(view));
            const float a = face + rim * float(edge * edge);
            verts.push_back({{float(p.x), float(p.y), float(p.z)}, {float(n.x), float(n.y), float(n.z)}, {c.r, c.g, c.b, a}});
        }
    }
}

int App::glow_joint() const {
    if (body_ == Body::SkeletonOnly) return -1;
    if (painting()) {  // 08 RG-15: the bone being painted, whatever the pointer is over
        const int n = primary();
        return n >= 0 && (n < skel_.joint_count() || skel_[n].volume) ? n : -1;
    }
    if (!settings_.show_weights) return -1;
    const int n = hover_bone_ >= 0 ? hover_bone_ : primary();
    return n >= 0 && (n < skel_.joint_count() || skel_[n].volume) ? n : -1;
}

// A weight as a colour, blue through green and yellow to red as in a weight-paint view, laid over the skin the more the
// joint carries it; none at all where it carries nothing.
const std::vector<float>* App::weight_heat(const std::string& part) {
    const int n = glow_joint();
    if (n < 0) return nullptr;
    const MeshBody* b = mesh_body();
    const DaeModel* m = b ? prop_model(part) : nullptr;
    if (b ? !m || !m->rigged : !part.empty()) return nullptr;
    const std::string key = std::to_string(n) + "|" + std::to_string(int(body_)) + "|" + (b ? b->id : "") + "|" +
                            std::to_string(weights_generation_);
    if (key != heat_key_) heat_.clear(), heat_key_ = key;
    auto [it, fresh] = heat_.try_emplace(part);
    if (!fresh) return &it->second;
    std::vector<float> w;  // the joint's weight per vertex
    if (m) {
        int sk40 = n;
        if (skel_[n].volume)
            for (size_t v = 0; v < skel_.volumes().size(); ++v)
                if (skel_.volumes()[v].node == n) sk40 = dae_volume(skel_, int(v));
        w.assign(m->positions.size() / 3, 0.f);
        for (size_t i = 0; i < m->joints.size() && i < m->weights.size() && i / 4 < w.size(); ++i)
            if (m->joints[i] == sk40) w[i / 4] += m->weights[i];
    } else {
        for (const Influence& f : mesh_.influences())
            w.push_back((f.a == n ? 1 - f.blend : 0.f) + (f.b == n ? f.blend : 0.f));
    }
    static const Rgb kRamp[] = {{0.20f, 0.40f, 1.00f}, {0.15f, 0.85f, 0.95f}, {0.30f, 0.90f, 0.35f},
                                {1.00f, 0.88f, 0.25f}, {1.00f, 0.30f, 0.22f}};
    std::vector<float>& out = it->second;
    out.assign(w.size() * 4, 0.f);
    for (size_t v = 0; v < w.size(); ++v) {
        const float t = std::clamp(w[v], 0.f, 1.f);
        if (t < 0.004f) continue;
        const int k = std::min(int(t * 4), 3);
        const Rgb c = mix(kRamp[k], kRamp[k + 1], t * 4 - float(k));
        out[v * 4] = c.r, out[v * 4 + 1] = c.g, out[v * 4 + 2] = c.b, out[v * 4 + 3] = 0.4f + 0.5f * t;
    }
    return &out;
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
    if (view) invalidate_floor_cache();
    verts.resize(pos.size() / 3);
    const std::vector<float>* heat = view ? weight_heat("") : nullptr;
    for (auto& part : mesh_.parts()) {
        const Rgb base = part.material == Material::Eye ? colours.eye : colours.body;
        for (std::uint32_t i = part.first_vertex; i < part.first_vertex + part.vertex_count; ++i) {
            Rgb c = base;
            if (heat && heat->size() >= (i + 1) * 4u)
                c = mix(c, {(*heat)[i * 4], (*heat)[i * 4 + 1], (*heat)[i * 4 + 2]}, (*heat)[i * 4 + 3]);
            verts[i] = {{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}, {nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]},
                        {c.r, c.g, c.b, 1}};
        }
    }
    scene_triangles(verts, skin_idx, true, 0.04f);  // App::scene_triangles: the Picker's render gathers them
    scene_triangles(verts, eye_idx, true, 0.6f);
}

// The onion ghosts while onion skin is on and, while Filter Curves is open (08 MC-4a), the pose before filtering
// at the current frame (offset 0). Nothing is evaluated while playing.
std::vector<OnionGhost> App::ghost_poses() {
    std::vector<OnionGhost> out;
    if (playing_ || !rig_) return out;
    if (const OnionView v = onion_view(); v.on) out = onion_ghosts(*rig_, doc_.clip(), frame_, shape(), v.s);
    if (const Clip* was = graph_.filter_original())
        out.push_back({{frame_, 0, 1.f}, vats::evaluate(*rig_, *was, frame_, shape()).globals});
    return out;
}

// Onion skin (08 ON-1..3): ghosts of the pose at nearby frames, earlier ones cool, later ones warm, fading
// with distance; the pre-filter ghost is grey. Drawn see-through and never picked.
void App::draw_onion(const SceneColours& colours) {
    const OnionView v = onion_view();
    const bool bones = v.bones_only || (body_ == Body::SkeletonOnly && !mesh_body());
    for (const auto& g : pinned_ghost_poses())  // pinned ghosts (ON-5): violet, whether onion skin is on or not
        draw_ghost(g, mix({0.72f, 0.42f, 1.0f}, colours.body, 0.2f), 0.35f, bones);
    const auto ghosts = ghost_poses();
    if (ghosts.empty()) return;
    const Rgb cool = mix({0.35f, 0.62f, 1.0f}, colours.body, 0.2f), warm = mix({1.0f, 0.58f, 0.28f}, colours.body, 0.2f);
    const Rgb grey = mix({0.85f, 0.85f, 0.85f}, colours.body, 0.2f);
    for (auto it = ghosts.rbegin(); it != ghosts.rend(); ++it)  // farthest first
        draw_ghost(it->globals, it->at.offset < 0 ? cool : it->at.offset > 0 ? warm : grey,
                   0.12f + 0.28f * it->at.weight, bones);
}

// One see-through copy of the body (or its bones) in a pose: the onion ghosts, the pre-filter ghost and the SL
// preview's original (08 SP-2).
void App::draw_ghost(const std::vector<Xform>& globals, const Rgb& c, float alpha, bool bones) {
    static std::vector<Vertex> verts;
    static std::vector<std::uint32_t> idx;
    static std::vector<float> pos, nrm;
    const Shape* sh = shape();
    if (bones) {
        verts.clear();
        std::vector<bool> shown(static_cast<size_t>(skel_.size()));
        for (int i = 0; i < skel_.joint_count(); ++i) shown[i] = node_visible(i);
        const auto segs = stick_segments(skel_, globals, sh, shown);
        static std::vector<Vec3> tris;
        tris.clear();
        for (const auto& s : segs) stick_mesh(tris, s.a, s.b, 0.008);
        for (size_t k = 0; k + 2 < tris.size(); k += 3)
            push_triangle(verts, tris[k], tris[k + 1], tris[k + 2], c);
        for (Vertex& vx : verts) vx.c[3] = alpha;
        host_.scene_triangles(verts, {}, true, 0.1f, true);
    } else if (const MeshBody* mb = mesh_body()) {
        const float tint[4] = {c.r, c.g, c.b, alpha};
        for (const std::string& path : mb->parts) {
            Prop part;
            part.path = path;
            part.rigged = true;
            draw_prop(part, verts, idx, &globals, sh, 1.f, {}, tint);
        }
    } else {
        mesh_.skin(globals, sh, pos, nrm);
        verts.resize(pos.size() / 3);
        for (size_t i = 0; i < verts.size(); ++i)
            verts[i] = {{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}, {nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]},
                        {c.r, c.g, c.b, alpha}};
        host_.scene_triangles(verts, mesh_.indices(), true, 0.05f, true);
    }
}

bool App::bones_hidden() const {
    if (!bone_style_run_.empty()) return bone_style_run_ == "hidden";
    return settings_.bone_style == "hidden";
}

bool App::stick_bones() const {
    return !bones_hidden();
}

std::vector<StickSegment> App::sticks() const {
    std::vector<bool> shown(static_cast<size_t>(skel_.size()));
    for (int i = 0; i < skel_.joint_count(); ++i) shown[i] = node_visible(i);
    return stick_segments(skel_, globals_, shape(), shown);
}

// Stick bones: lines and dots over the picture, so they show through the body in both hosts. Farthest first.
void App::draw_stick_bones(ImDrawList* dl) {
    if (globals_.empty()) return;
    const std::vector<StickSegment> segs = sticks();
    auto colour = [&](int i, bool& strong) {
        Rgb c = kCategoryColour[int(skel_[i].category)];
        planner_colour(i, c);  // 08 PP-2
        const bool sel = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        strong = sel || hot(i);
        if (i == primary()) c = kSelected;
        else if (sel) c = mix(kSelected, c, 0.45f);
        else if (contact_bone(i)) c = kContact;
        else if (hot(i)) c = mix(c, {1, 1, 1}, 0.6f);
        return IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), 255);
    };
    const ImU32 dark = IM_COL32(10, 12, 14, 170);
    struct Item {
        double depth;
        int seg, node;  // a segment, or a joint's dot (seg -1)
    };
    static std::vector<Item> items;
    items.clear();
    const Vec3 fwd = camera_.forward();
    for (int k = 0; k < int(segs.size()); ++k) items.push_back({-(segs[k].a + segs[k].b).dot(fwd) * 0.5, k, segs[k].node});
    for (int i = 0; i < skel_.joint_count(); ++i)
        if (node_visible(i)) items.push_back({-globals_[i].pos.dot(fwd) + 1e-4, -1, i});  // a dot just over its own lines
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth < b.depth; });
    for (const Item& it : items) {
        bool strong = false;
        const ImU32 c = colour(it.node, strong);
        double ax, ay, bx, by;
        if (it.seg < 0) {
            if (!projector_.to_screen(globals_[it.node].pos, ax, ay)) continue;
            int fold = 0;
            for (int j = 0; j < it.node; ++j) {
                if (node_visible(j) && (globals_[j].pos - globals_[it.node].pos).length() < 0.002)
                    ++fold;
            }
            const float r = strong ? 4.5f : 3.2f;
            if (fold > 0) {
                const float ring_r = r + fold * 3.5f;
                dl->AddCircle(ImVec2(float(ax), float(ay)), ring_r, dark, 0, strong ? 3.5f : 2.4f);
                dl->AddCircle(ImVec2(float(ax), float(ay)), ring_r, c, 0, strong ? 2.0f : 1.2f);
            } else {
                dl->AddCircleFilled(ImVec2(float(ax), float(ay)), r + 1.2f, dark);
                dl->AddCircleFilled(ImVec2(float(ax), float(ay)), r, c);
            }
        } else if (projector_.to_screen(segs[it.seg].a, ax, ay) && projector_.to_screen(segs[it.seg].b, bx, by)) {
            const ImVec2 a{float(ax), float(ay)}, b{float(bx), float(by)};
            dl->AddLine(a, b, dark, strong ? 5.f : 3.6f);
            dl->AddLine(a, b, c, strong ? 3.f : 1.8f);
        }
    }

    // Selected bone roll tick along bone's local X axis at its head.
    const int p = primary();
    if (p >= 0 && p < skel_.joint_count() && node_visible(p)) {
        double hx, hy, tx, ty;
        const Vec3 head = globals_[p].pos;
        const Vec3 roll_dir = local_axes(p).rotate({1, 0, 0});
        if (projector_.to_screen(head, hx, hy) && projector_.to_screen(head + roll_dir * 0.05, tx, ty)) {
            const double dx = tx - hx, dy = ty - hy;
            const double len = std::hypot(dx, dy);
            if (len > 1e-4) {
                const ImVec2 h{float(hx), float(hy)};
                const ImVec2 t{float(hx + (dx / len) * 16.0), float(hy + (dy / len) * 16.0)};
                dl->AddLine(h, t, dark, 3.6f);
                dl->AddLine(h, t, IM_COL32(255, 90, 80, 255), 2.0f);
                dl->AddCircleFilled(t, 2.0f, IM_COL32(255, 90, 80, 255));
            }
        }
    }
}

ImTextureID App::render_scene(int w, int h) {
    const SceneColours& colours = scene_colours();
    { VATS_PROFILE("vp begin+ground");
    if (!host_.scene_begin(ui::SceneTarget::View, w, h, camera_, colours)) return ImTextureID{};  // the viewer: the world is the view
    draw_reference(true, double(w) / h);  // 08 RF: the backdrop, behind everything
    host_.scene_ground({globals_.empty() ? 0 : globals_[0].pos.x, globals_.empty() ? 0 : globals_[0].pos.y, ground_shown()}); }

    static std::vector<Vertex> prop_verts;
    static std::vector<std::uint32_t> prop_indices;
    {
        VATS_PROFILE("vp body");
        if (mesh_body()) draw_mesh_body(prop_verts, prop_indices);  // replaces the Linden mesh (BD-2)
        else if (body_ != Body::SkeletonOnly) draw_avatar(true, globals_, colours);
    }
    { VATS_PROFILE("vp other actors"); draw_other_actors(colours); }  // couples and groups (GR-1)
    { VATS_PROFILE("vp onion"); draw_onion(colours); }  // ghosts (08 ON)
    draw_target(colours);  // the target ghost, to match by eye
    if (!sl_ghost_.empty())  // the SL preview's original (08 SP-2): green, bones when the onion ghosts are bones
        draw_ghost(sl_ghost_, mix({0.4f, 0.9f, 0.55f}, colours.body, 0.2f), 0.3f,
                   onion_view().bones_only || (body_ == Body::SkeletonOnly && !mesh_body()));
    { VATS_PROFILE("vp props"); draw_props(prop_verts, prop_indices); }
    draw_treadmill();  // 08 LP-8
    draw_backdrop();   // 08 LT-2
    draw_reference(false, double(w) / h);  // 08 RF: the plane in the scene

    VATS_PROFILE("vp volumes+bones+end");
    static std::vector<Vertex> volumes;
    draw_collision_volumes(volumes);
    host_.scene_triangles(volumes, {}, !xray_, 0.1f, true);

    // Other actors shown as a skeleton (GR): the body bones only, dimmed towards their colour.
    static std::vector<Vertex> bones;
    bones.clear();
    for (const OtherSkeleton& s : other_skeletons_) {
        static std::vector<Vec3> tris;
        tris.clear();
        for (const auto& seg : other_sticks(s)) stick_mesh(tris, seg.a, seg.b, 0.008);
        Rgb c = mix(kCategoryColour[0], {s.colour[0], s.colour[1], s.colour[2]}, 0.5f);
        c = {c.r * 0.7f, c.g * 0.7f, c.b * 0.7f};
        for (size_t k = 0; k + 2 < tris.size(); k += 3)
            push_triangle(bones, tris[k], tris[k + 1], tris[k + 2], c);
    }
    host_.scene_triangles(bones, {}, true, 0.25f);
    return host_.scene_end();
}

std::vector<StickSegment> App::other_sticks(const OtherSkeleton& s) const {
    std::vector<bool> shown(static_cast<size_t>(skel_.size()));
    for (int i = 0; i < skel_.joint_count(); ++i) shown[i] = node_shown(i);
    return stick_segments(skel_, s.globals, actor_shape(s.actor), shown);
}

int App::pick_bone(ImVec2 m, std::vector<int>* ranked) const { return pick_node(m, ranked, false); }

// with_points: attachment points count even while hidden (Inventory drops, VP-83).
int App::pick_node(ImVec2 m, std::vector<int>* ranked, bool with_points) const {
    std::vector<bool> shown(static_cast<size_t>(skel_.size()));
    for (int i = 0; i < skel_.size(); ++i)  // volumes are picked by their shells below
        shown[i] = !skel_[i].volume && (node_visible(i) || (with_points && skel_[i].attachment));
    std::vector<int> hits = pick_sticks(
        skel_, globals_, shape(), shown, !bones_hidden(),
        [&](const Vec3& p, double& x, double& y) { return projector_.to_screen(p, x, y); }, m.x, m.y);
    // The shells under the pointer, nearest first, after every stick, dot and point: those are drawn over them. Without
    // X-ray the body hides a shell behind its skin, as it is drawn.
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    std::vector<std::pair<double, int>> shells;
    for (const CollisionVolume& v : skel_.volumes())
        if (node_visible(v.node))
            if (const double t = ray_shell(o, d, volume_shell(globals_, shape(), v)); t < 1e30) shells.emplace_back(t, v.node);
    if (!shells.empty() && !xray_) {
        double skin = 1e30;
        if (body_ != Body::SkeletonOnly) pick_mesh_bone(m, &skin);
        std::erase_if(shells, [&](const auto& s) { return s.first > skin; });
    }
    std::sort(shells.begin(), shells.end());
    for (const auto& s : shells) hits.push_back(s.second);
    if (ranked) *ranked = hits;
    return hits.empty() ? -1 : hits.front();
}

// 08 FP-2: the bone that owns the skin point under m, from skin weights at the hit triangle.
bool App::body_point_under(ImVec2 m, Vec3& out) const {
    // The body as drawn: in the world view the swapped body in your live pose (as the viewer shows it), else the
    // editor's own. Every triangle counts, whatever bone it belongs to.
    const bool world = host_.world_view();
    const MeshBody* b = mesh_body();  // the body draw_mesh_body draws, in either view
    if (!b) return false;
    const auto& skins = world ? live_body_skin_pos_ : mesh_body_skin_pos_;
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    double best = 1e30;
    for (const std::string& path : b->parts) {
        const auto mdl = prop_models_.find(path);
        const auto pos = skins.find(path);
        if (mdl == prop_models_.end() || !mdl->second || pos == skins.end() || pos->second.size() != mdl->second->positions.size())
            continue;
        const SurfaceHit h = ray_surface(o, d, pos->second, mdl->second->indices);
        if (h.triangle >= 0 && h.t < best) best = h.t;
    }
    if (best >= 1e30) return false;
    out = o + d * best;
    return true;
}

int App::pick_mesh_bone(ImVec2 m, double* out_t) const {
    VATS_PROFILE("pick mesh bone (ray vs body)");
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    double best_t = 1e30;
    int best_joint = -1;

    if (const MeshBody* b = mesh_body()) {
        for (const std::string& path : b->parts) {
            auto it = prop_models_.find(path);
            const DaeModel* mdl = it != prop_models_.end() ? it->second.get() : nullptr;
            if (!mdl || !mdl->rigged) continue;
            auto sit = mesh_body_skin_pos_.find(path);
            if (sit == mesh_body_skin_pos_.end() || sit->second.empty()) continue;
            SurfaceHit h = ray_surface(o, d, sit->second, mdl->indices);
            if (h.triangle >= 0 && h.t < best_t) {
                const int j = surface_joint(skel_, *mdl, h);
                if (j >= 0) {
                    best_t = h.t;
                    best_joint = j;
                }
            }
        }
    } else if (body_ != Body::SkeletonOnly && !skin_pos_.empty()) {
        SurfaceHit h = ray_surface(o, d, skin_pos_, mesh_.indices());
        if (h.triangle >= 0 && h.t < best_t) {
            const int j = surface_joint(skel_, mesh_, h);
            if (j >= 0) {
                best_t = h.t;
                best_joint = j;
            }
        }
    }

    if (out_t) *out_t = best_t;
    return best_joint;
}

bool App::pick_surface(ImVec2 m, Vec3& point) const {
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    double best = 1e30;
    if (body_ != Body::SkeletonOnly) pick_mesh_bone(m, &best);  // the body shown: a mesh body in its place too
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

// The avatar as SL's camera sees it (slcam::Focus): a box at the agent, the middle of the body height, whatever the
// animation does to the body, as LLVOAvatar's position and scale are.
slcam::Focus App::sl_avatar_focus(int actor) const {
    slcam::Focus f;
    f.size.z = avatar_height(skel_, actor < 0 ? shape() : actor_shape(actor)) - kShapeEditorExtra;  // mBodySize.z
    Xform place;  // the view is the edited actor's space
    const Project& p = doc_.project;
    if (actor >= 0 && actor < int(p.actors.size()) && p.active >= 0 && p.active < int(p.actors.size()))
        place = p.actors[p.active].placement().inverse() * p.actors[actor].placement();
    f.avatar = {place.rot, place.apply({0, 0, f.size.z / 2})};
    return f;
}

double App::ray_prop(int i, const Vec3& o, const Vec3& d) const {
    const Prop& p = doc_.clip().props[i];
    auto it = prop_models_.find(p.path);
    const DaeModel* mdl = it != prop_models_.end() ? it->second.get() : nullptr;
    if (!p.visible || !mdl) return 1e30;
    if (p.rigged) {
        std::vector<float> pos, nrm;
        skin_prop(*mdl, skel_, globals_, shape(), pos, nrm);
        return ray_triangles(o, d, pos, mdl->indices);
    }
    // Into the mesh's own space, undoing what draw_prop does: re-centred on its box, scaled, then placed (VP-81).
    // The map is affine, so the ray parameter of the hit is the same in both spaces.
    const Xform inv = prop_frame(p).inverse();
    const Vec3 c = (mdl->bounds_min + mdl->bounds_max) * 0.5;
    auto unscale = [&](const Vec3& v) { return Vec3{v.x / p.scale.x, v.y / p.scale.y, v.z / p.scale.z}; };
    return ray_triangles(unscale(inv.apply(o)) + c, unscale(inv.rot.rotate(d)), mdl->positions, mdl->indices);
}

// Second Life's Alt press: LLToolCamera::handleMouseDown picks under the cursor and pickCallback puts the focus on what
// was hit (lltoolfocus.cpp:117-303), with Ctrl and Shift too. The focus is the surface point itself: for an avatar
// that is LLAgentCamera::calcFocusOffset's "don't do any funk heuristics" (llagentcamera.cpp:456-466). SL tests an
// avatar's collision volumes (LLVOAvatar::lineSegmentIntersect); VATs tests the mesh drawn, so the focus lands on the
// skin you clicked. Nothing hit, the sky: "invalid point", the focus stays and the drag moves nothing
// (lltoolfocus.cpp:205-210, 412-420).
bool App::sl_focus_at(ImVec2 m) {
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    double best = 1e30;
    slcam::Focus f = sl_avatar_focus(-1);
    if (body_ != Body::SkeletonOnly) pick_mesh_bone(m, &best);  // the body shown: a mesh body in its place too
    for (size_t i = 0; i < actor_pick_pos_.size(); ++i)  // other actors (GR)
        if (actor_pick_idx_[i])
            if (double t = ray_triangles(o, d, actor_pick_pos_[i], *actor_pick_idx_[i]); t < best)
                best = t, f = sl_avatar_focus(int(i));
    const auto& props = doc_.clip().props;
    for (int i = 0; i < int(props.size()); ++i)
        if (double t = ray_prop(i, o, d); t < best) {
            best = t;
            f = sl_avatar_focus(-1);
            // Worn (rigged, on a bone or a point) is an attachment: SL focuses on its avatar (llagentcamera.cpp:3171-3179).
            if (!props[i].rigged && props[i].bone.empty() && props[i].point.empty()) f.kind = slcam::Focus::Object;
        }
    if (best >= 1e30)  // skeleton only: the bones are the body; the point of the bone nearest the ray
        if (int b = pick_bone(m); b >= 0) {
            const Shape* sh = shape();
            const Vec3 a = globals_[b].pos, u = bone_tail(skel_, globals_, sh, b) - a;
            const Vec3 w = a - o;
            const double du = d.dot(u), den = u.dot(u) - du * du;
            const double s = den > 1e-12 ? std::clamp((d.dot(w) * du - w.dot(u)) / den, 0.0, 1.0) : 0.0;
            best = std::max(1e-3, d.dot(a + u * s - o));
        }
    // The ground, as far as the stock pick reaches (512 m, lltoolfocus.cpp:171).
    if (best >= 1e30 && d.z < -1e-6 && -o.z / d.z <= 512) best = -o.z / d.z, f.kind = slcam::Focus::Land;
    if (best >= 1e30) return false;
    sl_focus_ = f;
    focus_camera_on(o + d * best);
    return true;
}

// LLToolCamera::handleHover (lltoolfocus.cpp:384-505). Nothing moves until the pointer has gone SLOP_RANGE (4) pixels
// along an axis; then Ctrl orbits (MASK_ORBIT, lltoolmgr.h:39), Ctrl+Shift pans (MASK_PAN, :40) and anything else,
// plain Alt included, is the zoom tool: sideways orbits, up and down zooms. The modifiers count as they are now, not
// as they were at the press. SL's mouse deltas are in GL coordinates (y up), hence dy = -delta.y.
void App::sl_camera_drag(ImVec2 delta, float view_width) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!sl_valid_click_) return;
    constexpr float kSlopRange = 4;  // lltoolfocus.cpp:65
    sl_accum_x_ += std::fabs(delta.x), sl_accum_y_ += std::fabs(delta.y);
    sl_outside_slop_x_ = sl_outside_slop_x_ || sl_accum_x_ >= kSlopRange;
    sl_outside_slop_y_ = sl_outside_slop_y_ || sl_accum_y_ >= kSlopRange;
    if (!sl_outside_slop_x_ && !sl_outside_slop_y_) return;
    const double dx = delta.x, dy = -delta.y;
    const double radians_per_pixel = 2 * kPi / std::max(1.f, view_width);  // 360 degrees across the view
    cam_glide_.apply_input(camera_, [&](Camera& cam) {
        if (io.KeyCtrl && !io.KeyShift) {
            if (dx != 0) slcam::orbit_around(cam, sl_focus_, -dx * radians_per_pixel);
            if (dy != 0) slcam::orbit_over(cam, sl_focus_, -dy * radians_per_pixel);
        } else if (io.KeyCtrl && io.KeyShift) {
            // "Fudge factor for pan": 3 x the distance to the focus across the view.
            const double meters_per_pixel = 3 * (cam.eye() - cam.target).length() / std::max(1.f, view_width);
            slcam::pan(cam, sl_focus_, dx * meters_per_pixel, -dy * meters_per_pixel);
        } else {
            if (dx != 0) slcam::orbit_around(cam, sl_focus_, -dx * radians_per_pixel);
            if (dy != 0 && sl_outside_slop_y_) slcam::zoom_in(cam, sl_focus_, std::pow(0.99, dy));  // IN_FACTOR
        }
    });
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
        // From the keys, not globals_: those carry the previews (a follow-through still settling), which the edit would
        // key into the bone.
        const Evaluation keyed = vats::evaluate(*rig_, clip, frame_, shape(), constraints());
        drag_start_global_ = keyed.globals[p];
        drag_parent_global_ = skel_[p].parent >= 0 ? keyed.globals[skel_[p].parent] : Xform{};
        drag_start_offset_ = keyed.pose.offset[p];
        drag_start_euler_ = curve_euler(clip, skel_[p].name, frame_);
    }
    auto_ik_.on = false;
    body_drag_on_ = false;
    follow_through_.reset();
    if (!sp && !ph && p >= 0 && drag_tool_ == Tool::Move) {
        if (settings_.auto_ik && body_drag_joint(skel_, p)) {
            // FP-3: plant only legs the shown body has, by its skin when it is a mesh body.
            BodyGround ground{[this](int node) { return is_joint_weighted(node); }, {}};
            if (const MeshBody* b = body_ != Body::SkeletonOnly ? mesh_body() : nullptr)
                for (const std::string& path : b->parts)
                    if (const DaeModel* m = prop_model(path); m && m->rigged) ground.mesh.push_back(m);
            body_drag_ = begin_body_drag(*rig_, doc_.clip(), frame_, p, shape(), ground);
            body_drag_on_ = true;
        } else if (auto_ik_applies(p)) {
            auto_ik_begin(p);  // 08 AI-1
        }
    }
}

// Spec 08 AI: Auto IK. The chain for node as it stands now, with the length last chosen for it in this run.
bool App::auto_ik_applies(int node) const {
    if (!settings_.auto_ik || node < 0 || !rig_) return false;
    auto len = auto_ik_length_.find(node);
    return !auto_ik_chain(*rig_, doc_.clip(), frame_, node, len == auto_ik_length_.end() ? 0 : len->second).bones.empty();
}

void App::auto_ik_begin(int node) {
    auto len = auto_ik_length_.find(node);
    auto_ik_.node = node;
    auto_ik_set_chain(len == auto_ik_length_.end() ? 0 : len->second);
    auto_ik_.on = !auto_ik_.chain.bones.empty();
    if (auto_ik_.on) auto_ik_status();
}

// The chain from the pose as it stands. A drag that pressed the body pulls the point pressed: the dragged bone turns
// too, so a foot's tip goes where the pointer takes it instead of the ankle.
void App::auto_ik_set_chain(int len) {
    auto_ik_.chain = auto_ik_chain(*rig_, doc_.clip(), frame_, auto_ik_.node, len);
    auto_ik_.start = vats::evaluate(*rig_, doc_.clip(), frame_, shape());
    const Xform& g = auto_ik_.start.globals[auto_ik_.node];
    auto_ik_.from = g.pos;
    if (auto_ik_.grab && dot_drag_ >= 0 && !auto_ik_.chain.bones.empty() && (*auto_ik_.grab - g.pos).length() > 1e-3) {
        auto_ik_.chain.bones.push_back(auto_ik_.node);
        ++auto_ik_.chain.turning;
        auto_ik_.chain.grab_on = true;
        auto_ik_.chain.grab = g.inverse().apply(*auto_ik_.grab);
        auto_ik_.from = *auto_ik_.grab;
    }
}

void App::auto_ik_status() {
    const AutoIkChain& c = auto_ik_.chain;
    if (c.bones.empty()) return;
    std::string s = "Auto IK: " + skel_[auto_ik_.node].name + " pulls " + std::to_string(c.turning) +
                    (c.turning == 1 ? " bone" : " bones") + ", from " + skel_[c.bones.front()].name;
    const int above = c.turning - c.grab_on;  // the length chosen: a grabbed bone turns too but is not counted in it
    if (c.longest > 1) s += "   Wheel or [ ]: " + std::string(above < c.longest ? "more" : "") +
                            (above < c.longest && above > 1 ? " or " : "") + (above > 1 ? "fewer" : "") + " bones";
    status(s);
}

// AI-3: during an Auto IK drag the wheel and ] take one more bone up the chain, [ one fewer. The drag starts over from
// the press with the new chain, so bones it no longer takes go back to their keys.
bool App::auto_ik_input() {
    const bool dragging = dragging_gizmo_ || modal_ == Modal::Move || dot_drag_started_;
    if (!auto_ik_.on || !dragging) return false;
    const ImGuiIO& io = ImGui::GetIO();
    int step = viewport_hovered_ && io.MouseWheel != 0 ? (io.MouseWheel > 0 ? 1 : -1) : 0;
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) step = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) step = -1;
    if (step == 0) return false;
    skip_shortcuts_ = true;  // [ and ] also walk the selection
    const int above = auto_ik_.chain.turning - auto_ik_.chain.grab_on;  // the grabbed bone is not part of the length
    const int want = std::clamp(above + step, 1, std::max(1, auto_ik_.chain.longest));
    if (want != above) {
        auto_ik_length_[auto_ik_.node] = want;
        doc_.clip() = doc_.history.cancel();  // the old chain's keys go; the drag keys the new one from the press
        doc_.history.begin(doc_.clip());
        auto_ik_set_chain(want);  // from the press's pose again
    }
    auto_ik_status();
    return true;
}

// A joint's dot: the head of the hovered or primary bone, where a press drags it by Auto IK with any tool.
int App::pick_dot(ImVec2 m) const {
    if (!settings_.auto_ik) return -1;
    for (int c : {hover_bone_, primary()}) {
        double x, y;
        if (c < 0 || c >= skel_.joint_count() || !node_visible(c) || !projector_.to_screen(globals_[c].pos, x, y) ||
            std::hypot(m.x - x, m.y - y) > 7 || (!auto_ik_applies(c) && !body_drag_joint(skel_, c)))
            continue;
        return c;
    }
    return -1;
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
            ClampReport rep;
            key_limb_target(clip, *rig_, frame_, h->limb, target, shape(), constraints(), &rep);
            if (!rep.empty()) report_limit_clamp(rep);
        }
        mirror_edit({"ik." + rig_->limbs()[h->limb].name});  // PT-1
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
    if (body_drag_on_ && body_drag_.node == p && drag_tool_ == Tool::Move) {
        const bool plant = !ImGui::GetIO().KeyAlt;
        ClampReport rep;
        // Not mirrored: the drag solves each planted leg itself, and live Mirror would square the hips (Centre in
        // place) or copy a planted leg onto a free one, pulling feet off their spots.
        key_body_drag(clip, *rig_, frame_, body_drag_, drag_start_global_.pos + t, plant, shape(), constraints(), &rep);
        if (!rep.empty()) report_limit_clamp(rep);
        return;
    }
    if (auto_ik_.on && auto_ik_.node == p && drag_tool_ == Tool::Move) {  // 08 AI: the bones above follow
        // Each step from the last: the chain follows the pointer's path (08 AI-4).
        ClampReport rep;
        mirror_edit(key_auto_ik(clip, *rig_, frame_, auto_ik_.chain, auto_ik_.start, auto_ik_.from + t, shape(),
                                &auto_ik_.start.pose, constraints(), &rep));
        if (!rep.empty()) report_limit_clamp(rep);
        return;
    }
    if (gimbal_axis >= 0) {
        Vec3 e = drag_start_euler_;  // one channel changes, the other two stay exactly as keyed
        e[gimbal_axis] += gimbal_angle * kRadToDeg;
        Quat local = euler_to_quat(e * kDegToRad);
        if (const RigConstraints* rc = constraints()) {
            if (const JointLimit* lim = rc->find(n.name)) {
                ClampedJoint c;
                if (check_joint_clamp(n.name, p, *lim, local, shape(), c)) {
                    report_limit_clamp(c);
                }
                local = clamp_joint_rotation(*lim, local, shape(), p);
                e = nearest_euler(local, drag_start_euler_);
            }
        }
        key_euler(clip, n.name, frame_, e);
    } else if (drag_tool_ == Tool::Rotate) {
        // New global rotation, expressed back in the bone's frame after its parent and rest rotation.
        Quat global = (r * drag_start_global_.rot).normalized();
        Quat local = (n.rest.conj() * drag_parent_global_.rot.conj() * global).normalized();
        if (const RigConstraints* rc = constraints()) {
            if (const JointLimit* lim = rc->find(n.name)) {
                ClampedJoint c;
                if (check_joint_clamp(n.name, p, *lim, local, shape(), c)) {
                    report_limit_clamp(c);
                }
                local = clamp_joint_rotation(*lim, local, shape(), p);
            }
        }
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
    mirror_edit({n.name});  // PT-1
}

// Places the gizmo on the selection for the current camera; false when there is none. Runs again just before
// drawing, so a camera that moved during input (a navigation drag returns early) never leaves it behind.
bool App::place_gizmo() {
    // Rig from Scratch holds the rest pose to place joints: a drag there places (markers, pins), it never poses.
    if (rig_scratch_holds_rest()) return false;
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

// The viewer's keyboard camera, its third-person bindings (app_settings/key_bindings.xml:62-93), wherever the pointer is:
//   Alt + Left/A, Right/D   spin_around_cw / _ccw     Alt + Up/W, Down/S   move_forward / _backward
//   Alt + PgUp/E, PgDn/C    spin_over / _under        Ctrl+Alt + Up/W, Down/S   spin_over / _under
//   Ctrl+Alt+Shift + arrows or A D W S   pan_left / _right / _up / _down
// at LLAgentCamera::updateCamera's rates (llagentcamera.cpp:1411-1456): orbits 90 degrees a second, zoom the distance
// to the focus a second, pan 5 m a second. Not in the viewer's own world view, where these keys are the viewer's.
void App::keyboard_camera() {
    ImGuiIO& io = ImGui::GetIO();
    cam_keys_held_ = false;
    if (settings_.preset != Preset::SecondLife || host_.world_view() || !io.KeyAlt || io.WantTextInput) return;
    // get_orbit_rate (llviewerinput.cpp:388-402): a tap nudges, 5% of full speed rising to all of it over 0.25 s.
    auto rate = [](ImGuiKey a, ImGuiKey b) {
        double r = 0;
        for (ImGuiKey k : {a, b})
            if (ImGui::IsKeyDown(k)) {
                const double t = ImGui::GetKeyData(k)->DownDuration;
                r = std::max(r, t < 0.25 ? 0.05 + t * (1 - 0.05) / 0.25 : 1.0);
            }
        return r;
    };
    const double left = rate(ImGuiKey_LeftArrow, ImGuiKey_A), right = rate(ImGuiKey_RightArrow, ImGuiKey_D);
    const double up = rate(ImGuiKey_UpArrow, ImGuiKey_W), down = rate(ImGuiKey_DownArrow, ImGuiKey_S);
    const double over = rate(ImGuiKey_PageUp, ImGuiKey_E) - rate(ImGuiKey_PageDown, ImGuiKey_C);
    if (!left && !right && !up && !down && !over) return;
    cam_keys_held_ = true;
    const double dt = io.DeltaTime, orbit_rate = 90 * kDegToRad, pan_rate = 5;
    cam_glide_.apply_input(camera_, [&](Camera& cam) {
        if (io.KeyCtrl && io.KeyShift) {
            slcam::pan(cam, sl_focus_, (left - right) * pan_rate * dt, (up - down) * pan_rate * dt);
        } else if (io.KeyCtrl) {
            if (up != down) slcam::orbit_over(cam, sl_focus_, (up - down) * orbit_rate * dt);
        } else if (!io.KeyShift) {
            if (left != right) slcam::orbit_around(cam, sl_focus_, (right - left) * orbit_rate * dt);
            if (over != 0) slcam::orbit_over(cam, sl_focus_, over * orbit_rate * dt);
            if (up != down)
                slcam::orbit_in(cam, sl_focus_, (up - down) * (cam.eye() - cam.target).length() * dt);
        }
    });
}

void App::start_box(ImVec2 m, bool from_b, bool click_clears) {
    box_ = true, box_moved_ = false, box_from_b_ = from_b, box_click_clears_ = click_clears, box_armed_ = false;
    box_press_ = m;
    box_hits_.clear();
}

// Box selection: the drag, then the release applies it with the preset's modifiers (box_select.h). Like every other
// selection change it is not an undo step. A click without a drag does what a click on empty space always did.
// Bind to...: the hand chosen first, the thigh clicked in the view after, instead of a filtered two-bone selection in the
// right order. Esc or a right-click gives up; the camera keys and the wheel still move the view meanwhile.
bool App::bind_pick_input(bool hovered) {
    if (bind_pick_ < 0) return false;
    if (bind_pick_ >= skel_.size() || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        bind_pick_ = -1;
        skip_shortcuts_ = true;
        status("Bind cancelled");
        return true;
    }
    if (!hovered) return false;
    const int target = dot_hover_ >= 0 ? dot_hover_ : hover_bone_;
    if (target >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::GetIO().KeyAlt) return false;
    if (target < 0) return status("Bind to...: click the bone " + bone_label(bind_pick_) + " should ride, or Esc"), true;
    const int p = std::exchange(bind_pick_, -1);
    std::string why;
    bool ok = false;
    edit("Pin to Bone", [&](Clip& c) { ok = pin_here(c, *rig_, frame_, p, target, shape(), why); });
    status(ok ? bone_label(p) + " now rides " + bone_label(target) + " from frame " + std::to_string(int(frame_)) : why);
    select(p, false);
    return true;
}

bool App::box_input(ImVec2 m, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    // Blender: B over the view arms a box for the next left press, anywhere. With audio loaded B marks a beat instead.
    if (!box_ && settings_.preset == Preset::Blender && hovered && !io.WantTextInput && !doc_.clip().audio &&
        ImGui::IsKeyChordPressed(ImGuiKey_B)) {
        box_armed_ = true;
        skip_shortcuts_ = true;
        status("Box select: drag over the bones   Ctrl: remove   Esc: cancel");
    }
    if (box_armed_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            box_armed_ = false;
            skip_shortcuts_ = true;
            status("Cancelled");
            return true;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) start_box(m, true, false);
        else return false;  // Alt+left still orbits with 3-button emulation
    }
    if (!box_) return false;
    hover_bone_ = hover_handle_ = -1;
    gizmo_hover_ = Gizmo::None;
    box_moved_ = box_moved_ || std::hypot(m.x - box_press_.x, m.y - box_press_.y) >= 4;
    const Shape* sh = shape();
    if (box_moved_)
        box_hits_ = points_in_rect(projector_, globals_, box_press_.x, box_press_.y, m.x, m.y, [&](int i) {
            // What is drawn and could be clicked: visible, and not a zero-length bone (as pick_node).
            return i < skel_.size() && node_visible(i) && (sh ? skel_[i].end.mul(sh->scale[i]) : skel_[i].end).length() >= 1e-5;
        });
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        box_ = false;
        box_hits_.clear();
        skip_shortcuts_ = true;
        status("Cancelled");
        return true;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return true;
    box_ = false;
    last_click_ = ImVec2(-100, -100);
    if (!box_moved_) {
        if (box_click_clears_) clear_selection();
        last_click_ = m;
        return true;
    }
    const size_t before = selection_.size();
    const BoxMode mode = box_mode(settings_.preset, io.KeyShift, io.KeyCtrl, box_from_b_);
    if (mode == BoxMode::Replace) handles_.clear();
    apply_box(selection_, box_hits_, mode);
    selected_prop_ = -1;  // bones and a prop are never selected together (VP-27)
    handle_primary_ = selection_.empty() && !handles_.empty();
    status(selection_.empty() ? std::string("Nothing selected")
                              : count_noun(selection_.size(), "bone") + " selected" +
                                    (mode == BoxMode::Replace ? "" : " (was " + std::to_string(before) + ")"));
    box_hits_.clear();
    return true;
}

void App::viewport_input(const ImVec2& origin, const ImVec2& size, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    viewport_hovered_ = hovered;
    auto_ik_input();  // 08 AI-3: the chain's length while an Auto IK drag runs
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
            if (nav_mode == 0) cam_glide_.apply_input(camera_, [&](Camera& c) { c.orbit(d.x, d.y); });
            else if (nav_mode == 1) cam_glide_.apply_input(camera_, [&](Camera& c) { c.pan(d.x, d.y); });
            else if (nav_mode == 3) sl_camera_drag(d, size.x);  // Second Life's Alt drag
            else cam_glide_.apply_input(camera_, [&](Camera& c) { c.zoom(std::exp(0.005 * (d.y - d.x))); });
        }
        return;
    }
    if (box_input(m, hovered)) return;
    if (hovered && io.MouseWheel != 0 && !(auto_ik_.on && (dragging_gizmo_ || dot_drag_started_))) {
        cam_glide_.apply_input(camera_, [&](Camera& c) {
            if (preset == Preset::SecondLife && !host_.world_view()) {
                slcam::orbit_in(c, sl_focus_, c.distance * (1 - std::pow(std::sqrt(std::sqrt(2.0)), -io.MouseWheel)));
            } else {
                c.zoom(std::pow(0.9, io.MouseWheel));
            }
        });
    }
    // The viewer's world view: its own camera controls get those clicks (spec 09 U3).
    if (hovered && !dragging_gizmo_ && euler_drag_bone_ < 0 && dot_drag_ < 0 && !host_.world_view()) {
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
            // Alt, Ctrl+Alt or Ctrl+Alt+Shift: focus on what was clicked, then the drag zooms, orbits or pans.
            sl_valid_click_ = sl_focus_at(m);
            sl_accum_x_ = sl_accum_y_ = 0, sl_outside_slop_x_ = sl_outside_slop_y_ = false;
            start(0, 3);
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

    if (drag_limit_handle_ != LimitHandle::None) {
        if (!ImGui::IsMouseDown(0)) {
            finish_limit_handle_drag();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            cancel_limit_handle_drag();
            skip_shortcuts_ = true;
            status("Cancelled");
            return;
        }
        update_limit_handle_drag(m);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return;
    }

    // VP-26: a bone pressed with the Rotate tool turns about the view axis through its head once the cursor
    // has moved 4 px; the angle is the change of the cursor's screen angle around the projected head.
    if (bone_drag_ >= 0) {
        if (!ImGui::IsMouseDown(0)) {
            if (bone_drag_started_ && doc_.history.commit("Rotate " + skel_[bone_drag_].name, doc_.clip())) {
                mark_dirty();
                status("Rotated " + bone_label(bone_drag_) + " at frame " + std::to_string(int(std::round(frame_))));
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

    // 08 AI-1: a joint's dot pressed, then dragged 4 px: the joint moves in the view plane and Auto IK turns the bones
    // above it. A click without the drag only selected it.
    if (dot_drag_ >= 0) {
        const std::string name = skel_[dot_drag_].name;
        if (!ImGui::IsMouseDown(0)) {
            if (dot_drag_started_ && doc_.history.commit("Move " + name + (body_drag_on_ ? "" : " (Auto IK)"), doc_.clip())) {
                mark_dirty();
                status("Moved " + name + (body_drag_on_ ? " with planted feet" : " by Auto IK") + " at frame " + std::to_string(int(std::round(frame_))));
            }
            dot_drag_ = -1, dot_drag_started_ = false, auto_ik_.on = false, body_drag_on_ = false;
            return;
        }
        if (dot_drag_started_ && (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            doc_.clip() = doc_.history.cancel();
            dot_drag_ = -1, dot_drag_started_ = false, auto_ik_.on = false, body_drag_on_ = false;
            follow_through_.reset();
            skip_shortcuts_ = true;
            status("Cancelled");
            return;
        }
        if (!dot_drag_started_) {
            if (std::hypot(m.x - dot_drag_press_.x, m.y - dot_drag_press_.y) < 4) return;
            doc_.history.begin(doc_.clip());
            drag_tool_ = Tool::Move;
            capture_edit_start();
            if (!auto_ik_.on && !body_drag_on_) {  // nothing to pull after all
                doc_.clip() = doc_.history.cancel();
                dot_drag_ = -1;
                return;
            }
            dot_drag_started_ = true;
        }
        const double wpp = projector_.world_per_pixel(camera_, auto_ik_.on ? auto_ik_.from : drag_start_global_.pos);
        apply_delta(Quat{}, (camera_.right() * (m.x - dot_drag_press_.x) - camera_.up() * (m.y - dot_drag_press_.y)) * wpp);
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
            dragging_gizmo_ = false, auto_ik_.on = false, body_drag_on_ = false;
            skip_shortcuts_ = true;
            status("Cancelled");
            return;
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            apply_gizmo_drag(m, preset == Preset::SecondLife ? snap_on_ : io.KeyCtrl);
        } else {
            const char* label = tool == Tool::Rotate ? "Rotate" : tool == Tool::Scale ? "Scale" : "Move";
            std::string what = sp ? sp->name : ph ? rig_->limbs()[ph->limb].label + (ph->pole ? " pole" : " IK") : skel_[p].name;
            if (auto_ik_.on) what += " (Auto IK)";
            auto_ik_.on = false, body_drag_on_ = false;
            const bool reached = ph && !ph->pole && tool == Tool::Move && reach_after_drag(ph->limb);  // 08 RC-1, same step
            if (doc_.history.commit(std::string(label) + " " + what, doc_.clip())) {
                mark_dirty();
                if (!reached)
                    status(std::string(tool == Tool::Rotate ? "Rotated " : tool == Tool::Scale ? "Scaled " : "Moved ") + what +
                           " at frame " + std::to_string(int(std::round(frame_))));
            }
            gizmo_.end_drag();
            dragging_gizmo_ = false;
        }
        return;
    }

    gizmo_hover_ = hovered && gizmo_on ? gizmo_.hit(m) : Gizmo::None;
    std::vector<int> ranked;
    hover_handle_ = hovered ? pick_handle(m, hover_handle_pole_) : -1;
    hover_limit_handle_ = (hovered && edit_limits_mode_ && p >= 0) ? pick_limit_handle(m) : LimitHandle::None;
    if (hover_limit_handle_ != LimitHandle::None) {
        gizmo_hover_ = Gizmo::None;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    hover_bone_ = hovered && hover_handle_ < 0 && hover_limit_handle_ == LimitHandle::None ? pick_bone(m, &ranked) : -1;
    // The free-rotate disk only takes the click when no other bone is under the cursor (VP-24).
    if (gizmo_hover_ == Gizmo::Free && hover_bone_ >= 0 && hover_bone_ != p) gizmo_hover_ = Gizmo::None;
    // 08 AI-1: a joint's dot wins over the free-rotate disk it sits in (the gizmo's own parts win elsewhere).
    dot_hover_ = hovered && hover_handle_ < 0 && hover_limit_handle_ == LimitHandle::None && (gizmo_hover_ == Gizmo::None || gizmo_hover_ == Gizmo::Free) ? pick_dot(m) : -1;
    if (dot_hover_ >= 0) gizmo_hover_ = Gizmo::None;

    // 08 FP-2: pick bone from skin weights under cursor, where no stick, dot, point or volume is in reach: those are
    // drawn over the body, so what you see under the pointer is what a click takes.
    double mesh_t = 1e30;
    hover_grab_.reset();
    int mesh_bone = hovered && hover_bone_ < 0 && hover_handle_ < 0 && hover_limit_handle_ == LimitHandle::None &&
                            (gizmo_hover_ == Gizmo::None || gizmo_hover_ == Gizmo::Free)
                        ? pick_mesh_bone(m, &mesh_t)
                        : -1;
    if (mesh_bone >= 0) {
        Vec3 o, d;
        projector_.ray(camera_, m.x, m.y, o, d);
        double prop_t = 1e30;
        for (int i = 0; i < int(doc_.clip().props.size()); ++i)
            if (double t = ray_prop(i, o, d); t < prop_t) prop_t = t;
        if (mesh_t <= prop_t) {
            if (dot_hover_ < 0) hover_bone_ = mesh_bone, hover_grab_ = o + d * mesh_t;
            if (gizmo_hover_ == Gizmo::Free && hover_bone_ != p) gizmo_hover_ = Gizmo::None;
        }
    }

    if (bind_pick_input(hovered)) return;
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
    // Place on Furniture Point is armed (spec 09 build 20): the click picks the point, bones or not.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && seat_click(m)) return;
    if (preset == Preset::QAvimator && hovered && ImGui::IsMouseClicked(0) && gizmo_hover_ == Gizmo::None) {
        if (hover_bone_ < 0 && hover_handle_ < 0) {  // empty space: orbit, Shift pan, Alt zoom; Ctrl: a selection box
            if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt) start_box(m, false, !host_.world_view());
            else if (!host_.world_view()) nav_button = 0, nav_mode = io.KeyShift ? 1 : io.KeyAlt ? 2 : 0, nav_moved = false;
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
    if (edit_limits_mode_ && hover_limit_handle_ != LimitHandle::None && primary() >= 0) {
        start_limit_handle_drag(hover_limit_handle_, m);
        return;
    }
    // Clicking the same spot again steps through bones stacked under the cursor (VP-23), on a dot as on a stick.
    const bool again = std::hypot(m.x - last_click_.x, m.y - last_click_.y) <= 4;
    if (dot_hover_ >= 0) {  // 08 AI-1: selects it; a drag from here pulls it by Auto IK
        const int pick = again ? next_stacked(ranked, p, dot_hover_) : dot_hover_;
        select(pick, io.KeyShift);
        status(bone_label(pick));
        auto_ik_.grab.reset();
        if (primary() == pick && (pick == dot_hover_ || auto_ik_applies(pick) || body_drag_joint(skel_, pick)))
            dot_drag_ = pick, dot_drag_started_ = false, dot_drag_press_ = m;
    } else if (hover_handle_ >= 0) {
        select_handle({hover_handle_, hover_handle_pole_}, io.KeyShift);
    } else if (hover_bone_ >= 0) {
        // A bone of a limb in IK selects the limb's target instead (VP-25).
        int limb = rig_->limb_of_bone(hover_bone_);
        if (limb >= 0 && limb_states_[limb].ik_on) {
            select_handle({limb, false}, io.KeyShift);
            last_click_ = m;
            return;
        }
        const int pick = again ? next_stacked(ranked, p, hover_bone_) : hover_bone_;
        select(pick, io.KeyShift);
        status(bone_label(pick));
        auto_ik_.grab = pick == hover_bone_ ? hover_grab_ : std::nullopt;
        // A bone pressed with the Rotate tool turns (VP-26); with Move or Select a drag pulls it by Auto IK or moves
        // the body (FP-2, FP-3). A joint's dot pulls with any tool (above).
        if (tool == Tool::Rotate) {
            if (preset != Preset::QAvimator && primary() == pick)
                bone_drag_ = pick, bone_drag_started_ = false, bone_drag_press_ = m;  // VP-26
        } else if ((tool == Tool::Move || tool == Tool::Select) && settings_.auto_ik &&
                   (auto_ik_applies(pick) || body_drag_joint(skel_, pick))) {
            dot_drag_ = pick, dot_drag_started_ = false, dot_drag_press_ = m;
        }
    } else if (int prop = pick_prop(m); prop >= 0) {
        clear_selection();
        selected_prop_ = prop;
        status(doc_.clip().props[prop].name);
    } else if (int actor = pick_actor(m); actor >= 0) {
        activate_actor(actor);  // clicking another actor's body edits it (GR-1)
    } else {  // empty space: a drag draws a selection box, a click clears the selection (not with Shift)
        start_box(m, false, !io.KeyShift);
        return;
    }
    last_click_ = m;
    (void)origin;
    (void)size;
}

// The world view (the viewer): the triangles the world lacks, which the host draws with the world (spec 09 U5): the
// other actors' bodies (None, the default, draws nothing), the props, placed as in the app, and the edited actor's
// bone glyphs, as the app draws them. The avatar is the viewer's own, so no body or ground, unless View > Body swaps a mesh
// body in (spec 09 build 32): the host hides your avatar and this draws the mesh body in its place.
void App::render_world_scene() {
    actor_pick_pos_.clear();
    actor_pick_idx_.clear();
    other_skeletons_.clear();
    const SceneColours& colours = scene_colours();
    if (!host_.scene_begin(ui::SceneTarget::View, 1, 1, camera_, colours)) return;
    static std::vector<Vertex> verts;
    static std::vector<std::uint32_t> indices;
    draw_other_actors(colours);
    // Your actor, swapped: posed by the editor, or in a real-avatar mode by what your avatar does in the world (build 34).
    if (!editing_other() && swap_shown() && !globals_.empty()) draw_mesh_body(verts, indices, swap_live_globals(), true);
    draw_target(colours);  // the target ghost: through the world's scene triangles, as the other actors
    // Editing another actor than yours: it stands at its place with its body, posed live (None: its bones only).
    if (editing_other() && !doc_.project.actors[doc_.project.active].body.empty() && !globals_.empty())
        draw_actor_body(doc_.project.active, globals_, colours, true);
    draw_props(verts, indices);
    draw_treadmill();  // 08 LP-8
    draw_backdrop();   // 08 LT-2
    draw_reference(false, 1);  // 08 RF: the plane, when the host draws pictures (else draw_viewport's overlay)
    host_.scene_end();
}

// The world view's Skeleton Only actors, onion ghosts and collision volumes (spec 09 U4): lines over the world like the
// bones. Skeleton Only actors (a body older projects may have) are at their place around the active actor (your
// avatar); ghosts are bone lines, cool before the frame and warm after, as the viewer's 6g layer drew them; volumes
// are three rings each.
void App::draw_world_extras(ImDrawList* dl) {
    auto line = [&](const Vec3& a, const Vec3& b, ImU32 c, float w) {
        double ax, ay, bx, by;
        if (projector_.to_screen(a, ax, ay) && projector_.to_screen(b, bx, by))
            dl->AddLine(ImVec2(float(ax), float(ay)), ImVec2(float(bx), float(by)), c, w);
    };
    const Shape* sh = shape();

    // Skeleton Only actors (GR-1): other_skeletons_, filled by render_world_scene this frame. Your avatar's bones are
    // there for picking only (the worn avatar is drawn by the host).
    for (const OtherSkeleton& s : other_skeletons_) {
        if (!s.drawn) continue;
        for (const StickSegment& k : other_sticks(s)) {
            Rgb c = mix(kCategoryColour[int(skel_[k.node].category)], {s.colour[0], s.colour[1], s.colour[2]}, 0.5f);
            const ImU32 col = IM_COL32(int(c.r * 200), int(c.g * 200), int(c.b * 200), 220);
            line(k.a, k.b, IM_COL32(10, 12, 14, 130), 3.5f);
            line(k.a, k.b, col, 1.8f);
        }
    }

    // Onion skin (08 ON) and the pre-filter ghost (MC-4a): nothing is evaluated while they are off or playing.
    for (const OnionGhost& g : ghost_poses()) {
        const int a = int(255 * (0.25f + 0.6f * g.at.weight));
        const ImU32 c = g.at.offset < 0 ? IM_COL32(102, 178, 255, a) : g.at.offset > 0 ? IM_COL32(255, 158, 77, a)
                                                                                 : IM_COL32(220, 220, 220, a);
        for (int i = 1; i < skel_.volume_start(); ++i) {
            const int parent = skel_[i].parent;
            if (parent >= 0 && !skel_[i].attachment && skel_[i].category != Category::Face && node_visible(i))
                line(g.globals[parent].pos, g.globals[i].pos, c, 1.5f);
        }
    }

    // Pinned ghosts (08 ON-5), as the onion ghosts are drawn here: bone lines, violet.
    for (const auto& g : pinned_ghost_poses())
        for (int i = 1; i < skel_.volume_start(); ++i) {
            const int parent = skel_[i].parent;
            if (parent >= 0 && !skel_[i].attachment && skel_[i].category != Category::Face && node_visible(i))
                line(g[parent].pos, g[i].pos, IM_COL32(184, 107, 255, 220), 1.5f);
        }

    // The SL preview's original (08 SP-2), as the onion ghosts are drawn here: bone lines, green.
    for (int i = 1; !sl_ghost_.empty() && i < skel_.volume_start(); ++i) {
        const int parent = skel_[i].parent;
        if (parent >= 0 && !skel_[i].attachment && skel_[i].category != Category::Face && node_visible(i))
            line(sl_ghost_[parent].pos, sl_ghost_[i].pos, IM_COL32(102, 230, 140, 200), 1.5f);
    }

    // Collision volumes (VP-10): an ellipse in each of the shell's three planes.
    const Rgb shell = scene_colours().shell;
    for (const CollisionVolume& cv : skel_.volumes()) {
        if (!node_visible(cv.node)) continue;
        const VolumeShell s = volume_shell(globals_, sh, cv);
        const Vec3& axes = s.axes;
        const bool sel = std::find(selection_.begin(), selection_.end(), cv.node) != selection_.end();
        const Rgb c = cv.node == primary() ? kSelected : sel ? mix(kSelected, shell, 0.45f)
                      : hot(cv.node) ? mix(shell, {1, 1, 1}, 0.5f) : shell;
        const ImU32 col = IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), sel || hot(cv.node) ? 220 : 140);
        const Xform& g = s.frame;
        constexpr int kSegments = 24;
        for (int plane = 0; plane < 3; ++plane)
            for (int k = 0; k < kSegments; ++k) {
                auto at = [&](int n) {
                    const double t = 2 * kPi * n / kSegments;
                    Vec3 u;
                    u[plane] = std::cos(t), u[(plane + 1) % 3] = std::sin(t);
                    return g.apply(u.mul(axes));
                };
                line(at(k), at(k + 1), col, 1.2f);
            }
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
        viewport_min_ = origin, viewport_max_ = ImVec2(origin.x + size.x, origin.y + size.y);
        const ImVec2 m = ImGui::GetIO().MousePos;
        // AllowWhenBlockedByActiveItem: while another window's item is held (dragging the Face Cam, a title bar, a
        // slider) IsWindowHovered would say no window is hovered, and the press would also start a box or a pick.
        hovered = m.x >= origin.x && m.y >= origin.y && m.x < viewport_max_.x && m.y < viewport_max_.y &&
                  !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                  !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                  host_.pointer_on_world();
        projector_ = host_.projector(origin, size);
        if (ui::Host::HostUi* hui = host_.host_ui()) hui->place_view(origin, viewport_max_);  // the viewer's toasts stay in here
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        // Always there, as in Second Life: no tab to drag it off by, and nothing docks over it.
        ImGuiWindowClass fixed;
        fixed.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoDockingOverMe;
        ImGui::SetNextWindowClass(&fixed);
        bool open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (!open) return ImGui::End();

        origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        if (size.x < 8 || size.y < 8) return ImGui::End();
        viewport_min_ = origin, viewport_max_ = ImVec2(origin.x + size.x, origin.y + size.y);
        ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        w = std::max(1, int(size.x * scale.x)), h = std::max(1, int(size.y * scale.y));

        projector_ = host_.projector(origin, size);

        ImGui::InvisibleButton("##view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                   ImGuiButtonFlags_MouseButtonMiddle);
        // NoNavOverride: letting go of Alt (an Alt+click, the Second Life camera) wakes ImGui's keyboard navigation,
        // which would otherwise say not hovered until the pointer next moves, and the wheel would do nothing.
        // Only the view itself: no other window or popup over it, and no other item held.
        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride) && (ImGui::IsItemActive() || !ImGui::IsAnyItemActive());
    }
    // The view cube takes the pointer while it is over the cube or dragging it.
    ImVec2 vmax(origin.x + size.x, origin.y + size.y);
    const float cube = settings_.view_cube_size;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool over_cube = mp.x >= origin.x + 8 && mp.x <= origin.x + 8 + cube && mp.y >= origin.y + 8 && mp.y <= origin.y + 8 + cube;
    {
        VATS_PROFILE("vp input");
        const bool path = motion_path_input(hovered && !over_cube && cube_drag_ == 0);  // 08 MP-3: a key dot's drag
        // 08 RG-14 and RG-15: a marker's drag, a weight stroke.
        const bool rig = !path && rig_scratch_input(hovered && !over_cube && cube_drag_ == 0);
        const bool paint = !path && !rig && paint_input(hovered && !over_cube && cube_drag_ == 0);
        viewport_input(origin, size, hovered && !over_cube && cube_drag_ == 0 && !path && !rig && !paint);
    }
    if (world && is_view_drop(ImGui::GetDragDropPayload())) {
        // The world view has no window of its own to drop onto: an empty one over it while an item is dragged. Not
        // while a window is (also a payload): over it the dockspace would offer no place to dock that window.
        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("##world_drop", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        viewport_drop_target(origin, size);
        ImGui::End();
    } else {
        viewport_drop_target(origin, size);  // Inventory drops (VP-83); after hover, before the highlight
    }
    projector_ = host_.projector(origin, size);  // input may have moved the camera

    ImTextureID scene = 0;
    if (!world) { VATS_PROFILE("vp render_scene"); scene = render_scene(w, h); }
    else render_world_scene();
    ImDrawList* dl = world ? ImGui::GetBackgroundDrawList() : ImGui::GetWindowDrawList();  // world: behind every panel
    if (scene) dl->AddImage(scene, origin, ImVec2(origin.x + size.x, origin.y + size.y), ImVec2(0, 1), ImVec2(1, 0));
    if (world) draw_reference_overlay(dl, origin, size);  // 08 RF

    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    if (world) draw_world_extras(dl);
    draw_target_bones(dl);  // the target ghost's bones, thin lines in both hosts
    if (stick_bones()) draw_stick_bones(dl);  // 08 FP-1
    // Attachment points get a dot so they can be seen and clicked (their glyphs are only 4 cm).
    for (int i = skel_.joint_count(); i < skel_.volume_start(); ++i) {
        if (!node_visible(i) && i != hover_bone_) continue;  // a hidden point shows while a drop targets it
        double x, y;
        if (!projector_.to_screen(globals_[i].pos, x, y)) continue;
        bool sel = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        ImU32 c = i == primary() ? IM_COL32(255, 242, 51, 255) : sel ? IM_COL32(255, 200, 90, 255)
                  : hot(i) ? IM_COL32(220, 255, 225, 255) : ui::kAttachment;
        dl->AddCircleFilled(ImVec2(float(x), float(y)), 4.5f, c);
        dl->AddCircle(ImVec2(float(x), float(y)), 4.5f, IM_COL32(10, 12, 14, 200), 0, 1.2f);
    }
    draw_handles(dl);
    draw_joint_limit_badges(dl);
    draw_rig_map_overlay(dl);  // 08 RM
    draw_rig_scratch_overlay(dl);  // 08 RG-14: the markers
    draw_paint_overlay(dl);        // 08 RG-15: the brush
    if (bind_pick_ >= 0 && bind_pick_ < skel_.size()) {  // Bind to...: what the next click does, where the eye is
        const std::string text = "Click the bone " + skel_[bind_pick_].name + " rides  \xc2\xb7  Esc cancels";
        const ImVec2 lo = dl->GetClipRectMin(), hi = dl->GetClipRectMax(), ts = ImGui::CalcTextSize(text.c_str());
        const float pad = ImGui::GetFontSize() * 0.5f, x = std::max(lo.x + pad, (lo.x + hi.x - ts.x) * 0.5f - pad);
        dl->AddRectFilled(ImVec2(x, lo.y + pad), ImVec2(x + ts.x + 2 * pad, lo.y + 2 * pad + ts.y), IM_COL32(12, 13, 16, 215), ts.y);
        dl->AddRect(ImVec2(x, lo.y + pad), ImVec2(x + ts.x + 2 * pad, lo.y + 2 * pad + ts.y), IM_COL32(140, 217, 255, 220), ts.y, 0, 1.5f);
        dl->AddText(ImVec2(x + pad, lo.y + 1.5f * pad), IM_COL32(235, 245, 255, 255), text.c_str());
    }
    draw_joint_limit_viewport(dl);
    // 08 AI-1: the joint dots Auto IK drags by: the hovered bone's and the selected one's.
    for (int c : {primary(), dot_hover_}) {
        double x, y;
        if (c < 0 || !node_visible(c) || dragging_gizmo_ || modal_ != Modal::None ||
            (c != dot_hover_ && !auto_ik_applies(c) && !body_drag_joint(skel_, c)) ||
            !projector_.to_screen(globals_[c].pos, x, y))
            continue;
        const bool hot_dot = c == dot_hover_ || c == dot_drag_;
        dl->AddCircleFilled(ImVec2(float(x), float(y)), hot_dot ? 6.f : 4.5f, hot_dot ? IM_COL32(255, 230, 51, 255) : IM_COL32(245, 245, 245, 235));
        dl->AddCircle(ImVec2(float(x), float(y)), hot_dot ? 6.f : 4.5f, IM_COL32(10, 12, 14, 220), 0, 1.5f);
    }
    // 08 FP-3: small markers on planted feet while dragging with planted feet
    if (body_drag_on_ && (dragging_gizmo_ || modal_ == Modal::Move || dot_drag_started_)) {
        const bool plant = !ImGui::GetIO().KeyAlt;
        if (plant) {
            const double ground_z = contact_height();
            for (const PlantedFoot& f : body_drag_.feet) {
                Vec3 p = f.at.pos;
                p.z = ground_z;
                double sx = 0, sy = 0;
                if (!projector_.to_screen(p, sx, sy)) continue;
                dl->AddCircleFilled(ImVec2(float(sx), float(sy)), 4.0f, IM_COL32(80, 220, 255, 230));
                dl->AddCircle(ImVec2(float(sx), float(sy)), 6.5f, IM_COL32(10, 12, 14, 220), 0, 1.5f);
                dl->AddCircle(ImVec2(float(sx), float(sy)), 6.5f, IM_COL32(80, 220, 255, 255), 0, 1.0f);
            }
        }
    }
    draw_motion_paths(dl);  // 08 MP: through projector_, so the viewer draws it over the world too
    draw_balance(dl);  // View > Centre of Mass (08 CM-1)
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
        gizmo_.draw(dl, gizmo_hover_, mirror_live_ ? kMirrorTint : 0);  // PT-1: tinted while Mirror is on
    if (place_actor_gizmo()) actor_gizmo_.draw(dl, actor_gizmo_hover_);  // GR: placing another actor
    // Box selection: a thin accent rectangle with a faint fill; Blender's B, armed, a crosshair through the pointer.
    if (box_ && box_moved_) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImVec2 a(std::min(m.x, box_press_.x), std::min(m.y, box_press_.y)), b(std::max(m.x, box_press_.x), std::max(m.y, box_press_.y));
        dl->AddRectFilled(a, b, (accent_colour() & 0x00FFFFFF) | (36u << 24));
        dl->AddRect(a, b, accent_colour(), 0, 0, 1.f);
    } else if (box_armed_ && hovered) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImU32 c = (accent_colour() & 0x00FFFFFF) | (140u << 24);
        dl->AddLine(ImVec2(origin.x, m.y), ImVec2(vmax.x, m.y), c);
        dl->AddLine(ImVec2(m.x, origin.y), ImVec2(m.x, vmax.y), c);
    }

    // Hover label.
    // Not while Rig from Scratch places joints: its markers and pins say their own names, and a drag there never poses.
    if (hovered && (hover_bone_ >= 0 || hover_handle_ >= 0) && gizmo_hover_ == Gizmo::None && !dragging_gizmo_ &&
        !rig_scratch_holds_rest()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        std::string label = hover_handle_ >= 0 ? rig_->limbs()[hover_handle_].label + (hover_handle_pole_ ? " IK pole" : " IK")
                                                : bone_label(hover_bone_);
        const Tool tool = effective_tool();  // a bone pressed with Rotate or Scale is not pulled (only its dot is)
        if (hover_handle_ < 0 && (dot_hover_ >= 0 || ((tool == Tool::Move || tool == Tool::Select) && settings_.auto_ik &&
                                                      (auto_ik_applies(hover_bone_) || body_drag_joint(skel_, hover_bone_)))))
            label = bone_label(hover_bone_) + "\nDrag: Auto IK";  // 08 AI-1, FP-2, FP-3
        if (hover_handle_ < 0 && painting()) label = bone_label(hover_bone_) + "\nClick: paint this bone";  // RG-15
        if (hover_handle_ < 0 && contact_bone(hover_bone_))  // red with no word of why read as broken
            label += "\nRed: passes into another\nbody part (Animation Check)";
        if (hover_handle_ < 0 && skel_[hover_bone_].attachment && !skel_[hover_bone_].volume)  // spec 09 item 55: the viewer
            if (const std::vector<std::string> worn = host_.worn_on(skel_[hover_bone_].attach_id); !worn.empty()) {
                label += "\nYou wear here:";
                for (const std::string& w_item : worn) label += "\n  " + w_item;
                const auto track = doc_.clip().curves.find(skel_[hover_bone_].name);
                if (track != doc_.clip().curves.end() && !track->second.empty())
                    label += "\nThis point is keyed: in-world the animation moves what you wear here";
            }
        const char* name = label.c_str();
        ImVec2 ts = ImGui::CalcTextSize(name), at(m.x + 14, m.y + 22);
        at.x = std::max(dl->GetClipRectMin().x + 5, std::min(at.x, dl->GetClipRectMax().x - ts.x - 5));  // whole, in the view
        at.y = std::min(at.y, dl->GetClipRectMax().y - ts.y - 3);
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
    draw_view_cube(dl, origin, hovered);
    dl->PopClipRect();
    draw_prop_sl_popup();
    draw_context_menu();
    if (!world) return ImGui::End();
    // The world view takes the pointer only over what the editor draws, so every other click reaches the world.
    const bool taken = dragging_gizmo_ || drag_limit_handle_ != LimitHandle::None || bone_drag_ >= 0 || dot_drag_ >= 0 || euler_drag_bone_ >= 0 || modal_ != Modal::None || box_ ||
                       motion_path_.drag_limb >= 0 || (hovered && motion_path_.hover) ||
                       actor_dragging_ || cube_drag_ != 0 ||
                       (hovered && (hover_bone_ >= 0 || hover_handle_ >= 0 || hover_limit_handle_ != LimitHandle::None || dot_hover_ >= 0 || gizmo_hover_ != Gizmo::None ||
                                    actor_gizmo_hover_ != Gizmo::None || over_cube));
    if (taken) ImGui::SetNextFrameWantCaptureMouse(true);
}

}  // namespace vats
