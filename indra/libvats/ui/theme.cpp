// Viewport Avatar Toolset - the UI themes.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Each theme is a small set of colour roles; the ImGui style and the 3D scene colours are derived
// from them. "Dusk" is dark slate with a warm amber accent; "Studio Grey" is a mid grey with a
// steel-blue accent for people who find dark themes tiring.
#include "theme.h"

#include <cmath>
#include <string>

#include "imgui_internal.h"  // the section header's item, the windows to decorate

namespace vats {
namespace {

struct Rgb8 {
    int r, g, b;
};

struct ThemeDef {
    const char* name;
    Rgb8 bg, panel, frame, frame_hi, frame_active, text, text_dim, border, accent;
    SceneColours scene;
    TimelineColours timeline;
};

const ThemeDef kThemes[] = {
    {"Dusk",
     {30, 32, 37}, {36, 39, 45}, {47, 51, 59}, {58, 63, 73}, {66, 72, 84},
     {222, 224, 229}, {128, 133, 143}, {62, 66, 76}, {232, 168, 72},
     SceneColours{},
     {IM_COL32(26, 28, 32, 255), IM_COL32(24, 26, 30, 255), IM_COL32(26, 28, 32, 230), IM_COL32(255, 255, 255, 10),
      IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 40), IM_COL32(150, 154, 163, 255)}},
    {"Studio Grey",
     {62, 63, 66}, {70, 71, 74}, {50, 51, 54}, {84, 86, 90}, {96, 98, 103},
     {226, 226, 226}, {158, 158, 160}, {44, 45, 48}, {96, 150, 196},
     SceneColours{{0.44f, 0.45f, 0.47f}, {0.17f, 0.18f, 0.19f}, {0.85f, 0.86f, 0.88f},
                  {0.66f, 0.66f, 0.67f}, {0.93f, 0.92f, 0.90f}, {0.60f, 0.62f, 0.66f}, {0.30f, 0.29f, 0.28f},
                  {0.42f, 0.80f, 0.52f}, {0.40f, 0.84f, 1.00f}},
     {IM_COL32(52, 53, 56, 255), IM_COL32(48, 49, 52, 255), IM_COL32(58, 59, 62, 235), IM_COL32(255, 255, 255, 14),
      IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 56), IM_COL32(176, 177, 181, 255)}},
};

ThemeDef g_host;                          // the host's colours as a theme (apply_theme)
const ThemeDef* g_cur = &kThemes[0];      // the theme in use

Rgb8 rgb8(const ImVec4& c) { return {int(c.x * 255 + 0.5f), int(c.y * 255 + 0.5f), int(c.z * 255 + 0.5f)}; }
ImU32 with_alpha(Rgb8 c, int a) { return IM_COL32(c.r, c.g, c.b, a); }
Rgb8 scaled(Rgb8 c, float k) { return {int(c.r * k), int(c.g * k), int(c.b * k)}; }

// The host's colour roles as a theme; the timeline's colours follow from them.
const ThemeDef& host_theme(const HostColours& h) {
    g_host = ThemeDef{"Host",          rgb8(h.bg),   rgb8(h.panel),    rgb8(h.frame), rgb8(h.frame_hi), rgb8(h.frame_active),
                      rgb8(h.text),    rgb8(h.text_dim), rgb8(h.border), rgb8(h.accent), SceneColours{}, {}};
    const Rgb8 bg = scaled(g_host.panel, 0.82f), graph = scaled(g_host.panel, 0.74f);
    g_host.timeline = {with_alpha(bg, 255),          with_alpha(graph, 255),         with_alpha(bg, 230),
                       with_alpha(g_host.text, 12),  with_alpha(g_host.text, 26),    with_alpha(g_host.text, 46),
                       with_alpha(g_host.text_dim, 255)};
    return g_host;
}

ImVec4 v4(Rgb8 c, int a = 255) { return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, a / 255.f); }

}  // namespace

int theme_count() { return int(sizeof kThemes / sizeof kThemes[0]); }
const char* theme_name(int i) { return kThemes[i].name; }
int find_theme(const std::string& name) {
    for (int i = 0; i < theme_count(); ++i)
        if (name == kThemes[i].name) return i;
    return 0;
}
const SceneColours& scene_colours() { return g_cur->scene; }
const TimelineColours& timeline_colours() { return g_cur->timeline; }
ImU32 accent_colour() { return ImGui::ColorConvertFloat4ToU32(v4(g_cur->accent)); }

static ImFont* g_bold = nullptr;

