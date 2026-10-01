// Viewport Avatar Toolset - the SL .anim (keyframe motion) file, byte level.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Mirrors LLKeyframeMotion::serialize/deserialize in the SL viewer. Spec: docs/spec/03 section 3.1.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/math.h"
#include "vats/skeleton.h"

namespace vats {

constexpr float kAnimMaxDuration = 60.0f;   // MAX_ANIM_DURATION
constexpr float kAnimMaxOffset = 5.0f;      // LL_MAX_PELVIS_OFFSET
constexpr std::uint32_t kAnimMaxJoints = 216;  // LL_CHARACTER_MAX_ANIMATED_JOINTS
constexpr std::int32_t kAnimMaxConstraints = 10;
// The SL upload server refuses animation assets of this size or more (tested in-world 2026-09-26).
constexpr std::size_t kAnimMaxUploadBytes = 250000;

struct AnimJoint {
    std::string name;
    std::int32_t priority = -1;
    // Version 1.0: quantised {time, x, y, z}. Version 0.1: floats {time, x, y, z}
    // (rotation as Euler degrees). Only the vectors matching the file version are used.
    std::vector<std::array<std::uint16_t, 4>> rot, pos;
    std::vector<std::array<float, 4>> rot_legacy, pos_legacy;
};

struct AnimFile {
    std::uint16_t version = 1, sub_version = 0;
    std::int32_t base_priority = 3;
    float duration = 0;
    std::string emote;
    float loop_in = 0, loop_out = 0;
    std::int32_t loop = 0;
    float ease_in = 0, ease_out = 0;
    std::uint32_t hand_pose = 1;
    std::vector<AnimJoint> joints;
    std::int32_t num_constraints = 0;
    std::vector<AnimConstraint> constraints;  // present only when num_constraints is 0..10
    std::vector<std::uint8_t> trailing;       // any bytes after that, kept for byte-exact round trips

    bool legacy() const { return version == 0 && sub_version == 1; }
};

// Byte-level read and write. parse_anim fails only on structure (truncation, bad version).
bool parse_anim(const std::vector<std::uint8_t>& bytes, AnimFile& out, std::string& err);
std::vector<std::uint8_t> write_anim(const AnimFile& f);

// Every reason the viewer would refuse this file (empty = accepted). With for_upload the
// viewer's stricter upload-preview rules apply (unknown joint names are refused).
std::vector<std::string> validate_anim(const AnimFile& f, const Skeleton& skel, bool for_upload);

// One of the 19 emote names SL resolves (the viewer silently clears any other name).
bool is_known_emote(const std::string& name);

// The viewer's quantiser (llquantize.h), in binary32 like the viewer.
std::uint16_t f32_to_u16(float v, float lo, float hi);
float u16_to_f32(std::uint16_t u, float lo, float hi);
// What LLKeyframeMotion::serialize writes for a vector component: quantise, decode, quantise.
std::uint16_t anim_code(float v, float lo, float hi);

// Key codecs, bit-compatible with the viewer.
std::array<std::uint16_t, 3> encode_rotation(const Quat& q);
std::array<std::uint16_t, 3> encode_position(const Vec3& p);
Quat decode_rotation(const std::array<std::uint16_t, 4>& key);
Vec3 decode_position(const std::array<std::uint16_t, 4>& key);

// A value that anim_code turns back into code: the middle of the code's range. The viewer's
// quantiser is not stable on its own grid (re-encoding a decoded value can drop 1-2 codes),
// so imports use these values to make export(import(file)) reproduce the file's codes.
// Falls back to the decoded value for the few codes the viewer can never produce.
float stable_value(std::uint16_t code, float lo, float hi);
Quat stable_rotation(const std::array<std::uint16_t, 4>& key);
Vec3 stable_position(const std::array<std::uint16_t, 4>& key);

}  // namespace vats
