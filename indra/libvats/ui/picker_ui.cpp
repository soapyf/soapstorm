// Viewport Avatar Toolset - the Picker tab: joint dots and bone lines over the avatar or its silhouette, group
// selection, and selection sets.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (PK-1..PK-3, SS-1..SS-3). The layout, groups, framing and click tests are in the core
// (picker.h); here they are drawn with ImGui draw lists over one of two backdrops: the avatar, rendered by the host
// through its Picker scene target in the current (or the rest) pose, or the silhouette traced from the Linden body
// (plain ImGui drawing, so every host has it). A host without the Picker target gets the silhouette.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "app.h"
#include "widgets.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui_internal.h"
#include "theme.h"
#include "vats/edit.h"
#include "vats/picker.h"
#include "vats/selection_sets.h"

namespace vats {

struct PickerUi {
    int page = 0;
    int view[kPickerPageCount] = {};
    bool points = false, volumes = false;  // the Points menu's overlays
    int render_ok = -1;                    // the host's Picker target: -1 not tried yet, 0 missing, 1 renders
    // The avatar picture (style A) and what it shows: the dots are drawn from the same pose and framing, so they stay
    // on it while a new picture waits (at most ten a second while you drag or scrub, none while playing).
    ImTextureID picture{};
    bool have_picture = false;
    std::string picture_key;
    std::vector<Xform> picture_globals;
    PickerFit picture_fit;
    std::uint64_t picture_ns = 0;
    Pose chart_pose;
    bool have_chart = false;
    PickerCycle cycle;
};

namespace {

// Bone states, as the Bones list and the view colour them: pinned light blue, IK violet (spec 06 4.1 item 5).
constexpr ImU32 kPinned = IM_COL32(140, 217, 255, 255), kIk = IM_COL32(224, 130, 230, 255),
                kWhite = IM_COL32(255, 255, 255, 255), kPlainDot = IM_COL32(236, 238, 243, 255),
                kPlainLine = IM_COL32(236, 238, 243, 125), kHiddenCol = IM_COL32(180, 186, 197, 190),
                kVolume = IM_COL32(186, 166, 236, 255);

ImU32 with_alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0.f, 1.f) * 255) << 24); }
ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

std::string sets_path(const std::string& dir) { return dir + "selection_sets.json"; }

void dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float w, float dash) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 1e-3f) return;
    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
    for (float t = 0; t < len; t += dash * 1.8f) {
        const float e = std::min(len, t + dash);
        dl->AddLine(ImVec2(a.x + d.x * t, a.y + d.y * t), ImVec2(a.x + d.x * e, a.y + d.y * e), c, w);
    }
}

void dashed_circle(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float w) {
    constexpr int kDashes = 8;
    for (int i = 0; i < kDashes; ++i) {
        const float a0 = float(i) / kDashes * 6.2831853f, a1 = a0 + 0.55f * 6.2831853f / kDashes;
        dl->PathArcTo(c, r, a0, a1, 4);
        dl->PathStroke(col, 0, w);
    }
}

void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 edge, float w) {
    const ImVec2 p[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    dl->AddConvexPolyFilled(p, 4, fill);
    dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, w);
}

// A bold title over a dim line, as the picker's tooltips read.
void tip(const std::string& title, const std::string& sub, const std::string& more = "") {
    ImGui::BeginTooltip();
    ImGui::PushFont(bold_font(), 0);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (!sub.empty()) ImGui::TextUnformatted(sub.c_str());
    if (!more.empty()) ImGui::TextUnformatted(more.c_str());
    ImGui::PopStyleColor();
    ImGui::EndTooltip();
}

const char* kModes = "Shift adds, Ctrl removes";

// A small segmented button in the canvas's tool row.
bool tool_button(const char* label, bool on, float w = 0) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    const bool pressed = ImGui::Button(label, ImVec2(w, 0));
    if (on) ImGui::PopStyleColor();
    return pressed;
}

}  // namespace

void App::load_library_sets() {
    if (library_sets_loaded_) return;
    library_sets_loaded_ = true;
    const std::string path = sets_path(library_dir());
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!load_selection_sets(ss.str(), library_sets_, err)) {
        // Kept aside rather than overwritten by the next save (03 P10), as the pose library does.
        const std::string aside = path + ".corrupt-" + std::to_string(host_.ticks_ns() / 1000000);
        std::rename(path.c_str(), aside.c_str());
        message("Selection sets damaged", "They could not be read (" + err + ") and were renamed to\n" + aside);
    }
}

void App::save_library_sets() {
    std::string why;
    if (!write_text(sets_path(library_dir()), save_selection_sets(library_sets_), false, why))
        message("Could not save the selection sets", why);
}

void App::select_nodes(const std::vector<int>& nodes, int mode) {
    handle_primary_ = false;
    selected_prop_ = -1;
    if (mode == 0) selection_.clear(), handles_.clear();
    for (int n : nodes) {
        auto it = std::find(selection_.begin(), selection_.end(), n);
        if (mode == 2) {
            if (it != selection_.end()) selection_.erase(it);
            continue;
        }
        if (it == selection_.end()) selection_.push_back(n);
        if (skel_[n].volume) show_volumes_ = true;  // a hidden group is shown before its bones are selected
        else show_category_[static_cast<int>(skel_[n].category)] = true;
    }
}

int App::category_size(int category) const {
    int count = 0;
    for (int i = 0; i < skel_.size(); ++i)
        count += category == 8 ? skel_[i].volume : !skel_[i].volume && static_cast<int>(skel_[i].category) == category;
    return count;
}

void App::select_category(int category, int mode) {
    std::vector<int> nodes;
    for (int i = 0; i < skel_.size(); ++i)
        if (category == 8 ? skel_[i].volume : !skel_[i].volume && static_cast<int>(skel_[i].category) == category)
            nodes.push_back(i);
    select_nodes(nodes, mode);
}

