// Viewport Avatar Toolset - soft-body volumes: SL's collision volumes that fitted mesh and avatar physics move.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 1b (RM-9, RM-10). A model's butt, breast, belly and love-handle helper bones go on these
// volumes (rig_map.h); the shape sliders size them and SL's avatar physics bounces BELLY, BUTT and the PECs.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "vats/dae.h"
#include "vats/skeleton.h"

namespace vats {

// The soft-body volumes in the Bones list's order, with the names people know them by ("Butt" for BUTT). The .anim and
// the mapping keep SL's names.
struct SoftBodyPart {
    const char* volume;
    const char* name;
};
const std::vector<SoftBodyPart>& soft_body_parts();
// The friendly name of a soft-body volume, nullptr for anything else.
const char* soft_body_name(std::string_view volume);

// RM-10 Share. On a loaded model, the flesh weighted both to a soft-body volume and to the joint beside it (a breast's
// edge between LEFT_PEC and mChest, a cheek between BUTT and the thigh) can follow either more. The partner is the joint
// whose weights share the most of the volume's vertices, with the volumes that joint carries (mHipLeft with L_UPPER_LEG),
// and for a volume in the middle both sides' (BUTT: mHipLeft and mHipRight); another joint than the one the volume hangs
// from wins when it shares at least half as much, since that is the flesh that tears or follows as the body moves. SK-40
// indices (dae.h); empty for none.
std::vector<int> soft_body_partner(const Skeleton& skel, const DaeModel& model, int volume);
// The partner's joints by name: "mHipLeft and mHipRight".
std::string soft_body_partner_name(const Skeleton& skel, const std::vector<int>& partner);
// Moves the weight of each vertex weighted to both the volume and its partner between the two: share 0 puts all of it on
// the partner, 0.5 leaves it as loaded, 1 puts all of it on the volume, and between moves it evenly. Other weights, the
// sum and the number of joints per vertex stay; the four are kept largest first. volume: its SK-40 index.
void share_soft_body(const Skeleton& skel, DaeModel& model, int volume, double share);

}  // namespace vats
