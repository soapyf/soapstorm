// Viewport Avatar Toolset - body parts, mirroring, the pose/clip library and the pose and key clipboards.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/pose_ops.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "vats/edit.h"
#include "vats/json.h"
#ifdef VATS_LEGACY_IMPORT
#include "vats/legacy_import.h"
#endif

namespace vats {
namespace {

constexpr const char* kFormat = "vats-pose-library";
constexpr const char* kPoleChannels[3] = {"pole_x", "pole_y", "pole_z"};
constexpr const char* kFingers[] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool is_ik(std::string_view track) { return starts_with(track, "ik."); }

bool is_finger_ik(std::string_view track) {
    for (const char* f : kFingers)
        if (starts_with(track, std::string("ik.") + f)) return true;
    return false;
}

bool animated(const Clip& clip, const std::string& track) {
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return false;
    for (auto& [ch, c] : t->second)
        if (!c.empty()) return true;
    return false;
}

Vec3 eval3(const Clip& clip, const std::string& track, double frame, const char* const (&names)[3]) {
    Vec3 v;
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return v;
    for (int a = 0; a < 3; ++a)
        if (auto c = t->second.find(names[a]); c != t->second.end()) v[a] = c->second.evaluate(frame);
    return v;
}

void set3(Clip& clip, const std::string& track, double frame, const char* const (&names)[3], const Vec3& v) {
    for (int a = 0; a < 3; ++a) clip.curves[track][names[a]].set_key(frame, v[a]);
}

Quat mirror_q(const Quat& q) { return {q.w, -q.x, q.y, -q.z}; }
Vec3 mirror_v(const Vec3& v) { return {v.x, -v.y, v.z}; }
Vec3 mirror_euler(const Vec3& e) { return {-e.x, e.y, -e.z}; }

bool under(const Skeleton& skel, int node, int ancestor) {
    for (int p = skel[node].parent; p >= 0; p = skel[p].parent)
        if (p == ancestor) return true;
    return false;
}

void subtree(const Skeleton& skel, int root, std::vector<int>& out) {
    for (int i = 0; i < skel.size(); ++i)
        if (!skel[i].attachment && under(skel, i, root)) out.push_back(i);
}

void shift(Key& k, double df, double dv) {
    k.frame += df;
    k.lx += df;
    k.rx += df;
    k.value += dv;
    k.ly += dv;
    k.ry += dv;
}

// Key s's mirrored rotation (and position, where it has any) onto d, reading from the pose and clip before the edit.
void key_mirrored(Clip& clip, const Clip& before, const Skeleton& skel, double frame, const Pose& current, int s,
                  int d) {
    const std::string& dn = skel[d].name;
    key_rotation(clip, dn, frame, mirror_rotation(skel, s, d, current.rot[s]));
    if (skel[d].attachment || before.has_channels(skel[s].name, kPosChannels) || before.has_channels(dn, kPosChannels))
        key_offset(clip, dn, frame, mirror_v(current.offset[s]));
}

// A track's curves mirrored from src onto dst. rest(dst)^-1 * mirror(rest(src)) is a rotation about Z for every node
// (180 degrees for Chest, Spine and Skull, whose rests are not mirror images; none for the rest), and since
// R = Rz * Ry * Rx, it is exactly a constant added to rot_z. So negating rot_x and rot_z and offsetting rot_z keeps
// every curve's shape and gives, at every frame, the rotation mirror_rotation gives (E-5).
Track mirror_curves(const Skeleton& skel, const std::string& src, const std::string& dst, const Track& track) {
    Track out = track;
    for (auto& [ch, c] : out)
        if (mirror_flips(ch))
            for (auto& k : c.keys) {
                k.value = -k.value;
                k.ly = -k.ly;
                k.ry = -k.ry;
            }
    int s = skel.find(src), d = skel.find(dst);
    if (s < 0 || d < 0 || is_ik(src)) return out;
    Quat r = (skel[d].rest.conj() * mirror_q(skel[s].rest)).normalized();
    double dz = 2 * std::atan2(r.z, r.w) * kRadToDeg;  // r.x and r.y are 0 (pose_ops_mirror_rest_is_about_z)
    if (std::fabs(wrap_near(dz, 0)) < 1e-9) return out;
    if (!track.count("rot_x") && !track.count("rot_y") && !track.count("rot_z")) return out;
    FCurve& z = out["rot_z"];
    if (z.empty()) z.set_key(0, 0);  // a missing rot_z reads as 0
    for (auto& k : z.keys) shift(k, 0, dz);
    return out;
}

void mirror_ik(Clip& clip, const Clip& before, const std::string& src, const std::string& dst, double frame) {
    if (before.has_channels(src, kPosChannels))
        set3(clip, dst, frame, kPosChannels, mirror_v(eval3(before, src, frame, kPosChannels)));
    if (before.has_channels(src, kPoleChannels))
        set3(clip, dst, frame, kPoleChannels, mirror_v(eval3(before, src, frame, kPoleChannels)));
    if (before.has_channels(src, kRotChannels)) {
        Vec3 e = mirror_euler(curve_euler(before, src, frame));
        key_euler(clip, dst, frame, nearest_euler(euler_to_quat(e), curve_euler(before, dst, frame)));
    }
}

void freeze(FCurve& c) {
    for (auto& k : c.keys) {
        if (k.left != Handle::Free) k.left = Handle::Aligned;
        if (k.right != Handle::Free) k.right = Handle::Aligned;
    }
}

// Adds a key at x without changing the (frozen) curve's shape, outside the keyed range too.
void cut(FCurve& c, double x) {
    if (c.find(x) >= 0) return;
    Key k;
    k.frame = x;
    k.left = k.right = Handle::Aligned;
    k.lx = x - 1;
    k.rx = x + 1;
    if (x < c.keys.front().frame) {
        k.value = k.ly = k.ry = c.keys.front().value;
        k.interp = Interp::Constant;
        c.keys.insert(c.keys.begin(), k);
    } else if (x > c.keys.back().frame) {
        k.value = k.ly = k.ry = c.keys.back().value;
        c.keys.back().interp = Interp::Constant;
        c.keys.push_back(k);
    } else {
        insert_on_curve(c, x);
    }
}

// JSON

Json vec_json(const Vec3& v) {
    Json a = Json::array();
    for (int i = 0; i < 3; ++i) a.push(v[i]);
    return a;
}

bool json_vec(const Json& j, Vec3& v) {
    if (!j.is_array() || j.arr.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!j.arr[i].is_number()) return false;
        v[i] = j.arr[i].num;
    }
    return true;
}

Json curves_json(const std::map<std::string, Track>& curves) {
    Json out = Json::object();
    for (auto& [track, channels] : curves) {
        Json t = Json::object();
        for (auto& [ch, c] : channels) {
            if (c.empty()) continue;
            Json& keys = t.set(ch, Json::array());
            for (auto& k : c.keys) {
                Json r = Json::array();
                for (double v : {k.frame, k.value, double(int(k.interp)), double(int(k.left)), double(int(k.right)),
                                 k.lx, k.ly, k.rx, k.ry})
                    r.push(v);
                keys.push(std::move(r));
            }
        }
        if (!t.obj.empty()) out.set(track, std::move(t));
    }
    return out;
}

int code(double v, int hi) { return static_cast<int>(std::lround(std::clamp(v, 0.0, double(hi)))); }

bool json_curves(const Json& j, std::map<std::string, Track>& out, std::string& err) {
    if (!j.is_object()) return err = "curves is not an object", false;
    for (auto& [track, channels] : j.obj) {
        if (!channels.is_object()) return err = "curves." + track + " is not an object", false;
        for (auto& [ch, keys] : channels.obj) {
            std::string where = "curves." + track + "." + ch;
            if (!keys.is_array()) return err = where + " is not an array", false;
            FCurve c;
            for (auto& r : keys.arr) {
                bool ok = r.is_array() && r.arr.size() == 9;
                for (size_t i = 0; ok && i < 9; ++i) ok = r.arr[i].is_number();
                if (!ok) return err = where + " has a key that is not 9 numbers", false;
                Key k;
                k.frame = r.arr[0].num;
                k.value = r.arr[1].num;
                k.interp = static_cast<Interp>(code(r.arr[2].num, 2));
                k.left = static_cast<Handle>(code(r.arr[3].num, 6));
                k.right = static_cast<Handle>(code(r.arr[4].num, 6));
                k.lx = r.arr[5].num;
                k.ly = r.arr[6].num;
                k.rx = r.arr[7].num;
                k.ry = r.arr[8].num;
                c.keys.push_back(k);
            }
            if (c.empty()) continue;
            std::stable_sort(c.keys.begin(), c.keys.end(), [](auto& a, auto& b) { return a.frame < b.frame; });
            c.recompute_handles();
            out[track][ch] = std::move(c);
        }
    }
    return true;
}

bool json_item(const Json& j, LibraryItem& it, std::string& err) {
    if (!j.is_object()) return err = "not an object", false;
    auto str = [&](const char* key, std::string& out) {
        const Json* v = j.find(key);
        if (!v || v->is_null()) return true;
        if (!v->is_string()) return err = std::string(key) + " is not a string", false;
        out = v->str;
        return true;
    };
    if (!str("id", it.id) || !str("name", it.name) || !str("kind", it.kind) || !str("side", it.side)) return false;
    if (const Json* c = j.find("clip"); c && c->is_bool()) it.clip = c->b;
    if (it.clip) {
        if (const Json* v = j.find("length")) {
            if (!v->is_number()) return err = "length is not a number", false;
            it.length = v->num;
        }
        if (const Json* v = j.find("curves"); v && !json_curves(*v, it.curves, err)) return false;
        if (const Json* v = j.find("relative")) {
            if (!v->is_array()) return err = "relative is not an array", false;
            for (auto& r : v->arr) {
                if (!r.is_string()) return err = "relative holds a non-string", false;
                it.relative.push_back(r.str);
            }
        }
        return true;
    }
    if (const Json* v = j.find("bones")) {
        if (!v->is_object()) return err = "bones is not an object", false;
        for (auto& [bone, e] : v->obj)
            if (!json_vec(e, it.bones[bone])) return err = "bones." + bone + " is not [x, y, z]", false;
    }
    if (const Json* v = j.find("hip"); v && !v->is_null()) {
        Vec3 h;
        if (!json_vec(*v, h)) return err = "hip is not [x, y, z]", false;
        it.hip = h;
    }
    if (const Json* v = j.find("offsets")) {
        if (!v->is_object()) return err = "offsets is not an object", false;
        for (auto& [bone, e] : v->obj)
            if (!json_vec(e, it.offsets[bone])) return err = "offsets." + bone + " is not [x, y, z]", false;
    }
    return true;
}

void key_blend(FCurve& c, double frame, double v) {
    if (c.empty() && frame > 0) c.set_key(0, 1 - v, Interp::Constant);  // AM-54
    c.set_key(frame, v, Interp::Constant);
}

void paste_entry(Clip& clip, const PoseEntry& e, const std::string& track, double frame) {
    if (e.ik) {
        for (auto& [ch, v] : e.channels) {
            FCurve& c = clip.curves[track][ch];
            if (ch == "blend") key_blend(c, frame, v);
            else c.set_key(frame, v);
        }
        return;
    }
    key_euler(clip, track, frame, e.euler);
    if (e.has_pos) key_offset(clip, track, frame, e.pos);
}

const PoseEntry* entry(const PoseClipboard& cb, const std::string& track) {
    for (auto& e : cb.entries)
        if (e.track == track) return &e;
    return nullptr;
}

}  // namespace

