// Viewport Avatar Toolset - putting any rigged model on SL's skeleton. See vats/rig_map.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/rig_map.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "guard.h"
#include "vats/fbx.h"
#include "vats/json.h"
#include "vats/soft_body.h"

namespace vats {

const RigMapBone* RigMap::find(std::string_view source) const {
    for (const RigMapBone& b : bones)
        if (b.source == source) return &b;
    return nullptr;
}
RigMapBone* RigMap::find(std::string_view source) {
    return const_cast<RigMapBone*>(static_cast<const RigMap*>(this)->find(source));
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------------
// Names

enum class Role { None, Root, Pelvis, Spine, Chest, Neck, Head, Jaw, Eye, Ear, Collar, UpperArm, Forearm, Hand, Finger,
                  Thigh, Shin, Foot, Toe, Tail, Wing, Helper, Tip, Soft };  // Soft: a butt, breast, belly or love-handle helper

struct Parsed {
    int side = 0;            // +1 left, -1 right, 0 none
    std::string mirror;      // the same name for the other side; "" without a side
    Role role = Role::None;
    std::string finger;      // Thumb, Index, Middle, Ring or Pinky
    bool hind = false, front = false;
    bool twist = false;      // a twist, roll or metatarsal bone: it shares its limb bone's joint
    bool mid = false;        // "mid": the middle finger, on a finger ("lMid1"); a mid leg elsewhere
};

// MMD's Japanese bone words in English, longest first so 手首 (wrist) wins over 手 (hand); sides become words.
const std::pair<const char*, const char*> kJapanese[] = {
    {"全ての親", " root "}, {"上半身", " spine "}, {"下半身", " pelvis "}, {"センター", " center "}, {"グルーブ", " groove "},
    {"人差指", " index "}, {"人指", " index "}, {"親指", " thumb "}, {"中指", " middle "}, {"薬指", " ring "}, {"小指", " pinky "},
    {"手首", " wrist "}, {"足首", " ankle "}, {"つま先", " toe "}, {"ひじ", " elbow "}, {"ひざ", " knee "}, {"両目", " eyes "},
    {"捩", " twist "}, {"肩", " shoulder "}, {"腕", " arm "}, {"肘", " elbow "}, {"膝", " knee "}, {"首", " neck "},
    {"頭", " head "}, {"目", " eye "}, {"手", " hand "}, {"足", " leg "}, {"腰", " waist "}, {"先", " end "},
    {"左", " Left "}, {"右", " Right "}};

// Full-width ASCII (ＩＫ, １) as plain ASCII, and MMD's Japanese words in English: "左腕捩" -> " Left  arm  twist ".
std::string plain_name(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        if (a == 0xEF && i + 2 < s.size()) {  // U+FF01..U+FF5E: EF BC 81..BF, EF BD 80..9E
            const unsigned char b = static_cast<unsigned char>(s[i + 1]), c = static_cast<unsigned char>(s[i + 2]);
            const int cp = 0xF000 | ((b & 0x3F) << 6) | (c & 0x3F);
            if (cp >= 0xFF01 && cp <= 0xFF5E) {
                out += char(cp - 0xFEE0);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    if (std::all_of(out.begin(), out.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) return out;
    for (const auto& [ja, en] : kJapanese)
        for (size_t at = out.find(ja); at != std::string::npos; at = out.find(ja, at)) out.replace(at, std::strlen(ja), en);
    return out;
}

// Words of a name: separators, camel case and digits split it ("UpperArm" -> upper, arm; "Index1" -> index, 1).
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string w;
    auto flush = [&] {
        if (!w.empty()) out.push_back(lower(w)), w.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c)) {
            flush();
            continue;
        }
        if (!w.empty()) {
            const unsigned char p = static_cast<unsigned char>(w.back());
            const bool camel = std::isupper(c) && std::islower(p);
            const bool digit = bool(std::isdigit(c)) != bool(std::isdigit(p));
            // "IKFoot": a run of capitals ends at the last one, which starts the next word.
            const bool acronym = std::isupper(p) && std::isupper(c) && i + 1 < s.size() &&
                                 std::islower(static_cast<unsigned char>(s[i + 1]));
            if (camel || digit || acronym) flush();
        }
        w += char(c);
    }
    flush();
    return out;
}

// l <-> r, left <-> right, keeping the case.
std::string swap_side(const std::string& s) {
    const std::string l = lower(s);
    std::string t = l == "l" ? "r" : l == "r" ? "l" : l == "left" ? "right" : "left";
    if (!s.empty() && std::isupper(static_cast<unsigned char>(s[0]))) {
        const bool all = s.size() > 1 && std::isupper(static_cast<unsigned char>(s[1]));
        for (size_t i = 0; i < t.size(); ++i)
            if (i == 0 || all) t[i] = char(std::toupper(static_cast<unsigned char>(t[i])));
    }
    return t;
}

// The side a name gives, the name without it, and the name for the other side. A single letter counts only beside a
// separator ("thigh.L", "L_arm", "Bip01 L Thigh"), so "PalmR" is a palm, not a right side; Left and Right count as
// words anywhere ("LeftUpLeg", "HandRight").
int side_of(const std::string& name, std::string& base, std::string& mirror) {
    static const std::regex suffix(R"(^(.*[._\- ])(l|r|left|right)((?:[._\-]?\d+)?)$)", std::regex::icase);
    static const std::regex prefix(R"(^()(l|r|left|right)([._\- ].*)$)", std::regex::icase);
    static const std::regex infix(R"(^(.*[._\- ])(l|r)([._\- ].*)$)", std::regex::icase);
    static const std::regex word(R"(^(.*?)(Left|Right|LEFT|RIGHT|left|right)(.*)$)");
    std::smatch m;
    for (const std::regex* re : {&suffix, &prefix, &infix, &word})
        if (std::regex_match(name, m, *re)) {
            const std::string side = m[2].str();
            base = m[1].str() + " " + m[3].str();
            mirror = m[1].str() + swap_side(side) + m[3].str();
            const char c = char(std::tolower(static_cast<unsigned char>(side[0])));
            return c == 'l' ? 1 : -1;
        }
    base = name;
    mirror.clear();
    return 0;
}

// names: the rig's bone names, when known. A side written in camel case ("lThighBend", "HandR") counts only when the
// rig has the other side's name too, so a palm named "PalmR" stays a palm.
Parsed parse_name(const std::string& full, const std::set<std::string>* names = nullptr) {
    Parsed p;
    const size_t cut = full.find_last_of(":|");  // "mixamorig:Hips", "Armature|Hips"
    const std::string raw = cut == std::string::npos ? full : full.substr(cut + 1);
    const std::string prefix = cut == std::string::npos ? "" : full.substr(0, cut + 1);
    const std::string name = plain_name(raw);
    std::string base;
    p.side = side_of(name, base, p.mirror);
    if (p.side && name != raw) {  // the other side in the file's own spelling: 左 <-> 右
        p.mirror = raw;
        const bool left = p.side > 0;
        const std::string from = left ? "左" : "右", to = left ? "右" : "左";
        const size_t at = p.mirror.find(from);
        if (at == std::string::npos) p.mirror.clear();
        else p.mirror.replace(at, from.size(), to);
    }
    if (!p.side && names) {
        static const std::regex camel_prefix(R"(^([lr])([A-Z0-9].*)$)"), camel_suffix(R"(^(.*[a-z0-9])([LR])$)");
        std::smatch m;
        const bool front = std::regex_match(name, m, camel_prefix);
        if (front || std::regex_match(name, m, camel_suffix)) {
            const std::string side = front ? m[1].str() : m[2].str(), rest = front ? m[2].str() : m[1].str();
            const std::string other = side == "l" ? "r" : side == "r" ? "l" : side == "L" ? "R" : "L";
            const std::string mirror = front ? other + rest : rest + other;
            if (names->count(prefix + mirror)) {
                p.side = side == "l" || side == "L" ? 1 : -1;
                p.mirror = mirror;
                base = rest;
            }
        }
    }
    if (!p.mirror.empty()) p.mirror = prefix + p.mirror;
    const std::vector<std::string> w = words(base);
    auto has = [&](std::initializer_list<const char*> keys) {
        for (const std::string& x : w)
            for (const char* k : keys)
                if (x == k) return true;
        return false;
    };
    p.hind = has({"hind", "rear", "back", "haunch"});
    p.front = has({"front", "fore"});
    p.twist = has({"twist", "roll", "metatarsal", "metatarsals"});
    p.mid = has({"mid"});
    const bool arm = has({"arm"}), leg = has({"leg"});
    if (!w.empty() && (w.back() == "end" || w.back() == "tip" || w.back() == "nub")) p.role = Role::Tip;
    else if (has({"ik", "pole", "target", "ctrl", "control", "ctl", "mch", "goal", "locator", "helper", "aim", "ref", "track", "offset"})) p.role = Role::Helper;
    else if (has({"butt", "bum", "buttock", "buttocks", "glute", "glutes", "gluteus", "booty", "breast", "breasts", "boob", "boobs",
                  "pec", "pecs", "pectoral", "bust", "belly", "tummy", "paunch", "handle", "handles", "lovehandle", "lovehandles"}))
        p.role = Role::Soft;
    else if (has({"thumb", "index", "middle", "ring", "pinky", "pinkie", "little", "finger", "digit"})) {
        p.role = Role::Finger;
        p.finger = has({"thumb"}) ? "Thumb" : has({"index"}) ? "Index" : has({"middle"}) ? "Middle" : has({"ring"}) ? "Ring"
                 : has({"pinky", "pinkie", "little"}) ? "Pinky" : "";
    }
    else if (has({"toe", "toes", "ball"})) p.role = Role::Toe;
    else if (has({"foot", "feet", "ankle", "heel", "paw", "hoof"})) p.role = Role::Foot;
    else if (has({"thigh", "femur", "upleg"}) || (leg && has({"upper", "up"})) || (p.side && has({"hip"}))) p.role = Role::Thigh;
    else if (has({"shin", "calf", "knee", "tibia"}) || leg) p.role = Role::Shin;
    else if (has({"forearm", "lowerarm", "elbow", "radius", "ulna"}) || (arm && has({"lower", "fore"}))) p.role = Role::Forearm;
    else if (has({"upperarm", "humerus", "bicep", "shldr"}) || arm) p.role = Role::UpperArm;
    else if (has({"clavicle", "collar", "collarbone", "shoulder", "scapula"})) p.role = Role::Collar;
    else if (has({"hand", "palm", "wrist", "manus", "carpal", "metacarpal"})) p.role = Role::Hand;
    else if (has({"tail"})) p.role = Role::Tail;
    else if (has({"wing"})) p.role = Role::Wing;
    else if (has({"ear"})) p.role = Role::Ear;
    else if (has({"eye", "eyes", "eyeball"})) p.role = Role::Eye;
    else if (has({"jaw", "mandible", "chin"})) p.role = Role::Jaw;
    else if (has({"head", "skull", "cranium"})) p.role = Role::Head;
    else if (has({"neck"})) p.role = Role::Neck;
    else if (has({"chest", "ribcage", "thorax", "breast"})) p.role = Role::Chest;
    else if (has({"spine", "torso", "abdomen", "belly", "waist", "lumbar", "stomach"})) p.role = Role::Spine;
    else if (has({"pelvis", "hips", "hip", "body", "cog"})) p.role = Role::Pelvis;
    else if (has({"root", "armature", "skeleton", "rig", "master"})) p.role = Role::Root;
    return p;
}

// ---------------------------------------------------------------------------------------------
// The suggestion

struct Chain {
    std::vector<int> bones;
    std::vector<std::string> targets;  // SL names, as many as the chain may fill (bones beyond fold)
    std::string what;                  // "left leg"
};

// A name without a trailing number: "b_LeftLeg01_015" and "b_RightLeg01_019" are a pair.
std::string without_number(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && std::isdigit(static_cast<unsigned char>(s[e - 1]))) --e;
    if (e < s.size() && e > 0 && (s[e - 1] == '_' || s[e - 1] == '.' || s[e - 1] == '-')) --e;
    return s.substr(0, e);
}

class Suggest {
public:
    Suggest(const Skeleton& s, const std::vector<SourceBone>& b, const std::vector<RigTable>& t, bool bento)
        : skel(s), bones(b), tables(t), n(int(b.size())), bento(bento) {}
    RigMap run();

private:
    const Skeleton& skel;
    const std::vector<SourceBone>& bones;
    const std::vector<RigTable>& tables;
    const int n;
    const bool bento;
    RigMap map;
    std::vector<Parsed> name;
    std::vector<std::vector<int>> kids;
    std::vector<double> sub;  // weight in the bone and below it
    std::vector<int> side;    // by name, else by where it sits once the facing is known
    double floor_z = 0, height = 1;
    double size = 1;          // the larger of the bones' height and their spread across the ground
    int turn = 0;
    std::vector<Chain> chains;
    std::vector<char> in_limb;  // a bone some leg, arm, tail or wing chain holds
    std::vector<char> sat;      // a bone mapped because it sits on another's joint
    std::vector<std::pair<int, int>> pairs;  // mirrored limbs, by their first bones (named: left first)
    Vec3 lateral;  // across the body, before the turn (towards the left when names say which side is); zero if unknown
    Vec3 mid;      // a point on the body's midline: the pairs' middle, else its first weighted bone
    bool lying = false;  // the body lies along its spine (four legs, a bird, a fish) rather than standing up it
    bool humanoid = false;  // a rig table mapped a whole humanoid
    double tol = 0;      // how far off the midline a bone still counts as centred

