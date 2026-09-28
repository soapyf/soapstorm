// Viewport Avatar Toolset - operations on keys: insert, move, scale, flip, handles, filters, reverse.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/curve_ops.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <utility>

#include "vats/edit.h"

namespace vats {
namespace {

using CurveId = std::pair<std::string, std::string>;

struct Tagged {
    Key key;
    bool sel = false;
};

bool by_frame(const Tagged& a, const Tagged& b) { return a.key.frame < b.key.frame; }

bool automatic(Handle h) { return h != Handle::Aligned && h != Handle::Free; }

double bezier(double p0, double p1, double p2, double p3, double t) {
    double u = 1 - t;
    return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

void shift(Key& k, double df, double dv) {
    k.frame += df;
    k.lx += df;
    k.rx += df;
    k.value += dv;
    k.ly += dv;
    k.ry += dv;
}

FCurve* curve_of(Clip& clip, const CurveId& id) {
    auto t = clip.curves.find(id.first);
    if (t == clip.curves.end()) return nullptr;
    auto c = t->second.find(id.second);
    return c == t->second.end() ? nullptr : &c->second;
}

const FCurve* curve_of(const Clip& clip, const CurveId& id) { return curve_of(const_cast<Clip&>(clip), id); }

std::map<CurveId, std::vector<int>> by_curve(const std::vector<KeyRef>& sel) {
    std::map<CurveId, std::vector<int>> m;
    for (auto& r : sel) m[{r.track, r.channel}].push_back(r.index);
    return m;
}

// Sorts the keys into the curve, recomputes its handles and appends the selected keys' indices to sel.
void store(FCurve& curve, std::vector<Tagged>& keys, const CurveId& id, std::vector<KeyRef>& sel) {
    std::stable_sort(keys.begin(), keys.end(), by_frame);
    curve.keys.clear();
    for (size_t i = 0; i < keys.size(); ++i) {
        curve.keys.push_back(keys[i].key);
        if (keys[i].sel) sel.push_back({id.first, id.second, static_cast<int>(i)});
    }
    curve.recompute_handles();
}

// A drag's selection as keys of at_press. The unselected keys of the live curve are untouched copies of at_press
// keys (same frame), so every at_press key not among them is a selected one.
// With press_sel (indices into at_press) the selection is taken from it as it is.
struct Pressed {
    CurveId id;
    FCurve* now;
    const FCurve* base;
    std::vector<bool> picked;
    std::vector<double> slide;  // frames each unpicked key moves by (08 KT-3), empty = none
};

std::vector<Pressed> pressed(Clip& clip, const Clip& at_press, const std::vector<KeyRef>& sel,
                             const std::vector<KeyRef>* press_sel = nullptr) {
    std::vector<Pressed> out;
    if (press_sel) {
        for (auto& [id, idx] : by_curve(*press_sel)) {
            FCurve* now = curve_of(clip, id);
            const FCurve* base = curve_of(at_press, id);
            if (!now || !base) continue;
            std::vector<bool> picked(base->keys.size(), false);
            for (int i : idx)
                if (i >= 0 && i < static_cast<int>(picked.size())) picked[i] = true;
            out.push_back({id, now, base, std::move(picked), {}});
        }
        return out;
    }
    for (auto& [id, idx] : by_curve(sel)) {
        FCurve* now = curve_of(clip, id);
        const FCurve* base = curve_of(at_press, id);
        if (!now || !base) continue;
        std::vector<bool> is_sel(now->keys.size(), false);
        for (int i : idx)
            if (i >= 0 && i < static_cast<int>(is_sel.size())) is_sel[i] = true;
        std::vector<bool> picked(base->keys.size(), true);
        for (size_t i = 0; i < now->keys.size(); ++i) {
            if (is_sel[i]) continue;
            double f = now->keys[i].frame;
            auto it = std::lower_bound(base->keys.begin(), base->keys.end(), f,
                                       [](const Key& k, double v) { return k.frame < v; });
            if (it != base->keys.end() && it->frame == f) picked[it - base->keys.begin()] = false;
        }
        out.push_back({id, now, base, std::move(picked), {}});
    }
    return out;
}

// Rebuilds each pressed curve from at_press, with edit applied to its selected keys (given in time order).
template <class F>
void rebuild(std::vector<Pressed>& ps, std::vector<KeyRef>& sel, F&& edit) {
    sel.clear();
    for (auto& p : ps) {
        std::vector<Key> moving;
        std::vector<Tagged> keys;
        for (size_t j = 0; j < p.base->keys.size(); ++j) {
            if (p.picked[j]) {
                moving.push_back(p.base->keys[j]);
            } else {
                Key k = p.base->keys[j];
                if (!p.slide.empty()) shift(k, p.slide[j], 0);
                keys.push_back({k, false});
            }
        }
        edit(moving);
        for (auto& k : moving) keys.push_back({k, true});
        store(*p.now, keys, p.id, sel);
    }
}

// A reversed stepped segment must still read its old end key's value at that key's frame, yet hold its old held
// value right after, and a key's value is what holds after it. So a key whose held value differs from its own value
// is stored as the key plus a "sliver" key a fraction of a frame later that holds (05 section 5 item 8). The gap is
// under a frame, so every whole frame (what export samples) reads exactly right, and a second reversal folds the
// sliver back into the key it came from, restoring the original keys.
//
// A sliver is also a neighbour, and Auto, AutoClamped and Plateau handles take their slope from both neighbours. So
// the key before a sliver has its left handle frozen (Aligned) and the key after it its right handle, which keeps
// their Bezier segments exact; the sliver's own handle types, which shape nothing, remember the frozen types.
double sliver_gap(double frame) { return std::max(1e-3, 2e-5 * std::fabs(frame)); }  // > same_frame's tolerance

bool sloped(Handle h) { return h == Handle::Auto || h == Handle::AutoClamped || h == Handle::Plateau; }

bool is_sliver(const Key& k, const Key& before) {
    return before.interp == Interp::Constant && k.interp == Interp::Constant && (sloped(k.left) || k.left == Handle::Flat) &&
           (sloped(k.right) || k.right == Handle::Flat) && k.frame - before.frame <= 2 * sliver_gap(before.frame);
}

struct Held {
    Key key;
    double hold;  // the value key's segment holds when key.interp is Constant
};

// Keys in time order with each sliver folded into the key before it. A sliver needs a key after it, because the
// last key's value also holds after the curve.
std::vector<Held> fold(std::vector<Key> ks) {
    std::vector<Held> hs;
    for (size_t i = 0; i < ks.size(); ++i) {
        hs.push_back({ks[i], ks[i].value});
        if (i + 2 >= ks.size() || !is_sliver(ks[i + 1], ks[i])) continue;
        const Key& s = ks[++i];
        if (sloped(s.left)) hs.back().key.left = s.left;
        if (sloped(s.right)) ks[i + 1].right = s.right;
        hs.back().hold = s.value;
    }
    return hs;
}

// fold's inverse, in time order.
std::vector<Key> unfold(std::vector<Held> hs) {
    std::stable_sort(hs.begin(), hs.end(), [](const Held& a, const Held& b) { return a.key.frame < b.key.frame; });
    std::vector<Key> ks;
    for (size_t i = 0; i < hs.size(); ++i) {
        Key& k = ks.emplace_back(hs[i].key);
        if (k.interp != Interp::Constant || hs[i].hold == k.value || i + 1 == hs.size()) continue;
        Key s;
        s.frame = k.frame + sliver_gap(k.frame);
        s.value = s.ly = s.ry = hs[i].hold;
        s.interp = Interp::Constant;
        s.left = s.right = Handle::Flat;
        s.lx = s.frame - 1;
        s.rx = s.frame + 1;
        if (sloped(k.left)) s.left = std::exchange(k.left, Handle::Aligned);
        if (Handle& next = hs[i + 1].key.right; sloped(next)) s.right = std::exchange(next, Handle::Aligned);
        ks.push_back(s);
    }
    return ks;
}

// 02 section 3.5 on keys in their original time order whose frames are already mirrored: handles swap sides, each
// key takes its predecessor's interp (the first takes the last's) and, where that was a step, the value it held.
// Unlike the spec, a key keeps its own value (it now starts the held segment), which is what makes this lossless.
void reverse_rules(std::vector<Held>& hs) {
    if (hs.empty()) return;
    std::vector<Held> old = hs;
    for (size_t j = 0; j < hs.size(); ++j) {
        Key& k = hs[j].key;
        std::swap(k.lx, k.rx);
        std::swap(k.ly, k.ry);
        std::swap(k.left, k.right);
        const Held& prev = old[j == 0 ? old.size() - 1 : j - 1];
        k.interp = prev.key.interp;
        hs[j].hold = j > 0 && prev.key.interp == Interp::Constant ? prev.hold : k.value;
    }
}

}  // namespace

int insert_on_curve(FCurve& curve, double frame) {
    int found = curve.find(frame);
    if (found >= 0) return found;
    auto& keys = curve.keys;
    auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](double v, const Key& k) { return v < k.frame; });
    if (it == keys.begin() || it == keys.end() || (it - 1)->interp != Interp::Bezier) {
        curve.set_key(frame, curve.evaluate(frame));
        return curve.find(frame);  // not set_key's result: its index is unreliable when the insert reallocates
    }

