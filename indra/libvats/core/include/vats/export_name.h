// Viewport Avatar Toolset - export file names from a naming pattern.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 section 4.2.
#pragma once

#include <string>

namespace vats {

struct ExportNaming {
    std::string name;     // [NAME]; empty = the project file's stem, else "Animation"
    int number = 1;       // [#], 0-999, written with at least two digits
    std::string side;     // [SIDE]: "", "Left" or "Right"
    std::string pattern = "[NAME]_[#]_[SIDE]";
    std::string actor;    // [ACTOR] (spec 08 GR-3); when set and the pattern has no [ACTOR], appended as "_<actor>"
    std::string clip;     // [CLIP] (spec 08 CL-4); when set and the pattern has no [CLIP], appended as "_<clip>" (before
                          // an appended actor); unset, [CLIP] is left out
};

// The file name for one export. project_stem may be empty; ext is "anim" or "bvh" (no dot).
std::string export_file_name(const ExportNaming& naming, const std::string& project_stem, bool mirrored,
                             const std::string& ext);

}  // namespace vats
