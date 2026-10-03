// Viewport Avatar Toolset - Rig a Model from Scratch: SL's skeleton placed in a mesh that has none, by markers you drag.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 6 (RG-13, RG-14). The guess, the fit and bone heat are the core's (auto_rig.h,
// bone_heat.h); this window stands the model up, shows the markers in the view (each named by the SL joint it places,
// coloured by how sure the guess is), lets you drag them (mirrored by default), and shows the result live as the mesh
// body: the skeleton fitted to the markers, weighted by bone heat on a worker thread. Apply saves the rig beside the
// model and makes it a mesh body. Host calls only, so the viewer has it too.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "app.h"
#include "widgets.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/auto_rig.h"
#include "vats/fbx.h"
#include "vats/rig_map.h"
#include "vats/weight_paint.h"

namespace vats {

namespace {

constexpr const char* kPreviewBody = "run:rig-scratch-preview";
constexpr const char* kPreviewModel = "rig-scratch-preview:";

std::string stem_of(const std::string& path) {
    const std::string name = path.substr(path.find_last_of("/\\") + 1);
    return name.substr(0, name.rfind('.'));
}

// A marker's colour: yours white, else by the guess's confidence.
ImU32 marker_colour(int confidence) {
    if (confidence >= 100) return IM_COL32(235, 245, 255, 255);
    if (confidence >= 65) return IM_COL32(110, 220, 120, 255);
    if (confidence >= 40) return IM_COL32(250, 210, 70, 255);
    return IM_COL32(255, 120, 80, 255);
}

}  // namespace

// The weights worked out on a worker thread: copies of the model and the rig, so the UI keeps going.
struct RigWeightsJob {
    DaeModel model;
    ScratchRig rig;
    ScratchWeighReport report;
    std::atomic<bool> cancel{false}, done{false};
    std::mutex lock;
    double progress = 0;
    std::string stage;
    bool ok = false;
    std::thread worker;
    ~RigWeightsJob() {
        cancel = true;
        if (worker.joinable()) worker.join();
    }
};

struct RigScratchUi {
    std::string path;
    DaeModel as_is;        // the file as loaded, its own armature (if any) set aside
    std::vector<std::string> parts;
    RigMap map;            // height, turn, look and the rig (markers, groups, joints, weights)
    std::string facing;    // why that turn
    ScratchPlacement placement;
    DaeModel placed;       // stood up, every part shown, shape keys at the look's values: what the markers sit in
    std::string previous_body;
    bool own_session = false;
    bool due = true;        // the preview is rebuilt next frame
    bool weigh_due = true;  // the weights are worked out again once nothing is dragged
    bool hold_rest = true;  // the preview stands at rest while markers show
    bool mirror = true;
    std::string hovered, dragged, selected;  // marker ids
    Vec3 drag_depth_point;                   // where a dragged marker was, for a drag in the view's plane
    ImVec2 drag_press;                       // where the press was; it moves the marker once the pointer leaves it
    bool drag_moved = false;
    std::unique_ptr<RigWeightsJob> job;
    ScratchWeighReport report;
    std::vector<std::string> notes;  // the fit's
    std::string error;
    bool painted_warned = false;     // painted weights: working them out again waits for Weigh Again
};

namespace {

// The model stood up for the rig as it is now (turn, size, look's keys); markers and weights refer to it.
void place(RigScratchUi& ui) {
    ui.placement = scratch_placement(ui.as_is, ui.map.turn, ui.map.height);
    DaeModel m = ui.as_is;
    place_model(m, ui.placement);
    MeshLook keys = ui.map.look;
    keys.hidden.clear();  // every part: weights for all of them, hidden or not
    ui.placed = shown_model(m, keys);
}

// Re-place after a change of facing or size, carrying the markers along with the model.
void replace(RigScratchUi& ui) {
    const ScratchPlacement old = ui.placement;
    place(ui);
    for (auto& [id, mk] : ui.map.scratch.markers) mk.pos = ui.placement.apply(old.undo(mk.pos));
    ui.due = ui.weigh_due = true;
}

}  // namespace

void App::open_rig_scratch(const std::string& path) {
    if (rig_scratch_ui_) close_rig_scratch(false);
    auto ui = std::make_shared<RigScratchUi>();
    ui->path = path;
    DaeReport report;
    std::string err;
    if (!load_mesh_file_as_is(path, skel_, ui->as_is, report, err))
        return message("Rig from Scratch", stem_of(path) + ": " + (err.empty() ? "could not be read" : err));
    if (ui->as_is.triangle_count() == 0) return message("Rig from Scratch", stem_of(path) + " has no triangles to rig.");
    ui->as_is.rigged = false;  // its own armature, if it has one, is set aside: the rig is SL's, from scratch
    for (const DaePart& p : ui->as_is.parts) ui->parts.push_back(p.name);
    RigMap saved;
    const bool have = read_rig_map_file(rig_map_path(path), saved, err);
    if (!err.empty()) status("Rig from Scratch: " + err + "; starting afresh");
    ui->map.look = have ? saved.look : MeshLook{};
    if (have && saved.scratch.active()) {
        ui->map = saved;
        ui->map.bones.clear();
        if (!settle_scratch_weights(skel_, ui->map.scratch, ui->as_is.vertex_count()))
            ui->map.scratch.wjoints.clear(), ui->map.scratch.weights.clear();
        ui->weigh_due = !ui->map.scratch.weighted(ui->as_is.vertex_count());
        ui->painted_warned = saved.scratch.painted;
        status("Rig from Scratch: " + stem_of(path) + " opened with the rig saved beside it");
    } else {
        ui->map.height = kSlAvatarHeight;
        ui->map.turn = guess_scratch_turn(ui->as_is, ui->facing);
    }
    place(*ui);
    if (ui->map.scratch.markers.empty()) {
        // The tail, wings, hind limbs and ears ticked when the shape shows them (an obvious tail was left unticked).
        std::vector<std::string> found;
        ui->map.scratch.groups = guess_rig_groups(ui->placed, &found);
        for (const std::string& f : found) ui->notes.push_back("Ticked under Bones: " + f);
        ui->map.scratch.markers = guess_markers(ui->placed, ui->map.scratch.groups);
        ui->map.scratch.transfer = suggest_transfers(ui->placed);
        if (!ui->map.scratch.transfer.empty())
            ui->notes.push_back("Parts worn over another copy its weights (Weights by part); change it there.");
    }
    if (have && !saved.bones.empty())
        ui->notes.push_back("The model has a Map Rig mapping beside it; Apply replaces it with this rig (its look stays).");
    ui->previous_body = settings_.mesh_body;
    rig_scratch_ui_ = ui;
    show_rig_scratch_ = true;
    pending_tab_ = "###rig-scratch";  // docked among other tools (the Rig workspace): to the front
    reveal_node(skel_.find("mPelvis"));  // the body's bones show, so the fitted skeleton reads in the view
}

void App::open_unrigged_example() {
    namespace fs = std::filesystem;
    // A copy in the library without its mapping, so the rig saves beside it and the shipped files stay as they are.
    const fs::path from = u8path(data_dir_) / "bodies" / "mech" / "George.dae", to = u8path(library_dir()) / "bodies" / "mech-unrigged";
    std::error_code ec;
    fs::create_directories(to, ec);
    if (!fs::exists(to / "George.dae")) fs::copy_file(from, to / "George.dae", ec);
    if (!fs::exists(to / "George.dae"))
        return message("Rig from Scratch", "The example mech is missing (" + from.string() + ").");
    open_rig_scratch((to / "George.dae").string());
}

bool App::rig_scratch_holds_rest() const {
    return rig_scratch_ui_ && show_rig_scratch_ && rig_scratch_ui_->hold_rest && settings_.mesh_body == kPreviewBody;
}

void App::rig_scratch_preview() {
    RigScratchUi& ui = *rig_scratch_ui_;
    ui.due = false;
    const ScratchFit fit = fit_scratch_joints(skel_, ui.placed, ui.map.scratch);
    ui.map.scratch.joints = fit.joints;
    ui.notes.erase(std::remove_if(ui.notes.begin(), ui.notes.end(), [](const std::string& n) { return n.rfind("face:", 0) == 0; }), ui.notes.end());
    for (const std::string& n : fit.notes) ui.notes.push_back(n);
    DaeModel rigged = ui.placed;
    rig_from_scratch(skel_, rigged, ui.map.scratch);
    MeshLook hide;
    hide.hidden = ui.map.look.hidden;
    const std::string key = kPreviewModel + ui.path;
    prop_models_[key] = std::make_unique<DaeModel>(shown_model(rigged, hide));
    DaeReport report;
    report.rigged = report.scratch = true;
    prop_reports_[key] = report;
    if (!find_mesh_body(kPreviewBody)) bodies_.push_back({kPreviewBody, "", {}, {}});
    for (MeshBody& b : bodies_)
        if (b.id == kPreviewBody) b.name = stem_of(ui.path) + " (rigging)", b.parts = {key};
    if (!session_body_) {  // the settings file keeps the body chosen before
        session_body_.emplace(settings_.body, ui.previous_body);
        ui.own_session = true;
    }
    settings_.mesh_body = kPreviewBody;
    body_shapes_.clear();
    used_bones_key_.clear();
    mesh_body_skin_pos_.erase(key);
    ++weights_generation_;
    invalidate_floor_cache();
}

void App::start_rig_weights(bool wait) {
    RigScratchUi& ui = *rig_scratch_ui_;
    ui.weigh_due = false;
    ui.job.reset();  // a job still running for an older rig is cancelled and joined
    auto job = std::make_unique<RigWeightsJob>();
    job->model = ui.placed;
    job->rig = ui.map.scratch;
    rig_from_scratch(skel_, job->model, job->rig);
    RigWeightsJob* j = job.get();
    auto run = [j, skel = &skel_] {
        BoneHeatOptions opt;
        opt.cancel = &j->cancel;
        opt.progress = [j](double f, const std::string& what) {
            std::lock_guard<std::mutex> g(j->lock);
            j->progress = f, j->stage = what;
        };
        j->ok = weigh_scratch_rig(*skel, j->model, j->rig, opt, j->report);
        j->done = true;
    };
    if (wait) run();
    else job->worker = std::thread(run);
    ui.job = std::move(job);
    poll_rig_weights();
}

void App::poll_rig_weights() {
    RigScratchUi& ui = *rig_scratch_ui_;
    if (!ui.job || !ui.job->done) return;
    if (ui.job->worker.joinable()) ui.job->worker.join();
    if (ui.job->ok) {
        ui.map.scratch.wjoints = std::move(ui.job->rig.wjoints);
        ui.map.scratch.weights = std::move(ui.job->rig.weights);
        ui.map.scratch.joints = ui.job->rig.joints;
        ui.map.scratch.painted = false;
        ui.report = ui.job->report;
        ui.due = true;
        char line[160];
        std::snprintf(line, sizeof line, "Weights worked out by bone heat in %.2f s", ui.report.seconds);
        status(line);
    } else if (!ui.job->cancel) {
        ui.report = ui.job->report;
    }
    ui.job.reset();
}

void App::close_rig_scratch(bool applied) {
    show_rig_scratch_ = false;
    if (!rig_scratch_ui_) return;
    const RigScratchUi& ui = *rig_scratch_ui_;
    prop_models_.erase(kPreviewModel + ui.path);
    prop_reports_.erase(kPreviewModel + ui.path);
    mesh_body_skin_pos_.erase(kPreviewModel + ui.path);
    std::erase_if(bodies_, [](const MeshBody& b) { return b.id == kPreviewBody; });
    if (!applied && settings_.mesh_body == kPreviewBody) settings_.mesh_body = ui.previous_body;
    if (!applied && ui.own_session) session_body_.reset();
    body_shapes_.clear();
    used_bones_key_.clear();
    invalidate_floor_cache();
    rig_scratch_ui_.reset();  // a weights job still running is cancelled and joined
}

void App::apply_rig_scratch() {
    const std::shared_ptr<RigScratchUi> keep = rig_scratch_ui_;
    RigScratchUi& ui = *keep;
    if (ui.job) {  // a job running for this rig: its weights are the ones to save
        if (ui.job->worker.joinable()) ui.job->worker.join();
        poll_rig_weights();
    }
    // A change not yet shown (markers moved this frame) is fitted and weighed now, so the file has what you set.
    if (ui.due) rig_scratch_preview();
    if ((ui.weigh_due && !ui.painted_warned) || !ui.map.scratch.weighted(ui.as_is.vertex_count())) start_rig_weights(true);
    std::string why;
    const std::string file = rig_map_path(ui.path);
    RigMap map = ui.map, now;
    map.bones.clear(), map.spare_labels.clear(), map.share.clear(), map.quadruped = map.bento = map.along_ground = false;
    if (read_rig_map_file(file, now, why)) map.look = now.look;  // a look saved while the window was open
    if (!write_text(file, write_rig_map_json(map), false, why))
        return message("Rig not saved", why + "\n\nThe rig is saved beside the model (" + file +
                                            ") so it loads with it. Copy the model to a folder you can write to.");
    const std::string path = ui.path, previous = ui.previous_body;
    const bool own_session = ui.own_session;
    close_rig_scratch(true);
    forget_paint(path);        // strokes and brush caches of the weights replaced
    prop_models_.erase(path);  // read afresh: through the rig now
    prop_sources_.erase(path);
    prop_reports_.erase(path);
    mesh_body_skin_pos_.erase(path);
    for (MeshBody& b : bodies_)
        if (b.id.rfind("run:", 0) != 0 && std::count(b.parts.begin(), b.parts.end(), path)) {
            use_mesh_body(b.id);
            return status("Saved " + stem_of(file) + ".json; " + b.name + " now uses it");
        }
    import_body({path});
    if (settings_.mesh_body == kPreviewBody) {  // the import failed: back to the body shown before
        settings_.mesh_body = previous;
        if (own_session) session_body_.reset();
    }
}

void App::draw_rig_scratch_window() {
    if (!show_rig_scratch_) {
        if (rig_scratch_ui_) close_rig_scratch(false);  // closed by its title bar's X
        return;
    }
    if (rig_scratch_ui_) {
        RigScratchUi& ui = *rig_scratch_ui_;
        poll_rig_weights();
        if (ui.due) rig_scratch_preview();
        if (ui.weigh_due && ui.dragged.empty() && !ui.painted_warned) start_rig_weights(headless_);
        if (ui.job) host_.wake(0.05);  // keep frames coming for the progress bar
    }
    const float fs = ImGui::GetFontSize();
    {  // Against the right edge, over the side panels, so the model stays in sight (as Map Rig's window).
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float m = ImGui::GetFrameHeight() * 0.25f;
        const float w = std::min(std::clamp(0.3f * vp->WorkSize.x, 21 * fs, 28 * fs), vp->WorkSize.x - 2 * m);
        const float h = std::min(52 * fs, vp->WorkSize.y - ImGui::GetFrameHeight() - 2 * m);
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - w - m, vp->WorkPos.y + m}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({w, h}, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(dock_title("Rig a Model from Scratch", "Rig", "rig-scratch").c_str(), &show_rig_scratch_)) return ImGui::End();
    tab_tooltip("Rig a Model from Scratch");
    help_button("rig-from-scratch");
    auto pickers = [&] {  // the window's primary action while it is empty; beside Apply, a plain one
        if (rig_scratch_ui_ ? ImGui::Button("Open Model...") : primary_button("Open Model...", "", 0, icon::kOpen))
            show_dialog(Dialog::RigScratch);
        ImGui::SetItemTooltip("A mesh with no armature for SL: .dae, .fbx, .gltf or .glb. One it has of its own is set aside.");
        ImGui::SameLine();
        if (ImGui::Button("Example Mech")) open_unrigged_example();
        ImGui::SetItemTooltip("George, a CC0 mech by Quaternius (Animated Mech Pack), with its own rig left out:\na copy goes in "
                              "your library, so its rig saves beside it.");
    };
    if (!rig_scratch_ui_) {
        hint("Give a model with no skeleton SL's: drag markers onto its joints.");
        pickers();
        return ImGui::End();
    }
    RigScratchUi& ui = *rig_scratch_ui_;
    ScratchRig& rig = ui.map.scratch;
    ImGui::TextUnformatted(stem_of(ui.path).c_str());
    ImGui::SetItemTooltip("%s", ui.path.c_str());
    ImGui::SameLine();
    hint((std::to_string(ui.as_is.vertex_count()) + " vertices, " + std::to_string(ui.as_is.triangle_count()) + " triangles, " +
          std::to_string(ui.parts.size()) + (ui.parts.size() == 1 ? " part" : " parts")).c_str());
    for (const std::string& n : ui.notes) hint(n.c_str());

    // Facing and size, as Map Rig has them.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Facing");
    ImGui::SameLine(4.5f * fs);
    ImGui::SetNextItemWidth(-1);
    static const char* turns[] = {"as in the file", "turned 90 degrees", "turned 180 degrees", "turned 270 degrees"};
    if (ImGui::BeginCombo("##turn", turns[ui.map.turn & 3])) {
        for (int k = 0; k < 4; ++k)
            if (ImGui::Selectable(turns[k], ui.map.turn == k) && ui.map.turn != k) {
                ui.map.turn = k;
                replace(ui);
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", ui.facing.empty() ? "Which way the model faces: its front toward you in the front view"
                                                  : ("VATs guessed it so: " + ui.facing + ".").c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size");
    ImGui::SameLine(4.5f * fs);
    const bool sl_size = ui.map.height > 0;
    if (ImGui::RadioButton("SL-like", sl_size) && !sl_size) {
        ui.map.height = kSlAvatarHeight;
        replace(ui);
    }
    ImGui::SetItemTooltip("Floor to the top of its head: as tall as SL's default avatar (%.2f m), or the height you type", kSlAvatarHeight);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sl_size);
    ImGui::SetNextItemWidth(5 * fs);
    double h = sl_size ? ui.map.height : kSlAvatarHeight;
    if (ImGui::InputDouble("##height", &h, 0, 0, "%.2f m") && sl_size && h > 0.05 && h < 100) ui.map.height = h;
    if (ImGui::IsItemDeactivatedAfterEdit()) replace(ui);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::RadioButton("The file's own", !sl_size) && sl_size) {
        ui.map.height = 0;
        replace(ui);
    }
    ImGui::SetItemTooltip("The size the file gives it, in its own units");

    // The optional parts of the rig.
    subheading("Bones");
    int col = 0;
    for (const RigGroupDef& g : rig_group_defs()) {
        bool on = rig.groups.count(g.id) > 0;
        if (col++ % 4) ImGui::SameLine();
        if (ImGui::Checkbox(g.name, &on)) {
            if (on) {
                rig.groups.insert(g.id);
                // Its markers, guessed, where it has none yet.
                const auto guess = guess_markers(ui.placed, rig.groups);
                for (const RigMarkerDef& d : rig_marker_defs())
                    if (std::string(d.group) == g.id && !rig.markers.count(d.id) && guess.count(d.id)) rig.markers[d.id] = guess.at(d.id);
            } else {
                rig.groups.erase(g.id);
            }
            ui.due = ui.weigh_due = true;
        }
        ImGui::SetItemTooltip("%s", g.what);
    }
    if (ImGui::Checkbox("Fitted mesh", &rig.fitted)) ui.due = ui.weigh_due = true;
    ImGui::SetItemTooltip("Shares the weights near SL's collision volumes (belly, butt, chest, arms, legs...) with them, so the "
                          "wearer's shape sliders reshape the mesh");

    // The markers.
    subheading("Markers");
    ImGui::Checkbox("Mirror", &ui.mirror);
    ImGui::SetItemTooltip("Dragging a left marker moves the right one to match across the body's middle, and back; a marker on "
                          "the middle stays on it");
    ImGui::SameLine();
    if (ImGui::Checkbox("Rest pose", &ui.hold_rest) && !ui.hold_rest) status("Pose or play to see how it bends; the markers show in the rest pose");
    ImGui::SetItemTooltip("The model stands at rest while you place markers. Off: it plays the animation or takes the pose,\nso "
                          "you see how the weights bend it");
    ImGui::SameLine();
    if (ImGui::Button("Guess Again")) {
        rig.markers = guess_markers(ui.placed, rig.groups);
        ui.due = ui.weigh_due = true;
    }
    ImGui::SetItemTooltip("Every marker back to VATs' guess from the model's shape");
    int low = 0;
    for (const auto& [id, mk] : rig.markers) {
        const RigMarkerDef* d = find_rig_marker(id);
        if (d && (!d->group[0] || rig.groups.count(d->group))) low += mk.confidence < 50;
    }
    if (low) ImGui::TextColored(ImVec4(1, 0.6f, 0.35f, 1), "%d %s: check them in the view", low, low == 1 ? "guess is unsure" : "guesses are unsure");
    if (ImGui::BeginTable("##markers", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, std::min(14 * fs, std::max(8 * fs, ImGui::GetContentRegionAvail().y * 0.4f))))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Marker", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("SL joint", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Sure", ImGuiTableColumnFlags_WidthFixed, 2.4f * fs);
        ImGui::TableHeadersRow();
        for (const RigMarkerDef& d : rig_marker_defs()) {
            const auto it = rig.markers.find(d.id);
            if (it == rig.markers.end() || (d.group[0] && !rig.groups.count(d.group))) continue;
            const RigMarker& mk = it->second;
            ImGui::PushID(d.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(d.name, ui.selected == d.id || ui.hovered == d.id, ImGuiSelectableFlags_SpanAllColumns))
                ui.selected = ui.selected == d.id ? "" : d.id;
            if (ImGui::IsItemHovered()) ui.hovered = d.id;
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(mk.confidence >= 100 ? "You placed it" : ("Guessed: " + mk.why).c_str());
                ImGui::TextUnformatted("Drag it in the view; Shift slides it in the view's plane instead of into the body");
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", d.joint);
            ImGui::TableNextColumn();
            const ImU32 c = marker_colour(mk.confidence);
            if (mk.confidence >= 100) ImGui::TextDisabled("you");
            else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(c), "%d%%", mk.confidence);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    hint("Drag the markers inside the body, where it bends; check front and side.");

