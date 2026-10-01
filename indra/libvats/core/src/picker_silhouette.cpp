// Viewport Avatar Toolset - style B's silhouette: the Linden body's traced outlines, bent to the body shown.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The outlines are generated data (picker_silhouette_data.inc, from tools/picker_silhouette.py): for each page view,
// closed loops in page space and the chart bones (anchors) they were traced around, in 0.1 mm units.
#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <sstream>

#include "vats/picker.h"

namespace vats {
namespace {

struct SilView {
    int page, view;
    const char* names;        // the anchors' bones, comma separated, in picker_anchors' order
    const short* anchors;     // ax, ay, bx, by per anchor
    int anchor_values;
    const short* points;      // x, y per outline point
    const unsigned short* loop_ends;  // each loop's end, in points
    int loops;
};

#include "picker_silhouette_data.inc"

// An outline point tied to up to two chart bones: along each (t), across it (off) and by weight.
struct Tie {
    int k[2] = {-1, -1};
    double w[2] = {0, 0}, t[2] = {0, 0}, off[2] = {0, 0};
    V2 rel[2];  // for a bone of no length (a face dot): the offset from it
};
struct Bound {
    std::vector<std::vector<Tie>> loops;
    std::vector<PickerLine> ref;
    std::vector<std::string> names;
};

V2 perp(V2 d) { return {-d.y, d.x}; }

Bound bind(const SilView& v) {
    Bound b;
    constexpr double kUnit = 1e-4;
    std::stringstream names(v.names);
    for (std::string n; std::getline(names, n, ',');) b.names.push_back(n);
    for (int i = 0; i + 3 < v.anchor_values; i += 4)
        b.ref.push_back({-1, {v.anchors[i] * kUnit, v.anchors[i + 1] * kUnit}, {v.anchors[i + 2] * kUnit, v.anchors[i + 3] * kUnit}, -1});
    int start = 0;
    for (int l = 0; l < v.loops; ++l) {
        std::vector<Tie> loop;
        for (int p = start; p < v.loop_ends[l]; ++p) {
            const V2 q{v.points[p * 2] * kUnit, v.points[p * 2 + 1] * kUnit};
            // The two nearest bones; the second counts only when it is nearly as near.
            double d0 = 1e18, d1 = 1e18;
            int k0 = -1, k1 = -1;
            for (int k = 0; k < int(b.ref.size()); ++k) {
                const double d = segment_distance(q, b.ref[k].a, b.ref[k].b);
                if (d < d0) d1 = d0, k1 = k0, d0 = d, k0 = k;
                else if (d < d1) d1 = d, k1 = k;
            }
            Tie t;
            if (k0 < 0) {
                loop.push_back(t);
                continue;
            }
            constexpr double kEps = 0.01;
            const double w0 = 1 / ((d0 + kEps) * (d0 + kEps));
            const double w1 = k1 >= 0 && d1 < 2 * d0 + 0.02 ? 1 / ((d1 + kEps) * (d1 + kEps)) : 0;
            const int ks[2] = {k0, k1};
            const double ws[2] = {w0 / (w0 + w1), w1 / (w0 + w1)};
            for (int j = 0; j < 2; ++j) {
                if (ks[j] < 0 || ws[j] <= 0) continue;
                const PickerLine& r = b.ref[ks[j]];
                const V2 d = r.b - r.a;
                const double len = d.length();
                t.k[j] = ks[j], t.w[j] = ws[j];
                if (len < 1e-6) {
                    t.rel[j] = q - r.a;
                } else {
                    const V2 u = d * (1 / len);
                    t.t[j] = (q - r.a).dot(u) / len;
                    t.off[j] = (q - r.a).dot(perp(u));
                }
            }
            loop.push_back(t);
        }
        start = v.loop_ends[l];
        b.loops.push_back(std::move(loop));
    }
    return b;
}

double diag(const std::vector<PickerLine>& ls) {
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (const PickerLine& l : ls)
        for (V2 p : {l.a, l.b}) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    return x1 > x0 ? std::hypot(x1 - x0, y1 - y0) : 1;
}

}  // namespace

std::vector<std::vector<V2>> picker_silhouette(const Skeleton& skel, PickerPage page, int view,
                                               const std::vector<PickerLine>& anchors) {
    static std::map<std::pair<int, int>, Bound> cache;
    const SilView* v = nullptr;
    for (const SilView& s : kSilViews)
        if (s.page == int(page) && s.view == view) v = &s;
    if (!v) return {};
    auto it = cache.find({v->page, v->view});
    if (it == cache.end()) it = cache.emplace(std::make_pair(v->page, v->view), bind(*v)).first;
    const Bound& b = it->second;
    // The anchors must be the same bones in the same order; otherwise the outline is shown as traced.
    bool same = anchors.size() == b.ref.size() && b.names.size() == b.ref.size();
    for (size_t k = 0; same && k < anchors.size(); ++k)
        same = anchors[k].node >= 0 && anchors[k].node < skel.size() && skel[anchors[k].node].name == b.names[k];
    const std::vector<PickerLine>& now = same ? anchors : b.ref;
    const double girth = same ? diag(now) / diag(b.ref) : 1;  // a taller body is drawn wider too
    std::vector<std::vector<V2>> out;
    for (const std::vector<Tie>& loop : b.loops) {
        std::vector<V2> pts;
        for (const Tie& t : loop) {
            V2 p;
            for (int j = 0; j < 2; ++j) {
                if (t.k[j] < 0) continue;
                const PickerLine& r = now[size_t(t.k[j])];
                const V2 d = r.b - r.a;
                const double len = d.length();
                const PickerLine& ref = b.ref[size_t(t.k[j])];
                const V2 rd = ref.b - ref.a;
                if (rd.length() < 1e-6) {
                    p = p + (r.a + t.rel[j] * girth) * t.w[j];
                } else {
                    const V2 u = len > 1e-6 ? d * (1 / len) : rd * (1 / rd.length());
                    p = p + (r.a + d * t.t[j] + perp(u) * (t.off[j] * girth)) * t.w[j];
                }
            }
            pts.push_back(p);
        }
        out.push_back(std::move(pts));
    }
    return out;
}

}  // namespace vats