// Body parts

std::string side_of(std::string_view name) {
    if (starts_with(name, "pin:")) name.remove_prefix(4);
    if (starts_with(name, "L ")) return "Left";
    if (starts_with(name, "R ")) return "Right";
    bool l = name.find("Left") != std::string_view::npos, r = name.find("Right") != std::string_view::npos;
    return l == r ? "" : l ? "Left" : "Right";
}

BodyPart body_part(const Skeleton& skel, PartKind kind, const std::string& side) {
    BodyPart p;
    p.kind = kind;
    p.side = side;
    auto add = [&](const std::string& n) {
        if (int i = skel.find(n); i >= 0) p.bones.push_back(i);
    };
    auto limb = [&](const std::string& name) {
        if (!side.empty()) p.ik_tracks.push_back("ik." + name + side);
        else for (const char* s : {"Left", "Right"}) p.ik_tracks.push_back("ik." + name + s);
    };
    auto fingers = [&] {
        for (const char* f : kFingers) p.ik_tracks.push_back(std::string("ik.") + f + side);
    };
    auto category = [&](Category c) {
        for (int i = 0; i < skel.size(); ++i)
            if (!skel[i].attachment && skel[i].category == c && (side.empty() || side_of(skel[i].name) == side))
                p.bones.push_back(i);
    };
    auto label = [&](const char* base) { p.label = side.empty() ? base : side + " " + base; };
    switch (kind) {
        case PartKind::Arm:
            for (const char* n : {"mCollar", "mShoulder", "mElbow"}) add(n + side);
            limb("Arm");
            fingers();
            label("Arm");
            break;
        case PartKind::Hand:
            add("mWrist" + side);
            if (!p.bones.empty()) subtree(skel, p.bones[0], p.bones);
            fingers();
            label("Hand");
            break;
        case PartKind::Leg:
            for (const char* n : {"mHip", "mKnee", "mAnkle", "mFoot", "mToe"}) add(n + side);
            limb("Leg");
            label("Leg");
            break;
        case PartKind::Wing:
            category(Category::Wings);
            limb("Wing");
            label("Wing");
            break;
        case PartKind::HindLeg:
            category(Category::HindLimbs);
            limb("HindLeg");
            label("Hind Leg");
            break;
        case PartKind::Tail:
            category(Category::Tail);
            label("Tail");
            break;
        case PartKind::Head:
            add("mNeck");
            if (!p.bones.empty()) subtree(skel, p.bones[0], p.bones);
            label("Head");
            break;
        case PartKind::Torso:
            for (const char* n : {"mPelvis", "mSpine1", "mSpine2", "mTorso", "mSpine3", "mSpine4", "mChest"}) add(n);
            p.ik_tracks.push_back("ik.Spine");
            label("Torso");
            break;
        case PartKind::Category:
        case PartKind::Point:
            break;
    }
    return p;
}