    // How each part is weighted.
    if (ui.parts.size() > 1) {
        subheading("Weights by part");
        for (const std::string& part : ui.parts) {
            ImGui::PushID(part.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(part.empty() ? "(unnamed)" : part.c_str());
            ImGui::SameLine(std::min(9 * fs, ImGui::GetContentRegionAvail().x * 0.45f));
            ImGui::SetNextItemWidth(-1);
            const auto t = rig.transfer.find(part);
            const std::string now = t == rig.transfer.end() ? "" : t->second;
            if (ImGui::BeginCombo("##how", now.empty() ? "Bone heat" : ("Copy from " + now).c_str())) {
                if (ImGui::Selectable("Bone heat", now.empty())) rig.transfer.erase(part), ui.weigh_due = true;
                ImGui::SetItemTooltip("Its own weights, from the bones near it");
                for (const std::string& from : ui.parts)
                    if (from != part && !rig.transfer.count(from) &&
                        ImGui::Selectable(("Copy from " + from).c_str(), now == from))
                        rig.transfer[part] = from, ui.weigh_due = true;
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("A garment over the body (a vest, sleeves, a scarf) moves best with the body under it: copy its "
                                  "weights from that part, at the nearest point of its surface");
            ImGui::PopID();
        }
    }

    // The weights: worked out after every change, off the UI thread.
    subheading("Weights");
    if (ui.job) {
        double f;
        std::string stage;
        {
            std::lock_guard<std::mutex> g(ui.job->lock);
            f = ui.job->progress, stage = ui.job->stage;
        }
        ImGui::ProgressBar(float(f), ImVec2(-1, 0), stage.c_str());
    } else if (ui.painted_warned) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.35f, 1), "Its weights were painted by hand.");
        hint("Moving markers changes the skeleton; working the weights out again would lose your painting.");
        if (ImGui::Button("Weigh Again (loses the painting)")) ui.painted_warned = false, ui.weigh_due = true;
    } else if (!rig.weighted(ui.as_is.vertex_count())) {
        hint("Not worked out yet: the preview bends each vertex with its nearest bone.");
    } else {
        char line[96];
        std::snprintf(line, sizeof line, "By bone heat, in %.2f s.", ui.report.seconds);
        hint(line);
    }
    for (const std::string& l : ui.report.lines) hint(l.c_str());
    hint("Select a bone (click it in the view) to see its weights glow on the model; untick Rest pose and pose it to see it bend.");

    ImGui::Separator();
    ImGui::BeginDisabled(ui.job != nullptr);
    if (primary_button("Apply", "", 0, icon::kApply)) {
        apply_rig_scratch();
        ImGui::EndDisabled();
        return ImGui::End();
    }
    ImGui::EndDisabled();
    if (ui.job)
        ImGui::SetItemTooltip("Waiting for the weights to be worked out");
    else
        ImGui::SetItemTooltip("Saves the rig beside the model (%s) and makes the model a mesh body: pose it, paint its weights "
                              "(Rig > Paint Weights...) and export it for SL",
                              (stem_of(ui.path) + ".rigmap.json").c_str());
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        close_rig_scratch(false);
        return ImGui::End();
    }
    pickers();
    ImGui::End();
}

