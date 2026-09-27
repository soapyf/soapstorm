// Viewport Avatar Toolset - retargeting humanoid animations from other skeletons onto SL's.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/07. The method works on world rotations: for each mapped SL joint,
//   W_sl(t) = D(t) * A * W_sl_rest,  D(t) = W_src(t) * W_src_rest^-1
// where A is the swing that turns the SL rest bone onto the source rest bone (so an A-pose source
// drives SL's T-pose arms correctly, RT-6), and everything is first turned into SL axes by C
// (up axis to +Z, left hip to +Y). Unmapped source bones between mapped ones are covered because
// world rotations already include them.
#include "vats/retarget.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

#include "vats/anim_convert.h"
#include "vats/anim_file.h"
#include "vats/curve_ops.h"
#include "vats/json.h"
#include "guard.h"

namespace vats {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "mixamorig:Hips" -> "hips"
std::string key_name(const std::string& s) { return lower(s.substr(s.find_last_of(':') + 1)); }

// The shortest rotation taking direction a onto direction b.
Quat from_to(const Vec3& a, const Vec3& b) {
    Vec3 u = a.normalized(), v = b.normalized();
    double d = u.dot(v);
    if (d > 1 - 1e-12) return {};
    if (d < -1 + 1e-12) {  // opposite: any perpendicular axis
        Vec3 axis = std::fabs(u.x) < 0.9 ? u.cross({1, 0, 0}) : u.cross({0, 1, 0});
        return Quat::axis_angle(axis, kPi);
    }
    Vec3 c = u.cross(v);
    return Quat{1 + d, c.x, c.y, c.z}.normalized();
}

struct World {
    std::vector<Quat> rot;
    std::vector<Vec3> pos;
};

// Source FK for one frame (uniform scales).
World source_fk(const SourceAnim& src, const std::vector<Quat>& rot, const std::vector<Vec3>& pos) {
    const size_t n = src.joints.size();
    World w{std::vector<Quat>(n), std::vector<Vec3>(n)};
    std::vector<double> scale(n, 1);
    for (size_t j = 0; j < n; ++j) {
        int p = src.joints[j].parent;
        if (p < 0) {
            w.rot[j] = rot[j];
            w.pos[j] = pos[j];
            scale[j] = src.joints[j].scale;
        } else {
            w.rot[j] = w.rot[p] * rot[j];
            w.pos[j] = w.pos[p] + w.rot[p].rotate(pos[j] * scale[p]);
            scale[j] = scale[p] * src.joints[j].scale;
        }
    }
    return w;
}

// SL joints a mapping can fill (RT-4); Bento spine joints are left to their rest pose.
const std::vector<std::string>& sl_targets() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v{"mPelvis", "mTorso", "mChest", "mNeck", "mHead"};
        for (const char* side : {"Left", "Right"}) {
            for (const char* b : {"mCollar", "mShoulder", "mElbow", "mWrist", "mHip", "mKnee", "mAnkle", "mFoot", "mToe"})
                v.push_back(std::string(b) + side);
            for (const char* f : {"Thumb", "Index", "Middle", "Ring", "Pinky"})
                for (int i = 1; i <= 3; ++i) v.push_back("mHand" + std::string(f) + std::to_string(i) + side);
        }
        return v;
    }();
    return names;
}