    int b = static_cast<int>(it - keys.begin());
    Key& A = keys[b - 1];
    Key& B = keys[b];
    double x1 = std::clamp(A.rx, A.frame, B.frame), x2 = std::clamp(B.lx, A.frame, B.frame);
    double lo = 0, hi = 1, t = (frame - A.frame) / (B.frame - A.frame);
    for (int i = 0; i < 40; ++i) {
        double x = bezier(A.frame, x1, x2, B.frame, t);
        if (std::fabs(x - frame) < 1e-6) break;
        (x < frame ? lo : hi) = t;
        t = (lo + hi) * 0.5;
    }
    // de Casteljau (02 section 3.4).
    double q0x = lerp(A.frame, x1, t), q1x = lerp(x1, x2, t), q2x = lerp(x2, B.frame, t);
    double q0y = lerp(A.value, A.ry, t), q1y = lerp(A.ry, B.ly, t), q2y = lerp(B.ly, B.value, t);
    double r0x = lerp(q0x, q1x, t), r1x = lerp(q1x, q2x, t);
    double r0y = lerp(q0y, q1y, t), r1y = lerp(q1y, q2y, t);

    Key k;
    k.frame = frame;
    k.value = lerp(r0y, r1y, t);
    k.interp = Interp::Bezier;
    k.left = k.right = Handle::Aligned;
    k.lx = r0x;
    k.ly = r0y;
    k.rx = r1x;
    k.ry = r1y;

