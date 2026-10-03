// Viewport Avatar Toolset - Tools > Clips (spec 08 CL): several named clips per project, Export All Clips and the
// AO notecards.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The active clip is Project::clip, so the timeline, the graph and every tool act on it unchanged. Switching clips and
// every list edit are scene steps (one undo step each, with the clip list in SceneState). Animation names in the
// notecards are the export names of the active actor, as Export All Clips writes them.
#include <cstdio>

#include "app.h"
#include "theme.h"
#include "vats/ao_notecard.h"
#include "vats/clips.h"
#include "vats/export_name.h"

namespace vats {

void App::clip_edit(const std::string& label, const std::function<void(Project&)>& change) {
    if (doc_.history.is_open() || scene_busy()) return status("Finish the current edit first");
    playing_ = false;
    scene_edit(label, change);
    selected_prop_ = -1;  // props are the clip's
    if (frame_ > doc_.clip().end_frame) set_frame(doc_.clip().end_frame);
}

// fmt 0: Firestorm's AO, 1: ZHAO-II. Each clip with an AO state, under its export name without the mirrored copies.
std::string App::ao_notecard_text(int fmt, std::vector<std::string>* warnings) {
    const Project& p = doc_.project;
    std::vector<AoClip> clips;
    for (int k = 0; k < int(p.clips.size()); ++k) {
        if (p.clips[k].ao_state.empty()) continue;
        ExportNaming n = export_naming(k);
        if (multi_actor()) n.actor = p.actors[p.active].name;
        std::string name = export_file_name(n, export_stem(), active_actor_clip(k).mirror_export, "anim");
        clips.push_back({p.clips[k].ao_state, name.substr(0, name.size() - 5)});
    }
    AoNotecard card = ao_notecard(fmt ? AoFormat::Zhao : AoFormat::Firestorm, clips);
    if (warnings) *warnings = std::move(card.warnings);
    return card.text;
}

void App::save_ao_notecard(const std::string& path) {
    std::string why;
    if (!write_text(path, ao_notecard_text(ao_format_), false, why)) return message("Could not save " + path, why);
    status("Saved " + path.substr(path.find_last_of('/') + 1));
}

void App::draw_clips_panel() {
    if (!show_clips_) return;
    place_tool_window("Clips", 24, 40);
    if (!ImGui::Begin("Clips", &show_clips_)) return ImGui::End();
    help_button("clips");
    Project& p = doc_.project;
    hint("Several animations in one project, such as an AO's stands and walks.");

    const int n = clip_count(p), active = p.active_clip;
    // The list: click to switch, double-click to rename; the AO state is picked per clip.
    if (ImGui::BeginTable("##clips", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("AO state", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int k = 0; k < n; ++k) {
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (clip_renaming_ == k) {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                const bool done = ImGui::InputText("##name", clip_name_buf_, sizeof clip_name_buf_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (done || ImGui::IsItemDeactivated()) {
                    const std::string name = clip_name_buf_;
                    clip_renaming_ = -1;
                    if (!name.empty() && name != clip_name(p, k))
                        clip_edit("Rename Clip", [k, name](Project& pr) {
                            name_clips(pr);
                            pr.clips[k].name = name;
                        });
                }
            } else {
                if (ImGui::Selectable(clip_name(p, k).c_str(), k == active, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        clip_renaming_ = k;
                        std::snprintf(clip_name_buf_, sizeof clip_name_buf_, "%s", clip_name(p, k).c_str());
                    } else if (k != active) {
                        clip_edit("Switch to " + clip_name(p, k), [k](Project& pr) { set_active_clip(pr, k); });
                    }
                }
                ImGui::SetItemTooltip("Click to edit this clip, double-click to rename it");
            }
            ImGui::TableNextColumn();
            const std::string state = p.clips.empty() ? "" : p.clips[k].ao_state;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##ao", state.empty() ? "(none)" : state.c_str())) {
                auto pick = [&](const std::string& s, const char* text) {
                    if (ImGui::Selectable(text, s == state) && s != state)
                        clip_edit("AO State", [k, s](Project& pr) {
                            name_clips(pr);
                            pr.clips[k].ao_state = s;
                        });
                };
                pick("", "(none)");
                for (const std::string& s : ao_states()) {
                    const bool zhao = ao_state_known(AoFormat::Zhao, s);
                    pick(s, zhao ? s.c_str() : (s + " (Firestorm only)").c_str());
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    auto new_name = [&](std::string base) {
        for (int i = 2;; ++i) {
            std::string s = base + std::to_string(i);
            bool used = false;
            for (int k = 0; k < n; ++k) used |= clip_name(p, k) == s;
            if (!used) return s;
        }
    };
    if (ImGui::Button("Add")) clip_edit("Add Clip", [name = new_name("Clip ")](Project& pr) { add_clip(pr, name, false); });
    ImGui::SetItemTooltip("A new empty clip after this one, with its frame rate, length, loop, priority and export settings");
    ImGui::SameLine();
    if (ImGui::Button("Duplicate"))
        clip_edit("Duplicate Clip", [name = new_name(clip_name(p, active) + " ")](Project& pr) { add_clip(pr, name, true); });
    ImGui::SetItemTooltip("A copy of this clip, keys and all");
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
        clip_renaming_ = active;
        std::snprintf(clip_name_buf_, sizeof clip_name_buf_, "%s", clip_name(p, active).c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(n < 2);
    if (ImGui::Button("Delete")) {
        const std::string name = clip_name(p, active);
        host_.ask("Delete " + name + "?", "The clip and its keys go (one undo step).", {"Delete", "Cancel"}, [this, active](int b) {
            if (b == 0 && active < clip_count(doc_.project))
                clip_edit("Delete Clip", [active](Project& pr) { delete_clip(pr, active); });
        });
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(active == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) clip_edit("Move Clip", [active](Project& pr) { move_clip(pr, active, active - 1); });
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Move this clip up");
    ImGui::SameLine();
    ImGui::BeginDisabled(active >= n - 1);
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) clip_edit("Move Clip", [active](Project& pr) { move_clip(pr, active, active + 1); });
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Move this clip down");
    ImGui::SameLine();
    ImGui::BeginDisabled(n < 2);
    if (ImGui::Button("Use These Settings for All Clips"))
        clip_edit("Settings for All Clips", [](Project& pr) { apply_settings_to_all_clips(pr); });
    ImGui::SetItemTooltip("Every other clip takes this clip's frame rate (keeping its timing), priority, eases, hand pose and "
                          "export settings; each keeps its own export name and number");
    ImGui::EndDisabled();

    if (n > 1) {
        subheading("Export All Clips");
        std::string names;
        for (int k = 0; k < n; ++k) {
            ExportNaming nm = export_naming(k);
            if (multi_actor()) nm.actor = p.actors[p.active].name;
            names += (k ? "\n" : "") + export_file_name(nm, export_stem(), active_actor_clip(k).mirror_export, "anim");
        }
        if (multi_actor()) names += "\n(and the same for each other actor)";
        ImGui::TextDisabled("%s", names.c_str());
        if (ImGui::Button("Export All Clips (.anim)")) export_now(false, false, true);
        ImGui::SetItemTooltip("Each clip with its own export settings into this clip's export folder. [CLIP] in the pattern "
                              "is the clip's name; without it the name is added at the end.");
        if (host_.can_upload()) {
            ImGui::SameLine();
            if (ImGui::Button("Upload All Clips...")) upload_now(true);
        }
    }

    subheading("AO notecard");
    const char* formats[] = {"Firestorm AO (import notecard)", "ZHAO-II / Oracul"};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##aofmt", &ao_format_, formats, 2);
    std::vector<std::string> warnings;
    std::string text = ao_notecard_text(ao_format_, &warnings);
    if (text.empty()) {
        hint("Pick an AO state for each clip above to fill the notecard.");
    } else {
        ImGui::InputTextMultiline("##card", text.data(), text.size() + 1, ImVec2(-1, ImGui::GetTextLineHeight() * 8.5f),
                                  ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(text.c_str());
            status(std::string("Copied the ") + (ao_format_ ? "ZHAO-II" : "Firestorm AO") + " notecard");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save as .txt...")) show_dialog(Dialog::AoNotecard);
    }
    for (const std::string& w : warnings) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", w.c_str());
    hint(ao_format_ ? "Paste into the AO's animation notecard (ZHAO-II: Default) next to the uploaded animations. ZHAO-II "
                      "takes several animations only for Standing, Walking, Sitting and Sitting On Ground."
                    : "Put a notecard with this text in the folder with the uploaded animations, then in Firestorm's "
                      "Animation Overrider pick Import and the notecard. Names must match the animations exactly.");
    ImGui::End();
}

}  // namespace vats
