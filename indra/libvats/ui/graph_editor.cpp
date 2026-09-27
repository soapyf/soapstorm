// Viewport Avatar Toolset - the graph editor: curves of the selected bones, key editing.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "graph_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "icon_button.h"
#include "icons.h"
#include "theme.h"

namespace vats {
namespace {

constexpr ImU32 kAxis[4] = {IM_COL32(242, 89, 89, 255), IM_COL32(115, 230, 102, 255), IM_COL32(102, 153, 255, 255),
                            IM_COL32(230, 230, 230, 255)};
constexpr ImU32 kKeySelected = IM_COL32(255, 222, 70, 255);
constexpr ImU32 kHandle = IM_COL32(214, 180, 130, 255), kHandleFree = IM_COL32(110, 215, 235, 255);
constexpr float kPickPx = 7;

ImU32 lighten(ImU32 c, float t) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x + (1 - v.x) * t, v.y + (1 - v.y) * t, v.z + (1 - v.z) * t, v.w));
}
ImU32 alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0.f, 1.f) * 255) << 24); }

// A "nice" step (1, 2 or 5 x 10^n) close to span * target_px / px.
double nice_step(double span, double px, double target_px) {
    double raw = span * target_px / std::max(px, 1.0);
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (p * m >= raw) return p * m;
    return p * 10;
}

std::string format_value(double v) {
    char b[32];
    if (std::fabs(v) >= 10 || v == std::floor(v)) std::snprintf(b, sizeof b, "%.0f", v);
    else if (std::fabs(v) < 1) std::snprintf(b, sizeof b, "%.2f", v);
    else std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

double distance(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Channels

void GraphEditor::build_channels(const GraphContext& ctx) {
    std::vector<std::string> items = ctx.items;
    auto track_of = [](const std::string& item) { return item.substr(0, item.find('#')); };  // "ik.ArmLeft#pole"
    if (mode_ == Mode::AllAnimated)
        for (auto& [name, track] : ctx.clip.curves)
            if (std::none_of(items.begin(), items.end(), [&](const std::string& i) { return track_of(i) == name; }))
                items.push_back(name);
    if (items == item_names_) return;
    item_names_ = items;
    channels_.clear();
    row_filter_.clear();
    static const char* axes = "XYZ";
    for (int it = 0; it < int(items.size()); ++it) {
        const std::string name = track_of(items[it]);
        // An IK control's part (TG-41): "#target" shows blend, translate and rotate, "#pole" only the pole.
        const std::string part = items[it].size() > name.size() ? items[it].substr(name.size() + 1) : "";
        auto add = [&](const char* ch, std::string label, ImU32 colour, bool pole = false) {
            if (part == "pole" ? !pole : part == "target" && pole) return;
            channels_.push_back({name, ch, std::move(label), colour, pole, it});
        };
        bool ik = name.rfind("ik.", 0) == 0, pin = name.rfind("pin:", 0) == 0;
        int node = ik || pin ? -1 : ctx.skel.find(name);
        bool pos = ik || pin || name == "mPelvis" || (node >= 0 && ctx.skel[node].attachment) ||
                   ctx.clip.has_channels(name, kPosChannels);
        if (ik) add("blend", "IK / FK Blend", kAxis[3]);
        if (ik && pos)
            for (int a = 0; a < 3; ++a) add(kPosChannels[a], std::string("Translate ") + axes[a], kAxis[a]);
        for (int a = 0; a < 3; ++a) add(kRotChannels[a], std::string("Rotate ") + axes[a], kAxis[a]);
        if (!ik && pos)
            for (int a = 0; a < 3; ++a) add(kPosChannels[a], std::string("Translate ") + axes[a], kAxis[a]);
        if (ik && name != "ik.Spine")
            for (int a = 0; a < 3; ++a) {
                static const char* poles[] = {"pole_x", "pole_y", "pole_z"};
                add(poles[a], std::string("Pole ") + axes[a], lighten(kAxis[a], 0.45f), true);
            }
    }
    active_ = -1;
    reconcile(ctx.clip);
    fit_pending_ = true;  // TG-63: refit values once the new set is drawn
}

std::vector<int> GraphEditor::shown_channels() const {
    std::vector<int> out;
    for (int c = 0; c < int(channels_.size()); ++c) {
        if (row_filter_.empty()) {
            out.push_back(c);
            continue;
        }
        int slot = 0;
        for (int k = 0; k < c; ++k) slot += channels_[k].item == channels_[c].item;
        for (int r : row_filter_)
            if (r == channels_[c].item * 16 + 15 || r == channels_[c].item * 16 + slot) {
                out.push_back(c);
                break;
            }
    }
    return out;
}

const FCurve* GraphEditor::curve(const Clip& clip, const Channel& c) const {
    auto t = clip.curves.find(c.track);
    if (t == clip.curves.end()) return nullptr;
    auto ch = t->second.find(c.channel);
    return ch == t->second.end() ? nullptr : &ch->second;
}

bool GraphEditor::selected(const Channel& c, int key) const {
    for (auto& s : selection_)
        if (s.index == key && s.track == c.track && s.channel == c.channel) return true;
    return false;
}

void GraphEditor::reconcile(const Clip& clip) {
    std::vector<int> shown = shown_channels();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [&](const KeyRef& k) {
                                        for (int c : shown) {
                                            const Channel& ch = channels_[c];
                                            if (ch.track != k.track || ch.channel != k.channel) continue;
                                            const FCurve* cv = curve(clip, ch);
                                            return !cv || k.index < 0 || k.index >= int(cv->keys.size());
                                        }
                                        return true;
                                    }),
                     selection_.end());
    if (active_ >= 0 && std::find(shown.begin(), shown.end(), active_) == shown.end()) active_ = -1;
}

void GraphEditor::edit(GraphContext& ctx, const char* label, const std::function<void(Clip&)>& change) {
    ctx.history.begin(ctx.clip);
    change(ctx.clip);
    if (ctx.history.commit(label, ctx.clip)) ctx.changed();
}

// ---------------------------------------------------------------------------------------------
// Framing

void GraphEditor::fit_bounds(double f0, double f1, double v0, double v1, bool) {
    if (f1 - f0 < 4) f0 -= 2, f1 += 2;
    if (v1 - v0 < 2) v0 -= 5, v1 += 5;
    double pf = (f1 - f0) * 0.08, pv = (v1 - v0) * 0.08;
    view_ = {f0 - pf, f1 + pf, v0 - pv, v1 + pv};
}

