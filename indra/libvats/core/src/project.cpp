// Viewport Avatar Toolset - project files: the native .vat format and .hxanim migration.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/project.h"
#include "vats/prop.h"
#include "guard.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <initializer_list>
#include <set>

namespace vats {
namespace {

constexpr const char* kVATsFormat = "vats-project";
constexpr const char* kHextonFormat = "hexton-sl-anim";
constexpr const char* kEulerOrder = "xyz-extrinsic";

// .hxanim fields (03 section 3.4); the native format adds kVATsFields (section 3.5).
constexpr const char* kHextonFields[] = {"format", "version", "fps", "end_frame", "loop", "loop_in", "loop_out",
                                         "priority", "ease_in", "ease_out", "hand_pose", "emote", "mirror_export",
                                         "export", "curves", "props", "anchors"};
constexpr const char* kVATsFields[] = {"euler_order", "joint_priority", "constraints", "orphans", "ik_solve",
                                        "meta", "dynamics", "ragdoll", "actors", "active", "audio"};

Json list(std::initializer_list<Json> items) {
    Json r = Json::array();
    r.arr = items;
    return r;
}

bool known(std::string_view key, bool vats) {
    for (const char* k : kHextonFields)
        if (key == k) return true;
    if (vats)
        for (const char* k : kVATsFields)
            if (key == k) return true;
    return false;
}

// Numbers may be written as ints or floats (03 P18): ints are rounded and clamped, never cast blindly.
int to_int(double d, int lo = INT_MIN, int hi = INT_MAX) {
    return static_cast<int>(std::lround(std::clamp(d, double(lo), double(hi))));
}

struct Loader {
    std::string err;

    bool fail(std::string what) {
        err = std::move(what);
        return false;
    }
    bool expect(const Json& v, Json::Type t, const std::string& where) {
        static constexpr const char* names[] = {"null", "a bool", "a number", "a string", "an array", "an object"};
        if (v.type == t) return true;
        return fail(where + ": expected " + names[static_cast<int>(t)]);
    }

    // Each reader leaves the target at its default when the field is absent.
    bool get(const Json& doc, const char* key, int& out, int lo, int hi) {
        const Json* v = doc.find(key);
        if (!v) return true;
        if (!expect(*v, Json::Type::Number, key)) return false;
        out = to_int(v->num, lo, hi);
        return true;
    }
    bool get(const Json& doc, const char* key, double& out) {
        const Json* v = doc.find(key);
        if (!v) return true;
        if (!expect(*v, Json::Type::Number, key)) return false;
        out = v->num;
        return true;
    }
    bool get(const Json& doc, const char* key, bool& out) {
        const Json* v = doc.find(key);
        if (!v) return true;
        if (!expect(*v, Json::Type::Bool, key)) return false;
        out = v->b;
        return true;
    }
    bool get(const Json& doc, const char* key, std::string& out) {
        const Json* v = doc.find(key);
        if (!v) return true;
        if (!expect(*v, Json::Type::String, key)) return false;
        out = v->str;
        return true;
    }
    bool get(const Json& doc, const char* key, Json& out, Json::Type t) {
        const Json* v = doc.find(key);
        if (!v) return true;
        if (!expect(*v, t, key)) return false;
        out = *v;
        return true;
    }

    // A fixed-length array of numbers.
    bool numbers(const Json& v, size_t n, const std::string& where) {
        if (!expect(v, Json::Type::Array, where)) return false;
        if (v.arr.size() != n) return fail(where + ": expected " + std::to_string(n) + " numbers");
        for (auto& e : v.arr)
            if (!e.is_number()) return fail(where + ": expected " + std::to_string(n) + " numbers");
        return true;
    }

