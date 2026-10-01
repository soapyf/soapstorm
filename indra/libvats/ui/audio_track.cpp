// Viewport Avatar Toolset - the audio track and time editing in the app (spec 08 AU, TE).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Decoding, beats and the time edits are in the core (vats/audio.h, vats/time_edit.h) so the viewer
// shares them; this file plays the samples through the host and draws the waveform lane.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "vats/time_edit.h"
#include "theme.h"
#include "widgets.h"

namespace vats {

// --- Loading ---------------------------------------------------------------------------------------

void App::load_audio(const std::string& path) {
    AudioData data;
    std::string err;
    if (!load_audio_file(path, data, err)) return message("Could not load the audio", path + "\n\n" + err);
    edit("Load Audio", [&](Clip& c) {
        AudioTrack a = c.audio.value_or(AudioTrack{});
        a.path = path;
        c.audio = a;
    });
    audio_data_ = std::move(data);
    audio_loaded_path_ = path;
    char secs[32];
    std::snprintf(secs, sizeof secs, " (%.1f s)", audio_data_.seconds());
    status("Loaded " + path.substr(path.find_last_of("/\\") + 1) + secs);
}

// Keeps the decoded samples in step with the clip (undo, open and new change the path under us).
void App::sync_audio_data() {
    const Clip& c = doc_.clip();
    const std::string want = c.audio ? c.audio->path : std::string();
    if (want == audio_loaded_path_) return;
    stop_audio();
    audio_loaded_path_ = want;
    audio_data_ = {};
    if (want.empty()) return;
    std::string err;
    if (!load_audio_file(want, audio_data_, err)) status("Audio not found: " + want);
}

// --- Playback --------------------------------------------------------------------------------------

// Queues audio from the timeline frame on, for `seconds` (<= 0: to the end of the file).
void App::queue_audio_at(double frame, double seconds) {
    const Clip& c = doc_.clip();
    if (!c.audio || audio_data_.frames() == 0 || !host_.audio_start(audio_data_.rate, audio_data_.channels, float(c.audio->volume)))
        return;
    const double t = frame / std::max(c.fps, 1) - c.audio->offset;  // seconds into the file
    const int ch = audio_data_.channels;
    const double total = seconds > 0 ? seconds : 1e9;
    double lead = 0;
    if (t < 0) {  // the audio starts later on the timeline: silence first
        lead = std::min(-t, total);
        std::vector<float> zeros(size_t(lead * audio_data_.rate) * ch, 0.f);
        if (!zeros.empty()) host_.audio_queue(zeros.data(), zeros.size());
    }
    const size_t first = size_t(std::max(t, 0.0) * audio_data_.rate);
    if (first >= audio_data_.frames() || total - lead <= 0) return;
    const size_t count = std::min(audio_data_.frames() - first, size_t((total - lead) * audio_data_.rate));
    host_.audio_queue(audio_data_.pcm.data() + first * ch, count * ch);
}

void App::stop_audio() {
    host_.audio_stop();
    audio_running_ = false;
}

// Called once a frame after the playhead moved: playback follows the playhead, loops and jumps restart
// the stream, and a scrub while stopped plays a short snippet (AU-3).
void App::update_audio() {
    sync_audio_data();
    const Clip& c = doc_.clip();
    if (!c.audio || audio_data_.frames() == 0) return;
    const double fps = std::max(c.fps, 1);
    if (playing_) {
        const bool jumped = frame_ < audio_frame_ - 0.5 || frame_ > audio_frame_ + fps * 0.25;
        if (!audio_running_ || jumped) queue_audio_at(frame_, 0), audio_running_ = true;
    } else if (audio_running_) {
        stop_audio();
    } else if (std::fabs(frame_ - audio_frame_) >= 0.5) {
        queue_audio_at(frame_, 0.12);  // scrub snippet
    }
    audio_frame_ = frame_;
}

double App::snapped_frame(double frame) const { return beat_snapped_frame(doc_.clip(), frame); }

// --- Timeline lane ---------------------------------------------------------------------------------

// The waveform under the key rows and the beat grid (AU-2). x_of maps a timeline frame to a pixel. The grid shows
// whenever the track has a BPM or beats, even while its sound is not loaded (a missing file).
void App::draw_audio_lane(ImDrawList* dl, float x0, float x1, float y0, float y1, double last_frame) {
    const Clip& c = doc_.clip();
    if (!c.audio) return;
    const double fps = std::max(c.fps, 1);
    const double px_per_frame = (x1 - x0) / std::max(last_frame, 1.0);
    draw_beat_grid(dl, c, [&](double f) { return float(x0 + f * px_per_frame); }, 0, last_frame, y0, y1);
    if (audio_data_.frames() == 0) return;
    const float mid = (y0 + y1) / 2, half = (y1 - y0) * 0.42f;
    const double blocks_per_second = double(audio_data_.rate) / AudioData::kPeakBlock;
    const ImU32 wave = IM_COL32(120, 180, 230, 105);
    for (float x = x0; x < x1; x += 1) {
        const double f0 = (x - x0) / px_per_frame, f1 = (x + 1 - x0) / px_per_frame;
        const double t0 = f0 / fps - c.audio->offset, t1 = f1 / fps - c.audio->offset;
        if (t1 < 0 || t0 > audio_data_.seconds()) continue;
        size_t b0 = size_t(std::max(t0, 0.0) * blocks_per_second), b1 = size_t(std::max(t1, 0.0) * blocks_per_second) + 1;
        b1 = std::min(b1, audio_data_.peaks.size());
        float peak = 0;
        for (size_t b = b0; b < b1; ++b) peak = std::max(peak, audio_data_.peaks[b]);
        const float h = std::max(0.5f, std::min(1.f, peak * float(c.audio->volume)) * half);
        dl->AddLine(ImVec2(x, mid - h), ImVec2(x, mid + h), wave);
    }
}

void draw_beat_grid(ImDrawList* dl, const Clip& c, const std::function<float(double)>& x_of, double f0, double f1, float y0, float y1) {
    if (!c.audio) return;
    const double fps = std::max(c.fps, 1);
    const ImU32 beat = (accent_colour() & 0x00FFFFFF) | 0x90000000;
    for (double t : beat_times(*c.audio, std::max(f0, 0.0) / fps, f1 / fps)) {
        const float x = x_of(t * fps);
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), beat);
    }
}