void GraphEditor::frame_all(const GraphContext& ctx) {
    double f0 = 1e30, f1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (int c : shown_channels())
        if (const FCurve* cv = curve(ctx.clip, channels_[c]))
            for (auto& k : cv->keys) {
                f0 = std::min({f0, k.frame, k.lx}), f1 = std::max({f1, k.frame, k.rx});
                v0 = std::min({v0, k.value, k.ly, k.ry}), v1 = std::max({v1, k.value, k.ly, k.ry});
            }
    if (f0 > f1) fit_bounds(0, std::max(ctx.clip.end_frame, 1), -45, 45, true);
    else fit_bounds(f0, f1, v0, v1, true);
}

void GraphEditor::frame_selected(const GraphContext& ctx) {
    if (selection_.empty()) return frame_all(ctx);
    double f0 = 1e30, f1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (auto& s : selection_) {
        const Key& k = ctx.clip.curves.at(s.track).at(s.channel).keys[s.index];
        f0 = std::min(f0, k.frame), f1 = std::max(f1, k.frame), v0 = std::min(v0, k.value), v1 = std::max(v1, k.value);
    }
    fit_bounds(f0, f1, v0, v1, true);
}

void GraphEditor::fit_values(const GraphContext& ctx) {
    double a = std::max(view_.t0, 0.0), b = std::min(view_.t1, double(std::max(ctx.clip.end_frame, 1)));
    if (b <= a) a = view_.t0, b = view_.t1;
    double lo = 1e30, hi = -1e30;
    for (int c : shown_channels()) {
        const FCurve* cv = curve(ctx.clip, channels_[c]);
        if (!cv || cv->empty()) continue;
        for (int i = 0; i <= 120; ++i) {
            double v = cv->evaluate(a + (b - a) * i / 120);
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        for (auto& k : cv->keys)
            if (k.frame >= a && k.frame <= b) lo = std::min(lo, k.value), hi = std::max(hi, k.value);
    }
    if (lo > hi) return;
    double mid = (lo + hi) / 2, span = std::max(hi - lo, 0.5) * 1.24;
    view_.v0 = mid - span / 2;
    view_.v1 = mid + span / 2;
}

// ---------------------------------------------------------------------------------------------
// Commands

bool GraphEditor::key_span(const Clip& clip, double& a, double& b) const {
    if (selection_.empty()) return false;
    a = 1e30, b = -1e30;
    for (auto& s : selection_) {
        double f = clip.curves.at(s.track).at(s.channel).keys[s.index].frame;
        a = std::min(a, f), b = std::max(b, f);
    }
    a = std::round(a), b = std::round(b);
    return b > a;
}

bool GraphEditor::delete_selected(GraphContext& ctx) {
    if (selection_.empty() && pin_sel_ >= 0 && pin_sel_ < int(ctx.clip.pins.size()) && ctx.rig) {  // TG-85
        std::string joint = ctx.clip.pins[pin_sel_].joint;
        size_t pin = size_t(pin_sel_);
        edit(ctx, "Delete Pin", [&](Clip& c) { delete_pin(c, *ctx.rig, pin); });
        pin_sel_ = -1;
        ctx.status("Deleted the pin on " + joint);
        return true;
    }
    if (selection_.empty()) {
        ctx.status("No keys selected in the graph");
        return false;
    }
    auto sel = selection_;
    edit(ctx, "Delete Keys", [&](Clip& c) { delete_keys(c, sel); });
    ctx.status("Deleted " + std::to_string(sel.size()) + " key(s)");
    selection_.clear();
    return true;
}

void GraphEditor::copy_keys(GraphContext& ctx) {
    if (selection_.empty()) return ctx.status("Select keys in the graph to copy");
    double first = 1e30;
    for (auto& s : selection_) first = std::min(first, ctx.clip.curves.at(s.track).at(s.channel).keys[s.index].frame);
    clipboard_.clear();
    for (auto& s : selection_) {
        const Key& k = ctx.clip.curves.at(s.track).at(s.channel).keys[s.index];
        clipboard_.push_back({s.track, s.channel, k.frame - first, k.value, k.interp});
    }
    ctx.status("Copied " + std::to_string(clipboard_.size()) + " key(s)");
}

void GraphEditor::paste_keys(GraphContext& ctx) {
    if (clipboard_.empty()) return ctx.status("Nothing copied in the graph");
    std::vector<KeyRef> pasted;
    edit(ctx, "Paste Keys", [&](Clip& c) {
        for (auto& k : clipboard_) c.curves[k.track][k.channel].set_key(ctx.frame + k.offset, k.value, k.interp);
        for (auto& k : clipboard_) {
            const FCurve& cv = c.curves[k.track][k.channel];
            pasted.push_back({k.track, k.channel, cv.find(ctx.frame + k.offset)});
        }
    });
    selection_ = pasted;
    ctx.status("Pasted " + std::to_string(pasted.size()) + " key(s) at frame " + std::to_string(int(ctx.frame)));
}

// ---------------------------------------------------------------------------------------------
// Drawing

void GraphEditor::draw(GraphContext& ctx) {
    build_channels(ctx);
    reconcile(ctx.clip);
    if (pin_sel_ >= int(ctx.clip.pins.size())) pin_sel_ = -1;
    if (!framed_once_ && !channels_.empty()) {
        frame_all(ctx);
        framed_once_ = true;
        fit_pending_ = false;
    }
    draw_toolbar(ctx);
    const float list_w = ImGui::GetFontSize() * 11;
    ImGui::BeginChild("##channels", ImVec2(list_w, 0), ImGuiChildFlags_ResizeX);
    draw_channel_list(ctx);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##canvas", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_canvas(ctx);
    ImGui::EndChild();
    if (fit_pending_ && drag_ == Drag::None) {  // deferred so the new channel set is measured (pitfall 13)
        fit_values(ctx);
        fit_pending_ = false;
    }
}

void GraphEditor::draw_toolbar(GraphContext& ctx) {
    // Buttons wrap onto another line when the editor is narrow.
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool first = true;
    auto place = [&](float width) {
        if (!first) {
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + width > right) ImGui::NewLine();
        }
        first = false;
    };
    // Icon only; the tooltip names the button, its key and what it does.
    auto button = [&](const char* id, const char* icon, const std::string& tip) {
        place(icon_button_width());
        return icon_button(id, icon, tip);
    };
    auto need_keys = [&]() {
        if (!selection_.empty()) return true;
        ctx.status("Select keys in the graph first");
        return false;
    };
    auto sep = [&] {
        place(8);
        ImGui::TextDisabled("|");
    };

    place(ImGui::GetFontSize() * 9);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    int m = int(mode_);
    const char* modes[] = {"Selected bones", "All animated bones"};
    if (ImGui::Combo("##mode", &m, modes, 2)) mode_ = Mode(m);
    sep();
    // TG-31: the tooltip carries the preset's shortcut.
    auto with_key = [&](const char* label, const char* id) {
        std::string k = ctx.key_name ? ctx.key_name(id) : "";
        return std::string(label) + (k.empty() ? "" : " (" + k + ")");
    };
    if (button("frame_all", icon::kFrameAll, with_key("Frame All", "frame_all") + ": fit every shown curve")) frame_all(ctx);
    if (button("frame_selected", icon::kFrameSelected, with_key("Frame Selected", "frame_selected") + ": fit the selected keys"))
        frame_selected(ctx);
    sep();
    struct T {
        const char* label;
        Tangent t;
        CurveIcon icon;
        const char* tip;
    };
    static const T tangents[] = {
        {"Auto", Tangent::Auto, CurveIcon::Auto, "Auto: smooth, flat at peaks and valleys"},
        {"Spline", Tangent::Spline, CurveIcon::Spline, "Spline: smooth through the neighbours; can overshoot"},
        {"Plateau", Tangent::Plateau, CurveIcon::Plateau, "Plateau: smooth without ever overshooting"},
        {"Linear", Tangent::Linear, CurveIcon::Linear, "Linear: straight towards the neighbouring keys"},
        {"Flat", Tangent::Flat, CurveIcon::Flat, "Flat: level handles"},
        {"Stepped", Tangent::Stepped, CurveIcon::Stepped, "Stepped: hold the value until the next key"},
        {"Break", Tangent::Break, CurveIcon::Break, "Break: move each handle on its own"},
        {"Unify", Tangent::Unify, CurveIcon::Unify, "Unify: line both handles up again"},
    };
    for (auto& t : tangents) {
        place(icon_button_width());
        if (curve_icon_button(t.label, t.icon, t.tip) && need_keys()) {
            auto sel = selection_;
            edit(ctx, t.label, [&](Clip& c) { apply_tangent(c, sel, t.t); });
        }
    }
    sep();
    if (button("fit_values", icon::kFitValues, "Fit Values: fit the value range to the visible curves")) fit_values(ctx);
    if (button("euler_filter", icon::kEulerFilter, "Euler Filter: remove 360-degree jumps from rotation curves")) {
        std::vector<std::string> tracks;
        for (int c : shown_channels())
            if (channels_[c].channel.rfind("rot_", 0) == 0 &&
                std::find(tracks.begin(), tracks.end(), channels_[c].track) == tracks.end())
                tracks.push_back(channels_[c].track);
        if (tracks.empty()) {
            ctx.status("Select a bone with rotation curves first");
        } else {
            int n = 0;
            edit(ctx, "Euler Filter", [&](Clip& c) { n = euler_filter(c, tracks); });
            ctx.status(n ? "Euler filter fixed " + std::to_string(n) + " bone(s)" : "Rotation curves are already clean");
        }
    }
    if (button("flip_time", icon::kFlipTime, "Flip Time: mirror the selected keys in time") && need_keys()) {
        auto sel = selection_;
        edit(ctx, "Flip Time", [&](Clip& c) { flip_time(c, sel, snap_); });
        selection_ = sel;
    }
    if (button("flip_values", icon::kFlipValues, "Flip Values: mirror the selected keys across zero") && need_keys()) {
        auto sel = selection_;
        edit(ctx, "Flip Values", [&](Clip& c) { flip_values(c, sel); });
        selection_ = sel;
    }
    if (button("delete", icon::kDelete, "Delete (Delete): delete the selected keys")) delete_selected(ctx);
    sep();
    place(ImGui::GetFontSize() * 6);
    ImGui::Checkbox("Snap frames", &snap_);
    ImGui::SetItemTooltip("Keep keys on whole frames while moving and scaling");

    // Frame and Value boxes: show the earliest selected key; editing moves the whole selection.
    int ref = -1;
    for (int i = 0; i < int(selection_.size()); ++i) {
        const auto& s = selection_[i];
        double f = ctx.clip.curves.at(s.track).at(s.channel).keys[s.index].frame;
        if (ref < 0) ref = i;
        else {
            const auto& r = selection_[ref];
            if (f < ctx.clip.curves.at(r.track).at(r.channel).keys[r.index].frame) ref = i;
        }
    }
    ImGui::BeginDisabled(ref < 0);
    double rf = 0, rv = 0;
    if (ref >= 0) {
        const auto& r = selection_[ref];
        const Key& k = ctx.clip.curves.at(r.track).at(r.channel).keys[r.index];
        rf = k.frame, rv = k.value;
    }
    auto box = [&](const char* label, const char* id, double& v, const char* fmt) {
        place(ImGui::GetFontSize() * 8);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        double before = v;
        ImGui::InputDouble(id, &v, 0, 0, fmt);
        v = std::clamp(v, -100000.0, 100000.0);  // TG-87
        if (selection_.size() > 1) ImGui::SetItemTooltip("Shows the earliest selected key; editing moves them all");
        return ImGui::IsItemDeactivatedAfterEdit() && v != before;
    };
    double nf = rf, nv = rv;
    bool moved_f = box("Frame", "##kf", nf, "%.2f"), moved_v = box("Value", "##kv", nv, "%.3f");
    if (ref >= 0 && (moved_f || moved_v)) {
        double df = moved_f ? nf - rf : 0, dv = moved_v ? nv - rv : 0;
        if (snap_) df = std::round(df);
        df = std::max(df, -rf);  // the earliest key stops at frame 0
        auto sel = selection_;
        edit(ctx, "Move Keys", [&](Clip& c) {
            Clip before = c;
            move_keys(c, before, sel, df, dv, false);
            finish_transform(c, sel);
        });
        selection_ = sel;
    }
    ImGui::EndDisabled();
}