    bool curves(const Json& v, Clip& clip) {
        if (!expect(v, Json::Type::Object, "curves")) return false;
        for (auto& [track, channels] : v.obj) {
            std::string tw = "curves." + track;
            if (!expect(channels, Json::Type::Object, tw)) return false;
            for (auto& [channel, keys] : channels.obj) {
                std::string cw = tw + "." + channel;
                if (!expect(keys, Json::Type::Array, cw)) return false;
                FCurve c;
                for (size_t k = 0; k < keys.arr.size(); ++k) {
                    auto& r = keys.arr[k];
                    if (!numbers(r, 9, cw + "[" + std::to_string(k) + "]")) return false;
                    Key key;
                    key.frame = r.arr[0].num;
                    key.value = r.arr[1].num;
                    key.interp = static_cast<Interp>(to_int(r.arr[2].num, 0, 2));
                    key.left = static_cast<Handle>(to_int(r.arr[3].num, 0, 6));
                    key.right = static_cast<Handle>(to_int(r.arr[4].num, 0, 6));
                    key.lx = r.arr[5].num;
                    key.ly = r.arr[6].num;
                    key.rx = r.arr[7].num;
                    key.ry = r.arr[8].num;
                    c.keys.push_back(key);
                }
                if (c.empty()) continue;
                std::stable_sort(c.keys.begin(), c.keys.end(), [](auto& a, auto& b) { return a.frame < b.frame; });
                c.recompute_handles();
                clip.curves[track][channel] = std::move(c);
            }
        }
        return true;
    }

    bool orphans(const Json& v, Clip& clip) {
        if (!expect(v, Json::Type::Array, "orphans")) return false;
        for (size_t n = 0; n < v.arr.size(); ++n) {
            std::string w = "orphans[" + std::to_string(n) + "]";
            auto& e = v.arr[n];
            if (!expect(e, Json::Type::Object, w)) return false;
            OrphanJoint o;
            Json rot = Json::array(), pos = Json::array();
            if (!get(e, "name", o.name) || !get(e, "priority", o.priority, INT_MIN, INT_MAX) ||
                !get(e, "rot", rot, Json::Type::Array) || !get(e, "pos", pos, Json::Type::Array))
                return fail(w + "." + err);
            for (size_t k = 0; k < rot.arr.size(); ++k) {
                auto& r = rot.arr[k].arr;
                if (!numbers(rot.arr[k], 5, w + ".rot[" + std::to_string(k) + "]")) return false;
                o.rot.push_back({r[0].num, Quat{r[4].num, r[1].num, r[2].num, r[3].num}});
            }
            for (size_t k = 0; k < pos.arr.size(); ++k) {
                auto& r = pos.arr[k].arr;
                if (!numbers(pos.arr[k], 4, w + ".pos[" + std::to_string(k) + "]")) return false;
                o.pos.push_back({r[0].num, Vec3{r[1].num, r[2].num, r[3].num}});
            }
            clip.orphans.push_back(std::move(o));
        }
        return true;
    }

    bool constraints(const Json& v, Clip& clip) {
        if (!expect(v, Json::Type::Array, "constraints")) return false;
        auto nibble = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t n = 0; n < v.arr.size(); ++n) {
            std::string w = "constraints[" + std::to_string(n) + "]";
            auto& e = v.arr[n];
            if (!expect(e, Json::Type::String, w)) return false;
            AnimConstraint c{};
            if (e.str.size() != c.size() * 2)
                return fail(w + ": expected " + std::to_string(c.size() * 2) + " hex digits");
            for (size_t k = 0; k < c.size(); ++k) {
                int hi = nibble(e.str[2 * k]), lo = nibble(e.str[2 * k + 1]);
                if (hi < 0 || lo < 0) return fail(w + ": expected hex digits");
                c[k] = static_cast<std::uint8_t>(hi * 16 + lo);
            }
            clip.constraints.push_back(c);
        }
        return true;
    }

    // Spec 08 DY-1: [{root, length, stiffness, damping, drag, gravity, radius, baked, source}]; source is a
    // curves object; unknown fields are kept.
    bool dynamics(const Json& v, Clip& clip) {
        if (!expect(v, Json::Type::Array, "dynamics")) return false;
        static constexpr const char* known_keys[] = {"root", "length", "stiffness", "damping", "drag",
                                                     "gravity", "radius", "baked", "source"};
        for (size_t n = 0; n < v.arr.size(); ++n) {
            std::string w = "dynamics[" + std::to_string(n) + "]";
            const Json& e = v.arr[n];
            if (!expect(e, Json::Type::Object, w)) return false;
            DynChain d;
            if (!get(e, "root", d.root) || !get(e, "length", d.length, 1, 64) || !get(e, "stiffness", d.stiffness) ||
                !get(e, "damping", d.damping) || !get(e, "drag", d.drag) || !get(e, "gravity", d.gravity) ||
                !get(e, "radius", d.radius) || !get(e, "baked", d.baked))
                return fail(w + "." + err);
            if (const Json* s = e.find("source")) {
                Clip tmp;
                if (!curves(*s, tmp)) return fail(w + ".source." + err);
                d.source = std::move(tmp.curves);
            }
            for (auto& [k, x] : e.obj)
                if (std::find(std::begin(known_keys), std::end(known_keys), k) == std::end(known_keys))
                    d.extra.obj.emplace_back(k, x);
            clip.dynamics.push_back(std::move(d));
        }
        return true;
    }