BodyPart with_hand(const Skeleton& skel, BodyPart part) {
    if (part.kind != PartKind::Arm) return part;
    for (int b : body_part(skel, PartKind::Hand, part.side).bones)
        if (std::find(part.bones.begin(), part.bones.end(), b) == part.bones.end()) part.bones.push_back(b);
    return part;
}

BodyPart body_part_of(const Skeleton& skel, int node) {
    const Node& n = skel[node];
    std::string side = side_of(n.name);
    if (n.attachment) {
        BodyPart p;
        p.side = side;
        p.label = n.name;
        p.bones = {node};
        return p;
    }
    auto has = [&](const BodyPart& p) { return std::find(p.bones.begin(), p.bones.end(), node) != p.bones.end(); };
    if (!side.empty())
        for (PartKind k : {PartKind::Hand, PartKind::Arm, PartKind::Leg})
            if (BodyPart p = body_part(skel, k, side); has(p)) return p;
    if (BodyPart p = body_part(skel, PartKind::Head, ""); has(p)) return p;
    if (n.category == Category::Body) return body_part(skel, PartKind::Torso, "");
    switch (n.category) {
        case Category::Wings: return body_part(skel, PartKind::Wing, side);
        case Category::HindLimbs: return body_part(skel, PartKind::HindLeg, side);
        case Category::Tail: return body_part(skel, PartKind::Tail, "");
        default: break;
    }
    static const char* names[] = {"Body", "Hands", "Face", "Wings", "Tail", "Hind Limbs", "Groin", "Attachment Points"};
    BodyPart p;
    p.kind = PartKind::Category;
    p.side = side;
    p.label = side.empty() ? names[int(n.category)] : side + " " + names[int(n.category)];
    for (int i = 0; i < skel.size(); ++i)
        if (!skel[i].attachment && skel[i].category == n.category && (side.empty() || side_of(skel[i].name) == side))
            p.bones.push_back(i);
    return p;
}

