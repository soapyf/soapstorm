// Viewport Avatar Toolset - motion clean-up filters: One-Euro, Savitzky-Golay, zero-phase Butterworth.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 3 (MC-4a). Pure functions on curve samples taken once per frame, used by the
// motion capture clean-up and the graph editor's Filter Curves. Rotation samples are Euler degrees, position
// samples metres (the clip's own units), so the One-Euro speed coefficient has one value for each.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

enum class FilterKind { OneEuro, SavitzkyGolay, Butterworth };

struct FilterSettings {
    FilterKind kind = FilterKind::OneEuro;
    // One-Euro (Casiez, Roussel and Vogel, CHI 2012): a one-pole low-pass whose cutoff rises with speed.
    double min_cutoff = 1.5;  // Hz, the cutoff when still
    double beta = 0.02;       // Hz per degree/s on rotation channels
    double beta_m = 10.0;     // Hz per metre/s on position channels (hips travel ~0.05 m where a limb swings ~30 degrees)
    double d_cutoff = 1.0;    // Hz, the low-pass on the speed estimate
    // Savitzky-Golay: a least-squares polynomial fitted over a sliding window.
    int sg_half = 3;   // window of 2 * sg_half + 1 frames
    int sg_order = 2;  // polynomial degree, below the window
    // Butterworth, run forward then backward (zero phase) as cascaded second-order sections.
    double cutoff = 6.0;  // Hz; kept below 0.45 x the frame rate
    int order = 2;        // per pass, even (one section per 2); the forward-backward pair doubles it

    bool operator==(const FilterSettings&) const = default;
};

const char* filter_name(FilterKind k);  // "One-Euro", "Savitzky-Golay", "Butterworth"

// Filters samples taken at fps. The ends are padded by odd reflection (the signal continued through its end
// value), or with loop the samples are one period (frames loop_in..loop_out - 1) and wrap: the sample after
// the last is the first plus loop_drift (a whole turn, or the hips' travel per cycle). position picks beta_m.
std::vector<double> filter_samples(const std::vector<double>& x, double fps, const FilterSettings& s,
                                   bool position = false, bool loop = false, double loop_drift = 0);

// Shake: the RMS of the third finite difference (jerk), in units per second cubed. With loop the
// differences wrap as in filter_samples. 0 for fewer than 4 samples.
double jerk_rms(const std::vector<double>& x, double fps, bool loop = false, double loop_drift = 0);

struct CurveId {
    std::string track, channel;
    bool operator==(const CurveId&) const = default;
};

// Filters the curves over whole frames from..to: each is sampled once per frame, filtered, and its keys in
// the range are replaced by one key per frame (keys just outside hold the curve's shape there). When the clip
// loops and from..to lies inside loop_in..loop_out, the whole loop is filtered as one period, so loop_out
// stays loop_in plus its drift; only from..to is written. pos_* and pole_* channels count as metres (beta_m).
// Curves that do not exist are skipped.
void filter_curves(Clip& clip, const std::vector<CurveId>& curves, int from, int to, const FilterSettings& s);

// Shake per track over from..to (looped as filter_curves does): rotation jerk as the length of the three
// channels' jerk vector (degrees/s^3), and the same for position (metres/s^3; 0 without position curves).
struct JointShake {
    std::string track;
    double rot = 0, pos = 0;
};
std::vector<JointShake> shake_scores(const Clip& clip, const std::vector<std::string>& tracks, int from, int to);

}  // namespace vats