    // Spec 08 RD: {whole_body, bones, start, frames, blend_in, blend_out, gravity, stiffness, friction, baked,
    // source}; unknown fields are kept.
    bool ragdoll(const Json& e, Clip& clip) {
        if (!expect(e, Json::Type::Object, "ragdoll")) return false;
        static constexpr const char* known_keys[] = {"whole_body", "bones",     "start",     "frames",   "blend_in", "blend_out",
                                                     "gravity",    "stiffness", "friction", "baked",    "source"};
        Ragdoll r;
        if (!get(e, "whole_body", r.whole_body) || !get(e, "start", r.start, 0, 100000) ||
            !get(e, "frames", r.frames, 0, 100000) || !get(e, "blend_in", r.blend_in, 0, 100000) ||
            !get(e, "blend_out", r.blend_out, 0, 100000) || !get(e, "gravity", r.gravity) ||
            !get(e, "stiffness", r.stiffness) || !get(e, "friction", r.friction) || !get(e, "baked", r.baked))
            return fail("ragdoll." + err);
        if (const Json* b = e.find("bones")) {
            if (!expect(*b, Json::Type::Array, "ragdoll.bones")) return false;
            for (const Json& x : b->arr) {
                if (!expect(x, Json::Type::String, "ragdoll.bones")) return false;
                r.bones.push_back(x.str);
            }
        }
        if (const Json* s = e.find("source")) {
            Clip tmp;
            if (!curves(*s, tmp)) return fail("ragdoll.source." + err);
            r.source = std::move(tmp.curves);
        }
        for (auto& [k, x] : e.obj)
            if (std::find(std::begin(known_keys), std::end(known_keys), k) == std::end(known_keys)) r.extra.obj.emplace_back(k, x);
        clip.ragdoll = std::move(r);
        return true;
    }

    // Spec 08 AU: {path, offset, volume, bpm, beat_offset, beats: [seconds], snap}; unknown fields kept.
    bool audio(const Json& e, Clip& clip) {
        if (!expect(e, Json::Type::Object, "audio")) return false;
        static constexpr const char* known_keys[] = {"path", "offset", "volume", "bpm", "beat_offset", "beats", "snap"};
        AudioTrack a;
        if (!get(e, "path", a.path) || !get(e, "offset", a.offset) || !get(e, "volume", a.volume) ||
            !get(e, "bpm", a.bpm) || !get(e, "beat_offset", a.beat_offset) || !get(e, "snap", a.snap))
            return fail("audio." + err);
        if (!std::isfinite(a.offset) || !std::isfinite(a.volume) || !std::isfinite(a.bpm) || !std::isfinite(a.beat_offset))
            return fail("audio: a number is not finite");
        a.volume = std::clamp(a.volume, 0.0, 2.0), a.bpm = std::clamp(a.bpm, 0.0, 999.0);
        if (const Json* b = e.find("beats")) {
            if (!expect(*b, Json::Type::Array, "audio.beats")) return false;
            for (const Json& x : b->arr) {
                if (!expect(x, Json::Type::Number, "audio.beats") || !std::isfinite(x.num)) return fail("audio.beats: not a number");
                a.beats.push_back(x.num);
            }
            std::sort(a.beats.begin(), a.beats.end());
        }
        for (auto& [k, x] : e.obj)
            if (std::find(std::begin(known_keys), std::end(known_keys), k) == std::end(known_keys)) a.extra.obj.emplace_back(k, x);
        clip.audio = std::move(a);
        return true;
    }

    bool joint_priority(const Json& v, Clip& clip) {
        if (!expect(v, Json::Type::Object, "joint_priority")) return false;
        for (auto& [joint, pr] : v.obj) {
            if (!expect(pr, Json::Type::Number, "joint_priority." + joint)) return false;
            clip.joint_priority[joint] = to_int(pr.num);
        }
        return true;
    }