    // Freeze the neighbours so no later recompute undoes the split.
    auto freeze = [](Handle& facing, Handle& other) {
        if (automatic(facing))
            facing = (other == Handle::Vector || other == Handle::Free) ? Handle::Free : Handle::Aligned;
        if (automatic(other)) other = Handle::Aligned;
    };
    freeze(A.right, A.left);
    freeze(B.left, B.right);
    A.rx = q0x;
    A.ry = q0y;
    B.lx = q2x;
    B.ly = q2y;
    keys.insert(keys.begin() + b, k);
    return b;
}

void move_keys(Clip& clip, const Clip& at_press, std::vector<KeyRef>& sel, double dframe, double dvalue, bool snap,
               const std::vector<KeyRef>* press_sel) {
    auto ps = pressed(clip, at_press, sel, press_sel);
    double lo = std::numeric_limits<double>::infinity();
    for (auto& p : ps)
        for (size_t j = 0; j < p.picked.size(); ++j)
            if (p.picked[j]) lo = std::min(lo, p.base->keys[j].frame);
    if (snap) dframe = std::round(dframe);
    if (lo + dframe < 0) dframe = snap ? std::ceil(-lo) : -lo;
    // 08 KT-3: an unpicked Breakdown key keeps its share of the time between the nearest keys around it that are not
    // Breakdowns, when either of them moves.
    for (auto& p : ps) {
        const std::vector<Key>& ks = p.base->keys;
        const int n = int(ks.size());
        for (int j = 0; j < n; ++j) {
            if (p.picked[j] || ks[j].tag != KeyTag::Breakdown) continue;
            int a = j - 1, b = j + 1;
            while (a >= 0 && ks[a].tag == KeyTag::Breakdown) --a;
            while (b < n && ks[b].tag == KeyTag::Breakdown) ++b;
            if (a < 0 || b >= n || !(p.picked[a] || p.picked[b])) continue;
            const double fa = ks[a].frame + (p.picked[a] ? dframe : 0), fb = ks[b].frame + (p.picked[b] ? dframe : 0);
            double f = fa + (ks[j].frame - ks[a].frame) / (ks[b].frame - ks[a].frame) * (fb - fa);
            if (snap) f = std::round(f);
            p.slide.resize(n, 0);
            p.slide[j] = f - ks[j].frame;
        }
    }
    rebuild(ps, sel, [&](std::vector<Key>& ks) {
        for (auto& k : ks) shift(k, dframe, dvalue);
    });
}