    Vec3 at(int i) const { return bones[i].bind.pos; }
    static Vec3 flat(Vec3 v) { return v.z = 0, v; }
    Vec3 turned(const Vec3& v) const { return Quat::axis_angle({0, 0, 1}, turn * kPi / 2).rotate(v); }
    bool taken(const std::string& sl) const {
        for (const RigMapBone& b : map.bones)
            if (b.target == sl) return true;
        return false;
    }
    void set(int i, const std::string& sl, int confidence, const std::string& reason) {
        if (i < 0 || i >= n || !map.bones[i].target.empty() || skel.find(sl) < 0) return;
        map.bones[i].target = sl;
        map.bones[i].confidence = confidence;
        map.bones[i].reason = reason;
    }
    std::vector<int> weighted_kids(int i) const {
        std::vector<int> out;
        for (int k : kids[i])
            if (sub[k] > 0) out.push_back(k);
        return out;
    }
    // A bone that shares its limb bone's joint: a twist, roll or metatarsal bone, or a limb bone's second segment named
    // after it ("upper_arm.L.001" under "upper_arm.L"). It folds into the joint above it.
    bool extra(int i) const {
        if (name[i].twist) return true;
        const int p = bones[i].parent;
        if (p < 0 || !name[i].side) return false;
        const std::string& s = bones[i].name;
        size_t e = s.size();
        while (e > 0 && std::isdigit(static_cast<unsigned char>(s[e - 1]))) --e;
        return e < s.size() && e > 1 && std::strchr("._-", s[e - 1]) && s.substr(0, e - 1) == bones[p].name;
    }
    // Whether a bone of this role (not an extra one) carries weights at or below i.
    bool holds(int i, Role r) const {
        if (sub[i] <= 0) return false;
        if (name[i].role == r && !extra(i)) return true;
        for (int k : kids[i])
            if (holds(k, r)) return true;
        return false;
    }
    // Down a limb: each step to the only weighted child, until the limb branches or ends (a hand bone ends an arm).
    // Twist bones beside the limb bone do not end it, nor does a branch where one child alone leads on to the limb's
    // end (toward: the hand or the foot, by name), so the limb is the path to its end, whatever hangs off it.
    std::vector<int> follow(int b, bool stop_at_hand = false, Role toward = Role::None) const {
        std::vector<int> c{b};
        for (int cur = b; c.size() < 16;) {
            if (stop_at_hand && name[cur].role == Role::Hand && !extra(cur)) break;
            std::vector<int> w = weighted_kids(cur);
            auto keep = [&](auto drop) {
                std::vector<int> rest;
                for (int k : w)
                    if (!drop(k)) rest.push_back(k);
                if (!rest.empty()) w = rest;
            };
            if (w.size() > 1) keep([&](int k) { return extra(k); });
            if (w.size() > 1 && toward != Role::None) keep([&](int k) { return !holds(k, toward); });
            if (w.size() != 1) break;
            c.push_back(cur = w[0]);
        }
        return c;
    }
    bool below(int i, int anc) const {
        for (int k = i; k >= 0; k = bones[k].parent)
            if (k == anc) return true;
        return false;
    }
    int common(int a, int b) const {
        for (int k = a; k >= 0; k = bones[k].parent)
            if (below(b, k)) return k;
        return -1;
    }
    // An unweighted leaf under i on its side (an _end tip, not an eyebrow control): the end of its bone, where the next SL
    // joint goes.
    int tip_of(int i) const {
        int best = -1;
        for (int k : kids[i])
            if (sub[k] <= 0 && kids[k].empty() && name[k].role != Role::Helper && name[k].side == name[i].side &&
                (best < 0 || (at(k) - at(i)).length() > (at(best) - at(i)).length()))
                best = k;
        return best;
    }
    Vec3 end_of(const std::vector<int>& c) const {
        const int t = tip_of(c.back());
        return at(t >= 0 ? t : c.back());
    }
    double lowest(const std::vector<int>& c) const {
        double z = end_of(c).z;
        for (int b : c) z = std::min(z, at(b).z);
        return z;
    }
    std::string sided(const std::string& stem, int s, const std::string& tail = "") const {
        return stem + tail + (s > 0 ? "Left" : "Right");
    }
    int position_side(int i) const {  // +Y is left once the model faces +X
        const double y = turned(at(i) - mid).y;
        return std::fabs(y) < tol ? 0 : y > 0 ? 1 : -1;
    }
    double ahead(int i) const { return turned(at(i)).x; }  // how far forward, once facing +X

