// Viewport Avatar Toolset - the body picker's layout: clickable regions of an avatar outline and their bones.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/picker.h"

#include <algorithm>

namespace vats {
namespace {

// A region given once for the side drawn on the left of the canvas; with pairs, a sided one gets its mirror image for
// the other side too. Names use "@" for the side: "Right"/"Left" in bone names, "Right "/"Left " in labels.
struct Spec {
    const char* label;
    std::vector<const char*> bones;
    float x, y, w, h;
    bool round = false;
};

std::string sided(const char* s, const char* side) {
    std::string out = s;
    if (auto at = out.find('@'); at != std::string::npos) out.replace(at, 1, side);
    return out;
}

// left_side: the side of the avatar drawn on the canvas's left ("Right" when facing the viewer).
std::vector<PickerRegion> build(const std::vector<Spec>& specs, const char* left_side, const char* right_side,
                                bool pairs = true) {
    std::vector<PickerRegion> out;
    for (const Spec& s : specs) {
        const bool sided_spec = std::string(s.label).find('@') != std::string::npos;
        for (int m = 0; m < (sided_spec && pairs ? 2 : 1); ++m) {
            const char* side = m ? right_side : left_side;
            PickerRegion r;
            r.label = sided(s.label, (std::string(side) + " ").c_str());
            for (const char* b : s.bones) r.bones.push_back(sided(b, side));
            r.x = m ? kPickerWidth - s.x - s.w : s.x;
            r.y = s.y, r.w = s.w, r.h = s.h, r.round = s.round;
            out.push_back(std::move(r));
        }
    }
    return out;
}

// The body facing the viewer: its right side on the canvas's left. The last two groups are front or back only.
const std::vector<Spec> kBody = {
    {"Head", {"mHead"}, 0.41f, 0.00f, 0.18f, 0.15f, true},
    {"Neck", {"mNeck"}, 0.455f, 0.15f, 0.09f, 0.04f},
    {"@Collar", {"mCollar@"}, 0.29f, 0.19f, 0.11f, 0.05f},
    {"Chest", {"mChest"}, 0.40f, 0.19f, 0.20f, 0.12f},
    {"Torso", {"mTorso"}, 0.41f, 0.31f, 0.18f, 0.09f},
    {"Pelvis", {"mPelvis"}, 0.38f, 0.40f, 0.24f, 0.08f},
    {"@Upper Arm", {"mShoulder@"}, 0.21f, 0.20f, 0.08f, 0.15f},
    {"@Forearm", {"mElbow@"}, 0.17f, 0.35f, 0.08f, 0.15f},
    {"@Hand", {"mWrist@"}, 0.13f, 0.50f, 0.09f, 0.08f, true},
    {"@Thigh", {"mHip@"}, 0.38f, 0.48f, 0.085f, 0.22f},
    {"@Shin", {"mKnee@"}, 0.385f, 0.70f, 0.075f, 0.22f},
    {"@Ankle", {"mAnkle@"}, 0.385f, 0.92f, 0.075f, 0.04f},
    {"@Foot", {"mFoot@"}, 0.375f, 0.96f, 0.085f, 0.04f},
    {"@Toes", {"mToe@"}, 0.365f, 1.00f, 0.09f, 0.03f},
};
const std::vector<Spec> kFrontOnly = {
    {"Groin", {"mGroin"}, 0.47f, 0.48f, 0.06f, 0.04f},
};
const std::vector<Spec> kBackOnly = {
    {"@Wing", {"mWing1@", "mWing2@", "mWing3@", "mWing4@", "mWing4Fan@"}, 0.02f, 0.10f, 0.17f, 0.09f, true},
    {"Wings Root", {"mWingsRoot"}, 0.45f, 0.265f, 0.10f, 0.035f},
    {"Tail Base", {"mTail1", "mTail2"}, 0.47f, 0.49f, 0.06f, 0.08f},
    {"Tail Middle", {"mTail3", "mTail4"}, 0.47f, 0.57f, 0.06f, 0.08f},
    {"Tail Tip", {"mTail5", "mTail6"}, 0.47f, 0.65f, 0.06f, 0.08f},
};

// The back of a right hand, fingers up: the thumb on the canvas's left. The left hand is its mirror image.
const std::vector<Spec> kHand = {
    {"@Hand (Wrist)", {"mWrist@"}, 0.26f, 0.78f, 0.48f, 0.44f},
    {"@Thumb 1", {"mHandThumb1@"}, 0.12f, 0.80f, 0.13f, 0.13f, true},
    {"@Thumb 2", {"mHandThumb2@"}, 0.06f, 0.66f, 0.12f, 0.13f, true},
    {"@Thumb 3", {"mHandThumb3@"}, 0.02f, 0.53f, 0.11f, 0.12f, true},
    {"@Index 1", {"mHandIndex1@"}, 0.27f, 0.56f, 0.10f, 0.21f},
    {"@Index 2", {"mHandIndex2@"}, 0.27f, 0.38f, 0.10f, 0.17f},
    {"@Index 3", {"mHandIndex3@"}, 0.27f, 0.22f, 0.10f, 0.15f},
    {"@Middle 1", {"mHandMiddle1@"}, 0.39f, 0.52f, 0.10f, 0.25f},
    {"@Middle 2", {"mHandMiddle2@"}, 0.39f, 0.33f, 0.10f, 0.18f},
    {"@Middle 3", {"mHandMiddle3@"}, 0.39f, 0.16f, 0.10f, 0.16f},
    {"@Ring 1", {"mHandRing1@"}, 0.51f, 0.56f, 0.10f, 0.21f},
    {"@Ring 2", {"mHandRing2@"}, 0.51f, 0.38f, 0.10f, 0.17f},
    {"@Ring 3", {"mHandRing3@"}, 0.51f, 0.22f, 0.10f, 0.15f},
    {"@Pinky 1", {"mHandPinky1@"}, 0.63f, 0.62f, 0.10f, 0.16f},
    {"@Pinky 2", {"mHandPinky2@"}, 0.63f, 0.48f, 0.10f, 0.13f},
    {"@Pinky 3", {"mHandPinky3@"}, 0.63f, 0.36f, 0.10f, 0.11f},
};

// The face looking at the viewer: its right side on the canvas's left.
const std::vector<Spec> kFace = {
    {"Face Root", {"mFaceRoot"}, 0.44f, 0.00f, 0.12f, 0.05f},
    {"@Forehead", {"mFaceForehead@"}, 0.29f, 0.09f, 0.14f, 0.08f},
    {"Forehead Centre", {"mFaceForeheadCenter"}, 0.43f, 0.07f, 0.14f, 0.08f},
    {"@Eyebrow Outer", {"mFaceEyebrowOuter@"}, 0.22f, 0.22f, 0.09f, 0.05f},
    {"@Eyebrow Centre", {"mFaceEyebrowCenter@"}, 0.31f, 0.21f, 0.08f, 0.05f},
    {"@Eyebrow Inner", {"mFaceEyebrowInner@"}, 0.39f, 0.22f, 0.07f, 0.05f},
    {"@Upper Eyelid", {"mFaceEyeLidUpper@"}, 0.27f, 0.285f, 0.15f, 0.04f},
    {"@Eye", {"mEye@"}, 0.30f, 0.33f, 0.09f, 0.06f, true},
    {"@Eye (Alt)", {"mFaceEyeAlt@"}, 0.23f, 0.335f, 0.05f, 0.05f, true},
    {"@Eye Corner", {"mFaceEyecornerInner@"}, 0.40f, 0.335f, 0.04f, 0.05f},
    {"@Lower Eyelid", {"mFaceEyeLidLower@"}, 0.27f, 0.395f, 0.15f, 0.035f},
    {"@Ear", {"mFaceEar1@"}, 0.08f, 0.30f, 0.07f, 0.12f},
    {"@Ear Tip", {"mFaceEar2@"}, 0.06f, 0.18f, 0.07f, 0.11f},
    {"Nose Bridge", {"mFaceNoseBridge"}, 0.46f, 0.30f, 0.08f, 0.11f},
    {"@Nose", {"mFaceNose@"}, 0.39f, 0.43f, 0.07f, 0.07f},
    {"Nose Centre", {"mFaceNoseCenter"}, 0.46f, 0.42f, 0.08f, 0.06f},
    {"Nose Base", {"mFaceNoseBase"}, 0.46f, 0.48f, 0.08f, 0.04f},
    {"@Upper Cheek", {"mFaceCheekUpper@"}, 0.20f, 0.45f, 0.14f, 0.08f},
    {"@Lower Cheek", {"mFaceCheekLower@"}, 0.20f, 0.55f, 0.14f, 0.09f},
    {"@Lip Corner", {"mFaceLipCorner@"}, 0.30f, 0.615f, 0.06f, 0.05f, true},
    {"@Upper Lip", {"mFaceLipUpper@"}, 0.36f, 0.59f, 0.09f, 0.04f},
    {"Upper Lip Centre", {"mFaceLipUpperCenter"}, 0.45f, 0.585f, 0.10f, 0.045f},
    {"@Lower Lip", {"mFaceLipLower@"}, 0.36f, 0.66f, 0.09f, 0.04f},
    {"Lower Lip Centre", {"mFaceLipLowerCenter"}, 0.45f, 0.665f, 0.10f, 0.045f},
    {"Jaw", {"mFaceJaw"}, 0.33f, 0.75f, 0.34f, 0.08f},
    {"Chin", {"mFaceChin"}, 0.43f, 0.83f, 0.14f, 0.06f},
    // Inside the mouth and the shaper: a row below the face.
    {"Upper Teeth", {"mFaceTeethUpper"}, 0.02f, 1.00f, 0.18f, 0.06f},
    {"Tongue Base", {"mFaceTongueBase"}, 0.22f, 1.00f, 0.18f, 0.06f},
    {"Tongue Tip", {"mFaceTongueTip"}, 0.42f, 1.00f, 0.18f, 0.06f},
    {"Lower Teeth", {"mFaceTeethLower"}, 0.62f, 1.00f, 0.18f, 0.06f},
    {"Jaw Shaper", {"mFaceJawShaper"}, 0.82f, 1.00f, 0.16f, 0.06f},
};

std::vector<Spec> cat(std::vector<Spec> a, const std::vector<Spec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void mirror(std::vector<PickerRegion>& rs) {
    for (PickerRegion& r : rs) r.x = kPickerWidth - r.x - r.w;
}

}  // namespace

const char* picker_view_name(PickerView v) {
    static const char* const names[kPickerViewCount] = {"Front", "Back", "Left Hand", "Right Hand", "Face"};
    return names[int(v)];
}

float picker_height(PickerView v) {
    switch (v) {
        case PickerView::Front:
        case PickerView::Back: return 1.04f;
        case PickerView::LeftHand:
        case PickerView::RightHand: return 1.24f;
        case PickerView::Face: return 1.08f;
    }
    return 1;
}

const std::vector<PickerRegion>& picker_regions(PickerView v) {
    static const std::vector<PickerRegion> views[kPickerViewCount] = {
        build(cat(kBody, kFrontOnly), "Right", "Left"),
        [] {  // seen from behind: the avatar's left on the canvas's left, drawn as the front turned round
            auto r = build(cat(kBody, kBackOnly), "Right", "Left");
            mirror(r);
            return r;
        }(),
        [] {
            auto r = build(kHand, "Left", "Left", false);  // the back of the left hand: the right hand's mirror image
            mirror(r);
            return r;
        }(),
        build(kHand, "Right", "Right", false),
        build(kFace, "Right", "Left"),
    };
    return views[int(v)];
}

std::vector<int> region_nodes(const Skeleton& skel, const PickerRegion& r) {
    std::vector<int> out;
    for (const std::string& b : r.bones)
        if (int i = skel.find(b); i >= 0) out.push_back(i);
    return out;
}

}  // namespace vats
