// Viewport Avatar Toolset - face tracking: VTuber blendshapes onto Bento face bones.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/facecap.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

#include "vats/edit.h"
#include "vats/json.h"
#include "vats/mocap.h"

namespace vats {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool read_vec(const Json* j, Vec3& v) {
    if (!j || !j->is_array() || j->arr.size() != 3) return false;
    for (const Json& e : j->arr)
        if (!e.is_number() || !std::isfinite(e.num)) return false;
    v = {j->arr[0].num, j->arr[1].num, j->arr[2].num};
    return true;
}

bool read_weights(const Json& j, std::map<std::string, double>& out) {
    if (!j.is_object()) return false;
    for (auto& [k, v] : j.obj) {
        if (!v.is_number() || !std::isfinite(v.num)) return false;
        out[k] = v.num;
    }
    return true;
}

// "#rx,ry,rz[,...]" after a field name: Unity Euler degrees (applied Z, then X, then Y) -> SL rotation.
Quat unity_euler_field(std::string_view rest) {
    double a[3] = {0, 0, 0};
    for (int i = 0; i < 3 && !rest.empty(); ++i) {
        const size_t c = rest.find(',');
        auto r = std::from_chars(rest.data(), rest.data() + (c == std::string_view::npos ? rest.size() : c), a[i]);
        if (r.ec != std::errc() || !std::isfinite(a[i])) a[i] = 0;
        rest = c == std::string_view::npos ? std::string_view{} : rest.substr(c + 1);
    }
    const double k = kPi / 360;
    const Quat qx{std::cos(a[0] * k), std::sin(a[0] * k), 0, 0}, qy{std::cos(a[1] * k), 0, std::sin(a[1] * k), 0},
        qz{std::cos(a[2] * k), 0, 0, std::sin(a[2] * k)};
    // The axis signs follow the Unity convention the format states; confirmed with a real iPhone (2026-09-27).
    return unity_to_sl(qy * qx * qz).normalized();
}

double to_double(std::string_view s) {
    double v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && std::isfinite(v) ? v : 0;
}

}  // namespace

std::vector<std::string> FaceTable::bones() const {
    std::set<std::string> b;
    for (auto& [shape, motions] : shapes)
        for (auto& m : motions) b.insert(m.bone);
    for (const Gaze& g : gaze) {
        b.insert(g.eyes.begin(), g.eyes.end());
        for (auto& [lid, k] : g.lids) b.insert(lid);
    }
    return {b.begin(), b.end()};
}

bool parse_face_table(std::string_view text, FaceTable& out, std::string& err) {
    Json doc;
    if (!parse_json(text, doc, err)) return false;
    const Json* format = doc.find("format");
    if (!format || !format->is_string() || format->str != "vats-face-table") return err = "not a face table", false;
    FaceTable t;
    if (const Json* n = doc.find("name"); n && n->is_string()) t.name = n->str;
    const Json* shapes = doc.find("shapes");
    if (!shapes || !shapes->is_object()) return err = "the face table has no shapes", false;
    for (auto& [shape, bones] : shapes->obj) {
        if (!bones.is_object()) return err = "shape " + shape + " is not an object", false;
        for (auto& [bone, motion] : bones.obj) {
            FaceTable::Motion m;
            m.bone = bone;
            m.has_rot = read_vec(motion.find("rot"), m.rot);
            m.has_pos = read_vec(motion.find("pos"), m.pos);
            if (!m.has_rot && !m.has_pos) return err = shape + "." + bone + " needs rot or pos [x, y, z]", false;
            t.shapes[shape].push_back(m);
        }
    }
    if (const Json* g = doc.find("gaze"); g && g->is_object())
        for (int side = 0; side < 2; ++side) {
            const Json* s = g->find(side ? "right" : "left");
            if (!s || !s->is_object()) continue;
            if (const Json* e = s->find("eyes"); e && e->is_array())
                for (const Json& b : e->arr)
                    if (b.is_string()) t.gaze[side].eyes.push_back(b.str);
            if (const Json* l = s->find("lids"); l && !read_weights(*l, t.gaze[side].lids))
                return err = "gaze lids need bone fractions", false;
        }
    if (const Json* a = doc.find("aliases"); a && a->is_object())
        for (auto& [name, w] : a->obj)
            if (!read_weights(w, t.aliases[lower(name)])) return err = "alias " + name + " needs shape weights", false;
    if (const Json* p = doc.find("presets"); p && p->is_object())
        for (auto& [name, g] : p->obj)
            if (!read_weights(g, t.presets[name])) return err = "preset " + name + " needs shape gains", false;
    out = std::move(t);
    return true;
}

