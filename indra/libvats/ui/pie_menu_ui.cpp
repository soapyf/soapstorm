// Viewport Avatar Toolset - the Tab pie over the 3D view: hold, flick, release; or tap, then click.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// As Blender's pie menus and Maya's marking menus: the key held opens the ring at the pointer, a flick toward a
// slot and the release run it, so with practice the hand learns the directions and stops reading. A tap (a
// release before kTapSeconds without leaving the centre) leaves it open to click. The slots run the same actions
// as the toolbar and the menus, so undo and state stay one. The rings are data (pie.h).
#include <algorithm>
#include <cmath>
#include <string>

#include "app.h"
#include "icons.h"
#include "imgui_internal.h"
#include "theme.h"

namespace vats {
namespace {

constexpr double kTapSeconds = 0.25;   // a shorter press is a tap (Blender's Tap Key Timeout: 0.2 s)
constexpr double kFadeSeconds = 0.12;  // the fade in, unless Reduce motion

ImU32 with_alpha(ImU32 c, float a) {
    const float base = float((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    return (c & ~IM_COL32_A_MASK) | (ImU32(std::clamp(base * a, 0.f, 1.f) * 255 + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

ImVec2 unit(PieDir d) {
    const float a = float(int(d)) * 3.14159265f / 4;  // clockwise from up
    return ImVec2(std::sin(a), -std::cos(a));
}

}  // namespace

bool App::cli_pie(const std::string& spec) {
    const size_t slash = spec.find('/');
    const std::string ring = spec.substr(0, slash), dir = slash == std::string::npos ? "" : spec.substr(slash + 1);
    if (ring != "main" && ring != "more") return false;
    static const char* const dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int d = -1;
    for (int i = 0; i < kPieSlots; ++i)
        if (dir == dirs[i]) d = i;
    if (!dir.empty() && d < 0) return false;
    pie_ = Pie{};
    pie_.open = pie_.sticky = true;
    pie_.ring = ring == "more" ? PieRing::More : PieRing::Main;
    pie_.at_bone_frames = 3;  // centred on the selection once the view has drawn
    pie_.opened_at = -1;      // already faded in
    if (d >= 0) pie_.fake_offset = unit(PieDir(d));
    return true;
}

void App::open_pie() {
    if (pie_.open) return void(pie_.open = false);  // its key again closes it
    ImGuiKeyChord chord = 0;
    for (auto& [id, a] : actions_)
        if (id == "pie_menu") chord = a.key;
    const ImGuiKey key = ImGuiKey(chord & ~ImGuiMod_Mask_);
    const bool by_key = key != ImGuiKey_None && ImGui::IsKeyDown(key);
    if (by_key && !viewport_hovered_) return;  // elsewhere the key is ImGui's (Tab moves between fields)
    if (modal_ != Modal::None || doc_.history.is_open() || scene_busy()) return status("Finish the current edit first");
    pie_ = Pie{};
    pie_.open = true;
    pie_.held = by_key;
    pie_.sticky = !by_key;  // from Find a Tool: open until a click
    pie_.key = key;
    pie_.opened_at = ImGui::GetTime();
    // From the key, at the pointer; otherwise in the middle of the 3D view.
    const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);
    pie_.press = by_key ? ImGui::GetIO().MousePos
                 : central ? ImVec2(central->Pos.x + central->Size.x / 2, central->Pos.y + central->Size.y / 2)
                           : ImGui::GetMainViewport()->GetCenter();
    // Over a bone, the pie is for that bone: the slot that runs selects it first.
    if (by_key && hover_bone_ >= 0) pie_.bone = hover_bone_;
}

bool App::pie_slot_on(const PieSlot& s) const {
    const std::string id = s.id;
    if (id == "auto_ik") return settings_.auto_ik;
    if (id == "respect_joint_limits") return settings_.respect_joint_limits;
    if (id == "edit_limits") return edit_limits_mode_;
    if (id == "tool_select") return tool_ == Tool::Select;
    if (id == "tool_move") return tool_ == Tool::Move;
    if (id == "tool_rotate") return tool_ == Tool::Rotate;
    if (id == "tool_scale") return tool_ == Tool::Scale;
    return false;
}

const char* App::pie_slot_why(const PieSlot& s, bool enabled) const {
    if (!enabled) return "Select a bone first";
    if (pie_.bone >= 0) return nullptr;  // the bone under the pointer will be selected: its own checks run then
    const std::string id = s.id == std::string("relax") ? "tween" : s.id;
    for (auto& [aid, a] : actions_)
        if (aid == id && a.unavailable) return a.unavailable();
    return nullptr;
}

void App::run_pie_slot(const PieSlot& s) {
    const std::string id = s.id;
    if (id == "more" || id == "back") {  // the other ring, around the pointer, open to click
        pie_.ring = id == "more" ? PieRing::More : PieRing::Main;
        pie_.press = ImGui::GetIO().MousePos;
        pie_.held = false, pie_.sticky = true;
        pie_.fake_offset.reset();
        return;
    }
    pie_.open = false;
    if (pie_.bone >= 0 && std::find(selection_.begin(), selection_.end(), pie_.bone) == selection_.end()) select(pie_.bone, false);
    // The tools as the toolbar sets them (the Blender preset's W / E over the view would start a modal drag).
    if (id == "tool_select") tool_ = Tool::Select;
    else if (id == "tool_move") tool_ = Tool::Move;
    else if (id == "tool_rotate") tool_ = Tool::Rotate;
    else if (id == "tool_scale") tool_ = Tool::Scale;
    else if (id == "tween" || id == "relax") tween_relax_ = id == "relax", run_action("tween");  // the Relax box's mode
    else run_action(id.c_str());
}

void App::draw_pie_menu() {
    ImGuiIO& io = ImGui::GetIO();
    static const ImGuiID owner = ImHashStr("##vats_pie");
    // Over the 3D view the pie's key is the pie's: ImGui would move the keyboard focus between fields with Tab.
    // Owned from the frame before, so the press itself never reaches ImGui's focus keys.
    if (!pie_.open && viewport_hovered_ && !io.WantTextInput)
        for (auto& [id, a] : actions_)
            if (id == "pie_menu" && a.key && !(a.key & ImGuiMod_Mask_)) ImGui::SetKeyOwner(ImGuiKey(a.key), owner);
    if (!pie_.open) return;
    skip_shortcuts_ = true;  // while it is open, keys belong to it

    const float fs = ImGui::GetFontSize();
    const float dead = 0.9f * fs, ring_r = 1.1f * fs;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool has_selection = !selection_.empty() || !handles_.empty() || pie_.bone >= 0;
    const std::vector<PieItem> items = pie_items(pie_.ring, has_selection);

    // --pie: centred on the selected bone (or left in the middle of the view) once the view has drawn.
    if (pie_.at_bone_frames > 0 && --pie_.at_bone_frames == 0) {
        const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);
        pie_.press = central ? ImVec2(central->Pos.x + central->Size.x / 2, central->Pos.y + central->Size.y / 2) : vp->GetCenter();
        double x, y;
        if (const int p = primary(); p >= 0 && p < int(globals_.size()) && projector_.to_screen(globals_[p].pos, x, y))
            pie_.press = ImVec2(float(x), float(y));
    }

    // Each slot as wide as its label (at least a few letters' worth), the label over its key, one height for all.
    const float pad = 0.7f * fs, icon_w = 1.3f * fs, line = ImGui::GetTextLineHeight();
    const float item_h = 2 * line + 0.8f * fs;
    float item_w[kPieSlots] = {}, widest = 0;
    for (const PieItem& it : items)
        widest = std::max(widest, item_w[int(it.slot->dir)] =
                                      std::max(ImGui::CalcTextSize(it.slot->label).x + icon_w + 2 * pad, 5.f * fs));
    // The slots touch a circle from outside, each in its direction; wide enough that the top one clears the
    // diagonals beside it, so the ring reads as a ring.
    const float radius = std::max(5.4f * fs, 1.05f * widest);

    // Kept inside the window; the press point still counts for a flick, so a gesture made near an edge works.
    const float ext_x = radius + widest + 0.5f * fs, ext_y = radius + item_h + 0.5f * fs;
    pie_.centre = ImVec2(std::clamp(pie_.press.x, vp->WorkPos.x + ext_x, std::max(vp->WorkPos.x + ext_x, vp->WorkPos.x + vp->WorkSize.x - ext_x)),
                         std::clamp(pie_.press.y, vp->WorkPos.y + ext_y, std::max(vp->WorkPos.y + ext_y, vp->WorkPos.y + vp->WorkSize.y - ext_y)));
    const ImVec2 origin = pie_.sticky ? pie_.centre : pie_.press;
    const ImVec2 mouse = pie_.fake_offset ? ImVec2(origin.x + pie_.fake_offset->x * radius, origin.y + pie_.fake_offset->y * radius)
                                          : io.MousePos;
    const int dir = pie_dir_at(mouse.x - origin.x, mouse.y - origin.y, dead);
    const PieItem* hover = nullptr;
    for (const PieItem& it : items)
        if (int(it.slot->dir) == dir) hover = &it;
    const char* hover_why = hover ? pie_slot_why(*hover->slot, hover->enabled) : nullptr;

    // A full-window layer above everything, so the 3D view and panels behind take no hover or click meanwhile.
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::Begin("##pie", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float alpha = settings_.reduce_motion || pie_.opened_at < 0
                            ? 1.f
                            : float(std::clamp((ImGui::GetTime() - pie_.opened_at) / kFadeSeconds, 0.0, 1.0));
    const ImU32 text = with_alpha(ImGui::GetColorU32(ImGuiCol_Text), alpha);
    const ImU32 dim = with_alpha(ImGui::GetColorU32(ImGuiCol_TextDisabled), alpha);
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_PopupBg);
    const ImU32 border = with_alpha(ImGui::GetColorU32(ImGuiCol_Border), alpha);
    const ImU32 accent = accent_colour();
    const ImU32 shadow = with_alpha(IM_COL32(0, 0, 0, 60), alpha);
    const ImVec2 c = pie_.centre;

    // A soft disc behind the ring quietens the scene under it, so the slots read as one thing.
    for (int i = 0; i < 6; ++i)
        dl->AddCircleFilled(c, radius + item_h * (0.2f + 0.25f * float(i)), with_alpha(bg, 0.1f * alpha), 64);
    // The centre: a ring that marks the dead zone, and the arc of the direction the pointer points.
    dl->AddCircleFilled(c, ring_r, with_alpha(bg, 0.92f * alpha), 40);
    dl->AddCircle(c, ring_r, border, 40, 1.5f);
    if (dir >= 0) {
        const float a = std::atan2(mouse.y - origin.y, mouse.x - origin.x);
        dl->PathArcTo(c, ring_r, a - 0.45f, a + 0.45f, 12);
        dl->PathStroke(with_alpha(accent, alpha), 0, 3.f);
    }

    // The slots: a pill each, icon and label, the key faintly under the label so the pie teaches it.
    for (const PieItem& it : items) {
        const PieSlot& s = *it.slot;
        const ImVec2 u = unit(s.dir);
        const float w = item_w[int(s.dir)];
        const ImVec2 touch(c.x + u.x * radius, c.y + u.y * radius);
        const ImVec2 mid(touch.x + u.x * w / 2, touch.y + u.y * item_h / 2);
        const ImVec2 p0(mid.x - w / 2, mid.y - item_h / 2), p1(mid.x + w / 2, mid.y + item_h / 2);
        const bool hot = &it == hover;
        const bool usable = it.enabled && !pie_slot_why(s, it.enabled);
        const float round = 0.45f * fs;
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 2), ImVec2(p1.x, p1.y + 3), shadow, round);
        dl->AddRectFilled(p0, p1, with_alpha(hot && usable ? mix(bg, accent, 0.22f) : bg, 0.97f * alpha), round);
        dl->AddRect(p0, p1, hot ? with_alpha(accent, usable ? alpha : 0.5f * alpha) : border, round, 0, hot ? 1.5f : 1.f);
        if (hot && usable) {  // the direction, from the centre ring to the slot
            const ImVec2 from(c.x + u.x * ring_r, c.y + u.y * ring_r);
            dl->AddLine(from, touch, with_alpha(accent, 0.45f * alpha), 1.5f);
        }
        const std::string key = s.id == std::string("relax") ? "" : key_hint(s.id);
        const float ty = key.empty() ? (p0.y + p1.y - line) / 2 : p0.y + 0.4f * fs;
        const ImU32 ink = usable ? text : dim;
        dl->AddText(ImVec2(p0.x + pad, ty), pie_slot_on(s) ? with_alpha(accent, alpha) : ink, s.icon);
        dl->AddText(ImVec2(p0.x + pad + icon_w, ty), ink, s.label);
        if (!key.empty()) dl->AddText(ImVec2(p0.x + pad + icon_w, ty + line), dim, key.c_str());
    }

    // Under the centre: what the pie acts on, or why the hovered slot can't run.
    std::string title = pie_.bone >= 0 ? bone_label(pie_.bone)
                        : primary() >= 0 ? bone_label(primary())
                        : primary_handle() ? std::string("IK control")
                                           : "Nothing selected";
    if (hover_why) title = hover_why;
    const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    const ImVec2 t0(c.x - ts.x / 2, c.y + ring_r + 0.5f * fs), pill(0.5f * fs, 0.15f * fs);
    dl->AddRectFilled(ImVec2(t0.x - pill.x, t0.y - pill.y), ImVec2(t0.x + ts.x + pill.x, t0.y + ts.y + pill.y),
                      with_alpha(bg, 0.85f * alpha), ts.y);
    dl->AddText(t0, hover_why ? text : dim, title.c_str());
    ImGui::End();

    // Hold: the release runs the slot pointed at; a quick release at the centre was a tap, open to click.
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const PieItem* run = nullptr;
    if (pie_.held && !ImGui::IsKeyDown(pie_.key)) {
        pie_.held = false;
        if (hover) run = hover;
        else if (ImGui::GetTime() - pie_.opened_at < kTapSeconds) pie_.sticky = true;
        else close = true;
    } else if (pie_.sticky && !pie_.fake_offset && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hover) run = hover;
        else close = true;  // the centre cancels
    } else if (pie_.sticky && pie_.key != ImGuiKey_None && ImGui::IsKeyPressed(pie_.key, false)) {
        close = true;  // the key again
    }
    if (run) {
        if (const char* why = pie_slot_why(*run->slot, run->enabled)) status(why), close = true;
        else run_pie_slot(*run->slot);
    }
    if (close) pie_.open = false;
}

}  // namespace vats
