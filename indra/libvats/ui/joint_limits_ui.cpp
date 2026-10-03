// Viewport Avatar Toolset - joint limits viewport visualization and properties editing.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/edit.h"
#include "vats/math.h"
#include "vats/rig.h"
#include "vats/rig_constraints.h"
#include "vats/suggest_limits.h"
#include "vats/skeleton.h"
#include "widgets.h"

namespace vats {

void App::track_limit_edit(const char* label) {
    if (ImGui::IsItemActivated()) {
        limit_drag_open_ = true;
        limit_drag_before_ = doc_.project.joint_limits;
    }
    if (ImGui::IsItemDeactivated() && limit_drag_open_) {
        limit_drag_open_ = false;
        if (limit_drag_before_ != doc_.project.joint_limits) {
            Project& p = doc_.project;
            Clip clip_before = p.clip;
            SceneState before{p.actors, p.active, p.clips, p.active_clip, limit_drag_before_, p.mesh_looks};
            SceneState after{p.actors, p.active, p.clips, p.active_clip, p.joint_limits, p.mesh_looks};
            doc_.history.record_scene(label, std::move(clip_before), std::move(before), p.clip, std::move(after));
            mark_dirty();
        }
    }
}

// A hinge's 0 direction in the joint's frame: the bone at rest (the mesh body's own bone when it has one), square
// to the axis. Angles are turns of the bone about the axis from there, as the clamp measures them.
static Vec3 hinge_zero(const Shape* sh, int node, const Skeleton& skel, const Vec3& axis_frame) {
    const Vec3 bone = to_joint_frame(sh, node, rest_bone_dir(skel, sh, node));
    Vec3 r = bone - axis_frame * bone.dot(axis_frame);
    if (r.length() < 1e-6) r = std::fabs(axis_frame.x) < 0.9 ? axis_frame.cross({1, 0, 0}) : axis_frame.cross({0, 1, 0});
    return r.normalized();
}

void App::set_joint_limit_from_pose(int node) {
    if (node < 0 || node >= skel_.size()) return;
    const std::string& name = skel_[node].name;
    const std::string body_id = current_body_id();
    // The set the limit tools are editing: the suggestions while Suggest Limits previews them (their own undo step),
    // else the applied limits (a project undo step). Never one while showing the other.
    const bool pending = editing_pending_limits();
    const RigConstraints* rc = pending ? &pending_limits_.limits : doc_.project.body_constraints(body_id);
    const JointLimit lim = widen_limit_to_pose(skel_, shape(), node, rc ? rc->find(name) : nullptr, pose_.rot[node]);
    if (pending) {
        RigConstraints before = pending_limits_.limits;
        set_joint_limit(pending_limits_.limits, skel_, shape(), node, lim, mirror_limits_);
        record_pending_limit_edit(std::move(before));
    } else {
        scene_edit("Set Joint Limit From Pose: " + name, [&](Project& p) {
            set_joint_limit(p.get_or_create_constraints(body_id), skel_, shape(), node, lim, mirror_limits_);
        });
    }
    status("Set " + name + "'s limit to cover its pose" + std::string(pending ? " (suggestion)" : ""));
}

void App::draw_joint_limits_section(int node) {
    if (node < 0 || node >= skel_.size()) return;
    const std::string& name = skel_[node].name;
    const std::string body_id = current_body_id();
    const RigConstraints* rc = doc_.project.body_constraints(current_body_id());  // the applied limits, shown even with Respect off
    const JointLimit* existing = rc ? rc->find(name) : nullptr;
    JointLimit limit = existing ? *existing : JointLimit{};

    // Writes this joint's limit, and the other side's too when Mirror is on (as the viewport's handles do).
    const int other = skel_.mirror(node);
    auto store = [&](const JointLimit& l) {
        set_joint_limit(doc_.project.get_or_create_constraints(body_id), skel_, shape(), node, l, mirror_limits_);
    };
    // The same as one undo step.
    auto edit_limit = [&](const std::string& label, const JointLimit& l) {
        scene_edit(label + ": " + name, [&](Project& p) {
            set_joint_limit(p.get_or_create_constraints(body_id), skel_, shape(), node, l, mirror_limits_);
        });
    };
    auto label = [&](const char* text) { labelled_row(text); };

    label("Kind");
    const char* kind_names[] = {"None (Unlimited)", "Hinge", "Cone (+ Twist)"};
    int current_kind = int(limit.kind);
    if (ImGui::BeginCombo("##limit_kind", kind_names[current_kind])) {
        for (int i = 0; i < 3; ++i) {
            bool is_selected = (current_kind == i);
            if (ImGui::Selectable(kind_names[i], is_selected)) {
                if (i != current_kind) {
                    JointLimit new_lim;  // None: unlimited
                    if (i != 0) {
                        new_lim = limit;
                        // A new limit starts from the joint's own suggestion (its anatomy template or how it's
                        // rigged), so a fresh hinge bends the right way instead of about a fixed Y axis.
                        const RigConstraints tmpl = template_limits(skel_, shape());
                        if (const JointLimit* t = tmpl.find(name); t && t->kind == JointLimitKind(i)) new_lim = *t;
                        new_lim.kind = JointLimitKind(i);
                        new_lim.source = JointLimitSource::Manual;
                        new_lim.confidence = 1.0;
                        if (new_lim.kind == JointLimitKind::Hinge && new_lim.axis.length() < 1e-6) {
                            new_lim.axis = {0, 1, 0};
                            new_lim.min_angle = 0.0;
                            new_lim.max_angle = 90.0 * kDegToRad;
                        } else if (new_lim.kind == JointLimitKind::Cone && new_lim.cone_angle > 3.14) {
                            new_lim.bone_axis = {0, 1, 0};
                            new_lim.cone_angle = 45.0 * kDegToRad;
                            new_lim.twist_min = -30.0 * kDegToRad;
                            new_lim.twist_max = 30.0 * kDegToRad;
                        }
                    }
                    edit_limit("Change Joint Limit Kind", new_lim);
                }
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const bool in_edit = edit_limits_mode_;
    if (in_edit) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
    if (ImGui::Button(in_edit ? "Done Editing (L)" : "Edit Limits (L)")) {
        toggle_edit_limits();
    }
    if (in_edit) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Direct manipulation handles in viewport (key: L)");
    ImGui::SameLine();
    ImGui::Checkbox("Mirror", &mirror_limits_);
    ImGui::SetItemTooltip("Apply the same edit to the other side (on by default for L/R pairs)");

    if (editing_pending_limits())
        ImGui::TextWrapped("These are the applied limits. While Suggest Limits previews its suggestions, the "
                           "viewport's handles edit the suggestion.");
    if (!settings_.respect_joint_limits) ImGui::TextWrapped("Respect Joint Limits is off: posing ignores these limits.");
    if (!limit.is_limited()) {
        ImGui::TextDisabled("This joint has no motion limits. Pick a kind above to add one.");
        return;
    }

    // Quick fixes for a limit that faces the wrong way, each one undo step (and mirrored when Mirror is on).
    const Vec3 bone_dir = to_joint_frame(shape(), node, rest_bone_dir(skel_, shape(), node));
    if (ImGui::Button("Aim at Pose")) {
        // The joint as posed now shows which way it should bend: the hinge turns to that bend, the cone centres on it.
        const Quat q = to_joint_frame(shape(), node, pose_.rot[node]);
        const double angle = q.angle();
        if (angle < 5 * kDegToRad) {
            status("Bend " + name + " the way it should go first (Limits off on the toolbar), then Aim at Pose");
        } else {
            JointLimit l = limit;
            const Vec3 v{q.x, q.y, q.z};
            if (l.kind == JointLimitKind::Hinge) {
                l.axis = (q.w < 0 ? -v : v).normalized();  // the pose is a positive bend about it
                l.min_angle = std::min(l.min_angle, 0.0);
                l.max_angle = std::max(l.max_angle, std::min(kPi, angle + 5 * kDegToRad));
            } else {
                l.bone_axis = q.rotate(l.bone_axis.length() > 1e-6 ? l.bone_axis : bone_dir).normalized();
            }
            l.source = JointLimitSource::Manual;
            edit_limit("Aim Joint Limit at Pose", l);
            status("Aimed " + name + "'s limit at its pose");
        }
    }
    ImGui::SetItemTooltip("Bend the joint the way it should move (turn Limits off first), then click: the hinge turns "
                          "to bend that way, or the cone centres on the pose.");
    if (limit.kind == JointLimitKind::Hinge) {
        ImGui::SameLine();
        if (ImGui::Button("Flip")) {
            JointLimit l = limit;
            l.axis = -l.axis;  // the same range, bending the other way
            l.source = JointLimitSource::Manual;
            edit_limit("Flip Joint Limit", l);
        }
        ImGui::SetItemTooltip("Makes the hinge bend the other way.");
        ImGui::SameLine();
        if (ImGui::Button("Turn 90\xc2\xb0")) {
            JointLimit l = limit;
            l.axis = Quat::axis_angle(bone_dir, kPi / 2).rotate(l.axis).normalized();  // about the bone itself
            l.source = JointLimitSource::Manual;
            edit_limit("Turn Joint Limit", l);
        }
        ImGui::SetItemTooltip("Turns the hinge a quarter turn around the bone: bending forward becomes bending sideways.");
    }

    // Set From Pose and Clear buttons
    if (ImGui::Button("Set From Pose")) {
        set_joint_limit_from_pose(node);
    }
    ImGui::SetItemTooltip("Widens the joint limit to cover the current pose.");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        edit_limit("Clear Joint Limit", JointLimit{});
        status("Cleared joint limit for " + name);
    }
    ImGui::SetItemTooltip("Removes joint limits from this bone (and the other side's with Mirror on).");
    if (other >= 0 && other != node) {
        ImGui::SameLine();
        if (ImGui::Button("Copy to Other Side")) {
            const std::string other_name = skel_[other].name;
            scene_edit("Copy Joint Limit to " + other_name, [&](Project& p) {
                p.get_or_create_constraints(body_id).limits[other_name] =
                    mirror_joint_limit(skel_, shape(), node, other, limit);
            });
            status("Copied " + name + "'s limit to " + other_name);
        }
        ImGui::SetItemTooltip("Gives %s the mirror image of this limit.", skel_[other].name.c_str());
    }

    if (limit.kind == JointLimitKind::Hinge) {
        label("Hinge axis");
        const char* axes[] = {"Y axis (Pitch)", "X axis (Roll)", "Z axis (Yaw)", "Custom"};
        int axis_choice = 3;
        if (std::fabs(limit.axis.y - 1.0) < 1e-4 && std::fabs(limit.axis.x) < 1e-4 && std::fabs(limit.axis.z) < 1e-4) axis_choice = 0;
        else if (std::fabs(limit.axis.x - 1.0) < 1e-4 && std::fabs(limit.axis.y) < 1e-4 && std::fabs(limit.axis.z) < 1e-4) axis_choice = 1;
        else if (std::fabs(limit.axis.z - 1.0) < 1e-4 && std::fabs(limit.axis.x) < 1e-4 && std::fabs(limit.axis.y) < 1e-4) axis_choice = 2;
        if (ImGui::BeginCombo("##hinge_axis", axes[axis_choice])) {
            for (int a = 0; a < 3; ++a) {
                if (ImGui::Selectable(axes[a], axis_choice == a)) {
                    JointLimit l = limit;
                    l.axis = (a == 0) ? Vec3{0, 1, 0} : (a == 1) ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
                    l.source = JointLimitSource::Manual;
                    edit_limit("Change Hinge Axis", l);
                }
            }
            ImGui::EndCombo();
        }
        if (axis_choice == 3) {
            float av[3] = {float(limit.axis.x), float(limit.axis.y), float(limit.axis.z)};
            label("Custom axis");
            if (ImGui::DragFloat3("##cust_axis", av, 0.01f, -1.0f, 1.0f, "%.2f")) {
                Vec3 ax{av[0], av[1], av[2]};
                if (ax.length() > 1e-6) ax = ax.normalized();
                limit.axis = ax;
                limit.source = JointLimitSource::Manual;
                store(limit);
                mark_dirty();
            }
            track_limit_edit("Change Hinge Axis");
        }

        float min_deg = float(limit.min_angle / kDegToRad);
        label("Min angle");
        if (ImGui::DragFloat("##min_angle", &min_deg, 0.5f, -180.0f, float(limit.max_angle / kDegToRad), "%.1f°")) {
            limit.min_angle = min_deg * kDegToRad;
            limit.source = JointLimitSource::Manual;
            store(limit);
            mark_dirty();
        }
        track_limit_edit("Change Min Angle");

        float max_deg = float(limit.max_angle / kDegToRad);
        label("Max angle");
        if (ImGui::DragFloat("##max_angle", &max_deg, 0.5f, float(limit.min_angle / kDegToRad), 180.0f, "%.1f°")) {
            limit.max_angle = max_deg * kDegToRad;
            limit.source = JointLimitSource::Manual;
            store(limit);
            mark_dirty();
        }
        track_limit_edit("Change Max Angle");
    } else if (limit.kind == JointLimitKind::Cone) {
        float cone_deg = float(limit.cone_angle / kDegToRad);
        label("Cone angle");
        if (ImGui::DragFloat("##cone_angle", &cone_deg, 0.5f, 0.0f, 180.0f, "%.1f°")) {
            limit.cone_angle = cone_deg * kDegToRad;
            limit.source = JointLimitSource::Manual;
            store(limit);
            mark_dirty();
        }
        track_limit_edit("Change Cone Angle");

        float tw_min = float(limit.twist_min / kDegToRad);
        label("Twist min");
        if (ImGui::DragFloat("##tw_min", &tw_min, 0.5f, -180.0f, float(limit.twist_max / kDegToRad), "%.1f°")) {
            limit.twist_min = tw_min * kDegToRad;
            limit.source = JointLimitSource::Manual;
            store(limit);
            mark_dirty();
        }
        track_limit_edit("Change Twist Min");

        float tw_max = float(limit.twist_max / kDegToRad);
        label("Twist max");
        if (ImGui::DragFloat("##tw_max", &tw_max, 0.5f, float(limit.twist_min / kDegToRad), 180.0f, "%.1f°")) {
            limit.twist_max = tw_max * kDegToRad;
            limit.source = JointLimitSource::Manual;
            store(limit);
            mark_dirty();
        }
        track_limit_edit("Change Twist Max");
    }

    // Current angle readout
    const Shape* sh = shape();
    const Quat frame_rot = to_joint_frame(sh, node, pose_.rot[node]);  // as shown; keys are in SL's frame

    if (limit.kind == JointLimitKind::Hinge) {
        Vec3 axis = limit.axis.length() > 1e-6 ? limit.axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, axis, swing, twist);
        Vec3 tv{twist.x, twist.y, twist.z};
        double cur_angle = 2.0 * std::atan2(tv.dot(axis), twist.w);
        while (cur_angle > kPi) cur_angle -= 2.0 * kPi;
        while (cur_angle < -kPi) cur_angle += 2.0 * kPi;
        bool out = cur_angle < limit.min_angle - 0.02 || cur_angle > limit.max_angle + 0.02;
        if (out) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s Current angle: %.1f° (outside limits)", icon::kWarning, cur_angle / kDegToRad);
        } else {
            ImGui::TextDisabled("Current angle: %.1f°", cur_angle / kDegToRad);
        }
    } else if (limit.kind == JointLimitKind::Cone) {
        Vec3 bone_axis = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, bone_axis, swing, twist);
        double cur_swing = swing.angle();
        Vec3 tv{twist.x, twist.y, twist.z};
        double cur_twist = 2.0 * std::atan2(tv.dot(bone_axis), twist.w);
        while (cur_twist > kPi) cur_twist -= 2.0 * kPi;
        while (cur_twist < -kPi) cur_twist += 2.0 * kPi;
        const bool out = !is_frame_rotation_within_limits(limit, frame_rot);  // per-direction stops included
        if (out) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s Swing: %.1f°, twist: %.1f° (outside)", icon::kWarning, cur_swing / kDegToRad, cur_twist / kDegToRad);
        } else {
            ImGui::TextDisabled("Swing: %.1f°, twist: %.1f°", cur_swing / kDegToRad, cur_twist / kDegToRad);
        }
    }

    const char* src_name = "Template";
    if (limit.source == JointLimitSource::BindPose) src_name = "Bind Pose";
    else if (limit.source == JointLimitSource::Collision) src_name = "Collision";
    else if (limit.source == JointLimitSource::Animation) src_name = "Animation";
    else if (limit.source == JointLimitSource::Manual) src_name = "Manual edit";
    ImGui::TextDisabled("Source: %s (%.0f%% confidence)", src_name, limit.confidence * 100.0);
}

