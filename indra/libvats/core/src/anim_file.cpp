// Viewport Avatar Toolset - the SL .anim (keyframe motion) file, byte level.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Float arithmetic here must stay binary32 and uncontracted (no FMA) to match the viewer's
// codes bit for bit; CMake builds this file with -ffp-contract=off.
#include "vats/anim_file.h"
#include "guard.h"

#include <cmath>
#include <cstring>

namespace vats {
namespace {

class Writer {
public:
    std::vector<std::uint8_t> out;

    template <class T>
    void put(T v) {
        std::uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));  // little-endian hosts only (x86, ARM)
        out.insert(out.end(), b, b + sizeof(T));
    }
    void str(const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    }
};

class Reader {
public:
    const std::vector<std::uint8_t>& in;
    size_t at = 0;
    bool ok = true;

    template <class T>
    T get() {
        T v{};
        if (at + sizeof(T) > in.size()) {
            ok = false;
            at = in.size();
            return v;
        }
        std::memcpy(&v, in.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string str() {
        size_t end = at;
        while (end < in.size() && in[end]) ++end;
        if (end >= in.size()) {  // no terminating NUL
            ok = false;
            at = in.size();
            return {};
        }
        std::string s(reinterpret_cast<const char*>(in.data() + at), end - at);
        at = end + 1;
        return s;
    }
};

const char* const kEmotes[] = {
    "express_afraid",  "express_anger", "express_bored",      "express_cry",   "express_disdain",
    "express_embarrased", "express_frown", "express_kiss",    "express_laugh", "express_open_mouth",
    "express_repulsed", "express_sad",  "express_shrug",      "express_smile", "express_surprise",
    "express_tongue_out", "express_toothsmile", "express_wink", "express_worry"};

std::string fixed_name(const std::uint8_t* p) {
    size_t n = 0;
    while (n < 16 && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

float get_f32(const AnimConstraint& c, size_t off) {
    float v;
    std::memcpy(&v, c.data() + off, 4);
    return v;
}

}  // namespace

std::uint16_t f32_to_u16(float val, float lower, float upper) {
    val = val < lower ? lower : (val > upper ? upper : val);
    val -= lower;
    val /= (upper - lower);
    return static_cast<std::uint16_t>(static_cast<std::int32_t>(std::floor(val * 65535.0f)));
}

float u16_to_f32(std::uint16_t ival, float lower, float upper) {
    const float oo_u16max = 1.f / 65535.0f;
    float val = ival * oo_u16max;
    float delta = upper - lower;
    val *= delta;
    val += lower;
    float max_error = delta * oo_u16max;
    if (std::fabs(val) < max_error) val = 0.f;  // zeroes come through as zero
    return val;
}

std::uint16_t anim_code(float v, float lo, float hi) { return f32_to_u16(u16_to_f32(f32_to_u16(v, lo, hi), lo, hi), lo, hi); }

std::array<std::uint16_t, 3> encode_rotation(const Quat& qd) {
    // LLQuaternion::packToVector3 on the float quaternion.
    float x = static_cast<float>(qd.x), y = static_cast<float>(qd.y), z = static_cast<float>(qd.z),
          w = static_cast<float>(qd.w);
    float mag = std::sqrt(x * x + y * y + z * z + w * w);
    if (mag > 0.0000001f) {
        x /= mag;
        y /= mag;
        z /= mag;
    }
    if (w < 0) {
        x = -x;
        y = -y;
        z = -z;
    }
    return {anim_code(x, -1.f, 1.f), anim_code(y, -1.f, 1.f), anim_code(z, -1.f, 1.f)};
}

std::array<std::uint16_t, 3> encode_position(const Vec3& p) {
    const float m = kAnimMaxOffset;
    return {anim_code(static_cast<float>(p.x), -m, m), anim_code(static_cast<float>(p.y), -m, m),
            anim_code(static_cast<float>(p.z), -m, m)};
}

Quat decode_rotation(const std::array<std::uint16_t, 4>& k) {
    float x = u16_to_f32(k[1], -1.f, 1.f), y = u16_to_f32(k[2], -1.f, 1.f), z = u16_to_f32(k[3], -1.f, 1.f);
    float t = 1.f - (x * x + y * y + z * z);
    float w = t > 0 ? std::sqrt(t) : 0.f;
    return Quat{w, x, y, z}.normalized();
}

Vec3 decode_position(const std::array<std::uint16_t, 4>& k) {
    const float m = kAnimMaxOffset;
    return {u16_to_f32(k[1], -m, m), u16_to_f32(k[2], -m, m), u16_to_f32(k[3], -m, m)};
}

float stable_value(std::uint16_t code, float lo, float hi) {
    // anim_code is monotonic, so each code owns one interval of inputs. Find its ends by bisection.
    auto first_at_least = [&](int c) {
        double a = lo, b = hi;
        if (anim_code(lo, lo, hi) >= c) return a;
        for (int i = 0; i < 60; ++i) {
            double m = (a + b) * 0.5;
            (anim_code(static_cast<float>(m), lo, hi) >= c ? b : a) = m;
        }
        return b;
    };
    double start = first_at_least(code), end = code == 65535 ? hi : first_at_least(code + 1);
    float mid = static_cast<float>((start + end) * 0.5);
    return anim_code(mid, lo, hi) == code ? mid : u16_to_f32(code, lo, hi);
}

namespace {

// Per-range lookup tables, built on first use.
const std::vector<float>& stable_table(float lo) {
    auto build = [](float l, float h) {
        std::vector<float> t(65536);
        for (int c = 0; c < 65536; ++c) t[c] = stable_value(static_cast<std::uint16_t>(c), l, h);
        return t;
    };
    static const std::vector<float> rot = build(-1.f, 1.f), pos = build(-kAnimMaxOffset, kAnimMaxOffset);
    return lo == -1.f ? rot : pos;
}

}  // namespace

Quat stable_rotation(const std::array<std::uint16_t, 4>& k) {
    const auto& t = stable_table(-1.f);
    float x = t[k[1]], y = t[k[2]], z = t[k[3]];
    float s = 1.f - (x * x + y * y + z * z);
    return Quat{s > 0 ? std::sqrt(s) : 0.f, x, y, z}.normalized();
}

Vec3 stable_position(const std::array<std::uint16_t, 4>& k) {
    const auto& t = stable_table(-kAnimMaxOffset);
    return {t[k[1]], t[k[2]], t[k[3]]};
}

static bool parse_anim_bytes(const std::vector<std::uint8_t>& bytes, AnimFile& f, std::string& err) {
    f = AnimFile();
    Reader r{bytes};
    f.version = r.get<std::uint16_t>();
    f.sub_version = r.get<std::uint16_t>();
    if (!r.ok) {
        err = "file too short";
        return false;
    }
    if (!(f.version == 1 && f.sub_version == 0) && !f.legacy()) {
        err = "unsupported .anim version " + std::to_string(f.version) + "." + std::to_string(f.sub_version);
        return false;
    }
    f.base_priority = r.get<std::int32_t>();
    f.duration = r.get<float>();
    f.emote = r.str();
    f.loop_in = r.get<float>();
    f.loop_out = r.get<float>();
    f.loop = r.get<std::int32_t>();
    f.ease_in = r.get<float>();
    f.ease_out = r.get<float>();
    f.hand_pose = r.get<std::uint32_t>();
    std::uint32_t n = r.get<std::uint32_t>();
    if (!r.ok) {
        err = "truncated header";
        return false;
    }
    if (n > kAnimMaxJoints) {  // the viewer refuses these too; also bounds our allocation
        err = "too many joints (" + std::to_string(n) + ")";
        return false;
    }
    for (std::uint32_t j = 0; j < n && r.ok; ++j) {
        AnimJoint jt;
        jt.name = r.str();
        jt.priority = r.get<std::int32_t>();
        for (int pass = 0; pass < 2 && r.ok; ++pass) {
            std::int32_t count = r.get<std::int32_t>();
            if (count < 0 || static_cast<size_t>(count) > bytes.size()) {
                err = "bad key count in joint " + jt.name;
                return false;
            }
            for (std::int32_t k = 0; k < count && r.ok; ++k) {
                if (f.legacy()) {
                    std::array<float, 4> key{r.get<float>(), r.get<float>(), r.get<float>(), r.get<float>()};
                    (pass == 0 ? jt.rot_legacy : jt.pos_legacy).push_back(key);
                } else {
                    std::array<std::uint16_t, 4> key{r.get<std::uint16_t>(), r.get<std::uint16_t>(),
                                                     r.get<std::uint16_t>(), r.get<std::uint16_t>()};
                    (pass == 0 ? jt.rot : jt.pos).push_back(key);
                }
            }
        }
        f.joints.push_back(std::move(jt));
    }
    f.num_constraints = r.get<std::int32_t>();
    if (!r.ok) {
        err = "truncated joint data";
        return false;
    }
    if (f.num_constraints >= 0 && f.num_constraints <= kAnimMaxConstraints) {
        for (std::int32_t c = 0; c < f.num_constraints; ++c) {
            if (r.at + 86 > bytes.size()) {
                err = "truncated constraint";
                return false;
            }
            AnimConstraint con;
            std::memcpy(con.data(), bytes.data() + r.at, 86);
            r.at += 86;
            f.constraints.push_back(con);
        }
    }
    f.trailing.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.at), bytes.end());
    return true;
}

std::vector<std::uint8_t> write_anim(const AnimFile& f) {
    Writer w;
    w.put(f.version);
    w.put(f.sub_version);
    w.put(f.base_priority);
    w.put(f.duration);
    w.str(f.emote);
    w.put(f.loop_in);
    w.put(f.loop_out);
    w.put(f.loop);
    w.put(f.ease_in);
    w.put(f.ease_out);
    w.put(f.hand_pose);
    w.put(static_cast<std::uint32_t>(f.joints.size()));
    for (auto& j : f.joints) {
        w.str(j.name);
        w.put(j.priority);
        if (f.legacy()) {
            for (auto* keys : {&j.rot_legacy, &j.pos_legacy}) {
                w.put(static_cast<std::int32_t>(keys->size()));
                for (auto& k : *keys)
                    for (float v : k) w.put(v);
            }
        } else {
            for (auto* keys : {&j.rot, &j.pos}) {
                w.put(static_cast<std::int32_t>(keys->size()));
                for (auto& k : *keys)
                    for (std::uint16_t v : k) w.put(v);
            }
        }
    }
    w.put(f.num_constraints);
    for (auto& c : f.constraints) w.out.insert(w.out.end(), c.begin(), c.end());
    w.out.insert(w.out.end(), f.trailing.begin(), f.trailing.end());
    return w.out;
}

std::vector<std::string> validate_anim(const AnimFile& f, const Skeleton& skel, bool for_upload) {
    std::vector<std::string> bad;
    auto fin = [](float v) { return std::isfinite(v); };
    if (f.base_priority < -1) bad.push_back("base priority below -1");
    if (!fin(f.duration) || f.duration > kAnimMaxDuration) bad.push_back("duration over 60 s or not finite");
    if (!fin(f.loop_in) || !fin(f.loop_out) || !fin(f.ease_in) || !fin(f.ease_out))
        bad.push_back("loop or ease value not finite");
    if (f.hand_pose > 14) bad.push_back("hand pose out of range");
    if (f.joints.empty()) bad.push_back("no joints");
    if (f.joints.size() > kAnimMaxJoints) bad.push_back("more than 216 joints");
    std::vector<int> record_joint;
    for (auto& j : f.joints) {
        if (j.name == "mRoot" || j.name == "mScreen") bad.push_back("joint " + j.name + " is not allowed");
        int idx = skel.find_viewer(j.name);
        record_joint.push_back(idx);
        if (idx < 0 && for_upload) bad.push_back("unknown joint " + j.name);
        if (j.priority < -1) bad.push_back("joint " + j.name + " priority below -1");
        if (f.legacy()) {
            for (auto* keys : {&j.rot_legacy, &j.pos_legacy})
                for (auto& k : *keys)
                    if (!fin(k[0]) || !fin(k[1]) || !fin(k[2]) || !fin(k[3])) bad.push_back("non-finite key in " + j.name);
        } else {
            for (auto& k : j.rot) {
                float t = u16_to_f32(k[0], 0.f, f.duration);
                if (t < 0 || t > f.duration) bad.push_back("rotation key time out of range in " + j.name);
            }
        }
    }
    for (auto& c : f.constraints) {
        std::uint8_t chain = c[0];
        if (chain > f.joints.size()) bad.push_back("constraint chain longer than the joint list");
        if (c[1] >= 2) bad.push_back("unknown constraint type");
        int src = skel.find_volume(fixed_name(c.data() + 2));
        std::string target = fixed_name(c.data() + 30);
        if (src < 0) bad.push_back("unknown constraint source volume");
        if (target != "GROUND" && skel.find_volume(target) < 0) bad.push_back("unknown constraint target volume");
        for (size_t off : {18, 22, 26, 46, 50, 54, 58, 62, 66, 70, 74, 78, 82})
            if (!fin(get_f32(c, off))) bad.push_back("constraint value not finite");
        if (src >= 0) {
            // The source volume's joint and chain_length ancestors must all have records.
            int jnt = skel.volumes()[src].joint;
            for (int k = 0; k <= chain && jnt >= 0; ++k, jnt = skel[jnt].parent) {
                bool found = false;
                for (int rj : record_joint) found = found || rj == jnt;
                if (!found) {
                    bad.push_back("constraint chain joint " + skel[jnt].name + " has no record");
                    break;
                }
            }
        }
    }
    return bad;
}

bool is_known_emote(const std::string& name) {
    for (const char* e : kEmotes)
        if (name == e) return true;
    return false;
}

bool parse_anim(const std::vector<std::uint8_t>& bytes, AnimFile& f, std::string& err) {
    return guarded(err, [&] { return parse_anim_bytes(bytes, f, err); });
}

}  // namespace vats
