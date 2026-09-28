// Viewport Avatar Toolset - couples and groups: several actors in one scene (spec 08 section 2).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The active actor's clip is Project::clip, so every single-actor code path edits it unchanged. The
// view works in the active actor's space: the other actors are drawn at their placement relative to it.
#include <algorithm>
#include <cmath>
#include <fstream>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "theme.h"

namespace vats {

namespace {

const std::array<float, 3> kActorColours[] = {
    {0.85f, 0.62f, 0.45f}, {0.45f, 0.66f, 0.88f}, {0.62f, 0.82f, 0.50f}, {0.86f, 0.52f, 0.72f}, {0.80f, 0.76f, 0.46f}};

std::string unique_name(const Project& p, std::string base) {
    auto taken = [&](const std::string& n) {
        for (auto& a : p.actors)
            if (a.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int k = 2;; ++k)
        if (!taken(base + " " + std::to_string(k))) return base + " " + std::to_string(k);
}

}  // namespace

Xform App::actor_rel(int i) const {
    const Project& p = doc_.project;
    if (p.actors.size() < 2 || i == p.active) return {};
    return p.actors[p.active].placement().inverse() * p.actors[i].placement();
}

std::string App::actor_body_key(int i) const {
    const Project& p = doc_.project;
    if (i < int(p.actors.size()) && !p.actors[i].body.empty()) return p.actors[i].body;
    return settings_.mesh_body.empty() ? settings_.body : "mesh:" + settings_.mesh_body;
}

void App::sync_active_body() {
    Body want = Body::SLDefault;
    const std::string key = actor_body_key(doc_.project.active);
    bool found = false;
    for (int k = 0; k < kBodyCount; ++k)
        if (key == kBodyIds[k]) want = Body(k), found = true;
    if (!found || want == body_) return;  // ponytail: a mesh body chosen per actor shows only on the other actors
    body_ = want;
    mesh_.build(body_);
}

const Shape* App::actor_shape(int i) const {
    const Project& p = doc_.project;
    if (p.actors.size() < 2) return shape();
    const std::string b = actor_body_key(i);
    for (int k = 0; k < kBodyCount; ++k)
        if (b == kBodyIds[k]) return mesh_.shape(Body(k));
    if (b.rfind("mesh:", 0) == 0)  // BD-3: the body's own joints over the view's Linden shape
        if (const MeshBody* mb = find_mesh_body(b.substr(5))) return mesh_body_shape(*mb, mesh_.shape(body_));
    return shape();
}

ExternalTarget App::actor_resolver(int self) {
    return [this, self](const Pin& pin, double frame, Xform& out) {
        static thread_local int depth = 0;  // ponytail: cross-actor pins resolve one level deep, so they cannot loop
        const Project& p = doc_.project;
        int t = -1;
        for (int i = 0; i < int(p.actors.size()); ++i)
            if (p.actors[i].name == pin.target_actor) t = i;
        int bone = skel_.find(pin.target);
        if (t < 0 || t == self || self >= int(p.actors.size()) || bone < 0 || depth > 0) return false;
        ++depth;
        Evaluation e = vats::evaluate(*rig_, actor_clip(p, t), frame, actor_shape(t));
        --depth;
        out = p.actors[self].placement().inverse() * p.actors[t].placement() * e.globals[bone];
        return true;
    };
}

Evaluation App::evaluate_actor(int i, double frame) {
    rig_->external = actor_resolver(i);
    Evaluation e = vats::evaluate(*rig_, actor_clip(doc_.project, i), frame, actor_shape(i));
    rig_->external = actor_resolver(doc_.project.active);
    return e;
}

void App::activate_actor(int i) {
    Project& p = doc_.project;
    if (i == p.active || i < 0 || i >= int(p.actors.size())) return;
    if (p.actors[i].locked) return status(p.actors[i].name + " is locked");
    if (doc_.history.is_open() || scene_busy()) return status("Finish the current edit first");
    // Keep the camera on the same spot of the scene: old active space -> new active space.
    Xform m = p.actors[i].placement().inverse() * p.actors[p.active].placement();
    camera_.target = m.apply(camera_.target);
    camera_.yaw += (p.actors[p.active].rot_z - p.actors[i].rot_z) * kDegToRad;
    set_active_actor(p, i);
    clear_selection();
    clip_replaced();
    status("Editing " + p.actors[i].name);
}

void App::scene_edit(const std::string& label, const std::function<void(Project&)>& change) {
    if (doc_.history.is_open()) return status("Finish the current edit first");
    Project& p = doc_.project;
    Clip clip_before = p.clip;
    SceneState before{p.actors, p.active};
    change(p);
    if (p.actors.size() == 1) p.actors.clear(), p.active = 0;  // one actor left: a plain project again
    sync_actor_timing(p);
    doc_.history.record_scene(label, std::move(clip_before), std::move(before), p.clip, {p.actors, p.active});
    clip_replaced();
    mark_dirty();
}

void App::apply_restore(History::Restore r) {
    Project& p = doc_.project;
    if (r.scene) {
        p.actors = std::move(r.scene->actors);
        p.active = r.scene->active;
    } else if (r.actor != p.active && r.actor < int(p.actors.size())) {
        set_active_actor(p, r.actor);  // the step belongs to another actor: go back to it
        clear_selection();
    }
    p.clip = std::move(r.clip);
    sync_actor_timing(p);
}

// Other visible actors, dimmed and tinted with their colour, each with its own body: None (the default) draws nothing
// but the actor's props, Ruth is the SL default body, a mesh body its imported parts. An actor with nothing to draw is
// not evaluated.
void App::draw_other_actors(const SceneColours& colours) {
    const Project& p = doc_.project;
    actor_pick_pos_.assign(p.actors.size(), {});
    actor_pick_idx_.assign(p.actors.size(), nullptr);
    if (p.actors.size() < 2) return;
    static std::vector<Vertex> verts;
    static std::vector<std::uint32_t> idx;
    static std::vector<float> nrm;
    other_skeletons_.clear();
    for (int i = 0; i < int(p.actors.size()); ++i) {
        const Actor& a = p.actors[i];
        if (i == p.active || a.hidden) continue;
        const auto& props = actor_clip(p, i).props;
        if (a.body.empty() && std::none_of(props.begin(), props.end(), [](const Prop& pr) { return pr.visible; })) continue;
        Evaluation e = evaluate_actor(i, frame_);
        Xform rel = actor_rel(i);
        for (Xform& g : e.globals) g = rel * g;
        for (const Prop& prop : props)  // their props, at their place
            if (prop.visible) draw_prop(prop, verts, idx, &e.globals, actor_shape(i), 0.7f, rel);
        if (a.body.empty()) continue;  // None
        const std::string key = actor_body_key(i);
        if (key.rfind("mesh:", 0) == 0) {
            if (const MeshBody* mb = find_mesh_body(key.substr(5))) {
                harmonize_body(*mb);
                for (const std::string& path : mb->parts) {
                    Prop part;
                    part.path = path;
                    part.rigged = true;
                    draw_prop(part, verts, idx, &e.globals, actor_shape(i), 0.7f);
                }
            }
            continue;
        }
        int b = -1;
        for (int k = 0; k < kBodyCount; ++k)
            if (key == kBodyIds[k]) b = k;
        if (b < 0) continue;
        if (Body(b) == Body::SkeletonOnly) {  // drawn as bones by render_scene
            other_skeletons_.push_back({std::move(e.globals), a.colour, i});
            continue;
        }
        AvatarMesh* m = &mesh_;
        if (Body(b) != body_ || mesh_body()) {
            auto& slot = actor_meshes_[key];
            if (!slot) {
                slot = std::make_unique<AvatarMesh>(mesh_);
                slot->build(Body(b));
            }
            m = slot.get();
        }
        std::vector<float>& pos = actor_pick_pos_[i];
        m->skin(e.globals, actor_shape(i), pos, nrm);
        auto tint = [&](float body, float own) { return (body + (own - body) * 0.35f) * 0.8f; };
        Rgb c{tint(colours.body.r, a.colour[0]), tint(colours.body.g, a.colour[1]), tint(colours.body.b, a.colour[2])};
        verts.resize(pos.size() / 3);
        for (size_t v = 0; v < verts.size(); ++v)
            verts[v] = {{pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]}, {nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]},
                        {c.r, c.g, c.b, 1}};
        host_.scene_triangles(verts, m->indices(), true, 0.04f);
        actor_pick_idx_[i] = &m->indices();
    }
}

