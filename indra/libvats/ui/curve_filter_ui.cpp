// Viewport Avatar Toolset - the graph editor's Filter Curves dialog and the filter settings shared with the
// motion capture clean-up.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 3 (MC-4a). The filters are in the core (curve_filter.h).
#include <algorithm>
#include <cmath>

#include "graph_editor.h"
#include "widgets.h"

namespace vats {

bool filter_kind_ui(FilterKind& k) {
    int i = int(k);
    const char* kinds[] = {filter_name(FilterKind::OneEuro), filter_name(FilterKind::SavitzkyGolay),
                           filter_name(FilterKind::Butterworth)};
    if (!ImGui::Combo("##filter_kind", &i, kinds, 3)) return false;
    k = FilterKind(i);
    return true;
}

bool filter_params_ui(FilterSettings& s, const std::function<void(const char*)>& label) {
    bool changed = false;
    auto slider = [&](const char* name, const char* id, double& v, double lo, double hi, const char* fmt, const char* tip,
                      bool log = false) {
        label(name);
        float f = float(v);
        if (slider_float(id, &f, float(lo), float(hi), fmt, 0, log ? SliderCurve::Log : SliderCurve::Linear))
            v = std::clamp(double(f), lo, hi), changed = true;
        ImGui::SetItemTooltip("%s", tip);
    };
    auto int_slider = [&](const char* name, const char* id, int& v, int lo, int hi, const char* fmt, const char* tip) {
        label(name);
        if (vats::slider_int(id, &v, lo, hi, fmt)) v = std::clamp(v, lo, hi), changed = true;
        ImGui::SetItemTooltip("%s", tip);
    };
    switch (s.kind) {
        case FilterKind::OneEuro:
            slider("Min cutoff", "##fmin", s.min_cutoff, 0.1, 10, "%.2f Hz", "The cutoff while the joint is still: lower calms more jitter and lags more.", true);
            slider("Speed", "##fbeta", s.beta, 0, 0.2, "%.3f per deg/s", "How fast the cutoff rises with rotation speed: higher follows quick moves with less lag.", true);
            slider("Speed (position)", "##fbetam", s.beta_m, 0, 100, "%.1f per m/s", "The same for position curves (the hips' travel), in metres.", true);
            slider("Speed cutoff", "##fdcut", s.d_cutoff, 0.1, 10, "%.2f Hz", "Smooths the speed estimate that drives the cutoff.", true);
            break;
        case FilterKind::SavitzkyGolay: {
            int_slider("Window", "##fhalf", s.sg_half, 1, 15, "+-%d frames", "Frames fitted either side of each frame.");
            int_slider("Degree", "##forder", s.sg_order, 0, 5, "%d", "The fitted polynomial's degree: higher keeps peaks sharper and calms less.");
            s.sg_order = std::min(s.sg_order, 2 * s.sg_half - 1);
            break;
        }
        case FilterKind::Butterworth: {
            slider("Cutoff", "##fcut", s.cutoff, 0.5, 15, "%.1f Hz", "Motion faster than this is removed; run forward and back so nothing lags.", true);
            int sections = s.order / 2;
            int_slider("Sections", "##fsec", sections, 1, 4, "%d", "Second-order sections per pass: more cut off more sharply.");
            s.order = sections * 2;
            break;
        }
    }
    return changed;
}

void GraphEditor::open_filter(GraphContext& ctx) {
    if (ctx.history.is_open()) return ctx.status("Finish the current edit first");
    filter_ids_.clear();
    for (int c : shown_channels())
        if (const FCurve* cv = curve(ctx.clip, channels_[c]); cv && !cv->empty())
            filter_ids_.push_back({channels_[c].track, channels_[c].channel});
    if (filter_ids_.empty()) return ctx.status("Select a bone with curves first");
    double a = 0, b = 0;
    if (key_span(ctx.clip, a, b)) filter_from_ = int(a), filter_to_ = int(b);
    else if (ctx.clip.loop) filter_from_ = ctx.clip.loop_in, filter_to_ = ctx.clip.loop_out;
    else filter_from_ = 0, filter_to_ = std::max(ctx.clip.end_frame, 1);
    filter_before_ = ctx.clip;
    filter_dim_ = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w;
    ctx.history.begin(ctx.clip);  // the dialog's preview is an open step: OK commits it, Cancel puts the clip back
    filter_open_ = true;
    preview_filter(ctx);
}

void GraphEditor::preview_filter(GraphContext& ctx) {
    std::vector<std::string> tracks;
    for (const CurveId& id : filter_ids_)
        if (std::find(tracks.begin(), tracks.end(), id.track) == tracks.end()) tracks.push_back(id.track);
    ctx.clip = filter_before_;
    filter_curves(ctx.clip, filter_ids_, filter_from_, filter_to_, filter_);
    filter_shake_[0] = shake_scores(filter_before_, tracks, filter_from_, filter_to_);
    filter_shake_[1] = shake_scores(ctx.clip, tracks, filter_from_, filter_to_);
}

void GraphEditor::draw_filter_dialog(GraphContext& ctx) {
    if (!filter_open_) return;
    const char* title = "Filter Curves";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    // Bottom right and without the modal dimming, so the view shows the preview and the ghost while it is open.
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
    ImGui::Text("%zu curve(s). The ghost is the pose before filtering.", filter_ids_.size());
    bool changed = false;
    label("Filter");
    changed |= filter_kind_ui(filter_.kind);
    changed |= filter_params_ui(filter_, label);
    label("From");
    const float half_w = (field_w - ImGui::CalcTextSize("to").x - 2 * ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::SetNextItemWidth(half_w);
    changed |= ImGui::InputInt("##ffrom", &filter_from_, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half_w);
    changed |= ImGui::InputInt("##fto", &filter_to_, 0);
    filter_from_ = std::clamp(filter_from_, 0, 100000), filter_to_ = std::clamp(filter_to_, filter_from_, 100000);
    if (ctx.clip.loop && filter_from_ >= ctx.clip.loop_in && filter_to_ <= ctx.clip.loop_out)
        ImGui::TextDisabled("Inside the loop: filtered as a loop, so the seam stays joined.");
    // Scrub here: the dialog holds the rest of the app while it is open.
    label("Frame");
    float fr = float(ctx.frame);
    if (slider_float("##fframe", &fr, 0, float(std::max(ctx.clip.end_frame, 1)), "%.0f")) ctx.frame = std::round(fr);
    if (changed) preview_filter(ctx);

    // Shake score per bone: the RMS of the jerk (third difference), before and after.
    if (ImGui::BeginTable("##shake", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(label_w + field_w, ImGui::GetTextLineHeightWithSpacing() * 7))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Shake", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn("After");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < filter_shake_[0].size() && i < filter_shake_[1].size(); ++i) {
            const JointShake &b = filter_shake_[0][i], &a = filter_shake_[1][i];
            const std::string name = ctx.item_label ? ctx.item_label(b.track) : b.track;
            auto row = [&](const std::string& text, double before, double after, bool metres) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(), ImGui::TextUnformatted(text.c_str());
                for (double v : {before, after}) {
                    ImGui::TableNextColumn();
                    metres ? ImGui::Text("%.2f m/s3", v) : ImGui::Text("%.0f deg/s3", v);
                }
            };
            if (b.rot > 0) row(name, b.rot, a.rot, false);
            if (b.pos > 0) row(name + " position", b.pos, a.pos, true);
        }
        ImGui::EndTable();
    }
    auto close = [&] {
        ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = filter_dim_;
        filter_open_ = false;
        filter_before_ = Clip{};
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        if (ctx.history.commit("Filter Curves", ctx.clip)) ctx.changed();
        selection_.clear();  // the range's keys were replaced
        ctx.status(std::string(filter_name(filter_.kind)) + " filter on " + std::to_string(filter_ids_.size()) + " curve(s)");
        close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ctx.clip = ctx.history.cancel();
        close();
    }
    ImGui::EndPopup();
}

}  // namespace vats
