// Viewport Avatar Toolset - putting any rigged model on SL's skeleton: its own armature mapped onto SL's joints.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 1b (RM-1..RM-6). The readers report the file's own bones (DaeReport::bones); a mapping
// names an SL joint for each, and the readers load the skin again through it (SkinRemap): the joints stay where the
// model put them and the weights move to the mapped joints. The result is an ordinary rigged DaeModel, so it poses as
// a mesh body and exports for SL (rig_export.h) like any other.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "vats/auto_rig.h"
#include "vats/dae.h"
#include "vats/json.h"
#include "vats/retarget.h"

namespace vats {

struct RigMapBone {
    std::string source;  // the file's bone (SourceBone::name)
    // The SL joint (or collision volume) it maps to; "" for none: a bone with weights folds into its nearest mapped
    // ancestor's joint, one without (an IK target, a pole, an _end tip) is dropped.
    std::string target;
    int confidence = 0;  // 0..100: how sure the suggestion is; 100 when the user chose it, or the bone has an SL name
    std::string reason;  // why, in a few words
    // RM-8: the spare SL chain (SpareSlot::id) the bone rides, "" for none. Its joint then comes from where it sits
    // along its chain (use_spare_chain), and target only names it for the list.
    std::string spare;
};

struct RigMap {
    std::vector<RigMapBone> bones;  // one per source bone, in the file's order
    double height = 0;              // the size in metres the model is made (see along_ground); 0 keeps the file's own
    // How the size is measured: false, floor to top (an upright body); true, the longer side of its footprint, nose to
    // tail or wingtip to wingtip (a body that lies along its spine: four legs, a bird, a fish).
    bool along_ground = false;
    int turn = 0;                   // quarter turns about Z (after the up axis) that face the model along SL's +X
    // Four legs and a level spine. Layout: false puts the front legs on the arms (mCollar..mWrist) and the back legs on
    // the legs, which most SL animations and AOs move; true (Bento) puts the back legs on mHindLimb1..4 and the front
    // legs on the legs.
    bool quadruped = false, bento = false;
    std::string facing;             // why that turn
    std::vector<std::string> notes; // the suggestion's other decisions, explained (the leg chains, merges)
    std::map<std::string, std::string> spare_labels;  // RM-8: spare slot id -> what it holds ("scarf")
    // RM-10: soft-body volume -> its share (soft_body.h share_soft_body), applied as the weights load; a volume left out
    // keeps the weights as the rig splits them (0.5).
    std::map<std::string, double> share;
    // SK-3: the model's parts hidden and its shape key values, kept with the model so a re-import shows it the same. A
    // mapping with no bones only carries this: the model loads as it is, rigged to SL's own names.
    MeshLook look;
    // RG-14: a model rigged from scratch (its own armature, if any, set aside): its markers, joints and weights. height
    // and turn stand it up (scratch_placement); bones is then empty.
    ScratchRig scratch;

