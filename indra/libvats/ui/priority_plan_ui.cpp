// Viewport Avatar Toolset - Tools > Priority Planner...: which animation wins each bone in SL (spec 08 PP).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The rule and the lint are in the core (vats/priority_plan.h); this is the window, the Bones list, view and
// timeline tint, and the viewer's running animations (Host::running_motions).
#include <algorithm>
#include <mutex>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/priority_plan.h"

namespace vats {

struct PlannerUi {
    std::vector<PlanClip> clips{PlanClip{"This project", {}, true, false}};  // start order; the own clip included
    std::vector<int> winner;  // per skeleton node: index into clips, -1 = nobody claims it
    std::vector<Rgb> colour;  // per clip
    std::vector<PlanFinding> findings;
    Clip seen;  // the clip the own entry was made from
    bool have = false;
    bool tint = true;
    std::mutex mutex;
    std::vector<std::string> added;  // files the file dialog chose (it may answer on another thread)
};

namespace {

// Own clip: light blue; the others in turn. No yellow: that is the selection's.
constexpr Rgb kOwn{0.35f, 0.78f, 1.0f};
constexpr Rgb kPalette[] = {{0.95f, 0.55f, 0.25f}, {0.90f, 0.40f, 0.65f}, {0.65f, 0.50f, 0.95f},
                            {0.45f, 0.85f, 0.45f}, {0.95f, 0.35f, 0.30f}, {0.30f, 0.75f, 0.70f}};

ImU32 u32(const Rgb& c, float a = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, a)); }

bool has_context(const PlannerUi& ui) { return ui.clips.size() > 1; }

void resolve(PlannerUi& ui, const Skeleton& skel) {
    ui.winner.assign(size_t(skel.size()), -1);
    for (auto& [joint, w] : plan_winners(ui.clips))
        if (int n = skel.find(joint); n >= 0) ui.winner[size_t(n)] = w;
    ui.colour.clear();
    int k = 0;
    for (const PlanClip& c : ui.clips) ui.colour.push_back(c.own ? kOwn : kPalette[k++ % int(std::size(kPalette))]);
    ui.findings = plan_lint(skel, ui.clips);
}

// "3" or "2-5" over the clip's joints.
std::string priority_range(const PlanClip& c) {
    if (c.joints.empty()) return "-";
    int lo = 99, hi = -99;
    for (auto& [j, p] : c.joints) lo = std::min(lo, p), hi = std::max(hi, p);
    return lo == hi ? std::to_string(lo) : std::to_string(lo) + "-" + std::to_string(hi);
}

// Context clips go in before the own clip: yours is the one started last until you move it.
void insert_context(PlannerUi& ui, PlanClip c) {
    auto own = std::find_if(ui.clips.begin(), ui.clips.end(), [](const PlanClip& x) { return x.own; });
    ui.clips.insert(own, std::move(c));
}

}  // namespace

bool App::planner_colour(int node, Rgb& c) const {
    if (!show_planner_ || !planner_ui_ || !planner_ui_->tint || !has_context(*planner_ui_)) return false;
    const PlannerUi& ui = *planner_ui_;
    if (node < 0 || node >= int(ui.winner.size()) || ui.winner[size_t(node)] < 0) return false;
    c = ui.colour[size_t(ui.winner[size_t(node)])];
    return true;
}

void App::planner_row_mark(int node) {
    Rgb c;
    if (!planner_colour(node, c)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, u32(c, 0.14f));
    dl->AddRectFilled(ImVec2(b.x - 5, a.y + 1), ImVec2(b.x - 1, b.y - 1), u32(c));
}

void App::draw_planner_band(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    Rgb c;
    if (primary() < 0 || !planner_colour(primary(), c)) return;
    const PlannerUi& ui = *planner_ui_;
    const PlanClip& w = ui.clips[size_t(ui.winner[size_t(primary())])];
    dl->AddRectFilled(a, b, u32(c, 0.8f));
    const std::string text = skel_[primary()].name + ": " + w.name + " wins (" +
                             std::to_string(w.joints.at(skel_[primary()].name)) + ")";
    dl->AddText(ImVec2(a.x + 4, a.y - ImGui::GetTextLineHeight() - 1), u32(c), text.c_str());
}

void App::planner_add(const std::string& path) {
    show_planner_ = true;
    if (!planner_ui_) planner_ui_ = std::make_shared<PlannerUi>();
    std::lock_guard<std::mutex> lock(planner_ui_->mutex);
    planner_ui_->added.push_back(path);
}