// The SL joint whose direction from `joint` defines the bone for the rest alignment.
std::string aim_of(const std::string& joint) {
    static const std::map<std::string, std::string> base = {
        {"mTorso", "mChest"},          {"mChest", "mNeck"},          {"mNeck", "mHead"},
        {"mCollarLeft", "mShoulderLeft"}, {"mShoulderLeft", "mElbowLeft"}, {"mElbowLeft", "mWristLeft"},
        {"mWristLeft", "mHandMiddle1Left"}, {"mHipLeft", "mKneeLeft"},   {"mKneeLeft", "mAnkleLeft"},
        {"mAnkleLeft", "mFootLeft"},   {"mFootLeft", "mToeLeft"}};
    auto it = base.find(joint);
    if (it != base.end()) return it->second;
    std::string left = joint;
    if (auto r = left.find("Right"); r != std::string::npos) {
        left.replace(r, 5, "Left");
        auto l = base.find(left);
        if (l != base.end()) {
            std::string a = l->second;
            a.replace(a.find("Left"), 4, "Right");
            return a;
        }
    }
    if (joint.rfind("mHand", 0) == 0) {  // finger segment n aims at n + 1
        for (size_t i = 0; i < joint.size(); ++i)
            if (joint[i] == '1' || joint[i] == '2') {
                std::string a = joint;
                a[i] = char(joint[i] + 1);
                return a;
            }
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// BVH source (any skeleton)

static bool read_bvh(std::string_view text, SourceAnim& out, std::string& err) {
    std::vector<std::string> tok;
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        size_t b = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i > b) tok.emplace_back(text.substr(b, i - b));
    }
    size_t t = 0;
    auto next = [&]() -> std::string { return t < tok.size() ? tok[t++] : std::string(); };
    auto number = [&](double& v) {
        std::string s = next();
        char* end = nullptr;
        v = std::strtod(s.c_str(), &end);
        return !s.empty() && end && *end == 0 && std::isfinite(v);
    };
    out = SourceAnim{};
    std::vector<std::vector<std::string>> channels;
    // ROOT/JOINT blocks; End Site blocks are skipped.
    std::function<bool(int, bool, int)> block = [&](int parent, bool end_site, int depth) -> bool {
        if (depth > 256) return false;
        std::string name = tok[t - 1];
        if (next() != "{") return false;
        int me = -1;
        if (!end_site) {
            me = int(out.joints.size());
            out.joints.push_back({name, parent, {}, {}, 1});
            channels.emplace_back();
        }
        for (;;) {
            std::string k = next();
            if (k.empty()) return false;
            if (k == "}") return true;
            if (k == "OFFSET") {
                Vec3 o;
                for (int a = 0; a < 3; ++a)
                    if (!number(o[a])) return false;
                if (me >= 0) out.joints[me].offset = o;
            } else if (k == "CHANNELS" && me >= 0) {
                double n;
                if (!number(n) || n < 0 || n > 64) return false;
                for (int c = 0; c < int(n); ++c) channels[me].push_back(next());
            } else if (k == "JOINT") {
                next();
                if (!block(me, false, depth + 1)) return false;
            } else if (k == "End") {
                next();  // "Site"
                if (!block(me, true, depth + 1)) return false;
            } else {
                return false;
            }
        }
    };
    if (next() != "HIERARCHY" || next() != "ROOT") return err = "not a BVH file", false;
    next();
    if (!block(-1, false, 0)) return err = "malformed BVH hierarchy", false;
    double nframes = 0, frame_time = 0;
    if (next() != "MOTION" || next() != "Frames:" || !number(nframes) || nframes < 1 || nframes > 1e6 ||
        next() != "Frame" || next() != "Time:" || !number(frame_time) || frame_time <= 0 || frame_time > 60)
        return err = "malformed BVH motion header", false;
    size_t per_frame = 0;
    for (auto& c : channels) per_frame += c.size();
    const int N = int(nframes);
    if (per_frame == 0) return err = "the BVH file has no motion channels", false;
    if (tok.size() - t < per_frame * size_t(N)) return err = "not enough BVH motion data", false;
    if (double(N) * double(out.joints.size()) > kMaxSamples) return err = "the BVH animation is too large", false;
    out.fps = 1.0 / frame_time;
    out.rot.assign(N, std::vector<Quat>(out.joints.size()));
    out.pos.assign(N, std::vector<Vec3>(out.joints.size()));
    static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int f = 0; f < N; ++f)
        for (size_t j = 0; j < out.joints.size(); ++j) {
            Quat q;
            Vec3 p = out.joints[j].offset;
            for (auto& ch : channels[j]) {
                double v = std::strtod(tok[t++].c_str(), nullptr);
                if (!std::isfinite(v) || ch.size() != 9) continue;
                int a = ch[0] - 'X';
                if (a < 0 || a > 2) continue;
                if (ch.compare(1, 8, "position") == 0) p[a] = v;
                else if (ch.compare(1, 8, "rotation") == 0) q = q * Quat::axis_angle(axes[a], v * kDegToRad);
            }
            out.rot[f][j] = q;
            out.pos[f][j] = p;
        }
    return true;
}

