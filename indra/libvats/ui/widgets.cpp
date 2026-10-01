// Viewport Avatar Toolset - the shared slider.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "widgets.h"

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

// A slider is ImGui's drag widget over a frame drawn here: the part up to the value is filled and a grab marks it,
// as a slider shows. ImGui's frame is made transparent so ours shows through, text on top.
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
            const float x = p.x + std::clamp(ratio, 0.f, 1.f) * (q.x - p.x), grab = std::max(3.f, st.GrabMinSize * 0.35f);
            if (x > p.x + 1) dl->AddRectFilled(p, ImVec2(x, q.y), ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.22f), st.FrameRounding,
                                               ImDrawFlags_RoundCornersLeft);
            const float gx = std::clamp(x - grab / 2, p.x, q.x - grab);
            dl->AddRectFilled(ImVec2(gx, p.y + 2), ImVec2(gx + grab, q.y - 2),
                              ImGui::GetColorU32(g.ActiveId == id ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab),
                              st.GrabRounding);
        }
    }
    const ImVec4 clear(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
    const bool changed = ImGui::DragScalar(label, type, v, speed, lo, hi, fmt, flags | ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(3);
    return changed;
}

}  // namespace

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

}  // namespace vats