    void find_pairs();
    void find_facing(const std::vector<std::vector<int>>& legs);
    void map_chain(Chain c, int confidence);
    void place_tips();
    void sit_on_joints();
    void map_fingers(const std::vector<int>& hands, int s);  // s: the side
};

// Mirrored limbs: two weighted children of one bone whose chains are alike and level, mirror images across a vertical
// plane through their parent. Names pair them first (.L with .R); then the closest such pairs. They give the axis
// across the body, and with it which way along the body to look for a head and a tail.
void Suggest::find_pairs() {
    std::vector<char> used(n, 0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n && name[i].side > 0 && sub[i] > 0 && !used[i]; ++j)
            if (name[j].side < 0 && sub[j] > 0 && !used[j] && bones[j].parent == bones[i].parent &&
                without_number(name[i].mirror) == without_number(bones[j].name))
                pairs.push_back({i, j}), used[i] = used[j] = 1;
    const size_t named = pairs.size();
    struct Candidate {
        double half;
        int a, b;
    };
    std::vector<Candidate> found;
    for (int p = 0; p < n; ++p) {
        const std::vector<int> w = weighted_kids(p);
        for (size_t x = 0; x < w.size(); ++x)
            for (size_t y = x + 1; y < w.size(); ++y) {
                const int a = w[x], b = w[y];
                const Vec3 d = flat(at(a) - at(b));
                const double half = d.length() / 2;
                // Long chains may differ by a bone (an eagle's wings are 5 and 4 bones); short ones must match.
                const size_t la = follow(a).size(), lb = follow(b).size();
                if (name[a].side || name[b].side || half < 0.01 * size || (la != lb && (std::min(la, lb) < 3 || std::max(la, lb) - std::min(la, lb) > 1)) ||
                    std::fabs(at(a).z - at(b).z) > 0.05 * size)
                    continue;
                // The parent on the mirror plane: as far from one as from the other, across.
                if (std::fabs(flat(at(p) - (at(a) + at(b)) * 0.5).dot(d) / d.length()) > 0.25 * half) continue;
                found.push_back({half, a, b});
            }
    }
    std::sort(found.begin(), found.end(), [](const Candidate& l, const Candidate& r) { return l.half < r.half; });
    for (const Candidate& c : found)
        if (!used[c.a] && !used[c.b]) pairs.push_back({c.a, c.b}), used[c.a] = used[c.b] = 1;
    for (size_t k = 0; k < pairs.size(); ++k) {  // each pair's across, all pointing one way (to the left, when named)
        Vec3 d = flat(at(pairs[k].first) - at(pairs[k].second)).normalized();
        if (k >= named && d.dot(lateral) < 0) d = d * -1.0, std::swap(pairs[k].first, pairs[k].second);
        lateral += d;
    }
    // Snapped to the file's nearer axis: models are built square to their axes, and a slightly lopsided pair (a fish's
    // fins) would otherwise tilt the midline enough to push the far end of a long body off it.
    if (std::fabs(lateral.x) >= std::fabs(lateral.y)) lateral = {lateral.x > 0 ? 1.0 : lateral.x < 0 ? -1.0 : 0.0, 0, 0};
    else lateral = {0, lateral.y > 0 ? 1.0 : -1.0, 0};
    for (const auto& [a, b] : pairs) mid += (at(a) + at(b)) * (0.5 / double(pairs.size()));  // the pairs' middle
    for (int i = 0; i < n && pairs.empty(); ++i)  // else the first bone with weights
        if (bones[i].weight > 0) {
            mid = at(i);
            break;
        }
    // Upright or lying: the centred bones' spread along the body against their height.
    if (lateral.length() > 0) {
        const Vec3 along = lateral.cross({0, 0, 1});
        double lo = 1e300, hi = -1e300, bottom = 1e300, top = -1e300;
        for (int i = 0; i < n; ++i)
            if (bones[i].weight > 0 && !name[i].side && std::fabs((at(i) - mid).dot(lateral)) < 0.03 * size) {
                lo = std::min(lo, at(i).dot(along)), hi = std::max(hi, at(i).dot(along));
                bottom = std::min(bottom, at(i).z), top = std::max(top, at(i).z);
            }
        lying = hi > lo && hi - lo > top - bottom;
    }
    tol = lying ? 0.03 * size : 0.02 * height;
}

void Suggest::find_facing(const std::vector<std::vector<int>>& legs) {
    // Votes for the model's forward direction (horizontal, before any turn), from the strongest evidence down.
    Vec3 sides, knees, toes, face, spine;
    for (int i = 0; i < n; ++i)
        if (name[i].side > 0)
            for (int j = 0; j < n; ++j)
                if (name[j].side < 0 && without_number(bones[j].name) == without_number(name[i].mirror))
                    sides += at(i) - at(j);  // left minus right
    sides.z = 0;
    for (int i = 0; i < n; ++i)  // the eyes and the jaw sit forward of the head they hang from
        if (name[i].role == Role::Eye || name[i].role == Role::Jaw)
            for (int h = bones[i].parent; h >= 0; h = bones[h].parent)
                if (name[h].role == Role::Head) {
                    const Vec3 d = flat(at(i) - at(h));
                    if (d.length() > 1e-6) face += d.normalized();
                    break;
                }
    for (const std::vector<int>& c : legs) {
        if (c.size() >= 3 && legs.size() == 2) {  // the knee bends forward of the line from hip to ankle (a
            // digitigrade's first knee too); four legs bend both ways (a horse's hocks), so they say nothing
            const Vec3 k = flat(at(c[1]) - (at(c[0]) + at(c[2])) * 0.5);
            if (k.length() > 1e-6) knees += k.normalized();
        }
        // The toes: the end of the heaviest bone at the foot (the chain's last joint, or a bone sitting on it or on the
        // chain's end), when it is a foot: named so, a long leg's last bone, or lying flat. A shin standing on its tip
        // says nothing.
        const int last = c.back();
        const Vec3 end = end_of(c);
        int foot = last;
        for (int i = 0; i < n; ++i)
            if (((at(i) - at(last)).length() < 0.02 * height || (at(i) - end).length() < 0.02 * height) && sub[i] > sub[foot])
                foot = i;
        const int t = tip_of(foot);
        if (t < 0) continue;
        const Vec3 d = at(t) - at(foot);
        const bool is_foot = name[foot].role == Role::Foot || name[foot].role == Role::Toe || c.size() >= 4 ||
                             flat(d).length() > 2 * std::fabs(d.z);
        if (is_foot && flat(d).length() > 1e-6) toes += flat(d).normalized();
    }
    // A body lying along its spine: the head is the heavier of its two ends (a tail tapers off). The ends are the
    // centred bones with no weighted child, furthest either way along the body.
    if (lying) {
        const Vec3 along = lateral.cross({0, 0, 1});
        int front = -1, back = -1;
        double hi = -1e300, lo = 1e300;
        for (int i = 0; i < n; ++i) {
            if (bones[i].weight <= 0 || name[i].side || !weighted_kids(i).empty() || std::fabs((at(i) - mid).dot(lateral)) >= tol)
                continue;
            const int t = tip_of(i);
            const double x = at(t >= 0 ? t : i).dot(along);
            if (x > hi) hi = x, front = i;
            if (x < lo) lo = x, back = i;
        }
        if (front >= 0 && back >= 0 && front != back) {
            const double wf = bones[front].weight, wb = bones[back].weight;
            if (wf > 1.5 * wb) spine = along;
            else if (wb > 1.5 * wf) spine = along * -1.0;
        }
    }
    const Vec3 side_fwd{sides.y, -sides.x, 0};  // forward is left turned a quarter clockwise
    struct Vote {
        Vec3 v;
        double w;
        const char* what;
    };
    const Vote votes[] = {{side_fwd, 3, "its left and right bones"}, {knees, 1, "which way its knees bend"},
                          {toes, 2, "where its toes point"},        {face, 1, "where its eyes and jaw are"},
                          {spine, 2, "where its head and tail are"}};
    Vec3 sum;
    for (const Vote& v : votes)
        if (v.v.length() > 1e-9) sum += v.v.normalized() * v.w;
    if (sum.length() < 1e-9) {
        map.facing = "Nothing tells which way the model faces (no left and right bones, legs, face or tail): the file's own facing is kept.";
        return;
    }
    double best = -2;
    for (int k = 0; k < 4; ++k)
        if (const double x = Quat::axis_angle({0, 0, 1}, k * kPi / 2).rotate(sum.normalized()).x; x > best) best = x, turn = k;
    std::string agree, disagree;
    for (const Vote& v : votes) {
        if (v.v.length() < 1e-9) continue;
        const bool yes = Quat::axis_angle({0, 0, 1}, turn * kPi / 2).rotate(v.v.normalized()).x > 0.5;
        (yes ? agree : disagree) += std::string(yes ? agree : disagree).empty() ? v.what : std::string(", ") + v.what;
    }
    map.facing = "Faces SL's forward after a turn of " + std::to_string(turn * 90) + " degrees, from " + agree +
                 (disagree.empty() ? "." : " (" + disagree + " disagree).");
}

// The chain's bones take its SL joints in order. Bones mapped already (by a rig table) anchor it: a bone between two
// anchors takes a joint between theirs, or folds when there is none, so no joint is taken twice. A twist, roll or
// second segment takes none.
void Suggest::map_chain(Chain c, int confidence) {
    auto index_of = [&](int b) {
        const auto it = std::find(c.targets.begin(), c.targets.end(), map.bones[b].target);
        return map.bones[b].target.empty() || it == c.targets.end() ? -1 : int(it - c.targets.begin());
    };
    size_t next = 0;
    for (size_t k = 0; k < c.bones.size(); ++k) {
        const int b = c.bones[k];
        in_limb[b] = 1;
        if (const int a = index_of(b); a >= 0) {
            next = size_t(a) + 1;
            continue;
        }
        if (!map.bones[b].target.empty() || extra(b)) continue;
        size_t limit = c.targets.size();  // the next anchor's joint
        for (size_t j = k + 1; j < c.bones.size() && limit == c.targets.size(); ++j)
            if (const int a = index_of(c.bones[j]); a >= 0) limit = size_t(a);
        while (next < limit && taken(c.targets[next])) ++next;  // another bone has it (a table's)
        if (next < limit)
            set(b, c.targets[next++], confidence, c.what + ", bone " + std::to_string(k + 1) + " of " + std::to_string(c.bones.size()));
    }
    chains.push_back(std::move(c));
}

// A chain shorter than its SL chain places the next SL joint at its last bone's tip (no weights), so the limb's end
// joint sits where the model's limb ends: a two-bone leg's ankle at its foot, a finger's last joint at its tip.
void Suggest::place_tips() {
    for (const Chain& c : chains) {
        size_t len = 0;  // the joints the chain's bones took
        for (int b : c.bones)
            for (size_t k = 0; k < c.targets.size(); ++k)
                if (map.bones[b].target == c.targets[k]) len = std::max(len, k + 1);
        if (!len || len >= c.targets.size() || taken(c.targets[len])) continue;
        // Of the bones on the last joint (merged there), the heaviest one's tip.
        const std::string& last = c.targets[len - 1];
        int from = -1;
        for (int i = 0; i < n; ++i)
            if (map.bones[i].target == last && tip_of(i) >= 0 && (from < 0 || sub[i] > sub[from])) from = i;
        if (from < 0) continue;
        const int t = tip_of(from);
        map.bones[t].target.clear();
        set(t, c.targets[len], 80, "the tip of " + bones[from].name + ": places " + c.targets[len] + " (no weights)");
    }
}

// A weighted bone outside every chain that sits on a mapped joint (an IK foot carrying the toes, a second palm) goes on
// that joint too; its weights merge there.
// It sits only on a joint some other rule placed, never one a bone sat on (they would chain on from bone to bone), nor
// one of its own descendants' (a neck beside the jaw it carries), and only within the bone it hangs from (a scarf off the
// neck does not join the collar it passes). An eye turns to look, so nothing sits on it: eyelids stay with the head.
void Suggest::sit_on_joints() {
    sat.resize(size_t(n), 0);
    for (int i = 0; i < n; ++i) {
        if (!map.bones[i].target.empty() || bones[i].weight <= 0 || in_limb[i]) continue;
        int from = bones[i].parent;  // the mapped bone it hangs from, if any
        while (from >= 0 && map.bones[from].target.empty()) from = bones[from].parent;
        int best = -1;
        for (int j = 0; j < n; ++j)
            if (!map.bones[j].target.empty() && map.bones[j].confidence > 0 && side[j] == side[i] && !sat[j] && !below(j, i) &&
                (from < 0 || below(j, from)) && map.bones[j].target.rfind("mEye", 0) != 0 &&
                (at(j) - at(i)).length() < 0.03 * height && (best < 0 || (at(j) - at(i)).length() < (at(best) - at(i)).length()))
                best = j;
        if (best >= 0) set(i, map.bones[best].target, 60, "sits on " + bones[best].name + "'s joint"), sat[i] = 1;
    }
}

void Suggest::map_fingers(const std::vector<int>& hands, int s) {
    // Fingers: each chain off the hand, by its name, else in the order the hand holds them.
    // A carpal, palm or metacarpal leading into a finger folds into the wrist, and SL's three finger joints go on
    // the last three bones. The finger's name may be on any of its bones ("palm.01" > "f_index.01"); "mid" is
    // the middle finger.
    std::vector<std::vector<int>> fingers;
    std::vector<std::string> named;
    for (int h : hands)
        for (int k : weighted_kids(h)) {
            std::vector<int> f = follow(k);
            std::string kind;
            for (int b : f)
                if (kind.empty()) kind = name[b].finger.empty() && name[b].mid ? "Middle" : name[b].finger;
            auto real = [&] { return std::count_if(f.begin(), f.end(), [&](int b) { return !extra(b); }); };
            while (f.size() > 1 && (name[f[0]].role == Role::Hand || extra(f[0]) || real() > 3)) in_limb[f[0]] = 1, f.erase(f.begin());
            fingers.push_back(f), named.push_back(kind);
        }
    // Unnamed chains are fingers in the order the hand holds them, unless the others are named and they are not called
    // fingers at all (a bandage hanging off the hand).
    const bool any_named = std::any_of(named.begin(), named.end(), [](const std::string& k) { return !k.empty(); });
    std::vector<std::string> order = fingers.size() >= 5 ? std::vector<std::string>{"Thumb", "Index", "Middle", "Ring", "Pinky"}
                                                         : std::vector<std::string>{"Index", "Middle", "Ring", "Pinky"};
    std::set<std::string> used(named.begin(), named.end());
    size_t next = 0;
    for (size_t x = 0; x < fingers.size(); ++x) {
        const std::vector<int>& f = fingers[x];
        std::string kind = named[x];
        if (kind.empty() && any_named && std::none_of(f.begin(), f.end(), [&](int b) { return name[b].role == Role::Finger; })) continue;
        if (kind.empty() && name[f[0]].role == Role::Soft) continue;  // a pad or breast helper off the hand is no finger
        while (kind.empty() && next < order.size())
            if (!used.count(order[next++])) kind = order[next - 1], used.insert(kind);
        if (kind.empty()) continue;
        Chain fc{f, {}, (s > 0 ? "left " : "right ") + lower(kind) + " finger"};
        for (int k = 1; k <= 3; ++k) fc.targets.push_back(sided("mHand" + kind, s, std::to_string(k)));
        map_chain(fc, named[x].empty() ? 55 : 85);
    }
}

RigMap Suggest::run() {
    map.bones.resize(n);
    name.resize(n);
    kids.assign(n, {});
    sub.assign(n, 0);
    side.assign(n, 0);
    in_limb.assign(n, 0);
    Vec3 lo{1e300, 1e300, 1e300}, hi = lo * -1.0;
    std::set<std::string> names;
    for (const SourceBone& b : bones) names.insert(b.name);
    for (int i = 0; i < n; ++i) {
        map.bones[i].source = bones[i].name;
        name[i] = parse_name(bones[i].name, &names);
        side[i] = name[i].side;
        if (bones[i].parent >= 0 && bones[i].parent < i) kids[bones[i].parent].push_back(i);
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], at(i)[k]), hi[k] = std::max(hi[k], at(i)[k]);
    }
    floor_z = n ? lo.z : 0;
    height = n && hi.z > lo.z ? hi.z - lo.z : 1;
    size = n ? std::max({height, hi.x - lo.x, hi.y - lo.y}) : 1;
    for (int i = n - 1; i >= 0; --i) {
        sub[i] += std::max(0.0, bones[i].weight);
        if (bones[i].parent >= 0 && bones[i].parent < i) sub[bones[i].parent] += sub[i];
    }
    map.height = kSlAvatarHeight;

    // 0. Bones already named as SL's joints, or by LL's aliases (Daz's hip, lThigh, lShin): the viewer reads them so.
    bool sl_rigged = n > 0;
    for (int i = 0; i < n; ++i) {
        const int j = viewer_skin_joint(skel, bones[i].name);
        if (j >= 0 && j < skel.joint_count())
            set(i, skel[j].name, 100, skel[j].name == bones[i].name ? "SL's own name" : "LL's alias for " + skel[j].name);
        sl_rigged = sl_rigged && j >= 0 && j < skel.joint_count() && skel[j].name == bones[i].name;
    }
    // Every bone SL's own: a body made for SL (a devkit's part, often not a whole body), at SL's size already. Made
    // "SL-like" by its height, an upper body alone would be blown up to a whole avatar's.
    if (sl_rigged) map.height = 0;

    // 1. A rig table that maps a whole humanoid: the names say it all. The table that maps the most bones wins, less two
    // for each bone its mapping puts out of place (check_rig_map), so names a family shares with another (Spine, Head)
    // do not pick a table whose joints the armature does not nest that way; its hint breaks ties.
    {
        SourceAnim src;
        for (const SourceBone& b : bones) src.joints.push_back({b.name, b.parent, {}, {}, 1});
        BoneMap table;
        int t = -1, best = 0;
        for (int k = 0; k < int(tables.size()); ++k) {
            BoneMap m;
            int score = 4 * apply_rig_table(tables[size_t(k)], src, m);
            if (!score) continue;
            RigMap as_map;
            for (const SourceBone& b : bones) as_map.bones.push_back({b.name, "", 0, "", ""});
            for (auto& [sl, i] : m) as_map.bones[size_t(i)].target = sl;
            for (const std::string& p : check_rig_map(skel, bones, as_map)) score -= p.empty() ? 0 : 8;
            const std::string hint = lower(tables[size_t(k)].hint);
            if (!hint.empty() && std::any_of(bones.begin(), bones.end(), [&](const SourceBone& b) { return lower(b.name).find(hint) != std::string::npos; }))
                ++score;
            if (score > best) best = score, t = k, table = m;
        }
        if (t >= 0 && map_is_usable(table)) {
            humanoid = true;
            for (auto& [sl, i] : table) set(i, sl, 95, "the " + tables[t].name + " name for " + sl);
            map.notes.push_back("The names are " + tables[t].name + "'s: its table maps " + std::to_string(table.size()) +
                                " bones.");
        }
    }

    // 2. Legs. By name: a thigh, or a leg bone hanging from no other leg bone, and the bones below it. Then mirrored
    // pairs of chains that stand on the floor (dropping more than they reach out): legs whatever their names.
    find_pairs();
    if (humanoid) lying = false, tol = 0.02 * height;  // a whole humanoid stands up, however long its tail
    std::vector<std::vector<int>> legs;
    auto leggy = [&](int i) { return name[i].role == Role::Thigh || name[i].role == Role::Shin; };
    for (int i = 0; i < n; ++i)
        if (sub[i] > 0 && (name[i].role == Role::Thigh || (name[i].role == Role::Shin && name[i].side)) &&
            !(bones[i].parent >= 0 && leggy(bones[i].parent)) &&
            (map.bones[i].target.empty() || map.bones[i].target.rfind("mHip", 0) == 0))
            legs.push_back(follow(i, false, Role::Foot));
    auto in_legs = [&](int b) {
        for (const std::vector<int>& c : legs)
            if (std::find(c.begin(), c.end(), b) != c.end()) return true;
        return false;
    };
    for (const auto& [a, b] : pairs) {
        const std::vector<int> ca = follow(a), cb = follow(b);
        auto stands = [&](const std::vector<int>& c) {
            return c.size() >= 2 && lowest(c) < floor_z + 0.2 * height &&
                   at(c[0]).z - lowest(c) > flat(end_of(c) - at(c[0])).length();
        };
        if (!in_legs(a) && !in_legs(b) && stands(ca) && stands(cb) && !(humanoid && legs.size() >= 2)) legs.push_back(ca), legs.push_back(cb);
    }
    find_facing(legs);
    map.turn = turn;
    // Sides the names leave open, from where the bones sit. Upright, a mirrored pair is one of each, however close
    // together (eyes), and a bone whose head is on the midline goes with the limb it carries: a clavicle starting at
    // the spine is on the side its arm reaches out to.
    if (!lying)
        for (const auto& [a, b] : pairs)
            if (!side[a] && !side[b]) {
                const double y = turned(at(a) - at(b)).y;
                side[a] = y > 0 ? 1 : -1, side[b] = -side[a];
            }
    for (int i = n - 1; i >= 0; --i) {
        if (side[i]) continue;
        side[i] = position_side(i);
        if (side[i] || lying || sub[i] <= 0 || bones[i].weight <= 0) continue;
        Vec3 sum;
        int count = 0;
        for (int k = i; k < n; ++k)
            if (below(k, i)) sum += at(k), ++count;
        const double y = turned(sum * (1.0 / count) - mid).y;
        if (std::fabs(y) >= tol) side[i] = y > 0 ? 1 : -1;
    }

    // The pelvis: where the legs branch (all of them, so a quadruped's front legs off its chest still count), else a
    // bone named so, else the first bone that carries weight.
    int pelvis = -1;
    for (const std::vector<int>& c : legs) pelvis = pelvis < 0 ? (bones[c[0]].parent >= 0 ? bones[c[0]].parent : c[0]) : common(pelvis, c[0]);
    std::string pelvis_why = "the legs branch here";
    if (pelvis < 0)
        for (int i = 0; i < n && pelvis < 0; ++i)
            if (name[i].role == Role::Pelvis && sub[i] > 0) pelvis = i, pelvis_why = "named as the hips";
    if (pelvis < 0)
        for (int i = 0; i < n && pelvis < 0; ++i)
            if (bones[i].weight > 0) pelvis = i, pelvis_why = "the first bone with weights";
    if (pelvis < 0) {
        map.notes.push_back("No bone carries weights: there is nothing to map.");
        return map;
    }
    // Split hips (Daz, Character Creator, MMD): the legs hang from a pelvis bone beside the spine, both under the hips,
    // which are mPelvis; the legs' pelvis shares it. A 3ds Max Biped hangs its legs off its first spine bone: the
    // pelvis is the bone above. Either way the hips place mPelvis, so no spine bone shares it.
    int beside = -1;  // the legs' own pelvis bone, beside the spine
    bool split = false;
    if (!lying && legs.size() == 2 && bones[pelvis].parent >= 0) {
        const int up = bones[pelvis].parent;
        auto spine_off = [&](int b, int skip) {
            for (int k : weighted_kids(b))
                if (k != skip && !side[k] && !in_legs(k)) return true;
            return false;
        };
        if (!spine_off(pelvis, -1) && spine_off(up, pelvis)) beside = pelvis, pelvis = up, split = true;
        else if (name[pelvis].role == Role::Spine && name[up].role == Role::Pelvis) pelvis = up, split = true;
        if (split) pelvis_why = "the hips: the legs and the spine branch below here";
    }
    for (int i = 0; i < n; ++i)  // the hips a rig table or SL's own name gives (an export may hang every bone off its root)
        if (map.bones[i].target == "mPelvis" && i != pelvis) pelvis = i, split = true, pelvis_why = map.bones[i].reason;

    // Two pairs of legs: the back pair by name, else by where they sit.
    std::vector<std::vector<int>> front, hind;
    if (legs.size() >= 4) {
        double xs = 0;
        for (const std::vector<int>& c : legs) xs += ahead(c[0]) / double(legs.size());
        for (const std::vector<int>& c : legs) {
            const bool back = name[c[0]].hind || (!name[c[0]].front && ahead(c[0]) < xs);
            (back ? hind : front).push_back(c);
        }
    }
    if (hind.empty()) front = legs;
    // Four legs and a level spine: a quadruped (an upright body with four legs, a centaur, keeps the Bento hind legs).
    const bool quad = lying && legs.size() == 4 && front.size() == 2 && hind.size() == 2;
    map.quadruped = quad;
    map.bento = quad && bento;
    map.along_ground = lying;
    int front_root = -1;  // where the front legs hang from, for the chest
    if (quad) {
        front_root = bones[front[0][0]].parent;
        // A first bone that runs across the body (from the spine out to the shoulder or the hip) is the collar of an
        // arm; on a leg SL has no joint for it, so it folds into the body.
        auto across = [&](const std::vector<int>& c) {
            if (c.size() < 3) return false;
            const Vec3 d = at(c[1]) - at(c[0]);
            return flat(d).length() > 2 * std::fabs(d.z);
        };
        auto as_leg = [&](std::vector<int> c, bool hind_limb, const char* what) {
            const int s = side[c[0]] ? side[c[0]] : 1;
            if (across(c)) in_limb[c[0]] = 1, c.erase(c.begin());
            Chain ch{c, {}, std::string(s > 0 ? "left " : "right ") + what};
            if (hind_limb)
                for (int k = 1; k <= 4; ++k) ch.targets.push_back(sided("mHindLimb", s, std::to_string(k)));
            else
                for (const char* j : {"mHip", "mKnee", "mAnkle", "mFoot", "mToe"}) ch.targets.push_back(sided(j, s));
            map_chain(ch, 75);
        };
        for (const std::vector<int>& c : front) {
            if (bento) {
                as_leg(c, false, "front leg");
                continue;
            }
            const int s = side[c[0]] ? side[c[0]] : 1;
            Chain arm{c, {}, s > 0 ? "left front leg" : "right front leg"};
            if (across(c)) arm.targets.push_back(sided("mCollar", s));
            for (const char* j : {"mShoulder", "mElbow", "mWrist"}) arm.targets.push_back(sided(j, s));
            map_chain(arm, 75);
        }
        for (const std::vector<int>& c : hind) as_leg(c, bento, bento ? "hind leg" : "back leg");
        if (bento) {
            int root = hind[0][0];
            for (const std::vector<int>& c : hind) root = common(root, c[0]);
            root = root >= 0 && root == hind[0][0] ? bones[root].parent : root;
            if (root >= 0 && root != pelvis && below(root, pelvis)) set(root, "mHindLimbsRoot", 75, "the hind legs branch here");
        }
        map.notes.push_back(
            std::string("Four legs and a level spine: a quadruped. ") +
            (bento ? "Bento layout: the front legs go on SL's legs (mHip > mKnee > mAnkle) and the back legs on its hind "
                     "limbs (mHindLimb1..4). Few SL animations move the hind limbs; Front Legs on Arms is the other way."
                   : "The front legs go on SL's arms (mShoulder > mElbow > mWrist) and the back legs on its legs (mHip > "
                     "mKnee > mAnkle), the layout most SL animations and AOs move. Bento puts the back legs on mHindLimb1..4 "
                     "and the front legs on the legs instead."));
    } else {
        for (const std::vector<int>& c : front) {
            const int s = side[c[0]] ? side[c[0]] : 1;
            Chain ch{c, {}, s > 0 ? "left leg" : "right leg"};
            for (const char* j : {"mHip", "mKnee", "mAnkle", "mFoot", "mToe"}) ch.targets.push_back(sided(j, s));
            map_chain(ch, name[c[0]].role == Role::Thigh ? 85 : 65);
        }
        for (const std::vector<int>& c : hind) {
            const int s = side[c[0]] ? side[c[0]] : 1;
            Chain ch{c, {}, s > 0 ? "left hind leg" : "right hind leg"};
            for (int k = 1; k <= 4; ++k) ch.targets.push_back(sided("mHindLimb", s, std::to_string(k)));
            map_chain(ch, 75);
        }
        if (!front.empty()) {
            const std::vector<int>& c = front[0];
            size_t segs = c.size();
            map.notes.push_back(std::string(hind.empty() ? "Legs" : "Front legs") + ": " + std::to_string(segs) + " bones each, onto " +
                                (segs >= 4 ? "mHip > mKnee > mAnkle > mFoot" : segs == 3 ? "mHip > mKnee > mAnkle" : "mHip > mKnee") +
                                (segs >= 4 && name[c[1]].role == Role::Shin && name[c[2]].role == Role::Shin
                                     ? " (a digitigrade leg: three segments and a foot)." : "."));
        }
        if (!hind.empty()) {
            map.notes.push_back("Two pairs of legs: the back pair goes on SL's hind legs, mHindLimb1..4.");
            int root = hind[0][0];
            for (const std::vector<int>& c : hind) root = common(root, c[0]);
            root = root >= 0 && root == hind[0][0] ? bones[root].parent : root;
            if (root >= 0 && root != pelvis && below(root, pelvis))
                set(root, "mHindLimbsRoot", 75, "the hind legs branch here");
        }
    }

    // The head: a bone named so, else the end of the centred chain from the pelvis, up it (an upright body) or forward
    // along it (a lying one). A pelvis whose parents carry no weights shares them with the bones beside it (a bird's
    // neck and tail beside its body).
    auto centred = [&](int k) { return lying ? !side[k] : std::hypot(at(k).x - at(pelvis).x, at(k).y - at(pelvis).y) <= 0.15 * height; };
    auto key = [&](int k) { return lying ? turned(end_of(follow(k))).x : at(k).z; };  // lying: how far forward it reaches
    int head = -1;
    for (int i = 0; i < n && head < 0; ++i)  // the head a rig table or SL's own name gives, wherever it hangs
        if (map.bones[i].target == "mHead") head = i;
    for (int i = 0; i < n && head < 0; ++i)
        if (name[i].role == Role::Head && !name[i].side && sub[i] > 0 && below(i, pelvis)) head = i;
    if (head < 0) {
        head = pelvis;
        for (bool more = true; more;) {
            more = false;
            std::vector<int> next = weighted_kids(head);
            if (head == pelvis)
                for (int a = bones[pelvis].parent; a >= 0 && bones[a].weight <= 0; a = bones[a].parent)
                    for (int k : weighted_kids(a))
                        if (!below(pelvis, k) && !name[k].side) next.push_back(k);
            int best = -1;
            for (int k : next) {
                if (in_limb[k] || name[k].role == Role::Tail || !centred(k)) continue;
                // Lying, two bones reaching about as far forward (a fish's head and its jaw): the heavier is the head.
                const bool tie = lying && best >= 0 && std::fabs(key(k) - key(best)) < 0.05 * size;
                if (best < 0 || (tie ? sub[k] > sub[best] : key(k) > key(best))) best = k;
            }
            if (best >= 0 && key(best) >= key(head) - 0.05 * (lying ? size : height)) head = best, more = true;
        }
    }
    std::vector<int> spine;  // pelvis .. head
    for (int k = head; k >= 0 && k != pelvis && !below(pelvis, k); k = bones[k].parent) spine.insert(spine.begin(), k);
    spine.insert(spine.begin(), pelvis);
    std::set<int> on_spine(spine.begin(), spine.end());
    // A pelvis with no weights of its own shares mPelvis with the first spine bone, which places it.
    const bool shared_pelvis = !split && bones[pelvis].weight <= 0 && spine.size() > 2 && bones[spine[1]].weight > 0;

    // The chest: SL's arms hang from mChest. A quadruped's front legs hang there whatever the model calls it; when they
    // hang from the pelvis, the spine bone above them.
    int chest = -1;
    std::vector<std::vector<int>> arms;
    if (quad && spine.size() > 2) {
        if (on_spine.count(front_root) && front_root != pelvis && front_root != head && !(shared_pelvis && front_root == spine[1]))
            chest = front_root;
        else
            for (size_t s = shared_pelvis ? 2 : 1; s + 1 < spine.size(); ++s)
                if (chest < 0 || std::fabs(ahead(spine[s]) - ahead(front[0][0])) < std::fabs(ahead(chest) - ahead(front[0][0])))
                    chest = spine[s];
    } else if (!lying) {
        // Arms: sided limbs off the spine above the pelvis; a collar first when the second bone is the upper arm.
        for (size_t s = 1; s + (head != pelvis ? 1 : 0) < spine.size(); ++s)  // below the head: its sides are eyes and ears
            for (int k : weighted_kids(spine[s])) {
                const Role r = name[k].role;
                if (on_spine.count(k) || in_limb[k] || !side[k] || r == Role::Tail || r == Role::Wing || r == Role::Ear ||
                    r == Role::Eye || r == Role::Jaw || r == Role::Helper || r == Role::Chest || r == Role::Spine ||
                    r == Role::Pelvis || r == Role::Neck || r == Role::Head || r == Role::Soft)
                    continue;
                arms.push_back(follow(k, true, Role::Hand));
                chest = chest < 0 ? spine[s] : chest;
            }
        // A Biped hangs its clavicles off the neck, though they start on the chest, below the neck and beside the spine:
        // mChest goes on the spine bone below the neck, and the neck keeps mNeck. (A mech's shoulders hung from its neck
        // start out at its sides: the neck bone stays its chest.)
        const auto it = std::find(spine.begin(), spine.end(), chest);
        if (chest >= 0 && it != spine.end() && it - spine.begin() >= 2 && name[chest].role == Role::Neck &&
            std::all_of(arms.begin(), arms.end(), [&](const std::vector<int>& a) {
                return at(a[0]).z < at(chest).z && flat(at(a[0]) - at(chest)).length() < 0.04 * height;
            }))
            chest = *(it - 1);
    }
    // A lying body without four legs (a bird, a fish): its mirrored pairs are wings or fins; the spine bone they
    // branch from is the chest.
    std::vector<std::pair<int, int>> wings;
    if (lying && !quad)
        for (const auto& [a, b] : pairs)
            if (!in_limb[a] && !in_limb[b] && !on_spine.count(a) && bones[a].parent != head) {
                wings.push_back({a, b});
                if (chest < 0 && on_spine.count(bones[a].parent) && bones[a].parent != pelvis) chest = bones[a].parent;
            }
    std::set<int> done;  // hands whose fingers are mapped
    for (const std::vector<int>& c0 : arms) {
        std::vector<int> c = c0;
        const int s = side[c[0]];
        const std::string what = s > 0 ? "left arm" : "right arm";
        // The hand: the bone named so, or where the fingers branch; palms side by side are one hand together.
        std::vector<int> hands;
        const int end = c.back();
        const std::vector<int> wk = weighted_kids(end);
        if (name[end].role == Role::Hand) {
            hands = {end};
            c.pop_back();
        } else if (wk.size() >= 2 && std::all_of(wk.begin(), wk.end(), [&](int k) { return name[k].role == Role::Hand; })) {
            hands = wk;
        } else if (wk.size() >= 2) {
            hands = {end};
            c.pop_back();
        }
        Chain arm{c, {}, what};
        std::vector<int> real;  // without the twist bones and second segments, which fold
        std::copy_if(c.begin(), c.end(), std::back_inserter(real), [&](int b) { return !extra(b); });
        const bool collar = real.size() >= 2 && (name[real[0]].role == Role::Collar ||
                                                 (name[real[1]].role == Role::UpperArm && name[real[0]].role != Role::UpperArm) ||
                                                 (real.size() >= 3 && hands.empty()));
        if (collar) arm.targets.push_back(sided("mCollar", s));
        arm.targets.push_back(sided("mShoulder", s));
        arm.targets.push_back(sided("mElbow", s));
        if (hands.empty()) arm.targets.push_back(sided("mWrist", s));
        map_chain(arm, name[c0[0]].role != Role::None ? 85 : 65);
        for (int h : hands) {
            in_limb[h] = 1;
            set(h, sided("mWrist", s), 85, hands.size() > 1 ? "one of " + std::to_string(hands.size()) + " palm bones: together the " + what.substr(0, what.find(' ')) + " hand"
                                                             : "the " + what.substr(0, what.find(' ')) + " hand");
        }
        map_fingers(hands, s);
        done.insert(hands.begin(), hands.end());
    }
    // Hands a rig table placed that no arm reached (an export that flattened the arm bones under the root).
    for (int i = 0; i < n; ++i)
        if (const std::string& t = map.bones[i].target; !done.count(i) && (t == "mWristLeft" || t == "mWristRight"))
            map_fingers({i}, t == "mWristLeft" ? 1 : -1);
    if (chest < 0)  // no arms: the bone named chest, if any
        for (int b : spine)
            if (name[b].role == Role::Chest) chest = b;

    // The spine, pelvis to head, onto mPelvis > mTorso > mChest > mNeck > mHead.
    set(pelvis, "mPelvis", 85, pelvis_why);
    if (beside >= 0) {
        set(beside, "mPelvis", 80, "the legs hang here, beside the spine: it shares mPelvis with " + bones[pelvis].name);
        in_limb[beside] = 1;
    }
    size_t from = 1;
    if (shared_pelvis && spine[1] != chest) {
        set(spine[1], "mPelvis", 75, bones[pelvis].name + " carries no weights: " + bones[spine[1]].name +
                                         ", the first spine bone, places mPelvis");
        map.notes.push_back(bones[pelvis].name + " and " + bones[spine[1]].name + " both go on mPelvis; " +
                            bones[spine[1]].name + " carries the weights, so it places the joint.");
        from = 2;
    }
    if (head != pelvis) set(head, "mHead", name[head].role == Role::Head ? 90 : 65, name[head].role == Role::Head ? "named as the head" : lying ? "the front end of the spine" : "the top of the spine");
    std::vector<int> mid_spine(spine.begin() + std::min(from, spine.size()), spine.end() - (head != pelvis ? 1 : 0));
    const auto ci = std::find(mid_spine.begin(), mid_spine.end(), chest);
    if (ci != mid_spine.end()) {
        set(chest, "mChest", 80, quad ? (bento ? "the front legs hang here" : "the front legs hang here (SL's arms hang from mChest)")
                                 : !wings.empty() && arms.empty() ? "the wings or fins branch here"
                                 : arms.empty() ? "named as the chest" : "the arms branch here (SL's hang from mChest)");
        if (ci != mid_spine.begin()) set(mid_spine.front(), "mTorso", 75, "between the pelvis and the chest");
        if (ci + 1 != mid_spine.end()) set(*(ci + 1), "mNeck", 75, "between the chest and the head");
    } else {
        for (int b : mid_spine) {
            const Role r = name[b].role;
            if (r == Role::Neck && !taken("mNeck")) set(b, "mNeck", 80, "named as the neck");
            else if (r == Role::Spine && !taken("mTorso")) set(b, "mTorso", 75, "named as the spine");
        }
        // Upright, up from the pelvis; lying with no chest to go by, back from the head (a bird's long neck).
        const char* up[] = {"mTorso", "mChest", "mNeck"};
        const char* back[] = {"mNeck", "mChest", "mTorso"};
        std::vector<int> order(mid_spine.begin(), mid_spine.end());
        if (lying) std::reverse(order.begin(), order.end());
        size_t k = 0;
        for (int b : order)
            while (map.bones[b].target.empty() && k < 3)
                if (const char* j = (lying ? back : up)[k++]; !taken(j)) set(b, j, 60, "the spine, in order");
    }
    for (int b : mid_spine)
        if (map.bones[b].target.empty()) on_spine.insert(b);  // folds into the joint below it
    chains.push_back({{head}, {"mHead", "mSkull"}, "head"});

    // A lying body's wings or fins, its tail (a centred chain that runs back from the pelvis, or from beside it) and
    // jaw (a centred bone at the head, below it), whatever their names.
    for (const auto& [a, b] : wings)
        for (int r : {a, b}) {
            Chain c{follow(r), {}, side[r] > 0 ? "left wing or fin" : "right wing or fin"};
            for (int k = 1; k <= 4; ++k) c.targets.push_back(sided("mWing", side[r], std::to_string(k)));
            c.targets.push_back(sided("mWing4Fan", side[r]));
            if (side[r]) map_chain(c, 60);
        }
    if (lying)
        for (int i = 0; i < n; ++i) {
            const int p = bones[i].parent;
            if (sub[i] <= 0 || in_limb[i] || on_spine.count(i) || side[i] || !map.bones[i].target.empty()) continue;
            const std::vector<int> c = follow(i);
            const Vec3 end = turned(end_of(c));
            if ((p < 0 || on_spine.count(p) || below(pelvis, p)) && !taken("mTail1") && end.x < ahead(pelvis) - tol &&
                end.x < ahead(i)) {
                Chain t{c, {}, "tail"};
                for (int k = 1; k <= 6; ++k) t.targets.push_back("mTail" + std::to_string(k));
                map_chain(t, 65);
            } else if ((p == head || (head >= 0 && p == bones[head].parent)) && head != pelvis && i != head &&
                       at(i).z < at(head).z && ahead(i) > ahead(p)) {
                set(i, "mFaceJaw", 55, "below the head, at its front: the jaw");
            }
        }

    // Tails, wings, the jaw, eyes and ears, by name.
    for (int i = 0; i < n; ++i) {
        const int p = bones[i].parent;
        if (sub[i] <= 0 || in_limb[i] || on_spine.count(i) || (p >= 0 && name[p].role == name[i].role)) continue;
        const Role r = name[i].role;
        if (r == Role::Tail) {
            Chain c{follow(i), {}, "tail"};
            for (int k = 1; k <= 6; ++k) c.targets.push_back("mTail" + std::to_string(k));
            map_chain(c, 80);
        } else if (r == Role::Wing && side[i]) {
            Chain c{follow(i), {}, side[i] > 0 ? "left wing" : "right wing"};
            for (int k = 1; k <= 4; ++k) c.targets.push_back(sided("mWing", side[i], std::to_string(k)));
            c.targets.push_back(sided("mWing4Fan", side[i]));
            map_chain(c, 75);
        } else if (r == Role::Ear && side[i]) {
            map_chain({follow(i), {sided("mFaceEar", side[i], "1"), sided("mFaceEar", side[i], "2")}, "ear"}, 70);
        } else if (r == Role::Eye && side[i]) {
            set(i, sided("mEye", side[i]), 80, "named as an eye");
        } else if (r == Role::Jaw) {
            set(i, "mFaceJaw", 80, "named as the jaw");
        }
    }
    // A spine bone beside the spine (a second chain through the body) named so, while mTorso is free.
    for (int i = 0; i < n; ++i)
        if (map.bones[i].target.empty() && bones[i].weight > 0 && !in_limb[i] && !on_spine.count(i) && !side[i] &&
            name[i].role == Role::Spine && !taken("mTorso"))
            set(i, "mTorso", 60, "named as the spine, beside the chain to the head");

    // Soft-body helpers (a butt, breast, belly or love-handle bone off the hips or chest) go on SL's collision volume
    // for that part rather than folding into the joint they hang from: SL's fitted mesh moves them with the wearer's
    // body shape and avatar physics, and they can be keyed on their own.
    for (int i = 0; i < n; ++i) {
        if (!map.bones[i].target.empty() || bones[i].weight <= 0 || in_limb[i] || on_spine.count(i)) continue;
        const std::vector<std::string> w = words(plain_name(bones[i].name));
        auto has = [&](std::initializer_list<const char*> keys) {
            for (const std::string& x : w)
                for (const char* k : keys)
                    if (x == k) return true;
            return false;
        };
        const int s = side[i];
        std::string volume;
        std::vector<std::string> under = {"mPelvis", "mTorso", "mChest"};  // the joints it may hang from
        if (has({"butt", "bum", "buttock", "buttocks", "glute", "glutes", "gluteus", "booty", "rump"}))
            volume = "BUTT", under = {"mPelvis", "mHindLimbsRoot", "mTorso"};
        else if (has({"breast", "breasts", "boob", "boobs", "pec", "pecs", "pectoral", "bust"}))
            volume = s > 0 ? "LEFT_PEC" : s < 0 ? "RIGHT_PEC" : "CHEST", under = {"mChest", "mTorso", "mNeck"};
        else if (has({"belly", "tummy", "stomach", "paunch"}))
            volume = "BELLY";
        else if (has({"handle", "handles", "lovehandle", "lovehandles"}) || (has({"love"}) && s))
            volume = s > 0 ? "LEFT_HANDLE" : s < 0 ? "RIGHT_HANDLE" : "";
        else if (has({"upperback"}) || (has({"upper"}) && has({"back"})))
            volume = "UPPER_BACK", under = {"mChest", "mTorso"};
        else if (has({"lowerback"}) || (has({"lower"}) && has({"back"})))
            volume = "LOWER_BACK", under = {"mTorso", "mPelvis"};
        if (volume.empty()) continue;
        int anchor = bones[i].parent;  // where it hangs: the volume's own part of the body, not a hand's "pec" or a tail's "butt"
        while (anchor >= 0 && map.bones[anchor].target.empty()) anchor = bones[anchor].parent;
        if (anchor < 0 || std::find(under.begin(), under.end(), map.bones[anchor].target) == under.end())
            continue;
        set(i, volume, 70, "soft body: SL's " + volume + " collision volume (moves with SL's avatar physics)");
    }

    // Bones sitting on a mapped joint join it, before the chains' tips place their next joints and after (a foot bone
    // at the end of a leg whose ankle its tip places).
    for (int i = 0; i < n; ++i)
        if (on_spine.count(i)) in_limb[i] = 1;
    sit_on_joints();
    place_tips();
    sit_on_joints();

    if (lying)
        map.notes.push_back("Size: it lies along its spine, so its size is the longer side of its footprint (nose to tail, or "
                            "wingtip to wingtip), made as long as SL's avatar is tall.");
    for (int i = 0; i < n; ++i) {
        RigMapBone& b = map.bones[i];
        if (!b.target.empty()) continue;
        b.confidence = bones[i].weight > 0 ? 40 : 90;  // folding a weighted bone is a guess; dropping a bare one is not
        b.reason = sub[i] <= 0 ? (name[i].role == Role::Tip ? "an end tip with no weights: dropped"
                                  : name[i].role == Role::Helper ? "a helper (IK target, pole or control) with no weights: dropped"
                                                                 : "no weights anywhere below it: dropped")
                 : bones[i].weight <= 0 ? "no weights of its own: not a joint"
                                        : "no SL joint fits it: its weights fold into its parent's joint";
    }
    // The suggestion checked: what does not hang together is a guess, and says why.
    const std::vector<std::string> problems = check_rig_map(skel, bones, map);
    for (int i = 0; i < n; ++i)
        if (!problems[size_t(i)].empty()) {
            RigMapBone& b = map.bones[i];
            b.confidence = std::min(b.confidence, 30);
            b.reason += "; but it is " + problems[size_t(i)] + ": check it";
        }
    return map;
}

}  // namespace