void load_fonts(const char* assets_dir) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ImFontConfig regular = cfg;
    static const ImWchar private_use[] = {0xE000, 0xF8FF, 0};  // Inter's own glyphs there would hide the icons
    regular.GlyphExcludeRanges = private_use;
    if (!io.Fonts->AddFontFromFileTTF((std::string(assets_dir) + "/fonts/Inter-Regular.ttf").c_str(), 15.f, &regular))
        io.Fonts->AddFontDefault();
    // The icons (icons.h) join the regular font, so any label can hold one. ImGui rasterises each size as it
    // is used, so they stay sharp at every UI scale. Lucide's glyphs stand on the baseline and are an em
    // tall; the offset centres them on the capitals. Without the file, labels show a box instead.
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.GlyphOffset.y = 2;
    icons.Flags |= ImFontFlags_NoLoadError;
    io.Fonts->AddFontFromFileTTF((std::string(assets_dir) + "/fonts/lucide-icons.ttf").c_str(), 15.f, &icons);
    g_bold = io.Fonts->AddFontFromFileTTF((std::string(assets_dir) + "/fonts/Inter-SemiBold.ttf").c_str(), 15.f, &cfg);
}

ImFont* bold_font() { return g_bold ? g_bold : ImGui::GetIO().Fonts->Fonts[0]; }

