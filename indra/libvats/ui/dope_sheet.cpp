// Viewport Avatar Toolset - the dope sheet.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 17 (DS). The rows come from the core (dope_sheet.h); moving, scaling, tangents, delete and
// copy/paste are curve_ops and pose_ops on the graph editor's key selection.
#include "dope_sheet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui_internal.h"  // FindWindowByName: dock beside the graph the first time
#include "theme.h"

namespace vats {
namespace {

constexpr ImU32 kKeySelected = IM_COL32(255, 222, 70, 255);  // as the graph's
constexpr ImU32 kPin = IM_COL32(110, 190, 255, 255);          // as the graph's pin bands (TG-100)

ImU32 alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0.f, 1.f) * 255) << 24); }

double nice_step(double span, double px, double target_px) {  // 1, 2 or 5 x 10^n, as the graph's ruler
    double raw = span * target_px / std::max(px, 1.0);
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (p * m >= raw) return p * m;
    return p * 10;
}

void diamond(ImDrawList* dl, ImVec2 p, float r, bool sel) {
    const ImVec2 q[4] = {{p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}};
    dl->AddQuadFilled(q[0], q[1], q[2], q[3], sel ? kKeySelected : IM_COL32(20, 20, 22, 255));
    dl->AddQuad(q[0], q[1], q[2], q[3], sel ? kKeySelected : ui::kKey, 1.2f);
}

}  // namespace

float DopeSheet::x_of(double f) const {
    const auto& v = graph_.view_;
    return float(x0_ + (f - v.t0) / (v.t1 - v.t0) * w_);
}

double DopeSheet::f_at(float x) const {
    const auto& v = graph_.view_;
    return v.t0 + (x - x0_) / w_ * (v.t1 - v.t0);
}

// The tracks are the graph's: its items (IK parts folded into their track), plus every animated track in
// All animated bones.
std::vector<DopeSheet::Line> DopeSheet::lines(const GraphContext& ctx) const {
    std::vector<std::string> tracks;
    auto add = [&](const std::string& t) {
        if (std::find(tracks.begin(), tracks.end(), t) == tracks.end()) tracks.push_back(t);
    };
    for (const std::string& item : ctx.items) add(item.substr(0, item.find('#')));
    if (graph_.mode_ == GraphEditor::Mode::AllAnimated)
        for (const auto& [name, t] : ctx.clip.curves) add(name);
    else if (ctx.rig)  // a selected limb's IK target and pole keys go with its bones' (scaling a leg keeps its foot)
        tracks = with_limb_controls(*ctx.rig, ctx.clip, std::move(tracks));
    std::vector<Line> out{{"Summary", tracks, 0}};
    for (DopeRow& r : dope_rows(ctx.skel, tracks)) {
        out.push_back({r.label, r.tracks, 1});
        if (open_.count(r.label))
            for (const std::string& t : r.tracks) out.push_back({ctx.item_label ? ctx.item_label(t) : t, {t}, 2});
    }
    return out;
}

