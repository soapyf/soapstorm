// Viewport Avatar Toolset - the body picker: pages of joint dots and bone lines, their groups, framing and click tests.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/picker.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "vats/clip.h"
#include "vats/pose_ops.h"
#include "vats/pose_presets.h"

namespace vats {
namespace {

const char* const kSides[2] = {"Right", "Left"};  // facing the viewer, the avatar's right is drawn on the left
const char* const kFingers[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};

std::string sided(const std::string& s, const char* side) {
    std::string out = s;
    if (auto at = out.find('@'); at != std::string::npos) out.replace(at, 1, side);
    return out;
}

Quat rot_z(double a) { return Quat::axis_angle({0, 0, 1}, a); }
Quat rot_y(double a) { return Quat::axis_angle({0, 1, 0}, a); }

// The rotation taking the unit vector a to x and b to z (a, b orthogonal; x, z orthogonal).
Quat basis_to(const Vec3& a, const Vec3& b, const Vec3& x, const Vec3& z) {
    // Rotation matrix M with M a = x, M b = z, M (a x b)... built as (target frame) * (source frame)^T.
    const Vec3 c = b.cross(a), y = z.cross(x);  // right-handed thirds of each frame (a, c, b) and (x, y, z)
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = x[i] * a[j] + y[i] * c[j] + z[i] * b[j];
    const double tr = m[0][0] + m[1][1] + m[2][2];
    Quat q;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        q = {s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {(m[2][1] - m[1][2]) / s, s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s};
    } else {
        const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4};
    }
    return q.normalized();
}

Vec3 tip_of(const Skeleton& skel, const std::vector<Xform>& g, const Shape* shape, int n) {
    const Vec3 end = shape && size_t(n) < shape->scale.size() ? skel[n].end.mul(shape->scale[n]) : skel[n].end;
    return g[size_t(n)].apply(end);
}

// The view's turn of the body, after it faces the viewer: Front, Back; Extras' Wings (from behind), Tail (from the
// avatar's right) and Hind (from behind its right shoulder, 35 degrees round and 10 up, so the hind limbs stand apart
// from each other and from the legs).
Quat view_turn(PickerPage page, int view) {
    if (page == PickerPage::Body) return view == 1 ? rot_z(kPi) : Quat{};
    if (page == PickerPage::Extras) {
        if (view == 0) return rot_z(kPi);
        if (view == 1) return rot_z(kPi / 2);
        return rot_y(10 * kDegToRad) * rot_z(kPi / 2 + 35 * kDegToRad);
    }
    return {};
}

// A rigid transform whose rotation is r, taking the point `at` to `to`.
Xform turn_about(const Quat& r, const Vec3& at, const Vec3& to = {}) { return {r, to - r.rotate(at)}; }

}  // namespace

double segment_distance(V2 p, V2 a, V2 b) {
    const V2 d = b - a;
    const double l2 = d.dot(d);
    const double t = l2 > 0 ? std::clamp((p - a).dot(d) / l2, 0.0, 1.0) : 0.0;
    return (p - (a + d * t)).length();
}

const char* picker_page_name(PickerPage p) {
    static const char* const names[] = {"Body", "Hands", "Face", "Extras"};
    return names[int(p)];
}

int picker_view_count(PickerPage p) { return p == PickerPage::Face ? 1 : p == PickerPage::Extras ? 3 : 2; }

const char* picker_view_name(PickerPage p, int view) {
    switch (p) {
        case PickerPage::Body: return view ? "Back" : "Front";
        case PickerPage::Hands: return view ? "Palm" : "Back";
        case PickerPage::Face: return "Face";
        case PickerPage::Extras: return view == 0 ? "Wings" : view == 1 ? "Tail" : "Hind";
    }
    return "";
}

Pose picker_chart_pose(const Skeleton& skel) {
    Clip clip;
    LibraryItem arms;
    arms.kind = "pose";
    arms.bones["mShoulderLeft"] = {-50, 0, 0};
    arms.bones["mShoulderRight"] = {50, 0, 0};
    apply_pose(clip, skel, arms, 0, false);
    for (const LibraryItem& it : builtin_poses(skel))
        if (it.id == "builtin:hand-open") apply_pose(clip, skel, it, 0, false), apply_pose(clip, skel, it, 0, true);
    return evaluate_curves(skel, clip, 0);
}

// --- groups -------------------------------------------------------------------------------------------------------

