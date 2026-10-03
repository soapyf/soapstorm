// Viewport Avatar Toolset - the Tween slider and drag, the Blend slider after a pose, and the graph's easing menu.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.6 (TW-1..TW-3). The logic is in the core (tween.h).
#include <algorithm>
#include <cmath>
#include <string>

#include "app.h"
#include "theme.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "widgets.h"
#include "vats/tween.h"

namespace vats {

namespace {
constexpr double kBlendSeconds = 5;   // how long Blend stays after a pose, untouched
constexpr float kTweenPctPerPx = 0.4f;  // the Tween drag: 350 px from -20% to 120%
}  // namespace

bool App::begin_tween() {
    tween_on_ = tween_tracks(*rig_, doc_.clip(), frame_, selected_tracks());
    if (tween_on_.empty()) return false;
    doc_.history.begin(doc_.clip());
    tween_base_ = doc_.clip();
    return true;
}

void App::apply_tween() {
    Clip& clip = doc_.clip();
    for (const std::string& name : tween_on_) {  // every value starts again from the clip before the tween
        auto it = tween_base_.curves.find(name);
        if (it == tween_base_.curves.end()) clip.curves.erase(name);
        else clip.curves[name] = it->second;
    }
    const int n = tween(clip, tween_on_, frame_, tween_pct_ / 100.0, tween_relax_ ? TweenMode::Relax : TweenMode::Breakdown);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%.0f%%", tween_pct_);
    if (n) status(std::string(tween_relax_ ? "Relax " : "Tween ") + pct + ": keyed " + count_noun(size_t(n), "item") + " at frame " +
                  std::to_string(int(frame_)));
    else status(tween_relax_ ? "Relax needs a key at this frame and another key on the same item"
                             : "Tween needs a key before and after this frame on the selected items");
}

void App::start_tween() {
    if (modal_ != Modal::None || doc_.history.is_open()) return status("Finish the current edit first");
    if (!begin_tween()) return status("Select a bone first");
    modal_ = Modal::Tween;
    modal_press_ = ImGui::GetIO().MousePos;
    tween_press_pct_ = tween_pct_;
    apply_tween();
}

void App::tween_drag(ImVec2 m) {
    float pct = tween_press_pct_ + (m.x - modal_press_.x) * kTweenPctPerPx;
    if (ImGui::GetIO().KeyCtrl) pct = std::round(pct / 10) * 10;  // Ctrl: steps of 10%
    pct = std::clamp(pct, float(kTweenMin * 100), float(kTweenMax * 100));
    if (pct != tween_pct_) {
        tween_pct_ = pct;
        apply_tween();
    }
    char r[160];
    std::snprintf(r, sizeof r, "%s %.0f%%   move left / right, Ctrl: 10%% steps   click or Enter: key   Esc: cancel",
                  tween_relax_ ? "Relax" : "Tween", tween_pct_);
    modal_readout_ = r;
}

void App::offer_pose_blend(Clip before, double frame) {
    if (doc_.clip() == before) return;  // nothing was applied
    pose_blend_ = PoseBlend{std::move(before), doc_.clip(), doc_.clip(), frame, ImGui::GetTime() + kBlendSeconds,
                            doc_.project.active, 100};
}

void App::draw_tween_controls(bool compact) {
    const float em = ImGui::GetFontSize();
    ImGui::SameLine(0, 16);
    const bool none = selection_.empty() && handles_.empty();
    ImGui::BeginDisabled(none && modal_ != Modal::Tween);
    ImGui::SetNextItemWidth(em * (compact ? 9 : 12));
    const std::string fmt = tween_relax_ ? std::string(icon::kRelax) + " Relax %.0f%%" : std::string(icon::kTween) + " Tween %.0f%%";
    const bool changed = slider_float("##tween", &tween_pct_, float(kTweenMin * 100), float(kTweenMax * 100), fmt.c_str());
    const std::string k = key_hint("tween");
    std::string tip = tween_relax_ ? "Relax: pull the selected items' keys at this frame toward the curve their neighbouring "
                                     "keys make (100% = on it)"
                                   : "Tween: key the selected items at this frame, part of the way from the pose at the previous "
                                     "key to the pose at the next (0% = previous, 100% = next)";
    if (!k.empty()) tip += ". Or press " + k + " and move the mouse";
    ImGui::SetItemTooltip("%s. Double-click to type a value", tip.c_str());
    if (ImGui::IsItemActivated()) {
        tween_on_.clear();  // a drag's tracks are its own
        if (modal_ == Modal::None && !doc_.history.is_open() && !begin_tween()) status("Select a bone first");
    }
    // A typed value (Ctrl+click, Enter) arrives as the field lets go, no longer active: it applies as a drag does,
    // and Enter on the value already shown keys it too.
    if ((changed || item_entered()) && !tween_on_.empty() && doc_.history.is_open()) apply_tween();
    if (ImGui::IsItemDeactivated() && modal_ == Modal::None && doc_.history.is_open() && !tween_on_.empty()) {
        if (doc_.history.commit("Tween", doc_.clip())) mark_dirty();
        tween_on_.clear();
    }
    ImGui::SameLine();
    // A toggle like the toolbar's others (a checkbox here read as an empty button); the icon alone when narrow.
    const char* relax_tip = "Relax: the slider pulls existing keys toward the curve instead of placing a breakdown";
    if (compact ? icon_button("relax", icon::kRelax, relax_tip, tween_relax_)
                : icon_label_button(icon::kRelax, "Relax###relax", relax_tip, tween_relax_))
        tween_relax_ = !tween_relax_;
    ImGui::EndDisabled();
    tween_row_end_ = ImGui::GetItemRectMax().x;  // the timeline bar's width with names, without the passing Blend

    // TW-2: Blend, for a few seconds after a pose went on (longer while the pointer is on it).
    if (!pose_blend_) return;
    PoseBlend& b = *pose_blend_;
    if (b.actor != doc_.project.active || (ImGui::GetTime() > b.until && !doc_.history.is_open())) {
        pose_blend_.reset();
        return;
    }
    ImGui::SameLine(0, 16);
    if (ImGui::GetContentRegionAvail().x < em * 7) ImGui::NewLine();  // past the panel's edge: the row below
    ImGui::SetNextItemWidth(em * 7);
    const bool moved = slider_float("##blend", &b.pct, 0, 150, (std::string(icon::kBlend) + " Blend %.0f%%").c_str());
    ImGui::SetItemTooltip("Blend the pose just applied with the pose before it: 0%% = as before, 100%% = as applied, "
                          "150%% = pushed further");
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) b.until = ImGui::GetTime() + kBlendSeconds;
    if (ImGui::IsItemActivated()) {
        if (doc_.clip() != b.shown || doc_.history.is_open()) {  // edited since: the pose is not the last thing done
            pose_blend_.reset();
            return status("The pose has been edited since it was applied; Blend works right after applying one");
        }
        doc_.history.begin(doc_.clip());
    }
    if (moved && ImGui::IsItemActive()) {
        blend_pose(doc_.clip(), b.before, b.after, b.frame, b.pct / 100.0);
        char s[64];
        std::snprintf(s, sizeof s, "Blend %.0f%% at frame %d", b.pct, int(b.frame));
        status(s);
    }
    if (ImGui::IsItemDeactivated()) {
        if (doc_.history.commit("Blend Pose", doc_.clip())) mark_dirty();
        b.shown = doc_.clip();
    }
}