void scale_keys(Clip& clip, const Clip& at_press, std::vector<KeyRef>& sel, double pivot_frame, double pivot_value,
                double sx, double sy, bool snap) {
    auto ps = pressed(clip, at_press, sel);
    auto fx = [&](double f) { return pivot_frame + (f - pivot_frame) * sx; };
    auto fy = [&](double v) { return pivot_value + (v - pivot_value) * sy; };
    rebuild(ps, sel, [&](std::vector<Key>& ks) {
        auto hs = fold(ks);
        for (auto& [k, hold] : hs) {
            hold = fy(hold);
            k.frame = fx(k.frame);
            k.lx = fx(k.lx);
            k.rx = fx(k.rx);
            k.value = fy(k.value);
            k.ly = fy(k.ly);
            k.ry = fy(k.ry);
            if (snap) shift(k, std::round(k.frame) - k.frame, 0);
            if (k.frame < 0) shift(k, -k.frame, 0);
        }
        if (sx < 0) reverse_rules(hs);
        ks = unfold(std::move(hs));
    });
}

void flip_time(Clip& clip, std::vector<KeyRef>& sel, bool snap) {
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (auto& r : sel) {
        const FCurve* c = curve_of(clip, {r.track, r.channel});
        if (!c || r.index < 0 || r.index >= int(c->keys.size())) continue;
        lo = std::min(lo, c->keys[r.index].frame);
        hi = std::max(hi, c->keys[r.index].frame);
    }
    if (lo > hi) return;
    Clip before = clip;
    scale_keys(clip, before, sel, (lo + hi) * 0.5, 0, -1, 1, snap);
    finish_transform(clip, sel);
}

void flip_values(Clip& clip, std::vector<KeyRef>& sel) {
    for (auto& [id, idx] : by_curve(sel)) {
        FCurve* c = curve_of(clip, id);
        if (!c) continue;
        for (int i : idx) {
            if (i < 0 || i >= int(c->keys.size())) continue;
            Key& k = c->keys[i];
            k.value = -k.value;
            k.ly = -k.ly;
            k.ry = -k.ry;
        }
        c->recompute_handles();
    }
}

void finish_transform(Clip& clip, std::vector<KeyRef>& sel) {
    auto groups = by_curve(sel);
    sel.clear();
    for (auto& [id, idx] : groups) {
        FCurve* c = curve_of(clip, id);
        if (!c) continue;
        std::vector<Tagged> keys;
        for (auto& k : c->keys) keys.push_back({k, false});
        for (int i : idx)
            if (i >= 0 && i < int(keys.size())) keys[i].sel = true;
        std::stable_sort(keys.begin(), keys.end(), by_frame);
        std::vector<Tagged> merged;
        for (auto& t : keys) {
            if (!merged.empty() && same_frame(merged.back().key.frame, t.key.frame)) {
                if (t.sel || !merged.back().sel) merged.back() = t;  // the moved key wins
            } else {
                merged.push_back(t);
            }
        }
        store(*c, merged, id, sel);
    }
}

