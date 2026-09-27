// Viewport Avatar Toolset - icon codepoints of the Lucide icon font merged into the UI font.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// UTF-8 strings for ImGui labels (bytes rather than \u escapes, so every compiler's execution character set
// gives the same string). The font, app/assets/fonts/lucide-icons.ttf, holds only these glyphs:
// tools/subset-icons.sh reads the U+ codepoints below, so to add an icon, add it here and re-run the script.
#pragma once

namespace vats::icon {

// Timeline transport
inline constexpr char kStart[] = "\xee\x85\x9f";      // skip-back U+E15F
inline constexpr char kPrevKey[] = "\xee\x85\x87";    // rewind U+E147
inline constexpr char kPlay[] = "\xee\x84\xbc";       // play U+E13C
inline constexpr char kPause[] = "\xee\x84\xae";      // pause U+E12E
inline constexpr char kNextKey[] = "\xee\x82\xbd";    // fast-forward U+E0BD
inline constexpr char kEnd[] = "\xee\x85\xa0";        // skip-forward U+E160
inline constexpr char kLoop[] = "\xee\x85\x86";       // repeat U+E146

// Tools
inline constexpr char kSelect[] = "\xee\x87\x83";     // mouse-pointer-2 U+E1C3
inline constexpr char kMove[] = "\xee\x84\xa1";       // move U+E121
inline constexpr char kRotate[] = "\xee\x8b\xaa";     // rotate-3d U+E2EA
inline constexpr char kScale[] = "\xee\x8b\xab";      // scale-3d U+E2EB
inline constexpr char kLocal[] = "\xee\x81\xa1";      // box U+E061
inline constexpr char kWorld[] = "\xee\x83\xa8";      // globe U+E0E8
inline constexpr char kGimbal[] = "\xee\x8b\xbe";     // axis-3d U+E2FE
inline constexpr char kIkFk[] = "\xee\x8d\x98";       // bone U+E358
inline constexpr char kSetKey[] = "\xee\x97\xa2";     // diamond-plus U+E5E2

// Graph editor (the tangent buttons are drawn, see icon_button.h)
inline constexpr char kFrameAll[] = "\xee\x84\x92";       // maximize U+E112
inline constexpr char kFrameSelected[] = "\xee\x8a\x9e";  // focus U+E29E
inline constexpr char kFitValues[] = "\xee\x90\xbe";      // unfold-vertical U+E43E
inline constexpr char kEulerFilter[] = "\xee\x90\xa3";    // iteration-ccw U+E423
inline constexpr char kFlipTime[] = "\xee\x8d\xa0";       // flip-vertical-2 U+E360 (a left-right mirror)
inline constexpr char kFlipValues[] = "\xee\x8d\x9e";     // flip-horizontal-2 U+E35E (an up-down mirror)
inline constexpr char kDelete[] = "\xee\x86\x8e";         // trash-2 U+E18E

// Files and menus
inline constexpr char kNew[] = "\xee\x83\x89";           // file-plus U+E0C9
inline constexpr char kOpen[] = "\xee\x89\x87";          // folder-open U+E247
inline constexpr char kSave[] = "\xee\x85\x8d";          // save U+E14D
inline constexpr char kUndo[] = "\xee\x8a\xa1";          // undo-2 U+E2A1
inline constexpr char kRedo[] = "\xee\x8a\xa0";          // redo-2 U+E2A0
inline constexpr char kExport[] = "\xee\x83\x88";        // file-output U+E0C8
inline constexpr char kUpload[] = "\xee\x86\x9e";        // upload U+E19E
inline constexpr char kImport[] = "\xee\x88\xaf";        // import U+E22F
inline constexpr char kAddToLibrary[] = "\xee\x88\xbd";  // bookmark-plus U+E23D
inline constexpr char kRename[] = "\xee\x87\xb9";        // pencil U+E1F9

// Motion Capture
inline constexpr char kListen[] = "\xee\x85\x82";  // radio U+E142
inline constexpr char kStop[] = "\xee\x85\xa7";    // square U+E167
inline constexpr char kPhone[] = "\xee\x85\xa3";   // smartphone U+E163
inline constexpr char kRecord[] = "\xee\x8d\x85";  // circle-dot U+E345

// Actors
inline constexpr char kPlace[] = "\xee\x84\x91";     // map-pin U+E111
inline constexpr char kShown[] = "\xee\x82\xba";     // eye U+E0BA
inline constexpr char kHidden[] = "\xee\x82\xbb";    // eye-off U+E0BB
inline constexpr char kLocked[] = "\xee\x84\x8b";    // lock U+E10B
inline constexpr char kUnlocked[] = "\xee\x84\x8c";  // lock-open U+E10C

}  // namespace vats::icon