    // Elements must be objects.
    bool records(const Json& v, const char* what) {
        for (size_t n = 0; n < v.arr.size(); ++n)
            if (!expect(v.arr[n], Json::Type::Object, std::string(what) + "[" + std::to_string(n) + "]")) return false;
        return true;
    }

    // "anchors" (03 section 3.4.3, plus start_key). Frames are coerced to integers (AM-140, E-17).
    bool actors(const Json& v, Project& p, bool vats);  // defined after read_clip
    bool pins(const Json& v, Clip& clip) {
        if (!expect(v, Json::Type::Array, "anchors") || !records(v, "anchors")) return false;
        for (size_t n = 0; n < v.arr.size(); ++n) {
            std::string w = "anchors[" + std::to_string(n) + "]";
            auto& e = v.arr[n];
            Pin p;
            if (!get(e, "joint", p.joint) || !get(e, "via", p.via) || !get(e, "target", p.target) ||
                !get(e, "from", p.from, INT_MIN, INT_MAX) || !get(e, "to", p.to, INT_MIN, INT_MAX) ||
                !get(e, "start_key", p.start_key, INT_MIN, INT_MAX) ||
                !get(e, "release_key", p.release_key, INT_MIN, INT_MAX) || !get(e, "target_actor", p.target_actor))
                return fail(w + "." + err);
            if (const Json* pos = e.find("pos")) {
                if (!numbers(*pos, 3, w + ".pos")) return false;
                p.pos = {pos->arr[0].num, pos->arr[1].num, pos->arr[2].num};
            }
            if (const Json* rot = e.find("rot")) {
                if (!numbers(*rot, 4, w + ".rot")) return false;
                p.rot = {rot->arr[3].num, rot->arr[0].num, rot->arr[1].num, rot->arr[2].num};
            }
            static const std::set<std::string> known = {"joint", "via",       "target", "from", "to",
                                                        "start_key", "release_key", "pos", "rot", "target_actor"};
            for (auto& [k, x] : e.obj)
                if (!known.count(k)) p.extra.obj.emplace_back(k, x);
            clip.pins.push_back(std::move(p));
        }
        return true;
    }
};

// The clip's own fields: shared by the top level and each actor entry (GR-5).
bool read_clip(Loader& L, const Json& doc, Clip& c, bool vats, bool read_only) {
    // .hxanim limbs pose as the reference app posed them (02 section 3.7).
    std::string ik = vats ? "vats" : "literal";
    if (vats && !L.get(doc, "ik_solve", ik)) return false;
    if (ik == "literal")
        c.ik_solve = IkSolve::Literal;
    else if (ik != "vats" && !read_only)
        return L.fail("unsupported ik_solve \"" + ik + "\"");

    // loop_out defaults to end_frame (03 section 3.4).
    bool ok = L.get(doc, "fps", c.fps, 1, 120) && L.get(doc, "end_frame", c.end_frame, 0, 3600);
    c.loop_out = c.end_frame;
    ok = ok && L.get(doc, "loop", c.loop) && L.get(doc, "loop_in", c.loop_in, 0, 3600) &&
         L.get(doc, "loop_out", c.loop_out, 0, 3600) && L.get(doc, "priority", c.priority, 0, 6) &&
         L.get(doc, "ease_in", c.ease_in) && L.get(doc, "ease_out", c.ease_out) &&
         L.get(doc, "hand_pose", c.hand_pose, 0, 13) && L.get(doc, "emote", c.emote) &&
         L.get(doc, "mirror_export", c.mirror_export) &&
         L.get(doc, "export", c.export_settings, Json::Type::Object);
    if (const Json* v = doc.find("props"); ok && v) ok = props_from_json(*v, c.props, L.err);
    if (const Json* v = doc.find("curves"); ok && v) ok = L.curves(*v, c);
    if (const Json* v = doc.find("anchors"); ok && v) ok = L.pins(*v, c);
    if (vats) {
        if (const Json* v = doc.find("joint_priority"); ok && v) ok = L.joint_priority(*v, c);
        if (const Json* v = doc.find("constraints"); ok && v) ok = L.constraints(*v, c);
        if (const Json* v = doc.find("orphans"); ok && v) ok = L.orphans(*v, c);
        if (const Json* v = doc.find("dynamics"); ok && v) ok = L.dynamics(*v, c);
        if (const Json* v = doc.find("ragdoll"); ok && v) ok = L.ragdoll(*v, c);
        if (const Json* v = doc.find("audio"); ok && v) ok = L.audio(*v, c);
    }
    return ok;
}

bool Loader::actors(const Json& v, Project& p, bool vats) {
    if (!expect(v, Json::Type::Array, "actors") || !records(v, "actors")) return false;
    for (size_t n = 0; n < v.arr.size(); ++n) {
        std::string w = "actors[" + std::to_string(n) + "]";
        const Json& e = v.arr[n];
        Actor a;
        if (!get(e, "name", a.name) || !get(e, "body", a.body) || !get(e, "rot_z", a.rot_z) ||
            !get(e, "hidden", a.hidden) || !get(e, "locked", a.locked))
            return fail(w + "." + err);
        if (const Json* x = e.find("pos")) {
            if (!numbers(*x, 3, w + ".pos")) return false;
            a.pos = {x->arr[0].num, x->arr[1].num, x->arr[2].num};
        }
        if (const Json* x = e.find("colour")) {
            if (!numbers(*x, 3, w + ".colour")) return false;
            for (int k = 0; k < 3; ++k) a.colour[k] = float(x->arr[k].num);
        }
        if (const Json* x = e.find("clip")) {
            if (!expect(*x, Json::Type::Object, w + ".clip") || !read_clip(*this, *x, a.clip, vats, p.read_only))
                return fail(w + ".clip: " + err);
            for (auto& [k, y] : x->obj)
                if (!known(k, vats)) a.clip_extra.obj.emplace_back(k, y);
        }
        static const std::set<std::string> known = {"name", "body", "rot_z", "hidden", "locked", "pos", "colour", "clip"};
        for (auto& [k, x] : e.obj)
            if (!known.count(k)) a.extra.obj.emplace_back(k, x);
        p.actors.push_back(std::move(a));
    }
    return true;
}

}  // namespace

