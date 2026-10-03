// Viewport Avatar Toolset - the Second Life Bento skeleton.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "vats/xml.h"

namespace vats {
namespace {

Vec3 parse_vec(const std::string& s) {
    // strtod rather than sscanf: MSVC's /WX build (the viewer's) rejects sscanf as deprecated.
    const char* p = s.c_str();
    double v[3];
    for (double& x : v) {
        char* end = nullptr;
        x = std::strtod(p, &end);
        if (end == p) return {};
        p = end;
    }
    return {v[0], v[1], v[2]};
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

Category category_of(const std::string& g) {
    static const std::pair<const char*, Category> table[] = {
        {"Hand", Category::Hands},    {"Face", Category::Face},        {"Eyes", Category::Face},
        {"Ears", Category::Face},     {"Lips", Category::Face},        {"Mouth", Category::Face},
        {"Nose", Category::Face},     {"Wing", Category::Wings},       {"Tail", Category::Tail},
        {"Limb", Category::HindLimbs}, {"Groin", Category::Groin}};
    for (auto& [name, cat] : table)
        if (g == name) return cat;
    return Category::Body;  // Torso, Spine, Arms, Legs, Extra and anything unknown
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

const XmlNode* child(const XmlNode& n, std::string_view name) {
    for (auto& c : n.children)
        if (c.name == name) return &c;
    return nullptr;
}

}  // namespace

bool Skeleton::load_dir(const std::string& dir, std::string& err) {
    std::string skel, lad;
    if (!read_file(dir + "/avatar_skeleton.xml", skel) || !read_file(dir + "/avatar_lad.xml", lad)) {
        err = "cannot read avatar_skeleton.xml / avatar_lad.xml in " + dir;
        return false;
    }
    return load(skel, lad, err);
}

bool Skeleton::load(std::string_view skeleton_xml, std::string_view lad_xml, std::string& err) {
    *this = Skeleton();
    XmlNode skel;
    if (!parse_xml(skeleton_xml, skel, err)) {
        err = "avatar_skeleton.xml: " + err;
        return false;
    }
    if (skel.name != "linden_skeleton") {
        err = "avatar_skeleton.xml: root is not linden_skeleton";
        return false;
    }

    // Joints in document (pre-order) order, so parents always precede children.
    auto add_bone = [&](auto& self, const XmlNode& x, int parent) -> void {
        Node n;
        n.name = x.attr_or("name");
        n.parent = parent;
        n.pos = parse_vec(x.attr_or("pos"));
        n.end = parse_vec(x.attr_or("end"));
        n.rest = euler_to_quat(parse_vec(x.attr_or("rot")));  // viewer mayaQ XYZ
        n.group = x.attr_or("group");
        n.category = category_of(n.group);
        n.base = x.attr_or("support") == "base";
        if (x.attr("scale")) n.scale = parse_vec(x.attr_or("scale"));
        n.pivot = parse_vec(x.attr_or("pivot"));
        n.connected = x.attr_or("connected") == "true";
        std::istringstream aliases(x.attr_or("aliases"));
        for (std::string a; aliases >> a;) n.aliases.push_back(a);
        int idx = static_cast<int>(nodes_.size());
        nodes_.push_back(std::move(n));
        if (parent >= 0) nodes_[parent].children.push_back(idx);
        for (auto& c : x.children) {
            if (c.name == "bone") {
                self(self, c, idx);
            } else if (c.name == "collision_volume") {
                CollisionVolume v;
                v.name = c.attr_or("name");
                v.joint = idx;
                v.pos = parse_vec(c.attr_or("pos"));
                v.rot = euler_to_quat(parse_vec(c.attr_or("rot")));
                v.scale = parse_vec(c.attr_or("scale"));
                v.end = parse_vec(c.attr_or("end"));
                v.group = c.attr_or("group");
                volumes_.push_back(std::move(v));
            }
        }
    };
    for (auto& c : skel.children)
        if (c.name == "bone") add_bone(add_bone, c, -1);

    auto expect = [&](const char* attr, size_t got) {
        auto* a = skel.attr(attr);
        return !a || std::to_string(got) == *a;
    };
    if (!expect("num_bones", nodes_.size()) || !expect("num_collision_volumes", volumes_.size())) {
        err = "avatar_skeleton.xml: bone or collision volume count does not match the header";
        return false;
    }
    joint_count_ = static_cast<int>(nodes_.size());
    for (int i = 0; i < joint_count_; ++i) exact_.emplace(nodes_[i].name, i);

    XmlNode lad;
    if (!parse_xml(lad_xml, lad, err)) {
        err = "avatar_lad.xml: " + err;
        return false;
    }
    const XmlNode* lad_skel = child(lad, "skeleton");
    if (!lad_skel) {
        err = "avatar_lad.xml: no <skeleton> element";
        return false;
    }

    // Attachment points (non-HUD), ascending id. Rotation uses setQuat(roll, pitch, yaw),
    // i.e. Hamilton qx*qy*qz - the opposite order from joints.
    std::vector<const XmlNode*> points;
    for (auto& c : lad_skel->children)
        if (c.name == "attachment_point" && c.attr_or("hud") != "true") points.push_back(&c);
    std::stable_sort(points.begin(), points.end(), [](const XmlNode* a, const XmlNode* b) {
        return std::atoi(a->attr_or("id").c_str()) < std::atoi(b->attr_or("id").c_str());
    });
    for (auto* p : points) {
        Node n;
        n.name = p->attr_or("name");
        n.attach_id = std::atoi(p->attr_or("id").c_str());
        n.attachment = true;
        n.group = "Attachment";
        n.category = Category::AttachmentPoints;
        n.pos = parse_vec(p->attr_or("position"));
        Vec3 r = parse_vec(p->attr_or("rotation")) * kDegToRad;
        n.rest = (Quat::axis_angle({1, 0, 0}, r.x) * Quat::axis_angle({0, 1, 0}, r.y) *
                  Quat::axis_angle({0, 0, 1}, r.z))
                     .normalized();
        n.end = {0, 0, 0.04};  // display tail: 4 cm along its own +Z
        auto it = exact_.find(p->attr_or("joint"));
        if (it != exact_.end()) {
            n.parent = it->second;
        } else {
            // mRoot (Avatar Center) and any other missing parent: root-level at the pelvis rest.
            n.parent = -1;
            n.pos += nodes_[0].pos;
        }
        int idx = static_cast<int>(nodes_.size());
        if (n.parent >= 0) nodes_[n.parent].children.push_back(idx);
        nodes_.push_back(std::move(n));
    }

    // Collision volumes as animatable pseudo-joints (README decision 12), after the attachment points.
    volume_start_ = size();
    for (auto& v : volumes_) {
        Node n;
        n.name = v.name;
        n.parent = v.joint;
        n.pos = v.pos;
        n.end = v.end;
        n.rest = v.rot;
        n.group = "Collision";
        n.category = Category::CollisionVolumes;
        n.base = true;
        n.attachment = n.volume = true;
        v.node = size();
        nodes_[v.joint].children.push_back(v.node);
        nodes_.push_back(std::move(n));
    }

    // Name indices (spec SK-7) and the viewer's case-sensitive alias map. Volumes match exactly (as in the
    // viewer) and case-insensitively only where nothing else has the name.
    for (int i = joint_count_; i < size(); ++i) exact_.emplace(nodes_[i].name, i);
    for (int i = 0; i < joint_count_; ++i) lower_.emplace(lower(nodes_[i].name), i);
    for (int i = 0; i < joint_count_; ++i)
        for (auto& a : nodes_[i].aliases) lower_.emplace(lower(a), i);
    for (int i = joint_count_; i < size(); ++i) lower_.emplace(lower(nodes_[i].name), i);
    for (int i = 0; i < joint_count_; ++i)
        for (auto& a : nodes_[i].aliases) viewer_alias_[a] = i;
    for (int i = joint_count_; i < volume_start_; ++i) {
        std::string u = nodes_[i].name;
        std::replace(u.begin(), u.end(), ' ', '_');
        viewer_alias_.emplace(u, i);
    }
    for (int i = 0; i < static_cast<int>(volumes_.size()); ++i) volume_index_.emplace(volumes_[i].name, i);

    mirror_.resize(nodes_.size());
    set_reused({});  // pairs every bone with its other side's

    // Male shape: <param id="32"> param_skeleton at weight 1.
    male_.scale.assign(nodes_.size(), {1, 1, 1});
    male_.offset.assign(nodes_.size(), {});
    auto find_param = [&](auto& self, const XmlNode& n) -> const XmlNode* {
        if (n.name == "param" && n.attr_or("id") == "32" && child(n, "param_skeleton")) return &n;
        for (auto& c : n.children)
            if (auto* r = self(self, c)) return r;
        return nullptr;
    };
    if (const XmlNode* male = find_param(find_param, lad)) {
        for (auto& b : child(*male, "param_skeleton")->children) {
            if (b.name != "bone") continue;
            int i = find(b.attr_or("name"));
            if (i < 0) continue;
            male_.scale[i] = Vec3{1, 1, 1} + parse_vec(b.attr_or("scale"));
            if (b.attr("offset")) male_.offset[i] = parse_vec(b.attr_or("offset"));
        }
        // Keep the feet on the ground.
        int foot = find("mFootLeft");
        if (foot >= 0) {
            Pose zero(nodes_.size());
            double dz = global_pose(zero)[foot].pos.z - global_pose(zero, &male_)[foot].pos.z;
            male_.offset[0].z += dz;
        }
    }
    return true;
}

int Skeleton::find(std::string_view name) const {
    auto it = exact_.find(std::string(name));
    if (it != exact_.end()) return it->second;
    it = lower_.find(lower(name));
    return it != lower_.end() ? it->second : -1;
}

int Skeleton::find_viewer(std::string_view name) const {
    std::string s(name);
    auto it = exact_.find(s);
    if (it != exact_.end()) return it->second;
    it = viewer_alias_.find(s);
    return it != viewer_alias_.end() ? it->second : -1;
}

int Skeleton::find_volume(std::string_view name) const {
    auto it = volume_index_.find(std::string(name));
    return it != volume_index_.end() ? it->second : -1;
}

void Skeleton::set_reused(const std::vector<int>& nodes) {
    reused_.assign(nodes_.size(), 0);
    for (int i = 0; i < size(); ++i) {
        const int m = find(mirror_name(nodes_[i].name));
        mirror_[i] = m >= 0 ? m : i;
    }
    for (int n : nodes) {
        if (n < 0 || n >= size()) continue;
        reused_[n] = 1;
        mirror_[mirror_[n]] = mirror_[n];  // the partner keeps to itself too: nothing mirrors onto the reused bone
        mirror_[n] = n;
    }
}

std::string Skeleton::mirror_of(std::string_view name) const {
    const bool pin = name.substr(0, 4) == "pin:";
    const int n = find(pin ? name.substr(4) : name);
    if (n < 0) return mirror_name(name);
    return (pin ? "pin:" : "") + nodes_[mirror_[n]].name;
}

std::string Skeleton::mirror_name(std::string_view name) {
    std::string prefix;
    if (name.substr(0, 4) == "pin:") {
        prefix = "pin:";
        name.remove_prefix(4);
    }
    if (name.substr(0, 2) == "L ") return prefix + "R " + std::string(name.substr(2));
    if (name.substr(0, 2) == "R ") return prefix + "L " + std::string(name.substr(2));
    // Collision volumes: L_/R_ and LEFT_/RIGHT_ prefixes.
    for (auto [a, b] : {std::pair{"L_", "R_"}, {"R_", "L_"}, {"LEFT_", "RIGHT_"}, {"RIGHT_", "LEFT_"}})
        if (name.substr(0, std::strlen(a)) == a) return prefix + b + std::string(name.substr(std::strlen(a)));
    std::string out;
    for (size_t i = 0; i < name.size();) {
        if (name.substr(i, 4) == "Left") {
            out += "Right";
            i += 4;
        } else if (name.substr(i, 5) == "Right") {
            out += "Left";
            i += 5;
        } else {
            out += name[i++];
        }
    }
    return prefix + out;
}

Quat Skeleton::bone_frame(const Vec3& end) {
    double len = end.length();
    if (len < 1e-5) return {};
    Vec3 d = end * (1.0 / len);
    int a = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(d[i]) > std::fabs(d[a])) a = i;
    Vec3 axis;
    axis[a] = d[a] > 0 ? 1 : -1;
    Vec3 c = axis.cross(d);  // |dot| >= 1/sqrt(3), so the half-way quaternion is never degenerate
    return Quat{1 + axis.dot(d), c.x, c.y, c.z}.normalized();
}

Xform Skeleton::local_xform(int i, const Pose& pose, const Shape* shape) const {
    const Node& n = nodes_[i];
    Vec3 t = n.pos + pose.offset[i];
    if (shape) {
        t += shape->offset[i];
        if (n.parent >= 0) t = t.mul(shape->scale[n.parent]);  // parent's scale only, never cumulative
    }
    return {n.rest * pose.rot[i], t};
}

Pose Skeleton::pose_from_live(const std::vector<Quat>& local, const Vec3& pelvis, const Vec3& rest) const {
    Pose p(nodes_.size());
    for (size_t i = 0; i < nodes_.size() && i < local.size(); ++i) p.rot[i] = (nodes_[i].rest.conj() * local[i]).normalized();
    if (!nodes_.empty()) p.offset[0] = pelvis - rest;
    return p;
}

Vec3 Skeleton::worn_pelvis_rest(double worn_pelvis_to_foot) const {
    if (nodes_.empty()) return {};
    return nodes_[0].pos + Vec3{0, 0, worn_pelvis_to_foot - sl_body_size(*this).pelvis_to_foot};
}

SlBodySize sl_body_size(const Skeleton& skel, const Pose* pose, const Shape* shape) {
    // The joint's position in its parent's frame (LLJoint::getPosition) and its scale (getScale), by name.
    auto at = [&](const char* name, Vec3* scale) {
        const int i = skel.find(name);
        if (i < 0 || skel[i].name != name) {
            if (scale) *scale = {1, 1, 1};
            return Vec3{};
        }
        Vec3 p = skel[i].pos;
        if (pose && size_t(i) < pose->offset.size()) p += pose->offset[size_t(i)];
        if (shape && size_t(i) < shape->offset.size()) p += shape->offset[size_t(i)];
        if (scale) *scale = shape && size_t(i) < shape->scale.size() ? shape->scale[size_t(i)] : Vec3{1, 1, 1};
        return p;
    };
    Vec3 pelvis_s, neck_s, chest_s, head_s, torso_s, hip_s, knee_s, ankle_s;
    at("mPelvis", &pelvis_s);
    const Vec3 skull = at("mSkull", nullptr), neck = at("mNeck", &neck_s), chest = at("mChest", &chest_s);
    const Vec3 head = at("mHead", &head_s), torso = at("mTorso", &torso_s), hip = at("mHipLeft", &hip_s);
    const Vec3 knee = at("mKneeLeft", &knee_s), ankle = at("mAnkleLeft", &ankle_s), foot = at("mFootLeft", nullptr);
    SlBodySize b;
    b.pelvis_to_foot = hip.z * pelvis_s.z - knee.z * hip_s.z - ankle.z * knee_s.z - foot.z * ankle_s.z;
    b.height = b.pelvis_to_foot + std::sqrt(2.0) * (skull.z * head_s.z) + head.z * neck_s.z + neck.z * chest_s.z +
               chest.z * torso_s.z + torso.z * pelvis_s.z;
    return b;
}

std::vector<Xform> Skeleton::global_pose(const Pose& pose, const Shape* shape) const {
    std::vector<Xform> g(nodes_.size());
    for (int i = 0; i < size(); ++i) {
        Xform l = local_xform(i, pose, shape);
        g[i] = nodes_[i].parent >= 0 ? g[nodes_[i].parent] * l : l;
    }
    return g;
}

Vec3 bone_tail(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape, int i) {
    if (shape && i < static_cast<int>(shape->tails.size()) && shape->tails[i].length() > 0)
        return globals[i].apply(shape->tails[i]);
    const Vec3 end = shape ? skel[i].end.mul(shape->scale[i]) : skel[i].end;
    return globals[i].apply(end);
}

VolumeShell volume_shell(const std::vector<Xform>& globals, const Shape* shape, const CollisionVolume& v) {
    const Vec3 k = shape && v.node < static_cast<int>(shape->scale.size()) ? shape->scale[v.node] : Vec3{1, 1, 1};
    return {globals[v.node], v.scale.mul(k)};
}

double ray_shell(const Vec3& o, const Vec3& d, const VolumeShell& shell) {
    // Into the unit sphere's space: the ray stays a line and t keeps its meaning.
    const Xform inv = shell.frame.inverse();
    const Vec3 a = shell.axes, p = inv.apply(o), r = inv.rot.rotate(d);
    const Vec3 q{p.x / a.x, p.y / a.y, p.z / a.z}, u{r.x / a.x, r.y / a.y, r.z / a.z};
    const double A = u.dot(u), B = q.dot(u), C = q.dot(q) - 1;
    if (C <= 0) return 0;
    const double disc = B * B - A * C;
    if (A < 1e-30 || disc < 0) return 1e30;
    const double t = (-B - std::sqrt(disc)) / A;
    return t >= 0 ? t : 1e30;
}

std::vector<StickSegment> stick_segments(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
                                         const std::vector<bool>& shown) {
    std::vector<StickSegment> out;
    auto on = [&](int i) { return shown.empty() || (i < static_cast<int>(shown.size()) && shown[i]); };
    for (int i = 0; i < skel.joint_count(); ++i) {
        if (!on(i)) continue;
        bool child = false;
        for (int c : skel[i].children)
            if (c < skel.joint_count() && on(c)) out.push_back({i, globals[i].pos, globals[c].pos, false}), child = true;
        if (child) continue;
        const bool fitted = shape && i < static_cast<int>(shape->tails.size()) && shape->tails[i].length() > 0;
        const Vec3 d = bone_tail(skel, globals, shape, i) - globals[i].pos;
        const double len = d.length();
        if (len < 1e-5) continue;
        out.push_back({i, globals[i].pos, globals[i].pos + d * (fitted ? 1.0 : std::min(1.0, 0.05 / len)), true});
    }
    return out;
}

std::vector<int> pick_sticks(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
                             const std::vector<bool>& shown, bool sticks,
                             const std::function<bool(const Vec3&, double&, double&)>& to_screen, double mx, double my) {
    // The drawn sizes (ui draw_stick_bones and the attachment point dots), in pixels.
    constexpr double kDot = 3.2 + 1.2, kRing = 3.2, kRingStep = 3.5, kRingHalf = 1.8, kPoint = 4.5;
    constexpr double kOnTop = -100;  // inside a drawn dot or ring: ahead of every stick
    auto on = [&](int i) { return i < static_cast<int>(shown.size()) && shown[i]; };
    auto dist = [&](const Vec3& p, double& d) {
        double x, y;
        if (!to_screen(p, x, y)) return false;
        d = std::hypot(mx - x, my - y);
        return true;
    };
    std::vector<std::pair<double, int>> hits;
    if (sticks) {
        std::vector<double> best(static_cast<size_t>(skel.joint_count()), 1e30);
        for (const StickSegment& g : stick_segments(skel, globals, shape, shown)) {
            double ax, ay, bx, by;
            if (!to_screen(g.a, ax, ay) || !to_screen(g.b, bx, by)) continue;
            const double abx = bx - ax, aby = by - ay, apx = mx - ax, apy = my - ay;
            const double t = std::clamp((apx * abx + apy * aby) / std::max(abx * abx + aby * aby, 1e-9), 0.0, 1.0);
            if (const double d = std::hypot(apx - abx * t, apy - aby * t); d <= 10.0)
                best[g.node] = std::min(best[g.node], d + 0.04 * std::min(std::hypot(abx, aby), 200.0));
        }
        for (int i = 0; i < skel.joint_count(); ++i) {
            double d;
            if (!on(i)) continue;
            if (dist(globals[i].pos, d)) {
                int fold = 0;  // the shown joints before it on the same spot: drawn as a ring around them
                for (int j = 0; j < i; ++j)
                    if (on(j) && (globals[j].pos - globals[i].pos).length() < 0.002) ++fold;
                if (fold > 0) {
                    if (const double rd = std::fabs(d - (kRing + fold * kRingStep)); rd <= 5.0)
                        best[i] = std::min(best[i], rd * 0.5 + (rd <= kRingHalf ? kOnTop : 0));
                }
                // On the spot itself the stacked joints rank by order, so stepping walks the stack.
                if (d <= 9.0) best[i] = std::min(best[i], d * 0.5 + fold * 1e-4 + (d <= kDot ? kOnTop : 0));
            }
            if (best[i] < 1e30) hits.emplace_back(best[i], i);
        }
    }
    for (int i = skel.joint_count(); i < skel.size(); ++i) {  // attachment points (a dot each) and collision volumes
        double d;
        if (on(i) && dist(globals[i].pos, d) && d <= 9.0)
            hits.emplace_back(d * 0.5 + (!skel[i].volume && d <= kPoint ? kOnTop : 0), i);
    }
    std::sort(hits.begin(), hits.end());
    std::vector<int> ranked;
    for (const auto& h : hits) ranked.push_back(h.second);
    return ranked;
}

int next_stacked(const std::vector<int>& ranked, int current, int pick) {
    const auto it = std::find(ranked.begin(), ranked.end(), current);
    if (ranked.size() < 2 || it == ranked.end()) return pick;
    return ranked[size_t(it - ranked.begin() + 1) % ranked.size()];
}

}  // namespace vats