void DopeSheet::draw(GraphContext& ctx, const std::function<void(const std::vector<std::string>&)>& select_tracks) {
    Clip& clip = ctx.clip;
    ImGuiIO& io = ImGui::GetIO();
    std::vector<KeyRef>& sel = graph_.selection_;
    GraphEditor::View& view = graph_.view_;
    if (drag_ == Drag::None)  // undo or another editor may have taken keys away
        sel.erase(std::remove_if(sel.begin(), sel.end(),
                                 [&](const KeyRef& k) {
                                     auto t = clip.curves.find(k.track);
                                     if (t == clip.curves.end()) return true;
                                     auto c = t->second.find(k.channel);
                                     return c == t->second.end() || k.index < 0 || k.index >= int(c->second.keys.size());
                                 }),
                  sel.end());

    // Toolbar: the graph's own list mode and snap.
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(fs * 9);
    int mode = int(graph_.mode_);
    const char* modes[] = {"Selected bones", "All animated bones"};
    if (ImGui::Combo("##mode", &mode, modes, 2)) graph_.mode_ = GraphEditor::Mode(mode);
    ImGui::SameLine();
    ImGui::Checkbox("Snap frames", &graph_.snap_);
    ImGui::SetItemTooltip("Keep keys on whole frames while moving and scaling");

    const std::vector<Line> ls = lines(ctx);
    const float row = fs + 8, ruler = fs + 8, label_w = fs * 11;
    ImGui::BeginChild("##dope", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();  // scrolled content
    const float top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowHeight();
    const float avail_w = std::max(ImGui::GetContentRegionAvail().x, label_w + 50);
    x0_ = origin.x + label_w;
    w_ = avail_w - label_w;
    const float content_h = ruler + float(ls.size()) * row;
    ImGui::InvisibleButton("##sheet", ImVec2(avail_w, std::max(content_h, ImGui::GetContentRegionAvail().y)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || drag_ != Drag::None) hover_frame_ = ImGui::GetFrameCount();
    const ImVec2 m = io.MousePos;
    const int end = std::max(clip.end_frame, 1);
    const TimelineColours& tc = timeline_colours();
    auto line_top = [&](int i) { return origin.y + ruler + float(i) * row; };
    auto line_at = [&](float y) {
        const int i = int(std::floor((y - origin.y - ruler) / row));
        return i >= 0 && i < int(ls.size()) ? i : -1;
    };

    // Which frames of each track are selected, for drawing a row's key as selected.
    std::map<std::string, std::vector<double>> sel_frames;
    double smin = 1e30, smax = -1e30;
    for (const KeyRef& k : sel) {
        const double f = clip.curves.at(k.track).at(k.channel).keys[k.index].frame;
        sel_frames[k.track].push_back(f);
        smin = std::min(smin, f), smax = std::max(smax, f);
    }
    auto line_selected = [&](const Line& l, double f) {
        for (const std::string& t : l.tracks)
            if (auto it = sel_frames.find(t); it != sel_frames.end())
                for (double s : it->second)
                    if (same_frame(s, f)) return true;
        return false;
    };
    // ponytail: every row's keyed frames are gathered each frame; cache them per clip change if All animated on a
    // long mocap take gets slow.
    std::vector<std::vector<double>> frames;
    for (const Line& l : ls) frames.push_back(keyed_frames(clip, l.tracks));

    // Key area: background, outside the clip, the loop, the grid.
    const ImVec2 kmin(x0_, top), kmax(x0_ + w_, bottom);
    dl->PushClipRect(kmin, kmax, true);
    dl->AddRectFilled(kmin, kmax, tc.graph_bg);
    dl->AddRectFilled(kmin, ImVec2(x_of(0), bottom), IM_COL32(0, 0, 0, 40));
    dl->AddRectFilled(ImVec2(x_of(end), top), kmax, IM_COL32(0, 0, 0, 40));
    if (clip.loop) dl->AddRectFilled(ImVec2(x_of(clip.loop_in), top), ImVec2(x_of(clip.loop_out), bottom), ui::kLoop);
    const double tstep = std::max(1.0, nice_step(view.t1 - view.t0, w_, 70));
    for (double f = std::ceil(view.t0 / tstep) * tstep; f <= view.t1; f += tstep) {
        const bool major = std::fmod(std::fabs(f / tstep), 5.0) < 0.5;
        dl->AddLine(ImVec2(x_of(f), top), ImVec2(x_of(f), bottom), major ? tc.grid_major : tc.grid);
    }
    draw_beat_grid(dl, clip, [&](double f) { return x_of(f); }, view.t0, view.t1, top, bottom);  // AU-2, as the timeline
    dl->PopClipRect();

    // Rows: label, pin bands (TG-100), keys.
    for (int i = 0; i < int(ls.size()); ++i) {
        const Line& l = ls[i];
        const float y = line_top(i), mid = y + row / 2;
        if (y + row < top || y > bottom) continue;
        if (l.depth < 2) dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(x0_ + w_, y + row), IM_COL32(255, 255, 255, l.depth ? 8 : 16));
        dl->AddLine(ImVec2(origin.x, y + row), ImVec2(x0_ + w_, y + row), tc.grid);
        const float indent = origin.x + 6 + fs * (l.depth == 2 ? 1.6f : 0.f);
        if (l.depth == 1) {  // the expand arrow
            const float a = fs * 0.28f, cx = indent + a;
            if (open_.count(l.label)) dl->AddTriangleFilled(ImVec2(cx - a, mid - a * 0.6f), ImVec2(cx + a, mid - a * 0.6f), ImVec2(cx, mid + a * 0.7f), tc.text_dim);
            else dl->AddTriangleFilled(ImVec2(cx - a * 0.6f, mid - a), ImVec2(cx - a * 0.6f, mid + a), ImVec2(cx + a * 0.7f, mid), tc.text_dim);
        }
        dl->PushClipRect(ImVec2(origin.x, y), ImVec2(x0_ - 4, y + row), true);
        dl->AddText(ImVec2(indent + (l.depth == 1 ? fs : 0.f), mid - fs / 2),
                    l.depth == 2 ? tc.text_dim : ImGui::GetColorU32(ImGuiCol_Text), l.label.c_str());
        dl->PopClipRect();

        dl->PushClipRect(ImVec2(x0_, std::max(y, top)), ImVec2(x0_ + w_, y + row), true);
        for (const Pin& pin : clip.pins) {
            const bool shown = std::any_of(l.tracks.begin(), l.tracks.end(), [&](const std::string& t) {
                return t == pin.joint || t == "pin:" + pin.joint || t == pin.via || (!pin.target.empty() && t == pin.target);
            });
            if (!shown) continue;
            const double to = pin.to < 0 ? end : pin.to + 1;
            dl->AddRectFilled(ImVec2(x_of(pin.from), y + 1), ImVec2(x_of(to), y + row - 1), alpha(kPin, 0.14f));
            dl->AddLine(ImVec2(x_of(pin.from), y + 1), ImVec2(x_of(pin.from), y + row - 1), alpha(kPin, 0.63f));
        }
        float last_x = -1e9f;
        for (double f : frames[i]) {
            if (f < view.t0 - 1 || f > view.t1 + 1) continue;
            const bool s = line_selected(l, f);
            const float x = x_of(f);
            if (!s && x - last_x < 3) continue;  // covered by the key drawn before it
            last_x = x;
            diamond(dl, ImVec2(x, mid), l.depth == 0 ? fs * 0.36f : fs * 0.3f, s);
        }
        dl->PopClipRect();
    }

    // Scale handles either side of a selection spanning time.
    const bool span = sel.size() >= 2 && smax > smin && (drag_ == Drag::None || drag_ == Drag::Scale);
    const float hl = x_of(smin) - 8, hr = x_of(smax) + 8;
    const float hy0 = std::max(line_top(0), top + ruler), hy1 = std::min(line_top(int(ls.size())), bottom);
    if (span) {
        dl->PushClipRect(ImVec2(x0_, top + ruler), kmax, true);
        dl->AddRect(ImVec2(hl, hy0), ImVec2(hr, hy1), IM_COL32(255, 255, 255, 90));
        for (float x : {hl, hr}) dl->AddRectFilled(ImVec2(x - 3, hy0), ImVec2(x + 3, hy1), IM_COL32(235, 235, 235, 170), 2);
        dl->PopClipRect();
    }
    const bool in_ruler = m.y < top + ruler, in_keys = m.x >= x0_;
    auto on_handle = [&]() -> int {
        if (!span || in_ruler || !in_keys || m.y < hy0 || m.y > hy1) return 0;
        return std::fabs(m.x - hl) <= 5 ? -1 : std::fabs(m.x - hr) <= 5 ? 1 : 0;
    };
    // The key of row li under the pointer, nearest first.
    auto key_under = [&](int li, double& f) {
        double best = 7;
        for (double k : frames[li])
            if (double d = std::fabs(x_of(k) - m.x); d <= best) best = d, f = k;
        return best < 7;
    };

    if (hovered && drag_ == Drag::None) {
        if (on_handle()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (io.MouseWheel != 0) {  // zoom time about the pointer; Shift scrolls the rows
            if (io.KeyShift) {
                ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * row * 3);
            } else {
                const double k = std::pow(0.85, io.MouseWheel), fm = f_at(std::max(m.x, x0_));
                const double s = std::clamp((view.t1 - view.t0) * k, 0.5, 1e5), r = (fm - view.t0) / (view.t1 - view.t0);
                view.t0 = fm - s * r, view.t1 = view.t0 + s;
            }
        }
    }

    // Presses.
    const bool left = ImGui::IsMouseClicked(0), middle = ImGui::IsMouseClicked(2), right = ImGui::IsMouseClicked(1);
    if (hovered && drag_ == Drag::None && (left || middle || right)) {
        press_ = m;
        press_t0_ = view.t0, press_t1_ = view.t1;
        const int li = in_ruler ? -1 : line_at(m.y);
        const bool twice = left && ImGui::IsMouseDoubleClicked(0);
        double f = 0;
        const bool on_key = li >= 0 && in_keys && key_under(li, f);
        if (middle || (left && io.KeyAlt)) {
            drag_ = Drag::Pan;
        } else if (left && in_ruler) {
            drag_ = Drag::Scrub;
        } else if (!in_keys) {  // the labels: a click opens or closes a part, a double-click selects the row's bones
            if (left && li >= 0 && ls[li].depth == 1)
                open_.count(ls[li].label) ? (void)open_.erase(ls[li].label) : (void)open_.insert(ls[li].label);
            if (twice && li >= 0) select_tracks(ls[li].tracks);
        } else if (twice && li >= 0 && !on_key) {
            select_tracks(ls[li].tracks);
        } else if (int h = left ? on_handle() : 0) {
            drag_ = Drag::Scale;
            scale_pivot_ = h < 0 ? smax : smin;
            scale_w_ = h < 0 ? smin - smax : smax - smin;
            press_whole_ = same_frame(smin, 0) && same_frame(smax, clip.end_frame);  // Last frame and the loop go along
            press_clip_ = clip;
            press_sel_ = sel;
            ctx.history.begin(clip);
        } else if (on_key) {
            const std::vector<KeyRef> keys = keys_between(clip, ls[li].tracks, f, f);
            auto is_sel = [&](const KeyRef& k) { return std::find(sel.begin(), sel.end(), k) != sel.end(); };
            const bool all = std::all_of(keys.begin(), keys.end(), is_sel), any = std::any_of(keys.begin(), keys.end(), is_sel);
            auto add_all = [&] {
                for (const KeyRef& k : keys)
                    if (!is_sel(k)) sel.push_back(k);
            };
            auto remove_all = [&] { sel.erase(std::remove_if(sel.begin(), sel.end(), [&](const KeyRef& k) {
                                                  return std::find(keys.begin(), keys.end(), k) != keys.end();
                                              }),
                                              sel.end()); };
            if (right) {
                if (!any) sel = keys;
            } else if (io.KeyShift && io.KeyCtrl) {
                add_all();
            } else if (io.KeyShift) {
                all ? remove_all() : add_all();
            } else if (io.KeyCtrl) {
                remove_all();
            } else if (!any) {
                sel = keys;
            }
            if (left && std::any_of(keys.begin(), keys.end(), is_sel)) {
                drag_ = Drag::Move;  // starts moving after 3 px
                press_clip_ = clip;
                press_sel_ = sel;
            }
        } else if (left) {
            drag_ = Drag::Box;
        }
        if (right && in_keys && !in_ruler && !sel.empty()) ImGui::OpenPopup("##dope_key");
    }

    // Drags.
    if (drag_ != Drag::None) {
        const bool down = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2);
        const double df = (m.x - press_.x) / w_ * (press_t1_ - press_t0_);
        const bool moved = std::hypot(m.x - press_.x, m.y - press_.y) > 3;
        switch (drag_) {
            case Drag::Pan: view.t0 = press_t0_ - df, view.t1 = press_t1_ - df; break;
            case Drag::Scrub: ctx.frame = std::clamp(std::round(f_at(m.x)), 0.0, double(clip.end_frame)); break;
            case Drag::Move:
                if (!moved && !ctx.history.is_open()) break;
                if (!ctx.history.is_open()) ctx.history.begin(press_clip_);
                sel = press_sel_;
                move_keys(clip, press_clip_, sel, df, 0, graph_.snap_);
                break;
            case Drag::Scale: {
                // The dragged edge lands on a beat while Snap to Beats is on (AU-2), else on whole frames with Snap.
                const double edge = beat_snapped_frame(clip, scale_pivot_ + scale_w_ + df);
                const double sx = std::fabs(scale_w_) > 1e-9 ? (edge - scale_pivot_) / scale_w_ : 1;
                sel = press_sel_;
                scale_keys(clip, press_clip_, sel, scale_pivot_, 0, sx, 1, graph_.snap_);
                const bool whole = press_whole_ && sx > 0;  // reversed past the other handle: the length stays
                if (press_whole_) clip.end_frame = press_clip_.end_frame, clip.loop_in = press_clip_.loop_in, clip.loop_out = press_clip_.loop_out;
                if (whole) scale_length_with_keys(clip, press_clip_, scale_pivot_, sx);
                ImGui::SetTooltip(whole ? "Scale to %.0f frames: Last frame and the loop go along" : "Edge at frame %.1f",
                                  whole ? double(clip.end_frame) : edge);
                break;
            }
            case Drag::Box:
                if (moved) {
                    const ImVec2 a(std::min(m.x, press_.x), std::min(m.y, press_.y)), b(std::max(m.x, press_.x), std::max(m.y, press_.y));
                    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18));
                    dl->AddRect(a, b, IM_COL32(255, 255, 255, 110));
                }
                break;
            case Drag::None: break;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // Escape cancels the drag (spec README, UX 1)
            if (ctx.history.is_open() && (drag_ == Drag::Move || drag_ == Drag::Scale)) {
                clip = ctx.history.cancel();
                sel = press_sel_;
            }
            if (drag_ == Drag::Pan) view.t0 = press_t0_, view.t1 = press_t1_;
            drag_ = Drag::None;
            graph_.escape_used_ = true;
        } else if (!down) {  // release
            if (drag_ == Drag::Move && ctx.history.is_open()) {
                finish_transform(clip, sel);
                if (ctx.history.commit("Move Keys", clip)) ctx.changed();
            } else if (drag_ == Drag::Scale) {
                finish_transform(clip, sel);
                if (ctx.history.commit("Scale Keys", clip)) ctx.changed();
            } else if (drag_ == Drag::Box) {
                if (!io.KeyShift && !io.KeyCtrl) sel.clear();
                if (moved) {
                    const float y0 = std::min(m.y, press_.y), y1 = std::max(m.y, press_.y);
                    const double f0 = f_at(std::min(m.x, press_.x)), f1 = f_at(std::max(m.x, press_.x));
                    for (int i = 0; i < int(ls.size()); ++i) {
                        if (line_top(i) > y1 || line_top(i) + row < y0) continue;
                        for (const KeyRef& k : keys_between(clip, ls[i].tracks, f0, f1)) {
                            auto it = std::find(sel.begin(), sel.end(), k);
                            if (io.KeyCtrl && !io.KeyShift) {
                                if (it != sel.end()) sel.erase(it);
                            } else if (it == sel.end()) {
                                sel.push_back(k);
                            }
                        }
                    }
                }
            }
            drag_ = Drag::None;
        }
    }

    // Hover label on a key.
    if (hovered && drag_ == Drag::None && !in_ruler && in_keys)
        if (int li = line_at(m.y); li >= 0) {
            double f = 0;
            if (key_under(li, f))
                ImGui::SetTooltip("%s \xC2\xB7 frame %.0f \xC2\xB7 %d key(s)", ls[li].label.c_str(), f,
                                  int(keys_between(clip, ls[li].tracks, f, f).size()));
        }

    // The ruler stays at the top while the rows scroll; the playhead runs through everything.
    dl->AddRectFilled(ImVec2(origin.x, top), ImVec2(x0_ + w_, top + ruler), tc.ruler);
    dl->PushClipRect(ImVec2(x0_, top), kmax, true);
    for (double f = std::ceil(view.t0 / tstep) * tstep; f <= view.t1; f += tstep) {
        char b[16];
        std::snprintf(b, sizeof b, "%.0f", std::fabs(f) < 1e-9 ? 0.0 : f);  // never "-0"
        dl->AddLine(ImVec2(x_of(f), top + ruler - 5), ImVec2(x_of(f), top + ruler), tc.text_dim);
        dl->AddText(ImVec2(x_of(f) + 3, top + 3), tc.text_dim, b);
    }
    const double at = std::floor(ctx.frame + 1e-9);  // whole frames, as the graph and timeline show (TG-11)
    const float px = x_of(at);
    dl->AddLine(ImVec2(px, top), ImVec2(px, bottom), ui::kPlayhead, 2);
    dl->AddRectFilled(ImVec2(px - 14, top + 2), ImVec2(px + 14, top + ruler - 2), ui::kPlayhead, 3);
    char t[16];
    std::snprintf(t, sizeof t, "%d", int(at));
    dl->AddText(ImVec2(px - ImGui::CalcTextSize(t).x / 2, top + ruler / 2 - fs / 2), IM_COL32(20, 22, 26, 255), t);
    dl->PopClipRect();
    if (ls.size() == 1 && ls[0].tracks.empty()) {
        const char* hint = "Select a bone to see its keys";
        dl->AddText(ImVec2(x0_ + (w_ - ImGui::CalcTextSize(hint).x) / 2, top + ruler + row * 1.5f), tc.text_dim, hint);
    }

    // Right-click: tangents and delete for the selected keys (AM-22).
    if (ImGui::BeginPopup("##dope_key")) {
        struct T {
            const char* label;
            Tangent t;
        };
        static const T tangents[] = {{"Auto", Tangent::Auto},     {"Spline", Tangent::Spline}, {"Plateau", Tangent::Plateau},
                                     {"Linear", Tangent::Linear}, {"Flat", Tangent::Flat},     {"Stepped", Tangent::Stepped}};
        ImGui::TextDisabled("%d key(s)", int(sel.size()));
        for (const T& tg : tangents)
            if (ImGui::MenuItem(tg.label)) {
                const std::vector<KeyRef> keys = sel;
                graph_.edit(ctx, tg.label, [&](Clip& c) { apply_tangent(c, keys, tg.t); });
            }
        ImGui::Separator();
        if (menu_item_icon(icon::kDelete, "Delete Keys")) graph_.delete_selected(ctx);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------
// The panel

void App::draw_dope_panel() {
    if (!show_dope_) return;
    if (ImGuiWindow* g = ImGui::FindWindowByName("Graph"); g && g->DockId)  // a layout from before the dope sheet
        ImGui::SetNextWindowDockID(g->DockId, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Dope Sheet", &show_dope_)) {
        GraphContext g = graph_context();
        dope_.draw(g, [this](const std::vector<std::string>& tracks) {
            clear_selection();
            for (const std::string& t : tracks) {
                const std::string name = t.rfind("pin:", 0) == 0 ? t.substr(4) : t;
                if (name.rfind("ik.", 0) == 0) {
                    if (int l = rig_->find_limb(name.substr(3)); l >= 0 && std::find(handles_.begin(), handles_.end(), HandleRef{l, false}) == handles_.end())
                        select_handle({l, false}, true);
                } else if (int n = skel_.find(name); n >= 0 && std::find(selection_.begin(), selection_.end(), n) == selection_.end()) {
                    select(n, true);
                }
            }
            status(std::to_string(selection_.size() + handles_.size()) + " selected from the dope sheet");
        });
        if (graph_.take_escape()) skip_shortcuts_ = true;  // Esc cancelled a drag; keep the selection
    }
    ImGui::End();
}

}  // namespace vats
