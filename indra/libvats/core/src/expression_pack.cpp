// Viewport Avatar Toolset - expression packs: one short face-only .anim per expression.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/expression_pack.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "vats/edit.h"
#include "vats/face_anim.h"

namespace vats {

std::vector<Expression> starter_expressions() {
    using M = Expression::Motion;
    auto both = [](std::map<std::string, double> w, const char* base, double v) {
        w[std::string(base) + "Left"] = v, w[std::string(base) + "Right"] = v;
        return w;
    };
    std::map<std::string, double> smile = both(both({}, "mouthSmile", 0.6), "cheekSquint", 0.2);
    std::map<std::string, double> big = both(both(both(both({{"jawOpen", 0.15}}, "mouthSmile", 1), "cheekSquint", 0.5),
                                                  "eyeSquint", 0.3), "mouthUpperUp", 0.3);
    std::map<std::string, double> frown = both(both({}, "mouthFrown", 0.8), "browDown", 0.4);
    std::map<std::string, double> surprise = both(both({{"browInnerUp", 1}, {"jawOpen", 0.35}}, "browOuterUp", 1), "eyeWide", 0.8);
    std::map<std::string, double> angry = both(both(both(both({}, "browDown", 1), "eyeSquint", 0.4), "noseSneer", 0.5),
                                               "mouthFrown", 0.4);
    std::map<std::string, double> sad = both({{"browInnerUp", 0.9}, {"mouthShrugLower", 0.3}}, "mouthFrown", 0.7);
    return {
        {"smile", smile, {}, {}, M::Hold},
        {"big smile", big, {}, {}, M::Hold},
        {"frown", frown, {}, {}, M::Hold},
        {"surprise", surprise, {}, {}, M::Hold},
        {"wink L", {{"eyeBlinkLeft", 1}, {"cheekSquintLeft", 0.4}, {"mouthSmileLeft", 0.3}}, {}, {}, M::Hold},
        {"wink R", {{"eyeBlinkRight", 1}, {"cheekSquintRight", 0.4}, {"mouthSmileRight", 0.3}}, {}, {}, M::Hold},
        {"angry", angry, {}, {}, M::Hold},
        {"sad", sad, {}, {}, M::Hold},
        {"blink loop", {{"eyeBlinkLeft", 1}, {"eyeBlinkRight", 1}}, {}, {}, M::Blink},
        {"idle breathing", both({{"jawOpen", 0.06}, {"cheekPuff", 0.1}}, "noseSneer", 0.15), {}, {}, M::Breathe},
        {"kiss", both({{"mouthPucker", 1}, {"mouthFunnel", 0.3}}, "eyeSquint", 0.2), {}, {}, M::Hold},
        {"tongue out", {{"tongueOut", 1}, {"jawOpen", 0.35}}, {}, {}, M::Hold},
    };
}

Expression face_pose_expression(const LibraryItem& pose) {
    Expression e;
    e.name = pose.name.empty() ? "face" : pose.name;
    e.bones = pose.bones;
    e.offsets = pose.offsets;
    return e;
}

std::string expression_anim_name(const std::string& prefix, const std::string& name) {
    // Letters and digits, anything else as one "_"; lower case for the name, the prefix as typed.
    auto clean = [](const std::string& s, bool lower) {
        std::string out;
        bool gap = false;
        for (unsigned char c : s) {
            if (!std::isalnum(c)) {
                gap = true;
                continue;
            }
            if (gap && !out.empty()) out += '_';
            out += char(lower ? std::tolower(c) : c);
            gap = false;
        }
        return out;
    };
    const std::string p = clean(prefix, false), n = clean(name, true);
    return (p.empty() ? n : n.empty() ? p : p + "_" + n).substr(0, 63);
}

Clip expression_clip(const FaceTable& table, const Expression& e, const ExpressionPackOptions& opt) {
    using M = Expression::Motion;
    Clip clip;
    clip.fps = std::max(opt.fps, 1);
    clip.priority = std::clamp(opt.priority, 0, 6);
    clip.ease_in = std::max(opt.ease_in, 0.0), clip.ease_out = std::max(opt.ease_out, 0.0);
    // The face at full weight, as rotations and offsets from rest.
    std::map<std::string, Vec3> rot, pos;
    if (!e.bones.empty() || !e.offsets.empty()) {
        for (auto& [b, v] : e.bones)
            if (v.length() > 0.01) rot[b] = v;
        if (opt.positions)
            for (auto& [b, v] : e.offsets)
                if (v.length() > 1e-5) pos[b] = v;
    } else {
        Clip c;
        static const std::map<std::string, double> none;
        key_face_weights(c, table, e.weights, opt.positions, 0, &none, opt.scale);  // only the bones the weights move
        for (auto& [b, track] : c.curves) {
            if (c.has_channels(b, kRotChannels)) rot[b] = curve_euler(c, b, 0);
            if (c.has_channels(b, kPosChannels)) pos[b] = curve_offset(c, b, 0);
        }
    }
    // (seconds, share of the face) keys.
    std::vector<std::pair<double, double>> keys;
    double seconds = std::max(opt.length, 0.2);
    switch (e.motion) {
        case M::Hold: keys = {{0, 1}, {seconds, 1}}; break;
        case M::Blink:  // one blink in 4 s: shut in 80 ms, held 40 ms, open in 130 ms
            seconds = 4, keys = {{0, 0}, {2, 0}, {2.08, 1}, {2.12, 1}, {2.25, 0}, {4, 0}};
            break;
        case M::Breathe: seconds = 4, keys = {{0, 0}, {2, 1}, {4, 0}}; break;
    }
    clip.end_frame = std::max(2, int(std::lround(seconds * clip.fps)));
    clip.loop = opt.loop || e.motion != M::Hold;
    clip.loop_in = 0, clip.loop_out = clip.end_frame;
    int last = -1;
    for (auto& [t, s] : keys) {
        const int f = std::max(last + 1, int(std::lround(t * clip.fps)));
        last = f;
        for (auto& [b, v] : rot) key_euler(clip, b, f, v * s);
        for (auto& [b, v] : pos) key_offset(clip, b, f, v * s);
    }
    return clip;
}

std::vector<PackFile> expression_pack(const FaceTable& table, const std::vector<Expression>& list,
                                      const ExpressionPackOptions& opt, std::vector<std::string>* skipped) {
    std::vector<PackFile> out;
    for (const Expression& e : list) {
        Clip c = expression_clip(table, e, opt);
        if (c.curves.empty()) {
            if (skipped) skipped->push_back(e.name);
            continue;
        }
        std::string name = expression_anim_name(opt.prefix, e.name);  // a face pose named like a starter: "_2"
        for (int k = 2; std::any_of(out.begin(), out.end(), [&](const PackFile& f) { return f.name == name; }); ++k)
            name = expression_anim_name(opt.prefix, e.name + " " + std::to_string(k));
        out.push_back({std::move(name), std::move(c)});
    }
    return out;
}

}  // namespace vats