void App::draw_joint_limit_viewport(ImDrawList* dl) const {
    int node = primary();
    if (node < 0 || node >= int(globals_.size()) || globals_.empty()) return;

    const RigConstraints* rc = edited_constraints();
    if (!rc) return;
    const JointLimit* lim = rc->find(skel_[node].name);
    if (!lim || !lim->is_limited()) return;

    Vec3 center = globals_[node].pos;
    double cx, cy;
    if (!projector_.to_screen(center, cx, cy)) return;
    ImVec2 center_screen{float(cx), float(cy)};

    int parent_idx = skel_[node].parent;
    Quat parent_rot = (parent_idx >= 0 && parent_idx < int(globals_.size())) ? globals_[parent_idx].rot : Quat{};
    Quat base_rot = parent_rot * skel_[node].rest;
    const Shape* sh = shape();

    double bone_len = 0.25;
    if (!skel_[node].children.empty()) {
        int child = skel_[node].children.front();
        if (child >= 0 && child < int(globals_.size())) {
            double d = (globals_[child].pos - center).length();
            if (d > 0.03 && d < 1.5) bone_len = d;
        }
    }
    const double R = std::clamp(bone_len * 0.45, 0.06, 0.25);

    Quat pose_rot = (base_rot.conj() * globals_[node].rot).normalized();
    Quat frame_rot = to_joint_frame(sh, node, pose_rot);

    const bool is_clamped = last_clamp_report_.contains(skel_[node].name) && (clamp_flash_time_ > 0 && ImGui::GetTime() - clamp_flash_time_ < 0.6);
    const ImU32 accent = is_clamped ? IM_COL32(255, 140, 20, 255) : accent_colour();
    const ImU32 col_fill = (accent & 0x00FFFFFF) | (40u << 24);
    const ImU32 col_line = (accent & 0x00FFFFFF) | (220u << 24);
    const ImU32 col_warning = IM_COL32(255, 100, 50, 255);
    const ImU32 col_axis = IM_COL32(200, 200, 210, 180);

    if (lim->kind == JointLimitKind::Hinge) {
        Vec3 axis_frame = lim->axis.length() > 1e-6 ? lim->axis.normalized() : Vec3{0, 1, 0};
        Vec3 axis_world = base_rot.rotate(from_joint_frame(sh, node, axis_frame)).normalized();

        const Vec3 ref_frame = hinge_zero(sh, node, skel_, axis_frame);  // the bone itself: the arc's 0 is the rest pose
        Vec3 ref_world = base_rot.rotate(from_joint_frame(sh, node, ref_frame)).normalized();
        Vec3 bi_world = axis_world.cross(ref_world).normalized();

        Quat swing, twist;
        decompose_swing_twist(frame_rot, axis_frame, swing, twist);
        Vec3 tv{twist.x, twist.y, twist.z};
        double cur_angle = 2.0 * std::atan2(tv.dot(axis_frame), twist.w);
        while (cur_angle > kPi) cur_angle -= 2.0 * kPi;
        while (cur_angle < -kPi) cur_angle += 2.0 * kPi;
        bool violated = cur_angle < lim->min_angle - 0.02 || cur_angle > lim->max_angle + 0.02 || swing.angle() > 0.08;

        double span = lim->max_angle - lim->min_angle;
        if (span > 1e-4) {
            int steps = std::clamp(int(std::round(span / (6.0 * kDegToRad))), 6, 32);
            std::vector<ImVec2> pts;
            bool all_ok = true;
            for (int k = 0; k <= steps; ++k) {
                double a = lim->min_angle + span * (double(k) / steps);
                Vec3 wpos = center + (ref_world * std::cos(a) + bi_world * std::sin(a)) * R;
                double sx, sy;
                if (projector_.to_screen(wpos, sx, sy)) {
                    pts.push_back(ImVec2(float(sx), float(sy)));
                } else {
                    all_ok = false;
                }
            }
            if (all_ok && pts.size() >= 2) {
                std::vector<ImVec2> fan;
                fan.push_back(center_screen);
                fan.insert(fan.end(), pts.begin(), pts.end());
                dl->AddConvexPolyFilled(fan.data(), int(fan.size()), col_fill);
                dl->AddPolyline(pts.data(), int(pts.size()), col_line, 0, 2.0f);
                dl->AddLine(center_screen, pts.front(), col_line, 1.5f);
                dl->AddLine(center_screen, pts.back(), col_line, 1.5f);
            }
        }

        // Small axis line
        Vec3 a0 = center - axis_world * (R * 0.4);
        Vec3 a1 = center + axis_world * (R * 0.4);
        double a0x, a0y, a1x, a1y;
        if (projector_.to_screen(a0, a0x, a0y) && projector_.to_screen(a1, a1x, a1y)) {
            dl->AddLine(ImVec2(float(a0x), float(a0y)), ImVec2(float(a1x), float(a1y)), col_axis, 2.0f);
        }

        // Needle for current angle
        Vec3 needle_w = center + (ref_world * std::cos(cur_angle) + bi_world * std::sin(cur_angle)) * (R * 1.15);
        double nx, ny;
        if (projector_.to_screen(needle_w, nx, ny)) {
            ImVec2 needle_screen{float(nx), float(ny)};
            ImU32 needle_col = violated ? col_warning : accent;
            dl->AddLine(center_screen, needle_screen, needle_col, 2.5f);
            dl->AddCircleFilled(needle_screen, 4.0f, needle_col);
        }

        if (edit_limits_mode_) {
            Vec3 p_min = center + (ref_world * std::cos(lim->min_angle) + bi_world * std::sin(lim->min_angle)) * R;
            Vec3 p_max = center + (ref_world * std::cos(lim->max_angle) + bi_world * std::sin(lim->max_angle)) * R;
            Vec3 p_axis = center + axis_world * (R * 0.4);

            auto draw_handle = [&](const Vec3& pt, LimitHandle kind) {
                double sx, sy;
                if (!projector_.to_screen(pt, sx, sy)) return;
                const bool hot = (hover_limit_handle_ == kind || drag_limit_handle_ == kind);
                const ImVec2 pos{float(sx), float(sy)};
                const float rad = hot ? 6.5f : 5.0f;
                const ImU32 fill = hot ? IM_COL32(255, 230, 51, 255) : accent;
                dl->AddCircleFilled(pos, rad, fill);
                dl->AddCircle(pos, rad, IM_COL32(10, 12, 14, 230), 0, hot ? 2.0f : 1.5f);
            };

            draw_handle(p_min, LimitHandle::HingeMin);
            draw_handle(p_max, LimitHandle::HingeMax);
            draw_handle(p_axis, LimitHandle::HingeAxis);
        }
    } else if (lim->kind == JointLimitKind::Cone) {
        Vec3 bone_axis_frame = lim->bone_axis.length() > 1e-6 ? lim->bone_axis.normalized() : Vec3{0, 1, 0};
        Vec3 bone_axis_world = base_rot.rotate(from_joint_frame(sh, node, bone_axis_frame)).normalized();

        Vec3 u_frame = std::fabs(bone_axis_frame.y) < 0.9 ? Vec3{0, 1, 0}.cross(bone_axis_frame).normalized() : Vec3{1, 0, 0}.cross(bone_axis_frame).normalized();
        Vec3 v_frame = bone_axis_frame.cross(u_frame).normalized();
        Vec3 u_world = base_rot.rotate(from_joint_frame(sh, node, u_frame)).normalized();
        Vec3 v_world = base_rot.rotate(from_joint_frame(sh, node, v_frame)).normalized();

        double L = R * 1.3;
        double alpha = std::clamp(lim->cone_angle, 0.02, kPi * 0.95);
        double d = L * std::cos(alpha);
        double r_base = L * std::sin(alpha);
        Vec3 base_center = center + bone_axis_world * d;

        std::vector<ImVec2> base_pts;
        bool base_ok = true;
        constexpr int kConeSteps = 24;
        for (int k = 0; k < kConeSteps; ++k) {
            double phi = (2.0 * kPi * k) / kConeSteps;
            Vec3 wpos = base_center + (u_world * std::cos(phi) + v_world * std::sin(phi)) * r_base;
            if (!lim->cone_stops.empty()) {  // the rim follows the per-direction stops
                const double a = std::clamp(cone_stop(*lim, u_frame * std::cos(phi) + v_frame * std::sin(phi)), 0.02, kPi * 0.95);
                wpos = center + (bone_axis_world * std::cos(a) + (u_world * std::cos(phi) + v_world * std::sin(phi)) * std::sin(a)) * L;
            }
            double sx, sy;
            if (projector_.to_screen(wpos, sx, sy)) {
                base_pts.push_back(ImVec2(float(sx), float(sy)));
            } else {
                base_ok = false;
            }
        }
        if (base_ok && base_pts.size() == kConeSteps) {
            if (lim->cone_stops.empty())
                dl->AddConvexPolyFilled(base_pts.data(), int(base_pts.size()), col_fill);
            else
                dl->AddConcavePolyFilled(base_pts.data(), int(base_pts.size()), col_fill);
            dl->AddPolyline(base_pts.data(), int(base_pts.size()), col_line, ImDrawFlags_Closed, 1.8f);
            for (int k = 0; k < kConeSteps; k += 4) {
                dl->AddLine(center_screen, base_pts[k], col_line, 1.2f);
            }
        }

        // Twist arc
        double d_twist = L * 0.4;
        double r_twist = L * 0.25;
        Vec3 twist_center = center + bone_axis_world * d_twist;
        double tw_span = lim->twist_max - lim->twist_min;
        if (tw_span > 1e-4) {
            int tw_steps = std::clamp(int(std::round(tw_span / (8.0 * kDegToRad))), 6, 24);
            std::vector<ImVec2> tw_pts;
            bool tw_ok = true;
            for (int k = 0; k <= tw_steps; ++k) {
                double psi = lim->twist_min + tw_span * (double(k) / tw_steps);
                Vec3 wpos = twist_center + (u_world * std::cos(psi) + v_world * std::sin(psi)) * r_twist;
                double sx, sy;
                if (projector_.to_screen(wpos, sx, sy)) {
                    tw_pts.push_back(ImVec2(float(sx), float(sy)));
                } else {
                    tw_ok = false;
                }
            }
            if (tw_ok && tw_pts.size() >= 2) {
                dl->AddPolyline(tw_pts.data(), int(tw_pts.size()), col_line, 0, 2.0f);
                double tcx, tcy;
                if (projector_.to_screen(twist_center, tcx, tcy)) {
                    ImVec2 tc_screen{float(tcx), float(tcy)};
                    dl->AddLine(tc_screen, tw_pts.front(), col_line, 1.2f);
                    dl->AddLine(tc_screen, tw_pts.back(), col_line, 1.2f);
                }
            }
        }

        // Current bone direction & twist
        Quat swing, twist;
        decompose_swing_twist(frame_rot, bone_axis_frame, swing, twist);
        Vec3 cur_bone_world = globals_[node].rot.rotate(from_joint_frame(sh, node, bone_axis_frame)).normalized();
        double cur_swing = swing.angle();
        Vec3 tv{twist.x, twist.y, twist.z};
        double cur_twist = 2.0 * std::atan2(tv.dot(bone_axis_frame), twist.w);
        while (cur_twist > kPi) cur_twist -= 2.0 * kPi;
        while (cur_twist < -kPi) cur_twist += 2.0 * kPi;

        // The stop in the direction the tip swings (a suggested cone can stop sooner one way than another).
        const Vec3 tip = swing.rotate(bone_axis_frame) - bone_axis_frame * swing.rotate(bone_axis_frame).dot(bone_axis_frame);
        bool swing_violated = cur_swing > (tip.length() > 1e-6 ? cone_stop(*lim, tip.normalized()) : lim->cone_angle) + 0.02;
        bool twist_violated = cur_twist < lim->twist_min - 0.02 || cur_twist > lim->twist_max + 0.02;

        Vec3 cur_bone_tip = center + cur_bone_world * (L * 1.15);
        double bx, by;
        if (projector_.to_screen(cur_bone_tip, bx, by)) {
            ImVec2 b_screen{float(bx), float(by)};
            ImU32 b_col = swing_violated ? col_warning : accent;
            dl->AddLine(center_screen, b_screen, b_col, 2.5f);
            dl->AddCircleFilled(b_screen, 4.0f, b_col);
        }

        Vec3 twist_needle = twist_center + (u_world * std::cos(cur_twist) + v_world * std::sin(cur_twist)) * (r_twist * 1.25);
        double tx, ty, tcx, tcy;
        if (projector_.to_screen(twist_needle, tx, ty) && projector_.to_screen(twist_center, tcx, tcy)) {
            ImVec2 t_screen{float(tx), float(ty)};
            ImVec2 tc_screen{float(tcx), float(tcy)};
            ImU32 t_col = twist_violated ? col_warning : accent;
            dl->AddLine(tc_screen, t_screen, t_col, 2.0f);
            dl->AddCircleFilled(t_screen, 3.5f, t_col);
        }

        if (edit_limits_mode_) {
            Vec3 p_rim = base_center + u_world * r_base;
            Vec3 p_edge = base_center + v_world * r_base;
            Vec3 p_tw_min = twist_center + (u_world * std::cos(lim->twist_min) + v_world * std::sin(lim->twist_min)) * r_twist;
            Vec3 p_tw_max = twist_center + (u_world * std::cos(lim->twist_max) + v_world * std::sin(lim->twist_max)) * r_twist;

            auto draw_handle = [&](const Vec3& pt, LimitHandle kind) {
                double sx, sy;
                if (!projector_.to_screen(pt, sx, sy)) return;
                const bool hot = (hover_limit_handle_ == kind || drag_limit_handle_ == kind);
                const ImVec2 pos{float(sx), float(sy)};
                const float rad = hot ? 6.5f : 5.0f;
                const ImU32 fill = hot ? IM_COL32(255, 230, 51, 255) : accent;
                dl->AddCircleFilled(pos, rad, fill);
                dl->AddCircle(pos, rad, IM_COL32(10, 12, 14, 230), 0, hot ? 2.0f : 1.5f);
            };

            draw_handle(p_rim, LimitHandle::ConeRim);
            draw_handle(p_edge, LimitHandle::ConeEdge);
            draw_handle(p_tw_min, LimitHandle::TwistMin);
            draw_handle(p_tw_max, LimitHandle::TwistMax);
        }
    }
}