bool App::cli_picker(const std::string& page_view) {
    if (!picker_ui_) picker_ui_ = std::make_shared<PickerUi>();
    const size_t slash = page_view.find('/');
    const std::string page = page_view.substr(0, slash), view = slash == std::string::npos ? "" : page_view.substr(slash + 1);
    auto same = [](const std::string& a, const char* b) {
        return a.size() == std::strlen(b) &&
               std::equal(a.begin(), a.end(), b, [](char x, char y) { return std::tolower(x) == std::tolower(y); });
    };
    for (int p = 0; p < kPickerPageCount; ++p) {
        if (!same(page, picker_page_name(PickerPage(p)))) continue;
        for (int v = 0; v < picker_view_count(PickerPage(p)); ++v)
            if (view.empty() || same(view, picker_view_name(PickerPage(p), v))) {
                picker_ui_->page = p, picker_ui_->view[p] = view.empty() ? picker_ui_->view[p] : v;
                pending_tab_ = "Picker";
                return true;
            }
    }
    return false;
}

bool App::cli_select_group(const std::string& name) {
    for (const PickerGroup& g : picker_groups())
        if (g.name == name) return select_nodes(picker_group_nodes(skel_, g), 0), true;
    return false;
}

bool App::cli_picker_style(const std::string& style) {
    if (style != "silhouette" && style != "avatar" && style != "rest") return false;
    settings_.picker_style = style == "silhouette" ? "silhouette" : "avatar";
    settings_.picker_live = style != "rest";
    pending_tab_ = "Picker";
    return true;
}

