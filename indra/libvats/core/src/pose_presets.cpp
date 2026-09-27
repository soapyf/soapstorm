// Viewport Avatar Toolset - the built-in starter poses.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/pose_presets.h"

#include <string>
#include <utility>

namespace vats {
namespace {

// One digit, in degrees. c1..c3 curl the segments toward the palm (MCP, PIP, DIP; for the thumb
// CMC, MCP, IP); negative is extension. spread swings segment 1 sideways from its rest direction,
// positive toward the thumb side. oppose (thumb only) swings segment 1 down in front of the palm,
// roll (thumb only) turns it about its own length so the pad faces the fingers.
// The rest skeleton's fingers fan out (index about 25 degrees toward the thumb, pinky about 44 away),
// so fingers held together take spreads of roughly index -22, ring +15, pinky +35.
struct Digit {
    double c1 = 0, c2 = 0, c3 = 0, spread = 0, oppose = 0, roll = 0;
};

struct HandShape {
    const char* slug;
    const char* name;
    Digit thumb, index, middle, ring, pinky;
};

// Fingers curled into the palm, used by every pose that tucks some of them away.
constexpr Digit kFistIndex{75, 90, 50, -18}, kFistMiddle{70, 92, 55, 0}, kFistRing{75, 92, 52, 12},
    kFistPinky{85, 88, 45, 30};
// A thumb lying across the curled index and middle fingers.
constexpr Digit kFistThumb{9, 54, 42, 0, 27, 30};

// clang-format off
const HandShape kHands[] = {
    // slug              name                   thumb: c1 c2 c3 spread oppose roll  index: c1 c2 c3 spread  middle               ring                 pinky
    {"hand-relaxed",     "Relaxed",             {5, 10, 10, -8, 15, 10},    {10, 18, 8, -12},   {15, 22, 10, 0},    {20, 28, 14, 8},    {25, 32, 16, 18}},
    {"hand-rest",        "Rest",                {3, 5, 5, -6, 10, 5},       {6, 10, 5, -12},    {8, 12, 6, 0},      {10, 15, 8, 8},     {12, 18, 10, 18}},
    {"hand-open",        "Open (Spread)",       {-5, -5, -5, 20, 0, 0},     {-5, 0, 0, 6},      {-5, 0, 0, 0},      {-5, 0, 0, -6},     {-5, 0, 0, -8}},
    {"hand-flat",        "Flat",                {0, 0, 0, -22, 5, 0},       {0, 0, 0, -22},     {0, 0, 0, 0},       {0, 0, 0, 15},      {0, 0, 0, 35}},
    {"hand-fist",        "Fist",                kFistThumb,                 kFistIndex,         kFistMiddle,        kFistRing,          kFistPinky},
    {"hand-loose-fist",  "Loose Fist",          {5, 20, 15, -10, 15, 15},   {55, 70, 40, -18},  {55, 72, 42, 0},    {60, 72, 42, 12},   {65, 70, 40, 28}},
    {"hand-point",       "Point",               kFistThumb,                 {0, 5, 0, -12},     kFistMiddle,        kFistRing,          kFistPinky},
    {"hand-point-thumb", "Point (Thumb Up)",    {0, 0, -5, 5, 0, 0},        {0, 5, 0, -12},     kFistMiddle,        kFistRing,          kFistPinky},
    {"hand-peace",       "Peace (V)",           {12, 52, 40, -3, 28, 31},   {0, 0, 0, 4},       {0, 0, 0, -12},     kFistRing,          kFistPinky},
    {"hand-thumbs-up",   "Thumbs Up",           {0, -5, -10, 35, -5, 0},    kFistIndex,         kFistMiddle,        kFistRing,          kFistPinky},
    {"hand-ok",          "OK",                  {5, 27, 16, 2, 38, 23},     {40, 50, 25, -8},   {10, 15, 8, -4},    {15, 20, 10, 6},    {20, 25, 12, 14}},
    {"hand-pinch",       "Pinch",               {10, 18, 8, -3, 34, 24},    {35, 45, 20, -8},   {45, 60, 35, -2},   {55, 70, 42, 10},   {62, 72, 42, 26}},
    {"hand-pinch-loose", "Pinch (Loose)",       {9, 13, 5, -4, 25, 21},      {30, 38, 15, -8},   {35, 50, 28, -2},   {42, 58, 34, 10},   {50, 62, 36, 24}},
    {"hand-grip",        "Grip (Cylinder)",     {9, 23, 12, 1, 45, 19},     {25, 50, 35, -18},  {25, 55, 40, 0},    {28, 55, 40, 12},   {32, 50, 35, 28}},
    {"hand-glass",       "Hold Glass (Stem)",   {8, 20, 10, -2, 31, 25},    {45, 55, 30, -12},  {50, 60, 35, -2},   {70, 85, 50, 10},   {75, 85, 45, 28}},
    {"hand-phone",       "Hold Phone",          {5, 5, 0, -8, 45, 15},      {20, 65, 30, -18},  {20, 65, 30, 0},    {24, 65, 30, 12},   {28, 62, 30, 28}},
    {"hand-cup",         "Cup (C-Shape)",       {0, 5, 5, 10, 30, 10},      {25, 30, 15, -20},  {25, 30, 15, 0},    {25, 30, 15, 14},   {25, 30, 15, 32}},
    {"hand-claw",        "Claw",                {0, 30, 40, 10, 15, 0},     {0, 80, 60, 0},     {0, 80, 60, 0},     {0, 80, 60, 0},     {0, 80, 60, 0}},
    {"hand-horns",       "Rock (Horns)",        kFistThumb,                 {0, 5, 0, -5},      kFistMiddle,        kFistRing,          {0, 5, 0, 5}},
    {"hand-call-me",     "Call Me",             {0, -5, -5, 25, 0, 0},      kFistIndex,         kFistMiddle,        kFistRing,          {0, 5, 0, -5}},
    {"hand-pistol",      "Pistol (Finger Gun)", {0, 0, 0, 35, 0, 0},        {0, 5, 0, -24},     {0, 5, 0, 3},      kFistRing,          kFistPinky},
    {"hand-salute",      "Salute",              {0, 0, 0, -25, 10, 5},      {0, 0, 0, -24},     {0, 0, 0, 0},       {0, 0, 0, 17},      {0, 0, 0, 38}},
    {"hand-wave",        "Wave",                {0, 0, 5, 12, 0, 0},        {5, 5, 3, 4},       {5, 5, 3, 0},       {5, 5, 3, -4},      {5, 5, 3, -4}},
    {"hand-count-1",     "Counting 1",          kFistThumb,                 {0, 0, 0, -12},     kFistMiddle,        kFistRing,          kFistPinky},
    {"hand-count-2",     "Counting 2",          {12, 52, 40, -3, 28, 31},   {0, 0, 0, -8},      {0, 0, 0, -4},      kFistRing,          kFistPinky},
    {"hand-count-3",     "Counting 3",          {19, 52, 38, -7, 30, 34},   {0, 0, 0, -8},      {0, 0, 0, 0},       {0, 0, 0, 6},       kFistPinky},
    {"hand-count-4",     "Counting 4",          {10, 63, 47, -6, 23, 45},   {0, 0, 0, -8},      {0, 0, 0, 0},       {0, 0, 0, 6},       {0, 0, 0, 14}},
    {"hand-count-5",     "Counting 5",          {0, 0, 0, 10, 0, 0},        {0, 0, 0, -6},      {0, 0, 0, 0},       {0, 0, 0, 4},       {0, 0, 0, 12}},
    {"hand-typing",      "Typing",              {5, 10, 10, -5, 30, 5},     {28, 46, 15, -10},  {18, 56, 22, 0},    {18, 56, 22, 8},    {30, 43, 14, 18}},
    {"hand-surface",     "Resting on Surface",  {0, 5, 5, -5, 5, 0},        {5, 15, 5, -12},    {5, 15, 5, 0},      {5, 15, 5, 8},      {8, 15, 5, 18}},
    {"hand-pen",         "Holding Pen",         {8, 20, 10, -2, 31, 25},    {35, 40, 10, -10},  {45, 55, 25, -5},   {60, 70, 40, 10},   {65, 75, 45, 25}},
};
// clang-format on

// Whole-body poses: Euler degrees per bone. Symmetric poses list the left side only and get the right
// side mirrored. Arms point along +-Y in the rest pose, so shoulder X lowers them and elbow Z bends
// the forearm forward; the numbers were fitted to target wrist and elbow positions.
struct BodyPose {
    const char* slug;
    const char* name;
    bool symmetric;
    std::vector<std::pair<const char*, Vec3>> bones;
};

// clang-format off
const BodyPose kBodies[] = {
    {"body-stand", "Relaxed Stand", true,
     {{"mCollarLeft", {-5, 0, 0}}, {"mShoulderLeft", {-78, 0, 0}}, {"mElbowLeft", {0, 0, -12}}}},
    {"body-hips", "Hands on Hips", true,
     {{"mShoulderLeft", {-84, 42, -67}}, {"mElbowLeft", {0, -31, -91}}, {"mWristLeft", {39, 13, -14}}}},
    {"body-arms-crossed", "Arms Crossed", false,
     {{"mShoulderLeft", {-59, -16, -80}}, {"mElbowLeft", {0, 0, -87}},
      {"mShoulderRight", {56, -24, 76}}, {"mElbowRight", {0, 0, 85}}}},
    {"body-thinking", "Thinking", false,
     {{"mShoulderLeft", {-69, -14, -74}}, {"mElbowLeft", {0, 0, -67}},
      {"mShoulderRight", {68, -38, 55}}, {"mElbowRight", {0, 14, 132}}, {"mWristRight", {-14, 5, -11}},
      {"mNeck", {0, 6, 0}}, {"mHead", {-4, 6, 0}}}},
    {"body-wave", "Waving", false,
     {{"mCollarLeft", {-5, 0, 0}}, {"mShoulderLeft", {-78, 0, 0}}, {"mElbowLeft", {0, 0, -12}},
      {"mCollarRight", {-5, 0, 0}}, {"mShoulderRight", {49, -74, -30}}, {"mElbowRight", {0, 9, 91}},
      {"mWristRight", {-8, 11, 6}}, {"mHead", {3, 0, -5}}}},
    {"body-sit", "Sitting", true,
     {{"mHipLeft", {0, -90, 0}}, {"mKneeLeft", {0, 90, 0}},
      {"mShoulderLeft", {-78, -26, -23}}, {"mElbowLeft", {0, 0, -20}}}},
    // Weight on the right leg: hips drop to the left, shoulders tilt back the other way.
    {"body-contrapposto", "Contrapposto", false,
     {{"mPelvis", {-4, 0, 5}}, {"mHipRight", {4, 0, -5}}, {"mHipLeft", {1, -10, -5}}, {"mKneeLeft", {0, 20, 0}},
      {"mAnkleLeft", {0, -10, 0}}, {"mTorso", {4, 0, -3}}, {"mChest", {3, 0, -2}}, {"mHead", {-4, 2, 4}},
      {"mCollarLeft", {-5, 0, 0}}, {"mShoulderLeft", {-80, 0, 0}}, {"mElbowLeft", {0, 0, -12}},
      {"mCollarRight", {5, 0, 0}}, {"mShoulderRight", {76, 0, 0}}, {"mElbowRight", {0, 0, 15}}}},
};
// clang-format on

const char* const kDigits[] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};

// Rotations for one left-hand digit, by the hand poser's rules (AM-38).
void add_digit(const Skeleton& skel, LibraryItem& it, const char* digit, const Digit& d) {
    bool thumb = std::string(digit) == "Thumb";
    Vec3 palm = thumb ? Vec3{-0.7, 0, -1}.normalized() : Vec3{0, 0, -1};
    const double curls[3] = {d.c1, d.c2, d.c3};
    for (int n = 1; n <= 3; ++n) {
        std::string bone = std::string("mHand") + digit + std::to_string(n) + "Left";
        int node = skel.find(bone);
        if (node < 0) continue;
        Vec3 dir = skel[node].end.normalized();
        Quat q = Quat::axis_angle(dir.cross(palm), curls[n - 1] * kDegToRad);
        if (n == 1) {
            q = Quat::axis_angle(dir, d.roll * kDegToRad) * q;
            q = Quat::axis_angle(dir.cross({0, 0, -1}), d.oppose * kDegToRad) * q;
            q = Quat::axis_angle(dir.cross({1, 0, 0}), d.spread * kDegToRad) * q;
        }
        it.bones[bone] = quat_to_euler(q);
    }
}

LibraryItem hand_item(const Skeleton& skel, const HandShape& h) {
    LibraryItem it;
    it.id = std::string("builtin:") + h.slug;
    it.name = h.name;
    it.kind = "hand";
    it.side = "Left";
    const Digit* digits[] = {&h.thumb, &h.index, &h.middle, &h.ring, &h.pinky};
    for (int i = 0; i < 5; ++i) add_digit(skel, it, kDigits[i], *digits[i]);
    return it;
}

LibraryItem body_item(const Skeleton& skel, const BodyPose& b) {
    LibraryItem it;
    it.id = std::string("builtin:") + b.slug;
    it.name = b.name;
    it.kind = "pose";
    for (auto& [bone, e] : b.bones) {
        it.bones[bone] = e;
        int src = skel.find(bone), dst = skel.find(Skeleton::mirror_name(bone));
        if (b.symmetric && src >= 0 && dst >= 0 && dst != src)
            it.bones[skel[dst].name] = quat_to_euler(mirror_rotation(skel, src, dst, euler_to_quat(e)));
    }
    return it;
}

}  // namespace

const std::vector<LibraryItem>& builtin_poses(const Skeleton& skel) {
    static const std::vector<LibraryItem> items = [&] {
        std::vector<LibraryItem> v;
        for (auto& h : kHands) v.push_back(hand_item(skel, h));
        for (auto& b : kBodies) v.push_back(body_item(skel, b));
        return v;
    }();
    return items;
}

}  // namespace vats
