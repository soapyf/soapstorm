// Viewport Avatar Toolset - File > Import Animation (Retarget)...: mapping picker and fit report.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/07 (RT-1, RT-5, RT-7, RT-10, RT-11). The core does the work (vats/retarget.h);
// this is the dialog around it.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app.h"
#include "imgui.h"
#include "vats/fbx.h"
#include "vats/footlock.h"
#include "vats/project.h"
#include "vats/retarget.h"

namespace vats {

struct RetargetUi {
    std::string path;
    std::string confirm_split;  // part files an earlier split left, listed until the user replaces or cancels
    SourceAnim src;
    std::vector<RigTable> tables;
    int table = -1;  // -1 = manual
    BoneMap map;
    RetargetOptions opt;
    FitOptions fit;
    bool lock_feet = true;  // RT-9
    bool open = false, imported = false;
    std::string report;
    Clip raw;          // the last import before fitting: what split and trim start from (RT-10.4)
    bool fits = true;
    int trim_from = 0, trim_to = 0;
};

namespace {

std::string base_name(const std::string& p) { return p.substr(p.find_last_of("/\\") + 1); }

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

}  // namespace

void App::open_retarget(const std::string& path) {
    auto ui = std::make_shared<RetargetUi>();
    ui->path = path;
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string ext = path.substr(path.find_last_of('.') + 1), err;
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    bool ok = !f.bad() && !bytes.empty() &&
              (ext == "bvh"   ? read_bvh_source(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ui->src, err)
               : ext == "fbx" ? read_fbx_source(bytes, ui->src, err)
                              : read_gltf_source(bytes, path.substr(0, path.find_last_of("/\\")), ui->src, err));
    if (!ok) return message("Import failed", base_name(path) + ": " + (err.empty() ? "could not be read" : err));

    // Rig tables are data (RT-3): every data/retarget/*.json.
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(data_dir_ + "/retarget", ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream t(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << t.rdbuf();
        RigTable table;
        if (parse_rig_table(ss.str(), table, err)) ui->tables.push_back(std::move(table));
    }
    std::sort(ui->tables.begin(), ui->tables.end(), [](auto& a, auto& b) { return a.name < b.name; });
    ui->table = best_rig_table(ui->tables, ui->src, ui->map);
    ui->open = true;
    retarget_ui_ = ui;
}

// RT-10.4: clip in consecutive parts that each fit SL's limits (split_to_fit), saved as projects <stem>_part<N>.vat
// beside source. Parts from an earlier split may hold edits: when some exist they are listed in confirm and nothing
// is written until this runs again with overwrite. advice follows "Even two-second parts are over SL's limits."
void App::split_into_parts(const Clip& clip, const std::string& source, const FitOptions& fit, bool overwrite,
                           std::string& confirm, const std::string& advice) {
    std::vector<FitReport> reps;
    std::vector<Clip> parts = split_to_fit(skel_, clip, fit, &reps);
    if (parts.empty()) return message("Cannot split", "Even two-second parts are over SL's limits." + advice);
    const std::string stem = source.substr(0, source.find_last_of('.'));
    auto part_path = [&](size_t i) { return stem + "_part" + std::to_string(i + 1) + ".vat"; };
    if (!overwrite) {
        std::string existing;
        for (size_t i = 0; i < parts.size(); ++i)
            if (std::error_code ec; std::filesystem::exists(u8path(part_path(i)), ec)) existing += "- " + base_name(part_path(i)) + "\n";
        if (!existing.empty()) {
            confirm = existing;
            return;
        }
    }
    std::string written, why;
    for (size_t i = 0; i < parts.size(); ++i) {
        Project p;
        p.clip = std::move(parts[i]);
        if (!write_text(part_path(i), save_project(p), true, why))
            return message("Could not save the parts", "Could not write " + part_path(i) + "\n\n" + why +
                                                             (written.empty() ? "" : "\n\nAlready saved:\n" + written));
        written += "- " + base_name(part_path(i)) + "\n";
    }
    message("Split into " + std::to_string(reps.size()) + " parts",
            "Each part fits SL's limits and starts where the previous one ends. Saved beside the source file:\n" + written);
}

// RT-10.4: a clip that still does not fit is split into parts saved beside the source, or trimmed.
void App::draw_retarget_split(RetargetUi& ui) {
    ImGui::SeparatorText("Too long or too big for one upload");
    auto split = [&](bool overwrite) {
        split_into_parts(ui.raw, ui.path, ui.fit, overwrite, ui.confirm_split, " Allow more trades, or trim.");
    };
    if (ImGui::Button("Split into Parts...")) guarded(ui.path, [&] { split(false); });
    if (!ui.confirm_split.empty()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "These parts exist already and would be replaced (kept as .bak):");
        ImGui::TextUnformatted(ui.confirm_split.c_str());
        if (ImGui::Button("Replace Them")) ui.confirm_split.clear(), guarded(ui.path, [&] { split(true); });
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ui.confirm_split.clear();
    }
    ImGui::SetItemTooltip("Consecutive parts, each under 60 s and the upload limit, saved as projects beside the source file");
    ImGui::SameLine();
    const float k = ImGui::GetFontSize() / 15.f;
    ImGui::SetNextItemWidth(160 * k);
    ImGui::DragIntRange2("##trim", &ui.trim_from, &ui.trim_to, 1, 0, ui.raw.end_frame, "from %d", "to %d");
    ImGui::SameLine();
    if (ImGui::Button("Trim")) guarded(ui.path, [&] {
        Clip c = slice_clip(ui.raw, ui.trim_from, ui.trim_to);
        FitReport fit = fit_to_limits(skel_, c, ui.fit);
        std::vector<Prop> props = std::move(doc_.clip().props);
        c.props = std::move(props);
        doc_.clip() = std::move(c);
        doc_.dirty = true;
        update_title();
        ui.fits = fit.fits;
        char head[160];
        std::snprintf(head, sizeof head, "Trimmed to frames %d-%d: %s, %zu bytes.\n", ui.trim_from, ui.trim_to,
                      fit.fits ? "fits SL's limits" : "still does not fit", fit.bytes_after);
        ui.report = head;
        for (auto& s : fit.steps) ui.report += "- " + s + "\n";
    });
    ImGui::SetItemTooltip("Keep only this frame range of the import");
}

void App::draw_retarget_dialog() {
    if (!retarget_ui_ || !retarget_ui_->open) return;
    RetargetUi& ui = *retarget_ui_;
    const char* title = "Import Animation (Retarget)";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    const float k = ImGui::GetFontSize() / 15.f;
    ImGui::SetNextWindowSize(ImVec2(620 * k, 640 * k), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr)) return;
    help_button("retargeting");