int App::pick_actor(ImVec2 m) const {
    Vec3 o, d;
    projector_.ray(camera_, m.x, m.y, o, d);
    int hit = -1;
    double best = 1e30;
    if (host_.world_view()) {  // the world view: Skeleton Only actors are lines (draw_world_extras); a bone near the pointer
        for (const OtherSkeleton& s : other_skeletons_)
            for (int b = 0; b < skel_.joint_count(); ++b) {
                double hx, hy, tx, ty;
                const auto& g = s.globals;
                if (!node_visible(b) || !projector_.to_screen(g[b].pos, hx, hy) || !projector_.to_screen(g[b].apply(skel_[b].end), tx, ty))
                    continue;
                const double abx = tx - hx, aby = ty - hy, apx = m.x - hx, apy = m.y - hy;
                const double t = std::clamp((apx * abx + apy * aby) / std::max(abx * abx + aby * aby, 1e-9), 0.0, 1.0);
                if (double d = std::hypot(apx - abx * t, apy - aby * t); d < 10 && d < best) best = d, hit = s.actor;
            }
        if (hit >= 0) return hit;
        best = 1e30;  // then the bodies, as in the app
    }
    for (int i = 0; i < int(actor_pick_pos_.size()); ++i)
        if (actor_pick_idx_[i])
            if (double t = ray_triangles(o, d, actor_pick_pos_[i], *actor_pick_idx_[i]); t < best) best = t, hit = i;
    return hit;
}