void App::draw_picker_panel() {
    // A layout saved before the Picker existed: open it as a tab beside Bones.
    if (ImGuiWindow* bones = ImGui::FindWindowByName("Bones"); bones && bones->DockId)
        ImGui::SetNextWindowDockID(bones->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Picker")) return ImGui::End();
    load_library_sets();
    if (!picker_ui_) picker_ui_ = std::make_shared<PickerUi>();
    PickerUi& ui = *picker_ui_;
    if (!ui.have_chart) ui.chart_pose = picker_chart_pose(skel_), ui.have_chart = true;
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fs = ImGui::GetFontSize(), u = fs / 15.f, line_h = fs;
    const Clip& clip = doc_.clip();

    // --- pages ---
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2 * u, st.ItemSpacing.y));
        const float w = (ImGui::GetContentRegionAvail().x - 3 * 2 * u) / 4;
        for (int p = 0; p < kPickerPageCount; ++p) {
            if (p) ImGui::SameLine();
            if (tool_button(picker_page_name(PickerPage(p)), ui.page == p, w)) ui.page = p;
        }
        ImGui::PopStyleVar();
    }
    const PickerPage page = PickerPage(ui.page);
    int& view = ui.view[ui.page];
    view = std::clamp(view, 0, picker_view_count(page) - 1);

    // --- the canvas ---
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float below = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y;
    const float cw = std::max(avail.x, 120.f);
    const float ch = std::clamp(avail.y - below, std::min(cw * 0.95f, 260 * u), cw * 1.3f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const PickerRect canvas{0, 0, cw, ch};
    auto S = [&](V2 p) { return ImVec2(o.x + float(p.x), o.y + float(p.y)); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + cw, o.y + ch), timeline_background(), 6 * u);

    // Face chips: laid out first, their rows (Swap Sides at the end) are the Face page's bottom band.
    const auto& groups = picker_groups();
    const float pad = float(kPickerPad * line_h), btn_pad = 4 * u, chip_h = fs + 2 * 2 * u;
    const float icon_w = fs + 2 * btn_pad;  // icon_button_width() with the tool row's padding
    std::vector<std::pair<int, PickerRect>> chips;  // group (-1 = Swap Sides), place
    int chip_rows = 0;
    const float chip_fs = fs * 0.87f, chip_pad = 3 * u;  // chips a little smaller than the text, two rows at 255 px
    if (page == PickerPage::Face) {
        double x = pad, row = 0;
        ImGui::PushFont(nullptr, st.FontSizeBase * 0.87f);  // chip_fs before the UI scale
        for (int g = 0; g <= int(groups.size()); ++g) {
            if (g < int(groups.size()) && groups[g].kind != PickerGroupKind::Chip) continue;
            const double w = g < int(groups.size()) ? ImGui::CalcTextSize(groups[g].label.c_str()).x + 2 * chip_pad : icon_w;
            if (x > pad && x + w > cw - pad) x = pad, ++row;
            chips.push_back({g < int(groups.size()) ? g : -1, {x, row, w, chip_h}});
            x += w + 3 * u;
        }
        ImGui::PopFont();
        chip_rows = int(row) + 1;
        const double band_top = ch - pad - chip_rows * kPickerChipRow * line_h;
        for (auto& [g, r] : chips) {
            r.y = band_top + (kPickerChipRow * line_h - chip_h) / 2 + r.y * kPickerChipRow * line_h;
            if (g < 0) r.x = cw - pad - r.w;  // Swap Sides: right-aligned on its row
        }
    }
    const PickerRect labels_area = picker_label_area(page, canvas, line_h, chip_rows);
    const PickerRect area = picker_fit_area(page, canvas, line_h, chip_rows);
    // The top band's right: the backdrop and pose buttons.
    const PickerRect strip{labels_area.x + labels_area.w - 2 * icon_w - u, labels_area.y, 2 * icon_w + u, chip_h};

    // --- the backdrop: the avatar (A) or the silhouette (B) ---
    if (ui.render_ok < 0) {  // is there a Picker target? Once, with a tiny picture.
        Camera probe;
        ui.render_ok = host_.scene_begin(ui::SceneTarget::Picker, 4, 4, probe, scene_colours()) ? 1 : 0;
        if (ui.render_ok) host_.scene_end();
    }
    const bool has_body = mesh_body() || body_ != Body::SkeletonOnly;
    const bool style_a = settings_.picker_style == "avatar" && ui.render_ok == 1;
    const bool live = style_a && settings_.picker_live;
    const Shape* sh = shape();
    const std::vector<Xform> chart = skel_.global_pose(ui.chart_pose, sh);
    const std::vector<PickerPart> chart_parts = picker_parts(skel_, chart, chart, sh, page, view);
    const PickerLayout chart_layout = picker_layout(skel_, chart, chart_parts, sh, page, view);
    const auto silhouette = picker_silhouette(skel_, page, view, picker_anchors(skel_, chart, chart_parts, sh, page, view));
    // The framing: the rest layout and its silhouette (so A and B frame alike), and the pose shown when it reaches out.
    std::vector<V2> fit_pts = picker_extent(chart_layout);
    for (const auto& loop : silhouette) fit_pts.insert(fit_pts.end(), loop.begin(), loop.end());
    const std::vector<Xform>& shown = live ? globals_ : chart;
    std::vector<PickerPart> parts = live ? picker_parts(skel_, shown, chart, sh, page, view) : chart_parts;
    if (live) {
        const auto pts = picker_extent(picker_layout(skel_, shown, parts, sh, page, view));
        fit_pts.insert(fit_pts.end(), pts.begin(), pts.end());
    }
    PickerFit fit = picker_fit(fit_pts, area);
    std::vector<Xform> layout_globals = shown;
    if (style_a && has_body) {
        const float fb = io.DisplayFramebufferScale.x;
        const int pw = std::clamp(int(cw * fb), 16, 2048), ph = std::clamp(int(ch * fb), 16, 2048);
        char key[256];
        std::snprintf(key, sizeof key, "%d %d %d %d %d %s %s %.4f %.2f %.2f %d", ui.page, view, pw, ph, int(body_),
                      settings_.mesh_body.c_str(), settings_.theme.c_str(), fit.scale, fit.ox, fit.oy, int(live));
        bool stale = !ui.have_picture || ui.picture_key != key || ui.picture_globals.size() != shown.size();
        for (size_t i = 0; !stale && i < shown.size(); ++i)
            stale = (shown[i].pos - ui.picture_globals[i].pos).length() > 1e-5 ||
                    std::fabs(shown[i].rot.dot(ui.picture_globals[i].rot)) < 1 - 1e-9;
        const std::uint64_t now = host_.ticks_ns(), wait = 100'000'000;  // ten pictures a second at most
        if (stale && !playing_ && (!ui.have_picture || now - ui.picture_ns >= wait)) {
            // Render: the body's triangles, each part carried into page space, seen from +X in ortho.
            std::vector<Triangles> tris;
            std::vector<Xform> keep = globals_;
            globals_ = shown;  // a mesh body poses from globals_
            capture_triangles_ = &tris;
            static std::vector<Vertex> mv;
            static std::vector<std::uint32_t> mi;
            if (mesh_body()) draw_mesh_body(mv, mi);
            else draw_avatar(false, shown, scene_colours());
            capture_triangles_ = nullptr;
            globals_ = std::move(keep);
            Camera cam;
            cam.target = {0, (cw / 2 - fit.ox) / fit.scale, (fit.oy - ch / 2) / fit.scale};
            cam.yaw = cam.pitch = 0;
            cam.distance = 5;
            cam.ortho = true;
            const Mat4 proj = orthographic(cw / 2 / fit.scale, ch / 2 / fit.scale, -10, 20);
            if (host_.scene_begin(ui::SceneTarget::Picker, pw, ph, cam, scene_colours(), &proj)) {
                static std::vector<Vertex> v;
                static std::vector<std::uint32_t> idx;
                for (const Triangles& t : tris) {
                    v.clear(), idx.clear();
                    const size_t count = t.indices.empty() ? t.verts.size() : t.indices.size();
                    for (const PickerPart& p : parts) {
                        const std::uint32_t base = std::uint32_t(v.size());
                        for (const Vertex& x : t.verts) {
                            const Vec3 q = p.to_page.apply({x.p[0], x.p[1], x.p[2]});
                            const Vec3 n = p.to_page.rot.rotate({x.n[0], x.n[1], x.n[2]});
                            v.push_back({{float(q.x), float(q.y), float(q.z)}, {float(n.x), float(n.y), float(n.z)},
                                         {x.c[0], x.c[1], x.c[2], x.c[3]}});
                        }
                        for (size_t k = 0; k + 2 < count; k += 3) {
                            std::uint32_t tri[3];
                            Vec3 c;
                            for (int j = 0; j < 3; ++j) {
                                tri[j] = t.indices.empty() ? std::uint32_t(k + j) : t.indices[k + size_t(j)];
                                const Vertex& x = v[base + tri[j]];
                                c += Vec3{x.p[0], x.p[1], x.p[2]} * (1.0 / 3);
                            }
                            // The face keeps a little more neck, cut off straight when the picture is shown.
                            if (!picker_keeps(p, c) && !(page == PickerPage::Face && picker_keeps(p, c + Vec3{0, 0, 0.03})))
                                continue;
                            for (std::uint32_t j : tri) idx.push_back(base + j);
                        }
                    }
                    if (!idx.empty()) host_.scene_triangles(v, idx, true, t.gloss);
                }
                ui.picture = host_.scene_end();
                ui.have_picture = true;
                ui.picture_key = key;
                ui.picture_globals = shown;
                ui.picture_fit = fit;
                ui.picture_ns = now;
            } else {
                ui.render_ok = 0;  // the host has no Picker target after all: the silhouette from now on
            }
        } else if (stale && !playing_) {
            host_.wake(double(wait - (now - ui.picture_ns)) * 1e-9);  // the next picture, once the wait is over
        }
        if (ui.have_picture && ui.render_ok == 1) {
            // The dots follow the picture: its pose and framing, until the next one.
            layout_globals = ui.picture_globals;
            fit = ui.picture_fit;
            parts = picker_parts(skel_, layout_globals, chart, sh, page, view);
            const float cut = page == PickerPage::Face && !parts.empty() && parts[0].clip
                                  ? float(fit.px({0, parts[0].lo.z}).y) : ch;  // the neck's straight end
            dl->PushClipRect(o, ImVec2(o.x + cw, o.y + std::min(cut, ch)), true);
            dl->AddImage(ui.picture, o, ImVec2(o.x + cw, o.y + ch), ImVec2(0, 1), ImVec2(1, 0), IM_COL32(222, 224, 232, 235));
            dl->PopClipRect();
        }
    }
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_FrameBgActive), text_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 outline = mix(fill, text_col, 0.28f), feature = mix(fill, text_col, 0.22f);
    if (!style_a) {  // the silhouette stands in the rest pose, as style B's dots do
        for (const auto& loop : silhouette) {
            std::vector<ImVec2> pts;
            for (V2 q : loop) pts.push_back(S(fit.px(q)));
            if (pts.size() < 3) continue;
            const bool faint = page == PickerPage::Extras;  // the extras float around the body
            dl->AddConcavePolyFilled(pts.data(), int(pts.size()), faint ? with_alpha(fill, 0.7f) : fill);
            dl->AddPolyline(pts.data(), int(pts.size()), faint ? with_alpha(outline, 0.6f) : outline, ImDrawFlags_Closed,
                            1.5f * u);
        }
    }
    if (style_a && !has_body)
        dl->AddText(S({(cw - ImGui::CalcTextSize("No body: View > Body").x) / 2, area.y}),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), "No body: View > Body");

    const PickerLayout layout = picker_layout(skel_, layout_globals, parts, sh, page, view, ui.points, ui.volumes);
    const PickerScreen scr = picker_screen(layout, fit, 7.5 * u);
    // Labels and the Rest pose tag in small bold capitals, quieter than the panel's text.
    const float label_fs = fs * 0.8f;
    auto label_w = [&](const std::string& s) {
        ImGui::PushFont(bold_font(), st.FontSizeBase * 0.8f);  // label_fs before the UI scale
        const double w = ImGui::CalcTextSize(s.c_str()).x;
        ImGui::PopFont();
        return w;
    };
    // The Rest pose tag sits beside those buttons while the avatar stands in its rest pose.
    const bool rest_tag = style_a && !settings_.picker_live;
    const float tag_w = float(label_w("REST POSE")) + 10 * u;
    const PickerRect tag{strip.x - tag_w - 4 * u, labels_area.y, tag_w, chip_h};
    std::vector<PickerRect> blocked = {strip};
    if (rest_tag) blocked.push_back(tag);
    const std::vector<PickerLabel> labels =
        picker_labels(skel_, page, view, scr, labels_area, blocked, label_w, label_fs + 5 * u);

    // Face features on the silhouette, drawn through the face bones' own spots: brows, eyes, nose and lips.
    auto at_bone = [&](const char* name, ImVec2& out) {
        const int n = skel_.find(name);
        for (const PickerDot& d : scr.dots)
            if (d.node == n) return out = S(d.p), true;
        return false;
    };
    if (page == PickerPage::Face && !style_a) {
        for (const char* s : {"Right", "Left"}) {
            ImVec2 b[3], e;
            bool ok = true;
            int k = 0;
            for (const char* part : {"Outer", "Center", "Inner"})
                ok = at_bone((std::string("mFaceEyebrow") + part + s).c_str(), b[k++]) && ok;
            if (ok) dl->AddPolyline(b, 3, feature, 0, 3.5f * u);
            if (at_bone((std::string("mEye") + s).c_str(), e)) {
                const float w = float(0.0135 * fit.scale), h = float(0.006 * fit.scale);
                dl->PathClear();
                dl->PathLineTo(ImVec2(e.x - w, e.y));
                dl->PathBezierQuadraticCurveTo(ImVec2(e.x, e.y - 2 * h), ImVec2(e.x + w, e.y));
                dl->PathBezierQuadraticCurveTo(ImVec2(e.x, e.y + 1.6f * h), ImVec2(e.x - w, e.y));
                dl->PathFillConvex(mix(timeline_background(), fill, 0.4f));
                dl->PathLineTo(ImVec2(e.x - w, e.y));
                dl->PathBezierQuadraticCurveTo(ImVec2(e.x, e.y - 2 * h), ImVec2(e.x + w, e.y));
                dl->PathBezierQuadraticCurveTo(ImVec2(e.x, e.y + 1.6f * h), ImVec2(e.x - w, e.y));
                dl->PathStroke(feature, 0, 2 * u);
            }
        }
        ImVec2 a, b;
        if (at_bone("mFaceNoseBridge", a) && at_bone("mFaceNoseCenter", b)) dl->AddLine(a, b, with_alpha(feature, 0.7f), 2 * u);
        ImVec2 lips[8];
        const char* order[8] = {"mFaceLipCornerRight", "mFaceLipUpperRight", "mFaceLipUpperCenter", "mFaceLipUpperLeft",
                                "mFaceLipCornerLeft",  "mFaceLipLowerLeft",  "mFaceLipLowerCenter", "mFaceLipLowerRight"};
        bool ok = true;
        for (int k = 0; k < 8; ++k) ok = at_bone(order[k], lips[k]) && ok;
        if (ok) {
            dl->AddConcavePolyFilled(lips, 8, mix(fill, IM_COL32(150, 96, 110, 255), 0.35f));
            dl->AddPolyline(lips, 8, feature, ImDrawFlags_Closed, 1.5f * u);
        }
    }

    // --- states ---
    auto selected = [&](int n) { return std::find(selection_.begin(), selection_.end(), n) != selection_.end(); };
    auto group_nodes = [&](int g) { return picker_group_nodes(skel_, groups[size_t(g)]); };
    auto group_selected = [&](int g) {
        const std::vector<int> nodes = group_nodes(g);
        return !nodes.empty() && std::all_of(nodes.begin(), nodes.end(), selected);
    };
    auto group_hidden = [&](int g) {
        const std::vector<int> nodes = group_nodes(g);
        return !nodes.empty() && std::none_of(nodes.begin(), nodes.end(), [&](int n) { return node_visible(n); });
    };

    // --- hover ---
    const PickerRect& hot = labels_area;
    ImGui::SetCursorScreenPos(S({hot.x, hot.y}));
    ImGui::SetNextItemAllowOverlap();  // the backdrop and pose buttons sit on it
    ImGui::InvisibleButton("##picker_canvas", ImVec2(float(std::max(hot.w, 1.0)), float(std::max(hot.h, 1.0))));
    const V2 m{io.MousePos.x - o.x, io.MousePos.y - o.y};
    const bool on_strip = PickerRect{strip.x - 2 * u, strip.y - 2 * u, strip.w + 4 * u, strip.h + 4 * u}.contains(m);
    const bool canvas_hovered = ImGui::IsItemHovered() && !on_strip,
               canvas_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !on_strip;
    int hover_group = -1, hover_bone = -1;
    std::vector<int> ranked;
    const double dot_r = 7 * u, line_r = 4.5 * u;
    if (canvas_hovered) {
        for (const PickerLabel& l : labels)
            if (PickerRect{l.r.x - 3 * u, l.r.y - 2 * u, l.r.w + 6 * u, l.r.h + 4 * u}.contains(m)) hover_group = l.group;
        if (hover_group < 0) hover_group = picker_cap_hit(scr, m, 7 * u);
        if (hover_group < 0) {
            ranked = picker_hits(scr, m, dot_r, line_r);
            if (!ranked.empty()) {
                // After a click here, the bone that click picked (a second click takes the next one).
                const bool same = (m - ui.cycle.at).length() <= 4 * u && ui.cycle.ranked.size() == ranked.size();
                hover_bone = same ? ui.cycle.ranked[size_t(ui.cycle.index)] : ranked[0];
            }
        }
    }

    // --- the widgets on the canvas: the tool row (or the Face chips) and the top band's buttons ---
    const float row_y = o.y + float(labels_area.y + labels_area.h + (kPickerToolRow * line_h - chip_h) / 2);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(btn_pad, 2 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1 * u, st.ItemSpacing.y));
    const int mode = io.KeyCtrl ? 2 : io.KeyShift ? 1 : 0;
    auto select_group = [&](int g) {
        const std::vector<int> nodes = group_nodes(g);
        select_nodes(nodes, mode);
        const char* how = mode == 2 ? " removed" : mode == 1 ? " added" : "";
        status(groups[size_t(g)].name + how + ": " + std::to_string(selection_.size()) + " bone(s) selected");
    };
    auto swap_sides = [&] {
        ImGui::BeginDisabled(selection_.empty());
        if (icon_button("picker_swap", icon::kSwap, "Swap Sides: select the same bones on the other side")) {
            std::vector<int> other;
            for (int n : selection_)
                if (int k = skel_.mirror(n); k >= 0 && std::find(other.begin(), other.end(), k) == other.end())
                    other.push_back(k);
            select_nodes(other, 0);
            status("Swapped sides: " + std::to_string(selection_.size()) + " bone(s) selected");
        }
        ImGui::EndDisabled();
    };
    for (const auto& [g, r] : chips) {
        ImGui::SetCursorScreenPos(S({r.x, r.y}));
        if (g < 0) {
            swap_sides();
            continue;
        }
        ImGui::PushFont(nullptr, st.FontSizeBase * 0.87f);  // chip_fs before the UI scale
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(chip_pad, float(chip_h - chip_fs) / 2));
        const bool pressed = tool_button((groups[size_t(g)].label + "##chip").c_str(), group_selected(g), float(r.w));
        ImGui::PopStyleVar();
        ImGui::PopFont();
        if (pressed) select_group(g);
        if (ImGui::IsItemHovered()) {
            hover_group = g;
            tip(groups[size_t(g)].name + ": " + count_noun(group_nodes(g).size(), "bone"), kModes);
        }
    }
    if (page != PickerPage::Face) {
        ImGui::SetCursorScreenPos(ImVec2(o.x + float(labels_area.x), row_y));
        const int views = picker_view_count(page);
        for (int v = 0; v < views && views > 1; ++v) {
            if (v) ImGui::SameLine();
            if (tool_button((std::string(picker_view_name(page, v)) + "##view").c_str(), view == v)) view = v;
        }
        if (page == PickerPage::Body || page == PickerPage::Extras) {
            ImGui::SameLine(0, 5 * u);
            if (icon_button("picker_points", icon::kPlace, "Points: show attachment points or collision volumes here",
                            ui.points || ui.volumes))
                ImGui::OpenPopup("##picker_points");
            if (ImGui::BeginPopup("##picker_points")) {
                ImGui::MenuItem("Attachment Points", nullptr, &ui.points);
                ImGui::MenuItem("Collision Volumes", nullptr, &ui.volumes);
                ImGui::EndPopup();
            }
        }
        if (page == PickerPage::Hands) {
            ImGui::SameLine(0, 5 * u);
            for (int k = 1; k <= 3; ++k) {
                int g = -1;
                for (int i = 0; i < int(groups.size()); ++i)
                    if (groups[i].kind == PickerGroupKind::Row && groups[i].label == std::to_string(k)) g = i;
                if (k > 1) ImGui::SameLine();
                if (tool_button((std::to_string(k) + "##row").c_str(), g >= 0 && group_selected(g)) && g >= 0)
                    select_group(g);
                if (ImGui::IsItemHovered() && g >= 0) {
                    hover_group = g;
                    tip(groups[size_t(g)].name + ": " + count_noun(group_nodes(g).size(), "bone"),
                        std::string(k == 1 ? "The knuckles" : k == 2 ? "The middle joints" : "The fingertip joints") +
                            " of every finger, both hands",
                        kModes);
                }
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x + float(labels_area.x + labels_area.w) - icon_w, row_y));
        swap_sides();
    }
    // The top band's right: the backdrop (avatar or silhouette) and, for the avatar, the pose it stands in.
    ImGui::SetCursorScreenPos(S({strip.x, strip.y}));
    {
        ImGui::BeginDisabled(ui.render_ok != 1);
        const std::string style_tip =
            ui.render_ok != 1 ? "Avatar backdrop: not available here, this program cannot draw the avatar in the "
                                "Picker yet. The silhouette shows instead"
            : style_a ? "Avatar backdrop: your body as the view draws it (click for the silhouette)"
                      : "Silhouette backdrop: the Linden body's outline (click to show your avatar instead)";
        if (icon_button("picker_style", icon::kWalkTest, style_tip, style_a)) {
            settings_.picker_style = style_a ? "silhouette" : "avatar";
            save_settings();
        }
        ImGui::EndDisabled();
        if (ui.render_ok != 1) ImGui::SetItemTooltip("%s", style_tip.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!style_a);
        const std::string live_tip =
            !style_a ? "Live pose: with the avatar backdrop, whether it follows the current pose. The silhouette always "
                       "shows the rest pose"
            : settings_.picker_live ? "Live pose: the avatar and its dots follow the current pose (click for the rest "
                                      "pose, where crossed limbs never pile up)"
                                    : "Rest pose: the avatar and its dots stand in the rest pose (click to follow the "
                                      "current pose)";
        if (icon_button("picker_live", icon::kRunning, live_tip, live)) {
            settings_.picker_live = !settings_.picker_live;
            save_settings();
        }
        ImGui::EndDisabled();
        if (!style_a) ImGui::SetItemTooltip("%s", live_tip.c_str());
    }
    ImGui::PopStyleVar(2);

    // --- draw: lines, dots, fingertip circles, labels ---
    std::vector<int> lit;  // lit white: the hovered bone or group
    if (hover_group >= 0) lit = group_nodes(hover_group);
    else if (hover_bone >= 0) lit = {hover_bone};
    auto is_lit = [&](int n) { return std::find(lit.begin(), lit.end(), n) != lit.end(); };
    auto partner = [&](int n) {  // the timeline's Mirror is on and the other side is selected: this one moves too
        if (!mirror_live_ || selected(n)) return false;
        const int k = skel_.mirror(n);
        return k >= 0 && k != n && selected(k);
    };
    auto state_col = [&](int n, ImU32& c) {  // pinned or IK: their own colours
        if (!rig_) return false;
        if (pin_at(clip, *rig_, n, frame_) >= 0) return c = kPinned, true;
        const int l = rig_->limb_of_bone(n);
        if (l >= 0 && l < int(limb_states_.size()) && limb_states_[size_t(l)].ik_on) return c = kIk, true;
        return false;
    };
    const ImU32 accent = accent_colour(), canvas_bg = timeline_background();
    // Selected bones glow under everything.
    for (const PickerLine& l : scr.lines)
        if (selected(l.node)) {
            dl->AddLine(S(l.a), S(l.b), with_alpha(accent, 0.16f), 10 * u);
            dl->AddLine(S(l.a), S(l.b), with_alpha(accent, 0.3f), 6 * u);
        }
    for (const PickerLine& l : scr.lines) {
        const int n = l.node;
        ImU32 c;
        if (!node_visible(n) && !selected(n)) dashed(dl, S(l.a), S(l.b), kHiddenCol, 1.3f * u, 4 * u);
        else if (selected(n)) dl->AddLine(S(l.a), S(l.b), accent, 3 * u);
        else if (is_lit(n)) dl->AddLine(S(l.a), S(l.b), kWhite, 3 * u);
        else if (partner(n)) dashed(dl, S(l.a), S(l.b), accent, 1.8f * u, 4 * u);
        else if (state_col(n, c)) dl->AddLine(S(l.a), S(l.b), c, 2.4f * u);
        else dl->AddLine(S(l.a), S(l.b), kPlainLine, 1.6f * u);
    }
    // Dots: the last drawn on a spot shows a ring when another lies under it.
    for (size_t i = 0; i < scr.dots.size(); ++i) {
        const PickerDot& d = scr.dots[i];
        const int n = d.node;
        const ImVec2 p = S(d.p);
        bool stacked = false;
        for (size_t j = 0; j < i; ++j) stacked = stacked || (scr.dots[j].p - d.p).length() < 2 * u;
        if (stacked) dl->AddCircle(p, 6 * u, with_alpha(kPlainDot, 0.7f), 0, 1.3f * u);
        if (d.point) {
            const float r = 2.4f * u;
            const ImU32 c = selected(n) ? accent : is_lit(n) ? kWhite : skel_[n].volume ? kVolume : ui::kAttachment;
            dl->AddRectFilled(ImVec2(p.x - r - u, p.y - r - u), ImVec2(p.x + r + u, p.y + r + u), canvas_bg);
            dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), c);
            continue;
        }
        ImU32 c;
        if (is_lit(n)) dl->AddCircleFilled(p, 7 * u, with_alpha(kWhite, 0.22f));
        if (!node_visible(n) && !selected(n)) {
            dl->AddCircleFilled(p, 2.8f * u, canvas_bg);
            dl->AddCircle(p, 2.8f * u, kHiddenCol, 0, 1.2f * u);
        } else if (has_key_at(clip, skel_[n].name, frame_)) {
            diamond(dl, p, 4.6f * u, selected(n) ? accent : is_lit(n) ? kWhite : ui::kKey, canvas_bg, 1.3f * u);
        } else if (selected(n)) {
            dl->AddCircleFilled(p, 4 * u, accent);
            dl->AddCircle(p, 4 * u, canvas_bg, 0, 1.3f * u);
        } else if (is_lit(n)) {
            dl->AddCircleFilled(p, 3.8f * u, kWhite);
            dl->AddCircle(p, 3.8f * u, canvas_bg, 0, 1.3f * u);
        } else {
            const bool special = state_col(n, c);
            dl->AddCircleFilled(p, 3 * u, special ? c : kPlainDot);
            dl->AddCircle(p, 3 * u, canvas_bg, 0, 1.2f * u);
        }
        if (partner(n)) dashed_circle(dl, p, 5.5f * u, accent, 1.5f * u);
    }
    for (const PickerCap& c : scr.caps) {
        const ImVec2 p = S(c.tip);
        const float r = 4.2f * u;
        if (c.group == hover_group) dl->AddCircleFilled(p, r + 1.5f * u, with_alpha(kWhite, 0.9f));
        else if (group_selected(c.group)) dl->AddCircleFilled(p, r, accent);
        else dl->AddCircle(p, r, with_alpha(kPlainDot, 0.5f), 0, 1.4f * u);
    }
    ImGui::PushFont(bold_font(), st.FontSizeBase * 0.8f);  // label_fs before the UI scale
    const ImU32 label_plain = mix(ImGui::GetColorU32(ImGuiCol_TextDisabled), text_col, 0.3f);
    for (const PickerLabel& l : labels) {
        const bool on = group_selected(l.group), hov = l.group == hover_group, hidden = group_hidden(l.group);
        const ImVec2 a = S({l.r.x, l.r.y}), b = S({l.r.x + l.r.w, l.r.y + l.r.h});
        if (on || hov) dl->AddRectFilled(a, b, on ? with_alpha(accent, 0.18f) : with_alpha(kWhite, 0.09f), 4 * u);
        const ImU32 c = on ? accent : hov ? text_col : hidden ? with_alpha(label_plain, 0.55f) : label_plain;
        const std::string& text = groups[size_t(l.group)].label;
        const float tw = ImGui::CalcTextSize(text.c_str()).x;
        dl->AddText(ImVec2((a.x + b.x - tw) / 2, a.y + (b.y - a.y - label_fs) / 2), c, text.c_str());
    }
    if (rest_tag) {  // the avatar stands in its rest pose, not the pose on the timeline
        dl->AddRectFilled(S({tag.x, tag.y}), S({tag.x + tag.w, tag.y + tag.h}), ImGui::GetColorU32(ImGuiCol_FrameBg), 4 * u);
        dl->AddText(S({tag.x + 5 * u, tag.y + (tag.h - label_fs) / 2}), ImGui::GetColorU32(ImGuiCol_TextDisabled), "REST POSE");
    }
    ImGui::PopFont();

    // --- tooltips and clicks ---
    if (canvas_hovered && hover_group >= 0) {
        const PickerGroup& g = groups[size_t(hover_group)];
        const size_t count = group_nodes(hover_group).size();
        if (g.kind == PickerGroupKind::Finger)
            tip(g.name, "Click: all " + count_noun(count, "joint"), kModes);
        else
            tip(g.name + ": " + count_noun(count, "bone"), kModes,
                group_hidden(hover_group) ? "Hidden in Bones > Show: a click shows it" : "");
    } else if (canvas_hovered && hover_bone >= 0) {
        const std::string& name = skel_[hover_bone].name;
        std::string more;
        if (ranked.size() > 1) {
            const auto it = std::find(ui.cycle.ranked.begin(), ui.cycle.ranked.end(), hover_bone);
            const bool same = (m - ui.cycle.at).length() <= 4 * u && it != ui.cycle.ranked.end();
            const int next = same ? ui.cycle.ranked[size_t((it - ui.cycle.ranked.begin() + 1) % ui.cycle.ranked.size())]
                                  : ranked[1];
            more = "Click again: " + picker_bone_label(skel_[next].name);
        }
        if (!node_visible(hover_bone)) more += std::string(more.empty() ? "" : "\n") + "Hidden in Bones > Show: a click shows it";
        tip(picker_bone_label(name), skel_[hover_bone].attachment ? name + (skel_[hover_bone].volume ? ", collision volume"
                                                                                                     : ", attachment point")
                                                                  : name,
            more);
    }
    if (canvas_clicked) {
        if (hover_group >= 0) {
            select_group(hover_group);
            ui.cycle = {};
        } else if (!ranked.empty()) {
            const int n = ui.cycle.click(m, ranked, 4 * u);
            select_nodes({n}, mode);
            status(picker_bone_label(skel_[n].name) + (ranked.size() > 1 ? "  (click again for the one underneath)" : ""));
        } else if (mode == 0) {
            clear_selection();
            ui.cycle = {};
        }
    }

    // --- the selection, then Selection Sets folded to one line ---
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + ch + st.ItemSpacing.y));
    {
        std::string what, dim;
        const int prim = primary();
        for (int g = 0; g < int(groups.size()) && what.empty(); ++g) {
            const std::vector<int> nodes = group_nodes(g);
            if (nodes.size() > 1 && nodes.size() == selection_.size() && group_selected(g))
                what = groups[size_t(g)].name, dim = std::to_string(nodes.size()) + " bones";
        }
        if (what.empty() && prim >= 0) {
            if (selection_.size() > 1)  // several bones: how many, and the last picked (the one the gizmo turns)
                what = std::to_string(selection_.size()) + " bones", dim = "last " + picker_bone_label(skel_[prim].name);
            else
                what = picker_bone_label(skel_[prim].name);
        }
        if (mirror_live_ && !selection_.empty()) dim += dim.empty() ? "· Mirror on" : " · Mirror on";
        if (what.empty()) {
            ImGui::TextDisabled("Nothing selected");
        } else {
            ImGui::PushFont(bold_font(), 0);
            ImGui::TextUnformatted(what.c_str());
            ImGui::PopFont();
            if (!dim.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", dim.c_str());
            }
        }
    }
    draw_selection_sets();
    ImGui::End();
}

