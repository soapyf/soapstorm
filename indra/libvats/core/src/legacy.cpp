// Viewport Avatar Toolset - moving user data saved under the former name.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/legacy.h"

#include <cstdlib>
#include <filesystem>

namespace vats {
namespace fs = std::filesystem;

bool migrate_path(const std::string& from, const std::string& to) {
    std::error_code ec;
    if (from.empty() || to.empty() || !fs::exists(from, ec) || fs::exists(to, ec)) return false;
    fs::create_directories(fs::path(to).parent_path(), ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;
    // Another file system: copy, and delete the old copy only once the new one is complete.
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        fs::remove_all(to, ec);
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

std::vector<std::pair<std::string, std::string>> legacy_user_dirs() {
    const std::string old_dir = legacy_name("-animation-studio"), new_dir = "viewport-avatar-toolset";
    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return std::string(v ? v : "");
    };
    std::vector<std::string> bases;
#if defined(_WIN32)
    bases.push_back(env("APPDATA"));
#elif defined(__APPLE__)
    if (!env("HOME").empty()) bases.push_back(env("HOME") + "/Library/Application Support");
#else
    const std::string home = env("HOME");
    const std::string config = env("XDG_CONFIG_HOME"), data = env("XDG_DATA_HOME");
    bases.push_back(!config.empty() ? config : home.empty() ? "" : home + "/.config");
    bases.push_back(!data.empty() ? data : home.empty() ? "" : home + "/.local/share");
#endif
    std::vector<std::pair<std::string, std::string>> dirs;
    for (const std::string& b : bases)
        if (!b.empty()) dirs.push_back({b + "/" + old_dir, b + "/" + new_dir});
    return dirs;
}

std::vector<std::string> migrate_user_data() {
    std::vector<std::string> moved;
    for (const auto& [from, to] : legacy_user_dirs())
        if (migrate_path(from, to)) moved.push_back(to);
    return moved;
}

}  // namespace vats