static bool load_project_text(std::string_view text, Project& out, std::string& err, std::string_view source_path) {
    Json doc;
    if (!parse_json(text, doc, err)) return false;
    Loader L;
    auto done = [&](bool ok) {
        if (!ok) err = L.err;
        return ok;
    };
    if (!doc.is_object()) return done(L.fail("not a JSON object"));

    const Json* format = doc.find("format");
    bool vats = format && format->is_string() && format->str == kVATsFormat;
    if (!vats && !(format && format->is_string() && format->str == kHextonFormat))
        return done(L.fail("not a VATs or Hexton project (format is missing or unknown)"));

    Project p;
    Clip& c = p.clip;
    int version = kProjectVersion;
    if (!L.get(doc, "version", version, INT_MIN, INT_MAX)) return done(false);
    if (version < 1) return done(L.fail("unsupported version " + std::to_string(version)));
    p.read_only = version > kProjectVersion;
    p.migrated = !vats;

    std::string euler = kEulerOrder;
    if (vats && !L.get(doc, "euler_order", euler)) return done(false);
    if (euler != kEulerOrder && !p.read_only) return done(L.fail("unsupported euler_order \"" + euler + "\""));
    bool ok = read_clip(L, doc, c, vats, p.read_only);
    if (vats) {
        ok = ok && L.get(doc, "meta", p.meta, Json::Type::Object);
        if (const Json* v = doc.find("actors"); ok && v) ok = L.actors(*v, p, vats) && L.get(doc, "active", p.active, 0, 1000);
        if (ok && !p.actors.empty()) {
            if (p.actors.size() < 2) p.actors.clear();  // one actor is a plain project
            p.active = p.actors.empty() ? 0 : std::min(p.active, int(p.actors.size()) - 1);
            if (!p.actors.empty()) p.actors[p.active].clip = {};
            sync_actor_timing(p);
        }
    }
    if (!ok) return done(false);

    Json unknown = Json::object();
    for (auto& [k, v] : doc.obj)
        if (!known(k, vats)) unknown.obj.emplace_back(k, v);
    if (vats) {
        p.extra = std::move(unknown);
    } else {
        if (!source_path.empty()) p.meta.set("migrated_from", std::string(source_path));
        if (!unknown.obj.empty()) p.meta.set("hexton_extra", std::move(unknown));
    }
    out = std::move(p);
    return true;
}