void App::draw_selection_sets() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& st = ImGui::GetStyle();
    const Clip& clip = doc_.clip();
    const std::vector<SelectionSet>& sets = clip.selection_sets;
    const bool open = section_header("Selection Sets###picker_sets", false);
    {  // the count, right-aligned on the header
        const std::string n = std::to_string(sets.size()) + (sets.size() == 1 ? " set" : " sets");
        const ImVec2 hi = ImGui::GetItemRectMax(), lo = ImGui::GetItemRectMin();
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(hi.x - ts.x - st.FramePadding.x * 2, (lo.y + hi.y - ts.y) / 2),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), n.c_str());
    }
    if (!open) return;
    std::vector<std::string> names;
    for (int n : selection_) names.push_back(skel_[n].name);
    auto recall = [&](const SelectionSet& s, bool mirrored) {
        std::vector<int> nodes = recall_selection_set(skel_, s);
        if (mirrored)
            for (int& n : nodes) n = skel_.mirror(n) >= 0 ? skel_.mirror(n) : n;
        if (nodes.empty()) return status("None of " + s.name + "'s bones are in this skeleton");
        select_nodes(nodes, io.KeyShift ? 1 : 0);
        status(s.name + (mirrored ? " mirrored: " : ": ") + std::to_string(selection_.size()) + " bone(s) selected");
    };
    auto bone_list = [](const SelectionSet& s) {
        std::string t;
        for (const std::string& b : s.bones) t += (t.empty() ? "" : ", ") + b;
        return t.empty() ? std::string("(no bones)") : t;
    };
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s", set_name_.c_str());
    const float save_w = ImGui::CalcTextSize("Save Set").x + 2 * st.FramePadding.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - save_w - st.ItemSpacing.x, 40.f));
    if (ImGui::InputTextWithHint("##set_name", "Set name", buf, sizeof buf)) set_name_ = buf;
    ImGui::SameLine();
    ImGui::BeginDisabled(names.empty() || set_name_.empty());
    if (ImGui::Button("Save Set")) {
        edit("Save Selection Set", [&](Clip& c) { store_selection_set(c.selection_sets, set_name_, names); });
        if (set_to_library_) store_selection_set(library_sets_, set_name_, names), save_library_sets();
        status("Saved " + set_name_ + ": " + std::to_string(names.size()) + " bone(s)");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", names.empty() ? "Select bones first" : "Save the selected bones under this name; "
                                                                        "a set of the same name is replaced");
    ImGui::Checkbox("Also save to the library", &set_to_library_);
    ImGui::SetItemTooltip("Library sets are kept beside the pose library, for every project");

    int remove = -1;
    if (sets.empty()) hint("No sets in this project.");
    for (int i = 0; i < int(sets.size()); ++i) {
        ImGui::PushID(i);
        const std::string label = sets[i].name + "  (" + std::to_string(sets[i].bones.size()) + ")";
        if (ImGui::Selectable(label.c_str())) recall(sets[i], false);
        ImGui::SetItemTooltip("%s\nClick to select, Shift+click to add, right-click to edit", bone_list(sets[i]).c_str());
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Select Mirrored")) recall(sets[i], true);
            if (ImGui::MenuItem("Add Selected Bones", nullptr, false, !names.empty()))
                edit("Add to Selection Set", [&](Clip& c) { edit_selection_set(c.selection_sets[i], names, true); });
            if (ImGui::MenuItem("Remove Selected Bones", nullptr, false, !names.empty()))
                edit("Remove from Selection Set", [&](Clip& c) { edit_selection_set(c.selection_sets[i], names, false); });
            if (ImGui::MenuItem("Save to Library")) {
                store_selection_set(library_sets_, sets[i].name, sets[i].bones);
                save_library_sets();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Set")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove >= 0) edit("Delete Selection Set", [&](Clip& c) { c.selection_sets.erase(c.selection_sets.begin() + remove); });

    if (!library_sets_.empty()) {
        ImGui::SeparatorText("Library");
        int drop = -1;
        for (int i = 0; i < int(library_sets_.size()); ++i) {
            ImGui::PushID(1000 + i);
            const SelectionSet& s = library_sets_[i];
            if (ImGui::Selectable((s.name + "  (" + std::to_string(s.bones.size()) + ")").c_str())) recall(s, false);
            ImGui::SetItemTooltip("%s\nClick to select, Shift+click to add, right-click for more", bone_list(s).c_str());
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Select Mirrored")) recall(s, true);
                if (ImGui::MenuItem("Add to Project"))
                    edit("Save Selection Set", [&](Clip& c) { store_selection_set(c.selection_sets, s.name, s.bones); });
                if (ImGui::MenuItem("Delete from Library")) drop = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (drop >= 0) {
            library_sets_.erase(library_sets_.begin() + drop);
            save_library_sets();
        }
    }
}

}  // namespace vats