// --- Time editing ----------------------------------------------------------------------------------

std::vector<std::string> App::time_edit_tracks() const {
    return selection_.empty() && handles_.empty() ? std::vector<std::string>{} : selected_tracks();
}

void App::run_time_edit(const char* label, const std::function<void(Clip&, const std::vector<std::string>&)>& change) {
    const std::vector<std::string> tracks = time_edit_tracks();
    edit(label, [&](Clip& c) { change(c, tracks); });
    sync_actor_timing(doc_.project);  // GR-3: every actor keeps the same length and loop
    status(std::string(label) + (tracks.empty() ? " (all bones)" : " (selected bones)"));
}

void App::draw_time_prompt() {
    if (!time_prompt_) return;
    const char* title = time_prompt_ == 1 ? "Insert Frames" : "Stretch Range";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    double a = 0, b = 0;
    const bool has_range = clip_range(a, b);
    if (time_prompt_ == 1)
        ImGui::Text("Insert empty frames at frame %d%s.", int(std::round(frame_)),
                    time_edit_tracks().empty() ? " on every bone" : " on the selected bones");
    else
        ImGui::Text("Frames %d to %d (%d frames) become:", int(a), int(b), int(b - a));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputInt("Frames", &time_prompt_value_);
    time_prompt_value_ = std::clamp(time_prompt_value_, 1, 3600);
    auto close = [&] {
        time_prompt_ = 0;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        const int n = time_prompt_value_, at = int(std::round(frame_));
        if (time_prompt_ == 1)
            run_time_edit("Insert Frames", [&](Clip& c, auto& t) { insert_time(c, at, n, t); });
        else if (has_range)
            run_time_edit("Stretch Range", [&](Clip& c, auto& t) { scale_time(c, int(a), int(b), n, t); });
        close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    ImGui::EndPopup();
}

void App::add_time_actions(const std::function<void(const char*, Action)>& add) {
    auto no_range = [this]() -> const char* {
        double a, b;
        return clip_range(a, b) ? nullptr : "Shift-drag a frame range on the timeline first";
    };
    add("insert_frames", {"Insert Frames...", 0, 0, false,
                          [this] {
                              time_prompt_ = 1;
                              time_prompt_value_ = 10;
                          },
                          {}});
    add("remove_range", {"Remove Range", 0, 0, false,
                         [this] {
                             double a, b;
                             if (!clip_range(a, b)) return;
                             run_time_edit("Remove Range", [&](Clip& c, auto& t) { remove_time(c, int(a), int(b), t); });
                             range_a_ = range_b_ = -1;
                         },
                         no_range});
    add("stretch_range", {"Stretch Range...", 0, 0, false,
                          [this] {
                              double a, b;
                              if (!clip_range(a, b)) return;
                              time_prompt_ = 2;
                              time_prompt_value_ = int(b - a);
                          },
                          no_range});
    add("copy_range", {"Copy Range", 0, 0, false,
                       [this] {
                           double a, b;
                           if (!clip_range(a, b)) return;
                           range_clipboard_ = copy_range(doc_.clip(), int(a), int(b), time_edit_tracks());
                           status("Copied frames " + std::to_string(int(a)) + " to " + std::to_string(int(b)));
                       },
                       no_range});
    auto paste = [this](bool insert, bool mirrored) {
        const int at = int(std::round(frame_));
        const char* label = insert ? "Paste Range, Inserting" : mirrored ? "Paste Range Mirrored" : "Paste Range";
        edit(label, [&](Clip& c) { paste_range(c, range_clipboard_, at, insert, mirrored ? &skel_ : nullptr); });
        sync_actor_timing(doc_.project);
        status(std::string(label) + " at frame " + std::to_string(at));
    };
    auto nothing = [this]() -> const char* { return range_clipboard_.empty() ? "Copy a range first" : nullptr; };
    add("paste_range", {"Paste Range", 0, 0, false, [paste] { paste(false, false); }, nothing});
    add("paste_range_insert", {"Paste Range, Inserting", 0, 0, false, [paste] { paste(true, false); }, nothing});
    add("paste_range_mirrored", {"Paste Range Mirrored", 0, 0, false, [paste] { paste(false, true); }, nothing});
    // Audio (AU).
    add("load_audio", {"Load Audio...", 0, 0, false, [this] { show_dialog(Dialog::LoadAudio); }, {}});
    auto no_audio = [this]() -> const char* { return doc_.clip().audio ? nullptr : "Load an audio file first"; };
    add("remove_audio", {"Remove Audio", 0, 0, false,
                         [this] { edit("Remove Audio", [](Clip& c) { c.audio.reset(); }); }, no_audio});
    add("tap_beat", {"Mark a Beat Here", ImGuiKey_B, 0, false,
                     [this] {
                         const double t = frame_ / std::max(doc_.clip().fps, 1) - doc_.clip().audio->offset;
                         edit("Mark Beat", [&](Clip& c) {
                             auto& beats = c.audio->beats;
                             beats.insert(std::upper_bound(beats.begin(), beats.end(), t), t);
                         });
                     },
                     no_audio});
    add("clear_beats", {"Clear Marked Beats", 0, 0, false,
                        [this] { edit("Clear Beats", [](Clip& c) { c.audio->beats.clear(); }); },
                        [this]() -> const char* {
                            return doc_.clip().audio && !doc_.clip().audio->beats.empty() ? nullptr : "No beats are marked";
                        }});
}

void App::draw_time_menu_items() {
    if (menu_item_icon(icon::kRetime, "Retime Markers", nullptr, retime_on_)) set_retime(!retime_on_);  // TE-5 (retime_ui.cpp)
    ImGui::SetItemTooltip("Double-click the ruler to drop a marker; drag a marker to retime the keys around it");
    ImGui::Separator();
    for (const char* id : {"insert_frames", "remove_range", "stretch_range"}) menu_item(id);
    ImGui::Separator();
    for (const char* id : {"copy_range", "paste_range", "paste_range_insert", "paste_range_mirrored"}) menu_item(id);
}

// The audio settings shown in the timeline's right-click menu; each change is one undo step.
void App::draw_audio_menu_items() {
    menu_item("load_audio");
    Clip& c = doc_.clip();
    if (!c.audio) return;
    menu_item("remove_audio");
    ImGui::Separator();
    auto track = [&](const char* step) {
        if (ImGui::IsItemActivated()) doc_.history.begin(c);
        if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, c)) mark_dirty();
    };
    AudioTrack& a = *c.audio;
    float vol = float(a.volume), bpm = float(a.bpm), off = float(a.offset);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (slider_float("Volume", &vol, 0.f, 2.f, "%.2f")) a.volume = vol;
    track("Audio Volume");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::DragFloat("Start (s)", &off, 0.01f, -600.f, 600.f, "%.2f")) a.offset = off;
    ImGui::SetItemTooltip("Where the audio starts on the timeline. Ctrl+drag the timeline to slide it.");
    track("Move Audio");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::DragFloat("BPM", &bpm, 0.1f, 0.f, 999.f, bpm > 0 ? "%.1f" : "off")) a.bpm = std::max(0.f, bpm);
    ImGui::SetItemTooltip("A beat grid every 60/BPM seconds; 0 turns it off.");
    track("Audio BPM");
    if (ImGui::MenuItem("Beat Grid Starts Here", nullptr, false, a.bpm > 0))
        edit("Beat Grid Start", [&](Clip& cc) { cc.audio->beat_offset = frame_ / std::max(cc.fps, 1) - cc.audio->offset; });
    menu_item("tap_beat");
    menu_item("clear_beats");
    bool snap = a.snap;
    if (ImGui::MenuItem("Snap to Beats", nullptr, &snap)) edit("Snap to Beats", [&](Clip& cc) { cc.audio->snap = snap; });
}

}  // namespace vats