void App::draw_joint_limit_badges(ImDrawList* dl) const {
    if (globals_.empty()) return;
    const std::string body_id = current_body_id();
    const RigConstraints* applied = doc_.project.body_constraints(body_id);
    const bool show_pending = show_suggest_limits_ && pending_limits_.for_body(body_id);
    const double now = ImGui::GetTime();
    const bool limits_enabled = settings_.respect_joint_limits;

    for (int i = 0; i < skel_.joint_count(); ++i) {
        if (!node_visible(i)) continue;
        const std::string& name = skel_[i].name;

        const JointLimit* app_lim = applied ? applied->find(name) : nullptr;
        const JointLimit* pend_lim = show_pending ? pending_limits_.limits.find(name) : nullptr;

        const bool has_applied = (app_lim && app_lim->is_limited());
        const bool has_pending_only = (!has_applied && pend_lim && pend_lim->is_limited());

        if (!has_applied && !has_pending_only) continue;

        double sx, sy;
        if (!projector_.to_screen(globals_[i].pos, sx, sy)) continue;
        const ImVec2 p{float(sx), float(sy)};

        const bool is_clamped = last_clamp_report_.contains(name) && (clamp_flash_time_ > 0 && now - clamp_flash_time_ < 0.6);

        if (has_applied) {
            // Applied limit: small ring around joint dot
            ImU32 col;
            if (is_clamped) {
                col = IM_COL32(255, 140, 20, 255); // Warning accent flash
            } else {
                col = limits_enabled ? IM_COL32(70, 210, 240, 220) : IM_COL32(140, 150, 160, 70);
            }
            dl->AddCircle(p, 6.5f, col, 0, 1.8f);
        } else if (has_pending_only) {
            // Pending-but-not-applied: hollow mark (thin circle)
            ImU32 col = limits_enabled ? IM_COL32(180, 225, 255, 180) : IM_COL32(140, 150, 160, 60);
            dl->AddCircle(p, 6.5f, col, 0, 1.0f);
        }
    }
}