// GR-3: where each actor stands, for the sit-target script. Written next to the exported files.
void App::write_sit_note(const std::string& folder, const std::string& stem) {
    const Project& p = doc_.project;
    std::string t = "Viewport Avatar Toolset: actor placement for \"" + stem + "\"\n\n";
    t += "Each actor's animation plays around that avatar's own position. Seat every avatar at the offset\n"
         "below from one shared point (the pose ball or furniture root), with the same rotation.\n\n";
    char buf[512];
    for (const Actor& a : p.actors) {
        std::snprintf(buf, sizeof buf,
                      "%s\n    offset   <%.3f, %.3f, %.3f>\n    rotation llEuler2Rot(<0.0, 0.0, %.2f> * DEG_TO_RAD)\n"
                      "    llSitTarget(<%.3f, %.3f, %.3f>, llEuler2Rot(<0.0, 0.0, %.2f> * DEG_TO_RAD));\n\n",
                      a.name.c_str(), a.pos.x, a.pos.y, a.pos.z, a.rot_z, a.pos.x, a.pos.y, a.pos.z, a.rot_z);
        t += buf;
    }
    t += "Notes\n"
         "- llSitTarget's offset and rotation are in the frame of the prim that carries it. For several avatars,\n"
         "  use one sit target per linked prim, or move each seated avatar with llSetLinkPrimitiveParamsFast\n"
         "  (PRIM_POS_LOCAL / PRIM_ROT_LOCAL on the avatar's link number) to the offset above.\n"
         "- SL does not seat an avatar exactly at the sit target: sit scripts usually correct the height, often by\n"
         "  about 0.4 m. The right value depends on the avatar and the script: verify in-world and adjust Z.\n"
         "- Keep all actors' animations started together; their lengths and loop points already match.\n";
    std::ofstream(folder + "/" + stem + "_placement.txt", std::ios::binary) << t;
}