    ImGui::Text("%s", base_name(ui.path).c_str());
    ImGui::TextDisabled("%zu joints, %d frames at %.4g fps (%.1f s)", ui.src.joints.size(), ui.src.frames(), ui.src.fps,
                        ui.src.frames() > 1 ? (ui.src.frames() - 1) / ui.src.fps : 0.0);
    for (auto& n : ui.src.notes) ImGui::TextDisabled("%s", n.c_str());

    // RT-3 / RT-5: rig family, or a manual mapping pre-filled with the best matches.
    std::string current = ui.table >= 0 ? ui.tables[ui.table].name : "Manual";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Rig");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * ImGui::GetFontSize() / 15.f);
    if (ImGui::BeginCombo("##rig", current.c_str())) {
        for (int i = 0; i < int(ui.tables.size()); ++i) {
            BoneMap m;
            int n = apply_rig_table(ui.tables[i], ui.src, m);
            std::string label = ui.tables[i].name + "  (" + std::to_string(n) + " bones)";
            if (ImGui::Selectable(label.c_str(), ui.table == i)) ui.table = i, ui.map = m;
        }
        if (ImGui::Selectable("Manual", ui.table < 0)) ui.table = -1;
        ImGui::EndCombo();
    }
    std::vector<std::string> missing;
    const bool usable = map_is_usable(ui.map, &missing);
    if (!usable)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Pick source bones for: %s", join(missing, ", ").c_str());