std::string arkit_name(std::string_view name) {
    std::string s(name);
    auto ends = [&](const char* suf) { return s.size() > 2 && s.compare(s.size() - 2, 2, suf) == 0; };
    if (ends("_L")) s = s.substr(0, s.size() - 2) + "Left";
    else if (ends("_R")) s = s.substr(0, s.size() - 2) + "Right";
    if (!s.empty()) s[0] = char(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

std::map<std::string, double> face_weights(const FaceTable& table, const std::map<std::string, float>& raw,
                                           const FaceSettings& settings) {
    std::map<std::string, double> in;  // ARKit spelling, aliases expanded
    for (auto& [name, v] : raw) {
        const double w = std::clamp(double(v), 0.0, 1.0);
        if (auto a = table.aliases.find(lower(name)); a != table.aliases.end()) {
            for (auto& [shape, k] : a->second) in[shape] = std::max(in[shape], w * k);
        } else {
            double& d = in[arkit_name(name)];
            d = std::max(d, w);
        }
    }
    std::map<std::string, double> out;
    for (auto& [shape, w] : in) {
        if (!table.shapes.count(shape)) continue;
        double n = 0;
        if (auto it = settings.neutral.find(shape); it != settings.neutral.end()) n = std::clamp(it->second, 0.0, 0.95);
        double g = settings.gain;
        if (auto it = settings.gains.find(shape); it != settings.gains.end()) g *= it->second;
        const double v = std::clamp(std::max(0.0, w - n) / (1 - n) * g, 0.0, 1.5);
        if (v > 0) out[shape] = v;
    }
    return out;
}

void key_face(Clip& clip, const FaceTable& table, const VmcState& s, const FaceSettings& settings, double frame) {
    struct Sum {
        Vec3 rot, pos;
        bool pos_used = false;
    };
    std::map<std::string, Sum> bones;
    for (auto& [shape, motions] : table.shapes)
        for (auto& m : motions) bones[m.bone].pos_used |= m.has_pos;  // every table bone is keyed, even at rest
    for (auto& [shape, w] : face_weights(table, s.blend, settings))
        for (auto& m : table.shapes.at(shape)) {
            Sum& b = bones[m.bone];
            if (m.has_rot) b.rot += m.rot * w;  // ponytail: summed Euler; fine for the small face angles
            if (m.has_pos) b.pos += m.pos * w;
        }
    // Gaze (Euler degrees: y pitch, positive looks down; z yaw, positive looks to the avatar's left).
    for (int side = 0; side < 2; ++side) {
        const FaceTable::Gaze& g = table.gaze[side];
        if (g.eyes.empty()) continue;
        Vec3 e = bones[g.eyes[0]].rot;  // from the eyeLook shapes
        if (s.has_eyes) e = quat_to_euler(side ? s.eye_right : s.eye_left);
        else if (auto it = s.bones.find(side ? "RightEye" : "LeftEye"); it != s.bones.end()) e = quat_to_euler(it->second.rot);
        const Vec3 gaze{0, std::clamp(e.y * settings.eye_gain, -settings.eye_pitch_max, settings.eye_pitch_max),
                        std::clamp(e.z * settings.eye_gain, -settings.eye_yaw_max, settings.eye_yaw_max)};
        for (const std::string& eye : g.eyes) bones[eye].rot = gaze;
        for (auto& [lid, k] : g.lids) bones[lid].rot.y += k * gaze.y;
    }
    for (auto& [bone, b] : bones) {
        key_euler(clip, bone, frame, b.rot);
        if (b.pos_used && settings.positions) key_offset(clip, bone, frame, b.pos);
    }
    if (settings.head && s.has_face_head) key_euler(clip, "mHead", frame, quat_to_euler(s.face_head));
}

bool apply_ifacialmocap(std::string_view p, VmcState& s) {
    if (p.find("=head#") == std::string_view::npos) return false;
    std::map<std::string, float> blend;
    size_t at = 0;
    while (at < p.size()) {
        size_t end = p.find('|', at);
        if (end == std::string_view::npos) end = p.size();
        std::string_view field = p.substr(at, end - at);
        at = end + 1;
        if (field.rfind("=head#", 0) == 0) {  // head Euler degrees, then its position (unused)
            s.face_head = unity_euler_field(field.substr(6));
            s.has_face_head = true;
            continue;
        }
        if (field.rfind("rightEye#", 0) == 0 || field.rfind("leftEye#", 0) == 0) {
            const bool right = field[0] == 'r';
            (right ? s.eye_right : s.eye_left) = unity_euler_field(field.substr(right ? 9 : 8));
            s.has_eyes = true;
            continue;
        }
        if (field.find('#') != std::string_view::npos) continue;
        size_t sep = field.rfind('&');                         // version 2
        if (sep == std::string_view::npos) sep = field.rfind('-');
        if (sep == std::string_view::npos || sep == 0) continue;
        if (blend.size() < 128)  // the network is untrusted: never grow without bound
            blend[std::string(field.substr(0, sep))] = float(std::clamp(to_double(field.substr(sep + 1)) / 100.0, 0.0, 1.0));
    }
    s.blend = std::move(blend);
    return true;
}

std::vector<std::pair<std::string, float>> linden_head_morphs(const std::map<std::string, double>& arkit) {
    auto w = [&](const char* a, const char* b = nullptr) {
        auto get = [&](const char* n) {
            const auto it = n ? arkit.find(n) : arkit.end();
            return it == arkit.end() ? 0.0 : it->second;
        };
        return b ? (get(a) + get(b)) / 2 : get(a);
    };
    // ponytail: one ARKit pair per morph, a first mapping to tune by eye; the other Express_* morphs stay unused.
    const std::pair<const char*, double> map[] = {
        {"Blink_Left", w("eyeBlinkLeft")},   {"Blink_Right", w("eyeBlinkRight")},
        {"Express_Open_Mouth", w("jawOpen")}, {"Express_Smile", w("mouthSmileLeft", "mouthSmileRight")},
        {"Express_Frown", w("mouthFrownLeft", "mouthFrownRight")}, {"Express_Kiss", w("mouthPucker")},
    };
    std::vector<std::pair<std::string, float>> out;
    for (const auto& [morph, v] : map)
        if (const float q = float(std::round(std::clamp(v, 0.0, 1.0) * 20) / 20); q > 0) out.emplace_back(morph, q);
    return out;
}

}  // namespace vats
