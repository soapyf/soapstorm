// Viewport Avatar Toolset - live motion capture over the VMC protocol.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/mocap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "vats/curve_ops.h"
#include "vats/edit.h"
#include "vats/footlock.h"
#include "vats/json.h"
#include "guard.h"

namespace vats {

// --- OSC 1.0 ------------------------------------------------------------------------------------

namespace {

struct Reader {
    const std::uint8_t* p;
    size_t n, at = 0;

    bool left(size_t k) const { return at + k <= n; }
    bool u32(std::uint32_t& v) {
        if (!left(4)) return false;
        v = std::uint32_t(p[at]) << 24 | std::uint32_t(p[at + 1]) << 16 | std::uint32_t(p[at + 2]) << 8 | p[at + 3];
        at += 4;
        return true;
    }
    bool u64(std::uint64_t& v) {
        std::uint32_t hi, lo;
        if (!u32(hi) || !u32(lo)) return false;
        v = std::uint64_t(hi) << 32 | lo;
        return true;
    }
    // A string ends with a NUL and is padded to a multiple of 4 bytes.
    bool str(std::string& s) {
        size_t end = at;
        while (end < n && p[end]) ++end;
        if (end >= n) return false;
        s.assign(reinterpret_cast<const char*>(p + at), end - at);
        at = (end + 4) & ~size_t(3);
        return at <= n;
    }
};

bool parse_message(Reader r, std::vector<OscMessage>& out) {
    OscMessage m;
    std::string tags;
    if (!r.str(m.address) || m.address.empty() || m.address[0] != '/') return false;
    if (r.at == r.n) return out.push_back(std::move(m)), true;  // no type tags (old senders): no args
    if (!r.str(tags) || tags.empty() || tags[0] != ',') return false;
    for (size_t t = 1; t < tags.size(); ++t) {
        OscArg a;
        a.type = tags[t];
        std::uint32_t u;
        std::uint64_t u8;
        switch (a.type) {
            case 'i':
                if (!r.u32(u)) return false;
                a.num = std::int32_t(u);
                break;
            case 'f': {
                if (!r.u32(u)) return false;
                float f;
                std::memcpy(&f, &u, 4);
                a.num = f;
                break;
            }
            case 'h':
            case 't':
                if (!r.u64(u8)) return false;
                a.num = double(std::int64_t(u8));
                break;
            case 'd': {
                if (!r.u64(u8)) return false;
                double d;
                std::memcpy(&d, &u8, 8);
                a.num = d;
                break;
            }
            case 's':
            case 'S':
                if (!r.str(a.str)) return false;
                break;
            case 'b':
                if (!r.u32(u) || !r.left(u)) return false;
                a.str.assign(reinterpret_cast<const char*>(r.p + r.at), u);
                r.at = (r.at + u + 3) & ~size_t(3);
                if (r.at > r.n) return false;
                break;
            case 'c':
            case 'r':
            case 'm':
                if (!r.u32(u)) return false;
                a.num = u;
                break;
            case 'T': a.num = 1; break;
            case 'F':
            case 'N':
            case 'I': break;
            default: return false;  // unknown tags have unknown sizes: the rest cannot be read
        }
        m.args.push_back(std::move(a));
    }
    out.push_back(std::move(m));
    return true;
}

bool parse_packet(const std::uint8_t* data, size_t size, std::vector<OscMessage>& out, int depth) {
    if (size < 4 || size % 4 || depth > 8) return false;
    if (size >= 16 && std::memcmp(data, "#bundle", 8) == 0) {
        Reader r{data, size, 16};  // "#bundle\0" and the time tag, which live capture ignores
        bool ok = true;
        while (r.at < r.n) {
            std::uint32_t len;
            if (!r.u32(len) || !r.left(len)) return false;
            ok = parse_packet(data + r.at, len, out, depth + 1) && ok;
            r.at += len;
        }
        return ok;
    }
    return parse_message(Reader{data, size}, out);
}

}  // namespace

bool parse_osc(const std::uint8_t* data, size_t size, std::vector<OscMessage>& out) {
    std::string err;
    return guarded(err, [&] { return parse_packet(data, size, out, 0); });
}

// --- VMC ----------------------------------------------------------------------------------------

// M = [[0,0,1],[-1,0,0],[0,1,0]] has determinant -1, so a rotation axis maps as -M * axis.
Vec3 unity_to_sl(const Vec3& v) { return {v.z, -v.x, v.y}; }
Quat unity_to_sl(const Quat& q) { return Quat{q.w, -q.z, q.x, -q.y}; }

namespace {

bool read_xform(const OscMessage& m, Xform& out) {
    if (m.args.size() < 8 || m.args[0].type != 's') return false;
    for (int i = 1; i < 8; ++i)
        if (m.args[i].type != 'f' && m.args[i].type != 'd') return false;
    auto f = [&](int i) { return m.args[i].num; };
    out.pos = unity_to_sl(Vec3{f(1), f(2), f(3)});
    out.rot = unity_to_sl(Quat{f(7), f(4), f(5), f(6)}).normalized();  // VMC sends x y z w
    return true;
}

// The Unity humanoid hierarchy; an optional bone that is not sent is skipped over.
struct HumanBone {
    const char* name;
    const char* parent;
};
constexpr HumanBone kHuman[] = {
    {"Hips", nullptr}, {"Spine", "Hips"}, {"Chest", "Spine"}, {"UpperChest", "Chest"}, {"Neck", "UpperChest"},
    {"Head", "Neck"}, {"LeftEye", "Head"}, {"RightEye", "Head"}, {"Jaw", "Head"},
    {"LeftShoulder", "UpperChest"}, {"LeftUpperArm", "LeftShoulder"}, {"LeftLowerArm", "LeftUpperArm"},
    {"LeftHand", "LeftLowerArm"}, {"RightShoulder", "UpperChest"}, {"RightUpperArm", "RightShoulder"},
    {"RightLowerArm", "RightUpperArm"}, {"RightHand", "RightLowerArm"},
    {"LeftUpperLeg", "Hips"}, {"LeftLowerLeg", "LeftUpperLeg"}, {"LeftFoot", "LeftLowerLeg"}, {"LeftToes", "LeftFoot"},
    {"RightUpperLeg", "Hips"}, {"RightLowerLeg", "RightUpperLeg"}, {"RightFoot", "RightLowerLeg"},
    {"RightToes", "RightFoot"},
#define VATS_FINGERS(S)                                                                                            \
    {S "ThumbProximal", S "Hand"}, {S "ThumbIntermediate", S "ThumbProximal"},                                      \
        {S "ThumbDistal", S "ThumbIntermediate"}, {S "IndexProximal", S "Hand"},                                    \
        {S "IndexIntermediate", S "IndexProximal"}, {S "IndexDistal", S "IndexIntermediate"},                       \
        {S "MiddleProximal", S "Hand"}, {S "MiddleIntermediate", S "MiddleProximal"},                               \
        {S "MiddleDistal", S "MiddleIntermediate"}, {S "RingProximal", S "Hand"},                                   \
        {S "RingIntermediate", S "RingProximal"}, {S "RingDistal", S "RingIntermediate"},                           \
        {S "LittleProximal", S "Hand"}, {S "LittleIntermediate", S "LittleProximal"},                               \
        {S "LittleDistal", S "LittleIntermediate"}
    VATS_FINGERS("Left"), VATS_FINGERS("Right"),
#undef VATS_FINGERS
};

bool is_human(const std::string& name) {
    for (const HumanBone& b : kHuman)
        if (name == b.name) return true;
    return false;
}

const char* human_parent(const std::string& name) {
    for (const HumanBone& b : kHuman)
        if (name == b.name) return b.parent;
    return nullptr;
}

// Hips carry the root: the sender's avatar root times the hips' local transform.
Xform bone_of(const VmcState& s, const VmcState& rest, const std::string& name) {
    auto it = s.bones.find(name);
    Xform x = it != s.bones.end() ? it->second : rest.bones.at(name);
    if (name == "Hips") x = s.root * x;
    return x;
}

// A bone's world transform: its local transform under the nearest parent rest knows, as vmc_source chains them.
Xform world_of(const VmcState& s, const VmcState& rest, const std::string& name) {
    Xform x = bone_of(s, rest, name);
    for (const char* p = human_parent(name); p; p = human_parent(p))
        if (rest.bones.count(p)) return world_of(s, rest, p) * x;
    return x;
}

// s's hips standing up in rest's posture on the floor under s's feet: over the ground (X, Y) s's own hips; in
// height the lowest foot joint of s plus the hips' height above the feet in rest, so a crouch in s does not lower
// it. Without feet, s's hips as they are. rest must have hips.
Vec3 standing_hips(const VmcState& s, const VmcState& rest) {
    Vec3 hips = world_of(s, rest, "Hips").pos;
    double floor = 1e300, rest_floor = 1e300;
    for (const char* f : {"LeftFoot", "RightFoot", "LeftToes", "RightToes"})
        if (rest.bones.count(f)) {
            floor = std::min(floor, world_of(s, rest, f).pos.z);
            rest_floor = std::min(rest_floor, world_of(rest, rest, f).pos.z);
        }
    if (floor < 1e300) hips.z = floor + world_of(rest, rest, "Hips").pos.z - rest_floor;
    return hips;
}

}  // namespace

bool apply_vmc(const OscMessage& m, VmcState& s) {
    Xform x;
    if (m.address == "/VMC/Ext/Bone/Pos") {
        if (!read_xform(m, x) || !is_human(m.args[0].str)) return false;  // other names would grow the state forever
        s.bones[m.args[0].str] = x;
        return true;
    }
    if (m.address == "/VMC/Ext/Root/Pos") {
        if (!read_xform(m, x)) return false;
        s.root = x;
        return true;
    }
    if (m.address == "/VMC/Ext/Blend/Val") {
        if (m.args.size() < 2 || m.args[0].type != 's') return false;
        const double v = m.args[1].num;
        // Untrusted network: at most a few hundred names (ARKit has 52, VRM presets fewer).
        if (!std::isfinite(v) || (s.blend_pending.size() >= 256 && !s.blend_pending.count(m.args[0].str))) return false;
        s.blend_pending[m.args[0].str] = float(std::clamp(v, 0.0, 1.0));
        return true;
    }
    if (m.address == "/VMC/Ext/Blend/Apply") {
        s.blend = s.blend_pending;
        return true;
    }
    if (m.address == "/VMC/Ext/OK") {
        if (m.args.empty()) return false;
        const double v = m.args[0].num;
        s.loaded = std::isfinite(v) && std::fabs(v) < 1e9 ? int(v) : 0;
        return true;
    }
    return false;
}

VmcState vmc_t_pose(const VmcState& s) {
    VmcState r = s;
    for (auto& [name, x] : r.bones) x.rot = Quat{};
    r.root.rot = Quat{};  // facing is kept in the pose, not the rest
    // Standing where s stands, however bent s's legs are.
    if (r.bones.count("Hips")) r.bones["Hips"].pos = standing_hips(s, r) - r.root.pos;
    return r;
}

// --- Rokoko Studio Live -------------------------------------------------------------------------
// Format from Rokoko's open plugins (JSON v3: scene.actors[].body.<joint>.position/rotation, world
// transforms in Unity axes; LZ4 frame compression unless Studio is set to plain JSON).

bool lz4_frame_decompress(const std::uint8_t* data, size_t size, std::string& out) {
    const std::uint8_t *p = data, *end = data + size;
    auto u32 = [](const std::uint8_t* q) { return std::uint32_t(q[0] | q[1] << 8 | q[2] << 16 | std::uint32_t(q[3]) << 24); };
    if (size < 7 || u32(p) != 0x184D2204u) return false;
    const std::uint8_t flg = p[4];
    if ((flg >> 6) != 1 || (flg & 1)) return false;  // version 01; dictionaries are not supported
    const bool block_sum = flg & 0x10, content_size = flg & 0x08;
    const size_t header = 6 + (content_size ? 8 : 0) + 1;  // magic, FLG, BD, [size], header checksum
    if (size < header) return false;
    p += header;
    constexpr size_t kMax = size_t(16) << 20;  // a packet never decodes to more than this
    out.clear();
    while (true) {
        if (end - p < 4) return false;
        std::uint32_t n = u32(p);
        p += 4;
        if (n == 0) break;  // end mark (a content checksum may follow; not checked)
        const bool raw = n & 0x80000000u;
        n &= 0x7FFFFFFFu;
        if (size_t(end - p) < n + (block_sum ? 4u : 0u)) return false;
        const std::uint8_t *ip = p, *bend = p + n;
        if (raw) {
            out.append(reinterpret_cast<const char*>(ip), n);
        } else {
            while (ip < bend) {  // LZ4 block: token, literals, then an offset and match length
                const std::uint8_t token = *ip++;
                size_t lit = token >> 4;
                if (lit == 15)
                    for (std::uint8_t b = 255; b == 255 && ip < bend;) lit += b = *ip++;
                if (size_t(bend - ip) < lit || out.size() + lit > kMax) return false;
                out.append(reinterpret_cast<const char*>(ip), lit);
                ip += lit;
                if (ip == bend) break;  // the last sequence has literals only
                if (bend - ip < 2) return false;
                const size_t offset = size_t(ip[0] | ip[1] << 8);
                ip += 2;
                size_t len = token & 15;
                if (len == 15)
                    for (std::uint8_t b = 255; b == 255 && ip < bend;) len += b = *ip++;
                len += 4;
                if (offset == 0 || offset > out.size() || out.size() + len > kMax) return false;
                for (size_t i = 0, from = out.size() - offset; i < len; ++i) out.push_back(out[from + i]);  // may overlap
            }
        }
        p = bend + (block_sum ? 4 : 0);
    }
    return true;
}

namespace {

// Rokoko body joint -> Unity humanoid bone (fingers are added in rokoko_bones()).
const std::vector<std::pair<std::string, std::string>>& rokoko_bones() {
    static const std::vector<std::pair<std::string, std::string>> map = [] {
        std::vector<std::pair<std::string, std::string>> m = {
            {"hip", "Hips"}, {"spine", "Spine"}, {"chest", "Chest"}, {"neck", "Neck"}, {"head", "Head"}};
        for (const char* side : {"left", "right"}) {
            std::string s = side, S = s == "left" ? "Left" : "Right";
            for (auto [r, u] : {std::pair{"Shoulder", "Shoulder"}, {"UpperArm", "UpperArm"}, {"LowerArm", "LowerArm"},
                                {"Hand", "Hand"}, {"UpLeg", "UpperLeg"}, {"Leg", "LowerLeg"}, {"Foot", "Foot"},
                                {"Toe", "Toes"}})
                m.push_back({s + r, S + u});
            for (const char* f : {"Thumb", "Index", "Middle", "Ring", "Little"})
                for (auto [r, u] : {std::pair{"Proximal", "Proximal"}, {"Medial", "Intermediate"}, {"Distal", "Distal"}})
                    m.push_back({s + f + r, S + f + u});
        }
        return m;
    }();
    return map;
}

double member(const Json* o, const char* key, double fallback = 0) {
    const Json* v = o ? o->find(key) : nullptr;
    return v && v->is_number() ? v->num : fallback;
}

}  // namespace

static bool apply_rokoko_packet(const std::uint8_t* data, size_t size, VmcState& s, const std::string& actor,
                                std::string& actor_out, std::string& err) {
    std::string text;
    if (size >= 4 && data[0] == 0x04 && data[1] == 0x22 && data[2] == 0x4D && data[3] == 0x18) {
        if (!lz4_frame_decompress(data, size, text)) return err = "damaged LZ4 packet", false;
    } else {
        text.assign(reinterpret_cast<const char*>(data), size);
    }
    Json j;
    if (!parse_json(text, j, err) || !j.is_object()) return err = "not Rokoko JSON: " + err, false;
    const Json* scene = j.find("scene");
    const Json* actors = scene ? scene->find("actors") : nullptr;
    if (!actors || !actors->is_array()) return err = "no scene.actors: set Studio's custom streaming to JSON v3", false;
    const Json* a = nullptr;
    for (const Json& c : actors->arr) {
        const Json* name = c.find("name");
        const Json* body = c.find("body");
        if (!body || !body->is_object()) continue;
        if (actor.empty() || (name && name->is_string() && name->str == actor)) {
            a = &c;
            break;
        }
    }
    if (!a) return err = actor.empty() ? "no actor with a body in the stream" : "actor " + actor + " is not in the stream", false;
    const Json* name = a->find("name");
    actor_out = name && name->is_string() ? name->str : "";
    const Json* body = a->find("body");
    std::map<std::string, Xform> world;
    for (auto& [r, u] : rokoko_bones()) {
        const Json* jn = body->find(r.c_str());
        const Json* rot = jn ? jn->find("rotation") : nullptr;
        if (!rot || !rot->is_object()) continue;
        const Json* pos = jn->find("position");
        Xform x;
        x.pos = unity_to_sl(Vec3{member(pos, "x"), member(pos, "y"), member(pos, "z")});
        x.rot = unity_to_sl(Quat{member(rot, "w", 1), member(rot, "x"), member(rot, "y"), member(rot, "z")}).normalized();
        world[u] = x;
    }
    if (!world.count("Hips")) return err = "the actor has no hip joint", false;
    // World to local along the humanoid hierarchy, skipping bones Rokoko does not send (UpperChest).
    s.bones.clear();
    for (const HumanBone& b : kHuman) {
        auto it = world.find(b.name);
        if (it == world.end()) continue;
        const char* p = b.parent;
        while (p && !world.count(p)) p = human_parent(p);
        s.bones[b.name] = p ? world[p].inverse() * it->second : it->second;
    }
    s.root = Xform{};
    s.loaded = 1;
    return true;
}

bool apply_rokoko(const std::uint8_t* data, size_t size, VmcState& s, const std::string& actor,
                  std::string& actor_out, std::string& err) {
    return guarded(err, [&] { return apply_rokoko_packet(data, size, s, actor, actor_out, err); });
}

SourceAnim vmc_source(const VmcState& rest_in, const std::vector<VmcState>& frames, double fps, const VmcState* origin) {
    VmcState rest = rest_in;
    rest.root.rot = Quat{};  // facing is kept in the pose, not the rest
    SourceAnim src;
    src.fps = fps;
    src.has_bind = true;
    // Joints in hierarchy order (kHuman lists parents first); unknown names are ignored.
    std::map<std::string, int> index;
    std::vector<std::string> names;
    for (const HumanBone& b : kHuman) {
        if (!rest.bones.count(b.name)) continue;
        const char* p = b.parent;
        while (p && !index.count(p)) p = human_parent(p);
        SourceJoint j;
        j.name = b.name;
        j.parent = p ? index[p] : -1;
        Xform x = bone_of(rest, rest, b.name);
        // Hip travel is measured from here (07 RT-8): the origin standing, with the root's full turn as every
        // frame's hips have it, so a performer standing still where the origin stood records no travel.
        j.offset = j.name == "Hips" ? standing_hips(origin ? *origin : rest_in, rest_in) : x.pos;
        j.rot = x.rot;
        index[b.name] = int(src.joints.size());
        src.joints.push_back(j);
        names.push_back(b.name);
    }
    for (const VmcState& f : frames) {
        std::vector<Quat> rot;
        std::vector<Vec3> pos;
        for (const std::string& n : names) {
            Xform x = bone_of(f, rest, n);
            rot.push_back(x.rot);
            pos.push_back(x.pos);
        }
        src.rot.push_back(std::move(rot));
        src.pos.push_back(std::move(pos));
    }
    return src;
}

// --- Recording ----------------------------------------------------------------------------------

void MocapRecorder::begin(double now, double countdown_s, double fps_, int from_, int to_) {
    fps = std::max(fps_, 1.0);
    from = from_;
    to = to_ >= from_ ? to_ : -1;
    start = now + std::max(countdown_s, 0.0);
    frames.clear();
}

bool MocapRecorder::feed(double now, const VmcState& s) {
    if (!active()) return false;
    if (now < start) return true;
    size_t want = size_t(std::floor((now - start) * fps)) + 1;
    if (to >= 0) want = std::min(want, size_t(to - from + 1));
    while (frames.size() < want) frames.push_back(s);
    if (to >= 0 && frames.size() >= size_t(to - from + 1)) {
        stop();
        return false;
    }
    return true;
}

// --- Writing keys -------------------------------------------------------------------------------

namespace {

// MC-4 smoothing: each rotation becomes the normalised average of its neighbours (aligned to the
// same hemisphere), positions the plain average.
void smooth_source(SourceAnim& src, int radius) {
    if (radius <= 0 || src.frames() < 3) return;
    const auto rot = src.rot;
    const auto pos = src.pos;
    const int n = src.frames();
    for (int f = 0; f < n; ++f)
        for (size_t j = 0; j < src.joints.size(); ++j) {
            Quat q{0, 0, 0, 0};
            Vec3 p;
            int count = 0;
            for (int k = std::max(0, f - radius); k <= std::min(n - 1, f + radius); ++k, ++count) {
                Quat r = rot[k][j].dot(rot[f][j]) < 0 ? -rot[k][j] : rot[k][j];
                q = Quat{q.w + r.w, q.x + r.x, q.y + r.y, q.z + r.z};
                p += pos[k][j];
            }
            src.rot[f][j] = q.normalized();
            src.pos[f][j] = p * (1.0 / count);
        }
}

// MC-4a: filters every curve of the take (frames 0..last, keyed on each frame) and reports its shake before
// and after, rotation in degrees/s^3.
std::vector<std::string> filter_take(Clip& take, int last, const FilterSettings& s) {
    std::vector<CurveId> ids;
    std::vector<std::string> tracks;
    for (auto& [name, tr] : take.curves) {
        tracks.push_back(name);
        for (auto& [ch, c] : tr) ids.push_back({name, ch});
    }
    const std::vector<JointShake> before = shake_scores(take, tracks, 0, last);
    filter_curves(take, ids, 0, last, s);
    const std::vector<JointShake> after = shake_scores(take, tracks, 0, last);
    if (before.empty() || last < 3) return {};
    double b = 0, a = 0;
    std::vector<size_t> order;
    for (size_t i = 0; i < before.size(); ++i) b += before[i].rot, a += after[i].rot, order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return before[x].rot > before[y].rot; });
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s filter: shake %.0f -> %.0f deg/s3 (mean of %zu bones)", filter_name(s.kind),
                  b / double(before.size()), a / double(before.size()), before.size());
    std::vector<std::string> out{buf};
    for (size_t k = 0; k < order.size() && k < 3; ++k) {
        std::snprintf(buf, sizeof buf, "shake of %s: %.0f -> %.0f deg/s3", before[order[k]].track.c_str(),
                      before[order[k]].rot, after[order[k]].rot);
        out.push_back(buf);
    }
    for (size_t i = 0; i < before.size(); ++i)
        if (before[i].track == "mPelvis" && before[i].pos > 0) {
            std::snprintf(buf, sizeof buf, "shake of the hips' travel: %.2f -> %.2f m/s3", before[i].pos, after[i].pos);
            out.push_back(buf);
        }
    return out;
}

}  // namespace

