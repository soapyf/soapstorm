// Viewport Avatar Toolset - motion paths in the view: View > Motion Path.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 17 (MP). The frames and positions come from the core (motion_path.h); the path is drawn
// through the host's projector, so the viewer draws it over the world as the app draws it over its own view.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "imgui.h"
#include "widgets.h"

namespace vats {

// The tips the path follows: the selected bones and points, and the end bone of each selected IK target.
std::vector<int> App::motion_path_nodes() const {
    std::vector<int> nodes = selection_;
    for (const HandleRef& h : handles_)
        if (!h.pole)
            if (int end = rig_->limbs()[h.limb].end; std::find(nodes.begin(), nodes.end(), end) == nodes.end()) nodes.push_back(end);
    return nodes;
}

void App::draw_motion_path_menu() {
    MotionPathView& v = motion_path_;
    ImGui::Checkbox("Show Motion Path", &v.on);
    ImGui::SetItemTooltip("Where the selected bones' tips travel: cool before the current frame, warm after");
    ImGui::BeginDisabled(!v.on);
    ImGui::Checkbox("Whole Clip", &v.s.whole_clip);
    ImGui::BeginDisabled(v.s.whole_clip);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    slider_int("Before", &v.s.before, 1, 60, "%d frames");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    slider_int("After", &v.s.after, 1, 60, "%d frames");
    ImGui::EndDisabled();
    ImGui::Checkbox("Frame Numbers", &v.numbers);
    ImGui::SetItemTooltip("Number the keyed frames along the path");
    ImGui::EndDisabled();
}

// The paths for this frame, evaluated again only when the clip, the bones, the settings or (around the playhead) the
// frame changed. ponytail: a drag re-evaluates every frame of the path each UI frame, fine for a few hundred frames;
// a change of body shape alone is not noticed until one of those changes.
const std::vector<MotionPath>& App::motion_paths_now() {
    MotionPathView& v = motion_path_;
    const std::vector<int> nodes = motion_path_nodes();
    const double at = std::floor(frame_ + 1e-9);
    const bool same = v.cache_nodes == nodes && v.cache_s.whole_clip == v.s.whole_clip && v.cache_s.before == v.s.before &&
                      v.cache_s.after == v.s.after && (v.s.whole_clip || v.cache_frame == at) && v.cache_clip == doc_.clip();
    if (!same) {
        v.cache = motion_paths(*rig_, doc_.clip(), shape(), nodes, at, v.s);
        v.cache_nodes = nodes, v.cache_s = v.s, v.cache_frame = at, v.cache_clip = doc_.clip();
    }
    return v.cache;
}

// MP-3: a keyed dot on the end of a limb that is in IK at that frame drags the limb's IK target at that frame, in the
// plane facing the camera; one undo step on release, Escape puts it back. ponytail: pins are not dragged here; they
// keep their own markers in the graph.
bool App::motion_path_input(bool hovered) {
    MotionPathView& v = motion_path_;
    v.hover = false;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    if (v.drag_limb >= 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            doc_.clip() = doc_.history.cancel();
            v.drag_limb = -1;
            skip_shortcuts_ = true;
            return true;
        }
        Vec3 o, d;
        projector_.ray(camera_, m.x, m.y, o, d);
        const Vec3 n = camera_.forward();
        if (std::fabs(d.dot(n)) > 1e-9) {
            const double t = (v.drag_start - o).dot(n) / d.dot(n);
            Xform target = v.drag_target;
            target.pos = target.pos + (o + d * t - v.drag_start);
            doc_.clip() = v.drag_clip;  // from the press each time, so the keys never drift
            key_limb_target(doc_.clip(), *rig_, v.drag_frame, v.drag_limb, target, shape());
        }
        if (!ImGui::IsMouseDown(0)) {
            if (doc_.history.commit("Move Motion Path Key", doc_.clip())) mark_dirty();
            v.drag_limb = -1;
        }
        return true;
    }
    if (!v.on || !hovered || modal_ != Modal::None || dragging_gizmo_ || bone_drag_ >= 0 || doc_.history.is_open() ||
        v.cache_nodes != motion_path_nodes())  // the cache is last frame's paths; not ones no longer drawn
        return false;
    for (const MotionPath& p : v.cache) {
        int limb = -1;
        for (int l = 0; l < int(rig_->limbs().size()); ++l)
            if (rig_->limbs()[l].end == p.node) limb = l;
        if (limb < 0) continue;
        for (const PathPoint& pt : p.points) {
            double x = 0, y = 0;
            if (!pt.keyed || !projector_.to_screen(pt.pos, x, y) || std::hypot(x - m.x, y - m.y) > 6) continue;
            const Evaluation e = vats::evaluate(*rig_, doc_.clip(), pt.frame, shape());
            if (!e.limbs[limb].ik_on) continue;
            v.hover = true;
            ImGui::SetTooltip("Drag: move the %s IK target at frame %d", rig_->limbs()[limb].label.c_str(), int(pt.frame));
            if (ImGui::IsMouseClicked(0)) {
                v.drag_limb = limb, v.drag_frame = pt.frame, v.drag_start = pt.pos, v.drag_target = e.limbs[limb].target;
                v.drag_clip = doc_.clip();
                doc_.history.begin(doc_.clip());
            }
            return true;
        }
    }
    return false;
}

void App::draw_motion_paths(ImDrawList* dl) {
    if (!motion_path_.on || (selection_.empty() && handles_.empty())) return;
    const double at = std::floor(frame_ + 1e-9);
    auto colour = [&](double f, int a) {  // as the onion ghosts: cool before, warm after, white at the frame
        return f < at ? IM_COL32(102, 178, 255, a) : f > at ? IM_COL32(255, 158, 77, a) : IM_COL32(240, 240, 240, a);
    };
    for (const MotionPath& p : motion_paths_now()) {
        std::vector<ImVec2> s(p.points.size());
        std::vector<bool> ok(p.points.size());
        for (size_t i = 0; i < p.points.size(); ++i) {
            double x = 0, y = 0;
            ok[i] = projector_.to_screen(p.points[i].pos, x, y);
            s[i] = ImVec2(float(x), float(y));
        }
        for (size_t i = 1; i < s.size(); ++i)
            if (ok[i - 1] && ok[i]) {
                dl->AddLine(s[i - 1], s[i], IM_COL32(10, 12, 14, 140), 3.5f);
                dl->AddLine(s[i - 1], s[i], colour(p.points[i].frame - 0.5, 220), 1.6f);  // the segment's side
            }
        for (size_t i = 0; i < s.size(); ++i) {
            if (!ok[i]) continue;
            const PathPoint& pt = p.points[i];
            const float r = pt.frame == at ? 4.5f : pt.keyed ? 4.f : 2.f;
            dl->AddCircleFilled(s[i], r + 1.2f, IM_COL32(10, 12, 14, 200));
            dl->AddCircleFilled(s[i], r, colour(pt.frame, 255));
            if (motion_path_.numbers && pt.keyed) {
                char b[16];
                std::snprintf(b, sizeof b, "%d", int(pt.frame));
                dl->AddText(ImVec2(s[i].x + 6, s[i].y - ImGui::GetFontSize() - 2), IM_COL32(10, 12, 14, 200), b);
                dl->AddText(ImVec2(s[i].x + 5, s[i].y - ImGui::GetFontSize() - 3), colour(pt.frame, 255), b);
            }
        }
    }
}

}  // namespace vats
