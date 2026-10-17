// Viewport Avatar Toolset - View > Body > Resize Skeleton (spec 08 section 31, RS): the body an animation is made for,
// bigger or smaller as a whole, with the keys scaled to fit if asked.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/height_variant.h"
#include "vats/resize.h"

namespace vats {

double App::body_size() const { return body_resize(doc_.clip().export_settings); }

double App::actor_size(int i) const {
    const Project& p = doc_.project;
    return i == p.active || i < 0 || i >= int(p.actors.size()) ? body_size() : body_resize(actor_clip(p, i).export_settings);
}

const Shape* App::worn_shape() const {
    const Shape* w = swap_shown() ? nullptr : host_.body_shape();
    const double s = actor_size(0);
    if (!w || !host_resizes_ || s == 1) return w;
    // RS-7: in place, like the host's own shape, so a pointer kept for the worn body stays the worn body. Scaled about
    // the worn avatar's ground: its soles stand pelvis-to-foot (as SL measures it) below the pelvis, which the editor
    // keeps at SL's rest height, so legs longer than SL's put them below 0.
    if (s != worn_resized_s_ || w->scale != worn_resized_src_.scale || w->offset != worn_resized_src_.offset ||
        w->axes != worn_resized_src_.axes || w->tails != worn_resized_src_.tails) {
        const double ground = sl_body_size(skel_).pelvis_to_foot - sl_body_size(skel_, nullptr, w).pelvis_to_foot;
        worn_resized_ = resized_shape(skel_, w, s, ground);
        worn_resized_src_ = *w;
        worn_resized_s_ = s;
    }
    return &worn_resized_;
}

void App::update_avatar_resize() {
    const Shape* w = swap_shown() ? nullptr : host_.body_shape();
    const double s = w ? actor_size(0) : 1;
    host_resizes_ = true;  // so worn_shape() makes the resized shape the lift is read from
    Vec3 lift;
    if (s != 1)
        if (const Shape* r = worn_shape(); r && !r->offset.empty())
            lift = r->offset[0] - (w->offset.empty() ? Vec3{} : w->offset[0]);
    host_can_resize_ = host_.resize_avatar(s, lift);
    host_resizes_ = host_can_resize_ && s != 1;
}

bool App::any_resized() const {
    if (body_size() != 1) return true;
    for (int i = 0; i < int(doc_.project.actors.size()); ++i)
        if (actor_size(i) != 1) return true;
    return false;
}

// Height variants (08 HV) are SL shapes at 100%, baked from keys that fit the actor's own size: none while a skeleton is
// resized, so a resized actor's hip moves never play on a 1.75 m body.
std::vector<double> App::variant_heights(const Json& ex) const { return any_resized() ? std::vector<double>{} : export_heights(ex); }

const Shape* App::resized(const Shape* base, double s) const {
    if (s == 1) return base;
    for (const ResizedEntry& e : resized_)
        if (e.base == base && e.s == s &&
            (!base || (e.base_copy.scale == base->scale && e.base_copy.offset == base->offset &&
                       e.base_copy.tails == base->tails && e.base_copy.axes == base->axes)))
            return &e.shape;
    // ponytail: entries are never dropped, so every pointer handed out stays valid and no cache keyed by one is fooled
    // by a new shape at an old address. A few per body and size; a body rebuilt while resized adds one each time.
    resized_.push_back({base, s, base ? *base : Shape{}, resized_shape(skel_, base, s)});
    return &resized_.back().shape;
}

bool App::cli_resize(const std::string& percent, bool keys_off) {
    char* end = nullptr;
    const double pct = std::strtod(percent.c_str(), &end);
    if (end == percent.c_str() || !(pct >= kResizeMin * 100 && pct <= kResizeMax * 100)) return false;
    const bool saved = settings_.resize_keys;
    if (keys_off) settings_.resize_keys = false;
    apply_resize(pct / 100);
    settings_.resize_keys = saved;
    return true;
}

void App::apply_resize(double size) {
    size = std::clamp(size, kResizeMin, kResizeMax);
    const double from = body_size();
    if (std::fabs(size - from) < 1e-9) return;
    ResizedKeys n;
    const bool keys = settings_.resize_keys;
    edit("Resize Skeleton", [&](Clip& c) {
        if (!c.export_settings.is_object()) c.export_settings = Json::object();
        c.export_settings.set("size", size);
        if (keys) n = scale_position_keys(skel_, c, size / from);
    });
    // The camera keeps the same framing of the body, unless the world draws it at its own size (RS-7).
    if (!(host_.world_view() && !host_can_resize_ && worn_shape() && !editing_other()))
        cam_glide_.apply_input(camera_, [&](Camera& c) {
            c.target = c.target * (size / from);
            c.distance *= size / from;
        });
    char buf[200];
    if (keys)
        std::snprintf(buf, sizeof buf, "Resized the skeleton to %.0f%%; scaled %d position curves and %d pins to match",
                      size * 100, n.channels, n.pins);
    else
        std::snprintf(buf, sizeof buf, "Resized the skeleton to %.0f%%; the keys are as they were", size * 100);
    status(buf);
}

void App::draw_resize_window() {
    if (!show_resize_) {
        resize_window_open_ = false;
        return;
    }
    if (!resize_window_open_) resize_pending_ = body_size();  // opened: start from the clip's size
    resize_window_open_ = true;
    place_tool_window("Resize Skeleton", 26, 17);
    if (!ImGui::Begin("Resize Skeleton", &show_resize_)) return ImGui::End();
    help_button("resize-skeleton");
    ImGui::TextWrapped("Makes the body this animation is for bigger or smaller as a whole: every bone, the body and its "
                       "collision volumes. The feet stay on the ground.");
    ImGui::Spacing();

    const float label_w = ImGui::GetFontSize() * 5, field_w = ImGui::GetFontSize() * 6;
    float pct = float(resize_pending_ * 100);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::DragFloat("##size", &pct, 1.0f, float(kResizeMin * 100), float(kResizeMax * 100), "%.0f%%",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
        resize_pending_ = pct / 100.0;
    ImGui::SetItemTooltip("Drag, or double-click to type. 100%% is the body as it is; 200%% twice as big.");
    ImGui::SameLine();
    if (ImGui::Button("100%")) resize_pending_ = 1;

    // The height the bake shape reads in SL's shape editor, times the size; typing one sets the size from it.
    const double base_h = avatar_height(skel_, export_shape_base());
    if (base_h > 0.05) {
        float h = float(base_h * resize_pending_);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Height");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::DragFloat("##height", &h, 0.01f, float(base_h * kResizeMin), float(base_h * kResizeMax), "%.2f m",
                             ImGuiSliderFlags_AlwaysClamp))
            resize_pending_ = std::clamp(h / base_h, kResizeMin, kResizeMax);
        ImGui::SetItemTooltip("Floor to the top of the head, as SL's shape editor reads it (%.2f m at 100%%). Type the "
                              "height you want and the size follows.",
                              base_h);
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Keep feet on the ground and scale the keys", &settings_.resize_keys)) save_settings();
    ImGui::SetItemTooltip("On: the hip's moves, the IK targets and the held positions (Hold in World, Bind to a bone) "
                          "grow or shrink with the body, so a crouch, a sit or a jump keeps its depth and the feet stay on "
                          "the ground. Rotations are kept as they are.\nOff: only the body changes size; the hip's moves "
                          "keep their metres, so a crouch is shallower on a bigger body and goes through the floor on a "
                          "smaller one.\nOther bones' position keys always grow with the body.");
    if (worn_shape() && !editing_other()) {
        ImGui::Spacing();
        hint(host_can_resize_
                 ? "Your avatar is drawn at this size on your screen only: nothing is sent, and others see it as it is."
                 : "Your avatar keeps its own size on screen here (as it plays in-world, the walk test, or a viewer that "
                   "can't resize it). Exports, Animation Check and the other actors use the new size.");
    }

    ImGui::Spacing();
    const bool same = std::fabs(resize_pending_ - body_size()) < 1e-9;
    ImGui::BeginDisabled(same);
    if (primary_button("Apply", "Resizes the skeleton, and the keys if ticked; one undo step", 0, icon::kScale))
        apply_resize(resize_pending_);
    ImGui::EndDisabled();
    if (!same) {
        ImGui::SameLine();
        ImGui::TextDisabled("now %.0f%%", body_size() * 100);
    }
    ImGui::End();
}

}  // namespace vats
