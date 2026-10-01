// Viewport Avatar Toolset - the body picker: pages of joint dots and bone lines, their groups, framing and click tests.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (PK-1, PK-3). Pure layout and geometry; the UI (ui/picker_ui.cpp) draws it over a render
// of the avatar (style A) or over the silhouette traced from the Linden body (style B, picker_silhouette).
//
// Page space: every page view is drawn as if seen from +X looking down -X, +Y to the right and +Z up; a point's place
// on the page is (y, z) in metres. picker_parts() carries world points there: the body turned to face the viewer, each
// hand turned fingers up (so a hand reads the same whatever the arm does), the head turned to face the viewer.
#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "vats/skeleton.h"

namespace vats {

enum class PickerPage { Body, Hands, Face, Extras };
inline constexpr int kPickerPageCount = 4;
const char* picker_page_name(PickerPage p);          // "Body", "Hands", "Face", "Extras"
int picker_view_count(PickerPage p);                  // Body 2, Hands 2, Face 1, Extras 3
const char* picker_view_name(PickerPage p, int view);  // "Front"/"Back", "Back"/"Palm", "Face", "Wings"/"Tail"/"Hind"

struct V2 {
    double x = 0, y = 0;
    V2 operator+(V2 o) const { return {x + o.x, y + o.y}; }
    V2 operator-(V2 o) const { return {x - o.x, y - o.y}; }
    V2 operator*(double s) const { return {x * s, y * s}; }
    double dot(V2 o) const { return x * o.x + y * o.y; }
    double length() const { return std::sqrt(x * x + y * y); }
};
double segment_distance(V2 p, V2 a, V2 b);  // from p to the segment a-b

// The chart pose: SL's rest with the arms lowered 50 degrees and both hands open. Style B and "Rest pose" show it.
Pose picker_chart_pose(const Skeleton& skel);

// How one part of a page is carried into page space, and which of the body's triangles a render of it keeps
// (picker_keeps): with clip, those whose centre lies in [lo, hi] and, when there are keep capsules, near one of them
// (a hand's own skin, not the thigh it rests on). Page space throughout.
struct PickerPart {
    struct Capsule {
        Vec3 a, b;
        double r;
    };
    Xform to_page;
    bool clip = false;
    Vec3 lo, hi;
    std::vector<Capsule> keep;
};
bool picker_keeps(const PickerPart& part, const Vec3& centre);
// One part per page view, except Hands: the right hand (drawn on the left), then the left. chart: the chart pose's
// globals on the same body, which place the hands side by side the same way whatever the pose.
std::vector<PickerPart> picker_parts(const Skeleton& skel, const std::vector<Xform>& globals,
                                     const std::vector<Xform>& chart, const Shape* shape, PickerPage page, int view);
int picker_part_of(const Skeleton& skel, PickerPage page, int node);  // which part carries a node

// The page's groups: what a label, a fingertip circle, a knuckle row button or a face chip selects.
enum class PickerGroupKind { Label, Finger, Row, Chip };
struct PickerGroup {
    PickerPage page;
    PickerGroupKind kind;
    int view = -1;                   // the view the label shows in; -1 every view of the page
    std::string label;               // "R ARM", "1", "Brows"
    std::string name;                // "Right Arm", for tooltips and the status line
    std::vector<std::string> bones;  // skeleton names, selected together
};
const std::vector<PickerGroup>& picker_groups();
std::vector<int> picker_group_nodes(const Skeleton& skel, const PickerGroup& g);  // the bones the skeleton has

struct PickerDot {
    int node;
    V2 p;       // page space
    int group;  // the group of the page it belongs to (index into picker_groups), -1 none
    bool point = false;  // an attachment point or collision volume (the Points overlay)
};
struct PickerLine {
    int node;  // the bone the line is (a click on it picks the bone)
    V2 a, b;
    int group;
};
struct PickerCap {  // a circle past a fingertip, selecting the whole finger
    int group;
    V2 tip, dir;  // the fingertip and the finger's direction (unit, page space)
};
struct PickerLayout {
    std::vector<PickerDot> dots;
    std::vector<PickerLine> lines;
    std::vector<PickerCap> caps;
};
// The page view's dots and lines from a pose's globals (shape: the body's, for the bone tips). points, volumes: add
// the attachment points or the collision volumes near the page's bones (the Points menu).
PickerLayout picker_layout(const Skeleton& skel, const std::vector<Xform>& globals, const std::vector<PickerPart>& parts,
                           const Shape* shape, PickerPage page, int view, bool points = false, bool volumes = false);
// The bones style B's silhouette follows, as segments in page space (a dot is a segment of no length).
std::vector<PickerLine> picker_anchors(const Skeleton& skel, const std::vector<Xform>& globals,
                                       const std::vector<PickerPart>& parts, const Shape* shape, PickerPage page, int view);
// Style B: the Linden body's outline for a page view (traced by tools/picker_silhouette.py from the chart pose on the
// SL default shape), bent onto anchors (picker_anchors on the body shown): each outline point follows the two chart
// bones nearest to it, so the silhouette keeps the body's proportions. Closed loops in page space; empty = no data.
std::vector<std::vector<V2>> picker_silhouette(const Skeleton& skel, PickerPage page, int view,
                                               const std::vector<PickerLine>& anchors);

// --- the canvas ---------------------------------------------------------------------------------------------------
struct PickerRect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(V2 p) const { return p.x >= x && p.y >= y && p.x <= x + w && p.y <= y + h; }
    bool inside(const PickerRect& o) const { return x >= o.x && y >= o.y && x + w <= o.x + o.w && y + h <= o.y + o.h; }
    bool overlaps(const PickerRect& o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
};
// Page space to canvas pixels (y down), uniform.
struct PickerFit {
    double scale = 1, ox = 0, oy = 0;  // px per metre; where page (0, 0) lands
    V2 px(V2 p) const { return {ox + p.x * scale, oy - p.y * scale}; }
};
// The canvas's bands, in line heights: its padding, the tool row along the bottom (view switch, Points or knuckle rows,
// Swap Sides), the top band (the backdrop and pose buttons, HEAD and SPINE, WINGS) and the Hands page's band under the
// hands. The Face page has rows of chips at the bottom instead of the tool row.
inline constexpr double kPickerPad = 0.45, kPickerToolRow = 1.75, kPickerLabelBand = 1.35, kPickerChipRow = 1.55;
// Where the labels may go: the canvas less its padding and the bottom band; chip_rows: the Face page's rows of chips.
PickerRect picker_label_area(PickerPage page, const PickerRect& canvas, double line_h, int chip_rows = 0);
// Where the dots are fitted: the label area less the top band and the Hands page's label band.
PickerRect picker_fit_area(PickerPage page, const PickerRect& canvas, double line_h, int chip_rows = 0);
// Fits the bounding box of pts into r, keeping the aspect ratio, centred.
PickerFit picker_fit(const std::vector<V2>& pts, const PickerRect& r);
// Every place the framing must keep in view: dots, line ends and fingertip circles.
std::vector<V2> picker_extent(const PickerLayout& l);

// The layout on the canvas, in pixels.
struct PickerScreen {
    std::vector<PickerDot> dots;
    std::vector<PickerLine> lines;
    std::vector<PickerCap> caps;  // tip = the circle's centre
};
PickerScreen picker_screen(const PickerLayout& l, const PickerFit& fit, double cap_offset);

// A group's label on the canvas: its group, where it is, and its text.
struct PickerLabel {
    int group;
    PickerRect r;
};
// Places the labels of a page view (Label groups), each near its bones, on the free side of the canvas, clear of every
// dot and line, never over another label or a blocked place (the canvas's own buttons) or outside area. text_w: a
// label's width in pixels; h its height.
std::vector<PickerLabel> picker_labels(const Skeleton& skel, PickerPage page, int view, const PickerScreen& s,
                                       const PickerRect& area, const std::vector<PickerRect>& blocked,
                                       const std::function<double(const std::string&)>& text_w, double h);

// --- clicks -------------------------------------------------------------------------------------------------------
// The bones under m, nearest first: dots within dot_r, then lines within line_r; each bone once.
std::vector<int> picker_hits(const PickerScreen& s, V2 m, double dot_r, double line_r);
// The fingertip circle under m (its group), or -1.
int picker_cap_hit(const PickerScreen& s, V2 m, double r);
// A second click on the same spot takes the next bone under it (stacked dots, crossed arms).
struct PickerCycle {
    V2 at{-1e9, -1e9};
    std::vector<int> ranked;
    int index = 0;
    // The bone a click at m picks from ranked (nearest first): the first, or after a click within slop of the last one
    // over the same bones, the one after the last pick, round again after the last. -1 when ranked is empty.
    int click(V2 m, const std::vector<int>& now_ranked, double slop = 4);
};

// "Right Shin" for mKneeRight, "Left Index 2" for mHandIndex2Left, "Right Lip Corner" for mFaceLipCornerRight.
std::string picker_bone_label(const std::string& bone);

}  // namespace vats
