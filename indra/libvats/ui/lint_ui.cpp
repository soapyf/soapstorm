// Viewport Avatar Toolset - the Animation Check window and its status-bar badge (spec 08 CK).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The checks and fixes are in the core (lint.h); this file re-runs them when the clip has been still for a moment
// and shows the findings. Host calls only, so the viewer has it too.
#include <algorithm>
#include <cmath>
#include <optional>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/lint.h"

namespace vats {

struct CheckUi {
    Clip seen;                 // the clip as last seen, to notice edits
    std::string ao_state;      // and its AO state (the priority rule reads it)
    bool have = false, due = true, at_once = false;
    std::uint64_t changed_ns = 0;  // when seen last changed
    unsigned generation = ~0u;     // the document seen: a new one (open, import) is checked at once
    std::vector<LintFinding> findings;
};

namespace {

constexpr std::uint64_t kIdleNs = 500'000'000;  // re-check once the clip has been still this long
constexpr ImU32 kContactMark = IM_COL32(235, 80, 70, 230);  // self-contact frames on the timeline (08 SX)

ImU32 severity_colour(LintSeverity s) {
    return s == LintSeverity::Error ? IM_COL32(235, 80, 70, 255) : s == LintSeverity::Warning ? IM_COL32(240, 180, 70, 255)
                                                                                             : IM_COL32(110, 170, 240, 255);
}

// Error: a filled circle with a cross; Warning: a triangle with a bar; Info: a ring with an i.
void severity_icon(LintSeverity s) {
    const float sz = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(sz, sz));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = severity_colour(s);
    const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    const float r = sz * 0.42f, w = std::max(1.5f, sz / 10);
    if (s == LintSeverity::Error) {
        dl->AddCircleFilled(c, r, col);
        const float d = r * 0.45f;
        const ImU32 bg = IM_COL32(255, 255, 255, 255);
        dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), bg, w);
        dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), bg, w);
    } else if (s == LintSeverity::Warning) {
        dl->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r * 0.8f), ImVec2(c.x - r, c.y + r * 0.8f), col, w);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.25f), col, w);
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.55f), w * 0.6f, col);
    } else {
        dl->AddCircle(c, r, col, 0, w);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.1f), ImVec2(c.x, c.y + r * 0.5f), col, w);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.45f), w * 0.6f, col);
    }
}

}  // namespace

// Runs every frame, shown or not, so the badge stays current: after an edit, once the clip has been still for
// kIdleNs and no drag, field edit or playback is running.
void App::update_check() {
    if (!check_ui_) check_ui_ = std::make_shared<CheckUi>();
    CheckUi& ui = *check_ui_;
    if (doc_.history.is_open() || playing_ || dragging_gizmo_ || modal_ != Modal::None) return;
    const std::uint64_t now = host_.ticks_ns();
    const Clip& clip = doc_.clip();
    if (!ui.have || !(clip == ui.seen) || ui.ao_state != active_ao_state()) {
        ui.seen = clip;
        ui.ao_state = active_ao_state();
        ui.have = ui.due = true;
        ui.changed_ns = now;
        ui.at_once = ui.generation != doc_generation_;
        ui.generation = doc_generation_;
    }
    if (!ui.due || (!ui.at_once && now - ui.changed_ns < kIdleNs)) return;
    run_check();
}

std::string App::active_ao_state() const {
    const Project& p = doc_.project;
    return p.active_clip >= 0 && p.active_clip < int(p.clips.size()) ? p.clips[p.active_clip].ao_state : "";
}

void App::run_check() {
    CheckUi& ui = *check_ui_;
    ui.seen = doc_.clip();
    ui.have = true;
    ui.due = ui.at_once = false;
    const Clip& clip = doc_.clip();
    AnimExportOptions opt;  // as anim_bytes builds them; the clip's own reduce and leave_static are read by lint_clip
    opt.shape = export_shape();
    opt.positions = export_positions();
    opt.worn_overrides = host_.joint_overrides();
    if (multi_actor()) opt.external = actor_resolver(doc_.project.active);
    ui.ao_state = active_ao_state();
    // The other actors of a scene, frame by frame in this one's space, for the cross-actor contact rule.
    std::vector<LintPartner> partners;
    const Project& p = doc_.project;
    if (multi_actor() && std::find(settings_.check_off.begin(), settings_.check_off.end(), "actor_contact") == settings_.check_off.end())
        for (int i = 0; i < int(p.actors.size()); ++i) {
            if (i == p.active || p.actors[i].hidden) continue;
            LintPartner other{p.actors[i].name, {}};
            const Xform rel = actor_rel(i);
            for (int f = 0; f <= std::max(clip.end_frame, 1); ++f) {
                std::vector<Xform> g = evaluate_actor(i, f).globals;
                for (Xform& x : g) x = rel * x;
                other.frames.push_back(std::move(g));
            }
            partners.push_back(std::move(other));
        }
    ui.findings = lint_clip(skel_, clip, opt, settings_.check_off, mesh_body() ? view_body_shape() : nullptr, ui.ao_state,
                            partners);
}

// The self-penetration findings (08 SX) at a whole frame: their bones are tinted in the view.
bool App::contact_bone(int node) const {
    if (!check_ui_ || globals_.empty()) return false;
    const int here = int(std::floor(frame_ + 1e-9));
    for (const LintFinding& f : check_ui_->findings)
        if (f.rule == "self_contact" && std::binary_search(f.frames.begin(), f.frames.end(), here))
            for (const std::string& b : f.bones)
                if (skel_.find(b) == node) return true;
    return false;
}

