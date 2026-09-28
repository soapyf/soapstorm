// Viewport Avatar Toolset - the Inventory's Projects and Animations: folders of .vat and .anim files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Scanning with a metadata cache, and the file operations the Inventory offers (rename, duplicate,
// delete), which only ever touch files directly inside one of the listed folders. No UI; never throws.
// Spec: docs/spec/08 section 8 (FL). Paths are UTF-8 with '/' separators.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vats {

enum class LibKind { Project, Anim };  // .vat (and legacy projects) / .anim

struct LibFile {
    std::string path, name;  // name: the file name without its extension
    LibKind kind = LibKind::Project;
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;  // file-clock ticks, only compared
    std::string error;       // why the file could not be read; "" = the fields below are valid
    double seconds = 0;
    int fps = 0;             // projects
    int keys = 0;            // .anim: rotation and position keys over every joint
    int priority = 0;
    bool loop = false;
    int actors = 0;          // projects: 1, or the actor count of a group scene
};

// The kind a file name's extension belongs to; false for any other file.
bool lib_kind_of(const std::string& path, LibKind& kind);

// Reads one file's metadata (the whole project through load_project, or the .anim through parse_anim).
LibFile read_lib_file(const std::string& path);

// Lists a folder's files of one kind (not its subfolders), sorted by name, ignoring case. A file whose size
// and time are unchanged since the last scan keeps its cached metadata; only new or changed files are read.
class LibScanner {
public:
    std::vector<LibFile> scan(const std::string& dir, LibKind kind);
    LibFile info(const std::string& path);  // one file, through the same cache

private:
    std::map<std::string, LibFile> cache_;
};

// A file name from what the user typed: path separators, the characters Windows forbids, control
// characters, and leading dots and spaces are dropped; trimmed; at most 100 bytes. "" when nothing is left.
std::string lib_safe_name(const std::string& typed);

// True when path is a regular file directly inside one of roots (after resolving "..", "." and links).
bool lib_contains(const std::vector<std::string>& roots, const std::string& path);

// "<dir>/<stem>.<ext>", or the first free "<stem> 2.<ext>", "<stem> 3.<ext>", ...
std::string lib_free_path(const std::string& dir, const std::string& stem, const std::string& ext);

// The operations. Each refuses (false, err) a file outside roots; out gets the new path.
bool lib_rename(const std::vector<std::string>& roots, const std::string& path, const std::string& new_name,
                std::string& out, std::string& err);  // refuses a name that is taken
bool lib_duplicate(const std::vector<std::string>& roots, const std::string& path, std::string& out,
                   std::string& err);  // "<name> copy", then "<name> copy 2", ...
bool lib_delete(const std::vector<std::string>& roots, const std::string& path, std::string& err);

// Adds a folder to one of the Inventory's folder lists (settings project_folders / anim_folders, Add Folder...): '\'
// becomes '/', a trailing '/' is added, and a folder already listed is not added twice. False when nothing was added.
bool lib_add_folder(std::vector<std::string>& list, std::string dir);

// Add Community Folder... (08 CF): the folders to list for a community content folder (a clone of a community repo):
// its "poses", "clips" and "animations" subfolders as Animations folders and "projects" as a Projects folder, those
// that exist. A folder with none of them is listed whole in both sections. Each goes through lib_add_folder.
std::vector<std::pair<LibKind, std::string>> community_folders(const std::string& root);

}  // namespace vats