bool App::has_limit_badge(int node) const {
    if (node < 0 || node >= skel_.size()) return false;
    const std::string& name = skel_[node].name;
    const RigConstraints* applied = doc_.project.body_constraints(current_body_id());
    const JointLimit* app_lim = applied ? applied->find(name) : nullptr;
    const JointLimit* pend_lim = show_suggest_limits_ ? pending_limits_.limits.find(name) : nullptr;
    return (app_lim && app_lim->is_limited()) || (pend_lim && pend_lim->is_limited());
}

void App::draw_bone_limit_badge(int node, ImDrawList* dl, ImVec2 pos) const {
    if (node < 0 || node >= skel_.size()) return;
    const std::string& name = skel_[node].name;
    const RigConstraints* applied = doc_.project.body_constraints(current_body_id());
    const JointLimit* app_lim = applied ? applied->find(name) : nullptr;
    const JointLimit* pend_lim = show_suggest_limits_ ? pending_limits_.limits.find(name) : nullptr;

    const bool has_applied = (app_lim && app_lim->is_limited());
    const bool has_pending_only = (!has_applied && pend_lim && pend_lim->is_limited());

    if (!has_applied && !has_pending_only) return;

    const bool limits_enabled = settings_.respect_joint_limits;
    const bool is_clamped = last_clamp_report_.contains(name) && (clamp_flash_time_ > 0 && ImGui::GetTime() - clamp_flash_time_ < 0.6);

    if (has_applied) {
        ImU32 col = is_clamped ? IM_COL32(255, 140, 20, 255)
                               : (limits_enabled ? IM_COL32(70, 210, 240, 220) : IM_COL32(140, 150, 160, 80));
        const float r = ImGui::GetFontSize() * 0.23f;
        dl->AddCircleFilled(pos, r, col);
        dl->AddCircle(pos, r, IM_COL32(10, 12, 14, 200), 0, 1.0f);
    } else if (has_pending_only) {
        ImU32 col = limits_enabled ? IM_COL32(180, 225, 255, 180) : IM_COL32(140, 150, 160, 60);
        dl->AddCircle(pos, ImGui::GetFontSize() * 0.23f, col, 0, 1.2f);
    }
}

