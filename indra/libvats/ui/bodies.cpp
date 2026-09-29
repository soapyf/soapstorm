// Viewport Avatar Toolset - mesh bodies from devkits: rigged .dae or .fbx parts that replace the Linden mesh.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 1 (BD-1, BD-2, BD-4). Bodies live in the user's library only
// (<library>/bodies.json); devkits are never copied, shipped or uploaded.
#include <fstream>
#include <sstream>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/fbx.h"

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
        MeshBody body{id->str, name && name->is_string() ? name->str : id->str, {}};
        for (const Json& p : parts->arr)
            if (p.is_string()) body.parts.push_back(p.str);
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
        list.arr.push_back(std::move(o));
    }
    Json j = Json::object();
    j.set("vats-bodies", 1);
    j.set("bodies", std::move(list));
    std::ofstream(library_dir() + "bodies.json", std::ios::binary) << write_json(j);
}

const App::MeshBody* App::mesh_body() const { return find_mesh_body(settings_.mesh_body); }

void App::cli_mesh_body(const std::string& path) {
    const std::string id = "run:" + path;
    if (!find_mesh_body(id)) bodies_.push_back({id, stem_of(path), {path}});
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
    for (const std::string& path : b.parts)
        if (auto it = self->prop_models_.find(path); it != self->prop_models_.end() && it->second)
            apply_rig_turn(*it->second, decided->turn_binds, decided->turn_vertices);
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

// BD-1, BD-4: every part must be a rigged mesh; the report lists joints VATs does not know.
void App::import_body(const std::vector<std::string>& paths) {
    MeshBody body;
    body.name = paths.empty() ? "Body" : stem_of(paths[0]);
    body.id = "body-" + std::to_string(host_.ticks_ns());
    std::string report, problems;
    for (const std::string& path : paths) {
        DaeModel model;
        DaeReport dae_report;
        std::string err;
        if (!load_mesh_file(path, skel_, model, dae_report, err)) {
            problems += "- " + stem_of(path) + ": " + (err.empty() ? "could not be read" : err) + "\n";
            continue;
        }
        const DaeModel* m = &model;
        if (!m->rigged) {
            problems += "- " + stem_of(path) + ": not rigged to the SL skeleton, left out\n";
            continue;
        }
        body.parts.push_back(path);
        report += "- " + stem_of(path) + " (" + std::to_string(m->triangle_count()) + " triangles)\n";
        for (auto& j : dae_report.unmapped_joints) report += "    weights to an unknown joint: " + j + "\n";
    }
    if (body.parts.empty()) return message("No body imported", problems.empty() ? "No files were chosen." : problems);
    // A part that failed to load earlier (a drive not mounted yet) is cached as missing: load it afresh, and
    // drop cached body shapes, which may have been built without it.
    for (auto& p : body.parts) prop_models_.erase(p), prop_model(p);
    body_shapes_.clear();
    bodies_.push_back(body);
    save_bodies();
    use_mesh_body(body.id);
    message("Imported body " + body.name, "Parts:\n" + report + (problems.empty() ? "" : "\nLeft out:\n" + problems) +
                                              "\nThe body stays in your own library and is never shared.");
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

void App::draw_mesh_body(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices, const std::vector<Xform>* globals) {
    const MeshBody* b = mesh_body();
    if (!b) return;
    harmonize_body(*b);
    for (const std::string& path : b->parts) {
        Prop p;
        p.path = path;
        p.rigged = true;
        draw_prop(p, verts, indices, globals, globals ? view_body_shape() : nullptr);
    }
}

void App::draw_bodies_section() {
    if (!inventory_section("Bodies")) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool world = host_.world_view();
    ImGui::TextWrapped(world ? "Import a devkit's rigged parts, or any rigged mesh such as a creature, and double-click it to "
                               "show it in your avatar's place, on your screen only."
                             : "Pose on your own mesh body by importing its devkit's rigged parts. Double-click to switch.");
    ImGui::PopStyleColor();
    if (ImGui::Selectable(world ? "Your avatar" : "Linden body", settings_.mesh_body.empty(), ImGuiSelectableFlags_AllowDoubleClick) &&
        ImGui::IsMouseDoubleClicked(0))
        use_mesh_body("");
    int remove = -1;
    for (int i = 0; i < int(bodies_.size()); ++i) {
        const MeshBody& b = bodies_[i];
        if (!inv_match(b.name)) continue;
        ImGui::PushID(i);
        std::string label = b.name + "  (" + std::to_string(b.parts.size()) + (b.parts.size() == 1 ? " part)" : " parts)");
        if (ImGui::Selectable(label.c_str(), settings_.mesh_body == b.id, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(0))
            use_mesh_body(b.id);
        if (ImGui::IsItemHovered()) {
            std::string tip;
            for (auto& p : b.parts) tip += p + "\n";
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Use")) use_mesh_body(b.id);
            if (ImGui::MenuItem("Remove from Inventory")) remove = i;
            ImGui::SetItemTooltip("The mesh files stay where they are");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        if (bodies_[remove].id == settings_.mesh_body) use_mesh_body("");
        bodies_.erase(bodies_.begin() + remove);
        save_bodies();
    }
    if (icon_label_button(icon::kImport, "Import Body Parts (.dae, .fbx)...")) show_dialog(Dialog::ImportBody);
    ImGui::SetItemTooltip("Choose every part at once: body, head, hands and feet");
}

}  // namespace vats
