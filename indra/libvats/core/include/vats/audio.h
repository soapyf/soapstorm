// Viewport Avatar Toolset - the audio track: decoding and beat maths (spec 08 AU).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Decoding uses the vendored public-domain dr_wav, dr_mp3, dr_flac and stb_vorbis. Playback is the host's
// job (SDL in the app, the viewer's audio engine in the viewer); the core only hands over the samples.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

struct AudioData {
    int rate = 0, channels = 0;
    std::vector<float> pcm;    // interleaved, -1..1
    std::vector<float> peaks;  // mono max |sample| per kPeakBlock frames, for the waveform
    static constexpr int kPeakBlock = 256;

    size_t frames() const { return channels ? pcm.size() / channels : 0; }
    double seconds() const { return rate ? double(frames()) / rate : 0; }
};

// WAV, MP3, FLAC or Ogg Vorbis, told apart by their first bytes. On failure returns false and sets err.
bool decode_audio(const std::vector<std::uint8_t>& bytes, AudioData& out, std::string& err);
bool load_audio_file(const std::string& path, AudioData& out, std::string& err);

// Beat times in timeline seconds within [from, to]: the BPM grid and the tapped markers, sorted.
std::vector<double> beat_times(const AudioTrack& a, double from, double to);
// The nearest beat to t within tolerance seconds, else t.
double snap_to_beat(const AudioTrack& a, double t, double tolerance);
// A timeline frame on the nearest beat within 3 frames while the clip's audio has Snap to Beats on (AU-2), else
// unchanged: scrubbing, range picks and the dope sheet's scale handle.
double beat_snapped_frame(const Clip& c, double frame);

}  // namespace vats
