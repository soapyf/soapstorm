// Viewport Avatar Toolset - the rig export window and the joint offset inspector (spec 08 RG-4, RG-5).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Both work on the mesh body shown (Inventory > Bodies): its parts, as imported from the user's own files, are
// what write_rig_dae writes. The check and the joint table are in the core (rig_export.h); a fix edits the cached
// part in place for this session (the file on disk is never touched) and the body's shape cache is dropped so the
// view shows the change. Host calls only, so the viewer has both windows too.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/lint.h"
#include "vats/rig_export.h"
#include "widgets.h"

namespace vats {

struct RigExportUi {
    RigExportOptions opt;
    std::string body_id;  // the body the findings and tables are for
    bool due = true;
    std::vector<RigPart> parts;
    std::vector<std::string> part_names;
    std::vector<std::string> paths;  // per part: its file, whose source a fix edits
    std::vector<RigFinding> findings;
    std::vector<std::vector<RigJoint>> tables;  // per part
    int part = 0;                               // the inspector's part
    bool only_uploads = false;
    char filter[64] = "";
    std::string last_export;  // the file written last, for Local Mesh
    RigHeight height;         // where SL will stand it (RG-8)
    bool show_ground = true;  // draw SL's ground under the body while the window is open
};

namespace {

std::string stem_of(const std::string& path) {
    const std::string name = path.substr(path.find_last_of("/\\") + 1);
    return name.substr(0, name.rfind('.'));
}

LintSeverity as_lint(RigSeverity s) {
    return s == RigSeverity::Error ? LintSeverity::Error : s == RigSeverity::Warning ? LintSeverity::Warning : LintSeverity::Info;
}

// RG-12: what to set in the SL uploader (Build > Upload > Mesh Model, Upload options tab) for what was written.
std::string upload_steps(const RigExportUi& ui) {
    const bool positions = ui.opt.joint_positions && !ui.opt.bind_pose_only;
    std::string s = "On the uploader's Upload options tab:\n- Tick Include skin weight.\n";
    if (positions)
        s += "- Tick Include joint positions. It is required: without it SL ignores the joint positions and skins the mesh onto its "
             "default skeleton, in a heap.\n";
    if (positions && ui.opt.shape_proof)
        s += "- Tick Lock scale if joint position defined, so the wearer's shape sliders do not stretch it (Shape-proof wrote a "
             "position for every joint it needs).\n";
    else if (positions)
        s += "- Lock scale if joint position defined stops the shape sliders scaling the joints this file moves; tick Shape-proof "
             "here first to cover every joint it uses.\n";
    if (positions && ui.height.valid && std::fabs(ui.height.sole) > 0.01) {
        char z[64];
        std::snprintf(z, sizeof z, "%.3f", std::clamp(-ui.height.sole, -3.0, 3.0));
        s += std::string("- Set Z offset (raise or lower avatar) to ") + z + ", unless you evened the height out with mSkull here.\n";
    } else {
        s += "- Leave Z offset (raise or lower avatar) at 0.\n";
    }
    s += "If the uploader says \"Skinning disabled due to [COUNT] unknown joints\" or \"Rigged to unrecognized joint name [NAME]\", a "
         "bone has a name SL does not know; \"Skinning disabled due to too many joints: [JOINTS], maximum: [MAX]\" means over 110 "
         "joints in one mesh. VATs checks both before writing, so either means the file was changed or another one was chosen.";
    return s;
}

}  // namespace

// Rebuilds the parts, the findings and the joint tables from the body shown.
void App::refresh_rig_check() {
    if (!rig_ui_) rig_ui_ = std::make_shared<RigExportUi>();
    RigExportUi& ui = *rig_ui_;
    sync_mesh_looks();  // a look set since the last frame (--export-rig runs before any)
    ui.parts.clear();
    ui.part_names.clear();
    ui.paths.clear();
    ui.findings.clear();
    ui.tables.clear();
    ui.due = false;
    const MeshBody* b = mesh_body();
    ui.body_id = b ? b->id : "";
    ui.height = {};
    if (!b) return;
    for (const std::string& path : b->parts) {
        RigPart p;
        p.model = prop_model(path);
        p.name = stem_of(path);
        if (auto it = prop_reports_.find(path); it != prop_reports_.end()) {
            p.unmapped_joints = it->second.unmapped_joints;
            p.measured_scale = it->second.measured_scale;
            p.scale = it->second.scale;
        }
        ui.parts.push_back(p);
        ui.part_names.push_back(p.name);
        ui.paths.push_back(path);
    }
    ui.findings = check_rig_export(skel_, ui.parts, ui.opt);
    ui.height = rig_in_world_height(skel_, ui.parts, ui.opt);
    for (const RigPart& p : ui.parts) ui.tables.push_back(p.model ? rig_joints(skel_, *p.model, ui.opt) : std::vector<RigJoint>{});
    ui.part = std::clamp(ui.part, 0, std::max(0, int(ui.parts.size()) - 1));
}

void App::rig_check_stale() {
    if (rig_ui_) rig_ui_->due = true;
}

// A fix is found on the shown model (its vertex numbers are the shown ones), so it runs on a copy of that; the change
// is carried back to the part as loaded (joints whole, vertices through shown_vertex_sources, moves as differences on
// top of the baked shape keys), and what it shows is built again from that: a part shown or a key set later keeps it.
void App::rig_fix_applied(int part, const std::function<void(DaeModel&)>& fix) {
    RigExportUi& ui = *rig_ui_;
    if (part < 0 || part >= int(ui.paths.size())) return;
    const std::string& path = ui.paths[size_t(part)];
    DaeModel* src = part_source(path);
    const DaeModel* shown = prop_model(path);
    if (src && shown) {
        DaeModel after = *shown;
        fix(after);
        src->binds = after.binds, src->bound = after.bound, src->rig_axes = after.rig_axes;
        const std::vector<std::uint32_t> from = shown_vertex_sources(*src, prop_looks_[path]);
        for (size_t v = 0; v < from.size() && v < size_t(after.vertex_count()); ++v) {
            const size_t s = from[v];
            for (int c = 0; c < 3; ++c) src->positions[s * 3 + c] += after.positions[v * 3 + c] - shown->positions[v * 3 + c];
            for (int k = 0; k < 4 && v * 4 + k < after.joints.size() && s * 4 + k < src->joints.size(); ++k)
                src->joints[s * 4 + k] = after.joints[v * 4 + k], src->weights[s * 4 + k] = after.weights[v * 4 + k];
        }
        part_source_changed(path);
    }
    body_shapes_.clear();  // the view follows the edited part
    used_bones_key_.clear();  // and Hide Unused Bones its weights
    rig_ui_->due = true;
}

// The body is shown where its file bound it, its own ground at z 0; SL stands that ground height.sole over its own.
double App::ground_shown() const {
    if (show_rig_export_ && rig_ui_ && rig_ui_->show_ground && rig_ui_->height.valid && mesh_body() && !host_.world_view())
        return -rig_ui_->height.sole;
    return rest_floor();
}

void App::export_rig(const std::string& path) {
    refresh_rig_check();
    RigExportUi& ui = *rig_ui_;
    std::string text, why;
    if (!write_rig_dae(skel_, ui.parts, ui.opt, text, why)) return message("Rigged mesh not exported", why);
    if (!write_text(path, text, false, why)) return message("Could not write " + stem_of(path) + ".dae", why);
    ui.last_export = path;
    int listed = 0, positions = 0, tris = 0;
    for (size_t i = 0; i < ui.tables.size(); ++i) {
        for (const RigJoint& j : ui.tables[i]) listed += j.listed, positions += j.uploads;
        if (ui.parts[i].model) tris += ui.parts[i].model->triangle_count();
    }
    std::string summary = std::to_string(ui.parts.size()) + (ui.parts.size() == 1 ? " mesh, " : " meshes, ") + std::to_string(tris) +
                          " triangles, " + std::to_string(listed) + " joints listed";
    if (ui.opt.joint_positions && !ui.opt.bind_pose_only)
        summary += ", " + std::to_string(positions) + " joint position" + (positions == 1 ? "" : "s") +
                   " (upload with Include joint positions ticked)";
    else
        summary += ", no joint positions";
    // SK-3: what the body's look kept out of the file and baked into it.
    std::string left_out, baked;
    if (const MeshBody* b = mesh_body()) {
        const MeshLook look = mesh_look(*b);
        std::set<std::string> seen;
        for (const std::string& file : ui.paths)
            if (const DaeModel* src = part_source(file)) {
                for (const DaePart& p : src->parts)
                    if (look.hidden.count(p.name) && seen.insert("part:" + p.name).second) left_out += (left_out.empty() ? "" : ", ") + p.name;
                for (const std::string& k : shape_key_names(*src))
                    if (const double v = shape_key_value(*src, look, k); v != 0 && seen.insert("key:" + k).second)
                        baked += (baked.empty() ? "" : ", ") + k + " " + std::to_string(int(std::lround(v * 100))) + "%";
            }
    }
    if (!left_out.empty()) summary += ". Hidden parts left out: " + left_out;
    if (!baked.empty()) summary += ". Shape keys baked in: " + baked;
    status("Exported " + stem_of(path) + ".dae" + ": " + summary);
    message("Exported " + stem_of(path) + ".dae",
            summary + ".\n\nIn the viewer: Build > Upload > Mesh Model, choose this file. " + upload_steps(ui) +
                "\n\nCheck the preview on the Skin weights tab before paying.");
}

void App::draw_rig_export_window() {
    if (!show_rig_export_ || ImGui::GetFrameCount() < 3) return;
    if (!rig_ui_) rig_ui_ = std::make_shared<RigExportUi>();
    RigExportUi& ui = *rig_ui_;
    const MeshBody* b = mesh_body();
    if (ui.due || (b ? b->id : "") != ui.body_id) refresh_rig_check();
    place_tool_window("Export Rigged Mesh for SL", 36, 38);
    if (!ImGui::Begin("Export Rigged Mesh for SL", &show_rig_export_)) return ImGui::End();
    help_button("rigging-for-sl-without-add-ons");
    if (!b) {
        if (empty_state("No mesh body shown: it exports the body shown. Import a rigged mesh as a body, or show one.",
                        "Import Body Parts..."))
            show_dialog(Dialog::ImportBody);
        return ImGui::End();
    }
    ImGui::Text("%s", b->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu %s)", b->parts.size(), b->parts.size() == 1 ? "part" : "parts");
    if (ImGui::IsItemHovered()) {
        std::string tip;
        for (auto& p : b->parts) tip += p + "\n";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    hint("A rigged .dae the SL mesh uploader takes, checked against its rules.");
    ImGui::Separator();
    bool changed = false;
    changed |= ImGui::Checkbox("Joint positions", &ui.opt.joint_positions);
    ImGui::SetItemTooltip("Write each bound joint where the file bound it, as the position SL applies when you upload with\n"
                          "Include joint positions ticked. Off: the joints at SL's defaults, the mesh skinned onto them.");
    ImGui::BeginDisabled(!ui.opt.joint_positions);
    changed |= ImGui::Checkbox("Bind pose only", &ui.opt.bind_pose_only);
    ImGui::SetItemTooltip("The mesh was modelled in another pose (an A-pose) and its bone lengths are SL's: write no joint positions,\n"
                          "and let the inverse binds carry the pose. SL skins it back onto its rest skeleton.");
    changed |= ImGui::Checkbox("Stand it in SL's rest pose", &ui.opt.rest_pose);
    ImGui::SetItemTooltip("For an upright humanoid modelled in an A-pose and mapped from its own rig: write the joint positions\n"
                          "with its bones along SL's rest pose (arms out level), at their own lengths, and let the inverse\n"
                          "binds carry the A-pose. SL animations are made for that rest, so they then turn the arms as meant.");
    changed |= ImGui::Checkbox("Shape-proof", &ui.opt.shape_proof);
    ImGui::SetItemTooltip("Give every joint the mesh uses, and every joint above one, a position (0.11 mm off SL's default\n"
                          "where it sits on it), so the uploader's \"Lock scale if joint position defined\" locks them all\n"
                          "and the wearer's shape sliders no longer stretch the mesh. They count toward the 110 joints.");
    changed |= ImGui::Checkbox("mPelvis offset", &ui.opt.pelvis_offset);
    ImGui::SetItemTooltip("Also write mPelvis's position. It changes the wearer's pelvis-to-foot height and hover on every\n"
                          "animation; most bodies leave it out and keep the other joints' offsets from the pelvis.");
    ImGui::EndDisabled();
    if (changed) ui.due = true;
    // RG-8: where SL will stand it, for the default shape.
    if (ui.height.valid) {
        const double cm = ui.height.sole * 100;
        if (std::fabs(cm) < 1) ImGui::TextUnformatted("In Second Life it stands on the ground.");
        else ImGui::Text("In Second Life it %s %.1f cm %s the ground.", cm > 0 ? "floats" : "sinks", std::fabs(cm), cm > 0 ? "above" : "into");
        if (!host_.world_view()) {
            ImGui::SameLine();
            ImGui::Checkbox("Show SL's ground", &ui.show_ground);
            ImGui::SetItemTooltip("While this window is open, the ground grid is drawn where Second Life will put the ground\n"
                                  "under this body (the default shape), worked out as every SL viewer does.");
        }
    }

    // Findings, as the Animation Check shows them.
    ImGui::Separator();
    const bool refused = rig_export_refused(ui.findings);
    if (ui.findings.empty()) hint("Nothing to report.");
    else if (refused) hint("The uploader would refuse or break this. Fix the red items first.");
    else hint("Checked against the uploader's rules. Warnings upload, but read them.");
    // At least a few findings high: the window scrolls when it is short.
    const float below = ImGui::GetFrameHeightWithSpacing() * 3.5f;
    ImGui::BeginChild("##rig_findings", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y - below, ImGui::GetTextLineHeightWithSpacing() * 8)));
    const std::vector<RigFinding> findings = ui.findings;  // a fix below re-checks and replaces the list
    for (size_t i = 0; i < findings.size(); ++i) {
        const RigFinding& f = findings[i];
        ImGui::PushID(int(i));
        severity_icon(as_lint(f.severity));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", f.message.c_str());
        const float indent = ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::Indent(indent);
        if (f.fix && f.part >= 0 && f.part < int(ui.parts.size()) && ui.parts[f.part].model) {
            if (ImGui::Button(f.fix_label.c_str())) {
                // The cached part is the export's input; the file on disk is never touched.
                const std::string name = ui.parts[f.part].name, label = f.fix_label;
                rig_fix_applied(f.part, f.fix);  // refreshes the findings: f is a copy
                status("Rig export: " + label + " on " + name);
            }
            ImGui::SetItemTooltip("%s", "Applied to the part as loaded, for this session; re-import the file to undo");
            ImGui::SameLine();
        }
        std::vector<int> nodes;
        for (const std::string& n : f.joints) {
            int node = skel_.find(n);
            if (node < 0)
                if (int v = skel_.find_volume(n); v >= 0) node = skel_.volumes()[v].node;
            if (node >= 0) nodes.push_back(node);
        }
        if (!nodes.empty()) {
            if (ImGui::Button("Select Bones")) {
                clear_selection();
                for (int n : nodes) select(n, true);
            }
        }
        ImGui::Unindent(indent);
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (section_header("Uploading It", false)) ImGui::TextWrapped("%s", upload_steps(ui).c_str());
    if (ImGui::Button("Check Again")) ui.due = true;
    ImGui::SameLine();
    if (ImGui::Button("Joint Offset Inspector...")) show_joint_inspector_ = true;
    ImGui::BeginDisabled(refused);
    if (primary_button("Export .dae...", "", 0, icon::kExport)) show_dialog(Dialog::ExportRig);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", refused ? "Refused until the red items are fixed" : "Writes the file; nothing is uploaded");
    if (host_.world_view()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(ui.last_export.empty());
        if (ImGui::Button("Preview with Local Mesh")) {
            std::string why;
            if (host_.local_mesh_preview(ui.last_export, why)) status("Local Mesh: showing " + stem_of(ui.last_export) + ".dae" + " on your avatar");
            else message("Local Mesh preview", why);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Shows the exported file on your own avatar through Local Mesh, on your screen only; nothing is uploaded");
    }
    ImGui::End();
}

void App::draw_joint_inspector() {
    if (!show_joint_inspector_ || ImGui::GetFrameCount() < 3) return;
    if (!rig_ui_) rig_ui_ = std::make_shared<RigExportUi>();
    RigExportUi& ui = *rig_ui_;
    const MeshBody* b = mesh_body();
    if (ui.due || (b ? b->id : "") != ui.body_id) refresh_rig_check();
    place_tool_window("Joint Offset Inspector", 46, 40);
    if (!ImGui::Begin("Joint Offset Inspector", &show_joint_inspector_)) return ImGui::End();
    help_button("joint-offset-inspector");
    if (!b || ui.tables.empty()) {
        if (empty_state("No mesh body shown. Import a rigged mesh as a body, or show one (Inventory > Bodies).",
                        "Import Body Parts..."))
            show_dialog(Dialog::ImportBody);
        return ImGui::End();
    }
    hint("Each joint's offset from SL's default, in mm, as the uploader reads it.");
    if (ui.parts.size() > 1) {
        labelled_row("Part", kLabelEm, 14);
        if (ImGui::BeginCombo("##part", ui.part_names[ui.part].c_str())) {
            for (int i = 0; i < int(ui.part_names.size()); ++i)
                if (ImGui::Selectable(ui.part_names[i].c_str(), i == ui.part)) ui.part = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    filter_input("##filter", "Filter joints", ui.filter, sizeof ui.filter);
    ImGui::SameLine();
    ImGui::Checkbox("Only joints that upload", &ui.only_uploads);
    const std::vector<RigJoint>& table = ui.tables[ui.part];
    const DaeModel* model = ui.part < int(ui.parts.size()) ? ui.parts[ui.part].model : nullptr;
    int uploads = 0, noise = 0, weighted = 0;
    for (const RigJoint& j : table) uploads += j.uploads, noise += j.noise && j.listed, weighted += j.weighted;
    ImGui::Text("%d joints weighted, %d joint positions upload, %d under 1 mm", weighted, uploads, noise);
    if (noise && model) {
        ImGui::SameLine();
        if (ImGui::Button("Snap All Under 1 mm")) {
            rig_fix_applied(ui.part, [&](DaeModel& m) {
                for (const RigJoint& j : table)
                    if (j.noise && j.listed) snap_joint_to_default(skel_, m, j.node);
            });
            status("Joint Offset Inspector: " + std::to_string(noise) + " joints snapped to default");
        }
    }
    const std::string filter = ui.filter;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##joints", 8, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Joint", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Offset mm");
        ImGui::TableSetupColumn("X");
        ImGui::TableSetupColumn("Y");
        ImGui::TableSetupColumn("Z");
        ImGui::TableSetupColumn("Weighted");
        ImGui::TableSetupColumn("Uploads");
        ImGui::TableSetupColumn("##snap");
        ImGui::TableHeadersRow();
        for (const RigJoint& j : table) {
            if (ui.only_uploads && !j.uploads) continue;
            if (!filter.empty() && lower(j.name).find(lower(filter)) == std::string::npos) continue;
            if (!j.bound && !j.weighted && !ui.only_uploads && filter.empty()) continue;  // untouched joints only when asked for
            ImGui::TableNextRow();
            ImGui::PushID(j.node);
            ImGui::TableNextColumn();
            const bool volume = j.node > dae_root(skel_);
            if (ImGui::Selectable(j.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                const int node = volume ? skel_.volumes()[j.node - dae_root(skel_) - 1].node : j.node;
                clear_selection();
                if (node >= 0) select(node, false);
            }
            ImGui::SetItemTooltip("%s%s%s", volume ? "Collision volume (fitted mesh)" : "Joint", j.bound ? "; bound by the file" : "; not bound by the file",
                                  j.listed ? "; in the skin's joint list" : "");
            ImGui::TableNextColumn();
            if (j.noise && j.listed) ImGui::TextColored(ImVec4(0.94f, 0.7f, 0.27f, 1), "%.2f", j.offset_mm);
            else if (j.uploads) ImGui::Text("%.1f", j.offset_mm);
            else ImGui::TextDisabled("%.2f", j.offset_mm);
            for (int axis = 0; axis < 3; ++axis) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.1f", std::round(j.offset[axis] * 10000) / 10 + 0.0);  // no "-0.0"
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(j.weighted ? "yes" : "-");
            ImGui::TableNextColumn();
            if (j.uploads) ImGui::TextUnformatted(j.noise ? "yes (noise)" : "yes");
            else if (j.offset_mm > 0.0001) ImGui::TextDisabled(j.listed ? "under 0.1 mm" : "not listed");
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (model && j.bound && j.offset_mm > 0.0001) {
                if (ImGui::SmallButton("Snap")) {
                    rig_fix_applied(ui.part, [&](DaeModel& m) { snap_joint_to_default(skel_, m, j.node); });
                    status("Joint Offset Inspector: " + j.name + " snapped to default");
                }
                ImGui::SetItemTooltip("Move this joint's bind to SL's default (its vertices stay); re-import the file to undo");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace vats
