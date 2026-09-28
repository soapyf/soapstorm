// Viewport Avatar Toolset - the project's former name, kept as data only: first run moves the user data
// saved under it, and project files and libraries written under it still open.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <string>
#include <vector>

namespace vats {

// The former name. The only place it is spelled out; everything old is built from it.
inline constexpr const char* kLegacyName = "spite";

// kLegacyName + suffix: the old extension is legacy_name() after a dot, the old folders
// legacy_name("-animation-studio"), the old format tags legacy_name("-project") and so on.
inline std::string legacy_name(const char* suffix = "") { return std::string(kLegacyName) + suffix; }

// Moves the file or folder `from` to `to` when `from` exists and `to` does not (a rename, or copy then
// delete across file systems). Never overwrites; on any error `from` stays where it was. True when it moved.
bool migrate_path(const std::string& from, const std::string& to);

// The per-user folders under the former name and where they go now: on Linux the XDG config and data
// folders, on Windows %APPDATA%, on macOS ~/Library/Application Support. Read from the environment.
std::vector<std::pair<std::string, std::string>> legacy_user_dirs();

// First run after the rename: migrate_path() on each of legacy_user_dirs(). Returns the new paths filled.
std::vector<std::string> migrate_user_data();

}  // namespace vats
