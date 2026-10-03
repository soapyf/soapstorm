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

// Posing and the timeline bar (spec 08 TW, PT, TE-5)
inline constexpr char kTween[] = "\xee\x96\x92";         // between-horizontal-start U+E592
inline constexpr char kRelax[] = "\xee\x8e\x8b";         // spline U+E38B
inline constexpr char kBlend[] = "\xee\x96\x9c";         // blend U+E59C
inline constexpr char kMirror[] = "\xee\x8d\x9d";        // flip-horizontal U+E35D
inline constexpr char kRetime[] = "\xee\x87\xa0";        // timer U+E1E0
inline constexpr char kScratch[] = "\xee\x96\x96";       // notebook-pen U+E596
inline constexpr char kPropagate[] = "\xee\x81\xb3";     // chevrons-right U+E073

// Graph editor and dope sheet
inline constexpr char kEase[] = "\xee\x98\x8d";           // chart-spline U+E60D
inline constexpr char kFilter[] = "\xee\x83\x9c";         // funnel U+E0DC
inline constexpr char kSnapshot[] = "\xee\x81\xa4";       // camera U+E064
inline constexpr char kSwap[] = "\xee\x89\x8a";           // arrow-left-right U+E24A
inline constexpr char kClear[] = "\xee\x8a\x8f";          // eraser U+E28F
inline constexpr char kMore[] = "\xee\x82\xb6";           // ellipsis U+E0B6
inline constexpr char kSimplify[] = "\xee\x97\xa1";       // diamond-minus U+E5E1
inline constexpr char kDopeSheet[] = "\xee\x98\xa4";      // chart-gantt U+E624
inline constexpr char kMotionPath[] = "\xee\x94\xbe";     // route U+E53E

// Tool windows and their buttons
inline constexpr char kCheck[] = "\xee\x87\xbf";         // shield-check U+E1FF
inline constexpr char kWarning[] = "\xee\x81\xb7";       // circle-alert U+E077
inline constexpr char kFix[] = "\xee\x86\xb1";           // wrench U+E1B1
inline constexpr char kGoTo[] = "\xee\x82\xac";          // crosshair U+E0AC
inline constexpr char kRules[] = "\xee\x87\x90";         // list-checks U+E1D0
inline constexpr char kRefresh[] = "\xee\x85\x85";       // refresh-cw U+E145
inline constexpr char kSlPreview[] = "\xee\x88\x8e";     // ghost U+E20E
inline constexpr char kFit[] = "\xee\x88\xa0";           // shrink U+E220
inline constexpr char kSplit[] = "\xee\x85\x8e";         // scissors U+E14E
inline constexpr char kCopy[] = "\xee\x82\x9e";          // copy U+E09E
inline constexpr char kText[] = "\xee\x83\x8c";          // file-text U+E0CC
inline constexpr char kFind[] = "\xee\x85\x91";          // search U+E151
inline constexpr char kApply[] = "\xee\x81\xac";         // check U+E06C
inline constexpr char kStretch[] = "\xee\x87\x86";       // move-horizontal U+E1C6
inline constexpr char kTreadmill[] = "\xee\x8e\xb9";     // footprints U+E3B9
inline constexpr char kIdle[] = "\xee\x86\xb0";          // wind U+E1B0
inline constexpr char kOverlap[] = "\xee\x8a\x83";       // waves U+E283
inline constexpr char kAdd[] = "\xee\x84\xbd";           // plus U+E13D
inline constexpr char kAddLayer[] = "\xee\x9b\xa6";      // layers-plus U+E6E6
inline constexpr char kAllPanels[] = "\xee\x83\xbf";     // layout-grid U+E0FF
inline constexpr char kBake[] = "\xee\x8e\xbb";          // stamp U+E3BB
inline constexpr char kUnbake[] = "\xee\x85\x88";        // rotate-ccw U+E148
inline constexpr char kFace[] = "\xee\x85\xa4";          // smile U+E164
inline constexpr char kLookAt[] = "\xee\x94\xb6";        // scan-eye U+E536
inline constexpr char kQuality[] = "\xee\x86\xbf";       // gauge U+E1BF
inline constexpr char kPlanner[] = "\xee\x87\x91";       // list-ordered U+E1D1
inline constexpr char kUp[] = "\xee\x81\xb0";            // chevron-up U+E070
inline constexpr char kDown[] = "\xee\x81\xad";          // chevron-down U+E06D
inline constexpr char kRunning[] = "\xee\x82\x80";       // circle-play U+E080
inline constexpr char kBatch[] = "\xee\x8c\xbf";         // folders U+E33F
inline constexpr char kFootLock[] = "\xee\x80\xbf";      // anchor U+E03F
inline constexpr char kCleanUp[] = "\xee\x90\x92";       // sparkles U+E412

