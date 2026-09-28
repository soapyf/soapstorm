// Viewport Avatar Toolset - the Inventory's Projects and Animations: .vat and .anim files in folders.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 8 (FL). Scanning, metadata and the file operations are core's (vats/file_library.h);
// this is the Inventory's view of them and what it does with a file. Host calls only, so the viewer has it too.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app.h"
#include "widgets.h"
#include "icon_button.h"
#include "icons.h"
#include "vats/anim_file.h"
#include "vats/curve_ops.h"
#include "vats/file_library.h"
#include "theme.h"
#ifdef VATS_LEGACY_IMPORT
#include "vats/legacy_import.h"
#endif

namespace vats {

struct FileLibUi {
    struct Group {
        std::string title, dir;  // dir "" = the Recent group
        int folder = -1;         // index in the settings' folder list; -1 = the library folder or Recent
        std::vector<LibFile> items;
    };
    LibScanner scanner;
    std::vector<Group> groups[2];  // by LibKind
    bool stale = true, focused = false, app_away = false;
    std::mutex mutex;  // Add Folder... answers, which may come on another thread
    std::vector<std::pair<LibKind, std::string>> added;
    // The name prompt: Rename... (path = the file) or Save to Library... (path = "").
    bool prompt = false;
    std::string prompt_path;
    char buf[128] = {};
};

namespace {

std::string folder_of(const std::string& path) { return path.substr(0, path.find_last_of('/') + 1); }
std::string stem_of(const std::string& path) {
    std::u8string s = u8path(path).stem().u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace

// A file:// URL of a folder for Host::open_url, percent-encoded (spaces, non-ASCII names).
std::string folder_url(const std::string& dir) {
    std::string url = "file://";
    if (dir.empty() || dir[0] != '/') url += '/';  // C:/... on Windows
    for (unsigned char c : dir) {
        if (std::isalnum(c) || std::strchr("/-._~:", c)) {
            url += char(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            url += b;
        }
    }
    return url;
}

namespace {

std::string meta_line(const LibFile& f) {
    if (!f.error.empty()) return "Cannot read: " + f.error;
    char b[160];
    if (f.kind == LibKind::Anim)
        std::snprintf(b, sizeof b, "%.2f s, %s, priority %d%s", f.seconds, count_noun(f.keys, "key").c_str(), f.priority, f.loop ? ", loops" : "");
    else
        std::snprintf(b, sizeof b, "%.2f s, %d fps, priority %d%s, %d %s", f.seconds, f.fps, f.priority, f.loop ? ", loops" : "",
                      f.actors, f.actors == 1 ? "actor" : "actors");
    return b;
}

// A file's animation as a clip item (every track, the whole length). fps > 0 retimes it to that rate (an insert
// into a project); 0 keeps the file's own. False with err when the file cannot be used; never throws.
bool file_clip(const Skeleton& skel, const std::string& path, int fps, LibraryItem& item, std::string& err) {
    try {
        std::ifstream in(u8path(path), std::ios::binary);
        std::ostringstream ss;
        if (!in || !(ss << in.rdbuf())) return err = "the file could not be read", false;
        const std::string text = ss.str();
        LibKind kind = LibKind::Project;
        lib_kind_of(path, kind);
        Clip clip;
        if (kind == LibKind::Anim) {
            AnimFile f;
            if (!parse_anim(std::vector<std::uint8_t>(text.begin(), text.end()), f, err)) return false;
            if (auto problems = validate_anim(f, skel, false); !problems.empty()) return err = problems.front(), false;
            clip = import_anim(skel, f, fps).clip;
        } else {
            Project p;
            if (!load_project(text, p, err, path)) return false;
            clip = std::move(p.clip);
            if (fps > 0 && clip.fps != fps) retime_clip(clip, fps);
        }
        std::vector<std::string> tracks;
        for (auto& [name, t] : clip.curves) tracks.push_back(name);
        item = make_clip(clip, tracks, 0, clip.end_frame, "pose", "");
        item.name = stem_of(path);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// The kind icon: a figure with a play mark (.anim) or a clapperboard (project), or a warning for a bad file.
void draw_file_icon(ImDrawList* dl, ImVec2 at, float size, const LibFile& f, ImTextureID tex) {
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImVec2 b(at.x + size, at.y + size);
    auto P = [&](float x, float y) { return ImVec2(at.x + x * size, at.y + y * size); };
    const float w = std::max(1.2f, size / 22);
    dl->AddRectFilled(at, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
    if (!f.error.empty()) {
        const ImU32 amber = IM_COL32(240, 180, 70, 255);
        dl->AddTriangle(P(0.5f, 0.18f), P(0.84f, 0.8f), P(0.16f, 0.8f), amber, w * 1.2f);
        dl->AddLine(P(0.5f, 0.38f), P(0.5f, 0.6f), amber, w * 1.4f);
        dl->AddCircleFilled(P(0.5f, 0.7f), w, amber);
        return;
    }
    if (tex) {
        dl->AddImage(tex, at, b);
    } else if (f.kind == LibKind::Project) {
        dl->AddRect(P(0.2f, 0.38f), P(0.8f, 0.8f), col, 2, 0, w);
        dl->AddQuad(P(0.2f, 0.36f), P(0.78f, 0.2f), P(0.8f, 0.3f), P(0.22f, 0.46f), col, w);
        for (float x : {0.4f, 0.6f}) dl->AddLine(P(x - 0.06f, 0.3f + (0.6f - x) * 0.25f), P(x + 0.02f, 0.27f + (0.6f - x) * 0.25f), col, w);
    } else {
        dl->AddCircle(P(0.5f, 0.2f), size * 0.08f, col, 0, w);
        dl->AddLine(P(0.5f, 0.28f), P(0.5f, 0.58f), col, w);
        dl->AddLine(P(0.25f, 0.4f), P(0.75f, 0.4f), col, w);
        dl->AddLine(P(0.5f, 0.58f), P(0.34f, 0.86f), col, w);
        dl->AddLine(P(0.5f, 0.58f), P(0.66f, 0.86f), col, w);
    }
    if (f.kind == LibKind::Anim) {  // the play mark the pose library's clips carry
        float r = size * 0.16f;
        ImVec2 c(b.x - r - 2, b.y - r - 2);
        dl->AddCircleFilled(c, r + 1, IM_COL32(20, 22, 26, 200));
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.4f, c.y - r * 0.55f), ImVec2(c.x - r * 0.4f, c.y + r * 0.55f),
                              ImVec2(c.x + r * 0.6f, c.y), IM_COL32(240, 240, 240, 255));
    }
}

}  // namespace

bool App::inv_match(const std::string& name) const {
    if (inv_filter_.empty()) return true;
    auto low = [](unsigned char c) { return std::tolower(c); };
    return std::search(name.begin(), name.end(), inv_filter_.begin(), inv_filter_.end(),
                       [&](char a, char b) { return low(a) == low(b); }) != name.end();
}

void App::rescan_files() {
    if (file_lib_) file_lib_->stale = true;
}

void App::draw_file_library() {
    if (!file_lib_) file_lib_ = std::make_shared<FileLibUi>();
    FileLibUi& L = *file_lib_;
    const std::string lib = library_dir();  // "" when there is no data folder: the extra folders only
    const std::string dirs[2] = {lib.empty() ? "" : lib + "Projects/", lib.empty() ? "" : lib + "Animations/"};
    std::vector<std::string>* extra[2] = {&settings_.project_folders, &settings_.anim_folders};

    // Rescan when the Inventory gets the focus or the program comes back to the front, and after saves and exports.
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows), away = ImGui::GetIO().AppFocusLost;
    if ((focused && !L.focused) || (!away && L.app_away)) L.stale = true;
    L.focused = focused, L.app_away = away;
    if (L.stale) {
        L.stale = false;
        for (int k = 0; k < 2; ++k) {
            const LibKind kind = LibKind(k);
            std::error_code ec;
            auto& groups = L.groups[k];
            groups.clear();
            if (!lib.empty()) {
                std::filesystem::create_directories(u8path(dirs[k]), ec);
                groups.push_back({"Library", dirs[k], -1, L.scanner.scan(dirs[k], kind)});
            }
            if (kind == LibKind::Project) {
                FileLibUi::Group recent{"Recent", "", -1, {}};
                for (const std::string& path : settings_.recent) {
                    LibKind rk;
                    if (std::error_code e2; lib_kind_of(path, rk) && std::filesystem::is_regular_file(u8path(path), e2))
                        recent.items.push_back(L.scanner.info(path));
                }
                if (!recent.items.empty()) groups.push_back(std::move(recent));
            }
            for (int i = 0; i < int(extra[k]->size()); ++i) {
                std::string d = (*extra[k])[i];
                std::string title = d.substr(0, d.find_last_not_of("/\\") + 1);
                title = title.substr(title.find_last_of("/\\") + 1);
                groups.push_back({title.empty() ? d : title, d, i, L.scanner.scan(d, kind)});
            }
        }
    }
    std::vector<std::string> roots(dirs, dirs + 2);
    for (auto* list : extra) roots.insert(roots.end(), list->begin(), list->end());

    const float font = ImGui::GetFontSize(), icon = font * 2.2f;
    for (int k = 0; k < 2; ++k) {
        const LibKind kind = LibKind(k);
        const bool anim = kind == LibKind::Anim;
        ImGui::PushID(k);
        if (!inventory_section(anim ? "Animations" : "Projects")) {
            ImGui::PopID();
            continue;
        }
        if (!anim) {
            if (ImGui::Button("Save to Library...")) save_to_library();
            ImGui::SetItemTooltip("Save this project into the Projects library under a name");
            ImGui::SameLine();
        }
        if (ImGui::Button("Add Folder...")) {
            host_.open_folder_dialog("", [lib = file_lib_, kind](std::vector<std::string> files) {
                if (files.empty()) return;
                std::lock_guard<std::mutex> lock(lib->mutex);
                lib->added.emplace_back(kind, files[0]);
            });
        }
#ifdef VATS_LEGACY_IMPORT
        const std::string projects = std::string(".vat and ") + legacy_import::kProjectExtension;
#else
        const std::string projects = ".vat";
#endif
        ImGui::SetItemTooltip("List the %s files of another folder here too", anim ? ".anim" : projects.c_str());
        if (!anim && icon_label_button(icon::kCommunity, "Add Community Folder...")) {  // 08 CF: a clone of a community content repo
            host_.open_folder_dialog("", [lib = file_lib_](std::vector<std::string> files) {
                if (files.empty()) return;
                std::lock_guard<std::mutex> lock(lib->mutex);
                for (auto& f : community_folders(files[0])) lib->added.push_back(f);
            });
        }
        if (!anim) ImGui::SetItemTooltip("List a community folder's poses, clips, animations and projects here");
        int remove_folder = -1;
        for (const FileLibUi::Group& g : L.groups[k]) {
            int shown = 0;
            for (const LibFile& f : g.items) shown += inv_match(f.name);
            if (!inv_filter_.empty() && !shown) continue;
            ImGui::PushID(g.dir.empty() ? "recent" : g.dir.c_str());
            std::string label = g.title + " (" + std::to_string(shown) + ")###group";
            const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (!g.dir.empty()) ImGui::SetItemTooltip("%s", g.dir.c_str());
            if (!g.dir.empty() && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Show in Folder")) host_.open_url(folder_url(g.dir));
                if (g.folder >= 0 && ImGui::MenuItem("Remove Folder from Inventory")) remove_folder = g.folder;
                if (g.folder >= 0) ImGui::SetItemTooltip("The files stay where they are");
                ImGui::EndPopup();
            }
            if (open) {
                if (g.items.empty())
                    hint(g.folder >= 0 ? "No files here."
                         : anim  ? "Empty. Tick \"Also save to Animations library\" in Export, or copy .anim files into this folder."
                                 : "Empty. File > Save to Library... puts the open project here.");
                for (const LibFile& f : g.items) {
                    if (!inv_match(f.name)) continue;
                    ImGui::PushID(f.path.c_str());
                    ImTextureID tex = 0;
                    if (f.error.empty()) {  // the pose library's thumbnails: the middle frame on the current body
                        char name[40];
                        std::snprintf(name, sizeof name, "file-%016zx",
                                      std::hash<std::string>{}(f.path + "|" + std::to_string(f.size) + "|" + std::to_string(f.mtime) +
                                                               kBodyIds[int(body_)]));
                        const std::string png = thumb_png(lib + name), path = f.path;
                        tex = thumbnail(name, png, [&] {
                            LibraryItem it;
                            std::string err;
                            return file_clip(skel_, path, 0, it, err) && render_pose_thumbnail(it, png);
                        });
                    }
                    ImVec2 at = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##file", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, icon)) &&
                        ImGui::IsMouseDoubleClicked(0))
                        open_path(f.path);  // a project opens; a .anim is imported as a new project (File > Open)
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::TextUnformatted(f.name.c_str());
                        if (!f.error.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", meta_line(f).c_str());
                        else ImGui::TextUnformatted(meta_line(f).c_str());
                        ImGui::TextDisabled("%s", f.path.c_str());
                        ImGui::TextDisabled(anim ? "Double-click to open as a new project; drag onto the view to insert at frame %d"
                                                 : "Double-click or drag onto the view to open", int(std::round(frame_)));
                        ImGui::EndTooltip();
                    }
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("VATS_FILE", f.path.c_str(), f.path.size() + 1);
                        ImGui::TextUnformatted(f.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginPopupContextItem()) {
                        const bool ours = lib_contains(roots, f.path);
                        if (ImGui::MenuItem("Open")) open_path(f.path);
                        if (anim) {
                            if (ImGui::MenuItem("Insert into Current Project at This Frame")) insert_anim_file(f.path, false);
                            if (ImGui::MenuItem("Insert Mirrored")) insert_anim_file(f.path, true);
                            if (ImGui::MenuItem("Insert, Matching Poses...")) match_anim_file(f.path);  // 08 PM-1
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Rename...", nullptr, false, ours)) {
                            L.prompt = true, L.prompt_path = f.path;
                            std::snprintf(L.buf, sizeof L.buf, "%s", f.name.c_str());
                        }
                        if (ImGui::MenuItem("Duplicate", nullptr, false, ours)) {
                            std::string out, err;
                            if (lib_duplicate(roots, f.path, out, err)) status("Duplicated as " + out.substr(out.find_last_of('/') + 1));
                            else message("Could not duplicate", f.path + "\n\n" + err);
                            L.stale = true;
                        }
                        if (ImGui::MenuItem("Delete", nullptr, false, ours)) {
                            const std::string path = f.path, name = f.name;
                            host_.ask("Viewport Avatar Toolset", "Delete \"" + path.substr(path.find_last_of('/') + 1) + "\"? This cannot be undone.",
                                      {"Delete", "Cancel"}, [this, path, name, roots](int choice) {
                                          if (choice != 0) return;
                                          std::string err;
                                          if (lib_delete(roots, path, err)) status("Deleted " + name);
                                          else message("Could not delete", path + "\n\n" + err);
                                          rescan_files();
                                      });
                        }
                        if (!ours) ImGui::SetItemTooltip("Only files in the Inventory's folders can be renamed, duplicated or deleted");
                        ImGui::Separator();
                        if (ImGui::MenuItem("Show in Folder")) host_.open_url(folder_url(folder_of(f.path)));
                        ImGui::EndPopup();
                    }
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    draw_file_icon(dl, at, icon, f, tex);
                    const float x = at.x + icon + 8, line = ImGui::GetTextLineHeight();
                    dl->AddText(ImVec2(x, at.y + icon / 2 - line), ImGui::GetColorU32(ImGuiCol_Text), f.name.c_str());
                    dl->AddText(ImVec2(x, at.y + icon / 2), ImGui::GetColorU32(f.error.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                                meta_line(f).c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (remove_folder >= 0) {
            extra[k]->erase(extra[k]->begin() + remove_folder);
            save_settings();
            L.stale = true;
        }
        ImGui::PopID();
    }
}

// Rename... and Save to Library... share one name prompt; drawn every frame, since File > Save to Library... can
// open it while the Inventory tab is hidden. Folders from Add Folder... arrive here too.
void App::draw_file_prompt() {
    if (!file_lib_) return;
    FileLibUi& L = *file_lib_;
    {
        std::lock_guard<std::mutex> lock(L.mutex);
        for (auto& [kind, dir] : L.added) lib_add_folder(kind == LibKind::Project ? settings_.project_folders : settings_.anim_folders, dir);
        if (!L.added.empty()) save_settings(), L.stale = true;
        L.added.clear();
    }
    const char* title = L.prompt_path.empty() ? "Save to Library" : "Rename File";
    if (L.prompt && !ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title), L.prompt = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(L.prompt_path.empty() ? "Name for this project in the library" : "New name");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool ok = ImGui::InputText("##filename", L.buf, sizeof L.buf, ImGuiInputTextFlags_EnterReturnsTrue);
    ok = ImGui::Button(L.prompt_path.empty() ? "Save" : "Rename", ImVec2(90, 0)) || ok;
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    const std::string name = lib_safe_name(L.buf);
    if (ok && !name.empty()) {
        if (L.prompt_path.empty()) {  // Save to Library: asks before replacing another project of that name
            const std::string path = library_dir() + "Projects/" + name + ".vat";
            std::error_code ec;
            std::filesystem::create_directories(u8path(library_dir() + "Projects/"), ec);
            if (path != doc_.path && std::filesystem::exists(u8path(path), ec))
                host_.ask("Viewport Avatar Toolset", name + ".vat is already in the library. Replace it?", {"Replace", "Cancel"},
                          [this, path](int choice) {
                              if (choice == 0) save(path);
                          });
            else
                save(path);
        } else {
            std::vector<std::string> roots{library_dir() + "Projects/", library_dir() + "Animations/"};
            for (auto* list : {&settings_.project_folders, &settings_.anim_folders}) roots.insert(roots.end(), list->begin(), list->end());
            std::string out, err;
            if (lib_rename(roots, L.prompt_path, name, out, err)) {
                if (doc_.path == L.prompt_path) doc_.path = out, update_title();  // the open project follows its file
                std::replace(settings_.recent.begin(), settings_.recent.end(), L.prompt_path, out);
                save_settings();
                status("Renamed to " + out.substr(out.find_last_of('/') + 1));
            } else {
                message("Could not rename", L.prompt_path + "\n\n" + err);
            }
            L.stale = true;
        }
    }
    if ((ok && !name.empty()) || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::save_to_library() {
    if (!file_lib_) file_lib_ = std::make_shared<FileLibUi>();
    if (library_dir().empty()) return message("Save to Library", "There is no data folder to keep a library in.");
    std::string stem = doc_.path.empty() ? "Animation" : doc_.path.substr(doc_.path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));
    file_lib_->prompt = true, file_lib_->prompt_path.clear();
    std::snprintf(file_lib_->buf, sizeof file_lib_->buf, "%s", stem.c_str());
}

// A .anim's keys pasted at the current frame, as a pose-library clip is (AM-95): retimed to the project's frame
// rate, every bone it moves, one undo step. The length grows when the keys run past the end.
void App::insert_anim_file(const std::string& path, bool mirrored) {
    LibraryItem it;
    std::string err;
    if (!file_clip(skel_, path, doc_.clip().fps, it, err)) return message("Could not insert " + path.substr(path.find_last_of('/') + 1), err);
    const double at = std::round(frame_);
    edit("Insert Animation", [&](Clip& c) {
        c.end_frame = std::max(c.end_frame, int(std::ceil(at + it.length)));
        paste_clip(c, skel_, it, at, mirrored, nullptr);  // a whole-body item has no relative IK tracks to warn about
    });
    status("Inserted " + it.name + " at frame " + std::to_string(int(at)) + (mirrored ? " (mirrored)" : ""));
}

// 08 PM-1: the .anim as a clip of its own, offered to Match Poses to join onto the end of the clip.
void App::match_anim_file(const std::string& path) {
    LibraryItem it;
    std::string err;
    if (!file_clip(skel_, path, doc_.clip().fps, it, err)) return message("Could not insert " + path.substr(path.find_last_of('/') + 1), err);
    open_match_poses(library_clip(it, apply_mirrored_), it.name);
}

void App::anim_file_to_library(const std::string& path) {
    std::ifstream in(u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    anim_to_library(path.substr(path.find_last_of('/') + 1), std::vector<std::uint8_t>(text.begin(), text.end()));
}

// Replaces a library file of the same name, as Export replaces its own.
void App::anim_to_library(const std::string& file_name, const std::vector<std::uint8_t>& bytes) {
    if (library_dir().empty()) return;
    const std::string dir = library_dir() + "Animations/", name = lib_safe_name(file_name.substr(0, file_name.rfind('.')));
    std::error_code ec;
    std::filesystem::create_directories(u8path(dir), ec);
    std::string why;
    if (name.empty() || !write_text(dir + name + ".anim", std::string(bytes.begin(), bytes.end()), false, why))
        message("Could not save to the Animations library", dir + name + ".anim\n\n" + why);
    rescan_files();
}

// Inventory files dropped on the view: a .anim is inserted at the frame, or loaded into the actor it is dropped on; a
// project opens.
void App::file_drop(const ImGuiPayload* payload, std::string& hint) {
    const std::string path(static_cast<const char*>(payload->Data));
    LibKind kind = LibKind::Project;
    lib_kind_of(path, kind);
    const std::string name = stem_of(path);
    // A .anim over another actor's body (or your avatar's bones in the world view) loads into that actor (GR-6).
    if (const int actor = kind == LibKind::Anim ? pick_actor(ImGui::GetIO().MousePos) : -1;
        actor >= 0 && actor < int(doc_.project.actors.size())) {
        const std::string who = doc_.project.actors[actor].name;
        hint = "Load " + name + " into " + who;
        if (ImGui::AcceptDragDropPayload("VATS_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) load_actor_file(who, path);
        return;
    }
    hint = kind == LibKind::Anim ? "Insert " + name + " at frame " + std::to_string(int(std::round(frame_))) + (apply_mirrored_ ? " (mirrored)" : "")
                                 : "Open " + name;
    if (!ImGui::AcceptDragDropPayload("VATS_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) return;
    if (kind == LibKind::Anim) insert_anim_file(path, apply_mirrored_);
    else open_path(path);
}

}  // namespace vats