void App::draw_planner_panel() {
    if (!show_planner_) return;
    if (!planner_ui_) planner_ui_ = std::make_shared<PlannerUi>();
    PlannerUi& ui = *planner_ui_;
    bool changed = false;

    // Files chosen with Add Clips... (PP-1): read on this thread.
    std::vector<std::string> added;
    {
        std::lock_guard<std::mutex> lock(ui.mutex);
        added.swap(ui.added);
    }
    for (const std::string& path : added) {
        PlanClip c;
        std::string err;
        if (!load_plan_clip(skel_, path, c, err)) {
            message("Could not add a clip", path + ": " + err);
            continue;
        }
        insert_context(ui, std::move(c));
        changed = true;
    }
    // The own entry follows the project, as it exports, once nothing is being dragged.
    if (!doc_.history.is_open() && !dragging_gizmo_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        (!ui.have || !(doc_.clip() == ui.seen))) {
        ui.seen = doc_.clip();
        ui.have = true;
        for (PlanClip& c : ui.clips)
            if (c.own) {
                c = plan_clip_from_clip(skel_, doc_.clip(), anim_export_options(), "This project");
                c.own = true;
            }
        changed = true;
    }
    if (changed || ui.winner.size() != size_t(skel_.size())) resolve(ui, skel_);

    place_tool_window("Priority Planner", 30, 30);
    if (!ImGui::Begin("Priority Planner", &show_planner_)) return ImGui::End();
    help_button("priority-planner");
    hint("Add the animations yours plays with to see which wins each bone.");

    if (primary_button("Add Clips...", "", 0, icon::kAdd)) {
        host_.open_file_dialog({{"Animation", "anim;vat"}}, true, [p = planner_ui_](std::vector<std::string> files) {
            std::lock_guard<std::mutex> lock(p->mutex);
            for (auto& f : files) p->added.push_back(f);
        });
    }
    ImGui::SetItemTooltip("Your own .anim files or projects: an AO stand, a dance, a furniture pose");
    if (!host_.host_name().empty()) {  // the viewer: what runs on your own avatar (PP-5)
        ImGui::SameLine();
        if (ImGui::Button("Add Running Animations")) {
            std::vector<PlanClip> running = host_.running_motions();
            if (running.empty()) status("No animations are running on your avatar");
            for (PlanClip& c : running) {
                std::string low = c.name;
                for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                c.own = false, c.stand = low.find("stand") != std::string::npos;
                insert_context(ui, std::move(c));
            }
            resolve(ui, skel_);
        }
        ImGui::SetItemTooltip("The names and priorities of the animations playing on your avatar now");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Tint bones", &ui.tint);
    ImGui::SetItemTooltip("Colour the Bones list, the bones in the view and the timeline by who wins each bone");

    // The clips in start order.
    int move = -1, dir = 0, remove = -1;
    if (ImGui::BeginTable("clips", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Priority");
        ImGui::TableSetupColumn("Bones");
        ImGui::TableSetupColumn("Stand");
        ImGui::TableSetupColumn("##order");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(ui.clips.size()); ++i) {
            PlanClip& c = ui.clips[size_t(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::ColorButton("##c", ImVec4(ui.colour[size_t(i)].r, ui.colour[size_t(i)].g, ui.colour[size_t(i)].b, 1),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFrameHeight() * 0.6f, ImGui::GetFrameHeight() * 0.6f));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(priority_range(c).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", c.joints.size());
            ImGui::TableNextColumn();
            if (!c.own && ImGui::Checkbox("##stand", &c.stand)) changed = true;
            if (!c.own) ImGui::SetItemTooltip("An AO stand (the stand rule below)");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (icon_small_button("up", icon::kUp, "Started earlier")) move = i, dir = -1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == int(ui.clips.size()));
            if (icon_small_button("down", icon::kDown, "Started later")) move = i, dir = 1;
            ImGui::EndDisabled();
            if (!c.own) {
                ImGui::SameLine();
                if (icon_small_button("remove", icon::kDelete, "Remove")) remove = i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move >= 0) std::swap(ui.clips[size_t(move)], ui.clips[size_t(move + dir)]), changed = true;
    if (remove >= 0) ui.clips.erase(ui.clips.begin() + remove), changed = true;
    if (changed) resolve(ui, skel_);

    // The selected bone: who claims it, the winner first.
    if (const int p = primary(); p >= 0 && has_context(ui)) {
        subheading(skel_[p].name.c_str());
        const int w = ui.winner[size_t(p)];
        if (w < 0) hint("No clip keys it: SL's default pose (or the AO's own) shows.");
        for (int i = int(ui.clips.size()) - 1; i >= 0; --i) {  // later starts first: they win ties
            auto it = ui.clips[size_t(i)].joints.find(skel_[p].name);
            if (it == ui.clips[size_t(i)].joints.end()) continue;
            const Rgb& c = ui.colour[size_t(i)];
            ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1), "%s %s at %d", i == w ? "Wins:" : "Loses:",
                               ui.clips[size_t(i)].name.c_str(), it->second);
        }
    }

    subheading("Your clip");
    bool any = false;
    for (const PlanFinding& f : ui.findings)
        if (f.rule == "loses") ImGui::TextWrapped("%s", f.message.c_str()), any = true;
    if (!any) hint(has_context(ui) ? "Wins every bone it keys." : "Add a clip to compare with.");
    any = false;
    for (const PlanFinding& f : ui.findings) {
        if (f.rule == "loses") continue;
        if (!any) subheading("For AO makers"), any = true;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.75f, 0.4f, 1));
        ImGui::TextWrapped("%s", f.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

}  // namespace vats