void App::report_limit_clamp(const ClampedJoint& clamped) {
    last_clamp_report_.clamped.clear();
    last_clamp_report_.clamped.push_back(clamped);
    clamp_flash_time_ = ImGui::GetTime();
    status(clamped.message);
}

void App::report_limit_clamp(const ClampReport& report) {
    if (report.empty()) return;
    last_clamp_report_ = report;
    clamp_flash_time_ = ImGui::GetTime();
    status(report.clamped.front().message);
}

void App::toggle_edit_limits() {
    int node = primary();
    if (node < 0 || node >= skel_.size()) {
        status("Select a bone first to edit its limits");
        return;
    }
    edit_limits_mode_ = !edit_limits_mode_;
    if (edit_limits_mode_) {
        const std::string& name = skel_[node].name;
        const RigConstraints* rc = edited_constraints();
        const JointLimit* existing = rc ? rc->find(name) : nullptr;
        if (!existing || !existing->is_limited()) {
            set_joint_limit_from_pose(node);
        }
        status("Edit Limits on: drag handles in viewport (L to finish)");
    } else {
        status("Edit Limits off");
    }
}

App::LimitHandle App::pick_limit_handle(ImVec2 m) const {
    int node = primary();
    if (node < 0 || node >= int(globals_.size()) || globals_.empty()) return LimitHandle::None;
    const RigConstraints* rc = edited_constraints();
    if (!rc) return LimitHandle::None;
    const JointLimit* lim = rc->find(skel_[node].name);
    if (!lim || !lim->is_limited()) return LimitHandle::None;

    Vec3 center = globals_[node].pos;
    int parent_idx = skel_[node].parent;
    Quat parent_rot = (parent_idx >= 0 && parent_idx < int(globals_.size())) ? globals_[parent_idx].rot : Quat{};
    Quat base_rot = parent_rot * skel_[node].rest;
    const Shape* sh = shape();

    double bone_len = 0.25;
    if (!skel_[node].children.empty()) {
        int child = skel_[node].children.front();
        if (child >= 0 && child < int(globals_.size())) {
            double d = (globals_[child].pos - center).length();
            if (d > 0.03 && d < 1.5) bone_len = d;
        }
    }
    const double R = std::clamp(bone_len * 0.45, 0.06, 0.25);

    auto test_dist = [&](const Vec3& pt, double max_d = 12.0) -> std::pair<bool, double> {
        double sx, sy;
        if (!projector_.to_screen(pt, sx, sy)) return {false, 1e9};
        double d = std::hypot(sx - m.x, sy - m.y);
        return {d <= max_d, d};
    };

    LimitHandle best_handle = LimitHandle::None;
    double best_dist = 1e9;

    if (lim->kind == JointLimitKind::Hinge) {
        Vec3 axis_frame = lim->axis.length() > 1e-6 ? lim->axis.normalized() : Vec3{0, 1, 0};
        Vec3 axis_world = base_rot.rotate(from_joint_frame(sh, node, axis_frame)).normalized();

        const Vec3 ref_frame = hinge_zero(sh, node, skel_, axis_frame);  // the bone itself: the arc's 0 is the rest pose
        Vec3 ref_world = base_rot.rotate(from_joint_frame(sh, node, ref_frame)).normalized();
        Vec3 bi_world = axis_world.cross(ref_world).normalized();

        Vec3 p_min = center + (ref_world * std::cos(lim->min_angle) + bi_world * std::sin(lim->min_angle)) * R;
        Vec3 p_max = center + (ref_world * std::cos(lim->max_angle) + bi_world * std::sin(lim->max_angle)) * R;
        Vec3 p_axis = center + axis_world * (R * 0.4);

        if (auto [ok, d] = test_dist(p_min); ok && d < best_dist) { best_dist = d; best_handle = LimitHandle::HingeMin; }
        if (auto [ok, d] = test_dist(p_max); ok && d < best_dist) { best_dist = d; best_handle = LimitHandle::HingeMax; }
        if (auto [ok, d] = test_dist(p_axis); ok && d < best_dist) { best_dist = d; best_handle = LimitHandle::HingeAxis; }
    } else if (lim->kind == JointLimitKind::Cone) {
        Vec3 bone_axis_frame = lim->bone_axis.length() > 1e-6 ? lim->bone_axis.normalized() : Vec3{0, 1, 0};
        Vec3 bone_axis_world = base_rot.rotate(from_joint_frame(sh, node, bone_axis_frame)).normalized();

        Vec3 u_frame = std::fabs(bone_axis_frame.y) < 0.9 ? Vec3{0, 1, 0}.cross(bone_axis_frame).normalized() : Vec3{1, 0, 0}.cross(bone_axis_frame).normalized();
        Vec3 v_frame = bone_axis_frame.cross(u_frame).normalized();
        Vec3 u_world = base_rot.rotate(from_joint_frame(sh, node, u_frame)).normalized();
        Vec3 v_world = base_rot.rotate(from_joint_frame(sh, node, v_frame)).normalized();

        double L = R * 1.3;
        double alpha = std::clamp(lim->cone_angle, 0.02, kPi * 0.95);
        double d = L * std::cos(alpha);
        double r_base = L * std::sin(alpha);
        Vec3 base_center = center + bone_axis_world * d;

        Vec3 p_rim = base_center + u_world * r_base;
        Vec3 p_edge = base_center + v_world * r_base;

        double d_twist = L * 0.4;
        double r_twist = L * 0.25;
        Vec3 twist_center = center + bone_axis_world * d_twist;
        Vec3 p_tw_min = twist_center + (u_world * std::cos(lim->twist_min) + v_world * std::sin(lim->twist_min)) * r_twist;
        Vec3 p_tw_max = twist_center + (u_world * std::cos(lim->twist_max) + v_world * std::sin(lim->twist_max)) * r_twist;

        if (auto [ok, dst] = test_dist(p_rim); ok && dst < best_dist) { best_dist = dst; best_handle = LimitHandle::ConeRim; }
        if (auto [ok, dst] = test_dist(p_edge); ok && dst < best_dist) { best_dist = dst; best_handle = LimitHandle::ConeEdge; }
        if (auto [ok, dst] = test_dist(p_tw_min); ok && dst < best_dist) { best_dist = dst; best_handle = LimitHandle::TwistMin; }
        if (auto [ok, dst] = test_dist(p_tw_max); ok && dst < best_dist) { best_dist = dst; best_handle = LimitHandle::TwistMax; }
    }

    return best_handle;
}