void App::draw_rig_scratch_overlay(ImDrawList* dl) {
    if (!rig_scratch_holds_rest()) return;
    RigScratchUi& ui = *rig_scratch_ui_;
    const ScratchRig& rig = ui.map.scratch;
    // Labels are kept inside the view (the Rig workspace's is narrow: names ran off its edges), drawn after the markers,
    // the hovered and selected first; another that would overlap one already drawn waits for its own hover.
    struct Label {
        ImVec2 at;
        std::string text;
        bool hot, keep;
    };
    std::vector<Label> labels;
    const ImVec2 lo = dl->GetClipRectMin(), hi = dl->GetClipRectMax();
    auto label_at = [&](const ImVec2& c, const ImVec2& size, bool left) {
        if (left && c.x - 10 - size.x < lo.x) left = false;  // no room on that side: the other
        else if (!left && c.x + 10 + size.x > hi.x) left = c.x - 10 - size.x >= lo.x;
        const float x = left ? c.x - 10 - size.x : c.x + 10;
        return ImVec2(std::clamp(x, lo.x + 2, std::max(lo.x + 2, hi.x - size.x - 2)),
                      std::clamp(c.y - size.y / 2, lo.y + 2, std::max(lo.y + 2, hi.y - size.y - 2)));
    };
    for (const RigMarkerDef& d : rig_marker_defs()) {
        const auto it = rig.markers.find(d.id);
        if (it == rig.markers.end() || (d.group[0] && !rig.groups.count(d.group))) continue;
        double x, y;
        if (!projector_.to_screen(it->second.pos, x, y)) continue;
        const bool hot = ui.hovered == d.id || ui.dragged == d.id, picked = ui.selected == d.id;
        const ImVec2 c{float(x), float(y)};
        const ImU32 col = marker_colour(it->second.confidence);
        const float r = hot ? 7.5f : 6.f;
        dl->AddCircleFilled(c, r, col);
        dl->AddCircle(c, r, IM_COL32(10, 12, 14, 230), 0, 2);
        if (picked) dl->AddCircle(c, r + 4, IM_COL32(255, 242, 51, 255), 0, 2);
        // Names only where they help: the marker under the pointer, the selected one, and the guesses to check (amber and
        // red). Every name at once piles up at the wrists and shoulders.
        if (!hot && !picked && it->second.confidence >= 65) continue;
        std::string label = d.joint;
        if (hot) label += it->second.confidence >= 100 ? "  (yours)" : "  " + std::to_string(it->second.confidence) + "%: " + it->second.why;
        // The name on the outside of the body (away from the marker's mirror image), so a pair's names do not overlap.
        double mx = x, my = y;
        const bool outside_left = d.mirror[0] && projector_.to_screen(mirror_point(it->second.pos), mx, my) && mx > x + 1;
        labels.push_back({label_at(c, ImGui::CalcTextSize(label.c_str()), outside_left), label, hot, hot || picked});
    }
    // Joints no marker places: a pinned one shows its pin, the one under the pointer its name.
    for (const auto& [name, at] : rig.joints) {
        const std::string id = "joint:" + name;
        const auto pin = rig.pins.find(name);
        const bool hot = ui.hovered == id || ui.dragged == id;
        if (pin == rig.pins.end() && !hot) continue;
        double x, y;
        if (!projector_.to_screen(pin != rig.pins.end() ? pin->second : at, x, y)) continue;
        const ImVec2 c{float(x), float(y)};
        dl->AddCircleFilled(c, hot ? 5.5f : 4.5f, IM_COL32(240, 244, 248, 255));
        dl->AddCircle(c, hot ? 5.5f : 4.5f, IM_COL32(10, 12, 14, 230), 0, 2);
        if (!hot) continue;
        const std::string label = name + (pin != rig.pins.end() ? "  (pinned: right-click to unpin)" : "  (drag to place)");
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        labels.push_back({label_at(ImVec2(c.x, c.y - 16 + size.y / 2), size, false), label, true, true});
    }
    std::stable_partition(labels.begin(), labels.end(), [](const Label& l) { return l.keep; });
    std::vector<ImVec4> taken;  // x0, y0, x1, y1
    for (const Label& l : labels) {
        const ImVec2 size = ImGui::CalcTextSize(l.text.c_str());
        const ImVec4 box(l.at.x - 2, l.at.y - 1, l.at.x + size.x + 2, l.at.y + size.y + 1);
        if (!l.keep && std::any_of(taken.begin(), taken.end(), [&](const ImVec4& t) {
                return box.x < t.z && t.x < box.z && box.y < t.w && t.y < box.w;
            }))
            continue;
        taken.push_back(box);
        dl->AddText(ImVec2(l.at.x + 1, l.at.y + 1), IM_COL32(10, 12, 14, 220), l.text.c_str());
        dl->AddText(l.at, l.hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 230, 235, 235), l.text.c_str());
    }
    ui.hovered.clear();  // the list or the view sets it again next frame
}

