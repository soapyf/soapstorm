// Viewport Avatar Toolset - reading Hexton SL Animator's files: .hxanim projects and its pose and prop
// libraries. Built only with VATS_LEGACY_IMPORT (see core/CMakeLists.txt); the shared code reaches it
// through #ifdef VATS_LEGACY_IMPORT hooks, and builds without this file.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 3.4 and 3.7 (IO-40, IO-54). The first-run import of the app's settings and
// libraries is ui/migrate.cpp.
#pragma once

#include <string>
#include <string_view>

#include "vats/json.h"

namespace vats::legacy_import {

inline constexpr const char* kProjectFormat = "hexton-sl-anim";
inline constexpr const char* kPoseLibraryFormat = "hexton-pose-library";
inline constexpr const char* kPropLibraryFormat = "hexton-prop-library";
inline constexpr const char* kProjectExtension = ".hxanim";

// The open dialog's extra filter, and the status note after opening one (the project has no file name).
inline constexpr const char* kFilterName = "Hexton project";
inline constexpr const char* kFilterExtensions = "hxanim";
inline constexpr const char* kConvertedNote = " (converted from Hexton; Save As to keep it)";

// File association: the Linux MIME type and its shared-mime-info entry, added to the packaged one.
inline constexpr const char* kMimeType = "application/x-hexton-project";
inline constexpr const char* kMimeEntry = "  <mime-type type=\"application/x-hexton-project\">\n"
                                          "    <comment>Hexton animation project</comment>\n"
                                          "    <sub-class-of type=\"application/json\"/>\n"
                                          "    <glob pattern=\"*.hxanim\"/>\n"
                                          "    <icon name=\"viewport-avatar-toolset\"/>\n"
                                          "  </mime-type>\n";

// An .hxanim's fields the project format does not know: the source file goes to meta.migrated_from
// (when known) and the unknown fields to meta.hexton_extra, so nothing in the file is lost.
inline void keep_unknown(Json& meta, Json unknown, std::string_view source_path) {
    if (!source_path.empty()) meta.set("migrated_from", std::string(source_path));
    if (!unknown.obj.empty()) meta.set("hexton_extra", std::move(unknown));
}

}  // namespace vats::legacy_import
