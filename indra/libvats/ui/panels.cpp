// Viewport Avatar Toolset - the Bones, Properties and Timeline panels and the status bar.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui_internal.h"
#include "vats/anim_file.h"
#include "vats/edit.h"
#include "theme.h"

namespace vats {
namespace {

const char* const kHandPoses[] = {"Spread",       "Relaxed",     "Point",       "Fist",        "Relaxed Left",
                                  "Point Left",   "Fist Left",   "Relaxed Right", "Point Right", "Fist Right",
                                  "Salute Right", "Typing",      "Peace Right", "Palm Right"};
const char* const kEmotes[] = {"(none)",          "express_afraid",     "express_anger",      "express_bored",
                               "express_cry",     "express_disdain",    "express_embarrased", "express_frown",
                               "express_kiss",    "express_laugh",      "express_open_mouth", "express_repulsed",
                               "express_sad",     "express_shrug",      "express_smile",      "express_surprise",
                               "express_tongue_out", "express_toothsmile", "express_wink",     "express_worry"};
const char* const kCategories[] = {"Body", "Hands", "Face", "Wings", "Tail", "Hind Limbs", "Groin", "Attachment Points",
                                   "Collision Volumes"};

// A collapsible section header in the neutral frame colour (the accent is kept for selections).
bool section(const char* name, bool open_by_default = true) {
    return section_header(name, open_by_default);
}

bool contains_nocase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != hay.end();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Bones

void App::draw_bones_panel() {
    if (!ImGui::Begin("Bones")) return ImGui::End();
    ImGui::SetNextItemWidth(-1);
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", bone_filter_.c_str());
    if (ImGui::InputTextWithHint("##filter", "Filter bones...", buf, sizeof buf)) bone_filter_ = buf;

    // Select buttons (spec 06 4.1 item 3).
    const struct { const char *label, *id, *tip; } buttons[] = {
        {"Select All", "select_all", "Select every visible bone"},
        {"Keyed on Frame", "select_keyed_frame", "Select the bones with a key on the current frame"},
        {"All Keyed", "select_all_keyed", "Select every bone with a key anywhere in the clip"}};
    // Widths follow the labels so the longest one is not the one cut short.
    float text_w[3], total = 0;
    for (int b = 0; b < 3; ++b) total += text_w[b] = ImGui::CalcTextSize(buttons[b].label).x;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float room = ImGui::GetContentRegionAvail().x - 2 * st.ItemSpacing.x;
    // Too narrow for all three at full label width: flow them onto a second row instead of cutting labels.
    const bool fits = room >= total + 2 * st.FramePadding.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    float line = 0;  // width used on the current row
    for (int b = 0; b < 3; ++b) {
        const float w = fits ? room * text_w[b] / total : text_w[b] + 2 * st.FramePadding.x;
        if (b && (fits || line + st.ItemSpacing.x + w <= avail)) {
            ImGui::SameLine();
            line += st.ItemSpacing.x + w;
        } else {
            line = w;
        }
        if (ImGui::Button(buttons[b].label, ImVec2(w, 0))) run_action(buttons[b].id);
        std::string k = key_hint(buttons[b].id);
        ImGui::SetItemTooltip("%s%s", buttons[b].tip, k.empty() ? "" : (" (" + k + ")").c_str());
    }

    if (section("Show", false)) {
        for (int c = 0; c < 8; ++c) ImGui::Checkbox(kCategories[c], &show_category_[c]);
        ImGui::Checkbox("Collision Volumes", &show_volumes_);
    }
    ImGui::Separator();
    ImGui::BeginChild("tree");
    const Clip& clip = doc_.clip();
    const std::string filter = bone_filter_;
    const auto leaf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

    // Props hang under their bone or attachment point, or under World when the parent is unknown.
    std::vector<int> prop_parent(clip.props.size());
    std::vector<char> has_prop(skel_.size(), 0);
    bool world_props = false;
    for (size_t k = 0; k < clip.props.size(); ++k) {
        const Prop& p = clip.props[k];
        int at = skel_.find(!p.point.empty() ? p.point : p.bone);
        prop_parent[k] = at;
        if (at >= 0) has_prop[at] = 1;
        else world_props = true;
    }

    // A row is shown when it or any descendant passes the filter and category visibility.
    std::vector<char> shown(skel_.size(), 0);
    for (int i = skel_.size() - 1; i >= 0; --i) {
        bool self = node_visible(i) && contains_nocase(skel_[i].name, filter);
        bool kids = false;
        for (int c : skel_[i].children) kids = kids || shown[c];
        shown[i] = self || kids;
    }

    // A bone selected in the view opens its ancestors and scrolls into sight once (UI-23).
    const int prim = primary();
    std::vector<char> reveal(skel_.size(), 0);
    const bool reveal_now = prim != bones_seen_primary_ && !bones_clicked_ && prim >= 0;
    if (reveal_now)
        for (int a = skel_[prim].parent; a >= 0; a = skel_[a].parent) reveal[a] = 1;
    bones_seen_primary_ = prim;
    bones_clicked_ = false;

    // Drops: a library prop attaches, a scene prop is re-parented (node -1 = World).
    auto drop_target = [&](int node) {
        if (!ImGui::BeginDragDropTarget()) return;
        const std::string target = node >= 0 ? skel_[node].name : "";
        const bool point = node >= 0 && skel_[node].attachment;
        const std::string b = point ? "" : target, pt = point ? target : "";
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("VATS_PROP"))
            if (const PropLibraryItem* it = find_prop_item(static_cast<const char*>(pl->Data)))
                add_library_prop(*it, b, pt, !it->prop.rigged && b == it->prop.bone && pt == it->prop.point);
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("VATS_SCENE_PROP")) {
            int k = *static_cast<const int*>(pl->Data);
            if (k >= 0 && k < int(doc_.clip().props.size()))
                edit("Prop Parent", [&](Clip& c) {
                    Prop& q = c.props[k];
                    q.bone = b, q.point = pt, q.pos = {}, q.rot = {};
                });
        }
        ImGui::EndDragDropTarget();
    };
    auto prop_rows = [&](int parent) {
        for (int k = 0; k < int(clip.props.size()); ++k) {
            const Prop& p = clip.props[k];
            if (prop_parent[k] != parent || !contains_nocase(p.name, filter)) continue;
            ImGui::PushID(k);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.8f, 1, 1));
            ImGui::TreeNodeEx("##prop", leaf | (selected_prop_ == k ? ImGuiTreeNodeFlags_Selected : 0), "%s%s",
                              p.name.c_str(), p.rigged ? " (rigged)" : "");
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) {
                clear_selection();
                selected_prop_ = k;
                bones_clicked_ = true;
                status(p.rigged ? p.name + " (rigged: follows the avatar)"
                                : "Drag " + p.name + " onto a bone, attachment point or World to parent it");
            }
            if (!p.rigged && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("VATS_SCENE_PROP", &k, sizeof k);
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    };

    // Per-frame colour marks (spec 06 4.1 item 5), in priority order.
    auto row_colour = [&](int i, bool& pinned) -> ImU32 {
        const Node& n = skel_[i];
        pinned = pin_at(clip, *rig_, i, frame_) >= 0;
        if (pinned) return IM_COL32(140, 217, 255, 255);
        if (has_key_at(clip, n.name, frame_)) return IM_COL32(255, 217, 77, 255);
        if (clip.curves.count(n.name)) return n.attachment && !n.volume ? IM_COL32(140, 255, 128, 255) : IM_COL32(242, 191, 128, 255);
        if (n.attachment && !n.volume) return IM_COL32(115, 209, 115, 255);
        return 0;
    };

    // World.
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("World", world_props ? ImGuiTreeNodeFlags_SpanAvailWidth : leaf)) {
        drop_target(-1);
        prop_rows(-1);
        if (world_props) ImGui::TreePop();
    } else {
        drop_target(-1);
    }

    // IK Controls: every limb with IK data, "(FK)" where its IK is off at this frame.
    bool any_ik = false;
    for (const LimbState& s : limb_states_) any_ik = any_ik || s.uses_ik;
    if (any_ik && ImGui::TreeNodeEx("IK Controls", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        const std::string toggle = key_hint("ik_toggle");
        for (int l = 0; l < int(limb_states_.size()) && l < int(rig_->limbs().size()); ++l) {
            if (!limb_states_[l].uses_ik) continue;
            const LimbInfo& lim = rig_->limbs()[l];
            const bool on = limb_states_[l].ik_on;
            for (bool pole : {false, true}) {
                if (pole && lim.spine) continue;
                HandleRef h{l, pole};
                bool sel = std::find(handles_.begin(), handles_.end(), h) != handles_.end();
                std::string name = lim.label + (pole ? " Pole" : " IK") + (on ? "" : " (FK)");
                ImGui::PushID(l * 2 + pole);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 0.85f, 0.35f, 1) : ImVec4(1, 1, 1, 0.45f));
                ImGui::TreeNodeEx("##ik", leaf | (sel ? ImGuiTreeNodeFlags_Selected : 0), "%s", name.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemClicked()) select_handle(h, ImGui::GetIO().KeyShift), bones_clicked_ = true;
                ImGui::SetItemTooltip("%s%s", on ? "IK is on at this frame: the limb follows this control."
                                                 : "IK is off at this frame: the limb is posed by its bones (FK).",
                                      toggle.empty() ? "" : ("\nSwitch IK/FK with " + toggle + ".").c_str());
                ImGui::PopID();
            }
        }
        ImGui::TreePop();
    }

    // Skeleton.
    auto row = [&](auto& self_fn, int i) -> void {
        if (!shown[i]) return;
        const Node& n = skel_[i];
        bool has_kids = has_prop[i] != 0;
        for (int c : n.children) has_kids = has_kids || shown[c];
        bool selected = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (!has_kids) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (n.category == Category::Body || !filter.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (reveal[i]) ImGui::SetNextItemOpen(true);
        bool pinned = false;
        ImU32 colour = row_colour(i, pinned);
        if (colour) ImGui::PushStyleColor(ImGuiCol_Text, colour);
        const bool scratch = std::binary_search(scratch_marks_.begin(), scratch_marks_.end(), n.name);  // PT-2
        bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(intptr_t(i)), flags, "%s%s%s", n.name.c_str(),
                                      pinned ? " [pinned]" : "", scratch ? " (scratch)" : "");
        if (colour) ImGui::PopStyleColor();
        if (reveal_now && i == prim) ImGui::SetScrollHereY(0.5f);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(i, ImGui::GetIO().KeyShift), bones_clicked_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip("%s\n%s%s", n.name.c_str(), kCategories[int(n.category)],
                                  n.attachment ? "" : (n.base ? ", classic" : ", Bento"));
        drop_target(i);
        if (has_kids && open) {
            prop_rows(i);
            for (int c : n.children) self_fn(self_fn, c);
            ImGui::TreePop();
        }
    };
    for (int i = 0; i < skel_.size(); ++i)
        if (skel_[i].parent < 0) row(row, i);
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Properties

