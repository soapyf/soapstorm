// Viewport Avatar Toolset - Suggest Limits review workspace panel.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/suggest_limits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace vats {

namespace {

void grey_text(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool contains_nocase(std::string_view hay, std::string_view needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != hay.end();
}

}  // namespace

void App::open_suggest_limits() {
    show_suggest_limits_ = true;
    limit_preview_mode_ = LimitPreviewMode::Suggested;
    suggest_limits();
}

void App::suggest_limits() {
    std::vector<const DaeModel*> models;
    if (const MeshBody* b = mesh_body()) {
        for (const std::string& path : b->parts) {
            auto it = prop_models_.find(path);
            if (it != prop_models_.end() && it->second && it->second->rigged) {
                models.push_back(it->second.get());
            }
        }
    }
    const Clip* clip = &doc_.clip();
    suggested_limits_ = suggest_joint_limits(skel_, shape(), models, clip, suggest_options_);
    pending_limits_.start(current_body_id(), suggested_limits_);
    for (const auto& r : suggested_limits_) {
        if (!joint_limit_checked_.count(r.joint)) {
            joint_limit_checked_[r.joint] = (joint_limit_group(r.joint) != LimitGroup::Face);
        }
    }
}

bool App::joint_limit_is_checked(const std::string& joint) const {
    auto it = joint_limit_checked_.find(joint);
    if (it != joint_limit_checked_.end()) return it->second;
    LimitGroup g = joint_limit_group(joint);
    return group_checked_[static_cast<size_t>(g)];
}

void App::set_joint_limit_checked(const std::string& joint, bool checked) {
    joint_limit_checked_[joint] = checked;
}

void App::set_group_checked(LimitGroup group, bool checked) {
    group_checked_[static_cast<size_t>(group)] = checked;
    for (const auto& r : suggested_limits_) {
        if (joint_limit_group(r.joint) == group) {
            joint_limit_checked_[r.joint] = checked;
        }
    }
}

int App::count_pending_changes(LimitGroup group) const {
    return count_limit_changes(doc_.project.body_constraints(current_body_id()), pending_limits_.limits,
                               [&](const std::string& j) { return joint_limit_group(j) == group && joint_limit_is_checked(j); });
}

int App::count_all_pending_changes() const {
    return count_limit_changes(doc_.project.body_constraints(current_body_id()), pending_limits_.limits,
                               [&](const std::string& j) { return joint_limit_is_checked(j); });
}

int App::count_selected_pending_changes() const {
    return count_limit_changes(doc_.project.body_constraints(current_body_id()), pending_limits_.limits,
                               [&](const std::string& j) { return selected_limit_joints_.count(j) && joint_limit_is_checked(j); });
}

// Each Apply writes only the limits that change, as one undo step, and says how many it changed.
void App::apply_selected_limits() {
    if (!pending_limits_.for_body(current_body_id())) return;
    int count = 0;
    scene_edit("Apply Selected Joint Limits", [&](Project& p) {
        RigConstraints& rc = p.get_or_create_constraints(current_body_id());
        std::vector<std::string> selected(selected_limit_joints_.begin(), selected_limit_joints_.end());
        count = vats::apply_selected_limits(rc, pending_limits_.limits, selected,
            [&](const std::string& j) { return joint_limit_is_checked(j); });
    });
    status("Applied " + std::to_string(count) + " selected joint limits");
}

void App::apply_group_limits(LimitGroup group) {
    if (!pending_limits_.for_body(current_body_id())) return;
    int count = 0;
    scene_edit("Apply Joint Limits: " + std::string(limit_group_name(group)), [&](Project& p) {
        RigConstraints& rc = p.get_or_create_constraints(current_body_id());
        count = vats::apply_group_limits(rc, pending_limits_.limits, group,
            [&](const std::string& j) { return joint_limit_is_checked(j); });
    });
    status("Applied " + std::to_string(count) + " limits for " + std::string(limit_group_name(group)));
}

void App::apply_all_limits() {
    if (!pending_limits_.for_body(current_body_id())) return;
    int count = 0;
    scene_edit("Apply All Suggested Joint Limits", [&](Project& p) {
        RigConstraints& rc = p.get_or_create_constraints(current_body_id());
        count = vats::apply_all_limits(rc, pending_limits_.limits,
            [&](const std::string& j) { return joint_limit_is_checked(j); });
    });
    status("Applied " + std::to_string(count) + " suggested joint limits");
}

void App::discard_suggested_limits() {
    const int unapplied = count_all_pending_changes();
    pending_limits_.clear();
    show_suggest_limits_ = false;
    limit_preview_mode_ = LimitPreviewMode::Applied;
    status("Discarded " + std::to_string(unapplied) + " suggested joint limit changes");
}

bool App::editing_pending_limits() const {
    return edits_pending_limits(show_suggest_limits_, limit_preview_mode_, pending_limits_, current_body_id());
}

// Edits to the suggestions undo on their own (they are not in the project), in turn with the project's steps.
bool App::can_undo_pending_limits(bool redo) const {
    if (!show_suggest_limits_ || !pending_limits_.for_body(current_body_id())) return false;
    const size_t depth = doc_.history.undo_steps().size();
    return redo ? pending_limits_.can_redo(depth) : pending_limits_.can_undo(depth);
}

bool App::undo_pending_limits(bool redo) {
    if (!can_undo_pending_limits(redo)) return false;
    const size_t depth = doc_.history.undo_steps().size();
    redo ? pending_limits_.redo_edit(depth) : pending_limits_.undo_edit(depth);
    status(redo ? "Redid the suggested joint limit edit" : "Undid the suggested joint limit edit");
    return true;
}

void App::record_pending_limit_edit(RigConstraints before) {
    pending_limits_.record(std::move(before), doc_.history.undo_steps().size());
}

void App::select_other_side_limits() {
    std::unordered_set<std::string> new_sel;
    for (const auto& j : selected_limit_joints_) {
        int n = skel_.find(j);
        if (n >= 0) {
            int m = skel_.mirror(n);
            if (m >= 0 && m < skel_.size()) {
                new_sel.insert(skel_[m].name);
            }
        }
    }
    if (!new_sel.empty()) {
        selected_limit_joints_ = std::move(new_sel);
    }
}

void App::focus_on_joint_limit(const std::string& joint) {
    int node = skel_.find(joint);
    if (node < 0 || node >= int(globals_.size())) return;
    clear_selection();
    select(node, false);

    const RigConstraints* rc = edited_constraints();
    const JointLimit* lim = rc ? rc->find(joint) : nullptr;

    // Far enough to see the bone and its arc whole, whatever the body's size.
    const Vec3 p = globals_[node].pos;
    double bone_len = 0.25;
    for (int c : skel_[node].children)
        if (c < int(globals_.size())) bone_len = std::max(bone_len, (globals_[c].pos - p).length());
    const double distance = std::clamp(bone_len * 3.0, 0.5, 4.0);

    // A hinge is seen along its axis, so its arc faces the camera: from the outside of the body (the knee from beside
    // its own leg, not through the other one), or the camera's side for a joint on the midline. Others keep the view
    // direction.
    Vec3 from = -camera_.forward();
    if (lim && lim->kind == JointLimitKind::Hinge) {
        const int parent_idx = skel_[node].parent;
        const Quat parent_rot = (parent_idx >= 0 && parent_idx < int(globals_.size())) ? globals_[parent_idx].rot : Quat{};
        const Vec3 axis_frame = lim->axis.length() > 1e-6 ? lim->axis.normalized() : Vec3{0, 1, 0};
        const Vec3 axis = (parent_rot * skel_[node].rest).rotate(from_joint_frame(shape(), node, axis_frame)).normalized();
        const int pelvis = skel_.find("mPelvis");
        const Vec3 out = p - globals_[pelvis >= 0 ? pelvis : 0].pos;
        const double side = std::fabs(out.dot(axis)) > 0.03 ? out.dot(axis) : (camera_.eye() - p).dot(axis);
        from = side < 0 ? -axis : axis;
    }
    // Through the camera glide, so the view eases there.
    cam_glide_.look_from(camera_, from);
    cam_glide_.target_cam.target = p;
    cam_glide_.target_cam.distance = distance;
}

void App::start_joint_limit_test(const std::string& joint) {
    int node = skel_.find(joint);
    if (node < 0 || node >= int(globals_.size())) return;
    test_sweep_node_ = node;
    test_sweep_time_ = 0.0;
    test_sweep_duration_ = 1.6;
    test_sweep_orig_rot_ = (node < int(pose_.rot.size())) ? pose_.rot[node] : Quat{};
    status("Testing sweep range for " + joint);
}

void App::update_joint_limit_test_sweep(double dt) {
    if (test_sweep_node_ < 0 || test_sweep_node_ >= int(pose_.rot.size())) return;
    test_sweep_time_ += dt;
    if (test_sweep_time_ >= test_sweep_duration_) {
        pose_.rot[test_sweep_node_] = test_sweep_orig_rot_;
        globals_ = skel_.global_pose(pose_, shape());
        test_sweep_node_ = -1;
        return;
    }
    const std::string& joint = skel_[test_sweep_node_].name;
    const JointLimit* lim = pending_limits_.limits.find(joint);
    if (!lim) {
        const RigConstraints* applied = doc_.project.body_constraints(current_body_id());
        lim = applied ? applied->find(joint) : nullptr;
    }
    if (!lim || !lim->is_limited()) {
        test_sweep_node_ = -1;
        return;
    }
    double t = test_sweep_time_ / test_sweep_duration_;
    double s = std::sin(t * kPi);
    if (lim->kind == JointLimitKind::Hinge) {
        double ang = lim->min_angle + s * (lim->max_angle - lim->min_angle);
        Vec3 axis = lim->axis.length() > 1e-6 ? lim->axis.normalized() : Vec3{0, 1, 0};
        Quat frame_rot = Quat::axis_angle(axis, ang);
        pose_.rot[test_sweep_node_] = from_joint_frame(shape(), test_sweep_node_, frame_rot);
    } else if (lim->kind == JointLimitKind::Cone) {
        Vec3 bone_axis = lim->bone_axis.length() > 1e-6 ? lim->bone_axis.normalized() : Vec3{0, 1, 0};
        Vec3 perp = std::fabs(bone_axis.y) < 0.9 ? Vec3{0, 1, 0}.cross(bone_axis).normalized()
                                                 : Vec3{1, 0, 0}.cross(bone_axis).normalized();
        double swing_ang = s * lim->cone_angle;
        Quat swing = Quat::axis_angle(perp, swing_ang);
        double twist_ang = lim->twist_min + s * (lim->twist_max - lim->twist_min);
        Quat twist = Quat::axis_angle(bone_axis, twist_ang);
        pose_.rot[test_sweep_node_] = from_joint_frame(shape(), test_sweep_node_, (swing * twist).normalized());
    }
    globals_ = skel_.global_pose(pose_, shape());
}

void App::reset_joint_limit_to_suggestion(const std::string& joint) {
    auto it = pending_limits_.suggested.limits.find(joint);
    if (it != pending_limits_.suggested.limits.end()) {
        RigConstraints before = pending_limits_.limits;
        pending_limits_.limits.limits[joint] = it->second;
        record_pending_limit_edit(std::move(before));
        status("Reset " + joint + " to suggestion");
    }
}

static const char* limit_source_name(JointLimitSource s) {
    switch (s) {
        case JointLimitSource::Template: return "template";
        case JointLimitSource::BindPose: return "bind pose";
        case JointLimitSource::Collision: return "collision";
        case JointLimitSource::Animation: return "animation";
        default: return "manual";
    }
}

// Puts the next item on this line when it fits, else wraps it: the panel stays usable when docked narrow.
static void same_line_if_fits(const char* label, float extra = 0) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2 + extra;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right) ImGui::SameLine();
}

