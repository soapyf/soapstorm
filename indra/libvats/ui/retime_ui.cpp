// Viewport Avatar Toolset - retime markers on the timeline and Split Dance at Beats.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.3 (TE-5, TE-6). The maths is in the core (vats/retime.h); this file draws the
// markers, runs the drag as one undo step and writes the parts, through the Host only, so the viewer has it too.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "settings.h"
#include "theme.h"
#include "vats/audio.h"
#include "vats/export_name.h"
#include "vats/retime.h"

namespace vats {

void App::set_retime(bool on) {
    retime_on_ = on;
    if (!on) retime_markers_.clear(), retime_drag_ = -1;
    status(on ? "Retime: double-click the ruler to drop a marker, drag a marker to retime" : "Retime off");
}

// TE-5. The markers live on the ruler (y0..y1). A press on one starts a drag that re-runs drag_marker from the
// clip as it was at the press every frame, so the keys follow live and the drag is one undo step, "Retime".
// ponytail: copies the whole clip each frame of a drag; fine for SL-length clips.
bool App::retime_timeline(ImDrawList* dl, float x0, float x1, float y0, float y1, int last, bool hovered) {
    if (!retime_on_) return false;
    Clip& clip = doc_.clip();
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto x_of = [&](double fr) { return float(x0 + (x1 - x0) * fr / last); };
    const double mouse_frame = std::max(0.0, double(m.x - x0) / (x1 - x0) * last);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), (accent_colour() & 0x00FFFFFF) | 0x1C000000);  // the mode shows
    int over = -1;
    for (int i = 0; i < int(retime_markers_.size()); ++i) {
        const float x = x_of(retime_markers_[i]);
        const bool hot = retime_drag_ == i || (retime_drag_ < 0 && hovered && std::fabs(m.x - x) < 6 && m.y >= y0 && m.y <= y1);
        if (hot) over = i;
        const ImU32 c = hot ? accent_colour() : IM_COL32(236, 236, 240, 210);
        dl->AddLine(ImVec2(x, y1), ImVec2(x, y1 + 6), c, 2);
        dl->AddTriangleFilled(ImVec2(x - 6, y0 + 3), ImVec2(x + 6, y0 + 3), ImVec2(x, y1 - 1), c);
    }
    if (over >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (retime_drag_ < 0) ImGui::SetTooltip("Retime marker at frame %.2f: drag to stretch the keys since the marker before it",
                                                retime_markers_[over]);
    }
    if (ImGui::IsItemActivated() && over >= 0 && retime_markers_[over] > 0 && !doc_.history.is_open()) {
        retime_drag_ = over;
        retime_press_clip_ = clip;
        retime_press_markers_ = retime_markers_;
        retime_view_last_ = last;
        doc_.history.begin(clip);
    } else if (ImGui::IsItemActivated() && hovered && m.y >= y0 && m.y <= y1 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const double f = std::min(snap_marker(clip, mouse_frame, graph_.snap_frames()), double(clip.end_frame));
        auto at = std::lower_bound(retime_markers_.begin(), retime_markers_.end(), f);
        const bool taken = (at != retime_markers_.end() && *at - f < 0.5) || (at != retime_markers_.begin() && f - *std::prev(at) < 0.5);
        if (!taken) retime_markers_.insert(at, f);
    }
    if (retime_drag_ < 0) return false;
    if (ImGui::IsItemActive()) {
        clip = retime_press_clip_;
        retime_markers_ = retime_press_markers_;
        drag_marker(clip, retime_markers_, size_t(retime_drag_), snap_marker(clip, mouse_frame, graph_.snap_frames()));
        const double was = retime_press_markers_[retime_drag_], prev = retime_drag_ ? retime_press_markers_[retime_drag_ - 1] : 0;
        ImGui::SetTooltip("Frame %.2f: frames %.2f to %.2f play at %.0f%% of their length", retime_markers_[retime_drag_], prev,
                          was, 100 * (retime_markers_[retime_drag_] - prev) / (was - prev));
    } else {  // released
        retime_drag_ = -1;
        if (doc_.history.is_open() && doc_.history.commit("Retime", clip)) {
            sync_actor_timing(doc_.project);  // GR-3: every actor keeps the same length and loop
            mark_dirty();
        }
    }
    return true;
}