// View and Light menus
inline constexpr char kOrtho[] = "\xee\x8a\x91";        // frame U+E291
inline constexpr char kStudio[] = "\xee\x8b\x98";       // lamp U+E2D8
inline constexpr char kNoon[] = "\xee\x85\xb8";         // sun U+E178
inline constexpr char kKeyLight[] = "\xee\x8b\x9a";     // lamp-desk U+E2DA
inline constexpr char kRim[] = "\xee\x8a\x99";          // sun-dim U+E299
inline constexpr char kDusk[] = "\xee\x85\xba";         // sunset U+E17A
inline constexpr char kNight[] = "\xee\x84\x9e";        // moon U+E11E
inline constexpr char kBackdrop[] = "\xee\x91\x8b";     // wallpaper U+E44B
inline constexpr char kTarget[] = "\xee\x82\xac";       // crosshair U+E0AC (as kGoTo): View > Target Ghost

// Wave 3 and viewer build 20 (spec 08 sections 19-27, spec 09 build 20)
inline constexpr char kBlocking[] = "\xee\x8d\x87";     // toy-brick U+E347
inline constexpr char kTags[] = "\xee\x8d\x9c";         // tags U+E35C
inline constexpr char kPin[] = "\xee\x89\x99";          // pin U+E259
inline constexpr char kClips[] = "\xee\x8a\x9b";        // clapperboard U+E29B
inline constexpr char kReference[] = "\xee\x83\xb6";    // image U+E0F6
inline constexpr char kListing[] = "\xee\x8f\xa4";      // store U+E3E4
inline constexpr char kBalance[] = "\xee\x88\x92";      // scale U+E212
inline constexpr char kJumpArc[] = "\xee\x93\xb6";      // rabbit U+E4F6
inline constexpr char kPull[] = "\xee\x87\xa6";         // hand-grab U+E1E6
inline constexpr char kExpressionPack[] = "\xee\x84\xa9";  // package U+E129
inline constexpr char kLipSync[] = "\xee\x95\x9a";      // audio-lines U+E55A
inline constexpr char kCommunity[] = "\xee\x83\x99";    // folder-plus U+E0D9
inline constexpr char kHeights[] = "\xee\x85\x8b";      // ruler U+E14B
inline constexpr char kInWorld[] = "\xee\x87\xb3";      // earth U+E1F3
inline constexpr char kWalkTest[] = "\xee\x88\x9e";     // person-standing U+E21E
inline constexpr char kFaceCam[] = "\xee\x88\x85";      // webcam U+E205
inline constexpr char kSeat[] = "\xee\x8b\x80";         // armchair U+E2C0

// The Tools menu's items without a button of their own
inline constexpr char kFollow[] = "\xee\x87\x9b";       // locate-fixed U+E1DB
inline constexpr char kBind[] = "\xee\x84\x82";         // link U+E102
inline constexpr char kRelease[] = "\xee\x8a\xb6";      // pin-off U+E2B6
inline constexpr char kHand[] = "\xee\x87\x97";         // hand U+E1D7
inline constexpr char kDynamics[] = "\xee\x8f\x97";     // atom U+E3D7
inline constexpr char kTransition[] = "\xee\x96\x91";   // between-horizontal-end U+E591
inline constexpr char kRagdoll[] = "\xee\x86\x90";      // trending-down U+E190
inline constexpr char kActors[] = "\xee\x86\xa4";       // users U+E1A4
inline constexpr char kSeamless[] = "\xee\x87\xa7";     // infinity U+E1E7
inline constexpr char kInPlace[] = "\xee\x91\x8d";      // arrow-down-to-dot U+E44D
inline constexpr char kCycleStart[] = "\xee\x85\x89";   // rotate-cw U+E149

// Menus (each item its own glyph within a group)
inline constexpr char kFollowThrough[] = "\xee\x95\x82";  // waypoints U+E542
inline constexpr char kAvatarPhysics[] = "\xee\x88\xa3";  // vibrate U+E223
inline constexpr char kPaint[] = "\xee\x8b\xa7";  // paintbrush U+E2E7
inline constexpr char kEditLimits[] = "\xee\x93\xb1";  // pencil-ruler U+E4F1
inline constexpr char kRecent[] = "\xee\x87\xb5";  // history U+E1F5
inline constexpr char kSaveAs[] = "\xee\x8c\x9f";  // file-pen U+E31F
inline constexpr char kImportBvh[] = "\xee\x83\x85";  // file-input U+E0C5
inline constexpr char kImportAnim[] = "\xee\x8c\x98";  // file-down U+E318
inline constexpr char kRetarget[] = "\xee\x90\xbf";  // merge U+E43F
inline constexpr char kAudio[] = "\xee\x95\x9e";  // file-music U+E55E
inline constexpr char kFiles[] = "\xee\x83\x8f";  // files U+E0CF
inline constexpr char kUploadAll[] = "\xee\x82\x91";  // cloud-upload U+E091

}  // namespace vats::icon
