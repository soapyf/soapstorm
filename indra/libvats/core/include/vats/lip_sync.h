// Viewport Avatar Toolset - lip sync: mouth shapes from the audio track or from Rhubarb Lip Sync, keyed as ARKit
// shapes through the face table.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 25 (LS). Tier 1 reads the loaded audio: the loudness (RMS over 20 ms windows) opens
// the jaw, and the first two formants of an LPC fit (Levinson-Durbin, after Rabiner & Schafer, "Digital Processing
// of Speech Signals", 1978) choose an open, rounded or wide mouth. Tier 2 reads the mouth cues Rhubarb Lip Sync
// (github.com/DanielSWolf/rhubarb-lip-sync, MIT) writes, which the user runs; nothing of it is bundled. Both become
// a LipSync (Clip::lip_sync): mouth-shape cues on frames, which data/retarget/lip-shapes.json maps onto ARKit
// weights, keyed through key_face_weights.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "vats/audio.h"
#include "vats/clip.h"
#include "vats/facecap.h"

namespace vats {

// Mouth shape -> ARKit weights (or the table's VRM presets), from a "vats-lip-shapes" JSON.
struct LipShapes {
    std::map<std::string, std::map<std::string, double>> shapes;
};
bool parse_lip_shapes(std::string_view json, LipShapes& out, std::string& err);

// --- Tier 1: the audio ------------------------------------------------------------------------------------

// Linear prediction coefficients a[1..order] (a[0] = 1) of x by the autocorrelation method: Levinson-Durbin on
// the autocorrelation of x as given (window it first). All zero for silence.
std::vector<double> lpc(const std::vector<double>& x, int order);

struct Formants {
    double f1 = 0, f2 = 0;  // Hz; 0 = not found
};
// The first two peaks of the LPC spectrum of a 30 ms Hamming-windowed piece of mono centred on sample `at`,
// resampled to about 10 kHz first.
Formants lpc_formants(const std::vector<float>& mono, int rate, size_t at);

// "open", "rounded" or "wide" from the first two formants (adult vowel averages: /a/ 730/1090 Hz open, /u/
// 300/870 rounded, /i/ 270/2290 wide).
std::string vowel_class(const Formants& f);

struct LipAudioOptions {
    int from = 0, to = -1;  // frames; to = -1: the clip's end
    double range_db = 30;   // the loudest 20 ms window opens the mouth fully; this much quieter keeps it shut
};
// Tier 1: the loudness on each frame (LipSync::level) and a cue each time the vowel class changes, "X" (rest)
// where it is silent. audio_offset: where the audio starts on the timeline, seconds (AudioTrack::offset).
LipSync lip_sync_from_audio(const AudioData& audio, double audio_offset, int fps, int end_frame,
                            const LipAudioOptions& opt);

// --- Tier 2: Rhubarb Lip Sync -----------------------------------------------------------------------------

struct RhubarbCue {
    double start = 0;   // seconds into the audio
    std::string shape;  // A-H or X
};
// Rhubarb's JSON (-f json: {"mouthCues": [{"start", "end", "value"}]}) or TSV (-f tsv: "start<TAB>shape" lines).
// False with err on anything else.
bool parse_rhubarb(std::string_view text, std::vector<RhubarbCue>& out, std::string& err);

// The cues on the frames from..to (to = -1: end_frame): each on the frame nearest its start on the timeline;
// the one running at `from` starts there.
LipSync lip_sync_from_cues(const std::vector<RhubarbCue>& cues, double audio_offset, int fps, int end_frame, int from,
                           int to);

// --- Keying ------------------------------------------------------------------------------------------------

// The ARKit weights at frame: the running cue's shape times the frame's level.
std::map<std::string, double> lip_weights(const LipSync& ls, const LipShapes& shapes, int frame);

// Takes back clip.lip_sync's moves (if any), then adds ls's on every frame of its range to the mouth bones' curves
// (only the bones its shapes move, rotations and, with ls.positions, offsets), keeping the frames just outside
// the range as they were; stores ls in clip.lip_sync. One undo step for the caller.
void apply_lip_sync(Clip& clip, const FaceTable& table, const LipShapes& shapes, const LipSync& ls);
// Takes back clip.lip_sync's moves and removes it.
void remove_lip_sync(Clip& clip, const FaceTable& table, const LipShapes& shapes);

}  // namespace vats