namespace {

// WCAG relative luminance, for picking a label colour that reads on a fill.
float luminance(Rgb8 c) {
    auto lin = [](int v) {
        const float s = v / 255.f;
        return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

}  // namespace

ImU32 accent_text_colour() {
    const float l = luminance(g_cur->accent);
    const float on_dark = (l + 0.05f) / (luminance({24, 25, 28}) + 0.05f);
    const float on_light = (luminance({250, 250, 250}) + 0.05f) / (l + 0.05f);
    return on_dark >= on_light ? IM_COL32(24, 25, 28, 255) : IM_COL32(250, 250, 250, 255);
}

bool section_header(const char* name, bool open_by_default) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(name);
    bool open = ImGui::TreeNodeUpdateNextOpen(id, open_by_default ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    // The panel's full width, as CollapsingHeader spans it, from the indent (a group inside a group is set in).
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float bleed = w->WindowPadding.x * 0.5f;
    const ImRect bb(ImVec2(p.x - bleed, p.y), ImVec2(w->WorkRect.Max.x + bleed, p.y + h));
    ImGui::ItemSize(ImVec2(0, h), st.FramePadding.y);  // no width of its own: it never widens an auto-sized window
    if (!ImGui::ItemAdd(bb, id)) return open;
    bool hovered = false, held = false;
    if (ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick)) {
        open = !open;
        ImGui::TreeNodeSetOpen(id, open);
    }
    ImDrawList* dl = w->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(held ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                      st.FrameRounding);
    ImGui::RenderNavCursor(bb, id);
    // A small chevron, then the bold label.
    const float fs = ImGui::GetFontSize(), c = fs * 0.22f;
    const ImVec2 m(bb.Min.x + st.FramePadding.x + fs * 0.35f, (bb.Min.y + bb.Max.y) * 0.5f);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    if (open) {
        const ImVec2 pts[] = {ImVec2(m.x - c, m.y - c * 0.5f), ImVec2(m.x, m.y + c * 0.5f), ImVec2(m.x + c, m.y - c * 0.5f)};
        dl->AddPolyline(pts, 3, dim, 0, std::max(1.f, fs / 10));
    } else {
        const ImVec2 pts[] = {ImVec2(m.x - c * 0.5f, m.y - c), ImVec2(m.x + c * 0.5f, m.y), ImVec2(m.x - c * 0.5f, m.y + c)};
        dl->AddPolyline(pts, 3, dim, 0, std::max(1.f, fs / 10));
    }
    ImGui::PushFont(bold_font(), 0);
    const char* end = ImGui::FindRenderedTextEnd(name);
    ImGui::RenderTextClipped(ImVec2(m.x + fs * 0.75f, bb.Min.y), ImVec2(bb.Max.x - st.FramePadding.x, bb.Max.y), name, end,
                             nullptr, ImVec2(0, 0.5f));
    ImGui::PopFont();
    return open;
}

void decorate_floating_windows() {
    ImGuiContext& g = *GImGui;
    const ImU32 edge = ImGui::GetColorU32(ImGuiCol_Border, 1.6f);  // the border role, opaque
    for (ImGuiWindow* w : g.Windows) {
        if (!w->Active || w->Hidden || w->DockIsActive || w->ParentWindow ||
            (w->Flags & (ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)))
            continue;
        ImDrawList* dl = w->DrawList;
        const ImVec2 a = w->Pos, b(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        const float r = w->WindowRounding, s = std::round(ImGui::GetFontSize() * 0.55f);
        dl->PushClipRect(ImVec2(a.x - 2 * s, a.y - 2 * s), ImVec2(b.x + 2 * s, b.y + 2 * s), false);
        // The shadow: rings outside the window, fading out, so none of it lands on the content.
        for (int i = 1; i <= int(s); ++i) {
            const float k = 1 - float(i) / (s + 1);
            dl->AddRect(ImVec2(a.x - i, a.y - i), ImVec2(b.x + i, b.y + i), IM_COL32(0, 0, 0, int(60 * k * k)), r + i, 0, 1.f);
        }
        dl->AddRect(ImVec2(a.x - 0.5f, a.y - 0.5f), ImVec2(b.x + 0.5f, b.y + 0.5f), edge, r + 0.5f, 0, 1.f);
        dl->PopClipRect();
    }
}

void apply_theme(int theme, float scale, const HostColours* host) {
    g_cur = host ? &host_theme(*host) : &kThemes[theme >= 0 && theme < theme_count() ? theme : 0];
    const ThemeDef& t = *g_cur;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 10;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowRounding = 6;
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.DockingSeparatorSize = 5;  // a gap of the darker ground between panels
    s.WindowMenuButtonPosition = ImGuiDir_None;
    // A disabled button's tooltip says why it is disabled, so tooltips show on disabled items too.
    s.HoverFlagsForTooltipMouse |= ImGuiHoveredFlags_AllowWhenDisabled;
    s.HoverFlagsForTooltipNav |= ImGuiHoveredFlags_AllowWhenDisabled;

    ImVec4* c = s.Colors;
    const ImVec4 accent = v4(t.accent), accent_soft = v4(t.accent, 90);
    c[ImGuiCol_Text] = v4(t.text);
    c[ImGuiCol_TextDisabled] = v4(t.text_dim);
    c[ImGuiCol_WindowBg] = v4(t.panel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v4(t.panel, 250);
    c[ImGuiCol_Border] = v4(t.border, 160);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v4(t.frame);
    c[ImGuiCol_FrameBgHovered] = v4(t.frame_hi);
    c[ImGuiCol_FrameBgActive] = v4(t.frame_active);
    // A panel's tab strip sits between the ground and the panel, so each docked panel reads as one card.
    const Rgb8 strip{(t.bg.r + t.panel.r) / 2 + 1, (t.bg.g + t.panel.g) / 2 + 1, (t.bg.b + t.panel.b) / 2 + 1};
    // A floating window's title bar is a step lighter than the panels, so it reads as lifted off them; the docked
    // tab strips keep the strip colour (App::draw_dockspace pushes ImGuiCol_Tab's over the dockspace).
    const Rgb8 lifted{(2 * t.panel.r + 3 * t.frame_hi.r) / 5, (2 * t.panel.g + 3 * t.frame_hi.g) / 5,
                      (2 * t.panel.b + 3 * t.frame_hi.b) / 5};
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = v4(lifted);
    c[ImGuiCol_TitleBgActive] = v4(t.frame_hi);
    c[ImGuiCol_MenuBarBg] = v4(t.bg);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v4(t.frame_hi);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(t.frame_active);
    c[ImGuiCol_ScrollbarGrabActive] = accent_soft;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = v4(t.accent, 200);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = v4(t.frame_hi);
    c[ImGuiCol_ButtonHovered] = v4(t.frame_active);
    c[ImGuiCol_ButtonActive] = accent_soft;
    c[ImGuiCol_Header] = v4(t.accent, 60);  // selected rows
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_HeaderActive] = accent_soft;
    c[ImGuiCol_Separator] = v4(t.border);
    c[ImGuiCol_SeparatorHovered] = accent_soft;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextLink] = ImVec4(0.47f, 0.69f, 0.95f, 1);  // help links: a fixed blue that reads on every theme
    c[ImGuiCol_ResizeGripHovered] = accent_soft;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = v4(strip);
    c[ImGuiCol_TabHovered] = v4(t.frame_hi);
    c[ImGuiCol_TabSelected] = v4(t.panel);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = v4(strip);
    c[ImGuiCol_TabDimmedSelected] = v4(t.panel);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = accent_soft;
    c[ImGuiCol_DockingEmptyBg] = v4(t.bg);
    c[ImGuiCol_TextSelectedBg] = accent_soft;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.04f, 0.04f, 0.05f, 0.6f);

    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

}  // namespace vats