void App::draw_properties_panel() {
    if (!ImGui::Begin("Properties")) return ImGui::End();
    Clip& clip = doc_.clip();
    const float label_w = ImGui::GetFontSize() * 5.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    // Field edits merge into one undo step: opened when the field activates, committed on release.
    auto track_edit = [&](const char* step) {
        if (ImGui::IsItemActivated()) doc_.history.begin(clip);
        if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, clip)) mark_dirty();
    };

    int p = primary();
    if (selected_prop_ >= 0 && section("Prop")) draw_prop_section();
    if (selected_prop_ < 0 && section("Bone")) {
        if (p < 0) {
            hint("Select a bone in the view or the Bones list.");
        } else {
            const Node& n = skel_[p];
            ImGui::TextUnformatted(n.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", kCategories[int(n.category)], selection_.size() > 1 ? "  (+ more selected)" : "");
            // A pinned point is edited through its pin offset (AM-86): the pin would override its own keys.
            int pin = pin_at(clip, *rig_, p, frame_);
            const std::string track = pin >= 0 ? "pin:" + n.name : n.name;
            if (pin >= 0) {
                const Pin& pn = clip.pins[pin];
                ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "Pinned %s from frame %d%s", pn.target.empty() ? "in the world" : ("to " + pn.target).c_str(),
                                   pn.from, pn.to < 0 ? "" : (" to " + std::to_string(pn.to)).c_str());
                ImGui::TextDisabled("These values offset the pin.");
            }
            Vec3 e = curve_euler(clip, track, frame_);
            float ev[3] = {float(e.x), float(e.y), float(e.z)};
            label("Rotation");
            if (ImGui::DragFloat3("##rot", ev, 0.25f, 0, 0, "%.1f°")) key_euler(clip, track, frame_, {ev[0], ev[1], ev[2]});
            track_edit("Rotate");
            if (pin >= 0) {
                Vec3 o = curve_offset(clip, track, frame_);
                float ov[3] = {float(o.x), float(o.y), float(o.z)};
                label("Offset (m)");
                if (ImGui::DragFloat3("##pinpos", ov, 0.001f, 0, 0, "%.3f")) key_offset(clip, track, frame_, {ov[0], ov[1], ov[2]});
                track_edit("Move");
            }
            bool has_pos = pin < 0 && (n.attachment || clip.has_channels(n.name, kPosChannels));
            if (has_pos) {
                Vec3 o = curve_offset(clip, n.name, frame_);
                float ov[3] = {float(o.x), float(o.y), float(o.z)};
                label("Offset (m)");
                if (ImGui::DragFloat3("##pos", ov, 0.001f, 0, 0, "%.3f")) key_offset(clip, n.name, frame_, {ov[0], ov[1], ov[2]});
                track_edit("Move");
            } else if (pin < 0 && ImGui::SmallButton("Animate Position")) {
                edit("Add Position Keys", [&](Clip& c) { key_offset(c, n.name, frame_, {}); });
            }
            bool keyed_here = has_key_at(clip, n.name, frame_);
            ImGui::TextDisabled(keyed_here ? "Keyed at this frame" : "Not keyed at this frame");
            // IO-7: the bone's own priority in the exported file; "Clip" follows the clip's priority.
            auto jp = clip.joint_priority.find(n.name);
            const int own = jp == clip.joint_priority.end() ? -1 : jp->second;
            label("Priority");
            const std::string shown = own < 0 ? "Clip (" + std::to_string(clip.priority) + ")" : std::to_string(own);
            if (ImGui::BeginCombo("##jprio", shown.c_str())) {
                const std::string name = n.name;
                if (ImGui::Selectable(("Clip (" + std::to_string(clip.priority) + ")").c_str(), own < 0))
                    edit("Bone Priority", [&](Clip& c) { c.joint_priority.erase(name); });
                for (int pr = 0; pr <= 6; ++pr)
                    if (ImGui::Selectable((std::to_string(pr) + (pr > 4 ? "  (overrides most animations)" : "")).c_str(), own == pr))
                        edit("Bone Priority", [&](Clip& c) { c.joint_priority[name] = pr; });
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Priority of this bone alone in the exported .anim. Higher wins over other animations.");
        }
    }

    if (section("Animation")) {
        auto int_field = [&](const char* name, const char* id, int& v, int lo, int hi, const char* step) {
            label(name);
            int t = v;
            if (ImGui::DragInt(id, &t, 0.2f, lo, hi)) v = std::clamp(t, lo, hi);
            track_edit(step);
        };
        // Frame rate: asks on commit whether keys keep their frames or their timing (decision 7).
        label("Frame rate");
        if (!fps_edit_active_ && !ImGui::IsPopupOpen("Change Frame Rate")) fps_edit_ = clip.fps;  // keep the new rate while it is asked about
        ImGui::DragInt("##fps", &fps_edit_, 0.2f, 1, 120);
        fps_edit_active_ = ImGui::IsItemActive();
        fps_edit_ = std::clamp(fps_edit_, 1, 120);
        if (ImGui::IsItemDeactivatedAfterEdit() && fps_edit_ != clip.fps) ImGui::OpenPopup("Change Frame Rate");
        if (ImGui::BeginPopupModal("Change Frame Rate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Change the frame rate from %d to %d fps.", clip.fps, fps_edit_);
            const int to = fps_edit_, from = clip.fps;
            if (ImGui::Button("Keep Frame Numbers")) {
                edit("Frame Rate", [&](Clip& c) { c.fps = to; });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("Keys stay on their frames; the clip plays faster or slower");
            ImGui::SameLine();
            if (ImGui::Button("Keep Timing")) {
                if (multi_actor())  // every actor moves its keys (GR-3), as one undo step
                    scene_edit("Frame Rate", [to](Project& pr) {
                        for (int k = 0; k < int(pr.actors.size()); ++k) retime_clip(actor_clip(pr, k), to);
                    });
                else
                    edit("Frame Rate", [&](Clip& c) { retime_clip(c, to); });
                set_frame(std::round(frame_ * to / from));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("Keys move so everything happens at the same second");
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        int_field("Last frame", "##end", clip.end_frame, 0, 3600, "Length");
        clip.loop_out = std::min(clip.loop_out, clip.end_frame);
        clip.loop_in = std::min(clip.loop_in, clip.loop_out);
        double seconds = std::max(clip.end_frame, 1) / double(clip.fps);
        ImGui::SetCursorPosX(label_w);
        if (seconds > 60)
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%.2f s: over SL's 60 s limit", seconds);
        else
            ImGui::TextDisabled("%.2f seconds", seconds);

        label("Loop");
        bool loop = clip.loop;
        if (ImGui::Checkbox("##loop", &loop))
            edit("Loop", [&](Clip& c) {
                c.loop = loop;
                if (loop && c.loop_out == 0) c.loop_out = c.end_frame;
            });
        if (clip.loop) {
            int_field("Loop in", "##lin", clip.loop_in, 0, clip.loop_out, "Loop In");
            int_field("Loop out", "##lout", clip.loop_out, clip.loop_in, clip.end_frame, "Loop Out");
        }
        int_field("Priority", "##prio", clip.priority, 0, 6, "Priority");
        if (clip.priority > 4) {
            ImGui::SetCursorPosX(label_w);
            ImGui::TextColored(ImVec4(1, 0.75f, 0.4f, 1), "Some viewers treat priority above 4 as 4");
        }
        auto ease = [&](const char* name, const char* id, double& v, const char* step) {
            label(name);
            float f = float(v);
            if (ImGui::DragFloat(id, &f, 0.05f, 0, 10, "%.2f s")) v = std::clamp(std::round(f / 0.05) * 0.05, 0.0, 10.0);  // AM-1
            track_edit(step);
        };
        ease("Ease in", "##ein", clip.ease_in, "Ease In");
        ease("Ease out", "##eout", clip.ease_out, "Ease Out");

        label("Hand pose");
        int hp = std::clamp(clip.hand_pose, 0, 13);
        if (ImGui::Combo("##hand", &hp, kHandPoses, 14)) edit("Hand Pose", [&](Clip& c) { c.hand_pose = hp; });
        label("Expression");
        int em = 0;
        for (int i = 1; i < 20; ++i)
            if (clip.emote == kEmotes[i]) em = i;
        if (ImGui::Combo("##emote", &em, kEmotes, 20))
            edit("Expression", [&](Clip& c) { c.emote = em ? kEmotes[em] : ""; });
    }
    if (section("Export")) draw_export_section();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Timeline and transport

void App::draw_timeline_panel() {
    if (!ImGui::Begin("Timeline")) return ImGui::End();
    Clip& clip = doc_.clip();

    // Transport row.
    // Tooltips name the shortcut of the active control preset (TG-31, UI-26).
    auto tip = [&](const char* what, const char* id) {
        std::string k = key_hint(id);
        return k.empty() ? std::string(what) : std::string(what) + " (" + k + ")";
    };
    // Tool buttons: an icon and the name; the name and shortcut in the tooltip (UI-26). Icon only while the panel
    // is narrower than the row last took with names, so the tween controls stay on screen.
    const bool compact = timeline_row_w_ > 0 && ImGui::GetContentRegionAvail().x < timeline_row_w_;
    auto tool_button = [&](const char* icon, const char* label, bool on, const std::string& tip_text) {
        bool pressed = compact ? icon_button(label, icon, tip_text, on) : icon_label_button(icon, label, tip_text, on);
        ImGui::SameLine();
        return pressed;
    };
    // Transport: icon only, the name and shortcut in the tooltip (UI-26).
    auto transport = [&](const char* id, const char* icon, bool on, const std::string& tip_text) {
        bool pressed = icon_button(id, icon, tip_text, on);
        ImGui::SameLine();
        return pressed;
    };
    if (transport("start", icon::kStart, false, tip("Go to start", "start"))) run_action("start");
    if (transport("prev_key", icon::kPrevKey, false, tip("Previous key", "prev_key"))) run_action("prev_key");
    if (transport("play", playing_ ? icon::kPause : icon::kPlay, playing_, tip("Play / pause", "play"))) run_action("play");
    if (transport("next_key", icon::kNextKey, false, tip("Next key", "next_key"))) run_action("next_key");
    if (transport("end", icon::kEnd, false, tip("Go to end", "end"))) run_action("end");
    if (transport("loop", icon::kLoop, clip.loop, "Loop: repeat Loop In to Loop Out in SL (the Loop box in Properties)"))
        edit("Loop", [loop = !clip.loop](Clip& c) {
            c.loop = loop;
            if (loop && c.loop_out == 0) c.loop_out = c.end_frame;
        });
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    int f = int(std::floor(frame_ + 1e-9));
    if (ImGui::DragInt("##frame", &f, 0.2f, 0, clip.end_frame, "Frame %d")) set_frame(f);
    ImGui::SameLine();
    ImGui::TextDisabled("/ %d", clip.end_frame);
    ImGui::SameLine(0, 24);
    if (tool_button(icon::kSelect, "Select", tool_ == Tool::Select, tip("Select tool", "tool_select") + ": click bones without a gizmo"))
        tool_ = Tool::Select;
    if (tool_button(icon::kMove, "Move", tool_ == Tool::Move, tip("Move tool", "tool_move"))) tool_ = Tool::Move;
    if (tool_button(icon::kRotate, "Rotate", tool_ == Tool::Rotate, tip("Rotate tool", "tool_rotate"))) tool_ = Tool::Rotate;
    if (tool_button(icon::kScale, "Scale", tool_ == Tool::Scale, tip("Scale tool", "tool_scale") + " (static props only)"))
        tool_ = Tool::Scale;
    ImGui::SameLine(0, 16);
    const bool local = orientation_ == Orientation::Local, world = orientation_ == Orientation::World;
    if (tool_button(local ? icon::kLocal : world ? icon::kWorld : icon::kGimbal,
                    local ? "Local###orient" : world ? "World###orient" : "Gimbal###orient", false,
                    tip("Gizmo axes: Local, World or Gimbal", "orientation")))
        run_action("orientation");
    if (tool_button(icon::kIkFk, "IK / FK", false, tip("IK / FK", "ik_toggle") + ": switch the selected limb between IK and FK, matched"))
        run_action("ik_toggle");
    if (tool_button(icon::kFlipTime, "Mirror", mirror_live_, "Mirror: posing a bone or IK control also keys its other side"))
        mirror_live_ = !mirror_live_;  // PT-1
    ImGui::SameLine(0, 16);
    if (tool_button(icon::kSetKey, "Set Key", false, tip("Set Key", "key") + ": key the selected bones, pins and IK controls"))
        run_action("key");
    draw_tween_controls();  // spec 08 TW-1, TW-2
    if (!compact) timeline_row_w_ = tween_row_end_ - ImGui::GetWindowPos().x - ImGui::GetStyle().WindowPadding.x;
    ImGui::NewLine();

    // Timeline strip.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x, height = std::max(ImGui::GetContentRegionAvail().y, 48.f);
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    if (ImGui::BeginPopupContextItem("##timeline_menu")) {  // time editing and the audio track (08 TE, AU)
        draw_time_menu_items();
        ImGui::SeparatorText("Audio");
        draw_audio_menu_items();
        ImGui::EndPopup();
    }
    const float pad = 12, ruler = 20;
    const float x0 = origin.x + pad, x1 = origin.x + width - pad;
    const int last = std::max(clip.end_frame, 1);
    auto x_of = [&](double fr) { return float(x0 + (x1 - x0) * fr / last); };
    auto frame_at = [&](float x) { return std::clamp(std::round(double(x - x0) / (x1 - x0) * last), 0.0, double(last)); };

    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), timeline_background(), 4);
    // Ease in / out ramps (a neutral light that reads on every theme) and the loop region.
    constexpr ImU32 kEase = IM_COL32(255, 255, 255, 16);
    float ein = float(clip.ease_in * clip.fps), eout = float(clip.ease_out * clip.fps);
    dl->AddRectFilledMultiColor(ImVec2(x_of(0), origin.y + ruler), ImVec2(x_of(std::min<double>(ein, last)), origin.y + height),
                                kEase, 0, 0, kEase);
    dl->AddRectFilledMultiColor(ImVec2(x_of(std::max<double>(last - eout, 0)), origin.y + ruler),
                                ImVec2(x_of(last), origin.y + height), 0, kEase, kEase, 0);
    if (clip.loop)
        dl->AddRectFilled(ImVec2(x_of(clip.loop_in), origin.y + ruler), ImVec2(x_of(clip.loop_out), origin.y + height),
                          ui::kLoop);

    draw_audio_lane(dl, x0, x1, origin.y + ruler, origin.y + height, last);  // waveform and beats (AU-2)

    // Ruler (TG-2): labels at least 50 px apart; unlabelled frames get a minor tick once a frame is 4 px wide.
    const TimelineColours& tc = timeline_colours();
    const float px_per_frame = (x1 - x0) / last;
    int step = 600;
    for (int s : {1, 2, 5, 10, 15, 30, 60, 120, 300, 600})
        if (px_per_frame * s >= 50) {
            step = s;
            break;
        }
    if (px_per_frame >= 4)
        for (int fr = 0; fr <= last; ++fr)
            if (fr % step) dl->AddLine(ImVec2(x_of(fr), origin.y + ruler - 4), ImVec2(x_of(fr), origin.y + ruler), tc.grid_major);
    for (int fr = 0; fr <= last; fr += step) {
        float x = x_of(fr);
        dl->AddLine(ImVec2(x, origin.y + ruler - 6), ImVec2(x, origin.y + height), tc.grid_major);
        char t[16];
        std::snprintf(t, sizeof t, "%d", fr);
        dl->AddText(ImVec2(x + 3, origin.y + 3), tc.text_dim, t);
    }

    // Key ticks (TG-3, decision 6): a faint line for every keyed frame in the clip; diamonds for the whole
    // selection (bones, their pins, IK controls), the primary item's brighter and larger.
    float ky = origin.y + ruler + (height - ruler) * 0.5f;
    // Marks are gathered per pixel column: a dense clip has far more keys than pixels, and a mark per key
    // (hundreds of thousands with many bones selected) swamped both the CPU and the draw lists.
    const int cols = std::max(1, int(x1 - x0) + 1);
    std::vector<char> col(cols);
    auto mark = [&](double fr) {
        int c = int(std::lround(x_of(fr) - x0));
        if (c >= 0 && c < cols) col[c] = 1;
    };
    auto each_column = [&](auto&& draw) {
        for (int c = 0; c < cols; ++c)
            if (col[c]) draw(x0 + float(c));
        std::fill(col.begin(), col.end(), char(0));
    };
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track)
            for (auto& k : c.keys) mark(k.frame);
    each_column([&](float x) { dl->AddLine(ImVec2(x, ky - 10), ImVec2(x, ky + 10), IM_COL32(255, 255, 255, 40)); });
    auto diamond_at = [&](float x, float y, ImU32 c, float r, bool outline) {
        dl->AddQuadFilled(ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y), c);
        if (outline) dl->AddQuad(ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y), IM_COL32(20, 22, 26, 220));
    };
    std::vector<std::string> primary_tracks;
    if (primary() >= 0) primary_tracks = {skel_[primary()].name, "pin:" + skel_[primary()].name};
    else if (const HandleRef* h = primary_handle()) primary_tracks = {"ik." + rig_->limbs()[h->limb].name};
    auto mark_track = [&](const std::string& t) {
        auto it = clip.curves.find(t);
        if (it == clip.curves.end()) return;
        for (auto& [ch, c] : it->second)
            for (auto& k : c.keys) mark(k.frame);
    };
    for (const std::string& t : selected_tracks())
        if (std::find(primary_tracks.begin(), primary_tracks.end(), t) == primary_tracks.end()) mark_track(t);
    each_column([&](float x) { diamond_at(x, ky, ui::kKeyDim, 4, false); });
    for (const std::string& t : primary_tracks) mark_track(t);
    each_column([&](float x) { diamond_at(x, ky, ui::kKey, 6, true); });

    // Loop flags and ease edges, all draggable (decision 5, 05 section 5 items 1-2). Dragging a loop flag
    // turns loop on; Alt+drag inside the band moves both flags; the ease edges set seconds in 0.05 steps.
    static int dragging_handle = 0;  // 1 loop in, 2 loop out, 3 whole band, 4 ease in, 5 ease out
    static int band_press = 0, band_in = 0, band_out = 0;
    ImVec2 m = ImGui::GetIO().MousePos;
    const bool strip_hovered = ImGui::IsItemHovered();
    const float ytop = origin.y + ruler, ybot = origin.y + height;
    auto near = [&](float x, float ya, float yb) { return strip_hovered && std::fabs(m.x - x) < 7 && m.y >= ya && m.y <= yb; };
    auto flag = [&](int which, int fr) {
        float x = x_of(fr);
        bool hot = dragging_handle == which || near(x, ytop - 2, ytop + 12);
        ImU32 c = hot ? accent_colour() : clip.loop ? ui::kLoopHandle : (ui::kLoopHandle & 0x00FFFFFF) | 0x66000000;
        float d = which == 1 ? 9.f : -9.f;
        dl->AddTriangleFilled(ImVec2(x, ytop), ImVec2(x + d, ytop), ImVec2(x, ytop + 10), c);
        return hot;
    };
    auto ease_mark = [&](int which, double fr) {
        float x = x_of(fr);
        bool hot = dragging_handle == which || near(x, ybot - 12, ybot);
        dl->AddTriangleFilled(ImVec2(x, ybot - 9), ImVec2(x + 5, ybot - 1), ImVec2(x - 5, ybot - 1),
                              hot ? accent_colour() : IM_COL32(232, 168, 72, 190));
        return hot;
    };
    const bool over_in = flag(1, clip.loop_in), over_out = flag(2, clip.loop_out);
    draw_loop_seam_mark(dl, x_of(clip.loop_out), ytop + 12, strip_hovered);  // 08 LP-2
    const bool over_ein = ease_mark(4, std::min<double>(ein, last)), over_eout = ease_mark(5, std::max<double>(last - eout, 0));
    const bool over_band = clip.loop && strip_hovered && ImGui::GetIO().KeyAlt && m.y > ytop && m.x > x_of(clip.loop_in) &&
                           m.x < x_of(clip.loop_out);
    if (over_in || over_out || over_ein || over_eout || dragging_handle) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (!dragging_handle && strip_hovered) {
        if (over_in || over_out) ImGui::SetTooltip("Loop %s: drag to move (turns looping on)\nAlt+drag the band to move both",
                                                   over_in ? "in" : "out");
        else if (over_ein || over_eout) ImGui::SetTooltip("Ease %s: drag to set, %.2f s", over_ein ? "in" : "out",
                                                          over_ein ? clip.ease_in : clip.ease_out);
    }
    static bool picking_range = false;
    if (ImGui::IsItemActivated()) {
        dragging_handle = over_in ? 1 : over_out ? 2 : over_ein ? 4 : over_eout ? 5 : over_band ? 3 : 0;
        band_press = int(frame_at(m.x)), band_in = clip.loop_in, band_out = clip.loop_out;
        if (dragging_handle) doc_.history.begin(clip);
        picking_range = !dragging_handle && ImGui::GetIO().KeyShift;
        // Ctrl+drag slides the audio along the timeline (AU-2).
        dragging_audio_ = !dragging_handle && !picking_range && ImGui::GetIO().KeyCtrl && clip.audio;
        if (dragging_audio_) {
            doc_.history.begin(clip);
            audio_press_offset_ = clip.audio->offset, audio_press_frame_ = frame_at(m.x);
        }
        if (picking_range) range_a_ = range_b_ = int(snapped_frame(frame_at(m.x)));
        else if (!dragging_handle) range_a_ = range_b_ = -1;  // a plain click clears the range (TG-6)
    }
    if (ImGui::IsItemActive() && dragging_audio_ && clip.audio) {
        clip.audio->offset = audio_press_offset_ + (frame_at(m.x) - audio_press_frame_) / std::max(clip.fps, 1);
        ImGui::SetTooltip("Audio starts at %.2f s", clip.audio->offset);
    } else if (ImGui::IsItemActive()) {
        int fr = int(frame_at(m.x));
        auto seconds = [&](int frames) { return std::clamp(std::round(frames / double(clip.fps) / 0.05) * 0.05, 0.0, 10.0); };
        switch (dragging_handle) {
            case 1: clip.loop = true, clip.loop_in = std::min(fr, clip.loop_out); break;
            case 2: clip.loop = true, clip.loop_out = std::max(fr, clip.loop_in); break;
            case 3: {
                int d = std::clamp(fr - band_press, -band_in, last - band_out);
                clip.loop_in = band_in + d, clip.loop_out = band_out + d;
                break;
            }
            case 4: clip.ease_in = seconds(fr); break;
            case 5: clip.ease_out = seconds(last - fr); break;
            default:
                if (picking_range) range_b_ = int(snapped_frame(fr));
                else set_frame(snapped_frame(fr));  // scrub
        }
    }
    if (range_a_ >= 0 && range_a_ != range_b_) {
        float a = x_of(std::min(range_a_, range_b_)), b = x_of(std::max(range_a_, range_b_));
        dl->AddRectFilled(ImVec2(a, origin.y), ImVec2(b, origin.y + height), IM_COL32(255, 222, 70, 36));
        dl->AddRect(ImVec2(a, origin.y), ImVec2(b, origin.y + height), IM_COL32(255, 222, 70, 140));
    }
    if (ImGui::IsItemDeactivated() && dragging_audio_) {
        dragging_audio_ = false;
        if (doc_.history.is_open() && doc_.history.commit("Move Audio", clip)) mark_dirty();
    }
    if (ImGui::IsItemDeactivated() && dragging_handle) {
        static const char* const steps[] = {"", "Loop In", "Loop Out", "Move Loop", "Ease In", "Ease Out"};
        if (doc_.history.commit(steps[dragging_handle], clip)) mark_dirty();
        dragging_handle = 0;
    }

    // Playhead: whole frames while playback runs fractionally (TG-11).
    const double shown = std::floor(frame_ + 1e-9);
    float px = x_of(shown);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + height), ui::kPlayhead, 2);
    dl->AddRectFilled(ImVec2(px - 14, origin.y), ImVec2(px + 14, origin.y + 17), ui::kPlayhead, 3);
    char t[16];
    std::snprintf(t, sizeof t, "%d", int(shown));
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(px - ts.x / 2, origin.y + 1), IM_COL32(20, 22, 26, 255), t);
    ImGui::End();
}