// TW-3: the graph toolbar's Ease menu.
void GraphEditor::draw_ease_menu(GraphContext& ctx) {
    if (icon_button("ease", icon::kEase, "Ease: easing presets for the selected keys; shapes the segment after each "
                                         "selected key, up to the last selected key of a curve")) {
        if (selection_.empty()) ctx.status("Select keys in the graph first");
        else ImGui::OpenPopup("##ease");
    }
    if (!ImGui::BeginPopup("##ease")) return;
    static const char* const dirs[] = {"Ease In", "Ease Out", "Ease In-Out"};
    static const char* const shapes[] = {"Quad", "Cubic", "Sine", "Back", "Elastic", "Bounce"};
    for (int d = 0; d < 3; ++d) {
        if (!ImGui::BeginMenu(dirs[d])) continue;
        for (int s = 0; s < 6; ++s) {
            if (s == 3) subheading("Baked: a key per frame");
            if (!ImGui::MenuItem(shapes[s])) continue;
            const std::string label = std::string(dirs[d]) + " " + shapes[s];
            auto sel = selection_;
            int n = 0;
            edit(ctx, label.c_str(), [&](Clip& c) { n = apply_ease(c, sel, EaseShape(s), EaseDir(d)); });
            selection_ = sel;
            ctx.status(n ? label + ": " + count_noun(size_t(n), "segment") : "There is no key after the selected keys to ease toward");
        }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

}  // namespace vats
