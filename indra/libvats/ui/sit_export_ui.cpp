// Viewport Avatar Toolset - sit-system lines for couples and groups (spec 08 GR-2): AVsitter2 and nPose.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The Actors panel shows ready-to-paste lines for both systems, with Copy and Save as .txt; the placement note
// written with the export carries them too. Animation names are the export names, from the active actor's export
// settings, as Export writes them. The furniture root offset is kept in every actor's export settings as
// "sit_root": [x, y, z, rx, ry, rz] (metres, Euler degrees), so it is the same whichever actor is edited; the first
// actor that has it wins.
#include <algorithm>

#include "app.h"
#include "theme.h"
#include "vats/export_name.h"
#include "vats/sit_export.h"

namespace vats {

namespace {

// The first actor's that has one: an actor added later has none yet.
SitRoot read_sit_root(const Project& p) {
    SitRoot r;
    for (int i = 0; i < int(std::max<size_t>(p.actors.size(), 1)); ++i)
        if (const Json* a = actor_clip(p, i).export_settings.find("sit_root"); a && a->is_array() && a->arr.size() == 6) {
            for (int k = 0; k < 3; ++k) r.pos[k] = a->arr[k].is_number() ? a->arr[k].num : 0;
            for (int k = 0; k < 3; ++k) r.rot[k] = a->arr[3 + k].is_number() ? a->arr[3 + k].num : 0;
            break;
        }
    return r;
}

std::string text_of(const Json& ex, const char* key, const char* fallback) {
    const Json* v = ex.find(key);
    return v && v->is_string() ? v->str : fallback;
}

}  // namespace

// fmt 0: AVsitter2 AVpos lines, 1: nPose V4 XANIM lines, for every actor.
std::string App::sit_lines(int fmt) const {
    const Project& p = doc_.project;
    const Json& ex = doc_.clip().export_settings;
    const Json* num = ex.find("number");
    ExportNaming naming{text_of(ex, "name", ""), num && num->is_number() ? int(num->num) : 1, text_of(ex, "side", ""),
                        text_of(ex, "pattern", "[NAME]_[#]_[SIDE]"), ""};
    std::string stem = doc_.path.empty() ? "" : doc_.path.substr(doc_.path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));
    auto base = [&](bool mirrored) {
        std::string n = export_file_name(naming, stem, mirrored, "anim");
        return n.substr(0, n.size() - 5);
    };
    const std::string pose = base(false);  // the placement note's name
    std::vector<Sitter> sitters;
    for (const Actor& a : p.actors) {
        naming.actor = a.name;
        sitters.push_back({a.name, base(doc_.clip().mirror_export), a.placement()});
    }
    const SitRoot root = read_sit_root(p);
    return fmt == 0 ? avsitter_lines(pose, sitters, root) : npose_lines(sitters, root);
}

void App::save_sit_lines(const std::string& path) {
    std::string why;
    if (!write_text(path, sit_lines(sit_format_), false, why)) return message("Could not save " + path, why);
    status("Saved " + path.substr(path.find_last_of('/') + 1));
}

void App::draw_sit_export() {
    ImGui::SeparatorText("Sit systems (furniture)");
    const SitRoot root = read_sit_root(doc_.project);
    // Typed values apply on Enter, as one undo step for every actor.
    float pos[3] = {float(root.pos.x), float(root.pos.y), float(root.pos.z)};
    float rot[3] = {float(root.rot.x), float(root.rot.y), float(root.rot.z)};
    const bool pos_set = ImGui::InputFloat3("Sit target (m)", pos, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemTooltip("Where the shared sit target is from the furniture's root prim. Press Enter to apply.");
    const bool rot_set = ImGui::InputFloat3("Rotation (deg)", rot, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemTooltip("The sit target's rotation in the root prim, as the build window shows it. Press Enter to apply.");
    if (pos_set || rot_set) {
        Json a = Json::array();
        for (float v : pos) a.push(double(v));
        for (float v : rot) a.push(double(v));
        scene_edit("Furniture Root Offset", [a](Project& pr) {
            for (int k = 0; k < int(pr.actors.size()); ++k) {
                Json& ex = actor_clip(pr, k).export_settings;
                if (!ex.is_object()) ex = Json::object();
                ex.set("sit_root", a);
            }
        });
    }
    const char* names[] = {"AVsitter2 (AVpos notecard)", "nPose V4 (SET card)"};
    for (int fmt = 0; fmt < 2; ++fmt) {
        ImGui::PushID(fmt);
        std::string lines = sit_lines(fmt);
        ImGui::TextUnformatted(names[fmt]);
        ImGui::InputTextMultiline("##lines", lines.data(), lines.size() + 1,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * (fmt ? 4.5f : 8.5f)), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(lines.c_str());
            status(std::string("Copied the ") + (fmt ? "nPose" : "AVsitter") + " lines");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save as .txt...")) sit_format_ = fmt, show_dialog(Dialog::SitLines);
        ImGui::PopID();
    }
    hint("AVsitter: paste into the AVpos notecard; the prim needs one [AV]sitA/B pair per actor. nPose: paste into a SET "
         "card; the .init card needs SEAT_INIT|<number of actors>.");
}

}  // namespace vats