Clip live_pose(const Skeleton& skel, const RigTable& table, const VmcState& rest, const VmcState& now,
               const Shape* shape, const FaceTable* face, const FaceSettings& face_settings) {
    Clip out;
    if (!now.bones.empty()) {
        SourceAnim src = vmc_source(rest, {now}, 30);
        BoneMap map;
        apply_rig_table(table, src, map);
        RetargetOptions opt;
        opt.shape = shape;
        out = retarget(skel, src, map, opt).clip;
    }
    if (face && (!now.blend.empty() || now.has_face_head)) key_face(out, *face, now, face_settings, 0);
    return out;
}

namespace {

Quat slerp(const Quat& a, Quat b, double t) {
    if (a.dot(b) < 0) b = -b;
    const double c = std::min(1.0, a.dot(b));
    if (c > 0.9995) return nlerp(a, b, t);
    const double th = std::acos(c), s = std::sin(th);
    const double wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb}.normalized();
}

// Eases the first and last n frames of the take [from, last] from the animation that was there
// before, where there was animation on that side, so the punch edges do not jump.
void blend_edges(Clip& clip, const Clip& before, const std::vector<std::string>& tracks, int from, int last, int n) {
    struct Blend {
        std::string track;
        int frame;
        bool rot, pos;
        Quat q;
        Vec3 p;
    };
    std::vector<Blend> out;
    for (const std::string& name : tracks) {
        auto old = before.curves.find(name);
        if (old == before.curves.end()) continue;
        bool earlier = false, later = false;
        for (auto& [ch, c] : old->second)
            for (const Key& k : c.keys) earlier |= k.frame < from - 1e-6, later |= k.frame > last + 1e-6;
        const bool rot = clip.has_channels(name, kRotChannels), pos = clip.has_channels(name, kPosChannels);
        for (int i = 0; i < n; ++i) {
            const double w = (i + 1.0) / (n + 1);  // weight of the take
            for (int f : {earlier ? from + i : -1, later ? last - i : -1}) {
                if (f < from || f > last) continue;
                Blend b{name, f, rot, pos, {}, {}};
                if (rot)
                    b.q = slerp(euler_to_quat(curve_euler(before, name, f)), euler_to_quat(curve_euler(clip, name, f)), w);
                if (pos) {
                    Vec3 po = curve_offset(before, name, f), pn = curve_offset(clip, name, f);
                    b.p = po + (pn - po) * w;
                }
                out.push_back(b);
            }
        }
    }
    for (const Blend& b : out) {  // all values first: each new key changes the curve around it
        if (b.rot) key_rotation(clip, b.track, b.frame, b.q);
        if (b.pos) key_offset(clip, b.track, b.frame, b.p);
    }
}

}  // namespace