GraphContext App::graph_context() {
    std::vector<std::string> items;
    for (int s : selection_) {
        const std::string& name = skel_[s].name;
        items.push_back(name);
        // A pinned point also shows its pin offset curves (TG-40).
        bool pinned = doc_.clip().curves.count("pin:" + name) > 0;
        for (auto& pin : doc_.clip().pins) pinned = pinned || (pin.joint == name && pin.active(frame_));
        if (pinned) items.push_back("pin:" + name);
    }
    // IK controls show the selected part only: target or pole (TG-41).
    for (auto& h : handles_) {
        std::string item = "ik." + rig_->limbs()[h.limb].name + (h.pole ? "#pole" : "#target");
        if (std::find(items.begin(), items.end(), item) == items.end()) items.push_back(item);
    }
    auto label = [this](const std::string& item) {
        if (item.rfind("pin:", 0) == 0) return item.substr(4) + " (pin)";
        if (item.rfind("ik.", 0) == 0) {
            const size_t hash = item.find('#');
            const std::string limb = item.substr(3, hash == std::string::npos ? std::string::npos : hash - 3);
            int l = rig_->find_limb(limb);
            return (l >= 0 ? rig_->limbs()[l].label : limb) + (item.ends_with("#pole") ? " Pole" : " IK");
        }
        return item;
    };
    GraphContext g{doc_.clip(), doc_.history, skel_, frame_, std::move(items), label, [this] { mark_dirty(); },
                   [this](const std::string& s) { status(s); }, rig_.get(), shape(), settings_.preset,
                   settings_.emulate_3_button, [this](const char* id) { return key_hint(id); }};
    return g;
}