std::vector<int> pose_region(const Skeleton& skel, const std::string& kind, const std::string& side) {
    std::vector<int> out;
    if (kind == "arm") {
        out = body_part(skel, PartKind::Arm, side).bones;
        if (int w = skel.find("mWrist" + side); w >= 0) out.push_back(w);
    } else if (kind == "hand") {
        if (int w = skel.find("mWrist" + side); w >= 0) subtree(skel, w, out);
    } else if (kind == "pose") {
        for (int i = 0; i < skel.size(); ++i)
            if (!skel[i].attachment) out.push_back(i);
    } else {
        static const std::pair<const char*, PartKind> kinds[] = {{"leg", PartKind::Leg},
                                                                 {"wing", PartKind::Wing},
                                                                 {"hindleg", PartKind::HindLeg},
                                                                 {"tail", PartKind::Tail},
                                                                 {"head", PartKind::Head}};
        for (auto& [name, k] : kinds)
            if (kind == name) out = body_part(skel, k, side).bones;
    }
    return out;
}

// Mirroring

Quat mirror_rotation(const Skeleton& skel, int src, int dst, const Quat& rot) {
    return (skel[dst].rest.conj() * mirror_q(skel[src].rest * rot)).normalized();
}

// A track on a mesh body's reused bone (Skeleton::reused): the mirror tools leave it as it is.
static bool reused_track(const Skeleton& skel, std::string_view track) {
    if (track.substr(0, 4) == "pin:") track.remove_prefix(4);
    return skel.reused(skel.find(track));
}