bool App::rig_scratch_input(bool hovered) {
    if (!rig_scratch_holds_rest()) return false;
    RigScratchUi& ui = *rig_scratch_ui_;
    ScratchRig& rig = ui.map.scratch;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    // A marker first; else any other fitted joint ("joint:<name>"), which a drag pins (place_pin).
    std::set<std::string> marked;
    auto under = [&]() -> std::string {
        std::string best;
        double best_d = 10;
        for (const RigMarkerDef& d : rig_marker_defs()) {
            const auto it = rig.markers.find(d.id);
            if (it == rig.markers.end() || (d.group[0] && !rig.groups.count(d.group))) continue;
            marked.insert(d.joint);
            double x, y;
            if (!projector_.to_screen(it->second.pos, x, y)) continue;
            const double dist = std::hypot(x - m.x, y - m.y);
            if (dist < best_d) best_d = dist, best = d.id;
        }
        // The nearest of markers and joints, so a joint beside a marker is not shadowed by it.
        for (const auto& [name, at] : rig.joints) {
            if (marked.count(name)) continue;
            const auto pin = rig.pins.find(name);
            double x, y;
            if (!projector_.to_screen(pin != rig.pins.end() ? pin->second : at, x, y)) continue;
            const double dist = std::hypot(x - m.x, y - m.y);
            if (dist < best_d) best_d = dist, best = "joint:" + name;
        }
        return best;
    };
    auto pinned_joint = [](const std::string& id) { return id.rfind("joint:", 0) == 0 ? id.substr(6) : std::string(); };
    if (!ui.dragged.empty() && !ui.drag_moved) {  // a click selects; the marker moves once the pointer has
        ui.drag_moved = std::hypot(m.x - ui.drag_press.x, m.y - ui.drag_press.y) > 3;
        if (!ImGui::IsMouseDown(0)) ui.dragged.clear();
        if (!ui.drag_moved) return true;
    }
    if (!ui.dragged.empty()) {
        Vec3 o, d;
        projector_.ray(camera_, m.x, m.y, o, d);
        Vec3 at;
        const auto shown = prop_models_.find(kPreviewModel + ui.path);
        const bool slide = io.KeyShift || shown == prop_models_.end() || !shown->second ||
                           !mesh_middle_on_ray(*shown->second, o, d, at);
        if (slide) {  // in the view's plane through where the marker was
            const Vec3 n = camera_.forward();
            const double den = d.dot(n);
            if (std::fabs(den) > 1e-9) at = o + d * ((ui.drag_depth_point - o).dot(n) / den);
            else at = ui.drag_depth_point;
        }
        const std::string joint = pinned_joint(ui.dragged);
        if (!joint.empty()) place_pin(rig, joint, at, ui.mirror);
        else place_marker(rig, ui.dragged, at, ui.mirror);
        if (!ImGui::IsMouseDown(0)) {
            const RigMarkerDef* def = find_rig_marker(ui.dragged);
            if (!joint.empty()) status("Pinned " + joint + (ui.mirror ? " and its mirror" : "") + "; the joints below it follow");
            else
                status(std::string("Placed the ") + (def ? def->name : ui.dragged) + " marker (" + (def ? def->joint : "") + ")" +
                       (ui.mirror && def && def->mirror[0] ? ", and its mirror" : ""));
            ui.dragged.clear();
            ui.due = ui.weigh_due = true;
        }
        return true;
    }
    if (!hovered) return false;
    const std::string hit = under();
    if (!hit.empty()) ui.hovered = hit;
    // A press on a marker drags it; Alt (the camera in some presets) and Ctrl leave the press to the view.
    if (const std::string joint = pinned_joint(hit); !joint.empty() && ImGui::IsMouseClicked(1) && rig.pins.erase(joint)) {
        if (ui.mirror) {  // unpinned with its mirror, as it was pinned
            std::string other = joint;
            if (const size_t at = other.find("Left"); at != std::string::npos) other.replace(at, 4, "Right");
            else if (const size_t at2 = other.find("Right"); at2 != std::string::npos) other.replace(at2, 5, "Left");
            rig.pins.erase(other);
        }
        status("Unpinned " + joint + ": the fit places it again");
        ui.due = ui.weigh_due = true;
        return true;
    }
    if (!hit.empty() && ImGui::IsMouseClicked(0) && !io.KeyAlt && !io.KeyCtrl) {
        ui.dragged = ui.selected = hit;
        const std::string joint = pinned_joint(hit);
        const auto pin = rig.pins.find(joint);
        ui.drag_depth_point = joint.empty() ? rig.markers[hit].pos : pin != rig.pins.end() ? pin->second : rig.joints[joint];
        ui.drag_press = m, ui.drag_moved = false;
        return true;
    }
    return false;
}

