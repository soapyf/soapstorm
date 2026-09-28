// Viewport Avatar Toolset - face tracking: VTuber blendshapes onto Bento face bones.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 MC-5. SL animations carry no blendshapes, so each tracked shape (ARKit "perfect
// sync" names; VRM presets through aliases) becomes Bento face-bone motion from a data table
// (data/retarget/face-arkit.json). Weights arrive in VmcState::blend (VMC /VMC/Ext/Blend/*, or
// iFacialMocap's own UDP text) and are keyed into the same takes as body capture (MC-3).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vats/clip.h"

namespace vats {

struct VmcState;

struct FaceTable {
    struct Motion {
        std::string bone;
        Vec3 rot;  // degrees at weight 1 (VATs Euler, as key_euler takes)
        Vec3 pos;  // metres at weight 1 (as key_offset takes)
        bool has_rot = false, has_pos = false;
    };
    std::string name;
    std::map<std::string, std::vector<Motion>> shapes;           // ARKit shape -> bone motion at weight 1
    std::map<std::string, std::map<std::string, double>> aliases;  // lowercase preset name -> ARKit weights
    std::map<std::string, std::map<std::string, double>> presets;  // gain presets; "*" = every shape
    // Gaze, per side (0 left, 1 right): every bone in eyes gets the eye rotation (the classic and the
    // Bento eyes); each lid follows that fraction of the eye's pitch. The eyeLook shapes drive eyes[0].
    struct Gaze {
        std::vector<std::string> eyes;
        std::map<std::string, double> lids;
    };
    Gaze gaze[2];

    // Every bone the table moves, sorted (what a "face parts only" take writes).
    std::vector<std::string> bones() const;
};

// Reads a "vats-face-table" JSON. False with err on a bad table.
bool parse_face_table(std::string_view json, FaceTable& out, std::string& err);

// A sender's shape name in the table's ARKit spelling: "eyeBlink_L" (iFacialMocap) and "EyeBlinkLeft"
// (VRM perfect sync) both become "eyeBlinkLeft".
std::string arkit_name(std::string_view name);

struct FaceSettings {
    double gain = 1;                     // on every shape
    std::map<std::string, double> gains;  // per ARKit shape, times gain
    std::map<std::string, double> neutral;  // calibration: the performer's resting weights
    // Key the table's bone offsets. Off by default: most SL heads are mesh heads with their own face joint
    // positions, which the table's offsets (made for the SL default head) pull out of shape.
    bool positions = false;
    bool head = true;  // iFacialMocap: key mHead from its head rotation (VMC senders send the head as a bone)
    double eye_gain = 1;                         // on the gaze, whatever it comes from
    double eye_yaw_max = 25, eye_pitch_max = 20;  // degrees either way
};

// Raw sender weights (0..1) -> ARKit weights: aliases expanded, the neutral face taken out
// ((raw - neutral) / (1 - neutral), floored at 0), gains applied, clamped to 0..1.5.
std::map<std::string, double> face_weights(const FaceTable& table, const std::map<std::string, float>& raw,
                                           const FaceSettings& settings);

// Keys every table bone at frame from weights (bones at rest where nothing moves them), and mHead from
// s.face_head when settings.head and the sender gave one. The eyes use the sender's eye rotations when it
// sends them (s.has_eyes, or VMC LeftEye/RightEye bones), else the eyeLook shapes; then eye_gain, the
// limits, and the lids following the pitch.
void key_face(Clip& clip, const FaceTable& table, const VmcState& s, const FaceSettings& settings, double frame);

// The Linden head's own expression morphs (avatar_head.llm) from ARKit weights, for a face drawn on the system head
// (the face cam, spec 09 build 20): Blink_Left / Blink_Right (eyeBlink*), Express_Open_Mouth (jawOpen), Express_Smile
// (mouthSmile*), Express_Frown (mouthFrown*) and Express_Kiss (mouthPucker). The system head is not weighted to the
// face bones, so these carry the expression there. Weights 0..1 in steps of 1/20, so a caller rebuilds a mesh only
// when they change; morphs at 0 are left out.
std::vector<std::pair<std::string, float>> linden_head_morphs(const std::map<std::string, double>& arkit);

// --- iFacialMocap -------------------------------------------------------------------------------
// Format from the developer page (ifacialmocap.com/for-developer): the PC sends kIFacialMocapHello to
// the iPhone's port 49983 by UDP; the phone then streams "name-value|...|=head#rx,ry,rz,px,py,pz|
// rightEye#x,y,z|leftEye#x,y,z|" at 60 fps to the PC's port 49983; values 0-100, angles in degrees.
// Version 2 (hello + "|sendDataVersion=v2") uses '&' between name and value.
constexpr int kIFacialMocapPort = 49983;
constexpr const char* kIFacialMocapHello = "iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719";

// Applies one packet: weights into s.blend (0..1), the head rotation into s.face_head. False when it
// is not an iFacialMocap packet.
bool apply_ifacialmocap(std::string_view packet, VmcState& s);

}  // namespace vats
