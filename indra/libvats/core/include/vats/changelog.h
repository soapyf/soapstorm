// Viewport Avatar Toolset - the What's New text, from the changelog shipped with the app.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <string>
#include <string_view>

namespace vats {

// The newest `versions` releases of a Keep a Changelog file (CHANGELOG.md) in the Markdown subset the Welcome window
// draws: "## [0.4.1] - 2026-10-17" becomes "# What's new in 0.4.1 (2026-10-17)", "### Added" "## Added", nested
// bullets plain bullets; code ticks, link references and anything before the first release are left out, and so is
// an Unreleased section. "" when the text holds no release.
std::string recent_changes(std::string_view changelog, int versions);

}  // namespace vats
