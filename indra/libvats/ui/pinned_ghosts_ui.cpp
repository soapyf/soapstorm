// Viewport Avatar Toolset - pinned ghosts (a frame, another actor's frame or a library pose, drawn until removed) and
// the target ghost (another animation to match by eye).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 ON-5. They go through draw_ghost (the app) and draw_world_extras (the viewer), as the onion
// ghosts do. A view aid like the onion settings: not saved and not undone; each is evaluated every frame, so it
// always shows the clip as it is now (after an undo too).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "widgets.h"
#include "theme.h"

namespace vats {

void App::pin_ghost(int actor, double frame) {
    const Project& p = doc_.project;
    const std::string name = p.actors.empty() ? "" : p.actors[actor].name;
    const std::string at = std::to_string(int(std::round(frame)));
    pinned_ghosts_.push_back({actor == p.active ? "Frame " + at : name + ", frame " + at, name, std::round(frame), std::nullopt});
}

void App::pin_pose_ghost(const LibraryItem& pose) { pinned_ghosts_.push_back({"Pose " + pose.name, "", 0, pose}); }

// Each pinned ghost's globals in the active actor's space; one that names a removed actor is skipped.
std::vector<std::vector<Xform>> App::pinned_ghost_poses() {
    std::vector<std::vector<Xform>> out;
    if (!rig_) return out;
    const Project& p = doc_.project;
    for (const PinnedGhost& g : pinned_ghosts_) {
        if (g.pose) {
            out.push_back(pose_ghost(*rig_, doc_.clip(), frame_, shape(), *g.pose));
            continue;
        }
        int i = p.active;
        if (!g.actor.empty()) {
            i = -1;
            for (int k = 0; k < int(p.actors.size()); ++k)
                if (p.actors[k].name == g.actor) i = k;
            if (i < 0) continue;
        }
        if (i == p.active) {
            out.push_back(vats::evaluate(*rig_, doc_.clip(), g.frame, shape()).globals);
        } else {
            Evaluation e = evaluate_actor(i, g.frame);
            const Xform rel = actor_rel(i);
            for (Xform& x : e.globals) x = rel * x;
            out.push_back(std::move(e.globals));
        }
    }
    return out;
}

// The Onion Skin menu's part: pin, the other actors, and the pinned list with a remove button each.
void App::draw_pinned_ghost_menu() {
    const Project& p = doc_.project;
    subheading("Pinned Ghosts");
    if (menu_item_icon(icon::kPin, "Pin Ghost at This Frame")) pin_ghost(p.active, frame_);
    ImGui::SetItemTooltip("Keep a violet ghost of this frame's pose in the view while you work elsewhere");
    if (p.actors.size() > 1 && ImGui::BeginMenu("Ghost Other Actor at Frame")) {
        for (int i = 0; i < int(p.actors.size()); ++i)
            if (i != p.active && ImGui::MenuItem(p.actors[i].name.c_str())) pin_ghost(i, frame_);
        ImGui::EndMenu();
    }
    int remove = -1;
    for (int i = 0; i < int(pinned_ghosts_.size()); ++i) {
        ImGui::PushID(i);
        if (icon_small_button("remove", icon::kDelete, "Remove this ghost")) remove = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(pinned_ghosts_[i].label.c_str());
        ImGui::PopID();
    }
    if (remove >= 0) pinned_ghosts_.erase(pinned_ghosts_.begin() + remove);
    if (pinned_ghosts_.size() > 1 && menu_item_icon(icon::kDelete, "Remove All Pinned Ghosts")) pinned_ghosts_.clear();
}

// The target ghost: see-through in the theme's green, in the view's body (bones with Skeleton Only), its props
// tinted the same. In the viewer this is the world's scene triangles, as the other actors' bodies are there.
int App::target_actor_for_view() const {
    if (!target_) return 0;
    const Project& p = doc_.project;
    if (multi_actor())
        for (int i = 0; i < int(target_->actors.size()); ++i)
            if (target_->actors[i].name == p.actors[p.active].name) return i;
    return std::clamp(target_->active, 0, int(target_->actors.size()) - 1);
}

// Every actor of the target with its pose at this frame, in the view's space (the matching one first).
std::vector<std::pair<const Clip*, const std::vector<Xform>*>> App::target_ghosts() const {
    std::vector<std::pair<const Clip*, const std::vector<Xform>*>> out;
    if (target_globals_.empty()) return out;
    out.push_back({&target_->actors[target_main_].clip, &target_globals_});
    for (int i = 0, k = 0; i < int(target_->actors.size()) && k < int(target_others_.size()); ++i)
        if (i != target_main_) out.push_back({&target_->actors[i].clip, &target_others_[k++]});
    return out;
}

void App::draw_target(const SceneColours& colours) {
    const Rgb c = colours.target_ghost;
    static std::vector<Vertex> verts;
    static std::vector<std::uint32_t> idx;
    const float tint[4] = {c.r, c.g, c.b, target_opacity_};
    for (auto [clip, g] : target_ghosts()) {
        draw_ghost(*g, c, target_opacity_, body_ == Body::SkeletonOnly && !mesh_body());
        for (const Prop& p : clip->props)
            if (p.visible) draw_prop(p, verts, idx, g, shape(), 1.f, {}, tint);
    }
}

// Its bones as thin lines over the view (both hosts), for the bone groups View shows; face bones are left out as the
// onion ghosts' lines leave them.
// Collision volumes keyed in the target (jiggle) get a small ring where the target has them, so a bounce can be
// matched: the body mesh does not show them.
void App::draw_target_bones(ImDrawList* dl) {
    const Rgb c = scene_colours().target_ghost;
    const ImU32 col = IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), 210);
    for (auto [clip, gp] : target_ghosts()) {
        const std::vector<Xform>& g = *gp;
        for (int i = 1; i < skel_.volume_start(); ++i) {
            const int parent = skel_[i].parent;
            if (parent < 0 || skel_[i].attachment || skel_[i].category == Category::Face || !node_visible(i)) continue;
            double ax, ay, bx, by;
            if (projector_.to_screen(g[parent].pos, ax, ay) && projector_.to_screen(g[i].pos, bx, by))
                dl->AddLine(ImVec2(float(ax), float(ay)), ImVec2(float(bx), float(by)), col, 1.2f);
        }
        for (const CollisionVolume& v : skel_.volumes()) {
            const auto t = clip->curves.find(v.name);
            if (v.node < 0 || t == clip->curves.end() || !clip->has_channels(v.name, kPosChannels)) continue;
            double x, y;
            if (!projector_.to_screen(g[v.node].pos, x, y)) continue;
            const float r = std::max(3.f, ImGui::GetFontSize() * 0.3f);
            dl->AddCircle(ImVec2(float(x), float(y)), r, col, 12, 1.5f);
            dl->AddCircleFilled(ImVec2(float(x), float(y)), 1.5f, col);
        }
    }
}