bool mirror_flips(std::string_view ch) { return ch == "rot_x" || ch == "rot_z" || ch == "pos_y" || ch == "pole_y"; }

std::string mirror_track(const Skeleton& skel, const std::string& track) {
    std::string m = skel.mirror_of(track);
    if (m == track || is_ik(track)) return m;
    std::string_view bone = m;
    if (starts_with(bone, "pin:")) bone.remove_prefix(4);
    return skel.find(bone) >= 0 ? m : track;
}

void mirror_pose(Clip& clip, const Skeleton& skel, double frame, const Pose& current, MirrorMode mode) {
    const Clip before = clip;
    const char* from = mode == MirrorMode::LeftToRight ? "Left" : "Right";
    for (int d = 0; d < skel.size(); ++d) {
        int s = skel.mirror(d);
        if (mode != MirrorMode::Flip && (s == d || side_of(skel[s].name) != from)) continue;
        if (skel.reused(d) || (!animated(before, skel[s].name) && !animated(before, skel[d].name))) continue;
        key_mirrored(clip, before, skel, frame, current, s, d);
    }
    for (auto& [src, track] : before.curves) {
        if (!is_ik(src)) continue;
        std::string dst = Skeleton::mirror_name(src);
        if (mode != MirrorMode::Flip && (dst == src || side_of(src) != from)) continue;
        mirror_ik(clip, before, src, dst, frame);
    }
}

void mirror_bones(Clip& clip, const Skeleton& skel, double frame, const Pose& current, const std::vector<int>& nodes) {
    const Clip before = clip;
    for (int s : nodes)
        if (!skel.reused(s)) key_mirrored(clip, before, skel, frame, current, s, skel.mirror(s));
}