static Json curves_to_json(const std::map<std::string, Track>& tracks) {
    Json curves = Json::object();
    for (auto& [track, channels] : tracks) {
        Json t = Json::object();
        for (auto& [channel, curve] : channels) {
            if (curve.empty()) continue;
            Json& keys = t.set(channel, Json::array());
            for (auto& k : curve.keys) {
                keys.push(list({k.frame, k.value, int(k.interp), int(k.left), int(k.right), k.lx, k.ly, k.rx, k.ry}));
            }
        }
        if (!t.obj.empty()) curves.set(track, std::move(t));
    }
    return curves;
}

static void write_clip(Json& j, const Clip& c) {
    j.set("fps", c.fps);
    j.set("end_frame", c.end_frame);
    j.set("loop", c.loop);
    j.set("loop_in", c.loop_in);
    j.set("loop_out", c.loop_out);
    j.set("priority", c.priority);
    j.set("ease_in", c.ease_in);
    j.set("ease_out", c.ease_out);
    j.set("hand_pose", c.hand_pose);
    j.set("emote", c.emote);
    j.set("mirror_export", c.mirror_export);
    j.set("export", c.export_settings);

    j.set("curves", curves_to_json(c.curves));
    j.set("props", props_to_json(c.props));
    Json& anchors = j.set("anchors", Json::array());
    for (auto& pin : c.pins) {
        Json e = Json::object();
        e.set("joint", pin.joint);
        e.set("via", pin.via);
        e.set("target", pin.target);
        e.set("from", pin.from);
        e.set("to", pin.to);
        e.set("pos", list({pin.pos.x, pin.pos.y, pin.pos.z}));
        e.set("rot", list({pin.rot.x, pin.rot.y, pin.rot.z, pin.rot.w}));
        if (pin.start_key >= 0) e.set("start_key", pin.start_key);
        if (pin.release_key >= 0) e.set("release_key", pin.release_key);
        if (!pin.target_actor.empty()) e.set("target_actor", pin.target_actor);
        for (auto& [k, x] : pin.extra.obj)
            if (!e.find(k)) e.obj.emplace_back(k, x);
        anchors.push(std::move(e));
    }

    if (!c.joint_priority.empty()) {
        Json& jp = j.set("joint_priority", Json::object());
        for (auto& [joint, pr] : c.joint_priority) jp.set(joint, pr);
    }
    if (!c.constraints.empty()) {
        static constexpr char hex[] = "0123456789abcdef";
        Json& cs = j.set("constraints", Json::array());
        for (auto& con : c.constraints) {
            std::string s;
            for (std::uint8_t b : con) {
                s += hex[b >> 4];
                s += hex[b & 15];
            }
            cs.push(std::move(s));
        }
    }
    if (!c.orphans.empty()) {
        Json& os = j.set("orphans", Json::array());
        for (auto& o : c.orphans) {
            Json e = Json::object();
            e.set("name", o.name);
            e.set("priority", o.priority);
            Json& rot = e.set("rot", Json::array());
            for (auto& [t, q] : o.rot) rot.push(list({t, q.x, q.y, q.z, q.w}));
            Json& pos = e.set("pos", Json::array());
            for (auto& [t, v] : o.pos) pos.push(list({t, v.x, v.y, v.z}));
            os.push(std::move(e));
        }
    }
    if (!c.dynamics.empty()) {
        Json& ds = j.set("dynamics", Json::array());
        for (auto& d : c.dynamics) {
            Json e = Json::object();
            e.set("root", d.root);
            e.set("length", d.length);
            e.set("stiffness", d.stiffness);
            e.set("damping", d.damping);
            e.set("drag", d.drag);
            e.set("gravity", d.gravity);
            e.set("radius", d.radius);
            e.set("baked", d.baked);
            if (d.baked) e.set("source", curves_to_json(d.source));
            for (auto& [k, x] : d.extra.obj)
                if (!e.find(k)) e.obj.emplace_back(k, x);
            ds.push(std::move(e));
        }
    }
    if (c.ragdoll) {
        const Ragdoll& r = *c.ragdoll;
        Json e = Json::object();
        e.set("whole_body", r.whole_body);
        Json& bones = e.set("bones", Json::array());
        for (auto& b : r.bones) bones.push(b);
        e.set("start", r.start);
        e.set("frames", r.frames);
        e.set("blend_in", r.blend_in);
        e.set("blend_out", r.blend_out);
        e.set("gravity", r.gravity);
        e.set("stiffness", r.stiffness);
        e.set("friction", r.friction);
        e.set("baked", r.baked);
        if (r.baked) e.set("source", curves_to_json(r.source));
        for (auto& [k, x] : r.extra.obj)
            if (!e.find(k)) e.obj.emplace_back(k, x);
        j.set("ragdoll", std::move(e));
    }
    if (c.audio) {
        const AudioTrack& a = *c.audio;
        Json e = Json::object();
        e.set("path", a.path);
        e.set("offset", a.offset);
        e.set("volume", a.volume);
        e.set("bpm", a.bpm);
        e.set("beat_offset", a.beat_offset);
        Json& beats = e.set("beats", Json::array());
        for (double t : a.beats) beats.push(t);
        e.set("snap", a.snap);
        for (auto& [k, x] : a.extra.obj)
            if (!e.find(k)) e.obj.emplace_back(k, x);
        j.set("audio", std::move(e));
    }
    if (c.ik_solve == IkSolve::Literal) j.set("ik_solve", "literal");
}