std::string mirror_bone_name(std::string_view name) { return parse_name(std::string(name)).mirror; }

RigMap suggest_rig_map(const Skeleton& skel, const std::vector<SourceBone>& bones, const std::vector<RigTable>& tables,
                       bool bento) {
    return Suggest(skel, bones, tables, bento).run();
}

std::vector<std::string> check_rig_map(const Skeleton& skel, const std::vector<SourceBone>& bones, const RigMap& map) {
    const int n = int(bones.size());
    std::vector<std::string> out(static_cast<size_t>(n));
    std::vector<int> joint(static_cast<size_t>(n), -1);
    std::map<int, std::vector<int>> on;  // SL joint -> the bones mapped to it
    Vec3 lo{1e300, 1e300, 1e300}, hi = lo * -1.0;
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], bones[i].bind.pos[k]), hi[k] = std::max(hi[k], bones[i].bind.pos[k]);
        const RigMapBone* m = map.find(bones[i].name);
        // A spare chain's bones hang where its SL chain does, and several share a joint by design: not judged.
        const int j = m && !m->target.empty() && m->spare.empty() ? skel.find(m->target) : -1;
        if (j >= 0 && j < skel.joint_count()) joint[size_t(i)] = j, on[j].push_back(i);  // joints only, not volumes
    }
    const double size = n ? std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-6}) : 1;
    auto below = [&](int i, int a) {
        for (int k = i; k >= 0; k = bones[size_t(k)].parent)
            if (k == a) return true;
        return false;
    };
    auto close_to = [&](int a, int b) { return (bones[size_t(a)].bind.pos - bones[size_t(b)].bind.pos).length() <= 0.05 * size; };
    for (int i = 0; i < n; ++i) {
        const int j = joint[size_t(i)];
        int from = bones[size_t(i)].parent;
        while (from >= 0 && joint[size_t(from)] < 0) from = bones[size_t(from)].parent;
        if (j < 0 || from < 0) continue;  // hanging from no mapped bone (a flattened export), it is in no wrong place
        // The nearest SL joint above with bones of its own: one of them carries this bone, or it hangs beside one, from
        // the same mapped bone (a creature's chest and torso both off its body).
        for (int u = skel[j].parent; u >= 0; u = skel[u].parent) {
            const auto it = on.find(u);
            if (it == on.end()) continue;
            const bool ok = std::any_of(it->second.begin(), it->second.end(), [&](int a) {
                int up = bones[size_t(a)].parent;  // the bone a hangs from: on a joint SL has above a's
                auto above = [&](int x) {
                    for (int v = skel[joint[size_t(a)]].parent; v >= 0; v = skel[v].parent)
                        if (v == joint[size_t(x)]) return true;
                    return false;
                };
                while (up >= 0 && (joint[size_t(up)] < 0 || !above(up))) up = bones[size_t(up)].parent;
                return below(i, a) || (!below(a, i) && (up < 0 || below(i, up)));
            });
            if (!ok) out[size_t(i)] = "not below " + bones[size_t(it->second.front())].name + " (" + skel[u].name + ")";
            break;
        }
    }
    for (int i = 0; i < n; ++i) {
        const int j = joint[size_t(i)];
        if (j < 0) continue;
        // A bone that sits with another on its joint goes with it (an IK foot at the ankle merges there).
        if (!out[size_t(i)].empty())
            for (int a : on[j])
                if (a != i && out[size_t(a)].empty() && close_to(a, i)) out[size_t(i)].clear();
        // Weighted bones on one joint sit together (which of two apart is wrong, only the user knows: both show).
        if (bones[size_t(i)].weight <= 0 || !out[size_t(i)].empty()) continue;
        for (int a : on[j])
            if (a != i && bones[size_t(a)].weight > 0 && !close_to(a, i)) {
                out[size_t(i)] = "shares " + skel[j].name + " with " + bones[size_t(a)].name + " but does not sit with it";
                break;
            }
    }
    return out;
}

