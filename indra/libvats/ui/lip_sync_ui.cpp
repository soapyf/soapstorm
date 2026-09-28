// Viewport Avatar Toolset - lip sync in the Face window and its mouth shapes on the timeline (spec 08 LS).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The analysis, the Rhubarb reader and the keying are in the core (vats/lip_sync.h), so the viewer shares them;
// this file has the controls and the timeline row.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "vats/lip_sync.h"

namespace vats {

struct LipSyncUi {
    std::string head = "\x01";  // the head `table` was loaded for
    FaceTable table;
    LipShapes shapes;
    bool shapes_loaded = false;
    std::string error;
    int from = 0, to = -1;
    float range_db = 30;
    int drag = -1, drag_frame = 0;  // the cue being nudged on the timeline
};

namespace {

std::string read_text(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

bool App::lip_tables(std::string& err) {
    if (!lip_ui_) lip_ui_ = std::make_shared<LipSyncUi>();
    LipSyncUi& ui = *lip_ui_;
    if (!ui.shapes_loaded) {
        const std::string text = read_text(data_dir_ + "/retarget/lip-shapes.json");
        ui.error.clear();
        if (text.empty()) ui.error = "data/retarget/lip-shapes.json is missing";
        else if (!parse_lip_shapes(text, ui.shapes, ui.error)) ui.error = "lip-shapes.json: " + ui.error;
        ui.shapes_loaded = true;
    }
    if (const std::string head = face_head(); ui.head != head) {
        ui.table = FaceTable{};
        ui.head = head;
        std::string why;
        if (!load_face_table(head, ui.table, why)) ui.error = why;
    }
    return err = ui.error, ui.error.empty();
}

void App::draw_lip_sync(bool positions) {
    std::string err;
    if (!lip_tables(err)) return (void)ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", err.c_str());
    LipSyncUi& ui = *lip_ui_;
    Clip& clip = doc_.clip();
    hint("Keys the jaw and lips as mouth shapes over the frames below, from the loaded audio or from a Rhubarb Lip Sync "
         "file. The shapes show on the timeline: drag one to nudge it.");
    const float label_w = ImGui::GetFontSize() * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
    };
    if (ui.to < 0 || ui.to > clip.end_frame) ui.to = clip.end_frame;
    ui.from = std::clamp(ui.from, 0, ui.to);
    label("Frames");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    ImGui::DragIntRange2("##lipframes", &ui.from, &ui.to, 0.2f, 0, clip.end_frame);
    ImGui::SameLine();
    double a = 0, b = 0;
    ImGui::BeginDisabled(!clip_range(a, b));
    if (ImGui::SmallButton("Timeline Range")) ui.from = int(a), ui.to = int(b);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The range Shift+dragged on the timeline");
    if (!positions) hint("Move face bones is off, so only the jaw moves: most lip shapes only move bones.");

    ImGui::SeparatorText("From the audio");
    label("Quietest");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    ImGui::SliderFloat("##liprange", &ui.range_db, 12, 48, "-%.0f dB");
    ImGui::SetItemTooltip("The loudest moment opens the mouth fully; sound this far below it keeps the mouth shut");
    const bool has_audio = clip.audio && audio_data_.frames() > 0;
    ImGui::BeginDisabled(!has_audio);
    if (ImGui::Button("Lip Sync from Audio")) {
        LipAudioOptions opt;
        opt.from = ui.from, opt.to = ui.to, opt.range_db = ui.range_db;
        LipSync ls = lip_sync_from_audio(audio_data_, clip.audio->offset, clip.fps, clip.end_frame, opt);
        ls.positions = positions;
        edit("Lip Sync from Audio", [&](Clip& c) { apply_lip_sync(c, ui.table, ui.shapes, ls); });
        status("Lip sync from the audio: " + std::to_string(ls.cues.size()) + " mouth shapes on frames " +
               std::to_string(ls.from) + " to " + std::to_string(ls.to));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", has_audio ? "The loudness opens the jaw; the vowel chooses an open, rounded or wide mouth"
                                          : "Load an audio file first (File > Load Audio...)");

    ImGui::SeparatorText("From Rhubarb Lip Sync");
    hint("Run Rhubarb Lip Sync on the audio yourself, for example rhubarb -f json -o mouth.json speech.wav, then "
         "import its JSON or TSV. Times count from the audio's start.");
    if (ImGui::Button("Import Rhubarb...")) show_dialog(Dialog::Rhubarb);
    ImGui::SetItemTooltip("Mouth shapes A-H and X, mapped to face shapes by data/retarget/lip-shapes.json");

    if (clip.lip_sync) {
        ImGui::Separator();
        ImGui::Text("%zu mouth shapes on frames %d to %d", clip.lip_sync->cues.size(), clip.lip_sync->from, clip.lip_sync->to);
        if (ImGui::Button("Remove Lip Sync")) {
            edit("Remove Lip Sync", [&](Clip& c) { remove_lip_sync(c, ui.table, ui.shapes); });
            status("Removed the lip sync: the mouth is as it was before");
        }
        ImGui::SetItemTooltip("Take the lip sync's moves back out of the keys (one undo step)");
    }
}

void App::import_rhubarb(const std::string& path) {
    std::string err;
    if (!lip_tables(err)) return message("Cannot lip sync", err);
    LipSyncUi& ui = *lip_ui_;
    std::vector<RhubarbCue> cues;
    if (!parse_rhubarb(read_text(path), cues, err)) return message("Could not read the Rhubarb file", path + "\n\n" + err);
    const Clip& clip = doc_.clip();
    if (ui.to < 0 || ui.to > clip.end_frame) ui.to = clip.end_frame;
    LipSync ls = lip_sync_from_cues(cues, clip.audio ? clip.audio->offset : 0, clip.fps, clip.end_frame, ui.from, ui.to);
    ls.positions = face_positions();
    edit("Import Rhubarb", [&](Clip& c) { apply_lip_sync(c, ui.table, ui.shapes, ls); });
    const double last = cues.back().start + (clip.audio ? clip.audio->offset : 0);
    status("Imported " + std::to_string(ls.cues.size()) + " mouth shapes from " + path.substr(path.find_last_of("/\\") + 1) +
           (last * clip.fps > clip.end_frame + 0.5 ? " (the cues run past the last frame; lengthen the clip for the rest)" : ""));
}

// --- Timeline row ------------------------------------------------------------------------------------------

bool App::lip_sync_timeline(ImDrawList* dl, float x0, float x1, float y0, float y1, int last, bool hovered) {
    Clip& clip = doc_.clip();
    if (!clip.lip_sync || clip.lip_sync->cues.empty()) return false;
    if (!lip_ui_) lip_ui_ = std::make_shared<LipSyncUi>();
    LipSyncUi& ui = *lip_ui_;
    const LipSync& ls = *clip.lip_sync;
    const float h = ImGui::GetFontSize() + 2, top = y0 + 11;  // under the loop flags
    if (top + h > y1) return false;
    auto x_of = [&](double f) { return float(x0 + (x1 - x0) * f / std::max(last, 1)); };
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int count = int(ls.cues.size());
    auto frame_of = [&](int i) { return i == ui.drag ? ui.drag_frame : ls.cues[size_t(i)].frame; };
    int over = -1;
    for (int i = 0; i < count; ++i) {
        const float x = x_of(frame_of(i)), next = std::min(x1, i + 1 < count ? x_of(frame_of(i + 1)) : x_of(ls.to + 1));
        const std::string& s = ls.cues[size_t(i)].shape;
        const bool rest = s == "X";
        const ImU32 fill = i == ui.drag ? accent_colour() : rest ? IM_COL32(150, 150, 160, 70) : IM_COL32(236, 128, 170, 150);
        dl->AddRectFilled(ImVec2(x, top), ImVec2(std::max(x + 2, next - 1), top + h), fill, 2);
        const char* text = s.c_str();
        const std::string first(1, char(std::toupper(static_cast<unsigned char>(s.empty() ? ' ' : s[0]))));
        if (ImGui::CalcTextSize(text).x + 4 > next - x) text = first.c_str();
        if (ImGui::CalcTextSize(text).x + 4 <= next - x) dl->AddText(ImVec2(x + 2, top + 1), IM_COL32(245, 245, 250, 230), text);
        if (hovered && m.y >= top && m.y <= top + h && std::fabs(m.x - x) < 5) over = i;
    }
    auto clamp_frame = [&](int i, int f) {
        const int lo = i > 0 ? ls.cues[size_t(i) - 1].frame + 1 : ls.from;
        const int hi = i + 1 < count ? ls.cues[size_t(i) + 1].frame - 1 : ls.to;
        return std::clamp(f, lo, std::max(lo, hi));
    };
    if (ImGui::IsItemActivated() && over >= 0) ui.drag = over, ui.drag_frame = ls.cues[size_t(over)].frame;
    if (ui.drag >= count) ui.drag = -1;  // undone under a drag
    if (ui.drag < 0) {
        if (over >= 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("Mouth shape %s from frame %d: drag to nudge", ls.cues[size_t(over)].shape.c_str(),
                              ls.cues[size_t(over)].frame);
        }
        return false;
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        const int f = int(std::clamp(std::round(double(m.x - x0) / (x1 - x0) * last), 0.0, double(last)));
        ui.drag_frame = clamp_frame(ui.drag, f);
        set_frame(ui.drag_frame);  // hear it: a scrub snippet plays at the new frame
        ImGui::SetTooltip("Mouth shape %s from frame %d", ls.cues[size_t(ui.drag)].shape.c_str(), ui.drag_frame);
    }
    if (ImGui::IsItemDeactivated()) {
        const int i = std::exchange(ui.drag, -1);
        if (ui.drag_frame != ls.cues[size_t(i)].frame) {
            std::string err;
            if (!lip_tables(err)) {
                status(err);
                return true;
            }
            LipSync moved = ls;
            moved.cues[size_t(i)].frame = ui.drag_frame;
            edit("Nudge Mouth Shape", [&](Clip& c) { apply_lip_sync(c, ui.table, ui.shapes, moved); });
            status("Mouth shape " + moved.cues[size_t(i)].shape + " now starts at frame " + std::to_string(ui.drag_frame));
        }
    }
    return true;
}

}  // namespace vats