void App::draw_target_menu() {
    if (menu_item_icon(action_icon("target_show"), "Show Target Ghost", key_hint("target_show").c_str(), target_on_, target_.has_value()))
        run_action("target_show");
    if (target_) ImGui::SetItemTooltip("%s, drawn see-through in green over your avatar at the same frame", target_->name.c_str());
    else ImGui::SetItemTooltip("Load a target first");
    menu_item("target_load");
    ImGui::SetItemTooltip("Another animation (.vat or .anim) to match by eye; the open project stays as it is");
    menu_item("target_clear");
    ImGui::BeginDisabled(!target_);
    float pct = target_opacity_ * 100;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (slider_float("Opacity##target", &pct, 10, 90, "%.0f%%")) target_opacity_ = pct / 100;
    ImGui::EndDisabled();
}

// "Target: <name>" while a target is loaded (dimmed while hidden; a click shows or hides it) and, with a bone selected,
// how far that bone is from the target at this frame, green once it is within 5 degrees.
void App::draw_target_status() {
    if (!target_) return;
    ImGui::SameLine(0, 24);
    if (!target_on_) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool clicked = ImGui::SmallButton(("Target: " + target_->name + "##target_chip").c_str());
    if (!target_on_) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(target_on_ ? "The target ghost: click to hide it" : "The target ghost is hidden: click to show it");
    if (clicked) target_on_ = !target_on_;
    const int b = primary();
    if (target_globals_.empty() || b < 0 || b >= skel_.volume_start()) return;
    const double deg = bone_angle_apart(skel_, globals_, target_globals_, b);
    char text[32];
    std::snprintf(text, sizeof text, "%.0f\xc2\xb0 away", deg);
    ImGui::SameLine(0, 8);
    if (deg < 5) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", text);
    else ImGui::TextDisabled("%s", text);
    ImGui::SetItemTooltip("How far %s is turned from the target at this frame, against its parent: match from the hips "
                          "outward. Green is within 5\xc2\xb0.", skel_[b].name.c_str());
}

}  // namespace vats