void drag_handle(FCurve& curve, int key, bool right_side, double frame, double value) {
    if (key < 0 || key >= int(curve.keys.size())) return;
    Key& k = curve.keys[key];
    frame = right_side ? std::max(frame, k.frame) : std::min(frame, k.frame);
    Handle& mine = right_side ? k.right : k.left;
    Handle& other = right_side ? k.left : k.right;
    double& ox = right_side ? k.lx : k.rx;
    double& oy = right_side ? k.ly : k.ry;
    (right_side ? k.rx : k.lx) = frame;
    (right_side ? k.ry : k.ly) = value;
    if (mine != Handle::Free) {
        mine = Handle::Aligned;
        if (other != Handle::Free) {
            other = Handle::Aligned;
            double dx = k.frame - frame, dy = k.value - value, d = std::hypot(dx, dy);
            double len = std::hypot(ox - k.frame, oy - k.value);
            if (d > 0) {  // a zero-length handle has no direction: keep the other one (E-19)
                ox = k.frame + dx / d * len;
                oy = k.value + dy / d * len;
            }
        }
    }
    curve.recompute_handles();
}

void apply_tangent(Clip& clip, const std::vector<KeyRef>& sel, Tangent t) {
    for (auto& r : sel)
        if (FCurve* c = curve_of(clip, {r.track, r.channel}); c && r.index >= 0 && r.index < int(c->keys.size()))
            c->apply_tangent(r.index, t);
}

void delete_keys(Clip& clip, const std::vector<KeyRef>& sel) {
    for (auto& [id, idx] : by_curve(sel)) {
        FCurve* c = curve_of(clip, id);
        if (!c) continue;
        std::sort(idx.begin(), idx.end(), std::greater<>());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        for (int i : idx)
            if (i >= 0 && i < int(c->keys.size())) c->keys.erase(c->keys.begin() + i);
        c->recompute_handles();
    }
    prune(clip);
}

namespace {

// The three rotation curves of a track when they all have keys on exactly the same frames.
bool shared_frames(Track& t, FCurve* (&c)[3]) {
    for (int a = 0; a < 3; ++a) {
        auto it = t.find(kRotChannels[a]);
        if (it == t.end() || it->second.empty()) return false;
        c[a] = &it->second;
    }
    for (int a = 1; a < 3; ++a) {
        if (c[a]->keys.size() != c[0]->keys.size()) return false;
        for (size_t i = 0; i < c[0]->keys.size(); ++i)
            if (!same_frame(c[a]->keys[i].frame, c[0]->keys[i].frame)) return false;
    }
    return true;
}

// Moves the keys of one frame to the triple nearest ref: the same angles or (x+180, 180-y, z+180), each
// wrapped by 360. The y key of a flipped triple is mirrored (value and handles), keeping its shape.
bool fix_triple(Key* (&k)[3], const Vec3& ref) {
    Vec3 cur{k[0]->value, k[1]->value, k[2]->value};
    Vec3 alt{cur.x + 180, 180 - cur.y, cur.z + 180};
    // Exact multiples of 360 from the unflipped and flipped angles to their copies nearest ref.
    Vec3 wa, wb, da, db;
    for (int i = 0; i < 3; ++i) {
        wa[i] = std::round((wrap_near(cur[i], ref[i]) - cur[i]) / 360) * 360;
        wb[i] = std::round((wrap_near(alt[i], ref[i]) - alt[i]) / 360) * 360;
        da[i] = cur[i] + wa[i] - ref[i];
        db[i] = alt[i] + wb[i] - ref[i];
    }
    bool flip = db.dot(db) < da.dot(da) - 1e-6;
    if (flip) {
        shift(*k[0], 0, 180 + wb.x);
        double c = 180 + wb.y;  // y -> c - y, handles too: the mirrored key keeps its shape
        k[1]->value = c - k[1]->value;
        k[1]->ly = c - k[1]->ly;
        k[1]->ry = c - k[1]->ry;
        shift(*k[2], 0, 180 + wb.z);
        return true;
    }
    for (int i = 0; i < 3; ++i) shift(*k[i], 0, wa[i]);
    return wa != Vec3{};
}

}  // namespace