bool read_bvh_source(std::string_view text, SourceAnim& out, std::string& err) {
    return guarded(err, [&] { return read_bvh(text, out, err); });
}

// ---------------------------------------------------------------------------------------------
// Tables and mapping

static bool parse_table(std::string_view json, RigTable& out, std::string& err) {
    Json j;
    if (!parse_json(json, j, err)) return false;
    const Json *name = j.find("name"), *bones = j.find("bones"), *mirror = j.find("mirror");
    if (!name || !name->is_string() || !bones || !bones->is_object()) return err = "a rig table needs name and bones", false;
    out = RigTable{name->str, {}, {}};
    if (const Json* hint = j.find("hint"); hint && hint->is_string()) out.hint = hint->str;
    std::vector<std::pair<std::string, std::string>> pairs;
    if (mirror && mirror->is_array())
        for (auto& p : mirror->arr)
            if (p.is_array() && p.arr.size() == 2 && p.arr[0].is_string() && p.arr[1].is_string() && !p.arr[0].str.empty())
                pairs.emplace_back(p.arr[0].str, p.arr[1].str);
    auto swap_side = [&](std::string s) {
        for (auto& [a, b] : pairs)
            for (size_t i = s.find(a); i != std::string::npos; i = s.find(a, i + b.size())) s.replace(i, a.size(), b);
        return s;
    };
    for (auto& [sl, list] : bones->obj) {
        std::vector<std::string> names;
        for (auto& n : list.arr)
            if (n.is_string()) names.push_back(n.str);
        if (list.is_string()) names.push_back(list.str);
        out.bones[sl] = names;
        // Tables list the left side; the right side is derived with the mirror pairs.
        if (sl.find("Left") != std::string::npos) {
            std::string right = sl;
            right.replace(right.find("Left"), 4, "Right");
            std::vector<std::string> rn;
            for (auto& n : names) rn.push_back(swap_side(n));
            out.bones[right] = rn;
        }
    }
    return true;
}

bool parse_rig_table(std::string_view json, RigTable& out, std::string& err) {
    return guarded(err, [&] { return parse_table(json, out, err); });
}

int apply_rig_table(const RigTable& table, const SourceAnim& src, BoneMap& out) {
    out.clear();
    std::map<std::string, int> by_name;
    for (int j = int(src.joints.size()) - 1; j >= 0; --j) by_name[key_name(src.joints[j].name)] = j;  // first wins
    for (auto& [sl, names] : table.bones)
        for (auto& n : names)
            if (auto it = by_name.find(key_name(n)); it != by_name.end()) {
                out[sl] = it->second;
                break;
            }
    return int(out.size());
}

int best_rig_table(const std::vector<RigTable>& tables, const SourceAnim& src, BoneMap& out) {
    int best = -1, best_score = 0;
    for (int i = 0; i < int(tables.size()); ++i) {
        BoneMap m;
        int score = 2 * apply_rig_table(tables[i], src, m);
        if (score == 0) continue;
        if (!tables[i].hint.empty())
            for (auto& j : src.joints)
                if (lower(j.name).find(lower(tables[i].hint)) != std::string::npos) {
                    ++score;
                    break;
                }
        if (score > best_score) best = i, best_score = score, out = m;
    }
    return best;
}

bool map_is_usable(const BoneMap& map, std::vector<std::string>* missing) {
    std::vector<std::string> need{"mPelvis", "mHead"};
    for (const char* s : {"Left", "Right"})
        for (const char* b : {"mShoulder", "mElbow", "mHip", "mKnee"}) need.push_back(std::string(b) + s);
    bool ok = map.count("mTorso") || map.count("mChest");
    if (!ok && missing) missing->push_back("mTorso or mChest");
    for (auto& n : need)
        if (!map.count(n)) {
            ok = false;
            if (missing) missing->push_back(n);
        }
    return ok;
}