// TE-6: Tools > Split Dance at Beats...
void App::draw_split_dance_window() {
    if (!show_split_dance_) return;
    const float em = ImGui::GetFontSize();
    if (!ImGui::Begin("Split Dance at Beats", &show_split_dance_, ImGuiWindowFlags_AlwaysAutoResize)) return ImGui::End();
    ImGui::PushTextWrapPos(em * 26);
    const Clip& c = doc_.clip();
    const int fps = std::max(c.fps, 1);
    const std::vector<int> cuts = dance_cuts(c);
    if (cuts.empty()) {
        ImGui::TextWrapped("The animation is %.2f s: it fits SL's 60 s limit in one part.", double(c.end_frame) / fps);
        ImGui::PopTextWrapPos();
        return ImGui::End();
    }
    std::vector<int> beats;
    if (c.audio)
        for (double t : beat_times(*c.audio, 0, double(c.end_frame) / fps)) beats.push_back(int(std::lround(t * fps)));
    ImGui::TextWrapped("%.2f s in %zu parts of at most 60 s, each cut on the last beat before the limit. Each part starts on "
                       "the frame the one before it ends on.", double(c.end_frame) / fps, cuts.size() + 1);
    if (beats.empty()) hint("No beat grid or marked beats (the timeline's right-click menu, Audio): the cuts fall at 60 s.");
    if (ImGui::BeginTable("##parts", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Part");
        ImGui::TableSetupColumn("Frames");
        ImGui::TableSetupColumn("Ends");
        ImGui::TableHeadersRow();
        for (size_t k = 0; k <= cuts.size(); ++k) {
            const int a = k ? cuts[k - 1] : 0, b = k < cuts.size() ? cuts[k] : c.end_frame;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", k + 1);
            ImGui::TableNextColumn();
            ImGui::Text("%d-%d, %.2f s", a, b, double(b - a) / fps);
            ImGui::TableNextColumn();
            const bool on_beat = std::binary_search(beats.begin(), beats.end(), b);
            ImGui::TextUnformatted(k == cuts.size() ? "the end" : on_beat ? "on a beat" : "at 60 s (no beat)");
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    const bool can_save = !doc_.path.empty();
    ImGui::BeginDisabled(!can_save);
    auto save = [&](bool overwrite) {
        const std::string written = save_parts(split_dance(c, cuts), doc_.path, overwrite, split_dance_confirm_);
        if (!written.empty()) status("Saved " + std::to_string(cuts.size() + 1) + " parts beside the project");
    };
    if (icon_label_button(icon::kSave, "Save Parts as Projects")) guarded(doc_.path, [&] { save(false); });
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", can_save ? "<project>_part1.vat, _part2.vat... beside the project file"
                                         : "Save the project first: the parts are saved beside it");
    ImGui::SameLine();
    if (icon_label_button(icon::kExport, "Export All as .anim")) export_dance_parts(split_dance(c, cuts));
    ImGui::SetItemTooltip("Every part to the export folder under the export pattern, numbered from Export's Number%s",
                          [&] {
                              const Json* v = c.export_settings.find("save_to_library");
                              return v && v->is_bool() && v->b ? ", and to the Animations library" : "";
                          }());
    if (!split_dance_confirm_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "These parts exist already and would be replaced (kept as .bak):");
        ImGui::TextUnformatted(split_dance_confirm_.c_str());
        if (ImGui::Button("Replace Them")) split_dance_confirm_.clear(), guarded(doc_.path, [&] { save(true); });
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) split_dance_confirm_.clear();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// Each part exported as the project would be (Export's settings, the mirrored choice), swapped in for the document
// clip in turn as ScratchAside does, named by the export pattern with [#] counting up from Export's Number (a
// pattern without [#] gets "_[#]").
// ponytail: the active actor only, and no mirrored second copy; export a couple's parts actor by actor.
void App::export_dance_parts(std::vector<Clip> parts) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    const Json& ex = doc_.clip().export_settings;
    auto str = [&](const char* key, const char* fallback) {
        const Json* v = ex.find(key);
        return v && v->is_string() ? v->str : std::string(fallback);
    };
    const std::string folder = str("folder", "");
    if (std::error_code ec; folder.empty() || !std::filesystem::exists(u8path(folder), ec))
        return message("Choose an export folder first", "File > Export SL .anim..., then Choose... next to Folder.");
    const Json* lib = ex.find("save_to_library");
    const bool to_library = lib && lib->is_bool() && lib->b;
    ExportNaming naming = export_naming();  // the clip's name too, in a project with several clips (08 CL-4)
    if (naming.pattern.find("[#]") == std::string::npos) naming.pattern += "_[#]";
    const std::string stem = export_stem();
    const int first = naming.number;
    Clip keep = std::move(doc_.clip());
    std::string names;
    for (size_t i = 0; i < parts.size(); ++i) {
        naming.number = first + int(i);
        const std::string path = folder + "/" + export_file_name(naming, stem, keep.mirror_export, "anim");
        doc_.clip() = std::move(parts[i]);
        const bool ok = export_anim(path);
        if (ok && to_library) anim_file_to_library(path);
        if (!ok) break;  // export_anim said why
        names += "- " + path.substr(path.find_last_of('/') + 1) + "\n";
    }
    doc_.clip() = std::move(keep);
    if (!names.empty())
        message("Exported the parts", "To " + folder + (to_library ? ", and to the Animations library" : "") + ":\n" + names +
                                          "\nPlay them one after another in your dance HUD; each starts on the pose the one "
                                          "before it ends on.");
}

}  // namespace vats
