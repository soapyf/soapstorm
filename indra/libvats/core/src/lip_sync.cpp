// Viewport Avatar Toolset - lip sync: mouth shapes from the audio track or from Rhubarb Lip Sync.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/lip_sync.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include "from_chars_compat.h"
#include <complex>
#include <set>

#include "vats/edit.h"
#include "vats/face_anim.h"
#include "vats/json.h"

namespace vats {

namespace {

// What one frame's weights add to each mouth bone: Euler degrees and, with positions, metres.
struct Delta {
    Vec3 rot, pos;
    bool has_rot = false, has_pos = false;
};

std::map<std::string, Delta> deltas(const FaceTable& table, const std::map<std::string, double>& w, bool positions,
                                    double scale) {
    std::map<std::string, Delta> out;
    if (w.empty()) return out;
    Clip c;
    static const std::map<std::string, double> none;
    key_face_weights(c, table, w, positions, 0, &none, scale);  // keys only the bones the weights move, by their move
    for (auto& [bone, track] : c.curves) {
        Delta& d = out[bone];
        d.has_rot = c.has_channels(bone, kRotChannels);
        d.has_pos = c.has_channels(bone, kPosChannels);
        if (d.has_rot) d.rot = curve_euler(c, bone, 0);
        if (d.has_pos) d.pos = curve_offset(c, bone, 0);
    }
    return out;
}

// Adds (sign 1) or takes back (sign -1) ls's moves on its frames.
void add_moves(Clip& clip, const FaceTable& table, const LipShapes& shapes, const LipSync& ls, double sign) {
    for (int f = std::max(ls.from, 0); f <= std::min(ls.to, clip.end_frame); ++f)  // a file's or a shortened clip's
        for (auto& [bone, d] : deltas(table, lip_weights(ls, shapes, f), ls.positions, ls.scale)) {
            if (d.has_rot) key_euler(clip, bone, f, curve_euler(clip, bone, f) + d.rot * sign);
            if (d.has_pos) key_offset(clip, bone, f, curve_offset(clip, bone, f) + d.pos * sign);
        }
}

}  // namespace

bool parse_lip_shapes(std::string_view text, LipShapes& out, std::string& err) {
    Json doc;
    if (!parse_json(text, doc, err)) return false;
    const Json* format = doc.find("format");
    if (!format || !format->is_string() || format->str != "vats-lip-shapes") return err = "not a lip-shapes table", false;
    const Json* shapes = doc.find("shapes");
    if (!shapes || !shapes->is_object()) return err = "no \"shapes\"", false;
    out = {};
    for (auto& [name, weights] : shapes->obj) {
        if (!weights.is_object()) return err = "shapes." + name + " is not an object", false;
        auto& w = out.shapes[name];
        for (auto& [shape, v] : weights.obj) {
            if (!v.is_number() || !std::isfinite(v.num)) return err = "shapes." + name + "." + shape + " is not a number", false;
            w[shape] = std::clamp(v.num, 0.0, 1.0);
        }
    }
    return true;
}

// --- Tier 1 --------------------------------------------------------------------------------------------------

// Rabiner & Schafer (1978) section 8.3.2: the autocorrelation method's normal equations solved by Levinson-Durbin
// recursion, A(z) = 1 + sum a_i z^-i.
std::vector<double> lpc(const std::vector<double>& x, int order) {
    std::vector<double> a(size_t(std::max(order, 0)) + 1, 0.0), r(a.size(), 0.0);
    a[0] = 1;
    for (size_t k = 0; k < r.size(); ++k)
        for (size_t n = k; n < x.size(); ++n) r[k] += x[n] * x[n - k];
    double e = r[0];
    if (!(e > 0)) return a;
    for (size_t i = 1; i < a.size(); ++i) {
        double acc = r[i];
        for (size_t j = 1; j < i; ++j) acc += a[j] * r[i - j];
        const double k = -acc / e;  // the reflection coefficient
        const std::vector<double> prev = a;
        for (size_t j = 1; j < i; ++j) a[j] = prev[j] + k * prev[i - j];
        a[i] = k;
        e *= 1 - k * k;
        if (!(e > 0)) break;
    }
    return a;
}

Formants lpc_formants(const std::vector<float>& mono, int rate, size_t at) {
    Formants out;
    if (rate <= 0 || mono.empty()) return out;
    // To about 10 kHz: the first two formants lie below 3 kHz, and a low order keeps the fit on the envelope.
    // ponytail: a box average as the anti-alias filter; speech has little energy above 5 kHz.
    const int step = std::max(1, rate / 10000);
    const double rd = double(rate) / step;
    const int n = int(0.03 * rd);
    std::vector<double> x(size_t(std::max(n, 2)), 0.0);
    for (int k = 0; k < int(x.size()); ++k) {
        const long long i0 = (long long)at + (long long)(k - int(x.size()) / 2) * step;
        double s = 0;
        for (int j = 0; j < step; ++j)
            if (long long i = i0 + j; i >= 0 && i < (long long)mono.size()) s += mono[size_t(i)];
        x[size_t(k)] = s / step;
    }
    for (size_t k = x.size() - 1; k > 0; --k) x[k] -= 0.97 * x[k - 1];  // pre-emphasis
    for (size_t k = 0; k < x.size(); ++k) x[k] *= 0.54 - 0.46 * std::cos(2 * kPi * double(k) / double(x.size() - 1));
    const std::vector<double> a = lpc(x, int(rd / 1000) + 2);
    // Peaks of 1/|A(e^jw)|^2 every 10 Hz up to 4 kHz.
    auto power = [&](double hz) {
        std::complex<double> s = 0;
        const double w = 2 * kPi * hz / rd;
        for (size_t i = 0; i < a.size(); ++i) s += a[i] * std::polar(1.0, -w * double(i));
        return 1 / std::max(std::norm(s), 1e-30);
    };
    const double top = std::min(4000.0, rd / 2 - 20);
    double p0 = power(90), p1 = power(100);
    for (double hz = 110; hz <= top; hz += 10) {
        const double p2 = power(hz);
        if (p1 > p0 && p1 >= p2 && hz - 10 > 150) {
            if (out.f1 == 0) out.f1 = hz - 10;
            else {
                out.f2 = hz - 10;
                break;
            }
        }
        p0 = p1, p1 = p2;
    }
    return out;
}

// ponytail: fixed thresholds from adult vowel averages (Peterson & Barney 1952); children's and many women's
// formants run higher, so their /o/ can read open and their /a/ wide.
std::string vowel_class(const Formants& f) {
    if (f.f2 >= 1600) return "wide";
    if (f.f1 >= 600 || f.f1 == 0) return "open";
    return "rounded";
}

LipSync lip_sync_from_audio(const AudioData& audio, double audio_offset, int fps, int end_frame,
                            const LipAudioOptions& opt) {
    LipSync ls;
    fps = std::max(fps, 1);
    ls.from = std::clamp(opt.from, 0, std::max(end_frame, 0));
    ls.to = opt.to < 0 ? end_frame : std::clamp(opt.to, ls.from, end_frame);
    const size_t frames = audio.frames();
    const int ch = std::max(audio.channels, 1);
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0;
        for (int c = 0; c < ch; ++c) s += audio.pcm[i * size_t(ch) + size_t(c)];
        mono[i] = s / float(ch);
    }
    // Loudness: RMS over 20 ms windows, in dB below the loudest.
    const size_t win = std::max<size_t>(1, size_t(std::lround(0.02 * audio.rate)));
    std::vector<double> rms(frames / win + 1, 0.0);
    double loudest = 0;
    for (size_t w = 0; w < rms.size(); ++w) {
        double sum = 0;
        size_t n = 0;
        for (size_t i = w * win; i < std::min(frames, (w + 1) * win); ++i, ++n) sum += double(mono[i]) * mono[i];
        rms[w] = n ? std::sqrt(sum / double(n)) : 0;
        loudest = std::max(loudest, rms[w]);
    }
    const double range = std::max(opt.range_db, 1.0);
    std::vector<std::string> cls;
    for (int f = ls.from; f <= ls.to; ++f) {
        const double t = double(f) / fps - audio_offset;
        double level = 0;
        if (audio.rate > 0 && t >= 0 && t < audio.seconds() && loudest > 0) {
            const double r = rms[std::min(rms.size() - 1, size_t(t * audio.rate) / win)];
            level = r > 0 ? std::clamp(1 + 20 * std::log10(r / loudest) / range, 0.0, 1.0) : 0;
        }
        if (level < 0.05) level = 0;
        ls.level.push_back(level);
        cls.push_back(level > 0 ? vowel_class(lpc_formants(mono, audio.rate, size_t(t * audio.rate))) : "X");
    }
    // A class that lasts one frame is taken as noise in the formants: it keeps the one before.
    for (size_t i = 1; i + 1 < cls.size(); ++i)
        if (cls[i] != cls[i - 1] && cls[i] != cls[i + 1]) cls[i] = cls[i - 1];
    for (size_t i = 0; i < cls.size(); ++i)
        if (i == 0 || cls[i] != cls[i - 1]) ls.cues.push_back({ls.from + int(i), cls[i]});
    return ls;
}

