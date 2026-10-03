// Viewport Avatar Toolset - the shared slider.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "widgets.h"

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // the item's id and state before it is submitted, TempInputIsActive

namespace vats {
namespace {

// Decimal places of a printf format ("%.3f" -> 3), for the log curve's smallest step above zero.
int precision(const char* fmt) {
    const char* p = fmt ? std::strchr(fmt, '%') : nullptr;
    while (p && p[1] == '%') p = std::strchr(p + 2, '%');
    if (!p || !(p = std::strchr(p, '.'))) return 3;
    return std::clamp(std::atoi(p + 1), 0, 6);
}

// A slider is ImGui's drag widget over a frame drawn here: the part up to the value is filled as its level, and a
// thin bar along the bottom edge marks it, below the text so the value is never cut. ImGui's frame is made
// transparent so ours shows through, text on top.
bool drag(const char* label, ImGuiDataType type, void* v, const void* lo, const void* hi, float ratio, float speed,
          const char* fmt, ImGuiSliderFlags flags) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImGuiID id = win->GetID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + ImGui::CalcItemWidth(), p.y + ImGui::GetFrameHeight());
    const bool typing = ImGui::TempInputIsActive(id), disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    if (!win->SkipItems) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const ImU32 bg = ImGui::GetColorU32(g.ActiveId == id                              ? ImGuiCol_FrameBgActive
                                            : g.HoveredIdPreviousFrame == id && !disabled ? ImGuiCol_FrameBgHovered
                                                                                          : ImGuiCol_FrameBg);
        ImDrawList* dl = win->DrawList;
        dl->AddRectFilled(p, q, bg, st.FrameRounding);
        if (!typing) {
            const float x = p.x + std::clamp(ratio, 0.f, 1.f) * (q.x - p.x);
            if (x > p.x + 1) dl->AddRectFilled(p, ImVec2(x, q.y), ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.22f), st.FrameRounding,
                                               ImDrawFlags_RoundCornersLeft);
            // The level's edge as a bar in the frame padding under the text, with a short tick at its end.
            const float bar = std::max(2.f, std::round(ImGui::GetFontSize() * 0.13f)), tick = bar;
            const ImU32 col = ImGui::GetColorU32(g.ActiveId == id ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
            const float tx = std::clamp(x, p.x + bar, q.x - bar);
            if (tx > p.x + bar) dl->AddRectFilled(ImVec2(p.x + st.FrameRounding * 0.5f, q.y - bar), ImVec2(tx, q.y), col);
            dl->AddRectFilled(ImVec2(tx - bar, q.y - tick - bar), ImVec2(tx + bar * 0.5f, q.y), col);
        }
    }
    const ImVec4 clear(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
    const bool changed = ImGui::DragScalar(label, type, v, speed, lo, hi, fmt, flags | ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(3);
    // Being dragged, it is not hovered for what follows: its tooltip covered the value (and the sliders under it).
    if (g.ActiveId == id && !typing) g.LastItemData.StatusFlags &= ~ImGuiItemStatusFlags_HoveredRect;
    return changed;
}

}  // namespace

std::string dock_title(const char* full, const char* tab, const char* id) {
    const std::string key = std::string("###") + id;
    const ImGuiWindow* w = ImGui::FindWindowByName(key.c_str());
    return (w && w->DockIsActive ? tab : full) + key;
}

void tab_tooltip(const char* full) {
    // After Begin, a docked window's last item is its tab.
    if (ImGui::GetCurrentWindow()->DockIsActive) ImGui::SetItemTooltip("%s", full);
}

float label_column(float em) { return ImGui::GetFontSize() * em; }

void labelled_row(const char* label, float em, float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    const float column = label_column(em), used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    ImGui::SameLine(std::max(column, used + ImGui::GetStyle().ItemSpacing.x));
    ImGui::SetNextItemWidth(width > 0 ? width * ImGui::GetFontSize() : -FLT_MIN);
}

bool empty_state(const char* text, const char* action, float height) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::GetContentRegionAvail().x, wrap = std::min(w, ImGui::GetFontSize() * 22);
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, wrap);
    const float bh = action ? ImGui::GetFrameHeight() + st.ItemSpacing.y : 0;
    const float room = height > 0 ? height : ImGui::GetContentRegionAvail().y;
    const float y0 = ImGui::GetCursorPosY(), x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosY(y0 + std::max(0.f, (room - ts.y - bh) * 0.5f));
    ImGui::SetCursorPosX(x0 + (w - ts.x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ts.x + 1);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    bool pressed = false;
    if (action) {
        const float aw = ImGui::CalcTextSize(action, nullptr, true).x + 2 * st.FramePadding.x;
        ImGui::SetCursorPosX(x0 + (w - aw) * 0.5f);
        pressed = ImGui::Button(action);
    }
    if (height > 0) ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), y0 + height));
    return pressed;
}

