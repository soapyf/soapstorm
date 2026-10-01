// Viewport Avatar Toolset - motion quality numbers (spec 08 MQ).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/motion_quality.h"

#include <algorithm>
#include <cmath>

#include "vats/anim_file.h"
#include "vats/footlock.h"
#include "vats/loop_tools.h"

namespace vats {

MotionQuality measure_quality(const Rig& rig, const Clip& clip, const AnimExportOptions& opt) {
    MotionQuality q;
    std::vector<std::string> tracks;
    for (const auto& [name, track] : clip.curves) {
        for (const auto& [ch, c] : track) q.keys += int(c.keys.size());
        if (clip.has_channels(name, kRotChannels) || clip.has_channels(name, kPosChannels)) tracks.push_back(name);
    }
    q.shake = shake_scores(clip, tracks, 0, clip.end_frame);
    int rotating = 0;
    for (const JointShake& s : q.shake)
        if (clip.has_channels(s.track, kRotChannels)) q.jerk += s.rot * s.rot, ++rotating;
    q.jerk = rotating ? std::sqrt(q.jerk / rotating) : 0;

    FootLockOptions fl;
    fl.shape = opt.shape;
    const std::vector<FootContact> contacts = find_foot_contacts(rig, clip, fl);
    q.contacts = int(contacts.size());
    for (const FootContact& c : contacts) {
        const int ankle = rig.limbs()[c.limb].end;
        Vec3 last = evaluate(rig, clip, c.from, opt.shape).globals[ankle].pos;
        for (int f = c.from + 1; f <= c.to; ++f) {
            const Vec3 p = evaluate(rig, clip, f, opt.shape).globals[ankle].pos;
            q.foot_slide += std::hypot(p.x - last.x, p.y - last.y);
            last = p;
        }
    }

    // Hip drift: a remove_travel dry run on a clip holding only the hips.
    if (auto hip = clip.curves.find("mPelvis"); hip != clip.curves.end()) {
        Clip h;
        h.fps = clip.fps, h.end_frame = clip.end_frame, h.loop = clip.loop, h.loop_in = clip.loop_in, h.loop_out = clip.loop_out;
        h.curves[hip->first] = hip->second;
        const LoopRange r = loop_range(clip);
        q.hip_drift = remove_travel(h).speed() * (r.out - r.in) / std::max(clip.fps, 1);
    }

    q.loops = clip.loop;
    if (clip.loop)
        for (const SeamJump& j : loop_seam_jumps(clip, 0, 0)) {
            if (j.channel.rfind("rot_", 0) == 0) q.seam_deg = std::max(q.seam_deg, std::fabs(j.jump));
            else if (j.channel.rfind("pos_", 0) == 0) q.seam_mm = std::max(q.seam_mm, std::fabs(j.jump) * 1000);
        }

    q.bytes = write_anim(export_anim(rig.skeleton(), clip, opt).file).size();
    return q;
}

}  // namespace vats