// --- Tier 2 --------------------------------------------------------------------------------------------------

bool parse_rhubarb(std::string_view text, std::vector<RhubarbCue>& out, std::string& err) {
    out.clear();
    auto valid = [](const std::string& s) { return s.size() == 1 && std::string_view("ABCDEFGHX").find(s[0]) != std::string_view::npos; };
    const size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first == std::string_view::npos) return err = "the file is empty", false;
    if (text[first] == '{') {
        Json doc;
        if (!parse_json(text, doc, err)) return false;
        const Json* cues = doc.find("mouthCues");
        if (!cues || !cues->is_array()) return err = "no \"mouthCues\": not Rhubarb's JSON", false;
        for (const Json& c : cues->arr) {
            const Json* s = c.find("start");
            const Json* v = c.find("value");
            if (!s || !s->is_number() || !std::isfinite(s->num) || !v || !v->is_string() || !valid(v->str))
                return err = "a mouth cue needs a start time and a shape A-H or X", false;
            out.push_back({s->num, v->str});
        }
    } else {
        size_t line_no = 0;
        for (size_t pos = 0; pos < text.size();) {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(pos, end - pos);
            pos = end + 1, ++line_no;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
            if (line.find_first_not_of(" \t\xEF\xBB\xBF") == std::string_view::npos) continue;
            line.remove_prefix(line.find_first_not_of(" \t\xEF\xBB\xBF"));
            double t = 0;
            auto r = from_chars(line.data(), line.data() + line.size(), t);
            std::string_view rest(r.ptr, size_t(line.data() + line.size() - r.ptr));
            const size_t s = rest.find_first_not_of(" \t");
            const std::string shape(s == std::string_view::npos ? std::string_view{} : rest.substr(s));
            if (r.ec != std::errc() || !std::isfinite(t) || !valid(shape))
                return err = "line " + std::to_string(line_no) + " is not \"<seconds><Tab><shape A-H or X>\"", false;
            out.push_back({t, shape});
        }
    }
    if (out.empty()) return err = "no mouth cues", false;
    std::stable_sort(out.begin(), out.end(), [](const RhubarbCue& a, const RhubarbCue& b) { return a.start < b.start; });
    return true;
}