Clip mirrored_clip(const Skeleton& skel, const Clip& clip) {
    Clip out = clip;
    out.curves.clear();
    out.joint_priority.clear();
    for (auto& [name, track] : clip.curves) {
        std::string m = mirror_track(skel, name);
        if (reused_track(skel, name)) {  // a reused bone (a scarf on a wing) keeps its own motion, on its own side
            out.curves[name] = track;
            continue;
        }
        out.curves[m] = mirror_curves(skel, name, m, track);
    }
    for (auto& [joint, pr] : clip.joint_priority) out.joint_priority[mirror_track(skel, joint)] = pr;
    for (auto& p : out.pins) {
        for (std::string* n : {&p.joint, &p.via, &p.target})
            if (!n->empty()) *n = mirror_track(skel, *n);
        p.pos = mirror_v(p.pos);
        p.rot = mirror_q(p.rot);
    }
    // Reset joint positions' picked joints follow their bones to the other side.
    if (Json* picked = out.export_settings.find("reset_positions_joints"); picked && picked->is_array())
        for (Json& v : picked->arr)
            if (v.is_string()) v.str = mirror_track(skel, v.str);
    return out;
}

// Library

std::string new_item_id() {
    // ponytail: one unguarded generator; add a mutex if ids are ever made off the UI thread.
    static std::mt19937_64 rng{std::random_device{}()};
    static constexpr char hex[] = "0123456789abcdef";
    std::string s = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (char& c : s) {
        if (c == 'x') c = hex[rng() & 15];
        else if (c == 'y') c = hex[8 + (rng() & 3)];
    }
    return s;
}

bool load_library(std::string_view json_text, Library& out, std::string& err) {
    Json doc;
    if (!parse_json(json_text, doc, err)) return false;
    const Json* f = doc.find("format");
    bool ours = f && f->is_string() && f->str == kFormat;
#ifdef VATS_LEGACY_IMPORT
    ours = ours || (f && f->is_string() && f->str == legacy_import::kPoseLibraryFormat);
#endif
    if (!ours)
        return err = "not a pose library (format is missing or unknown)", false;
    Library lib;
    if (const Json* items = doc.find("items")) {
        if (!items->is_array()) return err = "items is not an array", false;
        for (size_t i = 0; i < items->arr.size(); ++i) {
            LibraryItem it;
            if (!json_item(items->arr[i], it, err)) return err = "items[" + std::to_string(i) + "]: " + err, false;
            lib.items.push_back(std::move(it));
        }
    }
    out = std::move(lib);
    return true;
}

std::string save_library(const Library& lib) {
    Json doc = Json::object();
    doc.set("format", kFormat);
    Json& items = doc.set("items", Json::array());
    for (auto& it : lib.items) {
        Json o = Json::object();
        o.set("id", it.id);
        o.set("name", it.name);
        o.set("kind", it.kind);
        o.set("side", it.side);
        if (it.clip) {
            o.set("clip", true);
            o.set("length", it.length);
            o.set("curves", curves_json(it.curves));
            Json& rel = o.set("relative", Json::array());
            for (auto& r : it.relative) rel.push(r);
        } else {
            Json& bones = o.set("bones", Json::object());
            for (auto& [bone, e] : it.bones) bones.set(bone, vec_json(e));
            o.set("hip", it.hip ? vec_json(*it.hip) : Json());
            if (!it.offsets.empty()) {
                Json& offs = o.set("offsets", Json::object());
                for (auto& [bone, v] : it.offsets) offs.set(bone, vec_json(v));
            }
        }
        items.push(std::move(o));
    }
    return write_json(doc);
}

LibraryItem make_pose(const Skeleton& skel, const Pose& displayed, const std::vector<int>& bones,
                      const std::string& kind, const std::string& side, bool with_hip) {
    LibraryItem it;
    it.id = new_item_id();
    it.kind = kind;
    it.side = side;
    for (int i : bones) it.bones[skel[i].name] = quat_to_euler(displayed.rot[i]);
    if (int hip = skel.find("mPelvis"); with_hip && hip >= 0) it.hip = displayed.offset[hip];
    return it;
}

