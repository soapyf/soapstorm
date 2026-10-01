// Viewport Avatar Toolset - the priority planner (spec 08 section 16).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/priority_plan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <tuple>

#include "vats/pose_ops.h"
#include "vats/project.h"

namespace vats {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < 6; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > 6) s += " and " + std::to_string(v.size() - 6) + " more";
    return s;
}

}  // namespace

PlanClip plan_clip_from_anim(const Skeleton& skel, const AnimFile& file, std::string name) {
    PlanClip c;
    c.name = std::move(name);
    for (const AnimJoint& j : file.joints) {
        if (j.rot.empty() && j.pos.empty() && j.rot_legacy.empty() && j.pos_legacy.empty()) continue;
        const int n = skel.find_viewer(j.name);
        if (n < 0) continue;
        c.joints[skel[n].name] = j.priority < 0 ? file.base_priority : j.priority;
    }
    return c;
}

PlanClip plan_clip_from_clip(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt, std::string name) {
    AnimExportOptions o = opt;  // the clip's own export settings win, as when it exports
    const Json& ex = clip.export_settings;
    if (const Json* v = ex.find("leave_static"); v && v->is_bool()) o.leave_out_static_rotations = v->b;
    if (const Json* r = ex.find("reduce"); r && r->is_array() && r->arr.size() == 2 && r->arr[0].is_number() &&
                                           r->arr[1].is_number())
        o.reduce_rot_deg = r->arr[0].num, o.reduce_pos_m = r->arr[1].num;
    const AnimExportResult r = export_anim(skel, clip.mirror_export ? mirrored_clip(skel, clip) : clip, o);
    return plan_clip_from_anim(skel, r.file, std::move(name));
}

bool load_plan_clip(const Skeleton& skel, const std::string& path, PlanClip& out, std::string& err) {
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    std::ifstream f(p, std::ios::binary);
    if (!f) return err = "could not be opened", false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return err = "could not be read", false;
    const std::u8string stem = p.stem().u8string(), ext = p.extension().u8string();
    std::string name(stem.begin(), stem.end());
    if (lower(std::string(ext.begin(), ext.end())) == ".anim") {
        AnimFile a;
        if (!parse_anim(bytes, a, err)) return false;
        out = plan_clip_from_anim(skel, a, name);
    } else {
        Project pr;
        if (!load_project(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), pr, err, path))
            return false;
        out = plan_clip_from_clip(skel, pr.clip, {}, name);
    }
    out.stand = lower(name).find("stand") != std::string::npos;
    return true;
}

int plan_winner(const std::vector<PlanClip>& clips, const std::string& joint) {
    int best = -1, best_priority = 0;
    for (int i = 0; i < int(clips.size()); ++i) {
        auto it = clips[i].joints.find(joint);
        // >=: a later start takes an equal priority (the viewer adds the newest motion's joint state first, and
        // LLJointStateBlender keeps the first of equals on top).
        if (it != clips[i].joints.end() && (best < 0 || it->second >= best_priority)) best = i, best_priority = it->second;
    }
    return best;
}

std::map<std::string, int> plan_winners(const std::vector<PlanClip>& clips) {
    std::map<std::string, int> out;
    for (const PlanClip& c : clips)
        for (auto& [joint, priority] : c.joints)
            if (!out.count(joint)) out[joint] = plan_winner(clips, joint);
    return out;
}

std::vector<PlanFinding> plan_lint(const Skeleton& skel, const std::vector<PlanClip>& clips) {
    std::vector<PlanFinding> out;
    const std::map<std::string, int> winners = plan_winners(clips);
    auto priority_at = [&](int clip, const std::string& joint) { return clips[size_t(clip)].joints.at(joint); };

    // PP-3: what the own clip loses, grouped by the clip that takes it.
    for (int own = 0; own < int(clips.size()); ++own) {
        if (!clips[size_t(own)].own) continue;
        std::map<std::tuple<int, int, int>, std::vector<std::string>> lost;  // (winner, its priority, ours) -> bones
        for (auto& [joint, priority] : clips[size_t(own)].joints)
            if (int w = winners.at(joint); w != own) lost[{w, priority_at(w, joint), priority}].push_back(joint);
        for (auto& [key, bones] : lost) {
            const auto [w, theirs, mine] = key;
            std::string why = theirs > mine ? " at a higher priority (" + std::to_string(theirs) + " over " + std::to_string(mine) + ")"
                                            : " at the same priority (" + std::to_string(theirs) + "), started later";
            out.push_back({"loses", w, bones, "Loses " + join(bones) + " to " + clips[size_t(w)].name + why});
        }
    }

    // PP-4, advice for AO makers.
    for (int i = 0; i < int(clips.size()); ++i) {
        const PlanClip& c = clips[size_t(i)];
        std::vector<std::string> high, face_hands;
        for (auto& [joint, priority] : c.joints) {
            const int n = skel.find(joint);
            const Category cat = n >= 0 ? skel[n].category : Category::Body;
            if (c.stand && cat == Category::Body && priority >= 4) high.push_back(joint);
            if ((cat == Category::Face || cat == Category::Hands) && priority >= 5) face_hands.push_back(joint);
        }
        if (!high.empty())
            out.push_back({"stand_priority", i, high,
                           c.name + " is a stand at priority 4 or more on " + join(high) +
                               ": it ties with or beats dances and poses at 4, and takes the body back each time the AO restarts it. "
                               "Stands belong at 3 or below."});
        const bool whole_body = c.joints.count("mPelvis") && (c.joints.count("mShoulderLeft") || c.joints.count("mShoulderRight"));
        if (whole_body && !face_hands.empty())
            out.push_back({"face_hands_high", i, face_hands,
                           c.name + " moves the whole body and keys " + join(face_hands) +
                               " at priority 5 or 6: every other animation's face and hand motion loses them. "
                               "Key the face and hands at the body's priority, or leave them out."});
    }
    return out;
}

}  // namespace vats