void App::start_limit_handle_drag(LimitHandle handle, ImVec2 m) {
    drag_limit_handle_ = handle;
    limit_handle_press_ = m;
    limit_drag_snapshot_ = LimitDragSnapshot::capture(editing_pending_limits(), doc_.project.joint_limits,
                                                      pending_limits_.limits);
    limit_drag_start_ = {};
    if (const int node = primary(); node >= 0) {
        const RigConstraints* rc = edited_constraints();
        if (const JointLimit* lim = rc ? rc->find(skel_[node].name) : nullptr) limit_drag_start_ = *lim;
    }
}

void App::update_limit_handle_drag(ImVec2 m) {
    if (drag_limit_handle_ == LimitHandle::None) return;
    int node = primary();
    if (node < 0 || node >= int(globals_.size())) return;

    const std::string& name = skel_[node].name;
    // The set the press was on, never the other one.
    RigConstraints& rc = limit_drag_snapshot_.is_pending ? pending_limits_.limits
                                                         : doc_.project.get_or_create_constraints(current_body_id());
    auto it = rc.limits.find(name);
    if (it == rc.limits.end()) return;
    // Every handle works relative to the press: the limit as it was then, changed by how far the pointer has moved
    // since, so grabbing a handle never jumps it. (Taking the pointer's position on the view plane as the new value
    // snapped axes square to the camera and swung arcs wherever that plane cut them.)
    const JointLimit start = limit_drag_start_.is_limited() ? limit_drag_start_ : it->second;
    JointLimit lim = start;

    const Vec3 center = globals_[node].pos;
    const int parent_idx = skel_[node].parent;
    const Quat parent_rot = (parent_idx >= 0 && parent_idx < int(globals_.size())) ? globals_[parent_idx].rot : Quat{};
    const Quat base_rot = parent_rot * skel_[node].rest;
    const Shape* sh = shape();
    const Vec3 view = camera_.forward();

    // Where the pointer at mm meets the plane through the joint with this normal, relative to the joint. A plane
    // seen nearly edge-on gives wild hits, so then the view plane stands in for it.
    auto on_plane = [&](ImVec2 mm, Vec3 normal) {
        Vec3 o, d;
        projector_.ray(camera_, mm.x, mm.y, o, d);
        if (std::fabs(d.dot(normal)) < 0.2) normal = -view;
        const double denom = d.dot(normal);
        if (std::fabs(denom) < 1e-6) return Vec3{};
        return o + d * ((center - o).dot(normal) / denom) - center;
    };
    // The pointer's turn about `normal` since the press, measured in the plane with the basis a, b.
    auto turned = [&](Vec3 normal, Vec3 a, Vec3 b) {
        const Vec3 p0 = on_plane(limit_handle_press_, normal), p1 = on_plane(m, normal);
        return std::remainder(std::atan2(p1.dot(b), p1.dot(a)) - std::atan2(p0.dot(b), p0.dot(a)), 2 * kPi);
    };
    // The turn that takes the pointer's press direction to its direction now, on the view plane: swings an axis the
    // way the pointer moves, from where it was.
    auto swing = [&]() {
        const Vec3 p0 = on_plane(limit_handle_press_, -view), p1 = on_plane(m, -view);
        if (p0.length() < 1e-6 || p1.length() < 1e-6) return Quat{};
        const Vec3 a = p0.normalized(), b = p1.normalized(), c = a.cross(b);
        return c.length() < 1e-9 ? Quat{} : Quat::axis_angle(c.normalized(), std::atan2(c.length(), a.dot(b)));
    };
    auto world = [&](const Vec3& frame_vec) { return base_rot.rotate(from_joint_frame(sh, node, frame_vec)).normalized(); };
    auto frame = [&](const Vec3& world_vec) { return to_joint_frame(sh, node, base_rot.conj().rotate(world_vec)); };

    const bool snap = (settings_.preset == Preset::SecondLife ? snap_on_ : ImGui::GetIO().KeyCtrl);
    const double snap_step = snap ? snap_deg_ * kDegToRad : 0.0;

    if (lim.kind == JointLimitKind::Hinge) {
        const Vec3 axis_frame = start.axis.length() > 1e-6 ? start.axis.normalized() : Vec3{0, 1, 0};
        const Vec3 axis_world = world(axis_frame);
        const Vec3 ref_frame = hinge_zero(sh, node, skel_, axis_frame);  // the bone itself, as drawn
        const Vec3 ref_world = world(ref_frame), bi_world = axis_world.cross(ref_world).normalized();

        if (drag_limit_handle_ == LimitHandle::HingeMin)
            lim = set_hinge_min(start, start.min_angle + turned(axis_world, ref_world, bi_world), snap_step);
        else if (drag_limit_handle_ == LimitHandle::HingeMax)
            lim = set_hinge_max(start, start.max_angle + turned(axis_world, ref_world, bi_world), snap_step);
        else if (drag_limit_handle_ == LimitHandle::HingeAxis)
            lim = set_hinge_axis(start, frame(swing().rotate(axis_world)));
    } else if (lim.kind == JointLimitKind::Cone) {
        const Vec3 bone_frame = start.bone_axis.length() > 1e-6 ? start.bone_axis.normalized() : Vec3{0, 1, 0};
        const Vec3 bone_world = world(bone_frame);
        const Vec3 u_frame = std::fabs(bone_frame.y) < 0.9 ? Vec3{0, 1, 0}.cross(bone_frame).normalized()
                                                          : Vec3{1, 0, 0}.cross(bone_frame).normalized();
        const Vec3 u_world = world(u_frame), v_world = world(bone_frame.cross(u_frame).normalized());

        if (drag_limit_handle_ == LimitHandle::ConeRim) {
            // Widening is a turn away from the bone in the plane that holds the bone and faces the view most.
            Vec3 side = view.cross(bone_world);
            if (side.length() < 1e-6) side = u_world;
            side = side.normalized();
            // Measured from the side of the bone the press was on, so dragging outward always widens.
            const Vec3 p0 = on_plane(limit_handle_press_, bone_world.cross(side).normalized());
            const Vec3 out = p0.dot(side) < 0 ? -side : side;
            lim = set_cone_angle(start, start.cone_angle + turned(bone_world.cross(out).normalized(), bone_world, out),
                                 snap_step);
        } else if (drag_limit_handle_ == LimitHandle::ConeEdge) {
            lim = set_cone_edge(start, frame(swing().rotate(bone_world)));
        } else if (drag_limit_handle_ == LimitHandle::TwistMin) {
            lim = set_twist_min(start, start.twist_min + turned(bone_world, u_world, v_world), snap_step);
        } else if (drag_limit_handle_ == LimitHandle::TwistMax) {
            lim = set_twist_max(start, start.twist_max + turned(bone_world, u_world, v_world), snap_step);
        }
    }

    // The value being dragged, by the pointer.
    const double deg = 1 / kDegToRad;
    switch (drag_limit_handle_) {
        case LimitHandle::HingeMin: ImGui::SetTooltip("%s min %.0f\xc2\xb0", name.c_str(), lim.min_angle * deg); break;
        case LimitHandle::HingeMax: ImGui::SetTooltip("%s max %.0f\xc2\xb0", name.c_str(), lim.max_angle * deg); break;
        case LimitHandle::ConeRim: ImGui::SetTooltip("%s cone %.0f\xc2\xb0", name.c_str(), lim.cone_angle * deg); break;
        case LimitHandle::TwistMin: ImGui::SetTooltip("%s twist min %.0f\xc2\xb0", name.c_str(), lim.twist_min * deg); break;
        case LimitHandle::TwistMax: ImGui::SetTooltip("%s twist max %.0f\xc2\xb0", name.c_str(), lim.twist_max * deg); break;
        case LimitHandle::HingeAxis: ImGui::SetTooltip("%s hinge axis", name.c_str()); break;
        case LimitHandle::ConeEdge: ImGui::SetTooltip("%s cone direction", name.c_str()); break;
        default: break;
    }

    set_joint_limit(rc, skel_, sh, node, lim, mirror_limits_);
    // The project is marked changed when the drag ends with a change (finish), not while it may yet be cancelled.
}