bool App::cli_rig_groups(const std::string& list) {
    if (!rig_scratch_ui_) return false;
    RigScratchUi& ui = *rig_scratch_ui_;
    std::stringstream ss(list);
    bool ok = true;
    for (std::string g; std::getline(ss, g, ',');) {
        if (std::none_of(rig_group_defs().begin(), rig_group_defs().end(), [&](const RigGroupDef& d) { return g == d.id; })) {
            ok = false;
            continue;
        }
        ui.map.scratch.groups.insert(g);
    }
    const auto guess = guess_markers(ui.placed, ui.map.scratch.groups);
    for (const auto& [id, mk] : guess) ui.map.scratch.markers.try_emplace(id, mk);
    ui.due = ui.weigh_due = true;
    return ok;
}

bool App::cli_rig_marker(const std::string& spec) {
    if (!rig_scratch_ui_) return false;
    const size_t eq = spec.find('=');
    Vec3 p;
    if (eq == std::string::npos || !find_rig_marker(spec.substr(0, eq)) ||
        std::sscanf(spec.c_str() + eq + 1, "%lf,%lf,%lf", &p.x, &p.y, &p.z) != 3)
        return false;
    place_marker(rig_scratch_ui_->map.scratch, spec.substr(0, eq), p, rig_scratch_ui_->mirror);
    rig_scratch_ui_->due = rig_scratch_ui_->weigh_due = true;
    return true;
}

void App::cli_rig_weights() {
    if (!rig_scratch_ui_) return;
    rig_scratch_ui_->painted_warned = false;
    start_rig_weights(true);
    for (const std::string& l : rig_scratch_ui_->report.lines) std::fprintf(stderr, "rig weights: %s\n", l.c_str());
    std::fprintf(stderr, "rig weights: %.3f s\n", rig_scratch_ui_->report.seconds);
}

void App::cli_rig_apply() {
    if (rig_scratch_ui_) apply_rig_scratch();
}

}  // namespace vats
