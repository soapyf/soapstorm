// Viewport Avatar Toolset - the What's New text, from the changelog shipped with the app.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/changelog.h"

namespace vats {

std::string recent_changes(std::string_view changelog, int versions) {
    std::string out;
    int seen = 0;
    bool in_release = false;
    while (!changelog.empty()) {
        const size_t end = changelog.find('\n');
        std::string line(changelog.substr(0, end));
        changelog = end == std::string_view::npos ? std::string_view() : changelog.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0) {  // a release: "## [0.4.1] - 2026-10-17"
            in_release = false;
            const size_t open = line.find('['), close = line.find(']');
            if (open == std::string::npos || close == std::string::npos || close < open) continue;
            const std::string version = line.substr(open + 1, close - open - 1);
            if (version == "Unreleased" || version == "unreleased") continue;
            if (seen == versions) break;
            ++seen, in_release = true;
            const size_t dash = line.find(" - ", close);
            out += "# What's new in " + version + (dash == std::string::npos ? "" : " (" + line.substr(dash + 3) + ")") + "\n";
            continue;
        }
        if (!in_release || (line.size() > 1 && line[0] == '[' && line.find("]: ") != std::string::npos)) continue;
        if (line.rfind("### ", 0) == 0) line = "## " + line.substr(4);
        const size_t lead = line.find_first_not_of(' ');
        if (lead != std::string::npos && lead > 0 && line.compare(lead, 2, "- ") == 0) line = line.substr(lead);
        for (size_t at; (at = line.find('`')) != std::string::npos;) line.erase(at, 1);
        out += line + "\n";
    }
    return out;
}

}  // namespace vats