std::string save_project(const Project& p) {
    const Clip& c = p.clip;
    Json j = Json::object();
    j.set("format", kVATsFormat);
    j.set("version", p.actors.size() >= 2 ? kProjectVersion : 1);  // single-actor files stay version 1 (GR-5)
    j.set("euler_order", kEulerOrder);
    write_clip(j, c);
    if (p.actors.size() >= 2) {
        Json& as = j.set("actors", Json::array());
        for (int i = 0; i < int(p.actors.size()); ++i) {
            const Actor& a = p.actors[i];
            Json e = Json::object();
            e.set("name", a.name);
            e.set("colour", list({double(a.colour[0]), double(a.colour[1]), double(a.colour[2])}));
            e.set("body", a.body);
            e.set("pos", list({a.pos.x, a.pos.y, a.pos.z}));
            e.set("rot_z", a.rot_z);
            e.set("hidden", a.hidden);
            e.set("locked", a.locked);
            if (i != p.active) {  // the active actor's clip is the top level
                Json cj = Json::object();
                write_clip(cj, a.clip);
                for (auto& [k, x] : a.clip_extra.obj)
                    if (!cj.find(k)) cj.obj.emplace_back(k, x);
                e.set("clip", std::move(cj));
            }
            for (auto& [k, x] : a.extra.obj)
                if (!e.find(k)) e.obj.emplace_back(k, x);
            as.push(std::move(e));
        }
        j.set("active", p.active);
    }
    j.set("meta", p.meta);
    for (auto& [k, v] : p.extra.obj)
        if (!j.find(k)) j.obj.emplace_back(k, v);
    if (!p.actors.empty())  // the active actor's clip is the top level, so its unknown fields are too
        for (auto& [k, v] : p.actors[p.active].clip_extra.obj)
            if (!j.find(k)) j.obj.emplace_back(k, v);
    return write_json(j);
}

}  // namespace vats

namespace vats {

const Clip& actor_clip(const Project& p, int i) { return i == p.active || p.actors.empty() ? p.clip : p.actors[i].clip; }
Clip& actor_clip(Project& p, int i) { return i == p.active || p.actors.empty() ? p.clip : p.actors[i].clip; }

void set_active_actor(Project& p, int i) {
    if (i == p.active || i < 0 || i >= int(p.actors.size())) return;
    p.actors[p.active].clip = std::move(p.clip);
    p.clip = std::move(p.actors[i].clip);
    p.actors[i].clip = {};
    p.active = i;
}

void sync_actor_timing(Project& p) {
    for (int i = 0; i < int(p.actors.size()); ++i) {
        if (i == p.active) continue;
        Clip& c = p.actors[i].clip;
        c.fps = p.clip.fps, c.end_frame = p.clip.end_frame, c.loop = p.clip.loop;
        c.loop_in = p.clip.loop_in, c.loop_out = p.clip.loop_out;
    }
}

bool load_project(std::string_view text, Project& out, std::string& err, std::string_view source_path) {
    return guarded(err, [&] { return load_project_text(text, out, err, source_path); });
}

}  // namespace vats
