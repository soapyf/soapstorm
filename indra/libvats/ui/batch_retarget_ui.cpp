// Viewport Avatar Toolset - File > Batch Retarget Folder...: a folder of animations from another rig, one project
// or .anim each, and a report table (spec 07 RT-13, RT-14). The core does the work (vats/batch_retarget.h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <mutex>
#include <utility>
#include <optional>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/batch_retarget.h"

namespace vats {

struct BatchUi {
    std::string folder;
    BatchRetargetOptions opt;
    std::optional<BatchReport> report;
    bool mixamo_notice = false;  // RT-14: shown under this run's report
    std::mutex mutex;
    std::optional<std::string> chosen;  // the folder dialog's answer (it may come on another thread)
    bool run = false;                   // --batch-retarget: run once the window shows
};

namespace {

std::string thousands(std::size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

}  // namespace

void App::batch_retarget_folder(const std::string& dir) {
    show_batch_retarget_ = true;
    batch_ui_ = std::make_shared<BatchUi>();
    batch_ui_->opt.tables = retarget_tables();
    batch_ui_->folder = dir;
    batch_ui_->run = true;
}

void App::draw_batch_retarget() {
    if (!show_batch_retarget_) return;
    if (!batch_ui_) {
        batch_ui_ = std::make_shared<BatchUi>();
        batch_ui_->opt.tables = retarget_tables();
    }
    BatchUi& ui = *batch_ui_;
    {
        std::lock_guard<std::mutex> lock(ui.mutex);
        if (ui.chosen) ui.folder = *ui.chosen, ui.report.reset(), ui.chosen.reset();
    }
    ImGui::SetNextWindowSize(window_size(46, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Batch Retarget", &show_batch_retarget_, ImGuiWindowFlags_NoDocking)) return ImGui::End();
    help_button("retargeting");

    const float label_w = ImGui::GetFontSize() * 5;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
    };
    label("Folder");
    if (icon_label_button(icon::kOpen, "Choose...")) host_.open_folder_dialog(ui.folder, [p = batch_ui_](std::vector<std::string> files) {
        if (files.empty()) return;
        std::lock_guard<std::mutex> lock(p->mutex);
        p->chosen = files[0];
    });
    ImGui::SameLine();
    ImGui::TextWrapped("%s", ui.folder.empty() ? "(none)" : ui.folder.c_str());

    // RT-3 families and the user's saved mappings (RT-12).
    label("Rig");
    ImGui::SetNextItemWidth(-1);
    const std::string current = ui.opt.table >= 0 && ui.opt.table < int(ui.opt.tables.size())
                                    ? ui.opt.tables[size_t(ui.opt.table)].name : "Best match for each file";
    if (ImGui::BeginCombo("##batch_rig", current.c_str())) {
        if (ImGui::Selectable("Best match for each file", ui.opt.table < 0)) ui.opt.table = -1;
        for (int i = 0; i < int(ui.opt.tables.size()); ++i)
            if (ImGui::Selectable(ui.opt.tables[size_t(i)].name.c_str(), ui.opt.table == i)) ui.opt.table = i;
        ImGui::EndCombo();
    }
    label("Save as");
    int kind = ui.opt.anim ? 1 : 0;
    ImGui::RadioButton("Projects (.vat)", &kind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("SL animations (.anim)", &kind, 1);
    ui.opt.anim = kind == 1;
    retarget_settings_ui(ui.opt.retarget, ui.opt.fit, ui.opt.lock_feet);

    ImGui::BeginDisabled(ui.folder.empty());
    if (icon_label_button(icon::kBatch, "Retarget All") || std::exchange(ui.run, false)) guarded(ui.folder, [&] {
        ui.opt.retarget.shape = ui.opt.fit.shape = export_shape();
        // ponytail: runs on the UI thread, one file after another; a worker and a progress bar when folders get big.
        ui.report = batch_retarget(skel_, *rig_, ui.folder, ui.opt, [](const std::string& path, const std::string& data, std::string& why) {
            return write_text(path, data, true, why);
        });
        ui.mixamo_notice = ui.report->mixamo && !settings_.mixamo_notice_seen;
        if (ui.mixamo_notice) settings_.mixamo_notice_seen = true, save_settings();
        int written = 0;
        for (const BatchRow& r : ui.report->rows) written += !r.output.empty();
        status("Batch Retarget: " + std::to_string(written) + " of " + std::to_string(ui.report->rows.size()) +
               " files written to " + ui.report->out_dir);
    });
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Every .bvh, .fbx, .gltf and .glb in the folder, each into the folder's retargeted/ folder");
    if (ui.report) {
        ImGui::SameLine();
        if (icon_label_button(icon::kOpen, "Open Output Folder")) host_.open_url(folder_url(ui.report->out_dir));
    }

    if (ui.mixamo_notice) {  // RT-14: once, never in the way
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.45f, 1));
        ImGui::TextWrapped("Mixamo animations: Adobe's Mixamo FAQ allows them in commercial projects, but the raw "
                           "animation data may not be redistributed as standalone assets. An animation you sell or give "
                           "away should be part of your own work, not the Mixamo motion on its own.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("helpx.adobe.com/creative-cloud/faq/mixamo-faq.html");
    }

    if (ui.report) {
        ImGui::Separator();
        if (ui.report->rows.empty()) hint("The folder has no .bvh, .fbx, .gltf or .glb files.");
        else if (ImGui::BeginTable("batch_report", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                                          ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("Saved as");
            ImGui::TableSetupColumn("Fits");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Frames");
            ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const BatchRow& r : ui.report->rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.file.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.output.empty() ? "-" : r.output.c_str());
                ImGui::TableNextColumn();
                if (r.bytes) ImGui::TextColored(r.fits ? ImVec4(0.4f, 0.85f, 0.5f, 1) : ImVec4(1, 0.5f, 0.4f, 1), r.fits ? "Yes" : "No");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.bytes ? (thousands(r.bytes) + " bytes").c_str() : "-");
                ImGui::TableNextColumn();
                if (r.frames) ImGui::Text("%d at %d fps", r.frames, r.fps);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", r.notes.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

}  // namespace vats
