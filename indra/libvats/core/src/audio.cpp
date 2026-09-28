// Viewport Avatar Toolset - the audio track: decoding and beat maths (spec 08 AU).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

extern "C" {
#include "dr_libs/dr_flac.h"
#include "dr_libs/dr_mp3.h"
#include "dr_libs/dr_wav.h"
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);
}

namespace vats {
namespace {

constexpr size_t kMaxSamples = size_t(1) << 28;  // 1 GB of float samples: a guard against hostile headers

bool starts_with(const std::vector<std::uint8_t>& b, const char* magic, size_t at = 0) {
    const size_t n = std::strlen(magic);
    return b.size() >= at + n && std::memcmp(b.data() + at, magic, n) == 0;
}

template <class F>
bool take(float* data, std::uint64_t frames, unsigned channels, unsigned rate, AudioData& out, F free_fn) {
    if (!data) return false;
    const bool ok = channels > 0 && channels <= 16 && rate >= 1000 && rate <= 384000 && frames * channels <= kMaxSamples;
    if (ok) {
        out.rate = int(rate), out.channels = int(channels);
        out.pcm.assign(data, data + frames * channels);
    }
    free_fn(data);
    return ok;
}

}  // namespace

bool decode_audio(const std::vector<std::uint8_t>& b, AudioData& out, std::string& err) {
    AudioData d;
    bool ok = false;
    unsigned ch = 0, rate = 0;
    if (starts_with(b, "RIFF") && starts_with(b, "WAVE", 8)) {
        drwav_uint64 n = 0;
        float* p = drwav_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &ch, &rate, &n, nullptr);
        ok = take(p, n, ch, rate, d, [](float* x) { drwav_free(x, nullptr); });
    } else if (starts_with(b, "fLaC")) {
        drflac_uint64 n = 0;
        float* p = drflac_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &ch, &rate, &n, nullptr);
        ok = take(p, n, ch, rate, d, [](float* x) { drflac_free(x, nullptr); });
    } else if (starts_with(b, "OggS")) {
        int c = 0, r = 0;
        short* s = nullptr;
        if (b.size() <= size_t(INT32_MAX)) {
            int n = stb_vorbis_decode_memory(b.data(), int(b.size()), &c, &r, &s);
            if (s && n > 0 && c > 0 && c <= 16 && r >= 1000 && size_t(n) * c <= kMaxSamples) {
                d.rate = r, d.channels = c;
                d.pcm.resize(size_t(n) * c);
                for (size_t i = 0; i < d.pcm.size(); ++i) d.pcm[i] = s[i] / 32768.f;
                ok = true;
            }
        }
        std::free(s);
    } else {
        drmp3_config cfg{};
        drmp3_uint64 n = 0;
        float* p = drmp3_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &cfg, &n, nullptr);
        ok = take(p, n, cfg.channels, cfg.sampleRate, d, [](float* x) { drmp3_free(x, nullptr); });
    }
    if (!ok || d.frames() == 0) {
        err = "not a readable WAV, MP3, FLAC or Ogg Vorbis file";
        return false;
    }
    const size_t frames = d.frames();
    d.peaks.resize((frames + AudioData::kPeakBlock - 1) / AudioData::kPeakBlock);
    for (size_t f = 0; f < frames; ++f) {
        float m = 0;
        for (int c = 0; c < d.channels; ++c) m += d.pcm[f * d.channels + c];
        float& peak = d.peaks[f / AudioData::kPeakBlock];
        peak = std::max(peak, std::fabs(m / d.channels));
    }
    out = std::move(d);
    return true;
}

bool load_audio_file(const std::string& path, AudioData& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return err = "cannot open " + path, false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > (std::streamoff(1) << 30)) return err = "the file is empty or larger than 1 GB", false;
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return decode_audio(bytes, out, err);
}

std::vector<double> beat_times(const AudioTrack& a, double from, double to) {
    std::vector<double> out;
    const double first = a.offset + a.beat_offset;  // the grid, on the timeline
    if (a.bpm > 0 && std::isfinite(a.bpm) && to > from) {
        const double step = 60.0 / a.bpm, lo = std::max(from, a.offset);  // no grid before the music starts
        double t = first + std::ceil((lo - first) / step) * step;
        for (int guard = 0; t <= to && guard < 100000; ++guard, t += step) out.push_back(t);
    }
    for (double b : a.beats)
        if (double t = a.offset + b; t >= from && t <= to) out.push_back(t);
    std::sort(out.begin(), out.end());
    return out;
}

double beat_snapped_frame(const Clip& c, double frame) {
    if (!c.audio || !c.audio->snap) return frame;
    const double fps = std::max(c.fps, 1);
    return std::round(snap_to_beat(*c.audio, frame / fps, 3.0 / fps) * fps);
}

double snap_to_beat(const AudioTrack& a, double t, double tolerance) {
    double best = t, gap = tolerance;
    for (double b : beat_times(a, t - tolerance, t + tolerance))
        if (std::fabs(b - t) <= gap) gap = std::fabs(b - t), best = b;
    return best;
}

}  // namespace vats