    const RigMapBone* find(std::string_view source) const;
    RigMapBone* find(std::string_view source);
};

// The SL-like height a mapping scales to by default, in metres: SL's default avatar (the Linden body in its SL Default
// shape) from its soles to the top of its head, as the model's height is measured (its vertex bounds).
inline constexpr double kSlAvatarHeight = 1.865;

// RM-2: a mapping suggested from the bones alone. The rig tables (data/retarget) first; when none fits a humanoid,
// name keywords and the armature's shape: chains, branching, mirrored pairs, sides, and where bones sit. Height is set
// to kSlAvatarHeight. tables may be empty. bento: a quadruped's layout (RigMap::bento).
RigMap suggest_rig_map(const Skeleton& skel, const std::vector<SourceBone>& bones, const std::vector<RigTable>& tables,
                       bool bento = false);

// What looks wrong in a mapping, per bone in the file's order ("" when nothing): a bone that is not below a bone on the
// nearest mapped SL joint above its own (nor beside one), or a weighted bone that shares its joint with a weighted bone
// it does not sit with (palms side by side, or split hips, merge; a twist bone a joint down does not). The suggestion
// checks itself this way and lowers the confidence of what it flags; the Map Rig window shows them red as you edit.
std::vector<std::string> check_rig_map(const Skeleton& skel, const std::vector<SourceBone>& bones, const RigMap& map);

// The model's size as map measures it (RigMap::along_ground), facing +X as a mapped model does.
double rig_map_size(const DaeModel& model, const RigMap& map);

// The same name for the other side ("UpperArm.L" <-> "UpperArm.R", "LeftHand" <-> "RightHand"); "" when it has none.
std::string mirror_bone_name(std::string_view name);

// RM-3: what a mapping does to the file's bones (the height is applied after loading: load_rig_mapped).
struct RigMapResult {
    SkinRemap remap;
    std::vector<int> node;             // per bone: the SK-40 index its weights go to; -1 when dropped
    std::vector<int> folded_into;      // per bone: the bone it folds into, else -1
    std::vector<bool> places;          // per bone: its bind places its joint (the most weighted bone mapped to it)
    std::vector<std::string> folded;   // "Twist.L into UpperArm.L (mShoulderLeft)", one per bone that folds
    std::vector<std::string> dropped;  // the bones dropped, by name
    std::vector<std::string> problems; // targets VATs does not know
    // Weights spread along a line once loaded (spread_spare_weights): a spare chain with more bones than its slot has
    // joints (RM-8), or a butt helper on one side shared between BUTT and the leg.
    struct Resample {
        std::vector<int> nodes;        // its SL joints, root to tip (SK-40)
        std::vector<Vec3> line;        // the bones' heads and the last one's tip, turned as the loaded model is
        std::vector<double> at;        // per point of line: the length along it
        std::vector<double> from, to;  // per node: the stretch of line its own bones cover
        int only = -1;                 // when set, the one node whose weight it spreads (a butt helper loaded on a leg)
    };
    std::vector<Resample> resample;
    std::map<std::string, std::string> labels;  // RM-8: SL joint -> what its spare chain holds
};
RigMapResult resolve_rig_map(const Skeleton& skel, const std::vector<SourceBone>& bones, const RigMap& map);

// ---------------------------------------------------------------------------------------------
// RM-8 Spare chains. SL's Bento chains a body seldom uses (the wings, the tail, the hind limbs, the groin, the extra
// spine joints, the tongue and the ears) carry parts SL has no joint for: a scarf on a wing, a ponytail on the tail, a
// skirt on the hind limbs. The uploader takes joint positions, so the chain's joints go where the part's own bones
// are, and whatever animates the chain then moves the part.

struct SpareSlot {
    std::string id;                   // "WingRight", as the mapping file names it
    std::string name;                 // "right wing"
    std::vector<std::string> joints;  // its SL joints, root to tip
    std::string worn;                 // what else commonly drives it in-world; "" for nothing common
    bool carries_body = false;        // the body hangs from it (mSpine1..4): bending it bends everything above
};
const std::vector<SpareSlot>& spare_slots();
const SpareSlot* find_spare_slot(std::string_view id);
// The slot's bones in the file's order (root to tip), from RigMapBone::spare.
std::vector<int> spare_chain_bones(const std::vector<SourceBone>& bones, const RigMap& map, const std::string& slot);
// True when no bone of map is on any of the slot's joints, other than the chain on the slot itself.
bool spare_slot_free(const RigMap& map, const SpareSlot& slot);
// From start down the one child that carries weights at or below it, until the chain branches or its weights end.
std::vector<int> spare_chain_from(const std::vector<SourceBone>& bones, int start);
// A label from the chain's bone names, without numbers or sides: "scarfb.003" -> "scarfb", "Hair_Tuft_L" -> "hair tuft".
std::string spare_label_for(const std::vector<SourceBone>& bones, const std::vector<int>& chain);
// Puts chain (bones root to tip) on the slot, at confidence 100; the bones the slot held before, and any other bone on
// its joints, fold again. Up to as many bones as the slot has joints, each takes the next joint at its own head; with
// more, the joints are spaced evenly along the chain and the weights spread between them (spread_spare_weights), so
// the chain still bends smoothly rather than in a few stiff pieces. label: "" takes spare_label_for.
void use_spare_chain(RigMap& map, const std::vector<SourceBone>& bones, const std::vector<int>& chain,
                     const std::string& slot, std::string label = "");
// The slot's bones fold into their parents again.
void clear_spare_chain(RigMap& map, const std::string& slot);
// What a spare chain moves with: the SL joint the model hangs it from (its first bone's nearest mapped ancestor's), and
// the one SL hangs the slot from (the nearest joint above it that the mapping uses). When they differ the part stays
// with SL's when the model's turns: a scarf tied at the neck, on a wing, which hangs from mChest. "" for none.
struct SpareHang {
    std::string model, sl;
};
SpareHang spare_hang(const Skeleton& skel, const std::vector<SourceBone>& bones, const RigMapResult& resolved,
                     const std::vector<int>& chain, const SpareSlot& slot);

// Spare-chain presets: mapping fragments kept for the next model ("scarf on right wing", "skirt on hind limbs").
struct SparePreset {
    struct Chain {
        std::string slot, label;
        std::vector<std::string> bones;  // the model's bone names, root to tip; empty in a built-in
    };
    std::string name;
    std::vector<Chain> chains;
};
// Scarf on right wing, ponytail on tail, skirt on hind limbs.
const std::vector<SparePreset>& builtin_spare_presets();
// The mapping's spare chains as a preset (only slot's, when given).
SparePreset spare_preset_from(const RigMap& map, const std::vector<SourceBone>& bones, const std::string& name,
                              const std::string& slot = "");
// Puts a preset's chains on the mapping. Each chain starts at its first bone found by name: exactly, else by its letters
// alone ("scarfb.003" matches "Scarf_B_03"), the first in the chain that matches; else (a built-in) at start, and a
// second chain at start's mirror-named bone (a skirt's other side). A chain found that way goes on its slot's side of
// the body, where start sits. A slot the model's own bones use is skipped. Returns how many chains it put on.
int apply_spare_preset(RigMap& map, const std::vector<SourceBone>& bones, const SparePreset& preset, int start = -1);
// {"vats-spare-presets": 1, "presets": [{"name": ..., "chains": [{"slot": ..., "label": ..., "bones": [...]}]}]}
std::string write_spare_presets_json(const std::vector<SparePreset>& presets);
bool parse_spare_presets_json(std::string_view text, std::vector<SparePreset>& out, std::string& err);
// A Resample on the loaded model: each vertex weighted to the chain finds its place along the line (the nearest point
// within the stretch its joints' own bones cover), and its chain weight goes to the joints either side of it, all on
// one joint at the middle of its bone and half and half at the next joint. Its other weights stay; at most four,
// summing to 1.
void spread_spare_weights(const Skeleton& skel, const RigMapResult::Resample& chain, DaeModel& model);

// SK-3: a look as the mapping file and the project write it, in an object: "hidden": ["Scarf", ...] and
// "shape_keys": {"Body - Obese": 1, ...}, each only when not empty. read_mesh_look reads them back (others ignored).
void write_mesh_look(const MeshLook& look, Json& into);
void read_mesh_look(const Json& from, MeshLook& out);

// RM-5: a mapping beside its model: "<folder>/<stem>.rigmap.json".
std::string rig_map_path(const std::string& model_path);
// {"vats-rig-map": 1, "height": 1.9, "turn": 1, "bones": {"Body": "mPelvis", "Foot.L_end": "", ...}}, and for a body
// lying along its spine "size": "along the ground", for a quadruped "layout": "bento" or "front legs on arms", and for
// spare chains "spares": {"WingRight": {"label": "scarf", "bones": ["Scarf.001", ...]}}, and for soft-body volumes
// shared otherwise than the rig splits them "share": {"BUTT": 0.3}. A model rigged from scratch: "scratch": {"markers":
// {"wrist_l": [x, y, z, confidence], ...}, "groups": [...], "fitted": true, "transfer": {"Vest": "Body"}, "joints":
// {"mPelvis": [x, y, z], ...}, "vertices": n, "weights": {"<SK-40 index>": [vertex, weight, vertex, weight, ...], ...}}.
std::string write_rig_map_json(const RigMap& map);
// The bones come out in the file's order; confidence 100 and reason "from the mapping file".
bool parse_rig_map_json(std::string_view text, RigMap& out, std::string& err);
// False with err empty when there is no such file.
bool read_rig_map_file(const std::string& path, RigMap& out, std::string& err);

// The model at path through map: read once as it is (its bones), then again through the resolved mapping, and scaled
// to map.height (a body lying along the ground also stood on the floor: its lowest point at z 0). report.bones keeps the file's own bones; report.warnings notes what folded. bones: the file's bones
// when the caller has them already (the first read is skipped).
bool load_rig_mapped(const std::string& path, const Skeleton& skel, const RigMap& map, DaeModel& out, DaeReport& report,
                     std::string& err, const std::vector<SourceBone>* bones = nullptr);

// RG-14: the model at path rigged from scratch by map.scratch: read as it is, its own armature set aside, stood up by
// map.turn and map.height, and rigged (rig_from_scratch). report.scratch is set; a warning when the saved weights are
// for another version of the file (the model then has nearest-bone weights until they are computed again).
bool load_scratch_rigged(const std::string& path, const Skeleton& skel, const RigMap& map, DaeModel& out, DaeReport& report,
                         std::string& err);
// A rig's saved weights made whole for this skeleton: empty slots (-1) and joints it does not have become mRoot at
// weight 0, the rest renormalised to sum to 1. Weights for another vertex count are dropped (false).
bool settle_scratch_weights(const Skeleton& skel, ScratchRig& rig, int vertices);

// RM-4: whether a model read as it is needs mapping before it can be a mesh body: it has an armature of its own and at
// least half its weighted bones have no SL name (a devkit with a helper bone or two does not).
bool rig_needs_mapping(const Skeleton& skel, const DaeReport& report);

}  // namespace vats
