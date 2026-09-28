// Viewport Avatar Toolset - the Face window's Export Expression Pack... dialog (spec 08 EX).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The clips come from the core (vats/expression_pack.h); each is exported with the project's export options, as
// Export would (the bake shape, Your avatar's joint positions, key reduction), then written to a folder, copied
// to the Animations library and, in the viewer, uploaded one after another.
#include <algorithm>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "vats/expression_pack.h"

namespace vats {

struct ExpressionPackUi {
    std::vector<char> starters;          // ticked, by starter_expressions() index
    std::map<std::string, bool> poses;   // ticked face poses, by library item id
    ExpressionPackOptions opt;
    char prefix[48] = "Face";
    bool to_library = false;
    FaceTable table;  // the Face window's, when the folder was asked for
};

namespace {

// The ticked expressions: the starters, then the face poses.
std::vector<Expression> chosen(const ExpressionPackUi& ui, const std::vector<LibraryItem>& items) {
    std::vector<Expression> list;
    const std::vector<Expression> starters = starter_expressions();
    for (size_t i = 0; i < starters.size() && i < ui.starters.size(); ++i)
        if (ui.starters[i]) list.push_back(starters[i]);
    for (const LibraryItem& it : items)
        if (it.kind == "face")
            if (auto p = ui.poses.find(it.id); p != ui.poses.end() && p->second) list.push_back(face_pose_expression(it));
    return list;
}

}  // namespace

void App::draw_expression_pack(const FaceTable& table, bool positions) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Export Expression Pack", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!pack_ui_) pack_ui_ = std::make_shared<ExpressionPackUi>();
    ExpressionPackUi& ui = *pack_ui_;
    const std::vector<Expression> starters = starter_expressions();
    if (ui.starters.size() != starters.size()) ui.starters.assign(starters.size(), 1);
    ui.opt.prefix = ui.prefix;
    ui.opt.positions = positions;
    const float label_w = ImGui::GetFontSize() * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    };
    hint("One short .anim per expression, face bones only, each easing in and out, for an expression HUD.");

    ImGui::SeparatorText("Starter set");
    for (size_t i = 0; i < starters.size(); ++i) {
        const bool keys = !expression_clip(table, starters[i], ui.opt).curves.empty();
        if (i % 3) ImGui::SameLine(ImGui::GetFontSize() * (1 + 9 * float(i % 3)));
        ImGui::BeginDisabled(!keys);
        bool on = ui.starters[i] && keys;
        if (ImGui::Checkbox(starters[i].name.c_str(), &on)) ui.starters[i] = on;
        ImGui::EndDisabled();
        if (!keys) ImGui::SetItemTooltip("Only moves face bones: turn on Move face bones in the Face window");
    }

    ImGui::SeparatorText("Your face poses");
    int poses = 0;
    for (const LibraryItem& it : library_.items) {
        if (it.kind != "face") continue;
        ImGui::PushID(it.id.c_str());
        bool on = ui.poses[it.id];
        if (ImGui::Checkbox(it.name.empty() ? "(unnamed)" : it.name.c_str(), &on)) ui.poses[it.id] = on;
        ImGui::PopID();
        ++poses;
    }
    if (!poses) ImGui::TextDisabled("None yet: Save Face Pose in the Face window keeps one.");

    ImGui::SeparatorText("Files");
    label("Prefix");
    ImGui::InputText("##packprefix", ui.prefix, sizeof ui.prefix);
    ImGui::SetItemTooltip("Every file is <prefix>_<expression>, lower case with _ for spaces: Face_wink_l");
    label("Priority");
    ImGui::SliderInt("##packprio", &ui.opt.priority, 0, 6);
    ImGui::SetItemTooltip("Above the body animations the face should win over (an AO's are often 3 or 4)");
    float len = float(ui.opt.length), ein = float(ui.opt.ease_in), eout = float(ui.opt.ease_out);
    label("Length");
    if (ImGui::DragFloat("##packlen", &len, 0.05f, 0.5f, 10, "%.2f s")) ui.opt.length = std::clamp(len, 0.5f, 10.f);
    ImGui::SetItemTooltip("How long a held expression lasts; the blink and breathing loops are 4 s");
    label("Ease in");
    if (ImGui::DragFloat("##packein", &ein, 0.01f, 0, 2, "%.2f s")) ui.opt.ease_in = std::clamp(ein, 0.f, 2.f);
    label("Ease out");
    if (ImGui::DragFloat("##packeout", &eout, 0.01f, 0, 2, "%.2f s")) ui.opt.ease_out = std::clamp(eout, 0.f, 2.f);
    ImGui::SetCursorPosX(label_w);
    ImGui::Checkbox("Hold until stopped (loop)", &ui.opt.loop);
    ImGui::SetItemTooltip("Off: a held expression plays once, then eases out. The blink and breathing loops always loop.");
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("Move face bones: %s (in the Face window)", positions ? "on" : "off");
    if (positions && host_.world_view()) {  // as the Face window warns (03 IO-11a)
        bool face_worn = false;
        for (const std::string& j : host_.joint_overrides())
            if (int n = skel_.find(j); n >= 0 && skel_[n].category == Category::Face) face_worn = true;
        if (face_worn && bake_shape_key(doc_.clip().export_settings, exporting_yours()) != "avatar")
            ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "Your mesh head has its own face joint positions. Set Bake "
                                                          "shape to Your avatar (Properties > Export) first.");
    }
    std::vector<std::string> skipped;
    const std::vector<PackFile> pack = expression_pack(table, chosen(ui, library_.items), ui.opt, &skipped);
    label("Saves as");
    if (pack.empty()) ImGui::TextDisabled("(tick an expression)");
    else ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "%s.anim%s", pack[0].name.c_str(),
                            pack.size() > 1 ? (" and " + std::to_string(pack.size() - 1) + " more").c_str() : "");
    ImGui::SetCursorPosX(label_w);
    ImGui::Checkbox("Also save to Animations library", &ui.to_library);
    ImGui::SetItemTooltip("Each .anim is also copied to the Inventory's Animations, replacing one of the same name");

    ImGui::Spacing();
    ImGui::BeginDisabled(pack.empty());
    if (ImGui::Button("Export to Folder...")) {
        ui.table = table;
        show_dialog(Dialog::ExpressionPack);
    }
    if (host_.can_upload()) {
        ImGui::SameLine();
        if (ImGui::Button("Upload All...")) {
            if (!upload_queue_.empty()) {
                status("An upload is already waiting for its confirmation");
            } else {
                AnimExportOptions eo = anim_export_options();  // Your avatar and the key reduction, as Export
                std::string problems;
                for (const PackFile& p : pack) {
                    const AnimExportResult r = vats::export_anim(skel_, p.clip, eo);
                    for (auto& e : r.errors) problems += "- " + p.name + ": " + e + "\n";
                    if (!r.errors.empty()) continue;
                    std::vector<std::uint8_t> bytes = write_anim(r.file);
                    if (ui.to_library) anim_to_library(p.name + ".anim", bytes);
                    upload_queue_.emplace_back(p.name, std::move(bytes));
                }
                if (!problems.empty()) {
                    upload_queue_.clear();
                    message("Cannot upload", problems);
                } else {
                    upload_next();
                }
            }
        }
        ImGui::SetItemTooltip("Uploads every file under its name; the viewer asks to confirm the price of each");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::export_expression_pack(const std::string& folder) {
    if (!pack_ui_) return;
    ExpressionPackUi& ui = *pack_ui_;
    std::vector<std::string> skipped;
    const std::vector<PackFile> pack = expression_pack(ui.table, chosen(ui, library_.items), ui.opt, &skipped);
    const AnimExportOptions eo = anim_export_options();  // Your avatar and the key reduction, as Export
    std::string problems, warnings;
    int written = 0;
    for (const PackFile& p : pack) {
        const AnimExportResult r = vats::export_anim(skel_, p.clip, eo);
        for (auto& e : r.errors) problems += "- " + p.name + ": " + e + "\n";
        if (!r.errors.empty()) continue;
        for (auto& w : r.warnings) warnings += "- " + p.name + ": " + w + "\n";
        const std::vector<std::uint8_t> bytes = write_anim(r.file);
        std::string why;
        if (!write_text(folder + "/" + p.name + ".anim", std::string(bytes.begin(), bytes.end()), false, why)) {
            problems += "- " + p.name + ".anim: " + why + "\n";
            continue;
        }
        if (ui.to_library) anim_to_library(p.name + ".anim", bytes);
        ++written;
    }
    rescan_files();  // the folder may be one of the Inventory's
    if (!problems.empty()) message("Some expressions were not exported", problems);
    else if (!warnings.empty()) message("Exported with warnings", warnings);
    std::string note;
    for (auto& s : skipped) note += (note.empty() ? "" : ", ") + s;
    status("Exported " + std::to_string(written) + " expressions to " + folder + (ui.to_library ? ", and to the Animations library" : "") +
           (note.empty() ? "" : " (left out, they move nothing without Move face bones: " + note + ")"));
}

}  // namespace vats