// A move / turn gizmo on another actor, in the active actor's space, so the view never shifts under it.
bool App::place_actor_gizmo() {
    const Project& p = doc_.project;
    if (!show_actors_ || place_actor_ < 0 || place_actor_ >= int(p.actors.size()) || place_actor_ == p.active ||
        p.actors[place_actor_].hidden)
        return false;
    const Xform rel = actor_rel(place_actor_);
    const bool rotate = actor_dragging_ ? actor_drag_rotate_ : effective_tool() == Tool::Rotate;
    actor_gizmo_.place(rotate ? GizmoKind::Rotate : GizmoKind::Move, rel.pos, rel.rot, camera_, projector_, gizmo_size_);
    actor_gizmo_.set_z_only(rotate);
    return actor_gizmo_.visible();
}

bool App::actor_gizmo_input(ImVec2 m, bool hovered) {
    actor_gizmo_hover_ = Gizmo::None;
    if (!place_actor_gizmo()) return actor_dragging_ = false;
    Project& p = doc_.project;
    Actor& a = p.actors[place_actor_];
    if (actor_dragging_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            a = actor_drag_start_;
            actor_gizmo_.end_drag();
            actor_dragging_ = false;
            skip_shortcuts_ = true;
            status("Cancelled");
            return true;
        }
        Quat r;
        Vec3 t;
        actor_gizmo_.drag(m, settings_.preset == Preset::SecondLife ? snap_on_ : ImGui::GetIO().KeyCtrl, snap_deg_, r, t);
        // Actors stay upright: only the turn about Z counts (the view's Z is world Z).
        const double turn = 2 * std::atan2(r.z, r.w);
        const Xform active = p.actors[p.active].placement();
        const Xform start = active.inverse() * actor_drag_start_.placement();
        const Xform world = active * Xform{Quat::axis_angle({0, 0, 1}, turn) * start.rot, start.pos + t};
        a.pos = world.pos;
        const Vec3 x = world.rot.rotate({1, 0, 0});
        a.rot_z = std::atan2(x.y, x.x) / kDegToRad;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            actor_gizmo_.end_drag();
            actor_dragging_ = false;
            Actor moved = a;
            a = actor_drag_start_;  // the whole drag is one undo step
            const int i = place_actor_;
            scene_edit("Place Actor", [i, moved](Project& pr) { pr.actors[i] = moved; });
        }
        return true;
    }
    if (!hovered) return false;
    actor_gizmo_hover_ = actor_gizmo_.hit(m);
    const bool rotate = effective_tool() == Tool::Rotate;
    if (rotate && actor_gizmo_hover_ != Gizmo::AxisZ && actor_gizmo_hover_ != Gizmo::ViewRing)
        actor_gizmo_hover_ = Gizmo::None;  // the Z ring or the view ring; only the turn about Z is used
    if (actor_gizmo_hover_ == Gizmo::None || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
    if (a.locked) {
        status(a.name + " is locked");
        return true;
    }
    if (doc_.history.is_open() || scene_busy()) {
        status("Finish the current edit first");
        return true;
    }
    actor_drag_start_ = a;
    actor_drag_rotate_ = rotate;
    actor_gizmo_.begin_drag(actor_gizmo_hover_, m);
    actor_dragging_ = true;
    return true;
}