const std::vector<PickerGroup>& picker_groups() {
    static const std::vector<PickerGroup> groups = [] {
        std::vector<PickerGroup> g;
        auto add = [&](PickerPage page, PickerGroupKind kind, int view, std::string label, std::string name,
                       std::vector<std::string> bones) {
            g.push_back({page, kind, view, std::move(label), std::move(name), std::move(bones)});
        };
        const auto B = PickerPage::Body, H = PickerPage::Hands, F = PickerPage::Face, X = PickerPage::Extras;
        const auto L = PickerGroupKind::Label;
        add(B, L, -1, "HEAD", "Head", {"mNeck", "mHead"});
        add(B, L, -1, "SPINE", "Spine", {"mPelvis", "mTorso", "mChest"});
        for (const char* s : kSides) {
            const std::string side = s, sh = side.substr(0, 1);
            add(B, L, -1, sh + " ARM", side + " Arm",
                {"mCollar" + side, "mShoulder" + side, "mElbow" + side, "mWrist" + side});
            add(B, L, -1, sh + " LEG", side + " Leg",
                {"mHip" + side, "mKnee" + side, "mAnkle" + side, "mFoot" + side, "mToe" + side});
        }
        // Hands: the fingertip circles first, so a finger's dots belong to its finger.
        for (const char* s : kSides)
            for (const char* f : kFingers) {
                std::vector<std::string> b;
                for (int k = 1; k <= 3; ++k) b.push_back(std::string("mHand") + f + std::to_string(k) + s);
                add(H, PickerGroupKind::Finger, -1, f,
                    std::string(s) + " " + f + (std::string(f) == "Thumb" ? "" : " finger"), b);
            }
        for (const char* s : kSides) {
            std::vector<std::string> b;
            for (const char* f : kFingers)
                for (int k = 1; k <= 3; ++k) b.push_back(std::string("mHand") + f + std::to_string(k) + s);
            std::string up = s;
            for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
            add(H, L, -1, up + " HAND", std::string(s) + " Hand", b);
        }
        for (int k = 1; k <= 3; ++k) {
            std::vector<std::string> b;
            for (const char* s : kSides)
                for (const char* f : kFingers) b.push_back(std::string("mHand") + f + std::to_string(k) + s);
            add(H, PickerGroupKind::Row, -1, std::to_string(k), "Knuckle row " + std::to_string(k), b);
        }
        // Face chips: both sides of a feature.
        auto both = [](std::initializer_list<const char*> parts) {
            std::vector<std::string> out;
            for (const char* p : parts)
                for (const char* s : kSides) out.push_back(sided(p, s));
            return out;
        };
        const auto C = PickerGroupKind::Chip;
        add(F, C, -1, "Brows", "Brows", both({"mFaceEyebrowOuter@", "mFaceEyebrowCenter@", "mFaceEyebrowInner@"}));
        add(F, C, -1, "Eyes", "Eyes", both({"mEye@", "mFaceEyeAlt@"}));
        add(F, C, -1, "Lids", "Lids", both({"mFaceEyeLidUpper@", "mFaceEyeLidLower@", "mFaceEyecornerInner@"}));
        {
            std::vector<std::string> b = {"mFaceNoseBridge", "mFaceNoseCenter", "mFaceNoseBase"};
            for (auto& x : both({"mFaceNose@"})) b.push_back(x);
            add(F, C, -1, "Nose", "Nose", b);
        }
        add(F, C, -1, "Cheeks", "Cheeks", both({"mFaceCheekUpper@", "mFaceCheekLower@"}));
        {
            std::vector<std::string> b = both({"mFaceLipCorner@", "mFaceLipUpper@", "mFaceLipLower@"});
            b.push_back("mFaceLipUpperCenter");
            b.push_back("mFaceLipLowerCenter");
            add(F, C, -1, "Lips", "Lips", b);
        }
        add(F, C, -1, "Jaw", "Jaw", {"mFaceJaw", "mFaceChin"});
        {
            std::vector<std::string> b = both({"mFaceForehead@"});
            b.push_back("mFaceForeheadCenter");
            add(F, C, -1, "Forehead", "Forehead", b);
        }
        add(F, C, -1, "Ears", "Ears", both({"mFaceEar1@", "mFaceEar2@"}));
        add(F, C, -1, "Mouth", "Mouth (teeth, tongue, jaw shaper)",
            {"mFaceTeethUpper", "mFaceTeethLower", "mFaceTongueBase", "mFaceTongueTip", "mFaceJawShaper"});
        {
            std::vector<std::string> b = {"mWingsRoot"};
            for (const char* s : kSides) {
                for (int k = 1; k <= 4; ++k) b.push_back("mWing" + std::to_string(k) + s);
                b.push_back(std::string("mWing4Fan") + s);
            }
            add(X, L, 0, "WINGS", "Wings", b);
        }
        {
            std::vector<std::string> b;
            for (int k = 1; k <= 6; ++k) b.push_back("mTail" + std::to_string(k));
            add(X, L, 1, "TAIL", "Tail", b);
        }
        add(X, L, 1, "GROIN", "Groin", {"mGroin"});
        {
            std::vector<std::string> b = {"mHindLimbsRoot"};
            for (const char* s : kSides)
                for (int k = 1; k <= 4; ++k) b.push_back("mHindLimb" + std::to_string(k) + s);
            add(X, L, 2, "HIND LIMBS", "Hind Limbs", b);
        }
        return g;
    }();
    return groups;
}

