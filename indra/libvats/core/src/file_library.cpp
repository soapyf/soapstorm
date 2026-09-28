// Viewport Avatar Toolset - the Inventory's Projects and Animations folders (see vats/file_library.h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/file_library.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "guard.h"
#include "vats/anim_file.h"
#ifdef VATS_LEGACY_IMPORT
#include "vats/legacy_import.h"
#endif
#include "vats/project.h"

namespace vats {
namespace {

namespace fs = std::filesystem;

// UTF-8 in and out: on Windows a plain std::string would be read in the ANSI code page.
fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string str(const fs::path& p) {
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr std::uintmax_t kMaxFileBytes = 256u << 20;  // bigger than any project VATs writes

bool read_limited(const std::string& path, std::string& out, std::string& err) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(u8(path), ec);
    if (ec) return err = "cannot read the file", false;
    if (size > kMaxFileBytes) return err = "the file is too large", false;
    std::ifstream f(u8(path), std::ios::binary);
    std::ostringstream ss;
    if (!f || !(ss << f.rdbuf())) return err = size ? "cannot read the file" : "the file is empty", false;
    out = ss.str();
    return true;
}

bool stat(const std::string& path, std::uintmax_t& size, std::int64_t& mtime) {
    std::error_code ec;
    size = fs::file_size(u8(path), ec);
    if (ec) return false;
    mtime = std::int64_t(fs::last_write_time(u8(path), ec).time_since_epoch().count());
    return !ec;
}

}  // namespace

bool lib_kind_of(const std::string& path, LibKind& kind) {
    const std::string ext = lower(str(u8(path).extension()));
    if (ext == ".vat") return kind = LibKind::Project, true;
#ifdef VATS_LEGACY_IMPORT
    if (ext == legacy_import::kProjectExtension) return kind = LibKind::Project, true;
#endif
    if (ext == ".anim") return kind = LibKind::Anim, true;
    return false;
}

LibFile read_lib_file(const std::string& path) {
    LibFile f;
    f.path = path;
    f.name = str(u8(path).stem());
    lib_kind_of(path, f.kind);
    stat(path, f.size, f.mtime);
    std::string text, err;
    bool ok = guarded(err, [&] {
        if (!read_limited(path, text, err)) return false;
        if (f.kind == LibKind::Anim) {
            AnimFile a;
            if (!parse_anim(std::vector<std::uint8_t>(text.begin(), text.end()), a, err)) return false;
            f.seconds = a.duration;
            f.priority = a.base_priority;
            f.loop = a.loop != 0;
            for (const AnimJoint& j : a.joints)
                f.keys += int(j.rot.size() + j.pos.size() + j.rot_legacy.size() + j.pos_legacy.size());
            return true;
        }
        Project p;
        if (!load_project(text, p, err, path)) return false;
        f.fps = p.clip.fps;
        f.seconds = p.clip.fps > 0 ? double(p.clip.end_frame) / p.clip.fps : 0;
        f.priority = p.clip.priority;
        f.loop = p.clip.loop;
        f.actors = p.actors.empty() ? 1 : int(p.actors.size());
        return true;
    });
    if (!ok) f.error = err.empty() ? "cannot read the file" : err;
    return f;
}

LibFile LibScanner::info(const std::string& path) {
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    stat(path, size, mtime);
    auto it = cache_.find(path);
    if (it != cache_.end() && it->second.size == size && it->second.mtime == mtime) return it->second;
    return cache_[path] = read_lib_file(path);
}

std::vector<LibFile> LibScanner::scan(const std::string& dir, LibKind kind) {
    std::vector<LibFile> out;
    std::error_code ec;
    for (fs::directory_iterator it(u8(dir), ec), end; !ec && it != end; it.increment(ec)) {
        LibKind k;
        std::error_code ec2;
        if (!lib_kind_of(str(it->path()), k) || k != kind || !it->is_regular_file(ec2)) continue;
        out.push_back(info(str(it->path())));
    }
    std::sort(out.begin(), out.end(), [](const LibFile& a, const LibFile& b) {
        std::string x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.path < b.path;
    });
    return out;
}

std::string lib_safe_name(const std::string& typed) {
    std::string s;
    for (unsigned char c : typed)
        if (c >= 0x20 && c != 0x7f && !std::strchr("/\\:*?\"<>|", c)) s += char(c);
    size_t a = s.find_first_not_of(". "), b = s.find_last_not_of(". ");
    s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
    if (s.size() > 100) {
        size_t n = 100;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // not in the middle of a character
        s.resize(n);
        s.erase(s.find_last_not_of(". ") + 1);
    }
    // Names Windows keeps for devices, with or without an extension.
    std::string up = s.substr(0, s.find('.'));
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL"};
    bool device = std::find_if(std::begin(reserved), std::end(reserved), [&](const char* r) { return up == r; }) != std::end(reserved) ||
                  (up.size() == 4 && (up.rfind("COM", 0) == 0 || up.rfind("LPT", 0) == 0) && up[3] >= '1' && up[3] <= '9');
    if (device) s += "_";
    return s;
}

bool lib_contains(const std::vector<std::string>& roots, const std::string& path) {
    std::error_code ec;
    const fs::path p = u8(path);
    if (!fs::is_regular_file(p, ec)) return false;
    const fs::path parent = fs::canonical(p.parent_path().empty() ? fs::path(".") : p.parent_path(), ec);
    if (ec) return false;
    for (const std::string& r : roots) {
        std::error_code ec2;
        fs::path root = fs::canonical(u8(r), ec2);
        if (!ec2 && root == parent) return true;
    }
    return false;
}

std::string lib_free_path(const std::string& dir, const std::string& stem, const std::string& ext) {
    std::string base = dir.empty() || dir.back() == '/' ? dir : dir + "/";
    std::error_code ec;
    for (int n = 1;; ++n) {
        std::string p = base + stem + (n > 1 ? " " + std::to_string(n) : "") + "." + ext;
        if (!fs::exists(u8(p), ec)) return p;
    }
}

namespace {

// dir with a trailing '/', the stem and the extension (without the dot) of a path.
void split(const std::string& path, std::string& dir, std::string& stem, std::string& ext) {
    const fs::path p = u8(path);
    dir = str(p.parent_path());
    if (!dir.empty() && dir.back() != '/') dir += '/';
    stem = str(p.stem());
    ext = str(p.extension());
    if (!ext.empty()) ext.erase(0, 1);
}

}  // namespace

bool lib_rename(const std::vector<std::string>& roots, const std::string& path, const std::string& new_name,
                std::string& out, std::string& err) {
    if (!lib_contains(roots, path)) return err = "the file is not in a library folder", false;
    const std::string name = lib_safe_name(new_name);
    if (name.empty()) return err = "the name is empty", false;
    std::string dir, stem, ext;
    split(path, dir, stem, ext);
    out = dir + name + "." + ext;
    if (out == path) return true;
    std::error_code ec;
    // A change of case only is the same file on Windows and macOS.
    if (fs::exists(u8(out), ec) && !fs::equivalent(u8(out), u8(path), ec)) return err = name + "." + ext + " already exists", false;
    fs::rename(u8(path), u8(out), ec);
    if (ec) return err = ec.message(), false;
    return true;
}

bool lib_duplicate(const std::vector<std::string>& roots, const std::string& path, std::string& out, std::string& err) {
    if (!lib_contains(roots, path)) return err = "the file is not in a library folder", false;
    std::string dir, stem, ext;
    split(path, dir, stem, ext);
    out = lib_free_path(dir, stem + " copy", ext);
    std::error_code ec;
    fs::copy_file(u8(path), u8(out), fs::copy_options::none, ec);
    if (ec) return err = ec.message(), false;
    return true;
}

bool lib_delete(const std::vector<std::string>& roots, const std::string& path, std::string& err) {
    if (!lib_contains(roots, path)) return err = "the file is not in a library folder", false;
    std::error_code ec;
    if (!fs::remove(u8(path), ec)) return err = ec ? ec.message() : "the file is gone", false;
    return true;
}

}  // namespace vats