double rig_map_size(const DaeModel& m, const RigMap& map) {
    const Vec3 d = m.bounds_max - m.bounds_min;
    return map.along_ground ? std::max(d.x, d.y) : d.z;
}

// ---------------------------------------------------------------------------------------------
// Spare chains

const std::vector<SpareSlot>& spare_slots() {
    static const std::vector<SpareSlot> slots = [] {
        std::vector<SpareSlot> out;
        auto chain = [](const std::string& stem, int n, const std::string& side = "") {
            std::vector<std::string> j;
            for (int k = 1; k <= n; ++k) j.push_back(stem + std::to_string(k) + side);
            return j;
        };
        // mWing4Fan is no link of the chain: it hangs beside mWing4, off mWing3.
        for (const char* s : {"Left", "Right"}) {
            const std::string side = s, low = side == "Left" ? "left" : "right";
            out.push_back({"Wing" + side, low + " wing", chain("mWing", 4, side), "Bento wings and their animations", false});
        }
        out.push_back({"Tail", "tail", chain("mTail", 6), "tails and tail animations", false});
        for (const char* s : {"Left", "Right"}) {
            const std::string side = s, low = side == "Left" ? "left" : "right";
            out.push_back({"Hind" + side, low + " hind limb", chain("mHindLimb", 4, side), "taur and quadruped bodies and their animations", false});
        }
        out.push_back({"Groin", "groin", {"mGroin"}, "groin attachments and their animations", false});
        out.push_back({"Spine12", "spine 1-2", {"mSpine1", "mSpine2"}, "", true});
        out.push_back({"Spine34", "spine 3-4", {"mSpine3", "mSpine4"}, "", true});
        out.push_back({"Tongue", "tongue", {"mFaceTongueBase", "mFaceTongueTip"}, "Bento heads and face animations", false});
        for (const char* s : {"Left", "Right"}) {
            const std::string side = s, low = side == "Left" ? "left" : "right";
            out.push_back({"Ear" + side, low + " ear", chain("mFaceEar", 2, side), "Bento heads and ear animations", false});
        }
        return out;
    }();
    return slots;
}

const SpareSlot* find_spare_slot(std::string_view id) {
    for (const SpareSlot& s : spare_slots())
        if (s.id == id) return &s;
    return nullptr;
}