void GraphEditor::draw_channel_list(GraphContext& ctx) {
    ImGuiIO& io = ImGui::GetIO();
    for (int it = 0; it < int(item_names_.size()); ++it) {
        std::string label = ctx.item_label ? ctx.item_label(item_names_[it]) : item_names_[it];
        int row = it * 16 + 15;
        bool sel = std::find(row_filter_.begin(), row_filter_.end(), row) != row_filter_.end();
        auto click = [&](int r, bool is_sel) {
            if (io.KeyCtrl || io.KeyShift) {
                if (is_sel) row_filter_.erase(std::find(row_filter_.begin(), row_filter_.end(), r));
                else row_filter_.push_back(r);
            } else {
                row_filter_.assign(1, r);
            }
            fit_pending_ = true;
            reconcile(ctx.clip);
        };
        ImGui::PushID(it);
        if (ImGui::Selectable(label.c_str(), sel)) click(row, sel);
        int slot = 0;
        for (int c = 0; c < int(channels_.size()); ++c) {
            if (channels_[c].item != it) continue;
            int r = it * 16 + slot++;
            bool csel = std::find(row_filter_.begin(), row_filter_.end(), r) != row_filter_.end();
            ImGui::Indent(12);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(channels_[c].colour));
            ImGui::PushID(c);
            if (ImGui::Selectable(channels_[c].label.c_str(), csel)) click(r, csel);
            ImGui::PopID();
            ImGui::PopStyleColor();
            ImGui::Unindent(12);
        }
        ImGui::PopID();
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
        row_filter_.clear();
        fit_pending_ = true;
    }
}