// The self-penetration findings' frames (08 SX): a short red mark at the foot of the timeline strip for each.
void App::draw_contact_marks(ImDrawList* dl, float x0, float x1, float y1, int last) const {
    if (!check_ui_) return;
    for (const LintFinding& f : check_ui_->findings)
        if (f.rule == "self_contact")
            for (int fr : f.frames) {
                const float x = x0 + (x1 - x0) * float(fr) / float(std::max(last, 1));
                dl->AddLine(ImVec2(x, y1 - 6), ImVec2(x, y1), kContactMark, 2);
            }
}

void App::draw_check_badge() {
    if (!check_ui_ || check_ui_->findings.empty()) return;
    LintSeverity worst = LintSeverity::Info;
    for (const LintFinding& f : check_ui_->findings) worst = std::min(worst, f.severity);
    ImGui::SameLine(0, 24);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(severity_colour(worst)));
    const bool open = ImGui::SmallButton(
        (std::string(icon::kWarning) + " Check: " + std::to_string(check_ui_->findings.size()) + "###check_badge").c_str());
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Animation Check: problems Second Life will show. Click to see them.");
    if (open) show_check_ = true;
}

void App::draw_check_panel() {
    update_check();
    if (!show_check_ || ImGui::GetFrameCount() < 3) return;  // placed beside the view once its size is known (--tab)
    place_tool_window(30, 32);
    if (!ImGui::Begin("Animation Check", &show_check_)) return ImGui::End();
    help_button("animation-check");
    CheckUi& ui = *check_ui_;
    if (ui.due) hint("Checking once the animation is still...");
    else if (ui.findings.empty()) hint("No problems found.");
    else hint("Problems Second Life will show. Fix applies the suggested change as one undo step.");
    if (icon_label_small_button(icon::kRefresh, "Check Again")) ui.due = ui.at_once = true;
    ImGui::SetItemTooltip("After changing the body, the bake shape or the worn avatar, which the check cannot see change");
    ImGui::Separator();

    const std::vector<LintFinding> findings = ui.findings;  // a Fix below re-checks and replaces the list
    ImGui::BeginChild("##findings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f));
    for (size_t i = 0; i < findings.size(); ++i) {
        const LintFinding& f = findings[i];
        ImGui::PushID(int(i));
        severity_icon(f.severity);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", f.message.c_str());
        ImGui::Indent(ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(!f.fix.apply);
        if (icon_label_small_button(icon::kFix, "Fix")) {
            // A list made before the latest edit is stale: check now and take the same finding's fix from the fresh
            // list, so the first click always applies (it used to be greyed out until the idle re-check).
            std::optional<LintFix> fix = f.fix;
            if (ui.due) {
                run_check();
                fix.reset();
                for (const LintFinding& g : ui.findings)
                    if (g.rule == f.rule && g.bones == f.bones && g.fix.apply) fix = g.fix;
            }
            if (fix) {
                edit(fix->label, [&](Clip& c) { fix->apply(c); });
                run_check();  // the result shows at once
                const size_t left = ui.findings.size();
                status("Animation Check: " + fix->label + (left ? " (" + std::to_string(left) + " left)" : " (no problems left)"));
            } else {
                status("Animation Check: that problem is gone since the last edit");
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", f.fix.apply ? f.fix.label.c_str() : "No automatic fix: see the help page");
        std::vector<int> nodes;
        for (const std::string& b : f.bones)
            if (int n = skel_.find(b); n >= 0) nodes.push_back(n);
        if (!nodes.empty()) {
            ImGui::SameLine();
            if (icon_label_small_button(icon::kSelect, "Select Bones")) {
                clear_selection();
                for (int n : nodes) select(n, true);
            }
        }
        if (!f.frames.empty()) {
            // The start of the next run of the finding's frames, wrapping: repeated clicks step through the runs.
            const int to = lint_goto_frame(f.frames, int(std::lround(frame_)));
            ImGui::SameLine();
            if (icon_label_small_button(icon::kGoTo, ("Go to Frame " + std::to_string(to) + "###goto").c_str())) set_frame(to);
            if (const int runs = lint_frame_runs(f.frames); runs > 1)
                ImGui::SetItemTooltip("%zu frames in %d stretches; click again for the next", f.frames.size(), runs);
            else if (f.frames.size() > 1)
                ImGui::SetItemTooltip("Frames %d to %d", f.frames.front(), f.frames.back());
        }
        ImGui::Unindent(ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Per-rule switches, remembered in the settings.
    if (icon_label_button(icon::kRules, "Rules...")) ImGui::OpenPopup("##check_rules");
    if (!settings_.check_off.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu switched off", settings_.check_off.size());
    }
    if (ImGui::BeginPopup("##check_rules")) {
        for (const LintRule& r : lint_rules()) {
            auto it = std::find(settings_.check_off.begin(), settings_.check_off.end(), r.id);
            bool on = it == settings_.check_off.end();
            if (ImGui::Checkbox(r.title, &on)) {
                if (on) settings_.check_off.erase(it);
                else settings_.check_off.push_back(r.id);
                save_settings();
                ui.due = ui.at_once = true;  // re-check now
            }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

}  // namespace vats
