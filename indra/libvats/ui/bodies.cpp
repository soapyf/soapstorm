// Viewport Avatar Toolset - mesh bodies from devkits: rigged .dae or .fbx parts that replace the Linden mesh.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 1 (BD-1, BD-2, BD-4). Bodies live in the user's library only
// (<library>/bodies.json); devkits are never copied, shipped or uploaded.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "app.h"
#include "profile.h"
#include "widgets.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/face_fit.h"
#include "vats/fbx.h"
#include "vats/picker.h"
#include "vats/rig_map.h"
#include "vats/soft_body.h"

namespace vats {

namespace {

std::string stem_of(const std::string& path) {
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    return name.substr(0, name.rfind('.'));
}

}  // namespace

void App::load_bodies() {
    bodies_.clear();
    std::ifstream f(library_dir() + "bodies.json", std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    Json j;
    std::string err;
    const Json* list = parse_json(ss.str(), j, err) && j.is_object() ? j.find("bodies") : nullptr;
    if (!list || !list->is_array()) return;
    for (const Json& b : list->arr) {
        const Json *id = b.find("id"), *name = b.find("name"), *parts = b.find("parts");
        if (!id || !id->is_string() || !parts || !parts->is_array()) continue;
        MeshBody body{id->str, name && name->is_string() ? name->str : id->str, {}, {}};
        for (const Json& p : parts->arr)
            if (p.is_string()) body.parts.push_back(p.str);
        if (const Json* labels = b.find("labels"); labels && labels->is_object())
            for (const auto& [joint, label] : labels->obj)
                if (label.is_string()) body.labels[joint] = label.str;
        bodies_.push_back(std::move(body));
    }
}

void App::save_bodies() const {
    Json list = Json::array();
    for (const MeshBody& b : bodies_) {
        if (b.id.rfind("run:", 0) == 0) continue;  // --mesh-body: this run only
        Json parts = Json::array();
        for (auto& p : b.parts) parts.arr.push_back(p);
        Json o = Json::object();
        o.set("id", b.id);
        o.set("name", b.name);
        o.set("parts", std::move(parts));
        if (!b.labels.empty()) {
            Json labels = Json::object();
            for (const auto& [joint, label] : b.labels) labels.set(joint, label);
            o.set("labels", std::move(labels));
        }
        list.arr.push_back(std::move(o));
    }
    Json j = Json::object();
    j.set("vats-bodies", 1);
    j.set("bodies", std::move(list));
    std::ofstream(library_dir() + "bodies.json", std::ios::binary) << write_json(j);
}

const App::MeshBody* App::mesh_body() const { return find_mesh_body(settings_.mesh_body); }

void App::sync_reused_bones() {
    std::map<std::string, std::string> labels;
    if (const MeshBody* b = mesh_body()) {
        for (const std::string& path : b->parts)
            if (const DaeModel* m = prop_model(path)) labels.insert(m->labels.begin(), m->labels.end());
        for (const auto& [joint, label] : b->labels)  // the body's own renames, for joints its parts still reuse
            if (auto it = labels.find(joint); it != labels.end()) it->second = label;
    }
    if (labels == bone_labels_) return;
    bone_labels_ = std::move(labels);
    std::vector<int> nodes;
    for (const auto& [joint, label] : bone_labels_)
        if (const int n = skel_.find(joint); n >= 0) nodes.push_back(n);
    skel_.set_reused(nodes);
}

std::string App::bone_label(const std::string& bone, bool plain) const {
    if (const char* soft = soft_body_name(bone)) return std::string(soft) + " (" + bone + ")";  // "Butt (BUTT)"
    const auto it = bone_labels_.find(bone);
    if (it != bone_labels_.end() && !it->second.empty()) return bone + " (" + it->second + ")";  // what it holds says more
    const std::string p = plain && settings_.plain_bone_names ? plain_bone_name(bone) : "";
    return p.empty() ? bone : bone + " \xc2\xb7 " + p;
}

std::string App::bone_label(int node, bool plain) const {
    return node >= 0 && node < skel_.size() ? bone_label(skel_[node].name, plain) : "";
}

std::string App::plain_label(int node) const {
    const std::string full = bone_label(node), bare = bone_label(node, false);
    return full.size() > bare.size() ? full.substr(bare.size() + 4) : "";  // after " · "
}

std::string App::picker_label(int node) const {
    const auto it = node >= 0 && node < skel_.size() ? bone_labels_.find(skel_[node].name) : bone_labels_.end();
    const std::string name = node >= 0 && node < skel_.size() ? picker_bone_label(skel_[node].name) : "";
    return it == bone_labels_.end() || it->second.empty() ? name : name + " (" + it->second + ")";
}

void App::set_bone_label(const std::string& joint, const std::string& label) {
    MeshBody* body = nullptr;
    for (MeshBody& b : bodies_)
        if (b.id == settings_.mesh_body) body = &b;
    if (!body) return;
    // The whole chain the joint is on takes the name, in the body and in the mapping beside each part that maps it.
    for (const SpareSlot& s : spare_slots()) {
        if (std::find(s.joints.begin(), s.joints.end(), joint) == s.joints.end()) continue;
        for (const std::string& j : s.joints)
            if (bone_labels_.count(j)) body->labels[j] = label;
        for (const std::string& path : body->parts) {
            RigMap map;
            std::string err;
            if (!read_rig_map_file(rig_map_path(path), map, err) || !map.spare_labels.count(s.id)) continue;
            map.spare_labels[s.id] = label;
            if (!write_text(rig_map_path(path), write_rig_map_json(map), false, err)) status("Not saved in the mapping: " + err);
        }
    }
    save_bodies();
}

void App::cli_mesh_body(const std::string& path) {
    const std::string id = "run:" + path;
    if (!find_mesh_body(id)) bodies_.push_back({id, stem_of(path), {path}, {}});
    if (!session_body_) session_body_.emplace(settings_.body, settings_.mesh_body);  // the saved body stays
    settings_.mesh_body = id;
}

const App::MeshBody* App::find_mesh_body(const std::string& id) const {
    for (const MeshBody& b : bodies_)
        if (!id.empty() && b.id == id) return &b;
    return nullptr;
}

const App::MeshBody* App::swap_body() const {
    const MeshBody* b = host_.world_view() ? mesh_body() : nullptr;
    if (b)  // a devkit on a drive not mounted: your avatar stays
        for (const std::string& path : b->parts)
            if (const_cast<App*>(this)->prop_model(path)) return b;  // ponytail: the model cache is the only state touched
    return nullptr;
}

const Shape* App::shape() const {
    if (const Shape* worn = worn_shape()) {  // the viewer: your actor is the avatar in the world (unless swapped)
        if (!editing_other()) return worn;
        return actor_shape(doc_.project.active);  // another actor: the proportions of the body it is drawn with
    }
    return view_body_shape();
}

const Shape* App::view_body_shape() const {
    const Shape* base = mesh_.shape(body_);
    const MeshBody* b = mesh_body();
    return b ? mesh_body_shape(*b, base) : base;
}

std::string App::current_body_id() const {
    const std::string key = actor_body_key(doc_.project.active);
    if (key.starts_with("mesh:")) return key.substr(5);
    if (!settings_.mesh_body.empty()) return settings_.mesh_body;
    return "sl-default";
}

const RigConstraints* App::constraints() const {
    return posing_limits(settings_.respect_joint_limits, show_suggest_limits_, limit_preview_mode_, pending_limits_,
                         current_body_id(), doc_.project.body_constraints(current_body_id()));
}

// The limits the limit tools show and edit: the suggestions while Suggest Limits previews them, else the applied
// ones. Unlike constraints() this ignores Respect Joint Limits: turning that off stops posing using them, it doesn't
// hide them from the tools that edit them.
const RigConstraints* App::edited_constraints() const {
    return editing_pending_limits() ? &pending_limits_.limits : doc_.project.body_constraints(current_body_id());
}

bool App::bone_used(int node) const {
    const MeshBody* b = mesh_body();
    if (!b || body_ == Body::SkeletonOnly || node < 0 || node >= skel_.size() ||
        skel_[node].category == Category::AttachmentPoints)
        return true;
    std::string key;
    for (const std::string& p : b->parts)  // the model's address too: a re-import of the same file is a new model
        key += p + "@" + std::to_string(prop_models_.count(p) ? reinterpret_cast<std::uintptr_t>(prop_models_.at(p).get()) : 0) + "|";
    if (key != used_bones_key_ || used_bones_.size() != size_t(skel_.size())) {
        used_bones_.assign(size_t(skel_.size()), 0);
        // Children come after their parents, so walking backwards carries each used bone up its chain.
        for (int n = skel_.size() - 1; n >= 0; --n) {
            if (!used_bones_[size_t(n)] && is_joint_weighted(n)) used_bones_[size_t(n)] = 1;
            if (used_bones_[size_t(n)] && skel_[n].parent >= 0 && !skel_[skel_[n].parent].volume)
                used_bones_[size_t(skel_[n].parent)] = 1;
        }
        used_bones_key_ = key;
    }
    return used_bones_[size_t(node)];
}

void App::reveal_rigged_groups() {
    if (body_ == Body::SkeletonOnly) return;
    std::string key = "linden";  // the system body: weighted to the classic joints only
    if (mesh_body()) bone_used(0), key = used_bones_key_;  // brings the used set up to date for the body shown
    if (key == revealed_groups_key_) return;
    revealed_groups_key_ = key;
    // Once per body. The groups it is weighted to come on (a body weighted to collision volumes, fitted mesh or soft-body
    // helpers, shows them as shells); the ones VATs switched on for an earlier body and this one does not use go off
    // again, so a scarf on the wings leaves no wings on the next body. A group you switch yourself stays as you set it.
    std::array<bool, 9> used{};
    for (int n = 0; n < skel_.size(); ++n)
        if (skel_[n].category != Category::AttachmentPoints && is_joint_weighted(n))
            used[skel_[n].volume ? 8 : static_cast<int>(skel_[n].category)] = true;
    for (int g = 0; g < 9; ++g) {
        bool& shown = g == 8 ? show_volumes_ : show_category_[size_t(g)];
        if (used[size_t(g)] && !shown) shown = auto_shown_[size_t(g)] = true;
        else if (!used[size_t(g)] && auto_shown_[size_t(g)]) shown = auto_shown_[size_t(g)] = false;
    }
}

bool App::is_joint_weighted(int node) const {
    if (body_ == Body::SkeletonOnly) return node_visible(node);
    // Asked many times a frame (shells, ground, balance, physics): worked out once per body, look and weight edit,
    // checked once a frame. Each answer used to scan every vertex (a 179k-vertex body cost ~17 ms a frame).
    if (weighted_frame_ != ImGui::GetFrameCount() || weighted_nodes_.size() != size_t(skel_.size())) {
        weighted_frame_ = ImGui::GetFrameCount();
        std::string key = std::to_string(int(body_)) + "|" + std::to_string(weights_generation_) + "|" +
                          std::to_string(look_generation_) + "|";
        const MeshBody* b = mesh_body();
        if (b)
            for (const std::string& p : b->parts)
                key += p + "@" + std::to_string(prop_models_.count(p) ? reinterpret_cast<std::uintptr_t>(prop_models_.at(p).get()) : 0) + "|";
        else
            key += "linden@" + std::to_string(reinterpret_cast<std::uintptr_t>(&mesh_)) + "#" + std::to_string(mesh_.influences().size());
        if (key != weighted_key_ || weighted_nodes_.size() != size_t(skel_.size())) {
            weighted_key_ = key;
            weighted_nodes_.assign(size_t(skel_.size()), 0);
            if (b) {
                std::vector<char> sk40(size_t(dae_index_count(skel_)) + 1, 0);
                for (const std::string& path : b->parts) {
                    auto it = prop_models_.find(path);
                    if (it == prop_models_.end() || !it->second || !it->second->rigged) continue;
                    const DaeModel& mdl = *it->second;
                    for (size_t i = 0; i < mdl.joints.size(); ++i)
                        if (mdl.weights[i] > 1e-4 && mdl.joints[i] >= 0 && size_t(mdl.joints[i]) < sk40.size()) sk40[size_t(mdl.joints[i])] = 1;
                }
                for (int n = 0; n < skel_.size(); ++n) {
                    int idx = n;
                    if (skel_[n].volume) {
                        idx = -1;
                        for (size_t vi = 0; vi < skel_.volumes().size(); ++vi)
                            if (skel_.volumes()[vi].node == n) idx = dae_volume(skel_, static_cast<int>(vi));
                    }
                    weighted_nodes_[size_t(n)] = idx >= 0 && size_t(idx) < sk40.size() && sk40[size_t(idx)];
                }
            } else {
                for (const auto& inf : mesh_.influences()) {
                    if (inf.a >= 0 && inf.a < skel_.size() && (1.0f - inf.blend) > 1e-4f) weighted_nodes_[size_t(inf.a)] = 1;
                    if (inf.b >= 0 && inf.b < skel_.size() && inf.blend > 1e-4f) weighted_nodes_[size_t(inf.b)] = 1;
                }
                for (int n = 0; n < skel_.size(); ++n)  // the system body: its joints and volumes only, as before
                    if (n >= skel_.joint_count() && !skel_[n].volume) weighted_nodes_[size_t(n)] = 0;
            }
        }
    }
    return node >= 0 && size_t(node) < weighted_nodes_.size() && weighted_nodes_[size_t(node)];
}

std::vector<std::vector<float>> App::shown_body_skin(const std::vector<Xform>& globals, const Shape* sh) const {
    std::vector<std::vector<float>> parts;
    std::vector<float> nrm;
    // The world view shows your worn avatar, whose mesh the UI does not have, unless View > Body swaps one in.
    const MeshBody* b = host_.world_view() ? (swap_shown() && !editing_other() ? swap_body() : nullptr) : mesh_body();
    if (b) {
        harmonize_body(*b);
        for (const std::string& path : b->parts)
            // ponytail: prop_model caches meshes and is not const; the cache is the only state it touches here.
            if (const DaeModel* m = const_cast<App*>(this)->prop_model(path); m && m->rigged)
                skin_prop(*m, skel_, globals, sh, parts.emplace_back(), nrm);
    } else if (!host_.world_view() && body_ != Body::SkeletonOnly) {
        mesh_.skin(globals, sh, parts.emplace_back(), nrm);
    }
    return parts;
}

// Skinned afresh from globals_ whenever they change: the skin the view last drew may be of another pose (a picker
// thumbnail, a live pose in the world view) or of none (the world view draws no body of the UI's).
const MeshContactFloor& App::current_contact_floor() const {
    if (!floor_cache_valid_ || floor_globals_ != globals_ || floor_shape_ != shape()) {
        VATS_PROFILE("contact floor");
        // The body the view drew this frame was skinned in this very pose: its vertices give the floor without
        // skinning the whole body a second time (5 ms a frame on a 179k-vertex body).
        std::vector<std::span<const float>> drawn;
        const MeshBody* b = !host_.world_view() ? mesh_body() : nullptr;
        if (b && mesh_skin_body_ == b->id && mesh_skin_shape_ == shape() && mesh_skin_globals_ == globals_)
            for (const std::string& path : b->parts) {
                const DaeModel* m = const_cast<App*>(this)->prop_model(path);
                if (!m || !m->rigged) continue;
                const auto it = mesh_body_skin_pos_.find(path);
                if (it == mesh_body_skin_pos_.end() || it->second.size() != m->positions.size()) {
                    drawn.clear();
                    b = nullptr;
                    break;
                }
                drawn.emplace_back(it->second);
            }
        else
            b = nullptr;
        if (b) {
            cached_floor_ = compute_mesh_contact_floor(drawn);
        } else {
            const auto parts = shown_body_skin(globals_, shape());
            cached_floor_ = compute_mesh_contact_floor(std::vector<std::span<const float>>(parts.begin(), parts.end()));
        }
        floor_globals_ = globals_;
        floor_shape_ = shape();
        floor_cache_valid_ = true;
    }
    return cached_floor_;
}

double App::contact_height() const {
    const MeshContactFloor& floor = current_contact_floor();
    if (floor.has_mesh) return floor.lowest_z;
    std::function<bool(int)> weighted = [this](int node) { return is_joint_weighted(node); };
    Balance b = balance_of(skel_, globals_, shape(), &weighted);
    return b.contact ? b.ground.z : 0.0;
}

// The floor the shown body stands on: its lowest point in the rest pose, so the grid stays put while the body
// jumps or crouches. 0 (SL's ground) when no body mesh is shown.
double App::rest_floor() const {
    const Shape* sh = shape();
    const MeshBody* b = mesh_body();
    const std::string key = (b ? b->id : "") + "|" + std::to_string(int(body_)) + "|" +
                            std::to_string(host_.world_view() && swap_shown() && !editing_other()) + "|" +
                            std::to_string(look_generation_);  // a part hidden or a shape key set: the soles may move
    if (key != rest_floor_key_ || sh != rest_floor_shape_) {
        const auto parts = shown_body_skin(skel_.global_pose(Pose(skel_.size()), sh), sh);
        const MeshContactFloor f = compute_mesh_contact_floor(std::vector<std::span<const float>>(parts.begin(), parts.end()));
        rest_floor_ = f.has_mesh ? f.lowest_z : 0.0;
        rest_floor_key_ = key;
        rest_floor_shape_ = sh;
    }
    return rest_floor_;
}

// The host stands your avatar's root over the world's ground as it stands SL's default body (its soles just at 0
// here); a swapped body whose soles rest elsewhere (longer legs, a creature) sinks or floats by the difference.
double App::swap_lift() const {
    if (!default_floor_) {
        std::vector<float> pos, nrm;
        mesh_.skin(skel_.global_pose(Pose(skel_.size()), nullptr), nullptr, pos, nrm);
        const MeshContactFloor f = compute_mesh_contact_floor(pos);
        default_floor_ = f.has_mesh ? f.lowest_z : 0.0;
    }
    return *default_floor_ - rest_floor();
}

// All parts of one body share one axis decision: a part with too little evidence of its own (eyes, teeth,
// lashes: a few joints near the centre line) is turned the way the part that could decide was turned.
void App::harmonize_body(const MeshBody& b) const {
    App* self = const_cast<App*>(this);  // ponytail: the model cache is the only state touched
    const DaeModel* decided = nullptr;
    for (const std::string& path : b.parts)
        if (const DaeModel* m = self->prop_model(path); m && m->rigged && m->turn_decided) {
            decided = m;
            break;
        }
    if (!decided) return;
    for (const std::string& path : b.parts) {
        if (auto it = self->prop_models_.find(path); it != self->prop_models_.end() && it->second)
            apply_rig_turn(*it->second, decided->turn_binds, decided->turn_vertices);
        if (auto it = self->prop_sources_.find(path); it != self->prop_sources_.end() && it->second)  // so a rebuild keeps it
            apply_rig_turn(*it->second, decided->turn_binds, decided->turn_vertices);
    }
}

const Shape* App::mesh_body_shape(const MeshBody& b, const Shape* base) const {
    auto [it, fresh] = body_shapes_.try_emplace({b.id, base});
    if (fresh) {
        harmonize_body(b);
        std::vector<const DaeModel*> parts;
        // ponytail: prop_model caches meshes and is not const; the cache is the only state it touches here.
        for (const std::string& path : b.parts) parts.push_back(const_cast<App*>(this)->prop_model(path));
        Shape s;
        const bool moved = shape_from_binds(skel_, parts, base, s);  // s is base when no joint moved
        if (rig_axes_from_parts(skel_, parts, s) || moved) it->second = std::move(s);  // and its rig axes
    }
    return it->second ? &*it->second : base;
}

void App::use_mesh_body(const std::string& id) {
    settings_.mesh_body = id;
    session_body_.reset();  // chosen in the app: saved as usual
    save_settings();
    const MeshBody* b = mesh_body();
    if (host_.world_view())  // spec 09 build 32: the body swap
        return status(!b ? "Showing your avatar again"
                      : swap_body() ? "Showing " + b->name + " in your avatar's place, on your screen only (nothing is sent)"
                                    : b->name + ": none of its parts could be read, so your avatar stays");
    status(b ? "Showing " + b->name + " (the Linden body is hidden)" : "Showing the Linden body");
}

// BD-1, BD-4: every part must be a rigged mesh; the report lists joints VATs does not know. A part rigged to bones of
// its own (RM-4) is mapped first: Map Rig to Second Life opens on it.
void App::import_body(const std::vector<std::string>& paths) {
    // The same files again: that body, its parts read afresh, not a second copy of it.
    for (const MeshBody& b : bodies_)
        if (same_files(b.parts, paths)) {
            for (const std::string& part : b.parts) prop_models_.erase(part), prop_model(part);
            body_shapes_.clear();
            used_bones_key_.clear();
            use_mesh_body(b.id);
            return status(b.name + " is in Inventory > Bodies already: its files are read again and it is shown");
        }
    MeshBody body;
    body.name = body_name(paths);
    body.id = "body-" + std::to_string(host_.ticks_ns());
    std::string report, problems, to_map, to_rig;
    for (const std::string& path : paths) {
        DaeModel model;
        DaeReport dae_report;
        std::string err;
        if (!load_mesh_file(path, skel_, model, dae_report, err)) {
            problems += "- " + stem_of(path) + ": " + (err.empty() ? "could not be read" : err) + "\n";
            continue;
        }
        const DaeModel* m = &model;
        if (rig_needs_mapping(skel_, dae_report)) {
            if (to_map.empty()) to_map = path;
            problems += "- " + stem_of(path) + ": rigged to bones of its own, not SL's: map them first\n";
            continue;
        }
        if (!m->rigged) {  // 08 RG-14: no skeleton at all; rig it from scratch
            if (to_rig.empty()) to_rig = path;
            problems += "- " + stem_of(path) + ": not rigged; rig it from scratch first\n";
            continue;
        }
        body.parts.push_back(path);
        report += "- " + stem_of(path) + " (" + std::to_string(m->triangle_count()) + " triangles)\n";
        for (auto& j : dae_report.unmapped_joints) report += "    weights to an unknown joint: " + j + "\n";
    }
    if (body.parts.empty()) {
        if (!to_map.empty()) {
            open_rig_map(to_map);
            return status(stem_of(to_map) + " has bones of its own: map them onto SL's in Map Rig to Second Life");
        }
        if (!to_rig.empty()) {
            open_rig_scratch(to_rig);
            return status(stem_of(to_rig) + " has no skeleton: place SL's in it in Rig a Model from Scratch");
        }
        return message("No body imported", problems.empty() ? "No files were chosen." : problems);
    }
    // A part that failed to load earlier (a drive not mounted yet) is cached as missing: load it afresh, and
    // drop cached body shapes, which may have been built without it.
    for (auto& p : body.parts) prop_models_.erase(p), prop_model(p);
    for (auto& p : body.parts)  // RM-8: what its spare chains hold, kept with the body (and renamed there)
        if (const DaeModel* m = prop_model(p)) body.labels.insert(m->labels.begin(), m->labels.end());
    body_shapes_.clear();
    used_bones_key_.clear();  // the new model may sit at the old one's address
    bodies_.push_back(body);
    save_bodies();
    use_mesh_body(body.id);
    message("Imported body " + body.name, "Parts:\n" + report + (problems.empty() ? "" : "\nLeft out:\n" + problems) +
                                              "\nThe body stays in your own library and is never shared.");
    if (!to_map.empty()) open_rig_map(to_map);  // after the switch, so Cancel comes back to the body just imported
    else if (!to_rig.empty()) open_rig_scratch(to_rig);
}

const std::vector<Xform>* App::swap_live_globals() {
    Pose live;
    if (!real_avatar_mode() || !swap_shown() || !host_.live_pose(skel_, live) || live.rot.size() != size_t(skel_.size()))
        return nullptr;
    swap_live_ = skel_.global_pose(live, view_body_shape());  // the body's own joints (as shape() is while swapped)
    return &swap_live_;
}

std::string App::real_mode_swap_note() const {
    const MeshBody* b = swap_body();
    if (!b) return "";
    return settings_.viewer_keep_swap ? "; " + b->name + " stays in your avatar's place and moves as your avatar does"
                                      : "; your real avatar shows meanwhile";
}

void App::draw_mesh_body(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices, const std::vector<Xform>* globals,
                         bool keep_live) {
    VATS_PROFILE("draw mesh body");
    const MeshBody* b = mesh_body();
    if (!b) return;
    harmonize_body(*b);
    for (const std::string& path : b->parts) {
        Prop p;
        p.path = path;
        p.rigged = true;
        draw_prop(p, verts, indices, globals, globals ? view_body_shape() : nullptr, 1.f, {}, nullptr, weight_heat(path));
        if (!globals) {
            mesh_body_skin_pos_[path] = prop_skin_pos_;
            invalidate_floor_cache();
        }
        if (keep_live) live_body_skin_pos_[path] = prop_skin_pos_;  // the world view's body as drawn: alt-cam picks it
    }
    if (!globals) mesh_skin_globals_ = globals_, mesh_skin_shape_ = shape(), mesh_skin_body_ = b->id;  // the floor reuses it
}

void App::draw_bodies_section() {
    const bool matches = std::any_of(bodies_.begin(), bodies_.end(), [&](const MeshBody& b) { return inv_match(b.name); });
    if (!inventory_section("Bodies", matches)) return;
    const bool world = host_.world_view();
    hint(world ? "Double-click a body to show it in your avatar's place." : "Double-click a body to pose on it.");
    if (ImGui::Selectable(world ? "Your avatar" : "Linden body", settings_.mesh_body.empty(), ImGuiSelectableFlags_AllowDoubleClick) &&
        ImGui::IsMouseDoubleClicked(0))
        use_mesh_body("");
    int remove = -1;
    for (int i = 0; i < int(bodies_.size()); ++i) {
        const MeshBody& b = bodies_[i];
        if (!inv_match(b.name)) continue;
        ImGui::PushID(i);
        const bool shown = settings_.mesh_body == b.id;
        if (ImGui::Selectable(b.name.c_str(), shown, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0))
            use_mesh_body(b.id);
        if (ImGui::BeginItemTooltip()) {  // its files; its mesh objects are its Parts (SK-1)
            ImGui::TextDisabled("%s", count_noun(b.parts.size(), "file").c_str());
            for (auto& part : b.parts) ImGui::TextUnformatted(part.c_str());
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Use")) use_mesh_body(b.id);
            // 08 RG-14, RG-15: any rigged body's weights can be painted; one rigged from scratch here is reopened there.
            bool rigged = false;
            for (const std::string& part : b.parts)
                if (const auto r = prop_reports_.find(part); r != prop_reports_.end() && r->second.rigged) rigged = true;
            if (rigged && ImGui::MenuItem("Paint Weights...")) use_mesh_body(b.id), show_paint_ = true;
            if (b.parts.size() == 1 && ImGui::MenuItem("Rig from Scratch...")) open_rig_scratch(b.parts[0]);
            ImGui::SetItemTooltip("Place SL's skeleton in it by markers again, or for the first time (its own rig is set aside)");
            if (ImGui::MenuItem("Remove from Inventory")) remove = i;
            ImGui::SetItemTooltip("The mesh files stay where they are");
            ImGui::EndPopup();
        }
        if (shown) {  // the shown body's parts and shape keys, under it
            ImGui::Indent();
            draw_body_look_section();
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        if (bodies_[remove].id == settings_.mesh_body) use_mesh_body("");
        bodies_.erase(bodies_.begin() + remove);
        save_bodies();
    }
    // One button for the three ways a body comes in.
    if (ImGui::Button((std::string("Add Body ") + icon::kDown).c_str())) ImGui::OpenPopup("##add_body");
    ImGui::SetItemTooltip("Import a devkit's rigged parts, map a model's own rig onto SL's, or rig a model from scratch");
    if (ImGui::BeginPopup("##add_body")) {
        if (menu_item_icon(icon::kImport, "Import Body Parts (.dae, .fbx, .gltf, .glb)...")) show_dialog(Dialog::ImportBody);
        ImGui::SetItemTooltip("Choose every part at once: body, head, hands and feet");
        if (menu_item_icon(icon::kSwap, "Map Rig to Second Life...")) show_rig_map_ = true;
        ImGui::SetItemTooltip("A model rigged to bones of its own (a game character, a creature): map them onto SL's skeleton");
        if (menu_item_icon(icon::kIkFk, "Rig a Model from Scratch...")) show_rig_scratch_ = true;  // 08 RG-14
        ImGui::SetItemTooltip("A model with no skeleton for SL: drag markers onto its joints and VATs rigs and weights it");
        ImGui::EndPopup();
    }
}

// ---- Spec 08 SK: parts and shape keys ----------------------------------------------------------------------------

DaeModel* App::part_source(const std::string& path) {
    prop_model(path);  // loads the source with the shown model
    const auto it = prop_sources_.find(path);
    return it != prop_sources_.end() ? it->second.get() : nullptr;
}

void App::part_source_changed(const std::string& path) {
    const auto src = prop_sources_.find(path);
    const auto shown = prop_models_.find(path);
    if (src == prop_sources_.end() || !src->second || shown == prop_models_.end() || !shown->second) return;
    const MeshLook look = look_for_path(path);
    *shown->second = shown_model(*src->second, look);  // in place: the model keeps its address, so its caches stay its own
    prop_looks_[path] = look;
    mesh_body_skin_pos_.erase(path);  // picking: its triangles index the new vertices
    ++weights_generation_;            // the weight glow, per vertex
    ++look_generation_;               // the rest floor
    ++paint_generation_;              // the weight brush's copy of it
    used_bones_key_.clear();          // Hide Unused Bones: a hidden part's bones
    invalidate_floor_cache();
    rig_check_stale();  // the rig export's check and joint tables
}

MeshLook App::mesh_look(const MeshBody& b) const {
    if (const auto it = doc_.project.mesh_looks.find(b.id); it != doc_.project.mesh_looks.end()) return it->second;
    MeshLook look;  // each mapping names its own file's parts and keys, so together they are the body's
    for (const std::string& path : b.parts)
        if (const auto r = prop_reports_.find(path); r != prop_reports_.end()) {
            look.hidden.insert(r->second.look.hidden.begin(), r->second.look.hidden.end());
            look.keys.insert(r->second.look.keys.begin(), r->second.look.keys.end());
        }
    return look;
}

MeshLook App::look_for_path(const std::string& path) const {
    // The body shown first, then any body with the file. ponytail: a file in two bodies shows the first one's look in
    // both; give the cache a key per body if that ever matters.
    const MeshBody* owner = nullptr;
    if (const MeshBody* b = mesh_body(); b && std::count(b->parts.begin(), b->parts.end(), path)) owner = b;
    for (const MeshBody& b : bodies_)
        if (!owner && std::count(b.parts.begin(), b.parts.end(), path)) owner = &b;
    if (owner && doc_.project.mesh_looks.count(owner->id)) return doc_.project.mesh_looks.at(owner->id);
    const auto r = prop_reports_.find(path);
    return r != prop_reports_.end() ? r->second.look : MeshLook{};
}

void App::sync_mesh_looks() {
    for (auto& [path, model] : prop_models_)
        if (model && prop_sources_.count(path))
            if (const MeshLook look = look_for_path(path); look != prop_looks_[path]) part_source_changed(path);
}

void App::save_looks_to_mappings() {
    const MeshBody* b = mesh_body();
    if (!b) return;
    const MeshLook look = mesh_look(*b);
    for (const std::string& path : b->parts) {
        const DaeModel* src = part_source(path);
        if (!src) continue;
        const MeshLook own = own_look(*src, look);
        RigMap map;
        std::string err;
        const std::string file = rig_map_path(path);
        if (!read_rig_map_file(file, map, err) && (!err.empty() || own.empty())) continue;  // broken, or nothing to keep
        if (map.look == own) continue;
        map.look = own;
        if (!write_text(file, write_rig_map_json(map), false, err)) status("Not saved in the mapping: " + err);
        else prop_reports_[path].look = own;
    }
}

void App::record_look_step(const std::string& label, std::map<std::string, MeshLook> before) {
    Project& p = doc_.project;
    if (before == p.mesh_looks) return;
    scratch_end(false);  // PT-2: scene steps go in the document's own history
    doc_.history.record_scene(label, p.clip, {p.actors, p.active, p.clips, p.active_clip, p.joint_limits, std::move(before)}, p.clip,
                              {p.actors, p.active, p.clips, p.active_clip, p.joint_limits, p.mesh_looks});
    mark_dirty();
    save_looks_to_mappings();
}

// Spec 08 SK-5: the Bento face pose closest to a shape key, into the pose library (and so the expression pack).
// ponytail: fitted on the body's file with most of the key; a key spread over several files fits on that one only.
const DaeModel* App::shape_key_model(const std::string& key) {
    const MeshBody* b = mesh_body();
    if (!b) return nullptr;
    const DaeModel* model = nullptr;
    size_t most = 0;
    for (const std::string& path : b->parts)
        if (const DaeModel* src = part_source(path)) {
            size_t n = 0;
            for (const DaeShapeKey& k : src->shape_keys) n += k.name == key ? k.vertices.size() : 0;
            if (n > most) most = n, model = src;
        }
    return model;
}

bool App::face_shape_key(const std::string& key) {
    const MeshBody* b = mesh_body();
    if (!b) return false;
    const auto [it, added] = face_keys_.try_emplace(b->id + '\n' + key, false);
    if (added)  // more than half its motion on face-weighted vertices: "Body - Emaciated" offered a face pose before
        if (const DaeModel* model = shape_key_model(key)) it->second = shape_key_face_share(skel_, *model, key) > 0.5;
    return it->second;
}

void App::fit_face_pose_to_key(const std::string& key) {
    const MeshBody* b = mesh_body();
    if (!b) return;
    const DaeModel* model = shape_key_model(key);
    if (!model) return;
    FaceFitOptions opt;
    opt.positions = face_positions();
    const FaceFit fit = fit_face_pose(skel_, view_body_shape(), *model, mesh_look(*b), key, opt);
    if (!fit.why.empty())
        return message("No face pose for " + key, "VATs cannot make a face pose for this shape key: " + fit.why +
                                                      ".\n\nSecond Life plays bones, not shape keys, so a key reaches SL only through the "
                                                      "Bento face bones its vertices are weighted to.");
    store_library_item(face_fit_pose(skel_, fit, key));
    char how[256];
    std::snprintf(how, sizeof how, "the face bones explain %.0f%% of it, %.1f mm off on average (at most %.1f mm)",
                  std::max(0.0, fit.explained) * 100, fit.residual_rms * 1000, fit.max_error * 1000);
    status("Saved face pose " + key + " to Inventory > Poses: " + how);
    if (fit.explained < 0.8)
        message("Face pose " + key, "Saved to Inventory > Poses, but " + std::string(how) +
                                        ". The key moves its vertices in ways the face bones weighted to them cannot follow; "
                                        "a head weighted to more Bento face bones fits closer.");
}

void App::draw_body_look_section(bool in_properties) {
    const MeshBody* b = mesh_body();
    if (!b) return;
    std::vector<std::string> parts, keys;  // over every file of the body, in file order
    std::map<std::string, double> initial;
    for (const std::string& path : b->parts)
        if (const DaeModel* src = part_source(path)) {
            for (const DaePart& p : src->parts)
                if (std::find(parts.begin(), parts.end(), p.name) == parts.end()) parts.push_back(p.name);
            for (const DaeShapeKey& k : src->shape_keys)
                if (initial.try_emplace(k.name, k.initial).second) keys.push_back(k.name);
        }
    if (parts.size() < 2 && keys.empty()) return;
    // In Properties (every workspace) under a header naming the body; Inventory > Bodies draws it under the body's row.
    if (in_properties && !section_header(("Body: " + b->name + "###body_look").c_str(), true)) return;
    struct Indent {  // under the Body header in Properties, Parts and Shape Keys read as its contents
        bool on;
        explicit Indent(bool o) : on(o) { if (on) ImGui::Indent(); }
        ~Indent() { if (on) ImGui::Unindent(); }
    } indent(in_properties);
    Project& p = doc_.project;
    const std::string id = b->id;
    // A body the project has no look for yet shows its mappings': the step starts from that, so undo comes back to it.
    auto materialise = [&] {
        if (!p.mesh_looks.count(id)) p.mesh_looks[id] = mesh_look(*b);
    };
    const MeshLook look = mesh_look(*b);

    // Folded or open as left, next time too: a body with 7 parts and 50 keys pushed Poses a long way down.
    const bool parts_open = parts.size() > 1 && inventory_section("Parts");
    if (parts.size() > 1)
        ImGui::SetItemTooltip("The mesh objects in the body's files. A hidden part is not drawn, picked or stood on, and is "
                              "left out of Export Rigged Mesh for SL");
    if (parts_open) {
        for (const std::string& name : parts) {
            bool shown = !look.hidden.count(name);
            if (ImGui::Checkbox((name.empty() ? "(unnamed)##part" : name + "##part").c_str(), &shown)) {
                materialise();
                auto before = p.mesh_looks;
                if (shown) p.mesh_looks[id].hidden.erase(name);
                else p.mesh_looks[id].hidden.insert(name);
                record_look_step((shown ? "Show Part " : "Hide Part ") + name, std::move(before));
            }
        }
        if (!look.hidden.empty() && ImGui::Button("Show All")) {
            materialise();
            auto before = p.mesh_looks;
            p.mesh_looks[id].hidden.clear();
            record_look_step("Show All Parts", std::move(before));
        }
    }

    const bool keys_open = !keys.empty() && inventory_section("Shape Keys");
    if (!keys.empty())
        ImGui::SetItemTooltip("The model's shape keys (morph targets). Export Rigged Mesh for SL bakes them into the mesh as set "
                              "here: SL meshes have no shape keys of their own");
    if (keys_open) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", shape_key_filter_.c_str());
        ImGui::SetNextItemWidth(-1);
        if (filter_input("##keyfilter", "Filter shape keys", buf, sizeof buf)) shape_key_filter_ = buf;
        auto lower = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string filter = lower(shape_key_filter_);
        // Grouped by the name's prefix where it has one: "Body - Obese" under Body, "vrc.v_aa" under vrc.
        std::vector<std::pair<std::string, std::vector<std::string>>> groups;
        for (const std::string& k : keys) {
            if (!filter.empty() && lower(k).find(filter) == std::string::npos) continue;
            const std::string g = shape_key_group(k, nullptr);
            auto it = std::find_if(groups.begin(), groups.end(), [&](auto& e) { return e.first == g; });
            if (it == groups.end()) it = groups.insert(groups.end(), {g, {}});
            it->second.push_back(k);
        }
        for (const auto& [group, names] : groups) {
            const bool grouped = groups.size() > 1 || !group.empty();
            if (grouped) {
                if (!filter.empty()) ImGui::SetNextItemOpen(true);
                ImGui::Indent();
                const bool open = section_header((group.empty() ? "Other" : group).c_str());
                ImGui::Unindent();
                if (!open) continue;
            }
            for (const std::string& name : names) {
                ImGui::PushID(name.c_str());
                const auto set = look.keys.find(name);
                float v = float(set != look.keys.end() ? set->second : initial[name]);
                size_t rest = 0;
                shape_key_group(name, &rest);
                const std::string shown = name.substr(rest);
                std::string fmt;  // the name inside the slider, '%' escaped
                for (char c : shown) fmt += c == '%' ? std::string("%%") : std::string(1, c);
                ImGui::SetNextItemWidth(-1);
                const bool changed = slider_float("##key", &v, 0.f, 1.f, (fmt + "  %.2f").c_str());
                if (ImGui::IsItemActivated()) {
                    materialise();
                    look_drag_before_ = p.mesh_looks;
                    look_drag_open_ = true;
                }
                if (changed) p.mesh_looks[id].keys[name] = v;  // live: sync_mesh_looks rebuilds the mesh
                if (ImGui::IsItemDeactivated() && look_drag_open_) {
                    look_drag_open_ = false;
                    record_look_step("Shape Key " + name, std::move(look_drag_before_));
                }
                const bool face = face_shape_key(name);
                ImGui::SetItemTooltip(face ? "%s\nRight-click: make a face pose from it" : "%s", name.c_str());
                if (face && ImGui::BeginPopupContextItem("##keymenu")) {
                    if (ImGui::MenuItem("Make Face Pose from This Key")) fit_face_pose_to_key(name);
                    ImGui::SetItemTooltip("Fit the Bento face bones to this shape key, for a pose SL can play: saved in Inventory > "
                                          "Poses and offered in Export Expression Pack");
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
    }
}

void App::draw_body_parts_menu() {
    const MeshBody* b = mesh_body();
    if (!b) return;
    std::vector<std::string> parts;
    for (const std::string& path : b->parts)
        if (const DaeModel* src = part_source(path))
            for (const DaePart& pt : src->parts)
                if (std::find(parts.begin(), parts.end(), pt.name) == parts.end()) parts.push_back(pt.name);
    if (parts.size() < 2) return;
    subheading("Parts");
    Project& p = doc_.project;
    const MeshLook look = mesh_look(*b);
    for (const std::string& name : parts) {
        const bool shown = !look.hidden.count(name);
        if (ImGui::MenuItem((name.empty() ? "(unnamed)##part" : name + "##part").c_str(), nullptr, shown)) {
            if (!p.mesh_looks.count(b->id)) p.mesh_looks[b->id] = look;
            auto before = p.mesh_looks;
            if (shown) p.mesh_looks[b->id].hidden.insert(name);
            else p.mesh_looks[b->id].hidden.erase(name);
            record_look_step((shown ? "Hide Part " : "Show Part ") + name, std::move(before));
        }
    }
    ImGui::TextDisabled("Shape keys: Properties > Body");
}

}  // namespace vats