LipSync lip_sync_from_cues(const std::vector<RhubarbCue>& cues, double audio_offset, int fps, int end_frame, int from,
                           int to) {
    LipSync ls;
    fps = std::max(fps, 1);
    ls.from = std::clamp(from, 0, std::max(end_frame, 0));
    ls.to = to < 0 ? end_frame : std::clamp(to, ls.from, end_frame);
    std::string running = "X";
    for (const RhubarbCue& c : cues) {
        const int f = int(std::floor((c.start + audio_offset) * fps + 0.5));
        if (f > ls.to) break;
        if (f <= ls.from) {
            running = c.shape;
            continue;
        }
        if (!ls.cues.empty() && ls.cues.back().frame == f) ls.cues.pop_back();  // two in one frame: the later wins
        ls.cues.push_back({f, c.shape});
    }
    ls.cues.insert(ls.cues.begin(), {ls.from, running});
    // Neighbours with the same shape are one cue.
    ls.cues.erase(std::unique(ls.cues.begin(), ls.cues.end(),
                              [](const LipSync::Cue& a, const LipSync::Cue& b) { return a.shape == b.shape; }),
                  ls.cues.end());
    return ls;
}

// --- Keying --------------------------------------------------------------------------------------------------

std::map<std::string, double> lip_weights(const LipSync& ls, const LipShapes& shapes, int frame) {
    std::map<std::string, double> w;
    if (frame < ls.from || frame > ls.to || ls.cues.empty()) return w;
    auto it = std::upper_bound(ls.cues.begin(), ls.cues.end(), frame,
                               [](int f, const LipSync::Cue& c) { return f < c.frame; });
    if (it == ls.cues.begin()) return w;
    auto s = shapes.shapes.find(std::prev(it)->shape);
    if (s == shapes.shapes.end()) return w;
    const size_t i = size_t(frame - ls.from);
    const double level = ls.level.empty() ? 1 : i < ls.level.size() ? ls.level[i] : 0;
    for (auto& [shape, v] : s->second)
        if (v * level > 0) w[shape] = v * level;
    return w;
}

void apply_lip_sync(Clip& clip, const FaceTable& table, const LipShapes& shapes, const LipSync& ls_in) {
    if (clip.lip_sync) add_moves(clip, table, shapes, *clip.lip_sync, -1);
    LipSync ls = ls_in;
    ls.from = std::clamp(ls.from, 0, std::max(clip.end_frame, 0));
    ls.to = std::clamp(ls.to, ls.from, std::max(clip.end_frame, 0));
    // Every bone some frame moves gets a key on every frame of the range and, as it was, just outside it, so the
    // frames around the range keep their motion.
    std::set<std::string> rot, pos;
    for (auto& [name, s] : shapes.shapes)
        for (auto& [bone, d] : deltas(table, s, ls.positions, ls.scale)) {
            if (d.has_rot) rot.insert(bone);
            if (d.has_pos) pos.insert(bone);
        }
    const Clip before = clip;
    for (int f : {ls.from - 1, ls.to + 1})
        if (f >= 0 && f <= clip.end_frame) {
            for (auto& b : rot) key_euler(clip, b, f, curve_euler(before, b, f));
            for (auto& b : pos) key_offset(clip, b, f, curve_offset(before, b, f));
        }
    for (int f = ls.from; f <= ls.to; ++f) {
        for (auto& b : rot) key_euler(clip, b, f, curve_euler(before, b, f));
        for (auto& b : pos) key_offset(clip, b, f, curve_offset(before, b, f));
    }
    add_moves(clip, table, shapes, ls, 1);
    clip.lip_sync = std::move(ls);
}

void remove_lip_sync(Clip& clip, const FaceTable& table, const LipShapes& shapes) {
    if (!clip.lip_sync) return;
    add_moves(clip, table, shapes, *clip.lip_sync, -1);
    clip.lip_sync.reset();
}

}  // namespace vats