std::vector<int> spare_chain_bones(const std::vector<SourceBone>& bones, const RigMap& map, const std::string& slot) {
    std::vector<int> out;
    for (int i = 0; i < int(bones.size()); ++i)
        if (const RigMapBone* m = map.find(bones[size_t(i)].name); m && m->spare == slot) out.push_back(i);
    return out;
}

bool spare_slot_free(const RigMap& map, const SpareSlot& slot) {
    for (const RigMapBone& b : map.bones)
        if (b.spare != slot.id && std::find(slot.joints.begin(), slot.joints.end(), b.target) != slot.joints.end()) return false;
    return true;
}

std::vector<int> spare_chain_from(const std::vector<SourceBone>& bones, int start) {
    const int n = int(bones.size());
    std::vector<double> sub(size_t(n), 0);
    for (int i = n - 1; i >= 0; --i) {
        sub[size_t(i)] += std::max(0.0, bones[size_t(i)].weight);
        if (const int p = bones[size_t(i)].parent; p >= 0 && p < i) sub[size_t(p)] += sub[size_t(i)];
    }
    std::vector<int> out;
    for (int cur = start; cur >= 0 && cur < n && sub[size_t(cur)] > 0;) {
        out.push_back(cur);
        int next = -1, count = 0;
        for (int k = cur + 1; k < n; ++k)
            if (bones[size_t(k)].parent == cur && sub[size_t(k)] > 0) next = k, ++count;
        cur = count == 1 ? next : -1;
    }
    return out;
}

std::string spare_label_for(const std::vector<SourceBone>& bones, const std::vector<int>& chain) {
    if (chain.empty()) return "";
    std::string name = bones[size_t(chain.front())].name;
    if (const size_t cut = name.find_last_of(":|"); cut != std::string::npos) name = name.substr(cut + 1);
    std::string out;
    for (const std::string& w : words(plain_name(name)))
        if (!std::isdigit(static_cast<unsigned char>(w[0])) && w != "l" && w != "r" && w != "left" && w != "right" && w != "bone" &&
            w != "jnt" && w != "joint" && w != "def")
            out += (out.empty() ? "" : " ") + w;
    return out;
}

namespace {

// Where a spare chain's joints go. The chain is a line through its bones' heads to the last one's tip; its bones are
// stretches of it. With no more bones than joints each bone's head places the next joint; with more, the joints are
// spaced evenly along the line and each bone goes on the joint its middle falls on.
struct SpareLayout {
    std::vector<Vec3> line;    // the heads, then the tip
    std::vector<double> at;    // per point: the length along the line
    std::vector<int> joint;    // per bone of the chain: the slot's joint it is on
    std::vector<Vec3> place;   // per joint used: where it goes
    bool spread = false;       // more bones than joints
};

SpareLayout spare_layout(const std::vector<SourceBone>& bones, const std::vector<int>& chain, size_t joints) {
    SpareLayout s;
    if (chain.empty() || !joints) return s;
    for (int b : chain) s.line.push_back(bones[size_t(b)].bind.pos);
    // The tip: the last bone's farthest child with no weights and nothing below it (an _end), else as far on as the
    // bone before it is long.
    const int last = chain.back();
    int tip = -1;
    for (int i = last + 1; i < int(bones.size()); ++i)
        if (bones[size_t(i)].parent == last && bones[size_t(i)].weight <= 0 &&
            std::none_of(bones.begin(), bones.end(), [&](const SourceBone& c) { return c.parent == i; }) &&
            (tip < 0 || (bones[size_t(i)].bind.pos - s.line.back()).length() > (bones[size_t(tip)].bind.pos - s.line.back()).length()))
            tip = i;
    const size_t n = chain.size();
    if (tip >= 0 && (bones[size_t(tip)].bind.pos - s.line.back()).length() > 1e-6) s.line.push_back(bones[size_t(tip)].bind.pos);
    else s.line.push_back(n >= 2 ? s.line[n - 1] * 2.0 - s.line[n - 2] : s.line[0]);
    s.at.push_back(0);
    for (size_t k = 1; k < s.line.size(); ++k) s.at.push_back(s.at.back() + (s.line[k] - s.line[k - 1]).length());
    s.spread = n > joints && s.at.back() > 1e-9;
    if (!s.spread) {
        for (size_t k = 0; k < n; ++k) s.joint.push_back(int(std::min(k, joints - 1)));
        for (size_t k = 0; k < std::min(n, joints); ++k) s.place.push_back(s.line[k]);
        return s;
    }
    const double step = s.at.back() / double(joints);
    auto point = [&](double d) {
        size_t j = 0;
        while (j + 2 < s.line.size() && s.at[j + 1] < d) ++j;
        const double len = s.at[j + 1] - s.at[j];
        return len > 0 ? s.line[j] + (s.line[j + 1] - s.line[j]) * ((d - s.at[j]) / len) : s.line[j];
    };
    for (size_t k = 0; k < joints; ++k) s.place.push_back(point(double(k) * step));
    for (size_t k = 0; k < n; ++k)
        s.joint.push_back(std::clamp(int((s.at[k] + s.at[k + 1]) / 2 / step), 0, int(joints) - 1));
    return s;
}

}  // namespace

void use_spare_chain(RigMap& map, const std::vector<SourceBone>& bones, const std::vector<int>& chain,
                     const std::string& slot, std::string label) {
    const SpareSlot* s = find_spare_slot(slot);
    if (!s || chain.empty()) return;
    clear_spare_chain(map, slot);
    for (RigMapBone& b : map.bones)  // a bone of the model's own on the slot's joints gives way
        if (std::find(s->joints.begin(), s->joints.end(), b.target) != s->joints.end())
            b.target.clear(), b.spare.clear(), b.confidence = 100, b.reason = "its joint went to the spare " + s->name;
    const SpareLayout lay = spare_layout(bones, chain, s->joints.size());
    if (label.empty()) label = spare_label_for(bones, chain);
    map.spare_labels[slot] = label.empty() ? s->name : label;
    for (size_t k = 0; k < chain.size(); ++k) {
        const std::string& name = bones[size_t(chain[k])].name;
        RigMapBone* b = map.find(name);
        if (!b) map.bones.push_back({name, "", 0, "", ""}), b = &map.bones.back();
        // The chain's own bones beyond the slot's joints (a chain no longer than the slot) all take one each.
        b->target = s->joints[size_t(lay.joint[k])];
        b->spare = slot;
        b->confidence = 100;
        b->reason = "on the spare " + s->name + " (" + map.spare_labels[slot] + "), bone " + std::to_string(k + 1) + " of " +
                    std::to_string(chain.size()) + (lay.spread ? ": its weights spread between " + std::to_string(s->joints.size()) + " joints" : "");
    }
}

void clear_spare_chain(RigMap& map, const std::string& slot) {
    for (RigMapBone& b : map.bones)
        if (b.spare == slot) b.spare.clear(), b.target.clear(), b.confidence = 100, b.reason = "you took it off the spare chain";
    map.spare_labels.erase(slot);
}

SpareHang spare_hang(const Skeleton& skel, const std::vector<SourceBone>& bones, const RigMapResult& r,
                     const std::vector<int>& chain, const SpareSlot& slot) {
    SpareHang h;
    if (chain.empty() || slot.joints.empty() || r.node.size() != bones.size()) return h;
    for (int a = bones[size_t(chain.front())].parent; a >= 0 && h.model.empty(); a = bones[size_t(a)].parent)
        if (const int n = r.node[size_t(a)]; n >= 0 && n < skel.joint_count()) h.model = skel[n].name;
    const int root = skel.find(slot.joints.front());
    for (int j = root >= 0 ? skel[root].parent : -1; j >= 0 && h.sl.empty(); j = skel[j].parent)
        if (std::find(r.node.begin(), r.node.end(), j) != r.node.end()) h.sl = skel[j].name;
    return h;
}

const std::vector<SparePreset>& builtin_spare_presets() {
    static const std::vector<SparePreset> presets = {
        {"Scarf on right wing", {{"WingRight", "scarf", {}}}},
        {"Ponytail on tail", {{"Tail", "ponytail", {}}}},
        {"Skirt on hind limbs", {{"HindLeft", "skirt", {}}, {"HindRight", "skirt", {}}}},
    };
    return presets;
}

SparePreset spare_preset_from(const RigMap& map, const std::vector<SourceBone>& bones, const std::string& name,
                              const std::string& slot) {
    SparePreset p{name, {}};
    for (const SpareSlot& s : spare_slots()) {
        if (!slot.empty() && s.id != slot) continue;
        const std::vector<int> chain = spare_chain_bones(bones, map, s.id);
        if (chain.empty()) continue;
        SparePreset::Chain c{s.id, map.spare_labels.count(s.id) ? map.spare_labels.at(s.id) : "", {}};
        for (int b : chain) c.bones.push_back(bones[size_t(b)].name);
        p.chains.push_back(std::move(c));
    }
    return p;
}