    ImGui::BeginChild("map", ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 6.5f),
                      ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("bones", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const std::string& sl : retarget_joints(skel_)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(sl.c_str());
            ImGui::TableNextColumn();
            auto it = ui.map.find(sl);
            int j = it == ui.map.end() ? -1 : it->second;
            ImGui::SetNextItemWidth(-1);
            ImGui::PushID(sl.c_str());
            if (ImGui::BeginCombo("##src", j >= 0 ? ui.src.joints[j].name.c_str() : "(none)", ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("(none)", j < 0)) ui.map.erase(sl), ui.table = -1;
                for (int k = 0; k < int(ui.src.joints.size()); ++k) {  // source files can repeat a joint name
                    ImGui::PushID(k);
                    if (ImGui::Selectable(ui.src.joints[k].name.c_str(), k == j)) ui.map[sl] = k, ui.table = -1;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // RT-7 and RT-11: rest pose choice, and which trades the fit may make.
    ImGui::Checkbox("Rest Pose from Frame 0", &ui.opt.rest_from_frame0);
    ImGui::SetItemTooltip("Use when the file's own rest pose is wrong or missing: frame 0 must then be a T-pose or A-pose");
    ImGui::SameLine();
    ImGui::Checkbox("Clean Up Foot Sliding", &ui.lock_feet);
    ImGui::SetItemTooltip("Holds planted feet still with leg IK where the source had them on the ground");
    ImGui::TextUnformatted("To fit SL's limits, VATs may:");
    ImGui::Checkbox("Reduce Keys", &ui.fit.allow_tolerance);
    ImGui::SameLine();
    ImGui::Checkbox("Lower the Frame Rate", &ui.fit.allow_fps);
    ImGui::SameLine();
    ImGui::Checkbox("Drop Face", &ui.fit.allow_drop_face);
    ImGui::SameLine();
    ImGui::Checkbox("Drop Finger Tips", &ui.fit.allow_drop_fingers);
    ImGui::SameLine();
    ImGui::Checkbox("Drop Toes", &ui.fit.allow_drop_toes);

    ImGui::BeginDisabled(!usable);
    if (ImGui::Button(ui.imported ? "Import Again" : "Import")) guarded(ui.path, [&] {
        ui.opt.shape = ui.fit.shape = export_shape();
        RetargetResult r = retarget(skel_, ui.src, ui.map, ui.opt);
        if (ui.lock_feet) {
            FootLockOptions fl;
            fl.shape = ui.opt.shape;
            for (auto& line : lock_feet(r.clip, *rig_, fl)) r.report.push_back(line);
        }
        ui.raw = r.clip;
        ui.trim_from = 0, ui.trim_to = std::min(r.clip.end_frame, 60 * std::max(r.clip.fps, 1));
        FitReport fit = fit_to_limits(skel_, r.clip, ui.fit);
        ui.fits = fit.fits;
        std::vector<Prop> props = std::move(doc_.clip().props);  // imports keep the scene's props (IO-23)
        new_document();
        doc_.clip() = std::move(r.clip);
        doc_.clip().props = std::move(props);
        doc_.dirty = true;
        update_title();
        char head[200];
        std::snprintf(head, sizeof head, "%s: %d frames at %d fps, %zu bytes (was %zu).", fit.fits ? "Fits SL's limits" : "Does not fit yet",
                      doc_.clip().end_frame + 1, fit.fps, fit.bytes_after, fit.bytes_before);
        ui.report = std::string(head) + "\n";
        for (auto& s : fit.steps) ui.report += "- " + s + "\n";
        for (auto& s : r.report) ui.report += "- " + s + "\n";
        ui.imported = true;
        status("Retargeted " + base_name(ui.path) + (fit.fits ? "" : " (over SL's limits: see the report)"));
    });
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ui.imported ? "Close" : "Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.open = false;
        ImGui::CloseCurrentPopup();
    }
    if (ui.imported && !ui.fits) draw_retarget_split(ui);
    if (!ui.report.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(untick a trade and Import again to try another)");
        ImGui::Separator();
        ImGui::TextWrapped("%s", ui.report.c_str());
    }
    ImGui::EndPopup();
}

}  // namespace vats