std::vector<std::string> merge_recording(Clip& clip, const Skeleton& skel, const RigTable& table, const VmcState& rest,
                                         const std::vector<VmcState>& frames, int from,
                                         const std::vector<std::string>& only_tracks, const MocapCleanup& cleanup,
                                         const Shape* shape, const FaceTable* face, const FaceSettings& face_settings) {
    std::vector<std::string> report;
    if (frames.empty()) return {"nothing was recorded"};
    const int fps = std::max(clip.fps, 1);
    bool has_face = false;
    for (const VmcState& f : frames) has_face |= !f.blend.empty() || f.has_face_head;
    has_face &= face != nullptr;
    RetargetResult res;
    if (!frames.front().bones.empty()) {
        // A take starts where the performer stands as it starts, unless they captured a rest pose to measure from.
        SourceAnim src = vmc_source(rest, frames, fps, rest.captured ? &rest : &frames.front());
        if (!cleanup.use_filter) smooth_source(src, cleanup.smooth);
        BoneMap map;
        apply_rig_table(table, src, map);
        if (!map.count("mPelvis") && !has_face) return {"the sender's bones do not match the humanoid table (no hips)"};
        if (map.count("mPelvis")) {
            RetargetOptions opt;
            opt.fps = fps;
            opt.shape = shape;
            res = retarget(skel, src, map, opt);
        }
    } else if (!has_face) {
        return {"the sender sent no body or face"};
    }
    // Face (MC-5): one key per frame on every table bone, then the same reduction as the body.
    if (has_face) {
        // Smoothing averages each shape's weight with its neighbours, as it does the body's rotations.
        const int r = cleanup.use_filter ? 0 : std::max(cleanup.smooth, 0), n = int(frames.size());
        for (int i = 0; i < n; ++i) {
            VmcState f = frames[i];
            if (r > 0) {
                std::map<std::string, float> sum;
                const int a = std::max(0, i - r), b = std::min(n - 1, i + r);
                for (int k = a; k <= b; ++k)
                    for (auto& [name, v] : frames[k].blend) sum[name] += v / float(b - a + 1);
                f.blend = std::move(sum);
            }
            key_face(res.clip, *face, f, face_settings, double(i));
        }
        // key_face keys every table bone's offset, at rest too. A bone no shape moved in this take gets no
        // position channel, so an exported .anim leaves a mesh head's own face joints alone (IO-11a); where the
        // clip already has one, the take overwrites it with rest as before.
        for (const std::string& bone : face->bones()) {
            auto t = res.clip.curves.find(bone);
            if (t == res.clip.curves.end() || clip.has_channels(bone, kPosChannels)) continue;
            bool moved = false;
            for (const char* ch : kPosChannels)
                if (auto c = t->second.find(ch); c != t->second.end())
                    for (const Key& k : c->second.keys) moved = moved || k.value != 0;
            if (!moved)
                for (const char* ch : kPosChannels) t->second.erase(ch);
        }
    }
    std::vector<std::string> shake;
    res.clip.fps = fps;  // a face-only take has no retarget to set it
    if (cleanup.use_filter) shake = filter_take(res.clip, int(frames.size()) - 1, cleanup.filter);
    if (cleanup.reduce) reduce_clip_keys(res.clip, cleanup.rot_deg, cleanup.pos_m);

    const int last = from + int(frames.size()) - 1;
    const Clip before = cleanup.blend > 0 ? clip : Clip{};
    std::vector<std::string> merged;
    for (auto& [name, tr] : res.clip.curves) {
        if (!only_tracks.empty() && std::find(only_tracks.begin(), only_tracks.end(), name) == only_tracks.end())
            continue;
        merged.push_back(name);
        for (auto& [ch, curve] : tr) {
            FCurve& dst = clip.curves[name][ch];
            // Keys just outside the take hold the old animation there (the spline would otherwise bend
            // towards the take from the nearest old keys).
            bool earlier = false, later = false;
            for (const Key& k : dst.keys) earlier |= k.frame < from - 1e-6, later |= k.frame > last + 1e-6;
            if (earlier && from > 0) insert_on_curve(dst, from - 1);
            if (later) insert_on_curve(dst, last + 1);
            dst.keys.erase(std::remove_if(dst.keys.begin(), dst.keys.end(),
                                          [&](const Key& k) { return k.frame >= from - 1e-6 && k.frame <= last + 1e-6; }),
                           dst.keys.end());
            for (Key k : curve.keys) {
                k.frame += from;
                dst.keys.push_back(k);
            }
            std::sort(dst.keys.begin(), dst.keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
            dst.recompute_handles();
        }
    }
    clip.end_frame = std::max(clip.end_frame, last);
    const int tracks = int(merged.size());
    if (cleanup.blend > 0) blend_edges(clip, before, merged, from, last, std::min(cleanup.blend, (last - from + 1) / 2));
    std::vector<std::string> locked;
    if (cleanup.lock_feet) {
        auto has = [&](const char* b) { return std::find(merged.begin(), merged.end(), b) != merged.end(); };
        FootLockOptions fl;
        fl.from = from, fl.to = last, fl.shape = shape, fl.heel_toe = cleanup.heel_toe;
        fl.left = has("mAnkleLeft") || has("mKneeLeft") || has("mHipLeft");
        fl.right = has("mAnkleRight") || has("mKneeRight") || has("mHipRight");
        if (fl.left || fl.right) locked = lock_feet(clip, Rig(skel), fl);
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, "recorded %zu frames (%.2f s) into %d bones, frames %d to %d", frames.size(),
                  (frames.size() - 1) / double(fps), tracks, from, last);
    report.push_back(buf);
    report.insert(report.end(), shake.begin(), shake.end());
    report.insert(report.end(), locked.begin(), locked.end());
    for (auto& r : res.report)
        if (r.find("no source bone") == std::string::npos) report.push_back(r);
    return report;
}

}  // namespace vats