void apply_pose(Clip& clip, const Skeleton& skel, const LibraryItem& pose, double frame, bool mirrored) {
    for (auto& [name, e] : pose.bones) {
        int src = skel.find(name);
        int dst = mirrored ? skel.find(skel.mirror_of(name)) : src;
        if (dst < 0) continue;
        Quat q = euler_to_quat(e);
        if (mirrored && !reused_track(skel, name)) q = src >= 0 ? mirror_rotation(skel, src, dst, q) : mirror_q(q);
        key_rotation(clip, skel[dst].name, frame, q);
    }
    if (pose.hip) key_offset(clip, "mPelvis", frame, mirrored ? mirror_v(*pose.hip) : *pose.hip);
    for (auto& [name, v] : pose.offsets) {
        const int dst = skel.find(mirrored ? skel.mirror_of(name) : name);
        if (dst >= 0) key_offset(clip, skel[dst].name, frame, mirrored && !reused_track(skel, name) ? mirror_v(v) : v);
    }
}

LibraryItem make_clip(const Clip& clip, const std::vector<std::string>& tracks, double a, double b,
                      const std::string& kind, const std::string& side) {
    LibraryItem it;
    it.id = new_item_id();
    it.kind = kind;
    it.side = side;
    it.clip = true;
    it.length = b - a;
    for (auto& name : tracks) {
        auto t = clip.curves.find(name);
        if (t == clip.curves.end()) continue;
        bool rel = kind != "pose" && is_ik(name) && !is_finger_ik(name);
        Track out;
        for (auto& [ch, src] : t->second) {
            if (src.empty() || (rel && ch == "blend")) continue;
            FCurve c = src;
            freeze(c);
            cut(c, a);
            cut(c, b);
            freeze(c);
            double base = rel ? c.evaluate(a) : 0;
            FCurve kept;
            for (Key k : c.keys) {
                if (k.frame < a - 0.001 || k.frame > b + 0.001) continue;
                shift(k, -a, -base);
                kept.keys.push_back(k);
            }
            out[ch] = std::move(kept);
        }
        if (out.empty()) continue;
        it.curves[name] = std::move(out);
        if (rel) it.relative.push_back(name);
    }
    return it;
}

void paste_clip(Clip& clip, const Skeleton& skel, const LibraryItem& item, double at, bool mirrored,
                std::vector<std::string>* warnings) {
    const double lo = at - 0.001, hi = at + item.length + 0.001;
    for (auto& [name, channels] : item.curves) {
        std::string dst = mirrored ? skel.mirror_of(name) : name;
        if (!is_ik(dst)) {
            std::string_view bone = dst;
            if (starts_with(bone, "pin:")) bone.remove_prefix(4);
            if (skel.find(bone) < 0) continue;
        }
        bool rel = std::find(item.relative.begin(), item.relative.end(), name) != item.relative.end();
        const bool flip = mirrored && !reused_track(skel, name);  // a reused bone keeps its own motion
        const Track mirror = flip ? mirror_curves(skel, name, dst, channels) : Track();
        for (auto& [ch, src] : flip ? mirror : channels) {
            if (src.empty()) continue;
            FCurve& d = clip.curves[dst][ch];
            double pre = d.evaluate(at - 1), base = rel ? d.evaluate(at) : 0;
            d.keys.erase(std::remove_if(d.keys.begin(), d.keys.end(),
                                        [&](const Key& k) { return k.frame >= lo && k.frame <= hi; }),
                         d.keys.end());
            bool earlier = !d.keys.empty() && d.keys.front().frame < at;
            if (at > 0 && !earlier)
                d.set_key(at - 1, pre, ch == "blend" ? std::optional<Interp>(Interp::Constant) : std::nullopt);
            std::vector<Key> in;
            for (Key k : src.keys) {
                shift(k, at, base);
                in.push_back(k);
            }
            // Merge: an incoming key replaces an existing one on the same frame.
            for (auto& k : in) {
                if (int i = d.find(k.frame); i >= 0) d.keys.erase(d.keys.begin() + i);
                auto it = std::upper_bound(d.keys.begin(), d.keys.end(), k.frame,
                                           [](double v, const Key& o) { return v < o.frame; });
                d.keys.insert(it, k);
            }
            d.recompute_handles();
        }
        auto blend = [&] {
            auto t = clip.curves.find(dst);
            auto c = t == clip.curves.end() ? Track::const_iterator() : t->second.find("blend");
            return t == clip.curves.end() || c == t->second.end() ? 0.0 : c->second.evaluate(at);
        };
        if (rel && warnings && is_ik(dst) && blend() < 0.5)
            warnings->push_back(dst + ": the limb is in FK at frame " + std::to_string(int(std::lround(at))) +
                                ", so the pasted motion will not show");
    }
}

