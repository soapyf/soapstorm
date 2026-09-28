// Viewport Avatar Toolset - live motion capture over the VMC protocol (OSC over UDP).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 3 (MC-2..MC-4). The network lives in the app; this is parsing, the
// Unity-to-SL conversion, recording and writing keys, all built on retargeting (07).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/curve_filter.h"
#include "vats/facecap.h"
#include "vats/retarget.h"

namespace vats {

// --- OSC 1.0 ------------------------------------------------------------------------------------

struct OscArg {
    char type = 0;       // the type tag: i f s b h d t T F N I ...
    double num = 0;      // i, f, h, d, and 1/0 for T/F
    std::string str;     // s, S, and the bytes of b
};

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};

// Decodes a packet (a message, or a bundle of messages and bundles). Messages decoded before an
// error are kept in out; returns false if anything was malformed.
bool parse_osc(const std::uint8_t* data, size_t size, std::vector<OscMessage>& out);

// --- VMC ----------------------------------------------------------------------------------------

// Unity is left-handed with Y up and Z forward; SL is right-handed with Z up and X forward.
// Positions: SL = (z, -x, y). A reflection changes handedness, so rotations map as pseudovectors.
Vec3 unity_to_sl(const Vec3& v);
Quat unity_to_sl(const Quat& q);

// The latest state of a VMC sender, already in SL axes.
struct VmcState {
    std::map<std::string, Xform> bones;  // Unity humanoid bone name -> local transform
    Xform root;                          // /VMC/Ext/Root/Pos
    int loaded = -1;                     // /VMC/Ext/OK: 1 = a model is loaded, -1 = never said
    // Face (MC-5): blendshape weights 0..1 as the sender names them. VMC sends them as /Blend/Val
    // and latches them with /Blend/Apply; iFacialMocap sends whole frames.
    std::map<std::string, float> blend, blend_pending;
    Quat face_head;  // iFacialMocap's head rotation (VMC senders send the head as a bone)
    bool has_face_head = false;
    Quat eye_left, eye_right;  // iFacialMocap's eye rotations (VMC senders send LeftEye/RightEye bones)
    bool has_eyes = false;
    // On a rest pose: the performer stood in it (Capture Rest Pose Now), so takes measure hip travel from it.
    bool captured = false;
};

// Applies one message; false when it is not a VMC message this handles.
bool apply_vmc(const OscMessage& m, VmcState& s);

// A rest pose for s: the same bone positions with identity rotations, which is a T-pose for
// VRM-normalised models (what VMC senders drive), with the hips standing on the floor under s's
// feet (a crouch in s does not lower them).
VmcState vmc_t_pose(const VmcState& s);

// Frames as a SourceAnim for retargeting (07): joints are the bones rest knows, parented by the
// Unity humanoid hierarchy; the root is folded into the hips. Bones missing from a frame hold rest.
// The hips' travel is measured from origin (default: rest) standing on the floor under its feet: its
// hips over the ground, at rest's height of the hips above the feet.
SourceAnim vmc_source(const VmcState& rest, const std::vector<VmcState>& frames, double fps,
                      const VmcState* origin = nullptr);

// --- Rokoko Studio Live -------------------------------------------------------------------------

// Rokoko Studio's custom streaming default port.
constexpr int kRokokoPort = 14043;

// An LZ4 frame (magic 04 22 4D 18), as Studio sends JSON v3 by default. Block and content checksums
// are skipped, not verified. False when data is not a valid LZ4 frame.
bool lz4_frame_decompress(const std::uint8_t* data, size_t size, std::string& out);

// Applies one JSON v3 packet (LZ4-compressed or plain JSON). The body joints are world transforms
// in Unity axes; they become Unity humanoid bones with local transforms, as VMC sends, so the VRM
// humanoid table and everything after it are shared. Takes the actor named `actor`, or the first
// actor with a body when it is empty; `actor_out` gets the one used. False with err on a bad packet.
bool apply_rokoko(const std::uint8_t* data, size_t size, VmcState& s, const std::string& actor,
                  std::string& actor_out, std::string& err);

// --- Recording (MC-3) ---------------------------------------------------------------------------

struct MocapRecorder {
    double fps = 30;
    int from = 0, to = -1;  // punch-in range in clip frames; to = -1: until stopped
    double start = -1;      // time the first frame is taken (after the countdown); -1 = idle
    std::vector<VmcState> frames;

    void begin(double now, double countdown_s, double fps_, int from_, int to_);
    bool active() const { return start >= 0; }
    bool counting_down(double now) const { return active() && now < start; }
    // Takes frames up to now, repeating the latest state across gaps. Returns false once the
    // punch range is full (recording is then over; frames holds exactly to - from + 1).
    bool feed(double now, const VmcState& s);
    void stop() { start = -1; }
};

// --- Writing keys (MC-3, MC-4) ------------------------------------------------------------------

struct MocapCleanup {
    int smooth = 0;          // box filter radius in frames on the source rotations; 0 = off (old settings)
    bool use_filter = false; // MC-4a: filter the take's curves with `filter` instead (smooth is then ignored)
    FilterSettings filter;
    bool reduce = true;      // key reduction after retargeting
    double rot_deg = 0.5, pos_m = 0.002;
    int blend = 4;           // frames at each punch edge eased from the old animation into the take
    bool lock_feet = false;  // foot-contact clean-up (07 RT-9) over the take
    bool heel_toe = true;    // its heel and toe contacts (08 FC); off = the ankle only
};

// Retargets the recorded frames with table and writes them into clip from frame `from`, replacing
// keys in that range. only_tracks non-empty limits it to those bones; other tracks are untouched.
// mPelvis's offset is the hips' travel from rest when rest.captured, else from where the first frame
// stands (vmc_source). With cleanup.use_filter the report gives the take's shake (jerk RMS) before and after
// the filter: the mean over its bones and the three shakiest. Returns report lines.
std::vector<std::string> merge_recording(Clip& clip, const Skeleton& skel, const RigTable& table, const VmcState& rest,
                                         const std::vector<VmcState>& frames, int from,
                                         const std::vector<std::string>& only_tracks, const MocapCleanup& cleanup,
                                         const Shape* shape = nullptr, const FaceTable* face = nullptr,
                                         const FaceSettings& face_settings = {});

// One live frame retargeted: the tracks to show (each a single key at frame 0), for previews.
// With face, the face bones (and iFacialMocap's head) come from now's blendshapes too; a sender with
// no body bones gives a face-only pose.
Clip live_pose(const Skeleton& skel, const RigTable& table, const VmcState& rest, const VmcState& now,
               const Shape* shape = nullptr, const FaceTable* face = nullptr, const FaceSettings& face_settings = {});

}  // namespace vats
