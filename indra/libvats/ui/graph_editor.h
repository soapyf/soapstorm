// Viewport Avatar Toolset - the graph editor: curves of the selected bones, key editing.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/05 sections 2.4-2.12.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "settings.h"
#include "vats/curve_filter.h"
#include "vats/curve_ops.h"
#include "vats/history.h"
#include "vats/pose_ops.h"
#include "vats/pose_tools.h"
#include "vats/rig.h"
#include "vats/skeleton.h"

namespace vats {

// What the graph editor works on this frame.
struct GraphContext {
    Clip& clip;
    History& history;
    const Skeleton& skel;
    double& frame;
    std::vector<std::string> items;  // tracks to show: bone names, "pin:<joint>", "ik.<Limb>"
    std::function<std::string(const std::string&)> item_label;  // "Left Arm IK", "Chest (pin)", ...
    std::function<void()> changed;                                // an edit was committed
    std::function<void(const std::string&)> status;
    const Rig* rig = nullptr;        // needed to move or delete pins (TG-102, TG-103)
    const Shape* shape = nullptr;
    Preset preset = Preset::Industry;  // drag navigation follows the 3D view's preset (TG-67, TG-92)
    bool emulate_3_button = false;     // Blender: Alt + left counts as middle
    // The active preset's first shortcut for an action id ("frame_all"), "" when unbound (TG-31).
    std::function<std::string(const char* action_id)> key_name;
};

// The filter settings shared by Filter Curves and the motion capture clean-up (curve_filter_ui.cpp). label lays
// out a row's label and sets the next item's width. True when a value changed.
bool filter_kind_ui(FilterKind& k);
bool filter_params_ui(FilterSettings& s, const std::function<void(const char*)>& label);

class GraphEditor {
    friend class DopeSheet;  // spec 08 DS: shares the key selection, clipboard, list mode, snap and time range

public:
    enum class Mode { Selected, AllAnimated };

    void draw(GraphContext& ctx);
    bool hovered() const { return hovered_; }
    bool snap_frames() const { return snap_; }  // the toolbar's Snap frames (also used by retime markers, 08 TE-5)
    // True once after Escape cancelled a graph drag this frame, so the app can skip its own Escape action.
    bool take_escape() { return std::exchange(escape_used_, false); }
    Mode mode() const { return mode_; }

    // Commands that act on the graph while the mouse is over it.
    void frame_all(const GraphContext& ctx);
    void frame_selected(const GraphContext& ctx);
    void fit_values(const GraphContext& ctx);
    bool delete_selected(GraphContext& ctx);
    void copy_keys(GraphContext& ctx);
    void paste_keys(GraphContext& ctx);
    // New, Open, Undo (TG-46). A drag in progress is dropped: it refers to the old clip.
    void clip_replaced() { selection_.clear(), active_ = -1, pin_sel_ = -1, drag_ = Drag::None; }
    // The frame span of the selected keys, rounded to whole frames; false unless it spans a frame.
    bool key_span(const Clip& clip, double& a, double& b) const;
    // Filter Curves (MC-4a): while its dialog is open the clip is the live preview and this is the clip as it
    // was (for the ghost); nullptr otherwise.
    const Clip* filter_original() const { return filter_open_ ? &filter_before_ : nullptr; }
    // PT-4: the buffer curves, drawn in grey until cleared. Tools that rewrite curves wholesale (Euler Filter,
    // Dynamics bake, the Loop tools) snapshot first, so the curves before them stay in view.
    void snapshot_curves(const Clip& clip) { buffer_.snapshot(clip); }
    void clear_snapshot() { buffer_.clear(); }  // New and Open: the grey curves were the old document's

private:
    struct Channel {
        std::string track, channel, label;
        ImU32 colour;
        bool pole = false;
        int item = 0;
    };
    struct View {
        double t0 = -2, t1 = 32, v0 = -100, v1 = 100;
    };

    void build_channels(const GraphContext& ctx);
    void draw_toolbar(GraphContext& ctx);
    void draw_channel_list(GraphContext& ctx);
    void draw_canvas(GraphContext& ctx);
    void draw_ease_menu(GraphContext& ctx);  // spec 08 TW-3, in tween_ui.cpp
    void draw_tag_menu_items(GraphContext& ctx);  // spec 08 KT-1, in key_tags_ui.cpp
    void edit(GraphContext& ctx, const char* label, const std::function<void(Clip&)>& change);
    void open_filter(GraphContext& ctx);  // curve_filter_ui.cpp
    void preview_filter(GraphContext& ctx);
    void draw_filter_dialog(GraphContext& ctx);

    const FCurve* curve(const Clip& clip, const Channel& c) const;
    std::vector<int> shown_channels() const;
    bool selected(const Channel& c, int key) const;
    void reconcile(const Clip& clip);
    void fit_bounds(double f0, double f1, double v0, double v1, bool pad_small);

    float x_of(double f) const { return float(canvas_min_.x + (f - view_.t0) / (view_.t1 - view_.t0) * canvas_w_); }
    float y_of(double v) const { return float(canvas_min_.y + ruler_ + (view_.v1 - v) / (view_.v1 - view_.v0) * plot_h_); }
    double f_at(float x) const { return view_.t0 + (x - canvas_min_.x) / canvas_w_ * (view_.t1 - view_.t0); }
    double v_at(float y) const { return view_.v1 - (y - canvas_min_.y - ruler_) / plot_h_ * (view_.v1 - view_.v0); }

    Mode mode_ = Mode::Selected;
    std::vector<std::string> item_names_;  // the item list the channels were built for
    std::vector<Channel> channels_;
    std::vector<int> row_filter_;          // selected list rows: item index * 16 + channel slot, or item * 16 + 15
    std::vector<KeyRef> selection_;
    int active_ = -1;                      // active channel index
    View view_;
    bool framed_once_ = false, fit_pending_ = false, hovered_ = false, snap_ = true;

    // Canvas geometry of the current frame.
    ImVec2 canvas_min_;
    float canvas_w_ = 1, plot_h_ = 1, ruler_ = 22;

    // Drag state.
    enum class Drag { None, Scrub, Move, Box, Handle, Scale, Pan, Zoom, Pin };
    Drag drag_ = Drag::None;
    ImVec2 press_pos_, last_click_{-100, -100};  // last_click_: the last plain click on a key (TG-71)
    Clip press_clip_;
    std::vector<KeyRef> press_sel_;
    double press_f_ = 0, press_v_ = 0;
    View press_view_;
    KeyRef handle_key_;
    bool handle_right_ = false;
    int scale_handle_ = -1;  // 0..7 around the box
    double scale_pivot_f_ = 0, scale_pivot_v_ = 0, scale_w_ = 1, scale_h_ = 1;
    KeyClipboard clipboard_;  // pose_ops.h (AM-98)
    int pin_sel_ = -1;           // selected pin (index into clip.pins)
    bool pin_end_ = false;       // dragging the release marker rather than the start
    int pin_frame_ = 0;          // where the dragged marker is
    bool escape_used_ = false;

    // Filter Curves: the shown curves over filter_from_..filter_to_; the history step stays open meanwhile.
    bool filter_open_ = false;
    Clip filter_before_;
    std::vector<CurveId> filter_ids_;
    int filter_from_ = 0, filter_to_ = 0;
    float filter_dim_ = 0;  // the style's modal dimming, put back on close
    FilterSettings filter_;
    std::vector<JointShake> filter_shake_[2];  // before, after
    CurveBuffer buffer_;
};

}  // namespace vats