void App::draw_suggest_limits_panel() {
    if (!show_suggest_limits_) {
        draw_suggest_limits_was_open_ = false;
        return;
    }

    // Another body now (actor, mesh body or project): its own suggestions, never the last body's.
    if (!pending_limits_.for_body(current_body_id())) suggest_limits();

    // A tool window (it can be docked anywhere) against the right of the view, so Properties beside it stays usable
    // (Edit Limits, Mirror and the quick fixes are there), narrow enough to leave the avatar in view, and tall for the
    // list. ImGui takes this the first time only.
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);
        ImVec2 c0 = vp->WorkPos, c1{vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y};
        if (central && central->Size.x > 8) c0 = central->Pos, c1 = {central->Pos.x + central->Size.x, central->Pos.y + central->Size.y};
        const float fs = ImGui::GetFontSize(), m = ImGui::GetFrameHeight() * 0.5f;
        // ponytail: at a large interface size on a small screen it still covers most of the view; dock it then.
        const float w = std::min(std::clamp(0.6f * (c1.x - c0.x), 16 * fs, 24 * fs), c1.x - c0.x - 2 * m);
        const float h = std::min(56 * fs, vp->WorkPos.y + vp->WorkSize.y - c0.y - 2 * m);
        ImGui::SetNextWindowPos({c1.x - w - m, c0.y + m}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({w, h}, ImGuiCond_FirstUseEver);
    }
    if (!draw_suggest_limits_was_open_) ImGui::SetNextWindowFocus();  // to the front when it opens, even docked
    draw_suggest_limits_was_open_ = true;
    if (!ImGui::Begin(dock_title("Suggest Joint Limits", "Limits", "suggest-limits").c_str(), &show_suggest_limits_)) {
        ImGui::End();
        return;
    }
    tab_tooltip("Suggest Joint Limits");
    help_button("joint-limits");

    // 1. Preview: one line, so the list keeps its room in a narrow window.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preview");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    int preview = int(limit_preview_mode_);
    if (ImGui::Combo("##preview", &preview, "Off\0Suggested\0Applied\0")) limit_preview_mode_ = LimitPreviewMode(preview);
    ImGui::SetItemTooltip("Which limits posing uses while this panel is open: none, the suggestions, or the applied "
                          "ones. With Suggested, drag the handles in the view to tune a suggestion, then apply it.");
    if (!settings_.respect_joint_limits)
        grey_text("Respect Joint Limits is off, so posing ignores the preview. Turn Limits on in the toolbar to pose "
                  "with them.");

    // 2. Suggestion Options
    if (section_header("Calculation Sources", false)) {
        bool recompute = false;
        if (ImGui::Checkbox("Templates", &suggest_options_.use_templates)) recompute = true;
        same_line_if_fits("Bind pose", ImGui::GetFrameHeight());
        if (ImGui::Checkbox("Bind pose", &suggest_options_.use_bind_pose)) recompute = true;
        same_line_if_fits("Collision sweep", ImGui::GetFrameHeight());
        if (ImGui::Checkbox("Collision sweep", &suggest_options_.use_collision)) recompute = true;
        same_line_if_fits("Animation", ImGui::GetFrameHeight());
        if (ImGui::Checkbox("Animation", &suggest_options_.use_animation)) recompute = true;
        if (recompute) suggest_limits();
    }

    ImGui::Separator();

    // 3. Search and Filters Bar
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", limit_search_filter_.c_str());
    ImGui::SetNextItemWidth(140);
    if (filter_input("##limit_filter", "Search...", buf, sizeof(buf))) {
        limit_search_filter_ = buf;
    }
    ImGui::SameLine();

    auto filter_chip = [](const char* label, bool& state) {
        if (state) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label)) state = !state;
        if (state) ImGui::PopStyleColor();
    };

    filter_chip("Low Conf", filter_low_confidence_);
    same_line_if_fits("Changed");
    filter_chip("Changed", filter_changed_);
    same_line_if_fits("Not Applied");
    filter_chip("Not Applied", filter_not_applied_);
    same_line_if_fits("Other Side");
    if (ImGui::Button("Other Side")) {
        select_other_side_limits();
    }

    ImGui::Separator();

    // 4. Limb-grouped Tree List
    const RigConstraints* applied = doc_.project.body_constraints(current_body_id());
    const int prim = primary();
    const std::string prim_name = (prim >= 0 && prim < skel_.size()) ? skel_[prim].name : "";

    // Sync viewport selection: if a joint is selected in viewport and not in limit selection, select it
    if (!prim_name.empty() && !selected_limit_joints_.count(prim_name) && pending_limits_.limits.find(prim_name)) {
        if (prim_name != last_clicked_limit_joint_) {
            selected_limit_joints_.clear();
            selected_limit_joints_.insert(prim_name);
            last_clicked_limit_joint_ = prim_name;
        }
    }

    // The list takes what the actions below it leave (as tall as they were last frame), and never fewer than a few
    // rows: a short window scrolls instead.
    const float list_h = std::max(ImGui::GetContentRegionAvail().y - limit_actions_h_, 8 * ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginChild("##limits_tree_scroll", ImVec2(0, list_h), true);
    std::vector<std::string> shown;  // the rows in list order, for Shift-click ranges
    std::string shift_clicked;

    for (int gi = 0; gi < kLimitGroupCount; ++gi) {
        LimitGroup g = static_cast<LimitGroup>(gi);

        // Gather joints for this group
        std::vector<SuggestedLimitRow> group_rows;
        for (const auto& r : suggested_limits_) {
            if (joint_limit_group(r.joint) != g) continue;

            // Search filter
            if (!limit_search_filter_.empty() && !contains_nocase(r.joint, limit_search_filter_)) continue;

            const JointLimit* cur_pending = pending_limits_.limits.find(r.joint);
            const JointLimit* cur_applied = applied ? applied->find(r.joint) : nullptr;
            const JointLimit& lim = cur_pending ? *cur_pending : r.limit;

            // Filter chips
            if (filter_low_confidence_ && lim.confidence >= 0.85) continue;
            if (filter_changed_ && cur_applied && (*cur_applied == lim)) continue;
            if (filter_not_applied_ && cur_applied && cur_applied->is_limited()) continue;

            group_rows.push_back({r.joint, lim});
        }

        if (group_rows.empty()) continue;  // face bones are left free, so Face never has rows

        ImGui::PushID(gi);

        // Group checkbox
        bool grp_chk = group_checked_[static_cast<size_t>(g)];
        if (ImGui::Checkbox("##grp_chk", &grp_chk)) {
            set_group_checked(g, grp_chk);
        }
        ImGui::SameLine();

        // Expand if group contains primary bone
        bool group_has_primary = false;
        if (!prim_name.empty()) {
            for (const auto& r : group_rows) {
                if (r.joint == prim_name) { group_has_primary = true; break; }
            }
        }
        // A joint newly selected in the viewport: open its group and scroll to it (once, so it can be closed again).
        const bool reveal = group_has_primary && prim_name != limit_revealed_joint_;
        if (reveal) ImGui::SetNextItemOpen(true);

        std::string grp_label = std::string(limit_group_name(g)) + " (" + std::to_string(group_rows.size()) + ")";
        int grp_changes = count_pending_changes(g);
        if (grp_changes > 0) grp_label += " [" + std::to_string(grp_changes) + " chg]";

        bool group_open = ImGui::TreeNodeEx("##grp_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", grp_label.c_str());

        if (group_open) {
            for (size_t ri = 0; ri < group_rows.size(); ++ri) {
                const auto& r = group_rows[ri];
                ImGui::PushID(static_cast<int>(ri));

                // Joint checkbox
                bool j_chk = joint_limit_is_checked(r.joint);
                if (ImGui::Checkbox("##j_chk", &j_chk)) {
                    set_joint_limit_checked(r.joint, j_chk);
                }
                ImGui::SameLine();

                // Selectable row with Shift/Ctrl support
                const bool is_selected = selected_limit_joints_.count(r.joint) > 0;

                ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_AllowOverlap;

                char range[96];
                if (r.limit.kind == JointLimitKind::Hinge)
                    std::snprintf(range, sizeof(range), "Hinge %.0f\xc2\xb0 to %.0f\xc2\xb0", r.limit.min_angle / kDegToRad,
                                  r.limit.max_angle / kDegToRad);
                else
                    std::snprintf(range, sizeof(range), "Cone %.0f\xc2\xb0, twist %.0f\xc2\xb0 to %.0f\xc2\xb0",
                                  r.limit.cone_angle / kDegToRad, r.limit.twist_min / kDegToRad,
                                  r.limit.twist_max / kDegToRad);
                // The buttons sit at the right of the name when both fit, else on the line below it; the range goes
                // below.
                const ImGuiStyle& st = ImGui::GetStyle();
                const char* row_label = r.joint.c_str();
                const float buttons_w = 3 * (icon_button_width() + st.ItemSpacing.x);
                const float avail = ImGui::GetContentRegionAvail().x;
                const bool buttons_beside = ImGui::CalcTextSize(row_label).x + st.FramePadding.x * 2 + buttons_w <= avail;
                if (reveal && r.joint == prim_name) {
                    ImGui::SetScrollHereY(0.3f);
                    limit_revealed_joint_ = prim_name;
                }
                if (ImGui::Selectable(row_label, is_selected, sel_flags, ImVec2(buttons_beside ? avail - buttons_w : 0, 0))) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl) {
                        if (selected_limit_joints_.count(r.joint)) selected_limit_joints_.erase(r.joint);
                        else selected_limit_joints_.insert(r.joint);
                    } else if (io.KeyShift && !last_clicked_limit_joint_.empty()) {
                        shift_clicked = r.joint;  // the range is filled in once every row is listed
                    } else {
                        selected_limit_joints_.clear();
                        selected_limit_joints_.insert(r.joint);
                    }
                    if (shift_clicked.empty()) last_clicked_limit_joint_ = r.joint;  // Shift keeps the range's anchor
                    int node_idx = skel_.find(r.joint);
                    if (node_idx >= 0) {
                        clear_selection();
                        select(node_idx, false);
                    }
                }

                shown.push_back(r.joint);

                // Per-joint action buttons: Focus, Test, Reset
                const float indent = ImGui::GetFrameHeight() + st.ItemSpacing.x;  // under the name
                if (buttons_beside) ImGui::SameLine();
                else ImGui::Indent(indent);
                if (icon_small_button("focus", icon::kFrameSelected, "Focus: turn the view to see this joint's limit"))
                    focus_on_joint_limit(r.joint);
                ImGui::SameLine();
                if (icon_small_button("test", icon::kPlay, "Test: swing the joint through its range"))
                    start_joint_limit_test(r.joint);
                ImGui::SameLine();
                if (icon_small_button("reset", icon::kUnbake, "Reset: back to the suggestion (Ctrl+Z undoes it)"))
                    reset_joint_limit_to_suggestion(r.joint);
                char detail[160];
                std::snprintf(detail, sizeof(detail), "%s  %.0f%% %s", range, r.limit.confidence * 100,
                              limit_source_name(r.limit.source));
                if (buttons_beside) ImGui::Indent(indent);
                else same_line_if_fits(detail);  // beside the buttons, else wrapped on a line of its own
                grey_text(detail);
                ImGui::Unindent(indent);

                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
    if (!shift_clicked.empty()) {  // Shift-click: every row from the last plain click to this one
        auto a = std::find(shown.begin(), shown.end(), last_clicked_limit_joint_);
        auto b = std::find(shown.begin(), shown.end(), shift_clicked);
        if (a == shown.end()) a = b;
        if (a > b) std::swap(a, b);
        selected_limit_joints_.insert(a, b + 1);
    }

    // 5. Actions Bar
    const int total_changes = count_all_pending_changes();
    const int sel_changes = count_selected_pending_changes();

    LimitGroup active_group = LimitGroup::Other;
    if (!prim_name.empty()) active_group = joint_limit_group(prim_name);
    else if (!selected_limit_joints_.empty()) active_group = joint_limit_group(*selected_limit_joints_.begin());
    const int grp_changes = (active_group != LimitGroup::Other) ? count_pending_changes(active_group) : 0;

    const float actions_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped("%d limits will change (%zu selected)", total_changes, selected_limit_joints_.size());

    char sel_btn[64], grp_btn[64], all_btn[64];
    std::snprintf(sel_btn, sizeof(sel_btn), "Apply Selected (%d)", sel_changes);
    std::snprintf(grp_btn, sizeof(grp_btn), "Apply %s (%d)",
                  active_group != LimitGroup::Other ? std::string(limit_group_name(active_group)).c_str() : "Group", grp_changes);
    std::snprintf(all_btn, sizeof(all_btn), "Apply All (%d)", total_changes);

    ImGui::BeginDisabled(sel_changes == 0);
    if (ImGui::Button(sel_btn)) {
        apply_selected_limits();
    }
    ImGui::EndDisabled();

    same_line_if_fits(grp_btn);
    ImGui::BeginDisabled(grp_changes == 0);
    if (ImGui::Button(grp_btn)) {
        apply_group_limits(active_group);
    }
    ImGui::EndDisabled();

    same_line_if_fits(all_btn);
    ImGui::BeginDisabled(total_changes == 0);
    if (ImGui::Button(all_btn)) {
        apply_all_limits();
    }
    ImGui::EndDisabled();

    same_line_if_fits("Discard");
    if (ImGui::Button("Discard")) {
        discard_suggested_limits();
    }
    limit_actions_h_ = ImGui::GetCursorPosY() - actions_y;

    ImGui::End();
}

}  // namespace vats
