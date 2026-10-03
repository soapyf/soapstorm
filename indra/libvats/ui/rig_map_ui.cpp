// Viewport Avatar Toolset - Map Rig to Second Life: a model's own bones mapped onto SL's skeleton, reviewed and edited.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 1b (RM-3..RM-5). The suggestion, the folding and the loading are the core's (rig_map.h);
// this window lists the file's bones with their SL joints, lets each be picked from a searchable list (mirrored to the
// other side), and shows the result live as the mesh body: the model skinned on SL's skeleton at its own joint
// positions, posable like any body. Apply saves <model>.rigmap.json beside the model and makes it a mesh body. Host
// calls only, so the viewer has it too.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "app.h"
#include "widgets.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/fbx.h"
#include "vats/rig_map.h"

namespace vats {

namespace {

// The preview body: a run-only mesh body over a cached model no file names.
constexpr const char* kPreviewBody = "run:rig-map-preview";
constexpr const char* kPreviewModel = "rig-map-preview:";

std::string stem_of(const std::string& path) {
    const std::string name = path.substr(path.find_last_of("/\\") + 1);
    return name.substr(0, name.rfind('.'));
}

bool contains_nocase(std::string_view hay, std::string_view needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != hay.end();
}

}  // namespace

struct RigMapUi {
    std::string path;               // the model
    std::vector<SourceBone> bones;  // its own armature
    std::vector<int> depth;         // per bone, for the tree's indent
    RigMap map, suggested;          // as edited; as the suggestion made it (Suggest Again, and the reasons)
    RigMapResult resolved;
    std::vector<std::string> problems;  // per bone, what check_rig_map flags: the row shows red
    double file_height = 0;         // the model's size at the file's own, as the mapping measures it (rig_map_size)
    double file_floor = 0;          // its lowest point at that size (a lying body is lifted onto the floor)
    std::string previous_body;      // the mesh body shown before the preview
    bool own_session = false;       // the preview set App::session_body_ (so closing clears it)
    bool due = true;                // the preview is rebuilt next frame
    std::string error;
    char filter[64] = "";
    char pick_filter[64] = "";
    int hovered = -1, selected = -1, picking = -1;  // bone rows
    bool mirror = true;
    char preset_name[64] = "";
    std::string note;  // what the last preset did
};

namespace {

const ImVec4 kWarn{1, 0.7f, 0.3f, 1};

std::string slot_joints(const SpareSlot& s) {
    return s.joints.size() == 1 ? s.joints[0] : s.joints.front() + ".." + s.joints.back();
}

// RM-8: what to know about a slot before using it, in one line each.
void slot_notes(const RigMapUi& ui, const Skeleton& skel, const SpareSlot& s, const std::vector<int>& chain) {
    if (!chain.empty()) {
        const SpareHang h = spare_hang(skel, ui.bones, ui.resolved, chain, s);
        if (!h.model.empty() && !h.sl.empty() && h.model != h.sl)
            ImGui::TextColored(kWarn, "Moves with %s in SL; the model hangs it from %s.", h.sl.c_str(), h.model.c_str());
    }
    if (s.carries_body)
        ImGui::TextColored(kWarn, "The body hangs from it: bending it bends everything above. Place parts here, don't animate them.");
    if (!s.worn.empty()) hint(("Worn " + s.worn + " move it too, and fight it.").c_str());
}

void apply_preset(RigMapUi& ui, const SparePreset& p, int start) {
    const int n = apply_spare_preset(ui.map, ui.bones, p, start);
    ui.note = p.name + ": " + (n ? std::to_string(n) + (n == 1 ? " chain put on" : " chains put on")
                                 : std::string("nothing fitted (no bone of that name, or the chain is in use)"));
    ui.due = ui.due || n > 0;
}

// The right-click menu of a bone row: its chain onto a spare slot, or off one.
void spare_menu(RigMapUi& ui, const Skeleton& skel, int bone, const std::vector<SparePreset>& saved) {
    const RigMapBone& m = ui.map.bones[size_t(bone)];
    if (!m.spare.empty()) {
        const SpareSlot* s = find_spare_slot(m.spare);
        if (ImGui::MenuItem(("Take Off the " + (s ? s->name : m.spare)).c_str())) clear_spare_chain(ui.map, m.spare), ui.due = true;
        ImGui::SetItemTooltip("Its bones fold into their parents again");
        return;
    }
    const std::vector<int> chain = spare_chain_from(ui.bones, bone);
    if (chain.empty()) return ImGui::TextDisabled("No weights at or below this bone: nothing to carry");
    if (ImGui::BeginMenu(("Use Spare Chain (" + std::to_string(chain.size()) + (chain.size() == 1 ? " bone)" : " bones)")).c_str())) {
        for (const SpareSlot& s : spare_slots()) {
            // Free, or used only by this chain's own bones: a ten-bone tail already on mTail1..6, whose last bones fold,
            // goes on again spread over all six.
            const bool own = !spare_slot_free(ui.map, s) && std::all_of(ui.map.bones.begin(), ui.map.bones.end(), [&](const RigMapBone& b) {
                return std::find(s.joints.begin(), s.joints.end(), b.target) == s.joints.end() ||
                       std::any_of(chain.begin(), chain.end(), [&](int c) { return ui.bones[size_t(c)].name == b.source; });
            });
            const bool fits = spare_slot_free(ui.map, s) || own;
            std::string label = s.name + "  " + slot_joints(s);
            label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
            if (ImGui::MenuItem(label.c_str(), nullptr, false, fits)) {
                use_spare_chain(ui.map, ui.bones, chain, s.id);
                ui.due = true;
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::Text("%d joints for %d bones%s", int(s.joints.size()), int(chain.size()),
                            chain.size() > s.joints.size() ? ": the weights spread along it" : "");
                if (!fits) ImGui::TextUnformatted("The model's own bones use it");
                if (own) ImGui::TextUnformatted("This chain is on it already: put on as a spare chain, all its bones bend it");
                slot_notes(ui, skel, s, {});
                ImGui::EndTooltip();
            }
        }
        ImGui::EndMenu();
    }
    ImGui::SetItemTooltip("%s and down its chain onto a Bento chain the body leaves free (a scarf on a wing,\na ponytail "
                          "on the tail): whatever animates that chain moves it.",
                          ui.bones[size_t(bone)].name.c_str());
    if (ImGui::BeginMenu("Spare Chain Preset")) {
        auto offer = [&](const SparePreset& p, const char* how) {
            if (ImGui::MenuItem(p.name.c_str())) apply_preset(ui, p, bone);
            ImGui::SetItemTooltip("%s", how);
        };
        for (const SparePreset& p : builtin_spare_presets())
            offer(p, p.chains.size() > 1 ? "From this bone, and its mirror-named bone for the other side"
                                         : "From this bone, on the side of the body it is on");
        if (!saved.empty()) ImGui::Separator();
        for (const SparePreset& p : saved) offer(p, "Its bones by name where this model has them, else from this bone");
        ImGui::EndMenu();
    }
}

// RM-8: the spare-chain finder. Every slot: free, used by the model's own bones, or holding a chain, with its label.
void draw_spare_chains(RigMapUi& ui, const Skeleton& skel, std::vector<SparePreset>& saved, const std::function<void()>& save) {
    int free = 0;
    for (const SpareSlot& s : spare_slots()) free += spare_slot_free(ui.map, s) && spare_chain_bones(ui.bones, ui.map, s.id).empty();
    const bool used = std::any_of(ui.map.bones.begin(), ui.map.bones.end(), [](const RigMapBone& b) { return !b.spare.empty(); });
    if (used) ImGui::SetNextItemOpen(true, ImGuiCond_Once);  // a mapping with spare chains opens on them
    if (!section_header(("Spare Chains (" + std::to_string(free) + " free)###spares").c_str(), false)) return;
    hint("SL chains this model leaves unused. Right-click a bone, such as the first of a scarf, ponytail or skirt, to put "
         "its chain on one: the chain's joints go where its bones are, and whatever animates the chain moves it.");
    if (ImGui::BeginTable("##slots", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Chain", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Joints", ImGuiTableColumnFlags_WidthFixed, 2.6f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Holds", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        for (const SpareSlot& s : spare_slots()) {
            ImGui::PushID(s.id.c_str());
            const std::vector<int> chain = spare_chain_bones(ui.bones, ui.map, s.id);
            const bool own = !spare_slot_free(ui.map, s);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (own) ImGui::TextDisabled("%s", s.name.c_str());
            else ImGui::TextUnformatted(s.name.c_str());
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(slot_joints(s).c_str());
                slot_notes(ui, skel, s, chain);
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d", int(s.joints.size()));
            ImGui::TableNextColumn();
            if (own) {
                ImGui::TextDisabled("in use");
                ImGui::SetItemTooltip("The model's own bones are on it");
            } else if (chain.empty()) {
                ImGui::TextDisabled("free");
            } else {
                char label[64];
                std::snprintf(label, sizeof label, "%s", ui.map.spare_labels[s.id].c_str());
                const float remove = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
                ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - remove, 3 * ImGui::GetFontSize()));
                if (ImGui::InputText("##label", label, sizeof label)) ui.map.spare_labels[s.id] = label;
                if (ImGui::IsItemDeactivatedAfterEdit()) ui.due = true;
                ImGui::SetItemTooltip("%d bones from %s; the name shown beside its joints, as in %s (%s)", int(chain.size()),
                                      ui.bones[size_t(chain.front())].name.c_str(), s.joints.front().c_str(), label);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) clear_spare_chain(ui.map, s.id), ui.due = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (const SpareSlot& s : spare_slots())  // the warnings of the chains in use, where they stay in sight
        if (const std::vector<int> chain = spare_chain_bones(ui.bones, ui.map, s.id); !chain.empty()) {
            ImGui::PushID(s.id.c_str());
            ImGui::TextUnformatted((ui.map.spare_labels[s.id] + " on the " + s.name + ":").c_str());
            ImGui::Indent();
            slot_notes(ui, skel, s, chain);
            ImGui::Unindent();
            ImGui::PopID();
        }
    // Presets: this mapping's chains kept for the next model, and the ones kept before.
    const SparePreset mine = spare_preset_from(ui.map, ui.bones, ui.preset_name);
    ImGui::SetNextItemWidth(std::min(12 * ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x * 0.45f));
    ImGui::InputTextWithHint("##preset", "scarf on wings", ui.preset_name, sizeof ui.preset_name);
    ImGui::SameLine();
    ImGui::BeginDisabled(mine.chains.empty() || !ui.preset_name[0]);
    if (ImGui::Button("Save as Preset")) {
        std::erase_if(saved, [&](const SparePreset& p) { return p.name == mine.name; });
        saved.push_back(mine);
        save();
        ui.note = "Saved the preset " + mine.name + ": right-click a bone of the next model, or Apply Preset";
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Keeps this model's spare chains (their slots, labels and bone names) to put on the next model");
    if (!saved.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Apply Preset")) ImGui::OpenPopup("presets");
        ImGui::SetItemTooltip("A saved preset, by its bones' names; where this model names them otherwise, from the bone "
                              "selected in the list");
        if (ImGui::BeginPopup("presets")) {
            for (const SparePreset& p : saved)
                if (ImGui::MenuItem(p.name.c_str())) apply_preset(ui, p, ui.selected);
            ImGui::EndPopup();
        }
    }
    if (!ui.note.empty()) hint(ui.note.c_str());
}

}  // namespace

void App::open_rig_map(const std::string& path) {
    if (rig_map_ui_) close_rig_map(false);
    auto ui = std::make_shared<RigMapUi>();
    ui->path = path;
    DaeModel plain;
    DaeReport report;
    std::string err;
    if (!load_mesh_file_as_is(path, skel_, plain, report, err))
        return message("Map Rig to Second Life", stem_of(path) + ": " + (err.empty() ? "could not be read" : err));
    if (report.bones.empty())
        return message("Map Rig to Second Life", stem_of(path) + " has no armature: there are no bones to map. Import it as a prop instead.");
    ui->bones = report.bones;
    for (const SourceBone& b : ui->bones) ui->depth.push_back(b.parent >= 0 ? ui->depth[size_t(b.parent)] + 1 : 0);
    ui->suggested = suggest_rig_map(skel_, ui->bones, retarget_tables());
    ui->map = ui->suggested;
    // A mapping saved beside the model is the starting point; bones it does not name keep the suggestion. A target the
    // suggestion agrees with keeps the suggestion's reason.
    RigMap saved;
    if (read_rig_map_file(rig_map_path(path), saved, err) && saved.bones.empty()) {
        ui->map.share = saved.share;  // a mapping that only keeps the model's look (SK-3): the suggestion stands
        ui->map.look = saved.look;
    } else if (!err.empty() || saved.bones.empty()) {
        if (!err.empty()) status("Map Rig: " + err + "; starting from a fresh suggestion");
    } else {
        if (saved.quadruped && saved.bento != ui->suggested.bento)  // saved in the other layout: suggest in that one
            ui->map = ui->suggested = suggest_rig_map(skel_, ui->bones, retarget_tables(), saved.bento);
        ui->map.height = saved.height;
        ui->map.turn = saved.turn;
        ui->map.along_ground = saved.along_ground;
        ui->map.spare_labels = saved.spare_labels;
        ui->map.share = saved.share;  // RM-10, and SK-3's parts and shape keys: Apply writes the mapping whole
        ui->map.look = saved.look;
        for (RigMapBone& b : ui->map.bones)
            if (const RigMapBone* s = saved.find(b.source); s && (s->target != b.target || s->spare != b.spare)) b = *s;
        status("Map Rig: " + stem_of(path) + " opened with the mapping saved beside it");
    }
    ui->previous_body = settings_.mesh_body;
    rig_map_ui_ = ui;
    show_rig_map_ = true;
    pending_tab_ = "###map-rig";  // docked among other tools (the Rig workspace): to the front
}

std::vector<SparePreset>& App::spare_presets() {
    if (!spare_presets_read_) {
        spare_presets_read_ = true;
        std::ifstream f(library_dir() + "spare-presets.json", std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string err;
        if (f && !parse_spare_presets_json(ss.str(), spare_presets_, err)) status("spare-presets.json: " + err);
    }
    return spare_presets_;
}

void App::save_spare_presets() {
    std::string why;
    if (!write_text(library_dir() + "spare-presets.json", write_spare_presets_json(spare_presets_), false, why))
        message("Preset not saved", why);
}

void App::open_example_mech() {
    namespace fs = std::filesystem;
    // A copy in the library, so the mapping saves beside it and the shipped files stay as they are.
    const fs::path from = u8path(data_dir_) / "bodies" / "mech", to = u8path(library_dir()) / "bodies" / "mech";
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const auto& e : fs::directory_iterator(from, ec))
        if (e.is_regular_file() && !fs::exists(to / e.path().filename(), ec)) fs::copy_file(e.path(), to / e.path().filename(), ec);
    if (!fs::exists(to / "George.dae"))
        return message("Map Rig to Second Life", "The example mech is missing (" + (from / "George.dae").string() + ").");
    open_rig_map((to / "George.dae").string());
}

void App::rig_map_preview() {
    RigMapUi& ui = *rig_map_ui_;
    ui.due = false;
    ui.resolved = resolve_rig_map(skel_, ui.bones, ui.map);
    ui.problems = check_rig_map(skel_, ui.bones, ui.map);
    auto model = std::make_unique<DaeModel>();
    DaeReport report;
    std::string err;
    if (!load_rig_mapped(ui.path, skel_, ui.map, *model, report, err, &ui.bones)) {
        ui.error = err.empty() ? "the model could not be read" : err;
        return;
    }
    ui.error.clear();
    if (ui.file_height <= 0) {  // the file's own size, for the size choice and the overlay: once the mapping maps anything
        RigMap own = ui.map;
        own.height = 0;
        DaeModel m;
        DaeReport r;
        own.along_ground = false;  // not lifted, to know by how much the preview is
        if (load_rig_mapped(ui.path, skel_, own, m, r, err, &ui.bones))
            ui.file_height = rig_map_size(m, ui.map), ui.file_floor = m.bounds_min.z;
    }
    const std::string key = kPreviewModel + ui.path;
    prop_models_[key] = std::move(model);
    prop_reports_[key] = report;
    if (!find_mesh_body(kPreviewBody)) bodies_.push_back({kPreviewBody, "", {}, {}});
    for (MeshBody& b : bodies_)
        if (b.id == kPreviewBody) b.name = stem_of(ui.path) + " (mapping)", b.parts = {key};
    if (!session_body_) {  // the settings file keeps the body chosen before
        session_body_.emplace(settings_.body, ui.previous_body);
        ui.own_session = true;
    }
    settings_.mesh_body = kPreviewBody;
    body_shapes_.clear();
    used_bones_key_.clear();
    invalidate_floor_cache();
}

void App::close_rig_map(bool applied) {
    show_rig_map_ = false;
    if (!rig_map_ui_) return;
    const RigMapUi& ui = *rig_map_ui_;
    prop_models_.erase(kPreviewModel + ui.path);
    prop_reports_.erase(kPreviewModel + ui.path);
    std::erase_if(bodies_, [](const MeshBody& b) { return b.id == kPreviewBody; });
    if (!applied && settings_.mesh_body == kPreviewBody) settings_.mesh_body = ui.previous_body;
    if (!applied && ui.own_session) session_body_.reset();
    body_shapes_.clear();
    used_bones_key_.clear();
    invalidate_floor_cache();
    rig_map_ui_.reset();
}

void App::apply_rig_map() {
    const std::shared_ptr<RigMapUi> keep = rig_map_ui_;
    const RigMapUi& ui = *keep;
    std::string why;
    const std::string file = rig_map_path(ui.path);
    RigMap map = ui.map, now;
    bool paint_dropped = false;
    if (read_rig_map_file(file, now, why)) {
        map.look = now.look;  // a look saved while the window was open
        // Weights painted over this mapping stay while it maps every bone as before; a changed mapping moves the weights
        // under them, so they go (the file's own weights come back).
        if (!now.scratch.wjoints.empty()) {
            const bool same = now.bones.size() == map.bones.size() &&
                              std::equal(now.bones.begin(), now.bones.end(), map.bones.begin(), [](const RigMapBone& a, const RigMapBone& b) {
                                  return a.source == b.source && a.target == b.target && a.spare == b.spare;
                              });
            if (same) map.scratch = now.scratch;
            else paint_dropped = true;
        }
    }
    if (!write_text(file, write_rig_map_json(map), false, why))
        return message("Mapping not saved", why + "\n\nThe mapping is saved beside the model (" + file +
                                                ") so it loads with it. Copy the model to a folder you can write to.");
    const std::string path = ui.path, previous = ui.previous_body;
    const bool own_session = ui.own_session;
    close_rig_map(true);
    prop_models_.erase(path);  // read afresh: through the mapping now
    prop_reports_.erase(path);
    for (MeshBody& b : bodies_)
        if (b.id.rfind("run:", 0) != 0 && b.parts == std::vector<std::string>{path}) {
            b.labels.clear();  // RM-8: what its spare chains hold now
            if (const DaeModel* m = prop_model(path)) b.labels = m->labels;
            save_bodies();
            use_mesh_body(b.id);
            return status("Saved " + stem_of(file) + ".json; " + b.name + " now uses it" +
                          (paint_dropped ? ". The mapping changed, so its painted weights were dropped" : ""));
        }
    import_body({path});
    if (paint_dropped) status("The mapping changed, so the weights painted over the old one were dropped");
    if (settings_.mesh_body == kPreviewBody) {  // the import failed: back to the body shown before
        settings_.mesh_body = previous;
        if (own_session) session_body_.reset();
    }
}

void App::draw_rig_map_window() {
    if (!show_rig_map_) {
        if (rig_map_ui_) close_rig_map(false);  // closed by its title bar's X
        return;
    }
    if (rig_map_ui_ && rig_map_ui_->due) rig_map_preview();
    const float fs = ImGui::GetFontSize();
    {  // Against the right edge, over the side panels rather than the view, so the preview stays in sight; tall for the
       // list. ImGui takes this the first time only.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float m = ImGui::GetFrameHeight() * 0.25f;
        const float w = std::min(std::clamp(0.32f * vp->WorkSize.x, 21 * fs, 30 * fs), vp->WorkSize.x - 2 * m);
        const float h = std::min(52 * fs, vp->WorkSize.y - ImGui::GetFrameHeight() - 2 * m);
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - w - m, vp->WorkPos.y + m}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({w, h}, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(dock_title("Map Rig to Second Life", "Map Rig", "map-rig").c_str(), &show_rig_map_)) return ImGui::End();
    tab_tooltip("Map Rig to Second Life");
    help_button("rig-any-model");
    auto pickers = [&] {  // the window's primary action while it is empty; beside Apply, a plain one
        if (rig_map_ui_ ? ImGui::Button("Open Model...") : primary_button("Open Model...", "", 0, icon::kOpen)) show_dialog(Dialog::MapRig);
        ImGui::SetItemTooltip("A rigged .fbx, .gltf, .glb or .dae with bones of its own");
        ImGui::SameLine();
        if (ImGui::Button("Example Mech")) open_example_mech();
        ImGui::SetItemTooltip("George, a CC0 mech by Quaternius (Animated Mech Pack):\ndigitigrade legs, four palm bones per hand, "
                              "IK feet and poles.\nA copy goes in your library, so your mapping saves beside it.");
    };
    if (!rig_map_ui_) {
        hint("Put any rigged model on SL's skeleton: open one to map its bones.");
        pickers();
        return ImGui::End();
    }
    RigMapUi& ui = *rig_map_ui_;
    ImGui::TextUnformatted(stem_of(ui.path).c_str());
    ImGui::SetItemTooltip("%s", ui.path.c_str());
    ImGui::SameLine();
    int mapped = 0, joints = 0;
    for (size_t i = 0; i < ui.bones.size() && i < ui.resolved.node.size(); ++i)
        mapped += ui.resolved.node[i] >= 0 && ui.resolved.folded_into[i] < 0;
    for (bool p : ui.resolved.places) joints += p;
    const std::string counts = std::to_string(ui.bones.size()) + " bones: " + std::to_string(mapped) + " on " +
                               std::to_string(joints) + " SL joints, " + std::to_string(ui.resolved.folded.size()) +
                               " fold, " + std::to_string(ui.resolved.dropped.size()) + " dropped";
    hint(counts.c_str());
    if (!ui.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", ui.error.c_str());
    for (const std::string& p : ui.resolved.problems) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", p.c_str());
    if (const auto red = std::count_if(ui.problems.begin(), ui.problems.end(), [](const std::string& p) { return !p.empty(); }))
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%d %s out of place (red): hover for why", int(red), red == 1 ? "bone looks" : "bones look");

    // Facing and size.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Facing");
    ImGui::SameLine(4.5f * fs);
    ImGui::SetNextItemWidth(-1);
    static const char* turns[] = {"as in the file", "turned 90 degrees", "turned 180 degrees", "turned 270 degrees"};
    if (ImGui::BeginCombo("##turn", turns[ui.map.turn & 3])) {
        for (int k = 0; k < 4; ++k)
            if (ImGui::Selectable(turns[k], ui.map.turn == k)) ui.map.turn = k, ui.due = true;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s%s", ui.suggested.facing.c_str(),
                          ui.map.turn == ui.suggested.turn ? "" : "\nYou turned it another way.");
    if (ui.suggested.quadruped) {  // the two ways SL quadrupeds are rigged
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Layout");
        ImGui::SameLine(4.5f * fs);
        for (const bool bento : {false, true}) {
            if (bento) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.5f * fs);  // under the first, as Size's are
            if (ImGui::RadioButton(bento ? "Bento Hind Legs" : "Front Legs on Arms", ui.map.bento == bento) && ui.map.bento != bento) {
                const RigMap keep = ui.map;
                ui.suggested = suggest_rig_map(skel_, ui.bones, retarget_tables(), bento);
                ui.map = ui.suggested;
                ui.map.height = keep.height, ui.map.turn = keep.turn, ui.map.along_ground = keep.along_ground, ui.due = true;
            }
            ImGui::SetItemTooltip("%s", bento ? "Back legs on mHindLimb1..4, front legs on SL's legs.\nFew SL animations move "
                                                "the hind limbs: pose them in VATs."
                                              : "Front legs on SL's arms, back legs on its legs.\nMost SL animations and AOs "
                                                "move these, so the creature moves with them.");
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size");
    ImGui::SameLine(4.5f * fs);
    const bool sl_size = ui.map.height > 0;
    if (ImGui::RadioButton("SL-like", sl_size) && !sl_size) ui.map.height = kSlAvatarHeight, ui.due = true;
    if (ui.map.along_ground)
        ImGui::SetItemTooltip("It lies along its spine, so its size is the longer side of its footprint (nose to tail, or "
                              "wingtip to wingtip):\nas long as SL's default avatar is tall (%.2f m), or the length you type",
                              kSlAvatarHeight);
    else
        ImGui::SetItemTooltip("Floor to the top of its head: as tall as SL's default avatar (%.2f m), or the height you type",
                              kSlAvatarHeight);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sl_size);
    ImGui::SetNextItemWidth(5 * fs);
    double h = sl_size ? ui.map.height : kSlAvatarHeight;
    if (ImGui::InputDouble("##height", &h, 0, 0, "%.2f m") && sl_size && h > 0.05 && h < 100) ui.map.height = h;
    if (ImGui::IsItemDeactivatedAfterEdit()) ui.due = true;
    ImGui::EndDisabled();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.5f * fs);
    char own[64];
    std::snprintf(own, sizeof own, "The file's own (%.2f m)", ui.file_height);
    if (ImGui::RadioButton(own, !sl_size) && sl_size) ui.map.height = 0, ui.due = true;
    ImGui::SetItemTooltip("The size the file gives it, in its own units");
    if (section_header("What VATs Decided", false)) {
        hint(ui.suggested.facing.c_str());
        for (const std::string& n : ui.suggested.notes) hint(n.c_str());
        for (const std::string& f : ui.resolved.folded) hint(("Folds: " + f + ".").c_str());
    }
    draw_spare_chains(ui, skel_, spare_presets(), [this] { save_spare_presets(); });
    ImGui::Separator();

    // The bones, as the file nests them.
    ImGui::SetNextItemWidth(std::min(12 * fs, ImGui::GetContentRegionAvail().x * 0.5f));
    filter_input("##filter", "Filter bones", ui.filter, sizeof ui.filter);
    ImGui::SameLine();
    ImGui::Checkbox("Mirror edits", &ui.mirror);
    ImGui::SetItemTooltip("Picking a joint for a left bone picks the mirrored joint for its right bone too, and back");
    const int shown = ui.hovered >= 0 ? ui.hovered : ui.selected;  // the bone the line under the list explains
    ui.hovered = -1;
    auto assign = [&](int i, const std::string& target) {
        auto put = [&](RigMapBone& b, const std::string& t) {
            b.target = t, b.spare.clear(), b.confidence = 100, b.reason = t.empty() ? "you chose none" : "you chose it";
        };
        put(ui.map.bones[size_t(i)], target);
        if (const std::string other = mirror_bone_name(ui.bones[size_t(i)].name); ui.mirror && !other.empty())
            if (RigMapBone* o = ui.map.find(other)) put(*o, target.empty() ? "" : Skeleton::mirror_name(target));
        ui.due = true;
    };
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2 + ImGui::GetFrameHeightWithSpacing() * 2.2f;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##bones", 3, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y - footer, 8 * fs)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("SL joint", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 1.6f * fs);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(ui.bones.size()); ++i) {
            const SourceBone& b = ui.bones[size_t(i)];
            const RigMapBone& m = ui.map.bones[size_t(i)];
            if (ui.filter[0] && !contains_nocase(b.name, ui.filter) && !contains_nocase(m.target, ui.filter)) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float indent = float(ui.depth[size_t(i)]) * 0.45f * fs + 0.01f;
            ImGui::Indent(indent);
            if (ImGui::Selectable(b.name.c_str(), ui.selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                ui.selected = ui.selected == i ? -1 : i;
            if (ImGui::IsItemHovered()) ui.hovered = i;
            if (ImGui::BeginPopupContextItem("spare")) {  // RM-8
                spare_menu(ui, skel_, i, spare_presets());
                ImGui::EndPopup();
            }
            ImGui::Unindent(indent);
            ImGui::TableNextColumn();
            const int folds = ui.resolved.folded_into.size() > size_t(i) ? ui.resolved.folded_into[size_t(i)] : -1;
            const std::string label = !m.target.empty() ? m.target + (m.spare.empty() ? "" : " (" + ui.map.spare_labels[m.spare] + ")")
                                    : folds >= 0       ? "into " + ui.bones[size_t(folds)].name
                                                       : "(none)";
            const bool problem = size_t(i) < ui.problems.size() && !ui.problems[size_t(i)].empty();
            const bool styled = m.target.empty() || problem;
            if (styled) ImGui::PushStyleColor(ImGuiCol_Text, problem ? ImVec4(1, 0.45f, 0.4f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::SmallButton((label + "###pick").c_str())) ui.picking = i, ui.pick_filter[0] = 0, ImGui::OpenPopup("pick");
            if (styled) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ui.hovered = i;
            if (ui.picking == i && ImGui::BeginPopup("pick")) {
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(14 * fs);
                filter_input("##pf", "Search SL joints", ui.pick_filter, sizeof ui.pick_filter);
                ImGui::BeginChild("##joints", ImVec2(14 * fs, 18 * fs));
                if (ImGui::Selectable("(none: fold or drop)", m.target.empty())) assign(i, ""), ImGui::CloseCurrentPopup();
                auto offer = [&](const std::string& name) {
                    if (ui.pick_filter[0] && !contains_nocase(name, ui.pick_filter)) return;
                    if (ImGui::Selectable(name.c_str(), name == m.target)) assign(i, name), ImGui::CloseCurrentPopup();
                };
                for (int j = 0; j < skel_.joint_count(); ++j) offer(skel_[j].name);
                for (const CollisionVolume& v : skel_.volumes()) offer(v.name);
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            // "you" for your own picks and a saved mapping's; a bone already named as SL's is the suggestion's 100.
            const bool yours = m.confidence >= 100 && (m.reason.rfind("you ", 0) == 0 || m.reason == "from the mapping file");
            if (problem) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", yours ? "you" : std::to_string(m.confidence).c_str());
            else if (yours) ImGui::TextDisabled("you");
            else if (m.confidence < 50) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%d", m.confidence);
            else ImGui::Text("%d", m.confidence);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // Why the bone under the pointer (or the one picked) goes where it goes.
    if (shown >= 0 && shown < int(ui.bones.size())) {
        const RigMapBone& m = ui.map.bones[size_t(shown)];
        const std::string& problem = size_t(shown) < ui.problems.size() ? ui.problems[size_t(shown)] : std::string();
        const std::string why = m.source + ": " + m.reason +
                                (problem.empty() || m.reason.find(problem) != std::string::npos ? "" : "; but it is " + problem + ": check it") + ".";
        hint(why.c_str());
    } else {
        hint("Hover a bone for why it goes where it does; click its SL joint to pick another.");
    }

    if (primary_button("Apply", "", 0, icon::kApply)) {
        apply_rig_map();
        return ImGui::End();
    }
    ImGui::SetItemTooltip("Saves the mapping beside the model (%s) and makes the model a mesh body, posable at once",
                          (stem_of(ui.path) + ".rigmap.json").c_str());
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        close_rig_map(false);
        return ImGui::End();
    }
    ImGui::SameLine();
    if (ImGui::Button("Suggest Again")) {
        const double height = ui.map.height;
        ui.map = ui.suggested, ui.map.height = height, ui.due = true;
    }
    ImGui::SetItemTooltip("Every bone back to VATs' suggestion (your size and layout stay)");
    pickers();
    ImGui::End();
}

// The bone the list hovers (or picked) where it is in the preview: its own head, carried by the SL joint its weights go
// to (or its nearest mapped parent's), the way the skin carries its vertices.
void App::draw_rig_map_overlay(ImDrawList* dl) const {
    if (!rig_map_ui_ || !show_rig_map_ || settings_.mesh_body != kPreviewBody) return;
    const RigMapUi& ui = *rig_map_ui_;
    auto it = prop_models_.find(kPreviewModel + ui.path);
    const DaeModel* m = it != prop_models_.end() ? it->second.get() : nullptr;
    if (!m || !m->rigged || ui.resolved.node.size() != ui.bones.size()) return;
    const double k = ui.map.height > 0 && ui.file_height > 1e-9 ? ui.map.height / ui.file_height : 1;
    const Quat turn = Quat::axis_angle({0, 0, 1}, ui.map.turn * kPi / 2);
    const Vec3 lift{0, 0, ui.map.along_ground ? -ui.file_floor * k : 0};
    for (int i : {ui.selected, ui.hovered}) {
        if (i < 0 || i >= int(ui.bones.size())) continue;
        int a = i;
        while (a >= 0 && ui.resolved.node[size_t(a)] < 0) a = ui.bones[size_t(a)].parent;
        const int n = a >= 0 ? ui.resolved.node[size_t(a)] : -1;
        if (n < 0 || n >= skel_.joint_count() || size_t(n) >= m->binds.size() || size_t(n) >= globals_.size()) continue;
        const Vec3 at = (globals_[size_t(n)] * m->binds[size_t(n)].inverse()).apply(turn.rotate(ui.bones[size_t(i)].bind.pos) * k + lift);
        double x, y;
        if (!projector_.to_screen(at, x, y)) continue;
        const ImVec2 c{float(x), float(y)};
        const ImU32 col = i == ui.hovered ? IM_COL32(120, 230, 255, 255) : IM_COL32(255, 230, 60, 255);
        dl->AddCircle(c, 9, IM_COL32(10, 12, 14, 220), 0, 4);
        dl->AddCircle(c, 9, col, 0, 2);
        const std::string& name = ui.bones[size_t(i)].name;
        dl->AddText(ImVec2(c.x + 12, c.y - 9), IM_COL32(10, 12, 14, 230), name.c_str());
        dl->AddText(ImVec2(c.x + 11, c.y - 10), col, name.c_str());
    }
}

}  // namespace vats