void GraphEditor::draw_canvas(GraphContext& ctx) {
    Clip& clip = ctx.clip;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    canvas_min_ = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 50.f);
    size.y = std::max(size.y, 60.f);
    canvas_w_ = size.x;
    plot_h_ = size.y - ruler_;
    ImVec2 cmax(canvas_min_.x + size.x, canvas_min_.y + size.y);
    ImGui::InvisibleButton("##graph", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    hovered_ = ImGui::IsItemHovered();
    const int end = std::max(clip.end_frame, 1);
    const ImVec2 m = io.MousePos;
    const float plot_top = canvas_min_.y + ruler_;

    // Background layers (TG-55).
    dl->PushClipRect(canvas_min_, cmax, true);
    const TimelineColours& tc = timeline_colours();
    dl->AddRectFilled(canvas_min_, cmax, tc.graph_bg);
    dl->AddRectFilled(ImVec2(canvas_min_.x, plot_top), ImVec2(x_of(0), cmax.y), IM_COL32(0, 0, 0, 40));
    dl->AddRectFilled(ImVec2(x_of(end), plot_top), ImVec2(cmax.x, cmax.y), IM_COL32(0, 0, 0, 40));
    if (clip.loop)
        dl->AddRectFilled(ImVec2(x_of(clip.loop_in), plot_top), ImVec2(x_of(clip.loop_out), cmax.y), ui::kLoop);
    // Pin bands (TG-100) for pins on displayed bones.
    auto pin_shown = [&](const Pin& pin) {
        for (auto& n : item_names_)
            if (n == pin.joint || n == "pin:" + pin.joint || n == pin.via || (!pin.target.empty() && n == pin.target))
                return true;
        return false;
    };
    // Where a pin's markers are drawn: its start, and its release frame (or -1 when held to the end).
    auto pin_marks = [&](int i, int& start, int& release) {
        const Pin& pin = clip.pins[i];
        start = pin.from, release = pin.to < 0 ? -1 : pin.to + 1;
        if (drag_ == Drag::Pin && i == pin_sel_) (pin_end_ ? release : start) = pin_frame_;
        if (release > clip.end_frame) release = -1;
    };
    for (int i = 0; i < int(clip.pins.size()); ++i) {
        if (!pin_shown(clip.pins[i])) continue;
        int start, release;
        pin_marks(i, start, release);
        bool sel = i == pin_sel_;
        ImU32 tint = sel ? IM_COL32(255, 222, 70, 255) : IM_COL32(110, 190, 255, 255);
        double to = release < 0 ? end : release;
        dl->AddRectFilled(ImVec2(x_of(start), plot_top), ImVec2(x_of(to), cmax.y), alpha(tint, sel ? 0.16f : 0.11f));
        dl->AddLine(ImVec2(x_of(start), plot_top), ImVec2(x_of(start), cmax.y), alpha(tint, 0.63f));
        if (release >= 0) dl->AddLine(ImVec2(x_of(release), plot_top), ImVec2(x_of(release), cmax.y), alpha(tint, 0.32f));
    }
    // Grid; the time labels go on the ruler, drawn after it.
    double tstep = std::max(1.0, nice_step(view_.t1 - view_.t0, size.x, 70));
    for (double f = std::ceil(view_.t0 / tstep) * tstep; f <= view_.t1; f += tstep) {
        bool major = std::fmod(std::fabs(f / tstep), 5.0) < 0.5;
        dl->AddLine(ImVec2(x_of(f), plot_top), ImVec2(x_of(f), cmax.y), major ? tc.grid_major : tc.grid);
    }
    double vstep = nice_step(view_.v1 - view_.v0, plot_h_, 40);
    for (double v = std::ceil(view_.v0 / vstep) * vstep; v <= view_.v1; v += vstep) {
        bool zero = std::fabs(v) < vstep * 1e-6;
        dl->AddLine(ImVec2(canvas_min_.x, y_of(v)), ImVec2(cmax.x, y_of(v)), zero ? tc.zero : tc.grid);
        const float ty = y_of(v) - ImGui::GetFontSize() - 1;  // above its line, never under the ruler
        if (ty >= plot_top) dl->AddText(ImVec2(canvas_min_.x + 4, ty), tc.text_dim, format_value(zero ? 0 : v).c_str());
    }
    dl->AddRectFilled(canvas_min_, ImVec2(cmax.x, plot_top), tc.ruler);
    for (double f = std::ceil(view_.t0 / tstep) * tstep; f <= view_.t1; f += tstep) {
        char b[16];
        std::snprintf(b, sizeof b, "%.0f", f);
        dl->AddLine(ImVec2(x_of(f), plot_top - 5), ImVec2(x_of(f), plot_top), tc.text_dim);
        dl->AddText(ImVec2(x_of(f) + 3, canvas_min_.y + 3), tc.text_dim, b);
    }
    // Pin markers in the ruler (TG-101): a filled diamond at the start, a hollow one at the release.
    struct PinHit {
        int pin;
        bool release;
    };
    std::vector<std::pair<ImVec2, PinHit>> pin_hits;  // release markers first, so they win the hit test
    for (int i = 0; i < int(clip.pins.size()); ++i) {
        if (!pin_shown(clip.pins[i])) continue;
        const Pin& pin = clip.pins[i];
        int start, release;
        pin_marks(i, start, release);
        ImU32 c = i == pin_sel_ ? kKeySelected : IM_COL32(110, 190, 255, 255);
        float y = plot_top - 6, r = 5;
        ImVec2 s(x_of(start), y);
        dl->AddQuadFilled(ImVec2(s.x, y - r), ImVec2(s.x + r, y), ImVec2(s.x, y + r), ImVec2(s.x - r, y), c);
        std::string label = pin.joint + (pin.target.empty() ? " held in the world" : " held to " + pin.target);
        dl->AddText(ImVec2(s.x + 8, y - ImGui::GetFontSize() / 2), alpha(c, 0.85f), label.c_str());
        if (release >= 0) {
            ImVec2 e(x_of(release), y);
            dl->AddQuad(ImVec2(e.x, y - r), ImVec2(e.x + r, y), ImVec2(e.x, y + r), ImVec2(e.x - r, y), c, 1.5f);
            pin_hits.insert(pin_hits.begin(), {e, {i, true}});
        }
        pin_hits.push_back({s, {i, false}});
    }
    auto pin_under = [&]() -> const PinHit* {
        if (m.y > plot_top + 4 || m.y < canvas_min_.y) return nullptr;
        for (auto& [p, hit] : pin_hits)
            if (std::fabs(m.x - p.x) <= 7) return &hit;
        return nullptr;
    };

    std::vector<int> shown = shown_channels();
    if (shown.empty()) {
        const char* hint = "Select a bone to see its curves";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(canvas_min_.x + (size.x - ts.x) / 2, plot_top + (plot_h_ - ts.y) / 2), tc.text_dim, hint);
    }

    // Curves: sampled every 2 px, holding the end values outside the keys (TG-51).
    auto is_hot = [&](int c) {
        if (c == active_) return true;
        for (auto& s : selection_)
            if (s.track == channels_[c].track && s.channel == channels_[c].channel) return true;
        return false;
    };
    for (int c : shown) {
        const FCurve* cv = curve(clip, channels_[c]);
        bool hot = is_hot(c);
        ImU32 col = hot ? lighten(channels_[c].colour, 0.25f) : alpha(channels_[c].colour, 0.8f);
        if (!cv || cv->empty()) col = alpha(channels_[c].colour, 0.25f);
        // Many curves at once: cold ones drop to ImGui's 1 px texture-antialiased lines, a fraction of the
        // vertices of a 1.5 px line, which is what a few hundred channels need to stay interactive.
        float width = hot ? 2.f : shown.size() > 24 ? 1.f : 1.5f;
        static std::vector<ImVec2> pts;  // one polyline per curve: far fewer draw-list entries than a line per step
        pts.clear();
        const float step = shown.size() > 24 ? 4.f : 2.f;  // a crowd of curves reads the same at half the samples
        for (float x = canvas_min_.x; x <= cmax.x + step; x += step) pts.emplace_back(x, y_of(cv ? cv->evaluate(f_at(x)) : 0));
        if (!channels_[c].pole) {
            dl->AddPolyline(pts.data(), int(pts.size()), col, ImDrawFlags_None, width);
        } else {
            for (size_t i = 1; i < pts.size(); ++i)
                if (i % 4 != 3) dl->AddLine(pts[i - 1], pts[i], col, width);  // poles are dashed
        }
    }

    // Keys and the handles of selected keys (TG-53/54).
    for (int c : shown) {
        const FCurve* cv = curve(clip, channels_[c]);
        if (!cv) continue;
        float last_x = -1e9f;  // an unselected key closer than this to the last one drawn is covered by it anyway
        for (int i = 0; i < int(cv->keys.size()); ++i) {
            const Key& k = cv->keys[i];
            if (k.frame < view_.t0 - 1 || k.frame > view_.t1 + 1) continue;
            ImVec2 p(x_of(k.frame), y_of(k.value));
            bool sel = selected(channels_[c], i);
            if (!sel && p.x - last_x < (shown.size() > 24 ? 8 : 4)) continue;
            last_x = p.x;
            if (sel) {
                ImU32 hc = k.left == Handle::Free || k.right == Handle::Free ? kHandleFree : kHandle;
                if (i > 0 && cv->keys[i - 1].interp == Interp::Bezier) {
                    ImVec2 h(x_of(k.lx), y_of(k.ly));
                    dl->AddLine(p, h, hc);
                    dl->AddCircleFilled(h, 3.5f, hc);
                }
                if (i + 1 < int(cv->keys.size()) && k.interp == Interp::Bezier) {
                    ImVec2 h(x_of(k.rx), y_of(k.ry));
                    dl->AddLine(p, h, hc);
                    dl->AddCircleFilled(h, 3.5f, hc);
                }
            }
            if (channels_[c].pole) {
                float r = sel ? 6 : 5;
                ImVec2 q[4] = {{p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}};
                dl->AddQuadFilled(q[0], q[1], q[2], q[3], sel ? kKeySelected : IM_COL32(20, 20, 22, 255));
                if (!sel) dl->AddQuad(q[0], q[1], q[2], q[3], channels_[c].colour);
            } else {
                float r = sel ? 4.5f : 3.5f;
                dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r),
                                  sel ? kKeySelected : IM_COL32(20, 20, 22, 255));
                if (!sel) dl->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), channels_[c].colour);
            }
        }
    }

    // Candidates under the cursor, nearest first; ties go to the active channel, then later channels (TG-70).
    struct Cand {
        double d;
        int channel, key;
    };
    auto candidates = [&]() {
        std::vector<Cand> out;
        for (int c : shown)
            if (const FCurve* cv = curve(clip, channels_[c]))
                for (int i = 0; i < int(cv->keys.size()); ++i) {
                    double d = distance(m, ImVec2(x_of(cv->keys[i].frame), y_of(cv->keys[i].value)));
                    if (d <= kPickPx) out.push_back({d, c, i});
                }
        std::stable_sort(out.begin(), out.end(), [&](const Cand& a, const Cand& b) {
            if (std::fabs(a.d - b.d) > 0.5) return a.d < b.d;
            if ((a.channel == active_) != (b.channel == active_)) return a.channel == active_;
            return a.channel > b.channel;
        });
        return out;
    };
    auto curve_under = [&]() {
        int best = -1;
        double best_d = kPickPx;
        for (int c : shown) {
            const FCurve* cv = curve(clip, channels_[c]);
            double d = std::fabs(y_of(cv ? cv->evaluate(f_at(m.x)) : 0) - m.y);
            if (d <= best_d) best_d = d, best = c;
        }
        return best;
    };
    auto ref_of = [&](int c, int k) { return KeyRef{channels_[c].track, channels_[c].channel, k}; };

    // Scale box around a selection of two or more keys (TG-94).
    double bf0 = 1e30, bf1 = -1e30, bv0 = 1e30, bv1 = -1e30;
    for (auto& s : selection_) {
        const Key& k = clip.curves.at(s.track).at(s.channel).keys[s.index];
        bf0 = std::min(bf0, k.frame), bf1 = std::max(bf1, k.frame), bv0 = std::min(bv0, k.value), bv1 = std::max(bv1, k.value);
    }
    ImVec2 box_handles[8];
    bool handle_on[8] = {};
    bool box_visible = selection_.size() >= 2 && (drag_ == Drag::None || drag_ == Drag::Scale);
    if (box_visible) {
        float x0 = x_of(bf0) - 8, x1 = x_of(bf1) + 8, y0 = y_of(bv1) - 8, y1 = y_of(bv0) + 8;
        bool span_t = bf1 > bf0, span_v = bv1 > bv0;
        ImVec2 pts[8] = {{x0, y0}, {(x0 + x1) / 2, y0}, {x1, y0}, {x1, (y0 + y1) / 2},
                         {x1, y1}, {(x0 + x1) / 2, y1}, {x0, y1}, {x0, (y0 + y1) / 2}};
        bool on[8] = {span_t && span_v, span_v, span_t && span_v, span_t, span_t && span_v, span_v, span_t && span_v, span_t};
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 120));
        for (int h = 0; h < 8; ++h) {
            box_handles[h] = pts[h];
            handle_on[h] = on[h];
            if (!on[h]) continue;
            ImU32 c = scale_handle_ == h && drag_ == Drag::Scale ? kKeySelected : IM_COL32(235, 235, 235, 230);
            dl->AddRectFilled(ImVec2(pts[h].x - 4, pts[h].y - 4), ImVec2(pts[h].x + 4, pts[h].y + 4), c);
        }
    }
    // The resize cursor over a scale-box handle (TG-94).
    int cursor_handle = drag_ == Drag::Scale ? scale_handle_ : -1;
    if (cursor_handle < 0 && box_visible && hovered_ && drag_ == Drag::None)
        for (int h = 0; h < 8; ++h)
            if (handle_on[h] && distance(m, box_handles[h]) <= 6) cursor_handle = h;
    if (cursor_handle >= 0) {
        static const ImGuiMouseCursor cursors[8] = {ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS,
                                                    ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW,
                                                    ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS,
                                                    ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW};
        ImGui::SetMouseCursor(cursors[cursor_handle]);
    }

    // Wheel zoom about the cursor: Shift = values only, Ctrl = time only (TG-66).
    if (hovered_ && io.MouseWheel != 0) {
        double k = std::pow(0.85, io.MouseWheel), fm = f_at(m.x), vm = v_at(m.y);
        if (!io.KeyShift) {
            double span = std::clamp((view_.t1 - view_.t0) * k, 0.5, 1e5), r = (fm - view_.t0) / (view_.t1 - view_.t0);
            view_.t0 = fm - span * r, view_.t1 = view_.t0 + span;
        }
        if (!io.KeyCtrl) {
            double span = std::clamp((view_.v1 - view_.v0) * k, 1e-4, 1e7), r = (vm - view_.v0) / (view_.v1 - view_.v0);
            view_.v0 = vm - span * r, view_.v1 = view_.v0 + span;
        }
    }

    // Presses, by precedence (TG-76).
    bool left = ImGui::IsMouseClicked(0), middle = ImGui::IsMouseClicked(2), right_btn = ImGui::IsMouseClicked(1);
    if (hovered_ && drag_ == Drag::None && (left || middle || right_btn)) {
        press_pos_ = m;
        press_f_ = f_at(m.x), press_v_ = v_at(m.y);
        press_view_ = view_;
        const PinHit* pin_hit = left || right_btn ? pin_under() : nullptr;
        if (left && !pin_hit) pin_sel_ = -1;  // TG-77
        // Navigation follows the preset's 3D view bindings (TG-67):
        //   Industry:    Alt + left or middle pans, Alt + right zooms.
        //   Blender:     middle pans (Shift or not), Ctrl + middle zooms; emulate 3-button makes Alt + left middle.
        //   QAvimator:   middle pans, no drag zoom.
        //   Second Life: Ctrl + Alt + left (Shift or not) or middle pans, Alt + left zooms, as the 3D view's
        //                Ctrl+Alt+Shift pan and Alt-drag zoom do.
        // Only Industry leaves middle free, so only there does middle with keys selected move them (TG-92).
        Drag nav = Drag::None;
        switch (ctx.preset) {
            case Preset::Industry:
                if (io.KeyAlt) nav = right_btn ? Drag::Zoom : Drag::Pan;
                break;
            case Preset::Blender:
                if (middle || (ctx.emulate_3_button && io.KeyAlt && left)) nav = io.KeyCtrl ? Drag::Zoom : Drag::Pan;
                break;
            case Preset::QAvimator:
                if (middle) nav = Drag::Pan;
                break;
            case Preset::SecondLife:
                if (middle || (io.KeyAlt && io.KeyCtrl && left)) nav = Drag::Pan;
                else if (io.KeyAlt && left) nav = Drag::Zoom;
                break;
        }
        if (nav != Drag::None) {
            drag_ = nav;
        } else if (middle && !selection_.empty()) {
            drag_ = Drag::Move;
            press_clip_ = clip;
            press_sel_ = selection_;
            ctx.history.begin(clip);
        } else if (pin_hit) {  // TG-102, TG-103
            pin_sel_ = pin_hit->pin;
            selection_.clear();
            if (left) {
                drag_ = Drag::Pin;
                pin_end_ = pin_hit->release;
                const Pin& pin = clip.pins[pin_sel_];
                pin_frame_ = pin_end_ ? (pin.to < 0 ? clip.end_frame + 1 : pin.to + 1) : pin.from;
            } else {
                ImGui::OpenPopup("##pin");
            }
        } else if (left && m.y < plot_top) {
            drag_ = Drag::Scrub;
        } else if (left && ImGui::IsMouseDoubleClicked(0)) {
            int c = curve_under();
            if (c < 0) c = active_;
            if (c >= 0) {
                double f = snap_ ? std::round(press_f_) : press_f_;
                int idx = 0;
                const Channel ch = channels_[c];
                edit(ctx, "Insert Key", [&](Clip& cl) { idx = insert_on_curve(cl.curves[ch.track][ch.channel], f); });
                selection_.assign(1, KeyRef{ch.track, ch.channel, idx});
                active_ = c;
            }
        } else if (left) {
            // Tangent handles of selected keys.
            for (auto& s : selection_) {
                const FCurve& cv = clip.curves.at(s.track).at(s.channel);
                const Key& k = cv.keys[s.index];
                bool has_l = s.index > 0 && cv.keys[s.index - 1].interp == Interp::Bezier;
                bool has_r = s.index + 1 < int(cv.keys.size()) && k.interp == Interp::Bezier;
                if (has_l && distance(m, ImVec2(x_of(k.lx), y_of(k.ly))) <= kPickPx) drag_ = Drag::Handle, handle_right_ = false;
                else if (has_r && distance(m, ImVec2(x_of(k.rx), y_of(k.ry))) <= kPickPx) drag_ = Drag::Handle, handle_right_ = true;
                if (drag_ == Drag::Handle) {
                    handle_key_ = s;
                    ctx.history.begin(clip);
                    break;
                }
            }
            // Scale box handles.
            if (drag_ == Drag::None && box_visible)
                for (int h = 0; h < 8; ++h)
                    if (handle_on[h] && distance(m, box_handles[h]) <= 6) {
                        drag_ = Drag::Scale;
                        scale_handle_ = h;
                        bool l = h == 0 || h == 6 || h == 7, r = h == 2 || h == 3 || h == 4;
                        bool t = h <= 2, b = h >= 4 && h <= 6;
                        scale_pivot_f_ = l ? bf1 : r ? bf0 : bf0;
                        scale_w_ = l ? bf0 - bf1 : r ? bf1 - bf0 : 0;
                        scale_pivot_v_ = t ? bv0 : b ? bv1 : bv0;
                        scale_h_ = t ? bv1 - bv0 : b ? bv0 - bv1 : 0;
                        press_clip_ = clip;
                        press_sel_ = selection_;
                        ctx.history.begin(clip);
                        break;
                    }
            // Keys.
            if (drag_ == Drag::None) {
                auto cands = candidates();
                if (!cands.empty()) {
                    // A plain click again on the spot of the last plain click cycles through stacked keys
                    // (TG-71). With a single key there is nothing to cycle, and after a Shift/Ctrl click the
                    // press must keep the selection so it can be dragged (TG-72).
                    bool plain = !io.KeyShift && !io.KeyCtrl;
                    bool again = plain && cands.size() > 1 && distance(m, last_click_) <= 3;
                    last_click_ = plain ? m : ImVec2(-100, -100);
                    Cand pick = cands.front();
                    if (again)
                        for (size_t i = 0; i < cands.size(); ++i)
                            if (selected(channels_[cands[i].channel], cands[i].key)) {
                                pick = cands[(i + 1) % cands.size()];
                                break;
                            }
                    KeyRef r = ref_of(pick.channel, pick.key);
                    auto it = std::find(selection_.begin(), selection_.end(), r);
                    bool was = it != selection_.end();
                    if (io.KeyShift && io.KeyCtrl) {
                        if (!was) selection_.push_back(r);
                    } else if (io.KeyShift) {
                        was ? (void)selection_.erase(it) : selection_.push_back(r);
                    } else if (io.KeyCtrl) {
                        if (was) selection_.erase(it);
                    } else if (!was || again) {
                        selection_.assign(1, r);
                    }
                    active_ = pick.channel;
                    if (std::find(selection_.begin(), selection_.end(), r) != selection_.end()) {
                        drag_ = Drag::Move;  // starts moving after 3 px
                        press_clip_ = clip;
                        press_sel_ = selection_;
                    }
                } else {
                    drag_ = Drag::Box;
                }
            }
            if (drag_ != Drag::Move) last_click_ = ImVec2(-100, -100);
        }
    }

    // Drags.
    if (drag_ != Drag::None) {
        bool down = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2);
        double df = f_at(m.x) - press_f_, dv = v_at(m.y) - press_v_;
        bool moved_enough = distance(m, press_pos_) > 3;
        switch (drag_) {
            case Drag::Pan: {
                double pf = (m.x - press_pos_.x) / canvas_w_ * (press_view_.t1 - press_view_.t0);
                double pv = (m.y - press_pos_.y) / plot_h_ * (press_view_.v1 - press_view_.v0);
                view_ = {press_view_.t0 - pf, press_view_.t1 - pf, press_view_.v0 + pv, press_view_.v1 + pv};
                break;
            }
            case Drag::Zoom: {
                double kt = std::exp(-0.006 * (m.x - press_pos_.x)), kv = std::exp(0.006 * (m.y - press_pos_.y));
                auto scale = [](double lo, double hi, double at, double k, double mn, double mx, double& o0, double& o1) {
                    double span = std::clamp((hi - lo) * k, mn, mx), r = (at - lo) / (hi - lo);
                    o0 = at - span * r, o1 = o0 + span;
                };
                scale(press_view_.t0, press_view_.t1, press_f_, kt, 0.5, 1e5, view_.t0, view_.t1);
                scale(press_view_.v0, press_view_.v1, press_v_, kv, 1e-4, 1e7, view_.v0, view_.v1);
                break;
            }
            case Drag::Scrub:
                ctx.frame = std::clamp(std::round(f_at(m.x)), 0.0, double(clip.end_frame));
                break;
            case Drag::Move:
                if (!moved_enough && !ctx.history.is_open()) break;
                if (!ctx.history.is_open()) ctx.history.begin(press_clip_);
                if (io.KeyShift) {  // lock to the dominant axis, re-evaluated live
                    if (std::fabs(m.x - press_pos_.x) > std::fabs(m.y - press_pos_.y)) dv = 0;
                    else df = 0;
                }
                selection_ = press_sel_;
                move_keys(clip, press_clip_, selection_, df, dv, snap_);
                break;
            case Drag::Handle: {
                FCurve& cv = clip.curves[handle_key_.track][handle_key_.channel];
                drag_handle(cv, handle_key_.index, handle_right_, f_at(m.x), v_at(m.y));
                break;
            }
            case Drag::Scale: {
                double sx = std::fabs(scale_w_) > 1e-9 ? (scale_w_ + df) / scale_w_ : 1;
                double sy = std::fabs(scale_h_) > 1e-9 ? (scale_h_ + dv) / scale_h_ : 1;
                selection_ = press_sel_;
                scale_keys(clip, press_clip_, selection_, scale_pivot_f_, scale_pivot_v_, sx, sy, snap_);
                break;
            }
            case Drag::Pin: {  // whole frames: the start up to the last held frame, the release up to end + 1
                const Pin& pin = clip.pins[pin_sel_];
                int f = int(std::lround(f_at(m.x)));
                pin_frame_ = pin_end_ ? std::clamp(f, pin.from + 1, clip.end_frame + 1)
                                      : std::clamp(f, 0, pin.to < 0 ? clip.end_frame : pin.to);
                break;
            }
            case Drag::Box: {
                ImVec2 a(std::min(m.x, press_pos_.x), std::min(m.y, press_pos_.y)),
                    b(std::max(m.x, press_pos_.x), std::max(m.y, press_pos_.y));
                if (moved_enough) {
                    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18));
                    dl->AddRect(a, b, IM_COL32(255, 255, 255, 110));
                }
                break;
            }
            default: break;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // Escape cancels the drag (spec README, UX 1)
            if (ctx.history.is_open() && (drag_ == Drag::Move || drag_ == Drag::Scale || drag_ == Drag::Handle)) {
                clip = ctx.history.cancel();
                if (drag_ != Drag::Handle) selection_ = press_sel_;
            }
            if (drag_ == Drag::Pan || drag_ == Drag::Zoom) view_ = press_view_;
            drag_ = Drag::None;
            scale_handle_ = -1;
            escape_used_ = true;
        } else if (!down) {  // release
            if (drag_ == Drag::Move && ctx.history.is_open()) {
                finish_transform(clip, selection_);
                if (ctx.history.commit("Move Keys", clip)) ctx.changed();
            } else if (drag_ == Drag::Scale) {
                finish_transform(clip, selection_);
                if (ctx.history.commit("Scale Keys", clip)) ctx.changed();
            } else if (drag_ == Drag::Handle) {
                if (ctx.history.commit("Edit Tangent", clip)) ctx.changed();
            } else if (drag_ == Drag::Pin) {
                const Pin& pin = clip.pins[pin_sel_];
                int before = pin_end_ ? (pin.to < 0 ? clip.end_frame + 1 : pin.to + 1) : pin.from;
                if (pin_frame_ != before && !ctx.rig) {
                    ctx.status("Pins can't be moved here");
                } else if (pin_frame_ != before) {
                    size_t i = size_t(pin_sel_);
                    int f = pin_frame_;
                    if (pin_end_)  // the core takes the last held frame; end + 1 means held to the end
                        edit(ctx, "Move Pin Release",
                             [&](Clip& c) { move_pin_end(c, *ctx.rig, i, f > c.end_frame ? f : f - 1, ctx.shape); });
                    else
                        edit(ctx, "Move Pin Start", [&](Clip& c) { move_pin_start(c, *ctx.rig, i, f, ctx.shape); });
                    const Pin& p = clip.pins[i];
                    ctx.status(!pin_end_ ? p.joint + " is now pinned from frame " + std::to_string(p.from)
                               : p.to < 0 ? p.joint + " is now held to the end"
                                          : p.joint + " is now released at frame " + std::to_string(p.to + 1));
                }
            } else if (drag_ == Drag::Box) {
                if (moved_enough) {
                    float x0 = std::min(m.x, press_pos_.x), x1 = std::max(m.x, press_pos_.x);
                    float y0 = std::min(m.y, press_pos_.y), y1 = std::max(m.y, press_pos_.y);
                    if (!io.KeyShift && !io.KeyCtrl) selection_.clear();
                    for (int c : shown)
                        if (const FCurve* cv = curve(clip, channels_[c]))
                            for (int i = 0; i < int(cv->keys.size()); ++i) {
                                float x = x_of(cv->keys[i].frame), y = y_of(cv->keys[i].value);
                                if (x < x0 || x > x1 || y < y0 || y > y1) continue;
                                KeyRef r = ref_of(c, i);
                                auto it = std::find(selection_.begin(), selection_.end(), r);
                                if (io.KeyCtrl && !io.KeyShift) {
                                    if (it != selection_.end()) selection_.erase(it);
                                } else if (it == selection_.end()) {
                                    selection_.push_back(r);
                                }
                            }
                } else {  // click on empty canvas: pick the curve under the cursor (TG-74)
                    active_ = curve_under();
                    if (!io.KeyShift && !io.KeyCtrl) selection_.clear();
                }
            }
            drag_ = Drag::None;
            scale_handle_ = -1;
        }
    }

    // Hover label on a key (TG-59).
    if (hovered_ && drag_ == Drag::None) {
        auto cands = candidates();
        if (!cands.empty()) {
            const Channel& ch = channels_[cands.front().channel];
            const Key& k = curve(clip, ch)->keys[cands.front().key];
            char b[160], val[32];
            std::string owner = ctx.item_label ? ctx.item_label(ch.track) : ch.track;
            if (std::fabs(k.value) >= 10) std::snprintf(val, sizeof val, "%s", format_value(k.value).c_str());
            else std::snprintf(val, sizeof val, "%.3f", k.value);
            std::snprintf(b, sizeof b, "%s \xC2\xB7 %s \xC2\xB7 frame %.0f \xC2\xB7 %s", owner.c_str(), ch.label.c_str(),
                          k.frame, val);
            ImVec2 ts = ImGui::CalcTextSize(b), at(x_of(k.frame) + 10, std::max(y_of(k.value) - 12 - ts.y, plot_top + 2));
            at.x = std::min(at.x, cmax.x - ts.x - 8);
            dl->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + ts.x + 4, at.y + ts.y + 2), IM_COL32(12, 13, 16, 220), 3);
            dl->AddText(at, lighten(ch.colour, 0.35f), b);
        }
    }

    // Playhead.
    const double at = std::floor(ctx.frame + 1e-9);  // playback is fractional; the playhead shows whole frames (TG-11)
    float px = x_of(at);
    dl->AddLine(ImVec2(px, canvas_min_.y), ImVec2(px, cmax.y), ui::kPlayhead, 2);  // same as the timeline (TG-5)
    dl->AddRectFilled(ImVec2(px - 14, canvas_min_.y + 2), ImVec2(px + 14, canvas_min_.y + 19), ui::kPlayhead, 3);
    char t[16];
    std::snprintf(t, sizeof t, "%d", int(at));
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(px - ts.x / 2, canvas_min_.y + 3), IM_COL32(20, 22, 26, 255), t);
    dl->PopClipRect();

    if (ImGui::BeginPopup("##pin")) {  // TG-103
        if (pin_sel_ < 0) ImGui::CloseCurrentPopup();
        else if (ImGui::MenuItem(("Delete the Pin on " + clip.pins[pin_sel_].joint).c_str())) {
            selection_.clear();
            delete_selected(ctx);
        }
        ImGui::EndPopup();
    }
}

}  // namespace vats