void App::finish_limit_handle_drag() {
    if (drag_limit_handle_ == LimitHandle::None) return;
    if (limit_drag_snapshot_.is_pending) {  // its own undo step (Ctrl+Z)
        record_pending_limit_edit(limit_drag_snapshot_.pending_before);
        drag_limit_handle_ = LimitHandle::None;
        return;
    }
    int node = primary();
    std::string name = (node >= 0 && node < skel_.size()) ? skel_[node].name : "Joint";
    if (limit_drag_snapshot_.applied_before != doc_.project.joint_limits) {
        Project& p = doc_.project;
        SceneState before{p.actors, p.active, p.clips, p.active_clip, limit_drag_snapshot_.applied_before, p.mesh_looks};
        SceneState after{p.actors, p.active, p.clips, p.active_clip, p.joint_limits, p.mesh_looks};
        doc_.history.record_scene("Edit Joint Limit: " + name, p.clip, std::move(before), p.clip, std::move(after));
        mark_dirty();
    }
    drag_limit_handle_ = LimitHandle::None;
}

void App::cancel_limit_handle_drag() {
    if (drag_limit_handle_ == LimitHandle::None) return;
    // Back exactly as it was, so the project is no more changed than before the press.
    limit_drag_snapshot_.restore(doc_.project.joint_limits, pending_limits_.limits);
    drag_limit_handle_ = LimitHandle::None;
}

}  // namespace vats