int apply_spare_preset(RigMap& map, const std::vector<SourceBone>& bones, const SparePreset& p, int start) {
    auto letters = [](const std::string& s) {
        std::string out;
        for (char c : s.substr(s.find_last_of(":|") == std::string::npos ? 0 : s.find_last_of(":|") + 1))
            if (std::isalpha(static_cast<unsigned char>(c))) out += char(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    auto by_name = [&](const std::string& want) {
        for (int i = 0; i < int(bones.size()); ++i)
            if (bones[size_t(i)].name == want) return i;
        const std::string l = letters(want);
        for (int i = 0; i < int(bones.size()); ++i)  // the first of a run of like-named bones: the chain's root
            if (letters(bones[size_t(i)].name) == l &&
                (bones[size_t(i)].parent < 0 || letters(bones[size_t(bones[size_t(i)].parent)].name) != l))
                return i;
        return -1;
    };
    const Quat turn = Quat::axis_angle({0, 0, 1}, map.turn * kPi / 2);
    int done = 0;
    for (size_t k = 0; k < p.chains.size(); ++k) {
        const SparePreset::Chain& c = p.chains[k];
        int first = c.bones.empty() ? -1 : by_name(c.bones.front());
        std::string slot = c.slot;
        if (first < 0 && start >= 0 && start < int(bones.size())) {
            first = k == 0 ? start : -1;
            if (k > 0)
                if (const std::string other = mirror_bone_name(bones[size_t(start)].name); !other.empty())
                    for (int i = 0; i < int(bones.size()); ++i)
                        if (bones[size_t(i)].name == other) first = i;
            // On the side of the body the bone is on: "Scarf on right wing" on a scarf at the left goes on the left.
            const double y = first >= 0 ? turn.rotate(bones[size_t(first)].bind.pos).y : 0;
            const bool left = slot.ends_with("Left"), right = slot.ends_with("Right");
            if (left && y < -1e-3) slot = slot.substr(0, slot.size() - 4) + "Right";
            else if (right && y > 1e-3) slot = slot.substr(0, slot.size() - 5) + "Left";
        }
        const SpareSlot* s = find_spare_slot(slot);
        if (first < 0 || !s || !spare_slot_free(map, *s)) continue;
        const std::vector<int> chain = spare_chain_from(bones, first);
        if (chain.empty()) continue;
        use_spare_chain(map, bones, chain, slot, c.label);
        ++done;
    }
    return done;
}

std::string write_spare_presets_json(const std::vector<SparePreset>& presets) {
    Json list = Json::array();
    for (const SparePreset& p : presets) {
        Json chains = Json::array();
        for (const SparePreset::Chain& c : p.chains) {
            Json o = Json::object(), names = Json::array();
            o.set("slot", c.slot);
            o.set("label", c.label);
            for (const std::string& b : c.bones) names.push(b);
            o.set("bones", std::move(names));
            chains.push(std::move(o));
        }
        Json o = Json::object();
        o.set("name", p.name);
        o.set("chains", std::move(chains));
        list.push(std::move(o));
    }
    Json j = Json::object();
    j.set("vats-spare-presets", 1);
    j.set("presets", std::move(list));
    return write_json(j);
}

bool parse_spare_presets_json(std::string_view text, std::vector<SparePreset>& out, std::string& err) {
    return guarded(err, [&] {
        out.clear();
        Json j;
        if (!parse_json(text, j, err)) return false;
        const Json* v = j.find("vats-spare-presets");
        const Json* list = j.find("presets");
        if (!v || !v->is_number() || !list || !list->is_array()) return err = "not VATs spare-chain presets", false;
        for (const Json& p : list->arr) {
            const Json *name = p.find("name"), *chains = p.find("chains");
            if (!name || !name->is_string() || !chains || !chains->is_array()) continue;
            SparePreset preset{name->str, {}};
            for (const Json& c : chains->arr) {
                const Json *slot = c.find("slot"), *label = c.find("label"), *bones = c.find("bones");
                if (!slot || !slot->is_string() || !find_spare_slot(slot->str)) continue;  // a slot this VATs does not know
                SparePreset::Chain chain{slot->str, label && label->is_string() ? label->str : "", {}};
                if (bones && bones->is_array())
                    for (const Json& b : bones->arr)
                        if (b.is_string()) chain.bones.push_back(b.str);
                preset.chains.push_back(std::move(chain));
            }
            if (!preset.chains.empty()) out.push_back(std::move(preset));
        }
        return true;
    });
}

void spread_spare_weights(const Skeleton& skel, const RigMapResult::Resample& c, DaeModel& m) {
    const size_t joints = c.nodes.size();
    if (joints < 2 || c.line.size() < 2 || c.at.size() != c.line.size() || c.at.back() <= 0) return;
    const double step = c.at.back() / double(joints);
    const int root = dae_root(skel);
    const size_t nv = m.positions.size() / 3;
    for (size_t v = 0; v < nv && v * 4 + 3 < m.joints.size() && v * 4 + 3 < m.weights.size(); ++v) {
        double w = 0, lo = 1e300, hi = -1e300;
        std::vector<std::pair<int, double>> keep;
        for (size_t k = 0; k < 4; ++k) {
            const int j = m.joints[v * 4 + k];
            const double x = m.weights[v * 4 + k];
            if (!(x > 0) || j == root) continue;
            if (const auto it = std::find(c.nodes.begin(), c.nodes.end(), j); it != c.nodes.end() && (c.only < 0 || j == c.only)) {
                const size_t q = size_t(it - c.nodes.begin());
                w += x, lo = std::min(lo, c.from[q]), hi = std::max(hi, c.to[q]);
            } else {
                keep.push_back({j, x});
            }
        }
        if (w <= 0) continue;
        // Its place along the chain: the nearest point within the stretch its joints' bones cover (a scarf curling back
        // past itself does not pull a vertex onto the wrong end).
        const Vec3 p{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
        double along = lo, best = 1e300;
        for (size_t j = 0; j + 1 < c.line.size(); ++j) {
            const double a = std::max(lo, c.at[j]), b = std::min(hi, c.at[j + 1]), len = c.at[j + 1] - c.at[j];
            if (a > b || len <= 0) continue;
            const Vec3 d = (c.line[j + 1] - c.line[j]) * (1 / len);
            const double t = std::clamp(c.at[j] + (p - c.line[j]).dot(d), a, b);
            if (const double dist = (c.line[j] + d * (t - c.at[j]) - p).length(); dist < best) best = dist, along = t;
        }
        // All on joint k at the middle of its bone, half and half where the next joint is.
        const double x = std::clamp(along / step - 0.5, 0.0, double(joints - 1));
        const size_t k = std::min(size_t(x), joints - 2);
        const double t = x - double(k);
        keep.push_back({c.nodes[k], w * (1 - t)});
        keep.push_back({c.nodes[k + 1], w * t});
        for (size_t a = 0; a < keep.size(); ++a)  // one entry per joint
            for (size_t b = keep.size() - 1; b > a; --b)
                if (keep[b].first == keep[a].first) keep[a].second += keep[b].second, keep.erase(keep.begin() + long(b));
        std::stable_sort(keep.begin(), keep.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        keep.resize(std::min<size_t>(keep.size(), 4));
        double total = 0;
        for (const auto& [j, x2] : keep) total += x2;
        for (size_t k2 = 0; k2 < 4; ++k2) {
            const bool used = k2 < keep.size() && keep[k2].second > 0 && total > 0;
            m.joints[v * 4 + k2] = used ? keep[k2].first : root;
            m.weights[v * 4 + k2] = used ? float(keep[k2].second / total) : 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Resolving

RigMapResult resolve_rig_map(const Skeleton& skel, const std::vector<SourceBone>& bones, const RigMap& map) {
    RigMapResult r;
    const int n = int(bones.size());
    r.node.assign(n, -1);
    r.folded_into.assign(n, -1);
    r.places.assign(n, false);
    std::vector<int> explicit_node(n, -1);
    // RM-8: a spare chain's bones take their joints from where they sit along it, and place those joints.
    const Quat turn = Quat::axis_angle({0, 0, 1}, map.turn * kPi / 2);
    std::map<int, Xform> spare_binds;
    std::set<std::string> slots;
    for (const RigMapBone& b : map.bones)
        if (!b.spare.empty()) slots.insert(b.spare);
    for (const std::string& id : slots) {
        const SpareSlot* slot = find_spare_slot(id);
        const std::vector<int> chain = spare_chain_bones(bones, map, id);
        if (!slot) {
            r.problems.push_back("no spare chain is called " + id);
            continue;
        }
        if (chain.empty()) continue;
        std::vector<int> nodes;
        for (const std::string& j : slot->joints) nodes.push_back(skel.find(j));
        if (std::find(nodes.begin(), nodes.end(), -1) != nodes.end()) continue;
        const SpareLayout lay = spare_layout(bones, chain, nodes.size());
        const auto label = map.spare_labels.find(id);
        for (size_t k = 0; k < chain.size(); ++k) explicit_node[size_t(chain[k])] = nodes[size_t(lay.joint[k])];
        for (size_t q = 0; q < lay.place.size(); ++q) {
            const size_t first = size_t(std::find(lay.joint.begin(), lay.joint.end(), int(q)) - lay.joint.begin());
            spare_binds[nodes[q]] = {bones[size_t(chain[std::min(first, chain.size() - 1)])].bind.rot, lay.place[q]};
            r.labels[slot->joints[q]] = label != map.spare_labels.end() && !label->second.empty() ? label->second : slot->name;
        }
        if (!lay.spread) continue;
        RigMapResult::Resample s;
        s.nodes = nodes;
        for (const Vec3& p : lay.line) s.line.push_back(turn.rotate(p));
        s.at = lay.at;
        // Each joint's own bones' stretch, half a joint's spacing wider either way.
        const double pad = lay.at.back() / double(nodes.size()) / 2;
        s.from.assign(nodes.size(), 1e300), s.to.assign(nodes.size(), -1e300);
        for (size_t k = 0; k < chain.size(); ++k) {
            const size_t q = size_t(lay.joint[k]);
            s.from[q] = std::min(s.from[q], lay.at[k] - pad), s.to[q] = std::max(s.to[q], lay.at[k + 1] + pad);
        }
        r.resample.push_back(std::move(s));
    }
    for (int i = 0; i < n; ++i) {
        const RigMapBone* m = map.find(bones[i].name);
        if (!m || m->target.empty() || !m->spare.empty()) continue;
        const int node = map_skin_joint(skel, m->target);
        if (node < 0 || node == dae_root(skel) || (node < skel.size() && skel[node].attachment && !skel[node].volume))
            r.problems.push_back(bones[i].name + ": " + m->target + " is not a joint SL skins to");
        else
            explicit_node[i] = node;
    }
    // A butt helper on one side (one cheek of two) carries the top of the thigh too: its weight goes to the BUTT volume
    // at the hip and to that side's L_UPPER_LEG or R_UPPER_LEG below it: all on BUTT at the bone, all on the leg half way
    // down to the knee, shared between. A leg lift then takes the lower cheek with the thigh instead of tearing it off.
    {
        auto bone_on = [&](const char* sl) {
            for (int i = 0; i < n; ++i)
                if (const RigMapBone* m = map.find(bones[i].name); m && m->target == sl && m->spare.empty()) return i;
            return -1;
        };
        const int butt = map_skin_joint(skel, "BUTT");
        const int hip[2] = {bone_on("mHipLeft"), bone_on("mHipRight")}, knee[2] = {bone_on("mKneeLeft"), bone_on("mKneeRight")};
        for (int i = 0; i < n && hip[0] >= 0 && hip[1] >= 0; ++i) {
            if (explicit_node[i] != butt || butt < 0) continue;
            const double dl = (bones[i].bind.pos - bones[hip[0]].bind.pos).length(),
                         dr = (bones[i].bind.pos - bones[hip[1]].bind.pos).length();
            if (std::fabs(dl - dr) < 0.2 * std::min(dl, dr)) continue;  // a butt bone in the middle stays on BUTT
            const int s = dl < dr ? 0 : 1;
            if (knee[s] < 0) continue;
            RigMapResult::Resample split;
            split.nodes = {butt, map_skin_joint(skel, s == 0 ? "L_UPPER_LEG" : "R_UPPER_LEG")};
            // Two joints spread along a line put theirs at its quarter and three quarters: here the bone and half way down.
            const Vec3 top = bones[i].bind.pos, down = bones[knee[s]].bind.pos - top;
            split.line = {turn.rotate(top - down * 0.25), turn.rotate(top + down * 0.75)};
            split.at = {0, (split.line[1] - split.line[0]).length()};
            split.from = {-1e300, -1e300}, split.to = {1e300, 1e300};
            if (split.nodes[1] < 0 || split.at[1] <= 1e-9) continue;
            explicit_node[i] = split.only = split.nodes[1];  // loaded on the leg's volume: each side's split finds its own
            r.resample.push_back(std::move(split));
        }
    }
    // The first mapped bone in the file's order, for weights with no mapped ancestor at all.
    int first = -1;
    for (int i = 0; i < n && first < 0; ++i)
        if (explicit_node[i] >= 0) first = i;
    for (int i = 0; i < n; ++i) {  // parents come first, so a parent's resolution is final
        if (explicit_node[i] >= 0) {
            r.node[i] = explicit_node[i];
            continue;
        }
        if (bones[i].weight <= 0) {
            r.dropped.push_back(bones[i].name);
            continue;
        }
        int a = bones[i].parent;
        while (a >= 0 && r.node[a] < 0) a = bones[a].parent;
        if (a < 0) a = first;
        if (a < 0) continue;  // nothing is mapped
        r.node[i] = explicit_node[a] >= 0 ? explicit_node[a] : r.node[a];  // the first mapped bone may come later
        r.folded_into[i] = a;
        const int v = r.node[i] - skel.size() - 1;  // a collision volume's index, when it is one
        r.folded.push_back(bones[i].name + " into " + bones[a].name + " (" +
                           (r.node[i] < skel.size() ? skel[r.node[i]].name : skel.volumes()[size_t(v)].name) + ")");
    }
    // Each SL joint is placed by the most weighted bone mapped to it (the first, on a tie): a weightless control bone
    // that shares a joint with the bone carrying the mesh never moves it.
    std::map<int, int> place;
    for (int i = 0; i < n; ++i)
        if (const int node = explicit_node[i]; node >= 0)
            if (auto it = place.find(node); it == place.end() || bones[i].weight > bones[it->second].weight) place[node] = i;
    // A limb split into "<part>_twist" and "<part>_stretch" halves (Auto-Rig Pro) may weight only the stretch half while
    // the weightless twist half starts at the real joint: the hip sits at thigh_twist's head, thigh_stretch starts
    // half way down the thigh. Whichever half starts farther from the next joint down the limb is where it bends. An
    // export that leaves the twist half out (arms without it) still has its control, "c_<part>_twist_offset", there.
    auto bone_named = [&](const std::string& name) {
        for (int k = 0; k < n; ++k)
            if (bones[k].name == name) return k;
        return -1;
    };
    for (auto& [node, i] : place) {
        const std::string& nm = bones[i].name;
        const size_t at = nm.find("_stretch");
        if (at == std::string::npos || node >= skel.size()) continue;
        int twist = bone_named(nm.substr(0, at) + "_twist" + nm.substr(at + 8));
        if (twist < 0) twist = bone_named("c_" + nm.substr(0, at) + "_twist_offset" + nm.substr(at + 8));
        if (twist < 0 || bones[twist].weight > 0) continue;
        const Node& joint = skel[node];
        int child = -1;  // the next placed joint down the limb
        for (int c : joint.children)
            if (place.count(c)) child = c;
        if (child < 0) continue;
        const Vec3 next = bones[place[child]].bind.pos;
        if ((bones[twist].bind.pos - next).length() > (bones[i].bind.pos - next).length() + 1e-4) {
            Xform bind = bones[i].bind;
            bind.pos = bones[twist].bind.pos;
            r.remap.binds[node] = bind;
            r.places[i] = true;
            place[node] = -1;  // handled
        }
    }
    for (auto& [node, i] : place) {
        if (i < 0) continue;
        r.places[i] = true;
        r.remap.binds[node] = bones[i].bind;
    }
    for (auto& [node, bind] : spare_binds) r.remap.binds[node] = bind;
    // A collision volume the weights go to (a soft-body helper's) keeps SL's place against its joint, wherever the model
    // puts that joint: avatar physics and fitted mesh expect it there, and one of two butt bones must not drag BUTT to
    // its side. (The bone's own place is not the volume's centre.)
    std::set<int> volumes;
    for (int i = 0; i < n; ++i)
        if (explicit_node[i] > dae_root(skel)) volumes.insert(explicit_node[i]);
    for (const RigMapResult::Resample& s : r.resample)
        for (int v : s.nodes)
            if (v > dae_root(skel)) volumes.insert(v);
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
    for (int node : volumes) {
        const CollisionVolume& v = skel.volumes()[size_t(node - dae_root(skel) - 1)];
        const auto joint = r.remap.binds.find(v.joint);
        if (joint == r.remap.binds.end()) {  // its joint stays at SL's rest, and so does it
            r.remap.binds.erase(node);
            continue;
        }
        const Xform own = rest[size_t(v.joint)] * Xform{v.rot, v.pos};
        r.remap.binds[node] = {(turn.conj() * own.rot).normalized(), joint->second.pos + turn.conj().rotate(own.pos - rest[size_t(v.joint)].pos)};
    }
    for (int i = 0; i < n; ++i)
        if (r.node[i] >= 0) r.remap.joints[bones[i].name] = r.node[i];
    r.remap.turn = ((map.turn % 4) + 4) % 4;
    return r;
}

// ---------------------------------------------------------------------------------------------
// The mapping file

void write_mesh_look(const MeshLook& look, Json& into) {
    if (!look.hidden.empty()) {
        Json hidden = Json::array();
        for (const std::string& p : look.hidden) hidden.push(p);
        into.set("hidden", std::move(hidden));
    }
    if (!look.keys.empty()) {
        Json keys = Json::object();
        for (const auto& [name, v] : look.keys) keys.set(name, v);
        into.set("shape_keys", std::move(keys));
    }
}

void read_mesh_look(const Json& from, MeshLook& out) {
    out = {};
    if (const Json* hidden = from.find("hidden"); hidden && hidden->is_array())
        for (const Json& p : hidden->arr)
            if (p.is_string()) out.hidden.insert(p.str);
    if (const Json* keys = from.find("shape_keys"); keys && keys->is_object())
        for (const auto& [name, v] : keys->obj)
            if (v.is_number() && std::isfinite(v.num)) out.keys[name] = std::clamp(v.num, -10.0, 10.0);  // Blender's slider range
}

std::string rig_map_path(const std::string& model_path) {
    const size_t slash = model_path.find_last_of("/\\");
    const size_t dot = model_path.rfind('.');
    const std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? model_path.substr(0, dot) : model_path;
    return stem + ".rigmap.json";
}

std::string write_rig_map_json(const RigMap& map) {
    Json j = Json::object();
    j.set("vats-rig-map", 1);
    j.set("height", map.height);
    j.set("turn", map.turn);
    if (map.along_ground) j.set("size", "along the ground");
    if (map.quadruped) j.set("layout", map.bento ? "bento" : "front legs on arms");
    Json b = Json::object();
    for (const RigMapBone& x : map.bones) b.set(x.source, x.target);
    j.set("bones", std::move(b));
    Json spares = Json::object();
    for (const RigMapBone& x : map.bones) {
        if (x.spare.empty()) continue;
        Json* s = spares.find(x.spare);
        if (!s) {
            const auto label = map.spare_labels.find(x.spare);
            s = &spares.set(x.spare, Json::object());
            s->set("label", label != map.spare_labels.end() ? label->second : "");
            s->set("bones", Json::array());
        }
        s->find("bones")->push(x.source);
    }
    if (!spares.obj.empty()) j.set("spares", std::move(spares));
    Json share = Json::object();
    for (const auto& [volume, s] : map.share) share.set(volume, s);
    if (!share.obj.empty()) j.set("share", std::move(share));
    write_mesh_look(map.look, j);
    // A rig from scratch, or weights painted on a body rigged some other way (a mapped rig, SL's own names): both live here.
    if (map.scratch.active() || map.scratch.weighted(int(map.scratch.wjoints.size() / 4))) {
        const ScratchRig& r = map.scratch;
        Json sc = Json::object();
        auto vec = [](const Vec3& v) {
            Json a = Json::array();
            for (int k = 0; k < 3; ++k) a.push(std::round(v[k] * 1e6) / 1e6);  // micrometres: no float noise in the file
            return a;
        };
        Json markers = Json::object();
        for (const auto& [id, mk] : r.markers) {
            Json a = vec(mk.pos);
            a.push(mk.confidence);
            markers.set(id, std::move(a));
        }
        sc.set("markers", std::move(markers));
        Json groups = Json::array();
        for (const std::string& g : r.groups) groups.push(g);
        sc.set("groups", std::move(groups));
        if (r.fitted) sc.set("fitted", true);
        if (r.painted) sc.set("painted", true);
        if (!r.transfer.empty()) {
            Json t = Json::object();
            for (const auto& [part, from] : r.transfer) t.set(part, from);
            sc.set("transfer", std::move(t));
        }
        Json joints = Json::object();
        for (const auto& [name, at] : r.joints) joints.set(name, vec(at));
        sc.set("joints", std::move(joints));
        if (!r.pins.empty()) {
            Json pins = Json::object();
            for (const auto& [name, at] : r.pins) pins.set(name, vec(at));
            sc.set("pins", std::move(pins));
        }
        const int nv = int(r.wjoints.size() / 4);
        if (r.weighted(nv)) {
            // By joint (its SK-40 index): vertex and weight pairs, the weights to 1/10000 (the uploader keeps 1/65535).
            sc.set("vertices", nv);
            std::map<int, Json> by;
            for (int v = 0; v < nv; ++v)
                for (int k = 0; k < 4; ++k) {
                    const float w = r.weights[size_t(v) * 4 + size_t(k)];
                    if (w <= 0) continue;
                    Json& list = by[r.wjoints[size_t(v) * 4 + size_t(k)]];
                    if (list.is_null()) list = Json::array();
                    list.push(v), list.push(std::round(double(w) * 1e4) / 1e4);
                }
            Json weights = Json::object();
            for (auto& [node, list] : by) weights.set(std::to_string(node), std::move(list));
            sc.set("weights", std::move(weights));
        }
        j.set("scratch", std::move(sc));
    }
    return write_json(j);
}

bool parse_rig_map_json(std::string_view text, RigMap& out, std::string& err) {
    return guarded(err, [&] {
        out = RigMap();
        Json j;
        if (!parse_json(text, j, err)) return false;
        const Json* v = j.find("vats-rig-map");
        const Json* b = j.find("bones");
        if (!v || !v->is_number() || !b || !b->is_object()) return err = "not a VATs rig mapping", false;
        if (v->num > 1) return err = "a rig mapping from a newer VATs", false;
        if (const Json* h = j.find("height"); h && h->is_number() && std::isfinite(h->num) && h->num >= 0 && h->num < 1e4)
            out.height = h->num;
        if (const Json* t = j.find("turn"); t && t->is_number() && std::isfinite(t->num)) out.turn = ((int(t->num) % 4) + 4) % 4;
        if (const Json* s = j.find("size"); s && s->is_string()) out.along_ground = s->str == "along the ground";
        if (const Json* l = j.find("layout"); l && l->is_string()) out.quadruped = true, out.bento = l->str == "bento";
        for (const auto& [source, target] : b->obj)
            if (target.is_string()) out.bones.push_back({source, target.str, 100, "from the mapping file", ""});
        if (const Json* spares = j.find("spares"); spares && spares->is_object())
            for (const auto& [slot, s] : spares->obj) {
                const Json* list = s.find("bones");
                if (!find_spare_slot(slot) || !list || !list->is_array()) continue;  // a chain this VATs does not know
                if (const Json* label = s.find("label"); label && label->is_string()) out.spare_labels[slot] = label->str;
                for (const Json& name : list->arr) {
                    if (!name.is_string()) continue;
                    RigMapBone* bone = out.find(name.str);
                    if (!bone) out.bones.push_back({name.str, "", 100, "from the mapping file", ""}), bone = &out.bones.back();
                    bone->spare = slot;
                }
            }
        if (const Json* share = j.find("share"); share && share->is_object())
            for (const auto& [volume, s] : share->obj)
                if (s.is_number() && std::isfinite(s.num) && soft_body_name(volume)) out.share[volume] = std::clamp(s.num, 0.0, 1.0);
        read_mesh_look(j, out.look);
        if (const Json* sc = j.find("scratch"); sc && sc->is_object()) {
            ScratchRig& r = out.scratch;
            r = {};
            auto vec = [](const Json& a, Vec3& dst) {
                if (!a.is_array() || a.arr.size() < 3) return false;
                for (int k = 0; k < 3; ++k)
                    if (!a.arr[size_t(k)].is_number() || !std::isfinite(a.arr[size_t(k)].num) || std::fabs(a.arr[size_t(k)].num) > 1e4) return false;
                dst = {a.arr[0].num, a.arr[1].num, a.arr[2].num};
                return true;
            };
            if (const Json* m = sc->find("markers"); m && m->is_object())
                for (const auto& [id, a] : m->obj) {
                    RigMarker mk;
                    if (!find_rig_marker(id) || !vec(a, mk.pos)) continue;
                    if (a.arr.size() > 3 && a.arr[3].is_number() && std::isfinite(a.arr[3].num)) mk.confidence = std::clamp(int(a.arr[3].num), 0, 100);
                    if (mk.confidence < 100) mk.why = "guessed";
                    r.markers[id] = mk;
                }
            if (const Json* g = sc->find("groups"); g && g->is_array()) {
                r.groups.clear();
                for (const Json& x : g->arr)
                    if (x.is_string()) r.groups.insert(x.str);
            }
            if (const Json* f = sc->find("fitted"); f && f->is_bool()) r.fitted = f->b;
            if (const Json* f = sc->find("painted"); f && f->is_bool()) r.painted = f->b;
            if (const Json* t = sc->find("transfer"); t && t->is_object())
                for (const auto& [part, from] : t->obj)
                    if (from.is_string()) r.transfer[part] = from.str;
            if (const Json* jn = sc->find("joints"); jn && jn->is_object())
                for (const auto& [name, a] : jn->obj) {
                    Vec3 pos;
                    if (vec(a, pos)) r.joints[name] = pos;
                }
            if (const Json* pn = sc->find("pins"); pn && pn->is_object())
                for (const auto& [name, a] : pn->obj) {
                    Vec3 pos;
                    if (vec(a, pos)) r.pins[name] = pos;
                }
            const Json* nv = sc->find("vertices");
            const Json* w = sc->find("weights");
            if (nv && nv->is_number() && nv->num > 0 && nv->num < 5e7 && w && w->is_object()) {
                const int n = int(nv->num);
                std::vector<std::vector<std::pair<int, double>>> per(static_cast<size_t>(n));
                for (const auto& [node, list] : w->obj) {
                    const int idx = std::atoi(node.c_str());
                    if (!list.is_array() || idx < 0) continue;
                    for (size_t i = 0; i + 1 < list.arr.size(); i += 2) {
                        const Json &vj = list.arr[i], &x = list.arr[i + 1];
                        if (vj.is_number() && x.is_number() && vj.num >= 0 && vj.num < n && std::isfinite(x.num) && x.num > 0)
                            per[size_t(vj.num)].push_back({idx, x.num});
                    }
                }
                r.wjoints.assign(size_t(n) * 4, -1);
                r.weights.assign(size_t(n) * 4, 0.f);
                for (int vtx = 0; vtx < n; ++vtx) {
                    auto& list = per[size_t(vtx)];
                    std::sort(list.begin(), list.end(), [](const auto& a, const auto& rhs) { return a.second > rhs.second; });
                    double sum = 0;
                    for (size_t k = 0; k < list.size() && k < 4; ++k) sum += list[k].second;
                    for (size_t k = 0; k < 4 && k < list.size() && sum > 0; ++k) {
                        r.wjoints[size_t(vtx) * 4 + k] = list[k].first;  // the rest stay -1: mRoot, set on load
                        r.weights[size_t(vtx) * 4 + k] = float(list[k].second / sum);
                    }
                }
            }
        }
        return true;
    });
}

bool read_rig_map_file(const std::string& path, RigMap& out, std::string& err) {
    err.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (parse_rig_map_json(ss.str(), out, err)) return true;
    err = path.substr(path.find_last_of("/\\") + 1) + ": " + err;
    return false;
}

bool load_rig_mapped(const std::string& path, const Skeleton& skel, const RigMap& map, DaeModel& out, DaeReport& report,
                     std::string& err, const std::vector<SourceBone>* bones) {
    DaeReport first;
    if (!bones) {
        DaeModel plain;
        if (!load_mesh_file_as_is(path, skel, plain, first, err)) return false;
        if (first.bones.empty()) {  // nothing to map: the file as it is
            out = std::move(plain);
            report = std::move(first);
            return true;
        }
        bones = &first.bones;
    }
    const RigMapResult r = resolve_rig_map(skel, *bones, map);
    if (!load_mesh_file_as_is(path, skel, out, report, err, &r.remap)) return false;
    for (const RigMapResult::Resample& c : r.resample) spread_spare_weights(skel, c, out);
    for (const auto& [volume, s] : map.share)  // RM-10
        if (const int v = skel.find_volume(volume); v >= 0) share_soft_body(skel, out, dae_volume(skel, v), s);
    out.labels = r.labels;
    // The size: the whole model scaled about its origin (the floor under it), joints and vertices alike. Its rest is
    // its own bind pose, so nothing the loader decided depends on its size.
    const double h = rig_map_size(out, map);
    if (const double k = map.height > 0 && h > 1e-9 ? map.height / h : 1; out.rigged && k != 1) {
        for (float& p : out.positions) p = float(p * k);
        for (DaeShapeKey& key : out.shape_keys)
            for (float& d : key.dpos) d = float(d * k);
        for (size_t j = 0; j < out.binds.size() && j < out.bound.size(); ++j)
            if (out.bound[j]) out.binds[j].pos = out.binds[j].pos * k;
        out.bounds_min = out.bounds_min * k, out.bounds_max = out.bounds_max * k;
        report.scale *= k;
    }
    // A body lying along its spine is often modelled about its middle (a fish, a bird in flight): its lowest point goes
    // on the floor, as an upright body's soles already are.
    if (const double lift = -out.bounds_min.z; map.along_ground && out.rigged && lift != 0) {
        for (size_t i = 2; i < out.positions.size(); i += 3) out.positions[i] = float(out.positions[i] + lift);
        for (size_t j = 0; j < out.binds.size() && j < out.bound.size(); ++j)
            if (out.bound[j]) out.binds[j].pos.z += lift;
        out.bounds_min.z += lift, out.bounds_max.z += lift;
    }
    for (const std::string& p : r.problems) report.warnings.push_back(p);
    if (!r.folded.empty()) {
        std::string list;
        for (const std::string& f : r.folded) list += (list.empty() ? "" : ", ") + f;
        report.warnings.push_back("weights folded into a mapped joint: " + list);
    }
    if (!out.rigged) return err = "the rig mapping maps none of the file's weighted bones", false;
    return true;
}

bool load_scratch_rigged(const std::string& path, const Skeleton& skel, const RigMap& map, DaeModel& out, DaeReport& report,
                         std::string& err) {
    if (!load_mesh_file_as_is(path, skel, out, report, err)) return false;
    place_model(out, scratch_placement(out, map.turn, map.height));
    ScratchRig rig = map.scratch;
    const int nv = out.vertex_count();
    if (!rig.wjoints.empty() && !settle_scratch_weights(skel, rig, nv)) {
        const std::string file = rig_map_path(path);
        report.warnings.push_back(file.substr(file.find_last_of("/\\") + 1) + ": its weights are for another version of the model (" +
                                  std::to_string(rig.wjoints.size() / 4) + " vertices, the model has " + std::to_string(nv) +
                                  "); weighted to the nearest bones until they are computed again");
        rig.wjoints.clear(), rig.weights.clear();
    }
    rig_from_scratch(skel, out, rig);
    report.rigged = true;
    report.scratch = true;
    report.skins_as_static = false;
    report.unmapped_joints.clear();
    report.look = map.look;
    return true;
}

bool settle_scratch_weights(const Skeleton& skel, ScratchRig& rig, int vertices) {
    if (!rig.weighted(vertices)) return false;
    const int root = dae_root(skel), count = dae_index_count(skel);
    for (size_t v = 0; v < size_t(vertices); ++v) {
        double sum = 0;
        for (size_t k = 0; k < 4; ++k) {
            int& j = rig.wjoints[v * 4 + k];
            float& w = rig.weights[v * 4 + k];
            if (j < 0 || j >= count || j == root) j = root, w = 0;  // empty, or a joint this VATs does not have
            sum += w;
        }
        for (size_t k = 0; k < 4 && sum > 0; ++k) rig.weights[v * 4 + k] = float(rig.weights[v * 4 + k] / sum);
    }
    return true;
}

bool rig_needs_mapping(const Skeleton& skel, const DaeReport& report) {
    if (report.remapped || report.scratch) return false;
    // SL-named as the viewer reads names: exactly, a joint, an alias or a volume. A game's Head, Chest or L_Hand is no
    // SL name (nor is an attachment point's: Chest, Pelvis, Neck), so such a rig goes to the mapping window.
    int weighted = 0, foreign = 0;
    for (const SourceBone& b : report.bones)
        if (b.weight > 0) {
            const int i = viewer_skin_joint(skel, b.name);
            ++weighted, foreign += i < 0 || (i >= skel.joint_count() && i < skel.size());
        }
    return weighted > 0 && foreign * 2 >= weighted;
}

}  // namespace vats
