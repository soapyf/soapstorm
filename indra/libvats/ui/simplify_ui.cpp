// Viewport Avatar Toolset - Edit > Simplify Curves...: dense curves back to few keys, with a live preview.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 15 (SC). The fit is in the core (simplify.h).
#include <algorithm>
#include <cmath>

#include "app.h"
#include "imgui.h"
#include "widgets.h"
#include "vats/footlock.h"

namespace vats {

void App::open_simplify() {
    if (doc_.history.is_open()) return status("Finish the current edit first");
    if (doc_.clip().curves.empty()) return status("Nothing to simplify: the animation has no keys");
    simplify_tracks_ = selected_tracks();
    simplify_all_ = simplify_tracks_.empty();
    simplify_.from = 0, simplify_.to = std::max(doc_.clip().end_frame, 1);
    simplify_before_ = doc_.clip();
    simplify_contacts_.clear();
    FootLockOptions fl;
    fl.shape = export_shape();
    for (const FootContact& c : find_foot_contacts(*rig_, simplify_before_, fl))
        simplify_contacts_.push_back(c.from), simplify_contacts_.push_back(c.to);
    graph_.snapshot_curves(simplify_before_);  // the curves as they were, grey in the graph (PT-4)
    simplify_dim_ = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w;
    doc_.history.begin(doc_.clip());  // the preview is an open step: OK commits it, Cancel puts the clip back
    simplify_open_ = true;
    preview_simplify();
}

void App::preview_simplify() {
    SimplifyOptions o = simplify_;
    if (simplify_planted_) o.keep = simplify_contacts_;
    doc_.clip() = simplify_before_;
    simplify_result_ = simplify_curves(doc_.clip(), simplify_all_ ? std::vector<std::string>{} : simplify_tracks_, o);
}

void App::draw_simplify_dialog() {
    if (!simplify_open_) return;
    const char* title = "Simplify Curves";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    // Bottom right and without the modal dimming, as Filter Curves: the view and the graph show the preview.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16, vp->WorkPos.y + vp->WorkSize.y - 16), ImGuiCond_Appearing,
                            ImVec2(1, 1));
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = 0;
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const float label_w = ImGui::GetFontSize() * 8, field_w = ImGui::GetFontSize() * 14;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(field_w);
    };
    bool changed = false;
    ImGui::BeginDisabled(simplify_tracks_.empty());
    changed |= ImGui::Checkbox("All bones", &simplify_all_);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Off: only the %zu selected track(s)", simplify_tracks_.size());
    float deg = float(simplify_.tol_deg), mm = float(simplify_.tol_mm);
    label("Rotation");
    if (slider_float("##sdeg", &deg, 0.01f, 5.f, "%.2f deg", 0, SliderCurve::Log))
        simplify_.tol_deg = std::clamp(double(deg), 0.01, 5.0), changed = true;
    ImGui::SetItemTooltip("How far a rotation curve may move from where it was, at any whole frame");
    label("Position");
    if (slider_float("##smm", &mm, 0.05f, 20.f, "%.2f mm", 0, SliderCurve::Log))
        simplify_.tol_mm = std::clamp(double(mm), 0.05, 20.0), changed = true;
    ImGui::SetItemTooltip("The same for position curves (the hips' travel)");
    label("From");
    const float half_w = (field_w - ImGui::CalcTextSize("to").x - 2 * ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::SetNextItemWidth(half_w);
    changed |= ImGui::InputInt("##sfrom", &simplify_.from, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half_w);
    changed |= ImGui::InputInt("##sto", &simplify_.to, 0);
    simplify_.from = std::clamp(simplify_.from, 0, 100000), simplify_.to = std::clamp(simplify_.to, simplify_.from, 100000);
    ImGui::BeginDisabled(simplify_contacts_.empty());
    changed |= ImGui::Checkbox("Keep frames where feet are planted", &simplify_planted_);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(simplify_contacts_.empty() ? "No foot contacts found in this animation"
                                                     : "Every curve keeps a key where a foot plants and where it lifts");
    // Scrub here: the dialog holds the rest of the app while it is open.
    label("Frame");
    float fr = float(frame_);
    if (slider_float("##sframe", &fr, 0, float(std::max(doc_.clip().end_frame, 1)), "%.0f")) set_frame(std::round(fr));
    if (changed) preview_simplify();

    ImGui::Separator();
    ImGui::Text("Keys in the range: %d -> %d", simplify_result_.before, simplify_result_.after);
    ImGui::TextDisabled("The grey curves in the graph are the curves as they were.");
    for (size_t i = 0; i < simplify_result_.skipped.size() && i < 6; ++i) ImGui::TextDisabled("%s", simplify_result_.skipped[i].c_str());
    if (simplify_result_.skipped.size() > 6) ImGui::TextDisabled("and %zu more left as they are", simplify_result_.skipped.size() - 6);

    auto close = [&] {
        ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = simplify_dim_;
        simplify_open_ = false;
        simplify_before_ = Clip{};
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        if (doc_.history.commit("Simplify Curves", doc_.clip())) mark_dirty();
        graph_.clip_replaced();  // the range's keys were replaced
        status("Simplified: " + count_noun(simplify_result_.before, "key") + " to " + std::to_string(simplify_result_.after));
        close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        doc_.clip() = doc_.history.cancel();
        close();
    }
    ImGui::EndPopup();
}

}  // namespace vats