// Pose clipboard

PoseClipboard copy_pose(const Clip& clip, double frame, const std::vector<std::string>& selected) {
    std::vector<std::string> names = selected;
    if (names.empty())
        for (auto& [name, t] : clip.curves)
            if (is_ik(name) || (!starts_with(name, "pin:") && animated(clip, name))) names.push_back(name);
    PoseClipboard cb;
    for (auto& n : names) {
        PoseEntry e;
        e.track = n;
        e.ik = is_ik(n);
        if (e.ik) {
            if (auto t = clip.curves.find(n); t != clip.curves.end())
                for (auto& [ch, c] : t->second)
                    if (!c.empty()) e.channels[ch] = c.evaluate(frame);
        } else {
            e.euler = curve_euler(clip, n, frame);
            e.pos = curve_offset(clip, n, frame);
            e.has_pos = clip.has_channels(n, kPosChannels);
        }
        cb.entries.push_back(std::move(e));
    }
    return cb;
}

void paste_pose(Clip& clip, const PoseClipboard& cb, double frame, const std::vector<std::string>& selected) {
    if (selected.empty()) {
        for (auto& e : cb.entries) paste_entry(clip, e, e.track, frame);
    } else if (cb.entries.size() == 1) {
        for (auto& s : selected)
            if (is_ik(s) == cb.entries[0].ik) paste_entry(clip, cb.entries[0], s, frame);
    } else {
        for (auto& s : selected)
            if (const PoseEntry* e = entry(cb, s)) paste_entry(clip, *e, s, frame);
    }
}

void paste_pose_part(Clip& clip, const PoseClipboard& cb, double frame, const std::vector<std::string>& part_bones) {
    for (auto& b : part_bones) {
        if (const PoseEntry* e = entry(cb, b)) paste_entry(clip, *e, b, frame);
        else if (const PoseEntry* m = entry(cb, Skeleton::mirror_name(b)); m && !m->ik)
            key_euler(clip, b, frame, mirror_euler(m->euler));
    }
}

// Key clipboard

KeyClipboard copy_keys(const Clip& clip, const std::vector<KeyRef>& sel) {
    KeyClipboard cb;
    double first = 0;
    for (auto& r : sel) {
        auto t = clip.curves.find(r.track);
        if (t == clip.curves.end()) continue;
        auto c = t->second.find(r.channel);
        if (c == t->second.end() || r.index < 0 || r.index >= int(c->second.keys.size())) continue;
        const Key& k = c->second.keys[r.index];
        if (cb.keys.empty() || k.frame < first) first = k.frame;
        cb.keys.push_back({r.track, r.channel, k.frame, k.value, k.interp});
    }
    for (auto& k : cb.keys) k.offset -= first;
    return cb;
}

std::vector<KeyRef> paste_keys(Clip& clip, const KeyClipboard& cb, double frame) {
    for (auto& k : cb.keys) clip.curves[k.track][k.channel].set_key(frame + k.offset, k.value, k.interp);
    std::vector<KeyRef> sel;
    for (auto& k : cb.keys) sel.push_back({k.track, k.channel, clip.curves[k.track][k.channel].find(frame + k.offset)});
    return sel;
}

}  // namespace vats
