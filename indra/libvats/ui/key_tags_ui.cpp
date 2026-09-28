// Viewport Avatar Toolset - key tags and blocking mode: the marks, the timeline bar's Blocking toggle and the menus.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (KT-1..KT-4). The logic is in the core (key_tags.h).
#include "key_tags_ui.h"

#include <string>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "vats/key_tags.h"

namespace vats {

namespace {
constexpr KeyTag kTags[] = {KeyTag::Extreme, KeyTag::Breakdown, KeyTag::Hold};
const char* const kTagTips[] = {"Extreme: a key pose, the furthest point of a move",
                                "Breakdown: an in-between; keeps its share of the time when a neighbouring key moves",
                                "Hold: a held pose; a pair of Hold keys drifts 1\xC2\xB0 on Convert Blocking to Spline"};
}  // namespace

ImU32 key_tag_colour(KeyTag tag) {
    switch (tag) {
        case KeyTag::Extreme: return IM_COL32(236, 90, 76, 255);
        case KeyTag::Breakdown: return IM_COL32(92, 200, 190, 255);
        case KeyTag::Hold: return IM_COL32(176, 140, 255, 255);
        case KeyTag::None: break;
    }
    return IM_COL32(240, 196, 92, 255);
}

void draw_key_tag_mark(ImDrawList* dl, ImVec2 c, float r, KeyTag tag, ImU32 fill, bool outline) {
    const ImU32 rim = IM_COL32(20, 22, 26, 220);
    switch (tag) {
        case KeyTag::Extreme: {
            const float d = r * 1.25f;  // a diamond looks smaller than a box of the same height
            dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), fill);
            if (outline) dl->AddQuad(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), rim);
            break;
        }
        case KeyTag::Breakdown:
            dl->AddCircleFilled(c, r * 0.9f, fill);
            if (outline) dl->AddCircle(c, r * 0.9f, rim);
            break;
        case KeyTag::Hold:
        case KeyTag::None: {
            const float w = tag == KeyTag::Hold ? r * 1.6f : r, h = tag == KeyTag::Hold ? r * 0.7f : r;
            dl->AddRectFilled(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), fill);
            if (outline) dl->AddRect(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), rim);
            break;
        }
    }
}

// KT-2: after every recorded edit (mark_dirty), with Blocking on, the step's new keys become Stepped, in the same step.
void App::blocking_after_edit() {
    if (!doc_.history.take_recorded() || !blocking_) return;
    if (step_new_keys(doc_.clip(), doc_.history.last_before())) doc_.history.amend_last(doc_.clip());
}

void App::draw_blocking_button(bool compact) {
    const std::string tip = "Blocking: new keys are Stepped, so poses hold until the next key. Convert Blocking to "
                            "Spline (right-click the timeline) smooths them when the blocking is done";
    const bool pressed = compact ? icon_button("Blocking", icon::kBlocking, tip, blocking_)
                                 : icon_label_button(icon::kBlocking, "Blocking", tip, blocking_);
    if (pressed) {
        blocking_ = !blocking_;
        status(blocking_ ? "Blocking on: new keys are Stepped" : "Blocking off: new keys follow the key before them");
    }
    ImGui::SameLine();
}

void App::draw_key_tag_menu_items() {
    if (ImGui::MenuItem("Blocking", nullptr, blocking_)) blocking_ = !blocking_;
    ImGui::SetItemTooltip("New keys are Stepped while this is on");
    if (ImGui::MenuItem("Convert Blocking to Spline")) {
        int holds = 0;
        edit("Convert Blocking to Spline", [&](Clip& c) { holds = blocking_to_spline(c); });
        blocking_ = false;
        status("Every key is Auto now; " + std::to_string(holds) + " hold(s) got a 1\xC2\xB0 drift. Blocking is off");
    }
    ImGui::SetItemTooltip("Auto tangents on every key (IK switches stay stepped); each pair of Hold keys becomes a "
                          "moving hold that drifts 1\xC2\xB0 towards the next pose. Turns Blocking off");
    const bool none = selection_.empty() && handles_.empty();
    if (begin_menu_icon(icon::kTags, "Tag Keys Here", !none)) {
        auto tag_here = [&](KeyTag tag) {
            int n = 0;
            const std::vector<std::string> tracks = selected_tracks();
            edit(tag == KeyTag::None ? "Clear Key Tags" : std::string("Tag ") + key_tag_name(tag), [&](Clip& c) {
                n = tag_keys_at(c, tracks, frame_, tag);
            });
            status(n ? "Tagged " + std::to_string(n) + " key(s) at frame " + std::to_string(int(frame_))
                     : "No keys of the selected items at this frame to tag");
        };
        for (int t = 0; t < 3; ++t) {
            if (ImGui::MenuItem(key_tag_name(kTags[t]))) tag_here(kTags[t]);
            ImGui::SetItemTooltip("%s", kTagTips[t]);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("No Tag")) tag_here(KeyTag::None);
        ImGui::EndMenu();
    }
    if (none) ImGui::SetItemTooltip("Select a bone first: tags the keys of the selected items at the current frame");
}

void GraphEditor::draw_tag_menu_items(GraphContext& ctx) {
    auto tag = [&](KeyTag t) {
        if (selection_.empty()) return ctx.status("Select keys in the graph first");
        auto sel = selection_;
        int n = 0;
        edit(ctx, t == KeyTag::None ? "Clear Key Tags" : (std::string("Tag ") + key_tag_name(t)).c_str(),
             [&](Clip& c) { n = tag_keys(c, sel, t); });
        ctx.status(std::to_string(n) + " key(s) tagged");
    };
    for (int t = 0; t < 3; ++t) {
        if (menu_item_icon(nullptr, key_tag_name(kTags[t]))) tag(kTags[t]);
        ImGui::SetItemTooltip("%s", kTagTips[t]);
    }
    if (menu_item_icon(nullptr, "No Tag")) tag(KeyTag::None);
    ImGui::SetItemTooltip("Clear the selected keys' tags");
}

}  // namespace vats