int euler_filter(Clip& clip, const std::vector<std::string>& tracks) {
    int changed = 0;
    for (auto& name : tracks) {
        auto t = clip.curves.find(name);
        if (t == clip.curves.end()) continue;
        bool any = false;
        FCurve* c[3];
        if (shared_frames(t->second, c)) {
            for (size_t i = 1; i < c[0]->keys.size(); ++i) {
                Key* k[3] = {&c[0]->keys[i], &c[1]->keys[i], &c[2]->keys[i]};
                any |= fix_triple(k, {c[0]->keys[i - 1].value, c[1]->keys[i - 1].value, c[2]->keys[i - 1].value});
            }
            if (any)
                for (FCurve* f : c) f->recompute_handles();
        } else {
            // ponytail: keys on different frames per channel get 360-degree shifts only; flipping those
            // would need keys added (AM-43 as written). Upgrade: insert_on_curve at the union of frames.
            for (const char* ch : kRotChannels) {
                auto it = t->second.find(ch);
                if (it == t->second.end()) continue;
                auto& keys = it->second.keys;
                bool moved = false;
                for (size_t i = 1; i < keys.size(); ++i) {
                    double d = std::round((wrap_near(keys[i].value, keys[i - 1].value) - keys[i].value) / 360) * 360;
                    if (d == 0) continue;
                    shift(keys[i], 0, d);
                    moved = true;
                }
                if (moved) it->second.recompute_handles();
                any |= moved;
            }
        }
        changed += any;
    }
    return changed;
}

void retime_clip(Clip& clip, int new_fps) {
    if (new_fps <= 0 || clip.fps <= 0 || new_fps == clip.fps) {
        if (new_fps > 0) clip.fps = new_fps;
        return;
    }
    const double r = double(new_fps) / clip.fps;
    auto frame = [&](double f) {
        double x = f * new_fps / clip.fps, n = std::round(x);
        return std::abs(x - n) <= 1e-6 ? n : x;
    };
    auto whole = [&](int f) { return f < 0 ? f : int(std::lround(f * r)); };  // -1 = none stays -1
    for (auto& [track, channels] : clip.curves)
        for (auto& [ch, c] : channels) {
            for (Key& k : c.keys) {
                k.frame = frame(k.frame);
                k.lx = frame(k.lx);
                k.rx = frame(k.rx);
            }
            c.recompute_handles();
        }
    clip.end_frame = whole(clip.end_frame);
    clip.loop_in = whole(clip.loop_in);
    clip.loop_out = whole(clip.loop_out);
    for (Pin& p : clip.pins) {
        p.from = whole(p.from);
        p.to = whole(p.to);
        p.start_key = whole(p.start_key);
        p.release_key = whole(p.release_key);
    }
    clip.fps = new_fps;
}

void reverse_clip(Clip& clip) {
    const int end = clip.end_frame;
    for (auto& [name, track] : clip.curves) {
        for (auto& [channel, c] : track) {
            if (c.empty()) continue;
            // Keys beyond the end would land on negative frames (E-2): cut the curve at the end first.
            if (c.keys.back().frame > end && !same_frame(c.keys.back().frame, end)) {
                insert_on_curve(c, end);
                c.keys.erase(std::remove_if(c.keys.begin(), c.keys.end(),
                                            [&](const Key& k) { return k.frame > end && !same_frame(k.frame, end); }),
                             c.keys.end());
            }
            auto hs = fold(c.keys);
            for (auto& h : hs) {
                h.key.frame = end - h.key.frame;
                h.key.lx = end - h.key.lx;
                h.key.rx = end - h.key.rx;
            }
            reverse_rules(hs);
            c.keys = unfold(std::move(hs));
            c.recompute_handles();
        }
    }

    int in = clip.loop_in, out = clip.loop_out;
    clip.loop_in = std::max(0, end - out);
    clip.loop_out = std::max(0, end - in);

    auto remap = [&](int f) { return f < 0 || f > end ? -1 : end - f; };
    for (auto& p : clip.pins) {
        int from = std::clamp(p.from, 0, end), to = p.to < 0 ? end : std::clamp(p.to, 0, end);
        p.from = end - to;
        p.to = from == 0 ? -1 : end - from;
        p.start_key = remap(p.start_key);
        p.release_key = remap(p.release_key);
    }
}

}  // namespace vats
