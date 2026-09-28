// Viewport Avatar Toolset - right-click menus in the 3D view: body parts, the whole avatar, points.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/04 VP-70..VP-74.
#include <algorithm>

#include "app.h"
#include "imgui_internal.h"
#include "vats/edit.h"
#include "vats/pose_presets.h"

namespace vats {
namespace {

// Library kind for a part's poses and clips ("" = no poses for this part).
std::string kind_of(PartKind k) {
    switch (k) {
        case PartKind::Arm: return "arm";
        case PartKind::Hand: return "hand";
        case PartKind::Leg: return "leg";
        case PartKind::Wing: return "wing";
        case PartKind::HindLeg: return "hindleg";
        case PartKind::Tail: return "tail";
        case PartKind::Head: return "head";
        case PartKind::Torso: return "pose";
        default: return "";
    }
}

std::string opposite(const std::string& side) { return side == "Left" ? "Right" : side == "Right" ? "Left" : ""; }

}  // namespace

std::vector<std::string> App::part_tracks(const BodyPart& part) const {
    std::vector<std::string> tracks;
    for (int b : with_hand(skel_, part).bones) tracks.push_back(skel_[b].name);  // an arm brings its hand (AM-90)
    return tracks;
}

void App::run_action(const char* id) {
    for (auto& [aid, a] : actions_)
        if (std::string_view(aid) == id) {
            const char* why = a.unavailable ? a.unavailable() : nullptr;
            why ? status(why) : a.run();
            return;
        }
}

void App::open_context_menu(int node) {
    context_node_ = node;
    ImGui::OpenPopup("##context");
}

void App::draw_context_menu() {
    if (!ImGui::BeginPopup("##context")) return;
    const int node = context_node_;
    Clip& clip = doc_.clip();
    const int f = int(std::round(frame_));
    auto header = [](const std::string& text) {
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::Separator();
    };
    auto poses_of_kind = [&](const std::string& kind, const std::string& side) {
        bool any = false;
        for (int i = 0; i < int(library_.items.size()); ++i) {
            const LibraryItem& it = library_.items[i];
            if (it.clip || it.kind != kind) continue;
            bool mirrored = !side.empty() && !it.side.empty() && it.side != side;
            std::string label = it.name + (mirrored ? "  (from the " + it.side + " side, mirrored)" : "");
            if (ImGui::MenuItem(label.c_str())) use_library_item(i, mirrored);
            any = true;
        }
        // Starter poses of this kind (hands are authored for the left side).
        bool header = false;
        for (const LibraryItem& it : builtin_poses(skel_)) {
            if (it.kind != kind) continue;
            if (!header && ImGui::BeginMenu("Starter poses")) header = true;
            else if (!header) break;
            bool mirrored = !side.empty() && !it.side.empty() && it.side != side;
            if (ImGui::MenuItem(it.name.c_str())) apply_library_item(it, mirrored);
            any = true;
        }
        if (header) ImGui::EndMenu();
        if (!any) ImGui::TextDisabled("  (none saved yet)");
    };
    auto clips_of_kind = [&](const std::string& kind, const std::string& side) {
        for (int i = 0; i < int(library_.items.size()); ++i) {
            const LibraryItem& it = library_.items[i];
            if (!it.clip || it.kind != kind) continue;
            bool mirrored = !side.empty() && !it.side.empty() && it.side != side;
            std::string label = "Paste Clip: " + it.name + " (" + std::to_string(int(it.length)) + " f)" +
                                (mirrored ? ", mirrored" : "");
            if (ImGui::MenuItem(label.c_str())) use_library_item(i, mirrored);
        }
    };
    auto hint = [&](const char* id) {  // the action's first shortcut in the current key preset
        for (auto& [aid, a] : actions_)
            if (aid == id && a.key) return key_label(a.key);
        return std::string();
    };
    auto select_part = [&](const BodyPart& part) {
        clear_selection();
        selection_ = with_hand(skel_, part).bones;
        for (auto& t : part.ik_tracks)
            if (clip.curves.count(t)) {
                int l = rig_->find_limb(t.substr(3));
                if (l < 0) continue;
                handles_.push_back({l, false});
                if (!rig_->limbs()[l].spine) handles_.push_back({l, true});
            }
        status("Selected " + std::to_string(selection_.size()) + " bones and " + std::to_string(handles_.size()) +
               " IK handles of " + part.label);
    };

    if (node < 0) {
        // Whole avatar (VP-73).
        header("Avatar");
        if (ImGui::MenuItem("Save Pose...")) {
            clear_selection();
            name_prompt_ = "Whole pose";
            name_action_ = NameAction::SavePose;
        }
        if (ImGui::BeginMenu("Poses")) {
            poses_of_kind("pose", "");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menu_item("select_all");
        menu_item("select_keyed_frame");  // one implementation with the Select menu (AM-132)
        menu_item("select_all_keyed");
        menu_item("key_all");
        if (ImGui::MenuItem("Copy Pose")) {
            clear_selection();
            run_action("copy");
        }
        if (!pose_clipboard_.entries.empty() && ImGui::MenuItem("Paste Pose")) {
            clear_selection();
            run_action("paste");
        }
        menu_item("hands");
        // Clips of the selected bones (VP-73).
        ImGui::Separator();
        ImGui::TextDisabled("Clips");
        double a, b;
        if (!selection_.empty() && clip_range(a, b)) {
            if (ImGui::MenuItem("Save Clip of Selected Bones...")) {
                name_prompt_ = "Clip";
                name_action_ = NameAction::SaveClip;
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Save Selected Bones Clip...");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Select bones, then Shift-drag a frame range on the timeline first");
        }
        clips_of_kind("selection", "");
        ImGui::Separator();
        for (const char* id : {"mirror_l2r", "mirror_r2l", "flip_pose", "reset_pose"}) menu_item(id);
    } else if (skel_[node].attachment) {
        // Attachment point (VP-74).
        const std::string& name = skel_[node].name;
        header(name);
        int pin = pin_at(clip, *rig_, node, frame_);
        bool rides_other = pin >= 0 && !clip.pins[pin].target.empty();
        if (pin < 0 || rides_other) {
            if (ImGui::MenuItem("Hold in World from Here")) {
                select(node, false);
                run_action("pin_world");
            }
        }
        auto other = std::find_if(selection_.begin(), selection_.end(), [&](int s) { return s != node; });
        bool two = selection_.size() == 2 && std::find(selection_.begin(), selection_.end(), node) != selection_.end();
        if (two && ImGui::MenuItem(("Bind to " + skel_[*other].name + " from Here").c_str())) {
            int target = *other;
            selection_ = {target, node};
            run_action("pin_bone");
        } else if (!two) {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Bind to a Bone...");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Select the bone to ride, then Shift-click this point");
        }
        if (pin >= 0) {
            const int parent = skel_[node].parent;
            std::string follow = parent >= 0 ? skel_[parent].name : "the avatar";
            if (ImGui::MenuItem(("Release from Here (follow " + follow + " again)").c_str())) {
                select(node, false);
                run_action("unpin");
            }
            if (ImGui::MenuItem("Delete Pin")) {
                select(node, false);
                run_action("delete_pin");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(("Select " + name).c_str())) select(node, false);
        if (ImGui::MenuItem(("Key " + name).c_str()))
            edit("Key", [&](Clip& c) { key_current(c, skel_, node, frame_); });
        if (ImGui::MenuItem(("Reset " + name).c_str())) edit("Reset", [&](Clip& c) { reset_bone(c, name, frame_); });
    } else {
        // Body part (VP-71/72).
        BodyPart part = body_part_of(skel_, node);
        const std::string& P = part.label;
        header(P);
        if (part.kind == PartKind::Hand) {
            if (ImGui::MenuItem(show_hands_ ? "Hide Hand Poser" : "Show Hand Poser", hint("hands").c_str()))
                show_hands_ = !show_hands_;
        } else {
            int limb = rig_->limb_of_bone(node);
            if (part.kind == PartKind::Torso) limb = rig_->find_limb("Spine");
            if (limb >= 0) {
                const LimbInfo& l = rig_->limbs()[limb];
                std::string label = "Switch " + l.label + " to " + (limb_states_[limb].ik_on ? "FK" : "IK");
                if (ImGui::MenuItem(label.c_str(), hint("ik_toggle").c_str())) {
                    if (part.kind == PartKind::Torso) select(rig_->limbs()[limb].end, false);
                    else select(node, false);
                    run_action("ik_toggle");
                }
            }
        }
        if (ImGui::MenuItem(("Select " + P).c_str())) select_part(part);
        if (part.kind == PartKind::Arm) ImGui::SetItemTooltip("Includes the hand and the IK handles");
        if (ImGui::MenuItem(("Key " + P).c_str()))
            edit("Key " + P, [&](Clip& c) {
                for (int b : part.bones) key_current(c, skel_, b, frame_);
            });
        if (ImGui::MenuItem(("Reset " + P).c_str()))
            edit("Reset " + P, [&](Clip& c) {
                for (int b : part.bones) reset_bone(c, skel_[b].name, frame_);
            });
        if (!part.side.empty()) {
            std::string other = opposite(part.side) + P.substr(part.side.size());
            if (ImGui::MenuItem(("Mirror " + P + " to " + other).c_str()))
                edit("Mirror " + P, [&](Clip& c) { mirror_bones(c, skel_, frame_, pose_, part.bones); });
        }
        if (ImGui::MenuItem(("Copy " + P + (part.kind == PartKind::Arm ? " (with hand)" : "")).c_str())) {
            part_clipboard_ = copy_pose(clip, frame_, part_tracks(part));
            status("Copied " + P);
        }
        if (!part_clipboard_.entries.empty() && ImGui::MenuItem(("Paste onto " + P).c_str())) {
            Clip before = doc_.clip();
            edit("Paste onto " + P, [&](Clip& c) { paste_pose_part(c, part_clipboard_, frame_, part_tracks(part)); });
            offer_pose_blend(std::move(before), frame_);
        }

        std::string kind = kind_of(part.kind);
        if (!kind.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Clips");
            double a, b;
            if (clip_range(a, b)) {
                if (ImGui::MenuItem(("Save " + P + " Clip (frames " + std::to_string(int(a)) + "-" + std::to_string(int(b)) +
                                     ")...")
                                        .c_str())) {
                    context_part_ = part;
                    name_prompt_ = P;
                    name_action_ = NameAction::SavePartClip;
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::MenuItem(("Save " + P + " Clip...").c_str());
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Shift-drag a frame range on the timeline first");
            }
            clips_of_kind(kind, part.side);
            if (part.kind != PartKind::Head) {
                ImGui::Separator();
                ImGui::TextDisabled("%s poses at frame %d", P.c_str(), f);
                poses_of_kind(kind, part.side);
            }
        }
        ImGui::Separator();
        if (part.kind == PartKind::Torso) {
            if (ImGui::MenuItem("Save Pose...")) {
                clear_selection();
                name_prompt_ = "Whole pose";
                name_action_ = NameAction::SavePose;
            }
            for (const char* id : {"key_all", "reset_pose", "mirror_l2r", "mirror_r2l", "flip_pose"}) menu_item(id);
        } else if (!kind.empty() && ImGui::MenuItem(("Save " + P + " Pose...").c_str())) {
            context_part_ = part;
            name_prompt_ = P;
            name_action_ = NameAction::SavePartPose;
        }
    }
    ImGui::EndPopup();
}

}  // namespace vats
