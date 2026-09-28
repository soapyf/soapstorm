// Viewport Avatar Toolset - operations on keys: insert, move, scale, flip, handles, filters, reverse.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/02 sections 2.3 (AM-28), 2.5 (AM-43), 2.11, 2.12, 3.3-3.5 and docs/spec/05
// section 2.9-2.10. Every function leaves automatic handles recomputed unless it says otherwise.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

// A key by curve and position in that curve's (sorted) key list.
struct KeyRef {
    std::string track, channel;
    int index = 0;
    bool operator==(const KeyRef&) const = default;
};

// Splits the curve at frame without changing its shape (AM-28, 02 section 3.4). If a key already
// exists at the frame, returns it unchanged. Outside the keyed range or on a non-Bezier segment it
// sets a key with the evaluated value. Returns the key's index.
int insert_on_curve(FCurve& curve, double frame);

// Moves the selected keys (and their handles) by (dframe, dvalue) from their state in at_press,
// so repeated calls during a drag do not drift. The whole selection's time delta is clamped so no
// key goes below frame 0 (spacing is kept; spec 05 section 5 item 6). snap rounds dframe to whole
// frames. Curves are re-sorted; sel is updated to the keys' new indices. Duplicates are NOT merged
// here (call finish_transform on release).
// Unselected Breakdown keys keep their share of the time between the nearest non-Breakdown keys around them when
// one of those moves (spec 08 KT-3). Such a key no longer sits where at_press has it, so a drag that calls this again
// and again passes press_sel, the selection as indices into at_press; without it the selection is worked out from sel.
void move_keys(Clip& clip, const Clip& at_press, std::vector<KeyRef>& sel, double dframe, double dvalue, bool snap,
               const std::vector<KeyRef>* press_sel = nullptr);

// Scales the selected keys about (pivot_frame, pivot_value) from their state in at_press. A negative
// sx reverses them in time with the rules of 02 section 3.5. snap rounds each resulting frame.
void scale_keys(Clip& clip, const Clip& at_press, std::vector<KeyRef>& sel, double pivot_frame, double pivot_value,
                double sx, double sy, bool snap);

// Flip Time about the centre of the selection's own time span (AM-116, TG-83), then merges.
void flip_time(Clip& clip, std::vector<KeyRef>& sel, bool snap);
// Flip Values: value and handle y multiplied by -1 about 0 (TG-84).
void flip_values(Clip& clip, std::vector<KeyRef>& sel);

// Ends a move or scale: on each touched curve, keys sharing a frame are merged and the selected
// (moved) key wins (E-1), handles are recomputed, and sel is updated.
void finish_transform(Clip& clip, std::vector<KeyRef>& sel);

// Drags one handle of a key to (frame, value) (AM-117, TG-93): a left handle cannot pass its key to
// the right, a right handle cannot pass it to the left. Unless the key's side is Free, both sides
// become Aligned and the opposite handle rotates to stay collinear, keeping its own length.
void drag_handle(FCurve& curve, int key, bool right_side, double frame, double value);

// Applies a tangent command to each selected key (AM-22).
void apply_tangent(Clip& clip, const std::vector<KeyRef>& sel, Tangent t);

// Deletes the selected keys. Empty curves and tracks are removed.
void delete_keys(Clip& clip, const std::vector<KeyRef>& sel);

// Euler filter over the rotation channels of the given tracks (AM-43, with 02 section 5 item 7:
// no keys are added). Each key moves to the equivalent angle nearest the previous key: a 360-degree
// shift per channel, or, where all three channels share every key frame, the equivalent triple
// (x+180, 180-y, z+180) when that is closer (the y curve is mirrored so its shape is kept). Returns how
// many tracks changed.
int euler_filter(Clip& clip, const std::vector<std::string>& tracks);

// Changes the frame rate keeping timing (decision 7, E-16 "keep duration"): every key frame and handle x,
// end_frame, loop points and pin frames scale by new_fps / fps. Results within 1e-6 of a whole frame snap
// to it, so 30 -> 24 -> 30 returns integer-frame keys exactly. Keep-frames is just setting clip.fps.
void retime_clip(Clip& clip, int new_fps);

// Reverses the whole clip in time (AM-115), including loop points and every pin's range and
// bookkeeping frames (E-2, E-3: keys beyond end_frame are reflected correctly, never negative).
// Stepped runs stay lossless: a key keeps its own value and a Constant "sliver" key a tiny gap
// later carries the held value, so every whole frame is exact and a second reverse (or Flip Time)
// folds the sliver back and returns the original curves.
void reverse_clip(Clip& clip);

}  // namespace vats