// Ends a placement-field drag or colour pick: the live change becomes one undo step on the actor it started on.
void App::finish_scene_drags() {
    Project& p = doc_.project;
    if (const int k = std::exchange(field_drag_actor_, -1); k >= 0 && k < int(p.actors.size())) {
        Actor moved = p.actors[k];
        p.actors[k] = field_drag_start_;
        if (!(moved == field_drag_start_)) scene_edit("Place Actor", [k, moved](Project& pr) { pr.actors[k] = moved; });
    }
    if (const int k = std::exchange(colour_actor_, -1); k >= 0 && k < int(p.actors.size())) {
        std::array<float, 3> chosen = p.actors[k].colour;
        p.actors[k].colour = colour_start_;
        if (chosen != colour_start_) scene_edit("Actor Colour", [k, chosen](Project& pr) { pr.actors[k].colour = chosen; });
    }
}

void App::draw_actors_panel() {
    // Also when the panel is closed or collapsed mid-drag, so a drag can never stay open (typing into a
    // field keeps it open: the value is still being entered).
    if (scene_busy() && !actor_dragging_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().WantTextInput)
        finish_scene_drags();
    if (!show_actors_) return;
    ImGui::SetNextWindowSize(window_size(22, 26), ImGuiCond_FirstUseEver);
    place_tool_window(2);
    if (!ImGui::Begin("Actors", &show_actors_)) return ImGui::End();
    help_button("couples-and-groups");
    Project& p = doc_.project;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Several avatars in one scene, each with its own animation. Click an actor's body in the view, "
                       "or its name here, to edit it. Placement is from the shared sit target.");
    ImGui::PopStyleColor();

    auto add = [&](const char* kind) {
        scene_edit(std::string("Add Actor"), [&](Project& pr) {
            if (pr.actors.empty()) {
                Actor first;
                first.name = "Actor 1";
                pr.actors.push_back(first);
                pr.active = 0;
            }
            const Actor& cur = pr.actors[pr.active];
            Actor a;
            a.colour = kActorColours[pr.actors.size() % 5];
            a.body = cur.body;
            Quat r = cur.placement().rot;
            if (std::string(kind) == "partner") {  // facing the current actor, its mirror image
                a.name = unique_name(pr, "Partner");
                a.clip = mirrored_clip(skel_, pr.clip);
                a.pos = cur.pos + r.rotate({0.6, 0, 0});
                a.rot_z = cur.rot_z + 180;
            } else {
                a.name = unique_name(pr, "Actor " + std::to_string(pr.actors.size() + 1));
                if (std::string(kind) == "copy") a.clip = pr.clip;
                else a.clip.fps = pr.clip.fps, a.clip.end_frame = pr.clip.end_frame;
                a.pos = cur.pos + r.rotate({0, -0.8, 0});
                a.rot_z = cur.rot_z;
            }
            a.clip.pins.erase(std::remove_if(a.clip.pins.begin(), a.clip.pins.end(),
                                             [](const Pin& pin) { return !pin.target_actor.empty(); }),
                              a.clip.pins.end());
            pr.actors.push_back(std::move(a));
        });
    };
    if (ImGui::Button("Add Partner")) add("partner");
    ImGui::SetItemTooltip("A mirrored copy of this actor's animation, facing it");
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) add("copy");
    ImGui::SameLine();
    if (ImGui::Button("Blank")) add("blank");
    ImGui::Separator();

    if (p.actors.size() < 2) {
        hint("One actor. Add a partner to start a couple or group scene.");
        ImGui::End();
        return;
    }
    int remove = -1;
    for (int i = 0; i < int(p.actors.size()); ++i) {
        Actor& a = p.actors[i];
        ImGui::PushID(i);
        ImVec4 col(a.colour[0], a.colour[1], a.colour[2], 1);
        ImGui::ColorButton("##c", col, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        std::string label = a.name + (a.hidden ? " (hidden)" : "") + (a.locked ? " (locked)" : "");
        const float buttons = 3 * (icon_button_width() + ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Selectable(label.c_str(), i == p.active, 0, ImVec2(ImGui::GetContentRegionAvail().x - buttons, 0)))
            activate_actor(i);
        if (ImGui::BeginPopupContextItem("actor_menu")) {  // on the name, where people right-click
            if (ImGui::MenuItem("Delete Actor")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i == p.active);
        const bool placing = place_actor_ == i;
        if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (icon_small_button("place", icon::kPlace,
                              "Place: move or turn this actor with a gizmo in the view (Rotate tool: turn about Z)"))
            place_actor_ = placing ? -1 : i;
        if (placing) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool hidden = a.hidden, locked = a.locked;
        // Each shows the actor's state; a click changes it.
        if (icon_small_button("hide", hidden ? icon::kHidden : icon::kShown, hidden ? "Hidden: click to show" : "Shown: click to hide") &&
            i != p.active)
            scene_edit(hidden ? "Show Actor" : "Hide Actor", [i, hidden](Project& pr) { pr.actors[i].hidden = !hidden; });
        ImGui::SameLine();
        if (icon_small_button("lock", locked ? icon::kLocked : icon::kUnlocked, locked ? "Locked: click to unlock" : "Unlocked: click to lock") &&
            i != p.active)
            scene_edit(locked ? "Unlock Actor" : "Lock Actor", [i, locked](Project& pr) { pr.actors[i].locked = !locked; });
        ImGui::PopID();
    }
    if (remove >= 0 && !scene_busy()) {
        scene_edit("Delete Actor", [remove](Project& pr) {
            if (remove == pr.active) set_active_actor(pr, remove == 0 ? 1 : 0);
            std::string gone = pr.actors[remove].name;
            pr.actors.erase(pr.actors.begin() + remove);
            if (pr.active > remove) --pr.active;
            for (int k = 0; k < int(pr.actors.size()); ++k)  // pins to the deleted actor go
                std::erase_if(actor_clip(pr, k).pins, [&](const Pin& pin) { return pin.target_actor == gone; });
        });
        // Indices into the actor list shift down past the deleted one.
        auto shift = [remove](int& k) { k = k == remove ? -1 : k > remove ? k - 1 : k; };
        shift(place_actor_);
        shift(pin_actor_);
        // The list may now be empty (one actor left is a plain project): nothing below may index it.
        ImGui::End();
        return;
    }

    // The active actor's settings.
    ImGui::SeparatorText(p.actors[p.active].name.c_str());
    Actor& cur = p.actors[p.active];
    char name[64];
    std::snprintf(name, sizeof name, "%s", cur.name.c_str());
    if (ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) && name[0] &&
        cur.name != name) {
        std::string from = cur.name, to = unique_name(p, name);
        scene_edit("Rename Actor", [from, to](Project& pr) {
            pr.actors[pr.active].name = to;
            for (int k = 0; k < int(pr.actors.size()); ++k)
                for (Pin& pin : actor_clip(pr, k).pins)
                    if (pin.target_actor == from) pin.target_actor = to;
        });
    }
    // Live while picking; one undo step when the mouse is released.
    float colour[3] = {cur.colour[0], cur.colour[1], cur.colour[2]};
    if (ImGui::ColorEdit3("Colour", colour, ImGuiColorEditFlags_NoInputs)) {
        if (colour_actor_ < 0) colour_actor_ = p.active, colour_start_ = cur.colour;
        if (colour_actor_ == p.active)
            for (int k = 0; k < 3; ++k) cur.colour[k] = colour[k];
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) finish_scene_drags();
    // Body (both hosts alike): None draws nothing, Ruth is the SL default body, or an imported mesh body. Older projects'
    // other Linden bodies still show by name.
    const char* current = cur.body.empty() ? "None" : cur.body.c_str();
    for (int k = 0; k < kBodyCount; ++k)
        if (cur.body == kBodyIds[k]) current = Body(k) == Body::SLDefault ? "Ruth" : kBodyNames[k];
    if (const MeshBody* mb = cur.body.rfind("mesh:", 0) == 0 ? find_mesh_body(cur.body.substr(5)) : nullptr) current = mb->name.c_str();
    if (ImGui::BeginCombo("Body", current)) {
        auto pick = [&](const std::string& id, const char* label) {
            if (ImGui::Selectable(label, cur.body == id)) scene_edit("Actor Body", [id](Project& pr) { pr.actors[pr.active].body = id; });
        };
        pick("", "None");
        pick(kBodyIds[int(Body::SLDefault)], "Ruth");
        for (int k = 0; k < int(bodies_.size()); ++k) {  // two bodies may share a name
            ImGui::PushID(k);
            pick("mesh:" + bodies_[k].id, bodies_[k].name.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Shown while another actor is edited. None: nothing, Ruth: the SL default body.");

    // Placement: drag the fields; one undo step per drag.
    ImGui::SeparatorText("Placement from the sit target");
    // The start state is taken before the widgets change anything, so a click that jumps the value still
    // records it; the whole drag is one undo step, applied to the actor it started on.
    const Actor before_fields = cur;
    float pos[3] = {float(cur.pos.x), float(cur.pos.y), float(cur.pos.z)}, rz = float(cur.rot_z);
    bool changed = ImGui::DragFloat3("Position (m)", pos, 0.005f, -20, 20, "%.3f");
    bool started = ImGui::IsItemActivated(), done = ImGui::IsItemDeactivated();
    changed |= ImGui::DragFloat("Turn (deg)", &rz, 0.5f, -360, 360, "%.1f");
    started |= ImGui::IsItemActivated(), done |= ImGui::IsItemDeactivated();
    if (started && field_drag_actor_ < 0) field_drag_actor_ = p.active, field_drag_start_ = before_fields;
    if (changed && field_drag_actor_ == p.active) {
        // The active actor is the view's frame: moving it moves the others the opposite way on screen.
        cur.pos = {pos[0], pos[1], pos[2]};
        cur.rot_z = rz;
    }
    if (done) finish_scene_drags();

    // GR-4: bind a point of this actor to a bone of another.
    ImGui::SeparatorText("Contact with another actor");
    std::vector<int> others;
    for (int k = 0; k < int(p.actors.size()); ++k)
        if (k != p.active) others.push_back(k);
    if (pin_actor_ < 0 || pin_actor_ >= int(p.actors.size()) || pin_actor_ == p.active) pin_actor_ = others[0];
    if (ImGui::BeginCombo("Other actor", p.actors[pin_actor_].name.c_str())) {
        for (int k : others)
            if (ImGui::Selectable(p.actors[k].name.c_str(), k == pin_actor_)) pin_actor_ = k;
        ImGui::EndCombo();
    }
    if (pin_bone_ < 0 || pin_bone_ >= skel_.size()) pin_bone_ = skel_.find("mWristRight");
    if (ImGui::BeginCombo("Their bone", skel_[pin_bone_].name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int k = 0; k < skel_.size(); ++k)
            if (!skel_[k].volume && ImGui::Selectable(skel_[k].name.c_str(), k == pin_bone_)) pin_bone_ = k;
        ImGui::EndCombo();
    }
    int sel = primary();
    ImGui::BeginDisabled(sel <= 0);
    if (ImGui::Button("Bind Selected Point to This Bone from Here")) {
        std::string why, other = p.actors[pin_actor_].name, bone = skel_[pin_bone_].name;
        bool ok = false;
        edit("Bind to " + other, [&](Clip& c) { ok = pin_to_actor(c, *rig_, frame_, sel, other, bone, shape(), why); });
        status(ok ? skel_[sel].name + " now follows " + other + "'s " + bone : why);
    }
    ImGui::EndDisabled();
    if (sel <= 0) ImGui::SetItemTooltip("Select the point that should follow, such as a hand");
    hint("Release it later with Release from Here, as for any pin.");
    ImGui::End();
}

}  // namespace vats