void App::draw_graph_panel() {
    if (!show_graph_) return;
    if (ImGui::Begin("Graph", &show_graph_)) {
        GraphContext g = graph_context();
        graph_.draw(g);
        if (graph_.take_escape()) skip_shortcuts_ = true;  // Esc cancelled a graph drag; keep the selection
    }
    ImGui::End();
    if (!show_graph_) settings_.show_graph = false, save_settings();  // closed with its x: remembered like Ctrl+G
}

void App::draw_status_bar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float h = ImGui::GetFrameHeight();
    if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, h, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextUnformatted(status_.c_str());
            if (multi_actor()) {  // GR: whose animation the Bones list, timeline, graph and keys edit
                const std::string editing = "Editing " + doc_.project.actors[doc_.project.active].name;
                if (status_ != editing) ImGui::SameLine(0, 24), hint(editing.c_str());
            }
            if (mirror_live_) {  // PT-1, in the gizmo's tint
                ImGui::SameLine(0, 24);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMirrorTint), "Mirror on");
            }
            if (scratch_on_) ImGui::SameLine(0, 24), hint("Scratch Pose: nothing is keyed until Set Key");  // PT-2
            if (size_t n = selection_.size() + handles_.size(); n > 1) {  // TG-112
                ImGui::SameLine(0, 24);
                hint((std::to_string(n) + " selected: keys, copy/paste and the graph apply to all of them").c_str());
            }
            if (ui::Host::HostUi* h = host_.host_ui(); h && h->unread_notices() > 0) {  // spec 09 U4b: the unread badge
                ImGui::SameLine(0, 24);
                if (ImGui::SmallButton(("Notifications (" + std::to_string(h->unread_notices()) + ")").c_str())) h->toggle_notices();
            }
            draw_check_badge();
            const std::string hint = graph_.hovered() ? graph_nav_hint() : nav_hint();
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(hint.c_str()).x - 16);
            ImGui::TextDisabled("%s", hint.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// The host's pane (the viewer's conversations): an empty dockable window whose inner rectangle the host fills.
void App::draw_host_pane() {
    ui::Host::HostUi* h = host_.host_ui();
    if (!h) return;
    bool shown = false;
    ImVec2 lo, hi;
    if (show_host_pane_) {
        ImGui::SetNextWindowSize(window_size(30, 18), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(h->pane_title(), &show_host_pane_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            lo = ImGui::GetCursorScreenPos();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            hi = ImVec2(lo.x + avail.x, lo.y + avail.y);
            shown = avail.x > 1 && avail.y > 1;
        }
        ImGui::End();
    }
    h->place_pane(shown && show_host_pane_, lo, hi);
}

}  // namespace vats