std::vector<std::string> retarget_joints(const Skeleton& skel) {
    std::vector<std::string> out;
    for (auto& n : sl_targets())
        if (skel.find(n) >= 0) out.push_back(n);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Retarget (RT-6..RT-8)

RetargetResult retarget(const Skeleton& skel, const SourceAnim& src, const BoneMap& map, const RetargetOptions& opt) {
    RetargetResult res;
    Clip& clip = res.clip;
    const int N = src.frames();
    if (N == 0 || src.joints.empty() || !map.count("mPelvis")) {
        res.report.push_back("nothing to retarget: no frames or no pelvis");
        return res;
    }

    // Rest pose: the file's bind pose, or frame 0 (RT-7).
    std::vector<Quat> rest_rot(src.joints.size());
    std::vector<Vec3> rest_pos(src.joints.size());
    const bool frame0 = opt.rest_from_frame0 || !src.has_bind;
    for (size_t j = 0; j < src.joints.size(); ++j) {
        rest_rot[j] = frame0 ? src.rot[0][j] : src.joints[j].rot;
        rest_pos[j] = frame0 ? src.pos[0][j] : src.joints[j].offset;
    }
    if (frame0) res.report.push_back("rest pose taken from frame 0");
    const World rest = source_fk(src, rest_rot, rest_pos);
    auto src_of = [&](const char* sl) { auto it = map.find(sl); return it == map.end() ? -1 : it->second; };

    // C: source axes to SL axes. Up is the dominant axis from pelvis to head; then a turn about Z
    // puts the left hip (or shoulder) on +Y, which makes +X forward.
    const int pelvis = src_of("mPelvis");
    int top = src_of("mHead");
    if (top < 0) top = src_of("mChest");
    Vec3 up = top >= 0 ? rest.pos[top] - rest.pos[pelvis] : Vec3{0, 1, 0};
    Vec3 up_axis;
    {
        int a = 0;
        for (int i = 1; i < 3; ++i)
            if (std::fabs(up[i]) > std::fabs(up[a])) a = i;
        up_axis[a] = up[a] < 0 ? -1 : 1;
    }
    Quat C = from_to(up_axis, {0, 0, 1});
    int l = src_of("mHipLeft"), r = src_of("mHipRight");
    if (l < 0 || r < 0) l = src_of("mShoulderLeft"), r = src_of("mShoulderRight");
    if (l >= 0 && r >= 0) {
        Vec3 side = C.rotate(rest.pos[l] - rest.pos[r]);
        C = Quat::axis_angle({0, 0, 1}, kPi / 2 - std::atan2(side.y, side.x)) * C;
    }
    res.report.push_back(std::string("source up axis: ") + (up_axis.y != 0 ? "Y" : up_axis.z != 0 ? "Z" : "X") +
                         (up_axis[0] + up_axis[1] + up_axis[2] < 0 ? " (down)" : ""));
    auto to_sl_rot = [&](const Quat& q) { return C * q * C.conj(); };

    // SL rest pose and the per-joint alignment A.
    Pose zero(skel.size());
    const std::vector<Xform> G = skel.global_pose(zero, opt.shape);
    std::vector<int> mapped(skel.size(), -1);
    std::vector<Quat> align(skel.size());
    for (auto& [sl, j] : map) {
        int node = skel.find(sl);
        if (node < 0 || j < 0 || j >= int(src.joints.size())) continue;
        mapped[node] = j;
    }
    for (int node = 0; node < skel.size(); ++node) {
        if (mapped[node] < 0) continue;
        std::string aim = aim_of(skel[node].name);
        int an = aim.empty() ? -1 : skel.find(aim);
        if (an < 0 || mapped[an] < 0) continue;
        Vec3 dir_sl = G[an].pos - G[node].pos;
        Vec3 dir_src = C.rotate(rest.pos[mapped[an]] - rest.pos[mapped[node]]);
        if (dir_sl.length() > 1e-6 && dir_src.length() > 1e-9) align[node] = from_to(dir_sl, dir_src);
    }

    // RT-8: hip travel scaled by pelvis-to-ankle height (knee when ankles are unmapped).
    double scale = 1;
    for (const char* low : {"mAnkleLeft", "mKneeLeft"}) {
        int sn = skel.find(low), sj = src_of(low);
        if (sn < 0 || sj < 0) continue;
        double sl_h = G[skel.find("mPelvis")].pos.z - G[sn].pos.z;
        double src_h = C.rotate(rest.pos[pelvis] - rest.pos[sj]).z;
        if (src_h > 1e-9) scale = sl_h / src_h;
        break;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "hip movement scaled by %.4g", scale);
    res.report.push_back(buf);
    for (const std::string& sl : retarget_joints(skel))
        if (!map.count(sl) && sl.rfind("mHand", 0) != 0) res.report.push_back(sl + " has no source bone; it keeps its rest pose");

    // Output rate: the source's, rounded and kept to 1..60 unless asked otherwise.
    const int fps = opt.fps > 0 ? opt.fps : std::clamp(int(std::lround(src.fps)), 1, 60);
    const double duration = (N - 1) / src.fps;
    // At most 10 minutes (split_to_fit cuts that into SL-sized parts). Rounded, not floored: a BVH frame time
    // of 0.0333333 gives a source rate a hair over 30, and the last frame must not be lost to that.
    const int frames = int(std::lround(std::clamp(duration, 0.0, kMaxSeconds) * fps)) + 1;
    if (duration > kMaxSeconds) res.report.push_back("only the first 10 minutes were read");
    clip.fps = fps;
    clip.end_frame = std::max(frames - 1, 1);
    clip.loop_in = 0;
    clip.loop_out = clip.end_frame;
    clip.priority = 3;
    clip.ease_in = clip.ease_out = 0.3;

    const int pelvis_node = skel.find("mPelvis");
    std::vector<Vec3> last_euler(skel.size());
    std::vector<Quat> rot_f(src.joints.size());
    std::vector<Vec3> pos_f(src.joints.size());
    for (int k = 0; k < frames; ++k) {
        // Sample the source at this output time.
        double s = std::min(k / double(fps) * src.fps, double(N - 1));
        int a = int(std::floor(s)), b = std::min(a + 1, N - 1);
        double u = s - a;
        for (size_t j = 0; j < src.joints.size(); ++j) {
            rot_f[j] = nlerp(src.rot[a][j], src.rot[b][j], u);
            pos_f[j] = src.pos[a][j] + (src.pos[b][j] - src.pos[a][j]) * u;
        }
        const World w = source_fk(src, rot_f, pos_f);
        std::vector<Quat> world_sl(skel.size());
        for (int node = 0; node < skel.size(); ++node) {
            const Node& n = skel[node];
            Quat parent = n.parent >= 0 ? world_sl[n.parent] : Quat{};
            if (mapped[node] < 0) {
                world_sl[node] = parent * n.rest;
                continue;
            }
            int j = mapped[node];
            Quat D = to_sl_rot(w.rot[j] * rest.rot[j].conj());
            world_sl[node] = (D * align[node] * G[node].rot).normalized();
            Quat key = n.rest.conj() * parent.conj() * world_sl[node];
            last_euler[node] = nearest_euler(key, last_euler[node]);
            Track& tr = clip.curves[n.name];
            for (int c = 0; c < 3; ++c) {
                Key kk;
                kk.frame = k;
                kk.value = last_euler[node][c];
                kk.interp = Interp::Linear;
                tr[kRotChannels[c]].keys.push_back(kk);
            }
            if (node == pelvis_node) {
                Vec3 off = C.rotate(w.pos[j] - rest.pos[j]) * scale;
                for (int c = 0; c < 3; ++c) {
                    Key kk;
                    kk.frame = k;
                    kk.value = off[c];
                    kk.interp = Interp::Linear;
                    tr[kPosChannels[c]].keys.push_back(kk);
                }
            }
        }
    }
    for (auto& [name, tr] : clip.curves)
        for (auto& [ch, c] : tr) c.recompute_handles();
    return res;
}

// ---------------------------------------------------------------------------------------------
// Fitting into SL's limits (RT-10, RT-11)

// Drops keys the reducers find redundant, per track. Rotation channels are reduced together when
// they share key frames (retargeted clips do); positions likewise.
void reduce_clip_keys(Clip& clip, double rot_deg, double pos_m) {
    auto reduce = [](Track& tr, const char* const (&ch)[3], bool rot, double tol) {
        FCurve *c[3];
        for (int a = 0; a < 3; ++a) {
            auto it = tr.find(ch[a]);
            if (it == tr.end()) return;
            c[a] = &it->second;
        }
        const size_t n = c[0]->keys.size();
        if (n < 3 || c[1]->keys.size() != n || c[2]->keys.size() != n) return;
        for (size_t k = 0; k < n; ++k)
            if (c[1]->keys[k].frame != c[0]->keys[k].frame || c[2]->keys[k].frame != c[0]->keys[k].frame) return;
        std::vector<int> kept;
        if (rot) {
            std::vector<Quat> q(n);
            for (size_t k = 0; k < n; ++k) q[k] = euler_to_quat({c[0]->keys[k].value, c[1]->keys[k].value, c[2]->keys[k].value});
            kept = reduce_rotation_keys(q, tol, 60);
        } else {
            std::vector<Vec3> v(n);
            for (size_t k = 0; k < n; ++k) v[k] = {c[0]->keys[k].value, c[1]->keys[k].value, c[2]->keys[k].value};
            kept = reduce_position_keys(v, tol, 60);
        }
        for (int a = 0; a < 3; ++a) {
            std::vector<Key> out;
            for (int k : kept) out.push_back(c[a]->keys[k]);
            c[a]->keys = std::move(out);
            c[a]->recompute_handles();
        }
    };
    for (auto& [name, tr] : clip.curves) {
        reduce(tr, kRotChannels, true, rot_deg);
        reduce(tr, kPosChannels, false, pos_m);
    }
}

FitReport fit_to_limits(const Skeleton& skel, Clip& clip, const FitOptions& opt) {
    FitReport rep;
    const Clip original = clip;
    Clip base = clip;  // full rate, after fps changes and joint drops; reduced into `clip` per try
    double rot = 0.05, pos = 0.0005;  // the export defaults (IO-14)
    auto size_of = [&](const Clip& c) {  // exported with the same tolerances, as the app will (IO-14)
        AnimExportOptions o;
        o.reduce_rot_deg = rot;
        o.reduce_pos_m = pos;
        o.shape = opt.shape;
        return write_anim(export_anim(skel, c, o).file).size();
    };
    auto fits = [&](size_t bytes) { return bytes < kAnimMaxUploadBytes; };
    auto attempt = [&] {
        clip = base;
        reduce_clip_keys(clip, rot, pos);
        return size_of(clip);
    };
    rep.bytes_before = size_of(original);
    rep.too_long = clip.end_frame / double(clip.fps) > 60;
    size_t bytes = attempt();

    auto drop = [&](const char* what, const std::function<bool(const Node&)>& pick) {
        std::vector<std::string> names;
        for (auto& [name, tr] : base.curves) {
            int n = skel.find(name);
            if (n >= 0 && pick(skel[n])) names.push_back(name);
        }
        if (names.empty()) return;
        for (auto& n : names) base.curves.erase(n);
        rep.dropped.insert(rep.dropped.end(), names.begin(), names.end());
        rep.steps.push_back(std::string("dropped ") + what + " (" + std::to_string(names.size()) + " joints)");
        bytes = attempt();
    };

    if (!fits(bytes) && opt.allow_tolerance) {
        static const double ladder[][2] = {{0.25, 0.001}, {0.5, 0.002}, {1, 0.005}, {2, 0.01}};
        for (auto& l : ladder) {
            rot = l[0], pos = l[1];
            bytes = attempt();
            char buf[96];
            std::snprintf(buf, sizeof buf, "key reduction tolerance %.2g° / %.3g mm", rot, pos * 1000);
            rep.steps.push_back(buf);
            if (fits(bytes)) break;
        }
    }
    if (!fits(bytes) && opt.allow_fps)
        for (int target : {24, 15, 10}) {
            if (base.fps <= target) continue;
            retime_clip(base, target);
            rep.steps.push_back("frame rate lowered to " + std::to_string(target) + " fps");
            bytes = attempt();
            if (fits(bytes)) break;
        }
    if (!fits(bytes) && opt.allow_drop_face) drop("face joints", [](const Node& n) { return n.category == Category::Face; });
    if (!fits(bytes) && opt.allow_drop_fingers)
        drop("finger segments past the first", [](const Node& n) {
            return n.name.rfind("mHand", 0) == 0 && (n.name.find('2') != std::string::npos || n.name.find('3') != std::string::npos);
        });
    if (!fits(bytes) && opt.allow_drop_toes) drop("toes", [](const Node& n) { return n.name.rfind("mToe", 0) == 0; });

    rep.bytes_after = bytes;
    rep.fits = fits(bytes) && !rep.too_long;
    rep.rot_tol_deg = rot;
    rep.pos_tol_m = pos;
    rep.fps = clip.fps;
    Json reduce = Json::array();
    reduce.push(rot);
    reduce.push(pos);
    clip.export_settings.set("reduce", reduce);
    if (rep.too_long) rep.steps.push_back("still over 60 s: trim or split the clip");
    return rep;
}

Clip slice_clip(const Clip& clip, int a, int b) {
    a = std::clamp(a, 0, clip.end_frame);
    b = std::clamp(b, a, clip.end_frame);
    Clip out = clip;
    for (auto& [name, tr] : out.curves)
        for (auto& [ch, c] : tr) {
            if (c.empty()) continue;
            insert_on_curve(c, a);
            insert_on_curve(c, b);
            c.keys.erase(std::remove_if(c.keys.begin(), c.keys.end(),
                                        [&](const Key& k) { return k.frame < a - 1e-6 || k.frame > b + 1e-6; }),
                         c.keys.end());
            for (Key& k : c.keys) k.frame -= a, k.lx -= a, k.rx -= a;
        }
    out.pins.clear();
    for (Pin p : clip.pins) {
        const int to = p.to < 0 ? clip.end_frame : p.to;
        if (to < a || p.from > b) continue;
        p.from = std::max(p.from, a) - a;
        p.to = p.to < 0 ? -1 : std::min(p.to, b) - a;
        auto shift = [&](int k) { return k >= a && k <= b ? k - a : -1; };
        p.start_key = shift(p.start_key), p.release_key = shift(p.release_key);
        out.pins.push_back(p);
    }
    out.end_frame = b - a;
    out.loop_in = std::clamp(clip.loop_in - a, 0, out.end_frame);
    out.loop_out = std::clamp(clip.loop_out - a, out.loop_in, out.end_frame);
    return out;
}

std::vector<Clip> split_to_fit(const Skeleton& skel, const Clip& clip, const FitOptions& opt, std::vector<FitReport>* reports) {
    const int fps = std::max(clip.fps, 1), frames = std::max(clip.end_frame, 1);
    const int min_parts = (frames + 60 * fps - 1) / (60 * fps);  // each part at most 60 s
    // ponytail: grows the part count by half each failed try instead of searching for the fewest parts.
    for (int n = std::max(min_parts, 2); frames / n >= 2 * fps; n += std::max(1, n / 2)) {
        std::vector<Clip> parts;
        std::vector<FitReport> reps;
        const int len = (frames + n - 1) / n;
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {
            Clip part = slice_clip(clip, k * len, std::min((k + 1) * len, frames));
            FitReport r = fit_to_limits(skel, part, opt);
            ok = r.fits;
            parts.push_back(std::move(part));
            reps.push_back(std::move(r));
        }
        if (!ok) continue;
        if (reports) *reports = std::move(reps);
        return parts;
    }
    return {};
}

}  // namespace vats
