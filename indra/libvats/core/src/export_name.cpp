// Viewport Avatar Toolset - export file names from a naming pattern.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/export_name.h"

#include <cstdio>
#include <cstring>

namespace vats {
namespace {

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

bool is_separator(char c) { return c == '_' || c == '-' || c == ' ' || c == '.'; }

}  // namespace

std::string export_file_name(const ExportNaming& n, const std::string& project_stem, bool mirrored, const std::string& ext) {
    std::string name = !n.name.empty() ? n.name : !project_stem.empty() ? project_stem : "Animation";
    std::string side = n.side;
    if (mirrored) side = side == "Left" ? "Right" : side == "Right" ? "Left" : side;
    char number[16];
    std::snprintf(number, sizeof number, "%02d", n.number < 0 ? 0 : n.number > 999 ? 999 : n.number);

    std::string s = n.pattern.empty() ? "[NAME]_[#]_[SIDE]" : n.pattern;
    if (!n.actor.empty() && s.find("[ACTOR]") == std::string::npos) s += "_[ACTOR]";
    replace_all(s, "[ACTOR]", n.actor);
    replace_all(s, "[NAME]", name);
    replace_all(s, "[#]", number);
    replace_all(s, "[SIDE]", side);
    std::string clean;
    for (char c : s)
        if (!std::strchr("\\/:*?\"<>|", c)) clean += c;
    // Collapse runs of the same separator ("__" -> "_"); mixed runs such as "_-" stay.
    std::string collapsed;
    for (char c : clean)
        if (!(is_separator(c) && !collapsed.empty() && collapsed.back() == c)) collapsed += c;
    size_t a = 0, b = collapsed.size();
    while (a < b && is_separator(collapsed[a])) ++a;
    while (b > a && is_separator(collapsed[b - 1])) --b;
    std::string out = collapsed.substr(a, b - a);
    if (out.empty()) out = name;
    if (mirrored && side.empty()) out += "_mirrored";
    return out + "." + ext;
}

}  // namespace vats