bool slider_float(const char* label, float* v, float lo, float hi, const char* fmt, float px, SliderCurve curve) {
    const float travel = slider_travel(ImGui::CalcItemWidth(), px);
    float ratio = hi > lo ? (*v - lo) / (hi - lo) : 0;
    ImGuiSliderFlags flags = 0;
    if (curve == SliderCurve::Log && hi > lo) {
        // ImGui's own curve: log from the format's smallest step (or lo) up; the speed is per the whole range.
        const float e = lo > 0 ? lo : std::pow(10.f, -float(precision(fmt)));
        const float a = std::log(e), b = std::log(std::max(hi, e * 1.0001f));
        ratio = *v <= e ? 0 : (std::log(*v) - a) / (b - a);
        flags |= ImGuiSliderFlags_Logarithmic;
    }
    return drag(label, ImGuiDataType_Float, v, &lo, &hi, ratio, (hi - lo) / travel, fmt, flags);
}

bool slider_int(const char* label, int* v, int lo, int hi, const char* fmt, float px) {
    const float travel = slider_travel(ImGui::CalcItemWidth(), px, hi - lo);
    const float ratio = hi > lo ? float(*v - lo) / float(hi - lo) : 0;
    // "%d frames" reads "1 frame" at 1: the word after the number loses its plural "s".
    std::string one;
    if (fmt && (*v == 1 || *v == -1))
        if (const char* d = std::strstr(fmt, "%d "); d && std::isalpha(static_cast<unsigned char>(d[3]))) {
            const char* end = d + 3;
            while (std::isalpha(static_cast<unsigned char>(*end))) ++end;
            if (end[-1] == 's') one = std::string(fmt, end - 1) + end, fmt = one.c_str();
        }
    return drag(label, ImGuiDataType_S32, v, &lo, &hi, ratio, float(hi - lo) / travel, fmt, 0);
}

bool filter_input(const char* id, const char* hint, char* buf, std::size_t size) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (buf[0]) ImGui::SetNextItemAllowOverlap();  // the × over its right end takes the pointer there
    bool changed = ImGui::InputTextWithHint(id, hint, buf, size);
    if (!buf[0] || win->SkipItems) return changed;
    // The × is laid over the box: the layout after it, and the item IsItem...() asks about, stay the box's.
    const ImGuiLastItemData box = g.LastItemData;
    struct {
        ImVec2 CursorPos, CursorPosPrevLine, CursorMaxPos, PrevLineSize;
        float PrevLineTextBaseOffset;
        bool IsSameLine, IsSetPos;
    } const dc{win->DC.CursorPos,  win->DC.CursorPosPrevLine,      win->DC.CursorMaxPos, win->DC.PrevLineSize,
               win->DC.PrevLineTextBaseOffset, win->DC.IsSameLine, win->DC.IsSetPos};
    const float h = box.Rect.GetHeight();
    ImGui::SetCursorScreenPos(ImVec2(box.Rect.Max.x - h, box.Rect.Min.y));
    ImGui::PushID(id);
    if (ImGui::InvisibleButton("clear", ImVec2(h, h))) buf[0] = 0, changed = true;
    const bool hot = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Clear the filter");
    ImGui::PopID();
    const float x = ImGui::GetItemRectMin().x + h * 0.5f, y = ImGui::GetItemRectMin().y + h * 0.5f, r = h * 0.17f;
    const ImU32 col = ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    win->DrawList->AddLine(ImVec2(x - r, y - r), ImVec2(x + r, y + r), col, 1.5f);
    win->DrawList->AddLine(ImVec2(x - r, y + r), ImVec2(x + r, y - r), col, 1.5f);
    win->DC.CursorPos = dc.CursorPos, win->DC.CursorPosPrevLine = dc.CursorPosPrevLine;
    win->DC.CursorMaxPos = dc.CursorMaxPos, win->DC.PrevLineSize = dc.PrevLineSize;
    win->DC.PrevLineTextBaseOffset = dc.PrevLineTextBaseOffset, win->DC.IsSameLine = dc.IsSameLine;
    win->DC.IsSetPos = dc.IsSetPos;
    g.LastItemData = box;
    return changed;
}

}  // namespace vats