std::vector<int> picker_group_nodes(const Skeleton& skel, const PickerGroup& g) {
    std::vector<int> out;
    for (const std::string& b : g.bones)
        if (int i = skel.find(b); i >= 0 && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    return out;
}

namespace {

// A page view's chains of bones: a line from each bone to the next, the last to its end.
struct Chain {
    std::vector<std::string> bones;
    bool palm = false;  // Hands: the first bone (the wrist) is only the start of the line to the knuckle
};

std::vector<Chain> page_chains(PickerPage page, int view) {
    std::vector<Chain> c;
    switch (page) {
        case PickerPage::Body:
            c.push_back({{"mPelvis", "mTorso", "mChest", "mNeck", "mHead"}});
            for (const char* s : kSides) {
                c.push_back({{sided("mCollar@", s), sided("mShoulder@", s), sided("mElbow@", s), sided("mWrist@", s)}});
                c.push_back({{sided("mHip@", s), sided("mKnee@", s), sided("mAnkle@", s), sided("mFoot@", s),
                              sided("mToe@", s)}});
            }
            break;
        case PickerPage::Hands:
            for (const char* s : kSides)
                for (const char* f : kFingers) {
                    Chain ch{{sided("mWrist@", s)}, true};
                    for (int k = 1; k <= 3; ++k) ch.bones.push_back(std::string("mHand") + f + std::to_string(k) + s);
                    c.push_back(ch);
                }
            break;
        case PickerPage::Face:
            for (const PickerGroup& g : picker_groups())
                if (g.page == PickerPage::Face && g.label != "Mouth")
                    for (const std::string& b : g.bones) c.push_back({{b}});  // dots at the ends, no lines
            break;
        case PickerPage::Extras:
            if (view == 0) {
                for (const char* s : kSides) {
                    c.push_back({{"mWingsRoot", sided("mWing1@", s), sided("mWing2@", s), sided("mWing3@", s),
                                  sided("mWing4@", s)}});
                    c.push_back({{sided("mWing4Fan@", s)}});
                }
            } else if (view == 1) {
                c.push_back({{"mTail1", "mTail2", "mTail3", "mTail4", "mTail5", "mTail6"}});
                c.push_back({{"mGroin"}});
            } else {
                for (const char* s : kSides)
                    c.push_back({{"mHindLimbsRoot", sided("mHindLimb1@", s), sided("mHindLimb2@", s),
                                  sided("mHindLimb3@", s), sided("mHindLimb4@", s)}});
            }
            break;
    }
    return c;
}

// The group of a page view a bone belongs to: the first such group naming it.
int group_of(PickerPage page, int view, const std::string& bone) {
    const auto& gs = picker_groups();
    for (int i = 0; i < int(gs.size()); ++i)
        if (gs[i].page == page && (gs[i].view < 0 || gs[i].view == view) &&
            std::find(gs[i].bones.begin(), gs[i].bones.end(), bone) != gs[i].bones.end())
            return i;
    return -1;
}

// The rest-pose rotation of a node (the hands' and head's frames are measured from it).
Quat rest_rot(const Skeleton& skel, int node) {
    Quat r;
    for (int n = node; n >= 0; n = skel[n].parent) r = skel[n].rest * r;
    return r;
}

// A hand turned fingers up, its back to the viewer, the wrist at the origin (before the view's turn and placing).
Xform hand_frame(const Skeleton& skel, const std::vector<Xform>& g, int side) {
    const int wrist = skel.find(sided("mWrist@", kSides[side]));
    if (wrist < 0) return {};
    // At rest the fingers point out along +Y (left) or -Y (right) with the back of the hand up (+Z).
    const Quat canon = basis_to({0, side ? 1.0 : -1.0, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0});
    const Quat r = canon * rest_rot(skel, wrist) * g[size_t(wrist)].rot.conj();
    return turn_about(r, g[size_t(wrist)].pos);
}

// Its extent across the page (min, max y) and up (max z), from the wrist and the fingertips.
void hand_extent(const Skeleton& skel, const std::vector<Xform>& g, const Shape* shape, int side, const Xform& to,
                 double& y0, double& y1, double& z1) {
    y0 = 1e9, y1 = -1e9, z1 = 0;
    for (const char* f : kFingers)
        for (int k = 1; k <= 3; ++k) {
            const int n = skel.find(std::string("mHand") + f + std::to_string(k) + kSides[side]);
            if (n < 0) continue;
            for (const Vec3& p : {g[size_t(n)].pos, tip_of(skel, g, shape, n)}) {
                const Vec3 q = to.apply(p);
                y0 = std::min(y0, q.y), y1 = std::max(y1, q.y), z1 = std::max(z1, q.z);
            }
        }
    if (y0 > y1) y0 = -0.05, y1 = 0.05, z1 = 0.2;
}

Xform body_frame(const Skeleton& skel, const std::vector<Xform>& g) {
    const int pelvis = skel.find("mPelvis");
    if (pelvis < 0) return {};
    const Vec3 fwd = g[size_t(pelvis)].rot.rotate({1, 0, 0});
    const double yaw = std::hypot(fwd.x, fwd.y) > 1e-6 ? std::atan2(fwd.y, fwd.x) : 0.0;
    const Vec3 at = g[size_t(pelvis)].pos;
    return turn_about(rot_z(-yaw), {at.x, at.y, 0});
}

}  // namespace

std::vector<PickerPart> picker_parts(const Skeleton& skel, const std::vector<Xform>& globals,
                                     const std::vector<Xform>& chart, const Shape* shape, PickerPage page, int view) {
    std::vector<PickerPart> parts;
    if (page == PickerPage::Hands) {
        const Quat turn = view == 1 ? rot_z(kPi) : Quat{};  // the palm: the hand turned round, fingers still up
        constexpr double kGap = 0.035;                       // between the two hands' nearest fingers
        for (int side = 0; side < 2; ++side) {
            // Placed from the chart pose, so the hands keep their places whatever the fingers do.
            const Xform chart_to = Xform{turn, {}} * hand_frame(skel, chart, side);
            double y0, y1, z1;
            hand_extent(skel, chart, shape, side, chart_to, y0, y1, z1);
            const double shift = side == 0 ? -kGap / 2 - y1 : kGap / 2 - y0;  // right hand left of centre
            PickerPart p;
            p.to_page = Xform{{}, {0, shift, 0}} * Xform{turn, {}} * hand_frame(skel, globals, side);
            // A render keeps the hand's own skin: what lies around its bones, not a thigh it rests on. The box cuts
            // the forearm off below the wrist.
            p.clip = true;
            const double reach = z1 * 1.3, width = (y1 - y0) * 0.5 + 0.08;
            const double mid = shift + (y0 + y1) / 2;
            p.lo = {-reach, mid - width, -0.025};
            p.hi = {reach, mid + width, reach};
            const char* s = kSides[side];
            auto at = [&](int n) { return p.to_page.apply(globals[size_t(n)].pos); };
            const int wrist = skel.find(sided("mWrist@", s));
            for (const char* f : kFingers) {
                std::vector<int> chain = {wrist};
                for (int k = 1; k <= 3; ++k) chain.push_back(skel.find(std::string("mHand") + f + std::to_string(k) + s));
                if (std::find(chain.begin(), chain.end(), -1) != chain.end()) continue;
                const bool thumb = std::string(f) == "Thumb";
                for (size_t k = 0; k + 1 < chain.size(); ++k)
                    p.keep.push_back({at(chain[k]), at(chain[k + 1]), k == 0 ? (thumb ? 0.026 : 0.032) : 0.017});
                p.keep.push_back({at(chain.back()), p.to_page.apply(tip_of(skel, globals, shape, chain.back())), 0.016});
            }
            parts.push_back(p);
        }
        return parts;
    }
    PickerPart p;
    if (page == PickerPage::Face) {
        const int head = skel.find("mHead");
        if (head >= 0) {
            const Quat r = rest_rot(skel, head) * globals[size_t(head)].rot.conj();
            p.to_page = turn_about(r, globals[size_t(head)].pos);
            // Keep the head and a little neck: down to just below the chin in the chart pose.
            const Xform chart_to = turn_about(rest_rot(skel, head) * chart[size_t(head)].rot.conj(), chart[size_t(head)].pos);
            double chin = -0.08;
            if (int c = skel.find("mFaceChin"); c >= 0) chin = chart_to.apply(tip_of(skel, chart, shape, c)).z;
            p.clip = true;
            p.lo = {-0.3, -0.115, chin - 0.025};
            p.hi = {0.3, 0.115, 0.4};
        }
    } else {
        p.to_page = Xform{view_turn(page, view), {}} * body_frame(skel, globals);
    }
    parts.push_back(p);
    return parts;
}

bool picker_keeps(const PickerPart& p, const Vec3& c) {
    if (!p.clip) return true;
    if (c.x < p.lo.x || c.y < p.lo.y || c.z < p.lo.z || c.x > p.hi.x || c.y > p.hi.y || c.z > p.hi.z) return false;
    if (p.keep.empty()) return true;
    for (const PickerPart::Capsule& k : p.keep) {
        const Vec3 d = k.b - k.a;
        const double l2 = d.dot(d);
        const double t = l2 > 0 ? std::clamp((c - k.a).dot(d) / l2, 0.0, 1.0) : 0.0;
        if ((c - (k.a + d * t)).length() <= k.r) return true;
    }
    return false;
}

int picker_part_of(const Skeleton& skel, PickerPage page, int node) {
    if (page != PickerPage::Hands || node < 0 || node >= skel.size()) return 0;
    const std::string& n = skel[node].name;
    return n.size() >= 4 && n.compare(n.size() - 4, 4, "Left") == 0 ? 1 : 0;
}

PickerLayout picker_layout(const Skeleton& skel, const std::vector<Xform>& globals, const std::vector<PickerPart>& parts,
                           const Shape* shape, PickerPage page, int view, bool points, bool volumes) {
    PickerLayout l;
    if (parts.empty() || globals.size() < size_t(skel.size())) return l;
    auto at = [&](int node, const Vec3& world) {
        const Vec3 q = parts[size_t(std::min<int>(picker_part_of(skel, page, node), int(parts.size()) - 1))].to_page.apply(world);
        return V2{q.y, q.z};
    };
    auto joint = [&](int n) { return at(n, globals[size_t(n)].pos); };
    auto end = [&](int n) { return at(n, tip_of(skel, globals, shape, n)); };
    std::set<int> seen;
    auto dot = [&](int n, V2 p) {
        if (seen.insert(n).second) l.dots.push_back({n, p, group_of(page, view, skel[n].name)});
    };
    for (const Chain& ch : page_chains(page, view)) {
        std::vector<int> nodes;
        for (const std::string& b : ch.bones)
            if (int n = skel.find(b); n >= 0) nodes.push_back(n);
        if (nodes.empty()) continue;
        for (size_t k = 0; k < nodes.size(); ++k) {
            const int n = nodes[k];
            // A face bone shows where it meets the face. The jaw's end is on the lower lip, so it shows further on,
            // between the lip and the chin; the lids end a few millimetres off the eye, so they show further off it.
            V2 a = page == PickerPage::Face ? end(n) : joint(n);
            if (page == PickerPage::Face && skel[n].name == "mFaceJaw") a = joint(n) + (end(n) - joint(n)) * 1.5;
            if (page == PickerPage::Face && skel[n].name.rfind("mFaceEyeLid", 0) == 0) {
                const std::string& nm = skel[n].name;
                const int eye = skel.find(nm.substr(nm.size() - 4) == "Left" ? "mEyeLeft" : "mEyeRight");
                if (eye >= 0) a = end(eye) + (end(n) - end(eye)) * 2.4;
            }
            dot(n, a);
            if (page == PickerPage::Face) continue;
            V2 b;
            if (k + 1 < nodes.size()) {
                b = joint(nodes[k + 1]);
            } else if (skel[n].name.rfind("mWrist", 0) == 0) {  // the hand: to the middle knuckle
                const int m = skel.find(sided("mHandMiddle1@", picker_part_of(skel, PickerPage::Hands, n) ? "Left" : "Right"));
                b = m >= 0 ? joint(m) : end(n);
            } else {
                b = end(n);
            }
            l.lines.push_back({n, a, b, group_of(page, view, skel[n].name)});
        }
        if (page == PickerPage::Hands && nodes.size() >= 2) {
            const int last = nodes.back();
            const V2 tip = end(last), from = joint(last);
            const V2 d = tip - from;
            const double len = d.length();
            const int g = group_of(page, view, skel[last].name);
            l.caps.push_back({g, tip, len > 1e-9 ? d * (1 / len) : V2{0, 1}});
        }
    }
    if (points || volumes) {
        // The Points menu: those hung off the page's bones.
        std::set<int> page_nodes;
        for (const PickerDot& d : l.dots) page_nodes.insert(d.node);
        for (int n = skel.joint_count(); n < skel.size(); ++n) {
            const Node& nd = skel[n];
            if (!(nd.volume ? volumes : points) || nd.parent < 0) continue;
            int anchor = nd.parent;
            if (page == PickerPage::Body && skel[anchor].category == Category::Body) {
                while (anchor >= 0 && !page_nodes.count(anchor)) anchor = skel[anchor].parent;
            }
            if (anchor < 0 || !page_nodes.count(anchor)) continue;
            l.dots.push_back({n, at(anchor, globals[size_t(n)].pos), -1, true});
        }
    }
    return l;
}

std::vector<PickerLine> picker_anchors(const Skeleton& skel, const std::vector<Xform>& globals,
                                       const std::vector<PickerPart>& parts, const Shape* shape, PickerPage page, int view) {
    if (page == PickerPage::Hands || page == PickerPage::Face) {
        // The page's own bones: a hand's lines, the face's dots.
        const PickerLayout l = picker_layout(skel, globals, parts, shape, page, view);
        std::vector<PickerLine> out = l.lines;
        if (page == PickerPage::Face) {
            for (const PickerDot& d : l.dots) out.push_back({d.node, d.p, d.p, d.group});
            if (int head = skel.find("mHead"); head >= 0 && !parts.empty()) {  // the crown, over the forehead
                const Vec3 a = parts[0].to_page.apply(globals[size_t(head)].pos), b = parts[0].to_page.apply(tip_of(skel, globals, shape, head));
                out.push_back({head, {a.y, a.z}, {b.y, b.z}, -1});
            }
        }
        return out;
    }
    // Body and Extras: the body's lines, as the Body page draws them, seen from this view.
    std::vector<PickerPart> p = parts;
    return picker_layout(skel, globals, p, shape, PickerPage::Body, 0).lines;
}

// --- canvas -------------------------------------------------------------------------------------------------------

PickerRect picker_label_area(PickerPage page, const PickerRect& canvas, double line_h, int chip_rows) {
    const double pad = kPickerPad * line_h;
    const double bottom = page == PickerPage::Face ? chip_rows * kPickerChipRow * line_h : kPickerToolRow * line_h;
    return {canvas.x + pad, canvas.y + pad, std::max(0.0, canvas.w - 2 * pad), std::max(0.0, canvas.h - 2 * pad - bottom)};
}

PickerRect picker_fit_area(PickerPage page, const PickerRect& canvas, double line_h, int chip_rows) {
    PickerRect r = picker_label_area(page, canvas, line_h, chip_rows);
    const double top = kPickerLabelBand * line_h;
    const double bottom = page == PickerPage::Hands ? kPickerLabelBand * line_h : 0;
    const double side = line_h * 0.3;  // room for a dot's ring at the edge
    r.x += side, r.w = std::max(1.0, r.w - 2 * side);
    r.y += top + side, r.h = std::max(1.0, r.h - top - bottom - 2 * side);
    return r;
}

PickerFit picker_fit(const std::vector<V2>& pts, const PickerRect& r) {
    PickerFit f;
    if (pts.empty()) return f;
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (V2 p : pts) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    const double w = std::max(x1 - x0, 1e-6), h = std::max(y1 - y0, 1e-6);
    f.scale = std::min(r.w / w, r.h / h);
    f.ox = r.x + r.w / 2 - (x0 + x1) / 2 * f.scale;
    f.oy = r.y + r.h / 2 + (y0 + y1) / 2 * f.scale;
    return f;
}

std::vector<V2> picker_extent(const PickerLayout& l) {
    std::vector<V2> out;
    for (const PickerDot& d : l.dots) out.push_back(d.p);
    for (const PickerLine& s : l.lines) out.push_back(s.a), out.push_back(s.b);
    for (const PickerCap& c : l.caps) out.push_back(c.tip);
    return out;
}

PickerScreen picker_screen(const PickerLayout& l, const PickerFit& fit, double cap_offset) {
    PickerScreen s;
    for (PickerDot d : l.dots) d.p = fit.px(d.p), s.dots.push_back(d);
    for (PickerLine x : l.lines) x.a = fit.px(x.a), x.b = fit.px(x.b), s.lines.push_back(x);
    for (PickerCap c : l.caps) {
        const V2 dir{c.dir.x, -c.dir.y};  // y down on the canvas
        c.tip = fit.px(c.tip) + dir * cap_offset;
        c.dir = dir;
        s.caps.push_back(c);
    }
    return s;
}

namespace {

bool segment_hits_rect(V2 a, V2 b, const PickerRect& r) {
    if (r.contains(a) || r.contains(b)) return true;
    const V2 c[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
    auto cross = [](V2 o, V2 p, V2 q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    for (int i = 0; i < 4; ++i) {
        const V2 p = c[i], q = c[(i + 1) % 4];
        if (cross(a, b, p) * cross(a, b, q) < 0 && cross(p, q, a) * cross(p, q, b) < 0) return true;
    }
    return false;
}

}  // namespace

std::vector<PickerLabel> picker_labels(const Skeleton& skel, PickerPage page, int view, const PickerScreen& s,
                                       const PickerRect& area, const std::vector<PickerRect>& blocked,
                                       const std::function<double(const std::string&)>& text_w, double h) {
    std::vector<PickerLabel> out;
    const auto& gs = picker_groups();
    const double pad = h * 0.3, dot_r = h * 0.45;
    auto free = [&](const PickerRect& r) {
        if (!r.inside(area)) return false;
        const PickerRect e{r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad};
        for (const PickerLabel& o : out)
            if (o.r.overlaps(e)) return false;
        for (const PickerRect& b : blocked)
            if (b.overlaps(e)) return false;
        for (const PickerDot& d : s.dots)
            if (PickerRect{e.x - dot_r, e.y - dot_r, e.w + 2 * dot_r, e.h + 2 * dot_r}.contains(d.p)) return false;
        for (const PickerLine& l : s.lines)
            if (segment_hits_rect(l.a, l.b, e)) return false;
        for (const PickerCap& c : s.caps)
            if (PickerRect{e.x - dot_r, e.y - dot_r, e.w + 2 * dot_r, e.h + 2 * dot_r}.contains(c.tip)) return false;
        return true;
    };
    const double mid = area.x + area.w / 2;
    for (int gi = 0; gi < int(gs.size()); ++gi) {
        const PickerGroup& g = gs[gi];
        if (g.page != page || g.kind != PickerGroupKind::Label || (g.view >= 0 && g.view != view)) continue;
        std::vector<V2> pts;
        for (const PickerDot& d : s.dots)
            if (!d.point && std::find(g.bones.begin(), g.bones.end(), skel[d.node].name) != g.bones.end())
                pts.push_back(d.p);
        if (pts.empty()) continue;
        double cx = 0, cy = 0, x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (V2 p : pts) {
            cx += p.x / double(pts.size()), cy += p.y / double(pts.size());
            x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
        }
        const double w = text_w(g.label) + 2 * pad;
        // Where it would like to be; the search below moves it off anything it covers.
        PickerRect pref{cx - w / 2, cy - h / 2, w, h};
        bool slide_x = true;
        if (page == PickerPage::Body) {
            const bool top = g.label == "HEAD" || g.label == "SPINE";  // the top band's left, side by side
            pref.x = top || cx < mid ? area.x : area.x + area.w - w;
            if (g.label == "SPINE" && !out.empty()) pref.x = out.back().r.x + out.back().r.w + h * 0.3;
            if (top) pref.y = area.y;
            else pref.y = (pts.size() >= 2 ? (pts[0].y + pts[1].y) / 2 : cy) - h;  // beside the upper limb
            slide_x = false;
        } else if (page == PickerPage::Hands) {
            pref.y = area.y + area.h - h;  // in the band under the hands
        } else if (g.label == "WINGS") {
            pref.y = y0 - h * 1.6;
        } else if (g.label == "TAIL") {
            pref.x = x0 - w * 0.2, pref.y = y0 - h * 1.8;
        } else if (g.label == "GROIN") {
            pref.x = x1 + h * 0.8, pref.y = cy - h / 2;
        } else {  // HIND LIMBS: beside the limbs, towards the free side
            pref.x = x0 - w - h * 0.8, pref.y = cy - h / 2;
        }
        pref.x = std::clamp(pref.x, area.x, area.x + area.w - w);
        pref.y = std::clamp(pref.y, area.y, area.y + area.h - h);
        PickerRect best = pref;
        bool found = free(pref);
        const double step = h * 0.5;
        if (page == PickerPage::Body && (g.label == "HEAD" || g.label == "SPINE"))  // along the top band first
            for (int k = 1; k < 60 && !found; ++k)
                if (const PickerRect r{pref.x + k * step, pref.y, w, h}; free(r)) best = r, found = true;
        for (int k = 1; k < 60 && !found; ++k)
            for (int sy : {1, -1}) {
                for (int sx = 0; sx <= (slide_x ? 2 : 0) && !found; ++sx) {
                    const double dx = sx == 0 ? 0 : (sx == 1 ? 1 : -1) * k * step, dy = sy * k * step;
                    for (const PickerRect r : {PickerRect{pref.x, pref.y + dy, w, h}, PickerRect{pref.x + dx, pref.y, w, h},
                                               PickerRect{pref.x + dx, pref.y + dy, w, h}})
                        if (!found && free(r)) best = r, found = true;
                }
                if (found) break;
            }
        best.x = std::clamp(best.x, area.x, std::max(area.x, area.x + area.w - w));
        best.y = std::clamp(best.y, area.y, std::max(area.y, area.y + area.h - h));
        out.push_back({gi, best});
    }
    return out;
}

// --- clicks -------------------------------------------------------------------------------------------------------

std::vector<int> picker_hits(const PickerScreen& s, V2 m, double dot_r, double line_r) {
    std::vector<std::pair<double, int>> dots, lines;
    for (const PickerDot& d : s.dots)
        if (double r = (d.p - m).length(); r <= dot_r) dots.push_back({r, d.node});
    for (const PickerLine& l : s.lines)
        if (double r = segment_distance(m, l.a, l.b); r <= line_r) lines.push_back({r, l.node});
    std::stable_sort(dots.begin(), dots.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::stable_sort(lines.begin(), lines.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<int> out;
    for (auto* v : {&dots, &lines})
        for (auto& [r, n] : *v)
            if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    return out;
}

int picker_cap_hit(const PickerScreen& s, V2 m, double r) {
    int best = -1;
    double bd = r;
    for (const PickerCap& c : s.caps)
        if (double d = (c.tip - m).length(); d <= bd) bd = d, best = c.group;
    return best;
}

int PickerCycle::click(V2 m, const std::vector<int>& now_ranked, double slop) {
    auto sorted = [](std::vector<int> v) { return std::sort(v.begin(), v.end()), v; };
    if (!now_ranked.empty() && !ranked.empty() && (m - at).length() <= slop && sorted(now_ranked) == sorted(ranked)) {
        index = (index + 1) % int(ranked.size());
    } else {
        ranked = now_ranked;
        index = 0;
    }
    at = m;
    return ranked.empty() ? -1 : ranked[size_t(index)];
}

std::string picker_bone_label(const std::string& bone) {
    static const std::map<std::string, std::string> kBody = {
        {"mHead", "Head"},         {"mNeck", "Neck"},     {"mChest", "Chest"},     {"mTorso", "Torso"},
        {"mPelvis", "Pelvis"},     {"mCollar", "Collar"}, {"mShoulder", "Upper Arm"}, {"mElbow", "Forearm"},
        {"mWrist", "Hand"},        {"mHip", "Thigh"},     {"mKnee", "Shin"},        {"mAnkle", "Ankle"},
        {"mFoot", "Foot"},         {"mToe", "Toes"},      {"mEye", "Eye"},          {"mGroin", "Groin"}};
    if (bone.size() < 2 || bone[0] != 'm' || !std::isupper(static_cast<unsigned char>(bone[1]))) return bone;
    std::string side, core = bone;
    for (const char* s : kSides)
        if (core.size() > std::strlen(s) && core.compare(core.size() - std::strlen(s), std::strlen(s), s) == 0) {
            side = std::string(s) + " ";
            core.resize(core.size() - std::strlen(s));
        }
    if (auto it = kBody.find(core); it != kBody.end()) return side + it->second;
    std::string s = core.substr(1);
    for (const char* prefix : {"Hand", "Face"})
        if (s.rfind(prefix, 0) == 0 && s.size() > std::strlen(prefix)) s = s.substr(std::strlen(prefix));
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool upper = std::isupper(static_cast<unsigned char>(c)), digit = std::isdigit(static_cast<unsigned char>(c));
        if (i && (upper || (digit && !std::isdigit(static_cast<unsigned char>(s[i - 1]))))) out += ' ';
        out += c;
    }
    return side + out;
}

}  // namespace vats
