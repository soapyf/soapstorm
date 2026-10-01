// Viewport Avatar Toolset - the dope sheet: key frames per body part and bone, beside the graph editor.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 17 (DS). It edits the graph editor's own key selection, clipboard, list mode, snap and
// time range (GraphEditor befriends it), so a key picked in one is picked in the other.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "graph_editor.h"
#include "vats/dope_sheet.h"

namespace vats {

class DopeSheet {
public:
    explicit DopeSheet(GraphEditor& graph) : graph_(graph) {}

    // select_tracks: a double-clicked row's tracks, for the app to select their bones and IK controls.
    void draw(GraphContext& ctx, const std::function<void(const std::vector<std::string>&)>& select_tracks);
    // Over the sheet this frame or the last (the app's shortcuts run before the panels draw).
    bool hovered() const { return hover_frame_ >= ImGui::GetFrameCount() - 1; }

private:
    struct Line {
        std::string label;
        std::vector<std::string> tracks;
        int depth = 0;  // 0 the summary, 1 a body part, 2 a bone or control
    };
    enum class Drag { None, Scrub, Move, Scale, Box, Pan };

    std::vector<Line> lines(const GraphContext& ctx) const;
    float x_of(double f) const;
    double f_at(float x) const;

    GraphEditor& graph_;
    std::set<std::string> open_;  // expanded body parts
    int hover_frame_ = -10;
    float x0_ = 0, w_ = 1;  // the key area of the current frame, screen x
    Drag drag_ = Drag::None;
    ImVec2 press_;
    Clip press_clip_;
    std::vector<KeyRef> press_sel_;
    double press_t0_ = 0, press_t1_ = 1;
    double scale_pivot_ = 0, scale_w_ = 1;
    bool press_whole_ = false;  // the scaled keys span 0 to Last frame: the length scales with them
};

}  // namespace vats
