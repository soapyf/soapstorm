// Viewport Avatar Toolset - document, commands, menus and shortcuts.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats_version.h"
#include "app.h"
#include "profile.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>

#include "icon_button.h"
#include "icons.h"
#include "imgui_internal.h"
#include "vats/anim_convert.h"
#include "vats/bvh.h"
#include "vats/clips.h"
#include "vats/edit.h"
#include "vats/footlock.h"
#include "vats/pose_presets.h"
#include "vats/world_reduce.h"
#include "theme.h"
#ifdef VATS_LEGACY_IMPORT
#include "vats/legacy_import.h"
#endif

namespace vats {
namespace {

constexpr const char* kAppName = "Viewport Avatar Toolset";

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string g_write_error;  // why the last write_file failed (IO-52)

// Writes to a temporary file and renames it over the target, so a failure never leaves half a file (IO-41).
// With backup, the previous file is kept as <path>.bak first.
bool write_file(const std::string& path, const void* data, size_t size, bool backup = false) {
    const std::string tmp = path + ".tmp";
    auto fail = [&](const char* what) {
        g_write_error = std::string(what) + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    };
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot create the file");
        if (!f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)) || !f.flush())
            return fail("writing failed");
    }
    if (std::error_code ec; backup && std::filesystem::exists(u8path(path), ec)) {
        std::remove((path + ".bak").c_str());
        std::rename(path.c_str(), (path + ".bak").c_str());  // best effort: a missing .bak never blocks the save
    }
#ifdef _WIN32
    std::remove(path.c_str());  // rename cannot replace an existing file there; elsewhere it swaps atomically
#endif
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return fail("cannot replace the file");
    return true;
}

std::string file_name(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string extension(const std::string& path) {
    size_t d = path.find_last_of('.');
    std::string e = d == std::string::npos ? "" : path.substr(d + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string with_extension(std::string path, const char* ext) {
    if (extension(path) != ext) path += std::string(".") + ext;
    return path;
}

}  // namespace

// A shortcut as people write it: "Ctrl+Up", "]", "Num ." rather than ImGui's key names.
std::string key_label(ImGuiKeyChord chord) {
    std::string s = ImGui::GetKeyChordName(chord);
    static const std::pair<const char*, const char*> names[] = {
        {"UpArrow", "Up"}, {"DownArrow", "Down"}, {"LeftArrow", "Left"}, {"RightArrow", "Right"},
        {"LeftBracket", "["}, {"RightBracket", "]"}, {"Period", "."}, {"Comma", ","}, {"Slash", "/"},
        {"Semicolon", ";"}, {"Apostrophe", "'"}, {"Minus", "-"}, {"Equal", "="}, {"Backslash", "\\"},
        {"GraveAccent", "`"}, {"KeypadDecimal", "Num ."}, {"KeypadAdd", "Num +"}, {"KeypadSubtract", "Num -"},
        {"Escape", "Esc"}, {"PageUp", "Page Up"}, {"PageDown", "Page Down"}};
    size_t plus = s.rfind('+', s.size() > 1 ? s.size() - 2 : 0);
    std::string head = plus == std::string::npos ? "" : s.substr(0, plus + 1), key = s.substr(head.size());
    for (auto& [from, to] : names)
        if (key == from) return head + to;
    if (key.rfind("Keypad", 0) == 0) return head + "Num " + key.substr(6);
    return s;
}

// Copies the loaded settings into the app's working state (start-up and the IO-54 import).
void App::apply_settings() {
    if (settings_.preset == Preset::SecondLife) tool_ = Tool::Move;
    body_ = Body::SLDefault;
    for (int b = 0; b < kBodyCount; ++b)
        if (settings_.body == kBodyIds[b]) body_ = Body(b);
    orientation_ = settings_.orientation == "world" ? Orientation::World
                   : settings_.orientation == "gimbal" ? Orientation::Gimbal : Orientation::Local;
    show_graph_ = settings_.show_graph;
    gizmo_size_ = settings_.gizmo_size;
    snap_deg_ = settings_.snap_degrees;
    show_welcome_ = settings_.show_welcome && !headless_;
}

bool App::init(float display_scale, std::string& err) {
    const std::string& data_dir = host_.paths().data;
    data_dir_ = data_dir;
    assets_dir_ = host_.paths().assets;
    display_scale_ = display_scale;
    std::error_code ec;
    [[maybe_unused]] const bool first_run = !std::filesystem::exists(u8path(host_.paths().settings), ec);
    settings_.load(host_.paths().settings);
    apply_look();
#ifdef VATS_LEGACY_IMPORT
    offer_migration(first_run);
#endif
    apply_settings();
    const std::string character = host_.paths().character.empty() ? data_dir + "/character" : host_.paths().character;
    if (!skel_.load_dir(character, err)) return false;
    if (!mesh_.load(skel_, character, err)) return false;
    rig_ = std::make_unique<Rig>(skel_);
    mesh_.build(body_);
    new_document();
    find_recoverable();
    build_actions();
    guarded("the library", [&] { load_library(); });
    reopen_quicksave();
    last_tick_ = host_.ticks_ns();
    return true;
}

void App::shutdown() {
    host_.audio_stop();
    if (!keep_autosave_) clear_autosave();  // a clean exit: whatever was unsaved was discarded on purpose
    free_thumbnails();
    help_ui_.reset();  // frees the help's images while the host can still free textures
    reference_ui_.reset();  // and the reference picture's
}

void App::set_body(int b, bool remember) {
    if (!remember && !session_body_) session_body_.emplace(settings_.body, settings_.mesh_body);
    if (remember) session_body_.reset();  // a body chosen in the app is saved as usual
    body_ = Body(std::clamp(b, 0, kBodyCount - 1));
    mesh_.build(body_);
    settings_.body = kBodyIds[int(body_)];
    if (!remember) settings_.mesh_body.clear();  // the Linden body asked for, not a mesh body
    else save_settings();
}

bool App::apply_builtin_pose(const std::string& slug) {
    for (const LibraryItem& it : builtin_poses(skel_)) {
        if (it.id != slug && it.id != "builtin:" + slug) continue;
        Clip before = doc_.clip();
        edit("Apply Pose", [&](Clip& c) {
            apply_pose(c, skel_, it, frame_, false);
            if (it.kind == "hand") apply_pose(c, skel_, it, frame_, true);  // the right hand, mirrored
        });
        offer_pose_blend(std::move(before), frame_);
        evaluate();
        return true;
    }
    return false;
}

void App::apply_look() {
    has_host_colours_ = host_.skin_colours(host_colours_);
    apply_theme(find_theme(settings_.theme), display_scale_ * settings_.interface_size, has_host_colours_ ? &host_colours_ : nullptr);
}

const CameraView* App::project_camera(int slot) const {
    static CameraView v;
    const Json* cams = doc_.project.meta.find("cameras");
    if (!cams || !cams->is_array() || slot >= int(cams->arr.size())) return nullptr;
    const Json& c = cams->arr[slot];
    const Json* t = c.find("target");
    if (!c.is_object() || !t || !t->is_array() || t->arr.size() != 3) return nullptr;
    v = CameraView{true, {t->arr[0].num, t->arr[1].num, t->arr[2].num}, 0, 0, 3.2};
    if (auto* x = c.find("yaw")) v.yaw = x->num;
    if (auto* x = c.find("pitch")) v.pitch = x->num;
    if (auto* x = c.find("distance")) v.distance = x->num;
    return &v;
}

void App::store_project_camera(int slot, const CameraView& v) {
    Json* cams = doc_.project.meta.find("cameras");
    if (!cams || !cams->is_array()) cams = &doc_.project.meta.set("cameras", Json::array());
    while (int(cams->arr.size()) <= slot) cams->push(Json());
    Json c = Json::object(), t = Json::array();
    t.push(v.target.x), t.push(v.target.y), t.push(v.target.z);
    c.set("target", t), c.set("yaw", v.yaw), c.set("pitch", v.pitch), c.set("distance", v.distance);
    cams->arr[slot] = c;
    mark_dirty();
}

// ---------------------------------------------------------------------------------------------
// Document

bool App::guarded(const std::string& path, const std::function<void()>& f) {
    try {
        f();
        return true;
    } catch (const std::exception& e) {
        message("Could not read " + file_name(path), e.what());
        return false;
    }
}

// For other files: the same safe write (temporary file, rename, optional .bak), with the reason on failure.
bool App::write_text(const std::string& path, const std::string& text, bool backup, std::string& why) {
    if (write_file(path, text.data(), text.size(), backup)) return true;
    why = g_write_error;
    return false;
}

void App::mark_dirty() {
    blocking_after_edit();  // 08 KT-2
    doc_.dirty = true;
    update_title();
}

void App::update_title() {
    std::string t = (doc_.path.empty() ? "Untitled" : file_name(doc_.path)) + (doc_.dirty ? "*" : "") + " - " + kAppName + " " + VATS_VERSION;
    host_.set_title(t);
}

void App::new_document() {
    ++doc_generation_;  // anything tied to the old document (a mocap take) lets go
    clear_autosave();
    raw_import_.reset();
    doc_ = Document();
    graph_.clear_snapshot();  // PT-4
    scratch_.reset(), scratch_marks_.clear();  // PT-2: the scratch pose and its history were the old document's
    pinned_ghosts_.clear();  // 08 ON-5: they point at the old document's frames and actors
    doc_.clip().loop_tangents = true;  // 08 LP-7: on for new projects (opened ones keep their own)
    clip_replaced();
    clear_selection();
    frame_ = 0;
    playing_ = false;
    update_title();
}

bool App::save(const std::string& path) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    // Prop paths are written relative to the project where possible (IO-42); in memory they stay absolute.
    Project copy = doc_.project;
    const std::string dir = path.substr(0, path.find_last_of('/'));
    for_each_clip(copy, [&](Clip& c) {  // every actor's clip of every take (08 CL)
        for (Prop& p : c.props) p.path = prop_path_to_stored(p.path, dir);
        if (c.audio) c.audio->path = prop_path_to_stored(c.audio->path, dir);  // AU-1, like props
        if (c.reference) c.reference->path = prop_path_to_stored(c.reference->path, dir);  // 08 RF
    });
    std::string text = save_project(copy);
    if (!write_file(path, text.data(), text.size(), true)) {
        message("Save failed", "Could not write " + path + "\n\n" + g_write_error);
        return false;
    }
    doc_.path = path;
    doc_.dirty = false;
    doc_.project.migrated = false;
    clear_autosave();
    if (!headless_) settings_.add_recent(path), save_settings();  // UI-29: not from scripted runs
    update_title();
    rescan_files();  // the Inventory's Projects
    status("Saved " + file_name(path));
    return true;
}

void App::load_project_file(const std::string& path, bool example) {
    std::string text, err;
    Project p;
    if (!read_file(path, text) || !load_project(text, p, err, path)) {
        message("Could not open project", path + "\n\n" + (err.empty() ? "The file could not be read." : err));
        return;
    }
    new_document();
    doc_.project = std::move(p);
    clip_replaced();  // history and body follow the file's active actor, not the empty document's
    const std::string dir = path.substr(0, path.find_last_of('/'));
    for_each_clip(doc_.project, [&](Clip& c) {  // every actor's clip of every take (08 CL)
        for (Prop& prop : c.props) {
            prop.path = prop_path_from_stored(prop.path, dir);
            // A starter prop saved on another computer (or by the help's examples): this installation's copy.
            if (prop.lib_id.rfind("starter-", 0) == 0 && !prop_model(prop.path))
                if (const PropLibraryItem* it = find_prop_item(prop.lib_id)) prop.path = it->prop.path;
        }
        if (c.audio) c.audio->path = prop_path_from_stored(c.audio->path, dir);
        if (c.reference) c.reference->path = prop_path_from_stored(c.reference->path, dir);  // 08 RF
    });
    if (auto miss = missing_prop_meshes(); !miss.empty()) {  // IO-42: placeholders, never a failed load
        std::string t;
        for (auto& m : miss) t += "- " + m + "\n";
        message("Some prop meshes are missing", t + "\nThey show as orange boxes until the files are back.");
    }
    // A converted project or a newer file is saved under a new name, never over the original.
    doc_.path = doc_.project.migrated || doc_.project.read_only || example ? "" : path;
    if (!headless_ && !example) settings_.add_recent(path), save_settings();
    update_title();
    std::string opened = "Opened " + file_name(path);
#ifdef VATS_LEGACY_IMPORT
    if (doc_.project.migrated) opened += legacy_import::kConvertedNote;
#endif
    if (example) opened += " (an example: Save As to keep your changes)";
    status(opened);
    if (doc_.project.read_only)
        message("Newer project file", "This project was saved by a newer version of VATs. Some parts may be missing; "
                                      "save it under a new name to keep the original.");
}

void App::import_file(const std::string& path) {
    std::string ext = extension(path), text;
    if (!read_file(path, text)) {
        message("Import failed", "Could not read " + path);
        return;
    }
    Clip clip;
    std::vector<std::string> report;
    std::optional<RawAnim> raw;  // IO-22: an untouched import re-exports byte for byte
    if (ext == "anim") {
        AnimFile f;
        std::string err;
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        if (!parse_anim(bytes, f, err)) {
            message("Import failed", file_name(path) + ": " + err);
            return;
        }
        auto problems = validate_anim(f, skel_, false);
        if (!problems.empty()) {
            message("Import failed", file_name(path) + " is not a valid SL animation: " + problems.front());
            return;
        }
        auto r = import_anim(skel_, f);
        clip = std::move(r.clip);
        report = std::move(r.report);
        raw = RawAnim{f, clip};
    } else {
        // IO-35: optional key reduction with the export defaults.
        auto r = import_bvh(skel_, text, settings_.bvh_reduce ? BvhImportOptions{0.05, 0.0005} : BvhImportOptions{});
        if (!r.ok) {
            message("Import failed", file_name(path) + ": " + r.error);
            return;
        }
        clip = std::move(r.clip);
        report = std::move(r.report);
    }
    std::vector<Prop> props = std::move(doc_.clip().props);  // imports keep the scene's props (IO-23)
    new_document();
    raw_import_ = std::move(raw);
    doc_.clip() = std::move(clip);
    doc_.clip().loop_tangents = true;  // a new project (08 LP-7)
    doc_.clip().props = std::move(props);
    doc_.dirty = true;
    update_title();
    status("Imported " + file_name(path) + ": " + std::to_string(doc_.clip().end_frame) + " frames at " +
           std::to_string(doc_.clip().fps) + " fps, " + std::to_string(doc_.clip().curves.size()) + " bones" +
           // IO-18: the rate is inferred; the Frame rate field's "Keep Timing" is the override.
           (ext == "anim" ? " (fps inferred: change Frame rate and pick Keep Timing to override)" : ""));
    if (!report.empty()) {
        std::string text2;
        for (auto& line : report) text2 += "- " + line + "\n";
        message("Import notes", text2);
    }
}

// GR-6: an animation file into an actor ("" = a new actor): a .anim, a BVH on the SL skeleton, or one actor of a
// project (asked which when it has several). put_clip_in_actor asks before replacing keys and does the rest.
void App::load_actor_file(const std::string& actor, const std::string& path) {
    std::string text, err;
    const std::string what = file_name(path), ext = extension(path);
    if (!read_file(path, text)) return message("Could not load " + what, "The file could not be read.");
    if (ext == "anim") {
        AnimFile f;
        if (!parse_anim(std::vector<std::uint8_t>(text.begin(), text.end()), f, err)) return message("Could not load " + what, err);
        if (auto problems = validate_anim(f, skel_, false); !problems.empty())
            return message("Could not load " + what, what + " is not a valid SL animation: " + problems.front());
        return put_clip_in_actor(actor, path, import_anim(skel_, f).clip, nullptr);
    }
    if (ext == "bvh") {
        auto r = import_bvh(skel_, text, settings_.bvh_reduce ? BvhImportOptions{0.05, 0.0005} : BvhImportOptions{});
        if (!r.ok)
            return message("Could not load " + what, r.error + "\n\nA BVH from another rig: load it with File > Import Animation "
                                                                "(Retarget)... and save it as a project first.");
        return put_clip_in_actor(actor, path, std::move(r.clip), nullptr);
    }
    Project q;
    if (!load_project(text, q, err, path)) return message("Could not load " + what, err);
    if (q.actors.size() < 2) return put_clip_in_actor(actor, path, std::move(q.clip), nullptr);
    std::vector<std::string> names;
    for (const Actor& a : q.actors) names.push_back(a.name);
    names.push_back("Cancel");
    host_.ask("Load which actor?", what + " has " + std::to_string(q.actors.size()) + " actors. Load the animation of:", names,
              [this, actor, path, q](int k) {
                  if (k >= 0 && k < int(q.actors.size())) put_clip_in_actor(actor, path, actor_clip(q, k), &q.actors[k]);
              });
}

// One actor alone: as a project of its own (its clip, props and export settings), or its .anim as Export writes it
// with its own bake shape and settings.
void App::save_actor(const std::string& actor, const std::string& path, bool anim) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    const int i = actor_index(actor);
    if (i < 0) return status(actor + " is no longer in the project");
    if (doc_.history.is_open() || scene_busy()) return status("Finish the current edit first");
    Project& pr = doc_.project;
    if (anim) {
        const int home = pr.active;
        set_active_actor(pr, i);
        export_anim(path);  // says what it wrote, or why not
        set_active_actor(pr, home);
        return;
    }
    Project one;
    one.clip = actor_clip(pr, i);
    const std::string dir = path.substr(0, path.find_last_of('/'));
    for (Prop& p : one.clip.props) p.path = prop_path_to_stored(p.path, dir);
    if (one.clip.audio) one.clip.audio->path = prop_path_to_stored(one.clip.audio->path, dir);
    if (one.clip.reference) one.clip.reference->path = prop_path_to_stored(one.clip.reference->path, dir);
    const std::string text = save_project(one);
    if (!write_file(path, text.data(), text.size(), true)) return message("Save failed", "Could not write " + path + "\n\n" + g_write_error);
    rescan_files();
    status("Saved " + actor + " as " + file_name(path));
}

// The options every .anim export of the active actor uses: bake shape (IO-13), positions (IO-11), the key-reduction
// tolerances (IO-14) and cross-actor pins (GR-4).
AnimExportOptions App::anim_export_options() {
    AnimExportOptions opt;
    opt.shape = export_shape();
    opt.positions = export_positions();
    opt.worn_overrides = host_.joint_overrides();  // the viewer: warns when face positions would pin a mesh head
    if (const Json* v = export_home_settings().find("leave_static"); v && v->is_bool()) opt.leave_out_static_rotations = v->b;  // IO-11b
    if (multi_actor()) opt.external = actor_resolver(doc_.project.active);
    // IO-14: the project's key-reduction tolerances, [rotation degrees, position metres].
    if (const Json* reduce = doc_.clip().export_settings.find("reduce"); reduce && reduce->is_array() && reduce->arr.size() == 2 &&
                                                                          reduce->arr[0].is_number() && reduce->arr[1].is_number())
        opt.reduce_rot_deg = reduce->arr[0].num, opt.reduce_pos_m = reduce->arr[1].num;
    // IO-14w (08 WR): "anywhere on the body", in metres, when that mode is chosen.
    const Json& ex = doc_.clip().export_settings;
    if (const Json* m = ex.find("reduce_mode"); m && m->is_string() && m->str == "world") {
        const Json* w = ex.find("reduce_world");
        opt.reduce_world_m = w && w->is_number() && w->num > 0 ? w->num : kReduceWorldDefault;
    }
    if (height_shape_) opt.shape = height_shape_, opt.positions = nullptr;  // HV: a height variant bakes on its body
    return opt;
}

Clip App::anim_export_clip() const {
    return doc_.clip().mirror_export ? mirrored_clip(skel_, doc_.clip()) : doc_.clip();
}

// The .anim the project exports, made in memory and saying nothing: 1 exported, 2 an imported .anim nobody has
// edited, going back out exactly as it came in (IO-22). r.errors non-empty = it must not be written; bytes still
// hold whatever file there is (the upload meter and the SL preview show an oversize one too).
int App::export_in_memory(AnimExportResult& r, std::vector<std::uint8_t>& bytes) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    if (raw_import_ && !doc_.clip().mirror_export && !height_shape_)
        if (const AnimFile* same = raw_reexport(*raw_import_, doc_.clip())) {
            r.file = *same;
            bytes = write_anim(*same);
            return 2;
        }
    r = vats::export_anim(skel_, anim_export_clip(), anim_export_options());
    bytes = write_anim(r.file);
    return 1;
}

// The .anim bytes the project exports: 0 when it cannot (after saying why), else as export_in_memory.
int App::anim_bytes(AnimExportResult& r, std::vector<std::uint8_t>& bytes) {
    const int made = export_in_memory(r, bytes);
    if (!r.errors.empty()) {
        std::string t;
        for (auto& e : r.errors) t += "- " + e + "\n";
        message("Cannot export", t);
        return 0;
    }
    return made;
}

bool App::face_check(FaceCheck& out, std::string& why) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    AnimExportResult r;
    if (!anim_bytes(r, out.bytes)) return why = "nothing to export", false;
    Json& ex = doc_.clip().export_settings;
    const Json saved = ex;  // the control: the same export on SL Default; put back as it was, no undo step
    if (!ex.is_object()) ex = Json::object();
    ex.set("shape", std::string("sl-default"));
    const int made = anim_bytes(r, out.control);
    ex = saved;
    if (!made) return why = "the SL Default export failed", false;
    const Clip& c = doc_.clip();
    out.fps = std::max(c.fps, 1);
    std::vector<double> keys{0, double(c.end_frame)};
    for (auto& [name, track] : c.curves) {
        const int n = skel_.find(name);
        if (n < 0 || skel_[n].category != Category::Face) continue;
        for (auto& [ch, curve] : track)
            for (const Key& k : curve.keys) keys.push_back(std::clamp(std::round(k.frame), 0.0, double(c.end_frame)));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const size_t step = (keys.size() + 39) / 40;  // at most 40 samples, the first and last kept
    for (size_t i = 0; i < keys.size(); i += step) out.frames.push_back(keys[i]);
    if (out.frames.back() != keys.back()) out.frames.push_back(keys.back());
    rig_->external = multi_actor() ? actor_resolver(doc_.project.active) : ExternalTarget{};
    for (double f : out.frames) out.poses.push_back(vats::evaluate(*rig_, c, f, shape()).pose);
    return true;
}

bool App::export_anim(const std::string& path) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    AnimExportResult r;
    std::vector<std::uint8_t> bytes;
    const int made = anim_bytes(r, bytes);
    if (!made) return false;
    if (!write_file(path, bytes.data(), bytes.size())) {
        message("Export failed", "Could not write " + path + "\n\n" + g_write_error);
        return false;
    }
    rescan_files();  // the export folder may be one of the Inventory's
    if (made == 2) {
        export_summary_ = "unchanged since import, written as it was";
        status("Exported " + file_name(path) + ": " + export_summary_);
        return true;
    }
    // UI-34: bones, length, priority, and each animated attachment point with what it does.
    char buf[160];
    std::snprintf(buf, sizeof buf, "%zu bones, %.2f s, priority %d, %zu bytes", r.file.joints.size(), r.file.duration,
                  r.file.base_priority, bytes.size());
    export_summary_ = buf;
    std::string points;
    for (const AnimJoint& j : r.file.joints) {
        int n = skel_.find_viewer(j.name);
        if (n < 0 || !skel_[n].attachment || skel_[n].volume) continue;
        const bool moves = !j.pos.empty(), turns = !j.rot.empty();
        points += (points.empty() ? "" : ", ") + skel_[n].name + (moves && turns ? " (moves, rotates)" : moves ? " (moves)" : " (rotates)");
    }
    if (!points.empty()) export_summary_ += "; points: " + points;
    if (r.static_positions)  // IO-11a
        export_summary_ += "; " + std::to_string(r.static_positions) + " unmoving position channels left out";
    if (r.static_rotations)  // IO-11b
        export_summary_ += "; " + std::to_string(r.static_rotations) + " bones that don't move left out";
    if (r.file.duration > 60) export_summary_ += "; over SL's 60 s limit";
    status("Exported " + file_name(path) + ": " + export_summary_);
    if (!r.warnings.empty()) {
        std::string t;
        for (auto& w : r.warnings) t += "- " + w + "\n";
        message("Exported with warnings", t);
    }
    return true;
}

bool App::export_bvh(const std::string& path, bool all_bones) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    BvhExportOptions opt;
    opt.all_bones = all_bones;
    const Json* positions = doc_.clip().export_settings.find("bvh_positions");
    opt.joint_positions = positions && positions->is_bool() && positions->b;
    opt.shape = export_shape();
    opt.positions = export_positions();
    if (multi_actor()) opt.external = actor_resolver(doc_.project.active);
    Clip clip = doc_.clip().mirror_export ? mirrored_clip(skel_, doc_.clip()) : doc_.clip();
    auto r = vats::export_bvh(skel_, clip, opt);
    if (!write_file(path, r.text.data(), r.text.size())) {
        message("Export failed", "Could not write " + path + "\n\n" + g_write_error);
        return false;
    }
    // Losses were shown before writing (UI-35); the status notes moved bones below the hip.
    int moved = 0;
    for (auto& [track, curves] : clip.curves) {
        int n = skel_.find(track);
        moved += n >= 0 && track != "mPelvis" && !skel_[n].attachment && clip.has_channels(track, kPosChannels);
    }
    moved = std::max(moved - r.static_positions, 0);  // positions that move nothing are not written (IO-11a)
    export_summary_ = moved == 0 ? std::string()
                      : std::to_string(moved) + (opt.joint_positions ? " moved bones below the hip written with positions"
                                                                     : " moved bones below the hip kept rotation only");
    status("Exported " + file_name(path) + (export_summary_.empty() ? "" : ": " + export_summary_));
    return true;
}

void App::guard_unsaved(std::function<void()> then) {
    if (!doc_.dirty || headless_) {
        then();
        return;
    }
    std::string text = "Save changes to " + (doc_.path.empty() ? std::string("Untitled") : file_name(doc_.path)) + "?";
    host_.ask(kAppName, text, {"Save", "Don't Save", "Cancel"}, [this, then = std::move(then)](int choice) mutable {
        if (choice == 1) {
            then();
        } else if (choice == 0) {
            if (doc_.path.empty()) {
                show_dialog(Dialog::SaveAs, std::move(then));  // the action continues once Save As has saved
            } else if (save(doc_.path)) {
                then();
            }
        }
    });
}

void App::request_quit() {
    guard_unsaved([this] { quit_ = true; });
}

// The system asked the app to stop (SIGTERM, logout, a test harness). Nobody may be there to answer a
// "Save changes?" prompt, so unsaved work goes to the autosave, which the next launch offers to recover.
void App::quit_unattended() {
    if (doc_.dirty && !headless_) keep_autosave_ = write_autosave();
    quit_ = true;
}

void App::open_path(const std::string& path) {
    std::string ext = extension(path);
    if (ext == "dae" || ext == "fbx")
        guarded(path, [&] { import_prop(path); });
    else if (ext == "anim" || ext == "bvh")
        guard_unsaved([this, path] { guarded(path, [&] { import_file(path); }); });
    else if (ext == "gltf" || ext == "glb")  // as File > Import Animation (Retarget)...
        guard_unsaved([this, path] { guarded(path, [&] { open_retarget(path); }); });
    else if (ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac")  // as File > Load Audio...
        guarded(path, [&] { load_audio(path); });
    else
        guard_unsaved([this, path] { guarded(path, [&] { load_project_file(path); }); });
}

void App::show_dialog(Dialog kind, std::function<void()> after_save) {
    if (kind == Dialog::SaveAs) after_save_as_ = std::move(after_save);  // another dialog leaves a pending action alone
    // Save As starts beside the current project (UI-36).
    const std::string dir = doc_.path.empty() ? "" : doc_.path.substr(0, doc_.path.find_last_of('/') + 1);
    const ui::FileFilter mesh{"Mesh", "dae;fbx"};
    std::string stem = doc_.path.empty() ? "Animation" : file_name(doc_.path).substr(0, file_name(doc_.path).rfind('.'));
    std::vector<ui::FileFilter> projects = {{"VATs project", "vat"}};
    std::string animations = "anim;bvh;vat";
#ifdef VATS_LEGACY_IMPORT
    projects.push_back({legacy_import::kFilterName, legacy_import::kFilterExtensions});
    animations += std::string(";") + legacy_import::kFilterExtensions;
#endif
    switch (kind) {
        case Dialog::Open: host_.open_file_dialog(projects, false, dialog_result(kind)); break;
        case Dialog::SaveAs: host_.save_file_dialog({{"VATs project", "vat"}}, dir + stem + ".vat", dialog_result(kind)); break;
        case Dialog::ImportAnim: host_.open_file_dialog({{"SL animation", "anim"}}, false, dialog_result(kind)); break;
        case Dialog::ImportBvh: host_.open_file_dialog({{"BVH motion", "bvh"}}, false, dialog_result(kind)); break;
        case Dialog::ImportProp: host_.open_file_dialog({mesh}, false, dialog_result(kind)); break;
        case Dialog::ImportRetarget:
            host_.open_file_dialog({{"Humanoid animation", "bvh;fbx;gltf;glb"}}, false, dialog_result(kind));
            break;
        case Dialog::LoadAudio: host_.open_file_dialog({{"Audio", "wav;mp3;ogg;flac"}}, false, dialog_result(kind)); break;
        case Dialog::ImportBody: host_.open_file_dialog({mesh}, true, dialog_result(kind)); break;
        case Dialog::ExportFolder: host_.open_folder_dialog("", dialog_result(kind)); break;
        case Dialog::LoadActor:
            host_.open_file_dialog({{"Animation", animations}}, false, dialog_result(kind));
            break;
        case Dialog::SaveActor:
            host_.save_file_dialog({{"VATs project", "vat"}}, dir + stem + "_" + file_actor_ + ".vat", dialog_result(kind));
            break;
        case Dialog::ExportActor:
            host_.save_file_dialog({{"SL animation", "anim"}}, dir + stem + "_" + file_actor_ + ".anim", dialog_result(kind));
            break;
        case Dialog::AoNotecard:
            host_.save_file_dialog({{"Text", "txt"}}, dir + stem + (ao_format_ ? "_ZHAO.txt" : "_FirestormAO.txt"), dialog_result(kind));
            break;
        case Dialog::SitLines:
            host_.save_file_dialog({{"Text", "txt"}}, dir + stem + (sit_format_ ? "_nPose.txt" : "_AVpos.txt"), dialog_result(kind));
            break;
        case Dialog::ExpressionPack: host_.open_folder_dialog("", dialog_result(kind)); break;
        case Dialog::Rhubarb:
            host_.open_file_dialog({{"Rhubarb Lip Sync (JSON or TSV)", "json;tsv;txt"}}, false, dialog_result(kind));
            break;
        case Dialog::LoadReference: host_.open_file_dialog({{"PNG picture", "png"}}, false, dialog_result(kind)); break;
        case Dialog::ListingMedia:  // 08 LM
            if (listing_png_) host_.save_file_dialog({{"PNG pictures", "png"}}, dir + stem + "_listing.png", dialog_result(kind));
            else host_.save_file_dialog({{"Animated GIF", "gif"}}, dir + stem + "_listing.gif", dialog_result(kind));
            break;
    }
}

ui::FilesChosen App::dialog_result(Dialog kind, bool folder_of_file) {
    return [this, kind, folder_of_file](std::vector<std::string> files) {
        std::string joined;
        for (size_t i = 0; i < files.size(); ++i) joined += (i ? "\n" : "") + files[i];
        if (folder_of_file && !joined.empty()) joined = joined.substr(0, joined.find_last_of("/\\"));  // a Save dialog picked the folder
        std::lock_guard<std::mutex> lock(dialog_mutex_);
        dialog_results_.emplace_back(kind, joined);
    };
}

// ---------------------------------------------------------------------------------------------
// Commands

void App::build_actions() {
    auto need_selection = [this]() -> const char* { return selection_.empty() ? "Select a bone first" : nullptr; };
    auto add = [this](const char* id, Action a) { actions_.emplace_back(id, std::move(a)); };
    add_time_actions(add);  // time editing and the audio track (audio_track.cpp)
    const ImGuiKeyChord ctrl = ImGuiMod_Ctrl, shift = ImGuiMod_Shift, alt = ImGuiMod_Alt;

    // No document change while an actor is being dragged: the drag's start state would outlive it.
    auto not_busy = [this]() -> const char* { return scene_busy() ? "Finish the current edit first" : nullptr; };
    add("new", {"New", ctrl | ImGuiKey_N, 0, false, [this] { guard_unsaved([this] { new_document(); }); }, not_busy});
    add("open", {"Open...", ctrl | ImGuiKey_O, 0, false, [this] { guard_unsaved([this] { show_dialog(Dialog::Open); }); }, not_busy});
    add("save", {"Save", ctrl | ImGuiKey_S, 0, false,
                 [this] { doc_.path.empty() ? show_dialog(Dialog::SaveAs) : (void)save(doc_.path); }, {}});
    add("save_as", {"Save As...", ctrl | shift | ImGuiKey_S, 0, false, [this] { show_dialog(Dialog::SaveAs); }, {}});
    add("save_to_library", {"Save to Library...", 0, 0, false, [this] { save_to_library(); }, {}});
    add("import_bvh", {"Import BVH...", 0, 0, false,
                       [this] { guard_unsaved([this] { show_dialog(Dialog::ImportBvh); }); }, {}});
    add("import_retarget", {"Import Animation (Retarget)...", 0, 0, false, [this] { guard_unsaved([this] { show_dialog(Dialog::ImportRetarget); }); }, {}});
    add("batch_retarget", {"Batch Retarget Folder...", 0, 0, false, [this] {  // 07 RT-13
                               show_batch_retarget_ = true;
                               batch_ui_ = nullptr;  // a fresh dialog re-reads the rig tables and saved mappings
                           }, {}});
    add("import_anim", {"Import SL .anim...", 0, 0, false,
                        [this] { guard_unsaved([this] { show_dialog(Dialog::ImportAnim); }); }, {}});
    add("import_prop", {"Import Prop / Mesh (.dae, .fbx)...", ctrl | ImGuiKey_I, 0, false, [this] { show_dialog(Dialog::ImportProp); }, {}});
    // Export settings first (UI-31); the dialog's button writes straight to the export folder.
    add("export_anim", {"Export SL .anim...", ctrl | ImGuiKey_E, 0, false, [this] { show_export_dialog_ = true; }, {}});
    if (host_.can_upload()) add("upload", {"Upload Animation...", 0, 0, false, [this] { upload_now(); }, {}});
    add("export_bvh", {"Export BVH (Animated Bones)...", 0, 0, false, [this] { export_now(true, false); }, {}});
    add("export_bvh_all", {"Export BVH (All Bento Bones)...", 0, 0, false, [this] { export_now(true, true); }, {}});
    // 08 CL-4: every clip, each with its own export settings.
    auto several_clips = [this]() -> const char* { return clip_count(doc_.project) > 1 ? nullptr : "The project has one clip (Tools > Clips)"; };
    add("export_all_clips", {"Export All Clips (.anim)", 0, 0, false, [this] { export_now(false, false, true); }, several_clips});
    if (host_.can_upload())
        add("upload_all_clips", {"Upload All Clips...", 0, 0, false, [this] { upload_now(true); }, several_clips});
    // In the viewer the editor closes and the viewer stays.
    add("quit", {host_.world_view() ? "Close Editor" : "Quit", ctrl | ImGuiKey_Q, 0, false, [this] { request_quit(); }, {}});

    add("undo", {"Undo", ctrl | ImGuiKey_Z, 0, false,
                 [this] {
                     std::string label = doc_.history.undo_label();
                     apply_restore(doc_.history.undo_step());
                     clip_replaced();
                     mark_dirty();
                     status("Undid " + label);
                 },
                 [this]() -> const char* {
                     return doc_.history.is_open() || scene_busy() ? "Finish the current edit first"
                            : doc_.history.can_undo() ? nullptr : "Nothing to undo";
                 }});
    add("redo", {"Redo", ctrl | ImGuiKey_Y, ctrl | shift | ImGuiKey_Z, false,
                 [this] {
                     apply_restore(doc_.history.redo_step());
                     clip_replaced();
                     mark_dirty();
                     status("Redid " + doc_.history.undo_label());
                 },
                 [this]() -> const char* {
                     return doc_.history.is_open() || scene_busy() ? "Finish the current edit first"
                            : doc_.history.can_redo() ? nullptr : "Nothing to redo";
                 }});
    add("key", {"Set Key", ImGuiKey_S, 0, false,
                [this] {
                    if (scratch_changed()) return scratch_end(true);  // PT-2: Set Key keeps a scratch pose
                    edit("Set Key", [&](Clip& c) {
                        for (int n : selection_) {
                            key_current(c, skel_, n, frame_);
                            const std::string pin = "pin:" + skel_[n].name;  // and the pin offset (AM-31)
                            if (c.curves.count(pin) || pin_at(c, *rig_, n, frame_) >= 0) {
                                key_euler(c, pin, frame_, curve_euler(c, pin, frame_));
                                key_offset(c, pin, frame_, curve_offset(c, pin, frame_));
                            }
                        }
                        for (auto& h : handles_) {  // IK controllers key their target and pole (AM-31)
                            const LimbState& s = limb_states_[h.limb];
                            key_limb_target(c, *rig_, frame_, h.limb, s.target, shape());
                            if (!rig_->limbs()[h.limb].spine) key_limb_pole(c, *rig_, frame_, h.limb, s.pole, shape());
                        }
                    });
                    status("Keyed " + std::to_string(selection_.size() + handles_.size()) + " item(s) at frame " +
                           std::to_string(int(frame_)));
                },
                [this]() -> const char* {
                    return selection_.empty() && handles_.empty() && !scratch_changed() ? "Select a bone first" : nullptr;
                }});
    // Spec 08 TW-1: a modal drag, like Blender's breakdowner; the timeline bar has the same as a slider.
    add("tween", {"Tween (Breakdown)", shift | ImGuiKey_E, 0, false, [this] { start_tween(); },
                  [this]() -> const char* { return selection_.empty() && handles_.empty() ? "Select a bone first" : nullptr; }});
    add("key_all", {"Set Key on All Visible Bones", shift | ImGuiKey_S, 0, false,
                    [this] {
                        int n = 0;
                        edit("Set Key on All Visible Bones", [&](Clip& c) {
                            for (int i = 0; i < skel_.size(); ++i)
                                if (node_visible(i)) key_current(c, skel_, i, frame_), ++n;
                        });
                        status("Keyed " + std::to_string(n) + " bones at frame " + std::to_string(int(frame_)));
                    },
                    {}});
    add("delete_key", {"Delete Key", ImGuiKey_Delete, ImGuiKey_Backspace, false,
                       [this] {
                           if (keys_hovered()) return (void)with_graph([&](GraphContext& g) { graph_.delete_selected(g); });
                           if (selected_prop_ >= 0) {  // Delete removes a selected prop (VP-82)
                               int i = selected_prop_;
                               selected_prop_ = -1;
                               return edit("Remove Prop", [&](Clip& c) { c.props.erase(c.props.begin() + i); });
                           }
                           int n = 0;
                           auto tracks = selected_tracks();
                           edit("Delete Key", [&](Clip& c) {
                               for (auto& t : tracks) n += delete_keys_at(c, t, frame_);
                           });
                           status(n ? "Deleted keys at frame " + std::to_string(int(frame_)) : "No keys here to delete");
                       },
                       [this]() -> const char* {
                           return keys_hovered() || selected_prop_ >= 0 || !selection_.empty() ? nullptr : "Select a bone first";
                       }});
    add("delete_frame", {"Delete Keys on All Bones at Frame", shift | ImGuiKey_Delete, 0, false,
                         [this] {
                             int n = 0;
                             edit("Delete Keys at Frame", [&](Clip& c) { n = delete_keys_at_all(c, frame_); });
                             status(std::to_string(n) + " key(s) deleted at frame " + std::to_string(int(frame_)));
                         },
                         {}});
    add("reset_bone", {"Reset Selected Bone", alt | ImGuiKey_R, 0, false,
                       [this] {
                           edit("Reset Bone", [&](Clip& c) {
                               for (int s : selection_) reset_bone(c, skel_[s].name, frame_);
                           });
                       },
                       need_selection});
    add("reset_hip", {"Reset Hip Position", alt | ImGuiKey_W, alt | ImGuiKey_H, false,  // Alt+H: in the viewer Alt+W is its camera
                      [this] {
                          if (!doc_.clip().has_channels("mPelvis", kPosChannels)) {
                              status("The hip has no position keys");
                              return;
                          }
                          edit("Reset Hip Position", [&](Clip& c) { key_offset(c, "mPelvis", frame_, {}); });
                      },
                      {}});
    add("reset_pose", {"Reset Whole Pose", alt | shift | ImGuiKey_R, 0, false,
                       [this] {
                           edit("Reset Pose", [&](Clip& c) {
                               std::vector<std::string> names;
                               for (auto& [name, t] : c.curves)  // bones only: resetting ik. would collapse IK limbs
                                   if (name.rfind("ik.", 0) != 0 && name.rfind("pin:", 0) != 0) names.push_back(name);
                               for (auto& n : names) reset_bone(c, n, frame_);
                           });
                       },
                       {}});
    add("mirror_bone", {"Mirror Bone to Other Side", ImGuiKey_M, 0, false,
                        [this] { edit("Mirror Bone", [&](Clip& c) { mirror_bones(c, skel_, frame_, pose_, selection_); }); },
                        need_selection});
    auto mirror = [this](MirrorMode mode, const char* label) {
        edit(label, [&](Clip& c) { mirror_pose(c, skel_, frame_, pose_, mode); });
        status(std::string(label) + " at frame " + std::to_string(int(frame_)));
    };
    add("mirror_l2r", {"Mirror Left to Right", 0, 0, false, [=] { mirror(MirrorMode::LeftToRight, "Mirror Left to Right"); }, {}});
    add("mirror_r2l", {"Mirror Right to Left", 0, 0, false, [=] { mirror(MirrorMode::RightToLeft, "Mirror Right to Left"); }, {}});
    add("flip_pose", {"Flip Pose", 0, 0, false, [=] { mirror(MirrorMode::Flip, "Flip Pose"); }, {}});
    add("reverse", {"Reverse Animation", 0, 0, false,
                    [this] {
                        edit("Reverse Animation", [&](Clip& c) { reverse_clip(c); });
                        status("The animation now plays backwards");
                    },
                    {}});

    add("play", {"Play / Pause", ImGuiKey_Space, 0, false,
                 [this] {
                     playing_ = !playing_;
                     if (!playing_) frame_ = std::round(frame_);
                     last_tick_ = host_.ticks_ns();
                 },
                 {}});
    add("next_frame", {"Next Frame", ImGuiKey_RightArrow, 0, true,
                       [this] { set_frame(frame_ + 1 > doc_.clip().end_frame ? 0 : std::floor(frame_) + 1); }, {}});
    add("prev_frame", {"Previous Frame", ImGuiKey_LeftArrow, 0, true,
                       [this] { set_frame(frame_ - 1 < 0 ? doc_.clip().end_frame : std::ceil(frame_) - 1); }, {}});
    add("next_key", {"Next Key", ImGuiKey_Period, 0, true, [this] { step_key(+1); }, {}});
    add("prev_key", {"Previous Key", ImGuiKey_Comma, 0, true, [this] { step_key(-1); }, {}});
    add("start", {"Go to Start", ImGuiKey_Home, 0, false, [this] { set_frame(0); }, {}});
    add("end", {"Go to End", ImGuiKey_End, 0, false, [this] { set_frame(doc_.clip().end_frame); }, {}});

    add("tool_select", {"Select Tool", ImGuiKey_Q, 0, false, [this] { tool_ = Tool::Select; }, {}});
    // In the Blender preset G / R over the view start a modal transform instead (VP-51).
    auto blender_modal = [this] { return settings_.preset == Preset::Blender && viewport_hovered_ && !ImGui::GetIO().WantTextInput; };
    add("tool_move", {"Move Tool", ImGuiKey_W, 0, false, [this, blender_modal] { blender_modal() ? start_modal(Modal::Move) : void(tool_ = Tool::Move); }, {}});
    add("tool_rotate", {"Rotate Tool", ImGuiKey_E, 0, false,
                        [this, blender_modal] { blender_modal() ? start_modal(Modal::Rotate) : void(tool_ = Tool::Rotate); }, {}});
    add("tool_scale", {"Scale Tool", ImGuiKey_R, 0, false, [this] { tool_ = Tool::Scale; }, {}});
    add("orientation", {"Cycle Local / World / Gimbal Axes", ImGuiKey_O, 0, false,
                        [this] {
                            orientation_ = orientation_ == Orientation::Local   ? Orientation::World
                                           : orientation_ == Orientation::World ? Orientation::Gimbal
                                                                                : Orientation::Local;
                            settings_.orientation = orientation_ == Orientation::Local   ? "local"
                                                    : orientation_ == Orientation::World ? "world" : "gimbal";
                            save_settings();
                            status(orientation_ == Orientation::Local   ? "Gizmo axes: the bone's own"
                                   : orientation_ == Orientation::World ? "Gizmo axes: world (X forward, Y left, Z up)"
                                                                        : "Gimbal: each ring turns exactly one rotation channel");
                        },
                        {}});
    // View presets turn the camera smoothly, like the view cube (VP-I4).
    add("view_front", {"Front", ImGuiKey_1, 0, false, [this] { look_from({1, 0, 0}); }, {}});
    add("view_back", {"Back", ctrl | ImGuiKey_1, 0, false, [this] { look_from({-1, 0, 0}); }, {}});
    add("view_right", {"Right", ImGuiKey_3, 0, false, [this] { look_from({0, -1, 0}); }, {}});
    add("view_left", {"Left", ctrl | ImGuiKey_3, 0, false, [this] { look_from({0, 1, 0}); }, {}});
    add("view_top", {"Top", ImGuiKey_7, 0, false, [this] { look_from({0, 0, 1}); }, {}});
    // VP-67: Numpad 5 in every preset (no preset table rebinds it).
    add("view_ortho", {"Orthographic", ImGuiKey_Keypad5, 0, false,
                       [this] {
                           if (!host_.set_orthographic(!camera_.ortho)) return status("This view has no orthographic mode yet");
                           status(camera_.ortho ? "Orthographic view" : "Perspective view");
                       },
                       {}});
    add("frame_selected", {"Frame Selected", ImGuiKey_F, 0, false,
                           [this] {
                               if (keys_hovered()) return (void)with_graph([&](GraphContext& g) { graph_.frame_selected(g); });
                               int p = primary();
                               if (p < 0) {
                                   camera_.target = {0, 0, 1.0};
                                   return;
                               }
                               Vec3 head = globals_[p].pos, tail = globals_[p].apply(skel_[p].end);
                               camera_.target = (head + tail) * 0.5;
                               if (skel_[p].category != Category::Body) camera_.distance = std::min(camera_.distance, 1.2);
                           },
                           {}});
    add("frame_all", {"Frame All", ImGuiKey_A, 0, false,
                      [this] {
                          if (keys_hovered()) return (void)with_graph([&](GraphContext& g) { graph_.frame_all(g); });
                          camera_.target = {0, 0, 1.0};
                          camera_.distance = Camera::kDefaultDistance;
                      },
                      {}});

    add("copy", {"Copy Pose", ctrl | ImGuiKey_C, 0, false,
                 [this] {
                     if (keys_hovered()) return with_graph([&](GraphContext& g) { graph_.copy_keys(g); });
                     if (prop_sl_copy()) return;
                     std::vector<std::string> tracks;
                     for (int s : selection_) tracks.push_back(skel_[s].name);
                     for (auto& h : handles_) tracks.push_back("ik." + rig_->limbs()[h.limb].name);
                     pose_clipboard_ = copy_pose(doc_.clip(), frame_, tracks);
                     status("Copied " + std::to_string(pose_clipboard_.entries.size()) + " item(s)" +
                            (tracks.empty() ? " (the whole pose)" : ""));
                 },
                 {}});
    add("paste", {"Paste Pose", ctrl | ImGuiKey_V, 0, false,
                  [this] {
                      if (keys_hovered()) return with_graph([&](GraphContext& g) { graph_.paste_keys(g); });
                      if (prop_sl_paste()) return;
                      std::vector<std::string> tracks;
                      for (int s : selection_) tracks.push_back(skel_[s].name);
                      for (auto& h : handles_) tracks.push_back("ik." + rig_->limbs()[h.limb].name);
                      Clip before = doc_.clip();
                      edit("Paste Pose", [&](Clip& c) { paste_pose(c, pose_clipboard_, frame_, tracks); });
                      offer_pose_blend(std::move(before), frame_);
                      status("Pasted the pose at frame " + std::to_string(int(frame_)));
                  },
                  [this]() -> const char* {
                      return keys_hovered() || selected_prop_ >= 0 || !pose_clipboard_.entries.empty() ? nullptr
                                                                                                           : "Copy a pose first";
                  }});
    add("ik_toggle", {"Switch IK / FK", ImGuiKey_K, 0, false,
                      [this] {
                          int l = limb_for_action();
                          const LimbInfo& limb = rig_->limbs()[l];
                          bool on = limb_states_[l].ik_on;
                          edit(on ? "Switch to FK" : "Switch to IK", [&](Clip& c) {
                              on ? switch_to_fk(c, *rig_, frame_, l, shape()) : switch_to_ik(c, *rig_, frame_, l, shape());
                          });
                          if (on) {
                              select(limb.end, false);
                          } else {
                              select_handle({l, false}, false);
                          }
                          status(limb.label + (on ? " is now FK" : " is now IK") + " from frame " + std::to_string(int(frame_)));
                      },
                      [this]() -> const char* { return limb_for_action() < 0 ? "Select a bone of an arm, leg, wing, finger or the spine" : nullptr; }});
    add("pin_world", {"Hold in World from Here", 0, 0, false,
                      [this] {
                          std::string why;
                          int p = primary();
                          bool ok = false;
                          edit("Hold in World", [&](Clip& c) { ok = pin_here(c, *rig_, frame_, p, -1, shape(), why); });
                          status(ok ? skel_[p].name + " is held in place from frame " + std::to_string(int(frame_)) : why);
                      },
                      need_selection});
    add("pin_bone", {"Bind to Selected Bone from Here", 0, 0, false,
                     [this] {
                         std::string why;
                         int p = selection_[1], target = selection_[0];
                         bool ok = false;
                         edit("Pin to Bone", [&](Clip& c) { ok = pin_here(c, *rig_, frame_, p, target, shape(), why); });
                         status(ok ? skel_[p].name + " now rides " + skel_[target].name : why);
                     },
                     [this]() -> const char* {
                         return selection_.size() == 2 ? nullptr : "Select the bone to ride, then Shift-click the point to pin";
                     }});
    add("unpin", {"Release from Here", 0, 0, false,
                  [this] {
                      std::string why;
                      int p = primary();
                      bool ok = false;
                      edit("Unpin", [&](Clip& c) { ok = unpin_here(c, *rig_, frame_, p, shape(), why); });
                      status(ok ? skel_[p].name + " follows its own bone again from frame " + std::to_string(int(frame_)) : why);
                  },
                  [this]() -> const char* {
                      int p = primary();
                      return p >= 0 && pin_at(doc_.clip(), *rig_, p, frame_) >= 0 ? nullptr : "The selected point is not pinned here";
                  }});
    add("delete_pin", {"Delete Pin", 0, 0, false,
                       [this] {
                           int i = pin_at(doc_.clip(), *rig_, primary(), frame_);
                           edit("Delete Pin", [&](Clip& c) { delete_pin(c, *rig_, size_t(i)); });
                           status("Pin deleted");
                       },
                       [this]() -> const char* {
                           int p = primary();
                           return p >= 0 && pin_at(doc_.clip(), *rig_, p, frame_) >= 0 ? nullptr : "The selected point is not pinned here";
                       }});
    add("hands", {"Hand Poser", ImGuiKey_H, 0, false,
                  [this] {
                      show_hands_ = !show_hands_;
                      if (show_hands_) status("Hands: drag a finger dot down to curl it, sideways to spread");
                  },
                  {}});
    add("graph", {"Graph Editor", ctrl | ImGuiKey_G, 0, false,
                  [this] {
                      show_graph_ = !show_graph_;
                      settings_.show_graph = show_graph_;
                      save_settings();
                  },
                  {}});
    add("dope_sheet", {"Dope Sheet", 0, 0, false, [this] { show_dope_ = !show_dope_; }, {}});  // spec 08 DS
    add("select_all", {"Select All", ctrl | ImGuiKey_A, 0, false,
                       [this] {
                           clear_selection();
                           for (int i = 0; i < skel_.size(); ++i)
                               if (node_visible(i)) selection_.push_back(i);
                           for (int l = 0; l < int(limb_states_.size()); ++l)  // and the handles of IK limbs (AM-132)
                               if (limb_states_[l].ik_on && node_visible(rig_->limbs()[l].end)) {
                                   handles_.push_back({l, false});
                                   if (!rig_->limbs()[l].spine) handles_.push_back({l, true});
                               }
                       },
                       {}});
    auto select_keyed = [this](bool on_frame) {
        std::vector<int> bones;
        std::vector<HandleRef> handles;
        auto keyed = [&](const std::string& t) {
            return on_frame ? has_key_at(doc_.clip(), t, frame_) : doc_.clip().curves.count(t) > 0;
        };
        for (int i = 0; i < skel_.size(); ++i)
            if (node_visible(i) && (keyed(skel_[i].name) || keyed("pin:" + skel_[i].name))) bones.push_back(i);
        for (int l = 0; l < int(rig_->limbs().size()); ++l)
            if (keyed("ik." + rig_->limbs()[l].name)) {
                handles.push_back({l, false});
                if (!rig_->limbs()[l].spine) handles.push_back({l, true});
            }
        if (bones.empty() && handles.empty())  // leave the selection alone (AM-132)
            return status(on_frame ? "Nothing is keyed on this frame" : "Nothing is keyed yet");
        clear_selection();
        selection_ = bones;
        handles_ = handles;
        status("Selected " + std::to_string(bones.size()) + " bones and " + std::to_string(handles.size()) + " IK handles");
    };
    add("select_keyed_frame", {"Select Keyed on Frame", ctrl | shift | ImGuiKey_A, 0, false, [=] { select_keyed(true); }, {}});
    add("select_all_keyed", {"Select All Keyed", 0, 0, false, [=] { select_keyed(false); }, {}});
    add("select_none", {"Select None", ImGuiKey_Escape, 0, false, [this] { clear_selection(); }, {}});
    add("select_parent", {"Select Parent", ImGuiKey_UpArrow, ImGuiKey_LeftBracket, true,
                          [this] {
                              int p = primary();
                              select(p < 0 || skel_[p].parent < 0 ? 0 : skel_[p].parent, false);
                          },
                          {}});
    add("select_child", {"Select Child", ImGuiKey_DownArrow, ImGuiKey_RightBracket, true,
                         [this] {
                             int p = primary();
                             if (p < 0) return select(0, false);
                             for (int c : skel_[p].children)
                                 if (node_visible(c)) return select(c, false);
                         },
                         {}});
    auto sibling = [this](int dir) {
        int p = primary();
        if (p < 0 || skel_[p].parent < 0) return;
        std::vector<int> sibs;
        for (int c : skel_[skel_[p].parent].children)
            if (node_visible(c)) sibs.push_back(c);
        auto it = std::find(sibs.begin(), sibs.end(), p);
        if (it == sibs.end()) return;
        int i = int(it - sibs.begin());
        select(sibs[(i + dir + int(sibs.size())) % int(sibs.size())], false);
    };
    add("next_sibling", {"Next Sibling", shift | ImGuiKey_RightBracket, 0, true, [=] { sibling(+1); }, {}});
    add("prev_sibling", {"Previous Sibling", shift | ImGuiKey_LeftBracket, 0, true, [=] { sibling(-1); }, {}});

    add("reset_camera", {"Reset Camera", 0, 0, false,
                         [this] {
                             const bool ortho = camera_.ortho;  // a view mode, not a camera place
                             camera_ = Camera();
                             camera_.ortho = ortho;
                             status("Camera reset");
                         },
                         {}});
    add("snap_toggle", {"Toggle Snapping", 0, 0, false,
                        [this] {
                            snap_on_ = !snap_on_;
                            status(snap_on_ ? "Snapping on" : "Snapping off");
                        },
                        {}});
    add("save_clip", {"Save Clip of Selected Bones...", 0, 0, false,
                      [this] {
                          name_prompt_ = "Clip";
                          name_action_ = NameAction::SaveClip;
                      },
                      [this]() -> const char* {
                          double a, b;
                          if (selection_.empty()) return "Select the bones to save first";
                          return clip_range(a, b) ? nullptr : "Shift-drag a frame range on the timeline, or select keys in the graph";
                      }});
    // RT-9 and 08 FC on the current clip (footlock_ui.cpp).
    add("foot_lock", {"Clean Up Foot Sliding...", 0, 0, false, [this] { show_foot_lock_ = true; }, {}});
    add("follow_target", {"Follow Target (Bake)...", 0, 0, false, [this] { show_follow_ = true; },
                          [this]() -> const char* {
                              return selection_.size() == 2 ? nullptr
                                                            : "Select the bone to follow, then Shift-click the bone or point that follows it";
                          }});
    add("about", {"About Viewport Avatar Toolset", 0, 0, false, [this] { show_about_ = true; }, {}});
    add("shortcuts", {"Keyboard Shortcuts...", 0, 0, false, [this] { show_shortcuts_ = !show_shortcuts_; }, {}});
    add("prefs", {"Preferences...", ctrl | ImGuiKey_Comma, 0, false, [this] { show_prefs_ = !show_prefs_; }, {}});
    add("help_contents", {"Help Contents", ImGuiKey_F1, 0, false, [this] { open_help(); }, {}});
    add("tutorials", {"Tutorials", 0, 0, false, [this] { open_help("tutorials"); }, {}});
    add("help", {"Controls", 0, 0, false, [this] { show_help_ = !show_help_; }, {}});
    add("welcome", {"Welcome", 0, 0, false, [this] { show_welcome_ = true; }, {}});
    add("zoom_in", {"Zoom In", 0, 0, true, [this] { camera_.zoom(0.85); }, {}});
    add("zoom_out", {"Zoom Out", 0, 0, true, [this] { camera_.zoom(1 / 0.85); }, {}});
    static const char* view_ids[4] = {"cam_1", "cam_2", "cam_3", "cam_4"};
    static const char* store_ids[4] = {"store_cam_1", "store_cam_2", "store_cam_3", "store_cam_4"};
    static const char* view_labels[4] = {"Camera View 1", "Camera View 2", "Camera View 3", "Camera View 4"};
    static const char* store_labels[4] = {"Store Camera View 1", "Store Camera View 2", "Store Camera View 3",
                                          "Store Camera View 4"};
    for (int i = 0; i < 4; ++i) {
        // Per project, falling back to the global slots (decision 4 in docs/spec/README.md).
        add(view_ids[i], {view_labels[i], 0, 0, false,
                          [this, i] {
                              const CameraView* v = project_camera(i);
                              if (!v) v = settings_.cameras[i].set ? &settings_.cameras[i] : nullptr;
                              if (!v) return status("Camera view " + std::to_string(i + 1) + " is empty: store it first");
                              camera_.target = v->target, camera_.yaw = v->yaw, camera_.pitch = v->pitch,
                              camera_.distance = v->distance;
                          },
                          {}});
        add(store_ids[i], {store_labels[i], 0, 0, false,
                           [this, i] {
                               CameraView v{true, camera_.target, camera_.yaw, camera_.pitch, camera_.distance};
                               settings_.cameras[i] = v;
                               save_settings();
                               store_project_camera(i, v);
                               status("Stored camera view " + std::to_string(i + 1));
                           },
                           {}});
    }

    for (auto& [id, a] : actions_) industry_keys_[id] = {a.key, a.key2};
    apply_preset();
}

// Bindings that differ from Industry (Maya-style), spec 04 section 3.2 and 05 section 3.1.
void App::apply_preset() {
    const ImGuiKeyChord ctrl = ImGuiMod_Ctrl, shift = ImGuiMod_Shift, alt = ImGuiMod_Alt;
    using Keys = std::pair<ImGuiKeyChord, ImGuiKeyChord>;
    static const std::map<std::string, Keys> blender = {
        {"redo", {ctrl | shift | ImGuiKey_Z, ctrl | ImGuiKey_Y}},
        {"key", {ImGuiKey_I, 0}},
        {"key_all", {shift | ImGuiKey_I, 0}},
        {"delete_key", {alt | ImGuiKey_I, ImGuiKey_Delete}},
        {"reset_hip", {alt | ImGuiKey_G, alt | ImGuiKey_H}},
        {"flip_pose", {ctrl | shift | ImGuiKey_V, 0}},
        {"next_key", {ImGuiKey_UpArrow, 0}},
        {"prev_key", {ImGuiKey_DownArrow, 0}},
        {"start", {shift | ImGuiKey_LeftArrow, 0}},
        {"end", {shift | ImGuiKey_RightArrow, 0}},
        {"tool_select", {ImGuiKey_W, 0}},
        {"tool_move", {ImGuiKey_G, 0}},
        {"tool_rotate", {ImGuiKey_R, 0}},
        {"tool_scale", {ImGuiKey_S, 0}},
        {"orientation", {ImGuiKey_Comma, 0}},
        {"prev_frame", {ImGuiKey_LeftArrow, 0}},
        {"next_frame", {ImGuiKey_RightArrow, 0}},
        {"play", {ImGuiKey_Space, 0}},
        {"view_front", {ImGuiKey_Keypad1, ImGuiKey_1}},
        {"view_back", {ctrl | ImGuiKey_Keypad1, ctrl | ImGuiKey_1}},
        {"view_right", {ImGuiKey_Keypad3, ImGuiKey_3}},
        {"view_left", {ctrl | ImGuiKey_Keypad3, ctrl | ImGuiKey_3}},
        {"view_top", {ImGuiKey_Keypad7, ImGuiKey_7}},
        {"frame_selected", {ImGuiKey_KeypadDecimal, 0}},
        {"frame_all", {ImGuiKey_Home, 0}},
        {"select_all", {ImGuiKey_A, 0}},
        {"select_parent", {ImGuiKey_LeftBracket, 0}},
        {"select_child", {ImGuiKey_RightBracket, 0}},
        {"import_prop", {ctrl | alt | ImGuiKey_I, 0}},
    };
    static const std::map<std::string, Keys> qavimator = {
        {"save_as", {ctrl | ImGuiKey_A, 0}},
        {"select_all", {shift | ImGuiKey_A, 0}},  // Ctrl+A is Save As there
        {"redo", {ctrl | shift | ImGuiKey_Z, ctrl | ImGuiKey_Y}},
        {"frame_all", {ctrl | ImGuiKey_0, ImGuiKey_A}},
        {"zoom_in", {ImGuiKey_PageUp, 0}},
        {"zoom_out", {ImGuiKey_PageDown, 0}},
        {"cam_1", {ImGuiKey_F9, 0}},
        {"cam_2", {ImGuiKey_F10, 0}},
        {"cam_3", {ImGuiKey_F11, 0}},
        {"cam_4", {ImGuiKey_F12, 0}},
        {"store_cam_1", {shift | ImGuiKey_F9, 0}},
        {"store_cam_2", {shift | ImGuiKey_F10, 0}},
        {"store_cam_3", {shift | ImGuiKey_F11, 0}},
        {"store_cam_4", {shift | ImGuiKey_F12, 0}},
    };
    // Second Life: its build-tool habits. Esc resets the camera, G toggles snapping; the Move gizmo
    // is the default and Ctrl / Ctrl+Shift switch to Rotate / Scale while held (see viewport.cpp).
    // The tool keys (Q W E R) and Frame All (A) stay as in Industry: nothing else here uses them.
    static const std::map<std::string, Keys> second_life = {
        {"select_none", {0, 0}},
        {"reset_camera", {ImGuiKey_Escape, 0}},
        {"snap_toggle", {ImGuiKey_G, 0}},
        {"redo", {ctrl | ImGuiKey_Y, ctrl | shift | ImGuiKey_Z}},
    };
    // Industry's second bindings that the table leaves out (spec 3.2): Alt+V play, Alt+. / Alt+, frames.
    static const std::map<std::string, Keys> industry_extra = {
        {"play", {ImGuiKey_Space, alt | ImGuiKey_V}},
        {"next_frame", {ImGuiKey_RightArrow, alt | ImGuiKey_Period}},
        {"prev_frame", {ImGuiKey_LeftArrow, alt | ImGuiKey_Comma}},
    };
    for (auto& [id, a] : actions_) {
        Keys k = industry_keys_[id];
        if (settings_.preset != Preset::Blender)
            if (auto it = industry_extra.find(id); it != industry_extra.end()) k = it->second;
        const auto* table = settings_.preset == Preset::Blender      ? &blender
                            : settings_.preset == Preset::QAvimator  ? &qavimator
                            : settings_.preset == Preset::SecondLife ? &second_life
                                                                     : nullptr;
        if (table)
            if (auto it = table->find(id); it != table->end()) k = it->second;
        preset_keys_[id] = {k.first, k.second};
        const KeyPair keys = effective_keys(settings_.key_overrides, id, preset_keys_[id]);  // Edit > Keyboard Shortcuts
        a.key = keys[0];
        a.key2 = keys[1];
    }
}

std::string App::key_hint(const char* id) const {
    for (auto& [aid, a] : actions_)
        if (std::string_view(aid) == id) return a.key ? key_label(a.key) : "";
    return "";
}

std::string App::nav_hint() const {
    if (host_.world_view())
        return "Clicks and keys: the editor   Camera: Alt+drag, wheel, Alt+arrows   Chat: click the Chat pane";
    switch (settings_.preset) {
        case Preset::Blender:
            return settings_.emulate_3_button ? "Middle or Alt+left: orbit   Shift: pan   Ctrl: zoom   Wheel: zoom"
                                              : "Middle drag: orbit   Shift+middle: pan   Ctrl+middle: zoom   Wheel: zoom";
        case Preset::QAvimator:
            return "Drag empty space: orbit   Shift: pan   Alt: zoom   Shift/Ctrl/Alt+drag a bone: rotate it";
        case Preset::SecondLife:
            return "Alt+click: focus, drag to orbit/zoom   Ctrl+Alt: orbit   Ctrl+Alt+Shift: pan   Hold Ctrl: rotate   G: snap";
        default: return "Alt + drag: left orbit, middle pan, right zoom    Wheel: zoom";
    }
}

// The graph editor's drag navigation for the active preset (05 TG-67).
std::string App::graph_nav_hint() const {
    switch (settings_.preset) {
        case Preset::Blender:
            return settings_.emulate_3_button ? "Graph: middle or Alt+left drag pans   Ctrl+middle: zoom   Wheel: zoom"
                                              : "Graph: middle drag pans   Ctrl+middle: zoom   Wheel: zoom";
        case Preset::QAvimator: return "Graph: middle drag pans   Wheel: zoom";
        case Preset::SecondLife: return "Graph: Ctrl+Alt+drag or middle drag pans   Alt+drag: zoom   Wheel: zoom";
        default: return "Graph: Alt+left or Alt+middle drag pans   Alt+right: zoom   Middle drag moves keys   Wheel: zoom";
    }
}

void App::handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (skip_shortcuts_) {
        skip_shortcuts_ = false;
        return;
    }
    if (modal_ != Modal::None) return;  // the modal transform handles every key
    if (deferred_action_ && (!doc_.history.is_open() || --deferred_frames_ <= 0)) {
        run_action(std::exchange(deferred_action_, nullptr));
        return;
    }
    // Keys go to text fields first, except Ctrl shortcuts (spec UI-15/UI-16).
    for (auto& [id, a] : actions_) {
        for (ImGuiKeyChord k : {a.key, a.key2}) {
            if (!k) continue;
            static const std::string_view global[] = {"new", "open", "save", "save_as", "export_anim", "quit",
                                                       "undo", "redo", "prefs", "graph"};
            bool is_global = (k & ImGuiMod_Ctrl) && std::find(std::begin(global), std::end(global), id) != std::end(global);
            if (io.WantTextInput && !is_global) continue;  // text fields keep every other key (UI-15/16)
            // A text field locks keys like Ctrl+Z for itself, so global actions read the raw key state (UI-15).
            const bool pressed =
                io.WantTextInput && is_global
                    ? io.KeyMods == (k & ImGuiMod_Mask_) && ImGui::GetKeyData(ImGuiKey(k & ~ImGuiMod_Mask_))->DownDuration == 0.f
                    : ImGui::IsKeyChordPressed(k, a.repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None);
            if (pressed) {
                if (io.WantTextInput && (id == "undo" || id == "redo")) {
                    ImGui::ClearActiveID();  // the field lets go and commits; the undo runs once it has (UI-15)
                    deferred_action_ = id.c_str(), deferred_frames_ = 5;
                    return;
                }
                const char* why = a.unavailable ? a.unavailable() : nullptr;
                if (why)
                    status(why);
                else
                    a.run();
                return;
            }
        }
    }
}

void App::menu_item(const char* id) {
    for (auto& [aid, a] : actions_) {
        if (std::string_view(aid) != id) continue;
        std::string shortcut = a.key ? key_label(a.key) : "";
        const char* why = a.unavailable ? a.unavailable() : nullptr;
        if (menu_item_icon(action_icon(id), a.label, shortcut.c_str(), false, why == nullptr)) a.run();
        if (why) ImGui::SetItemTooltip("%s", why);
        return;
    }
}

void App::draw_menus() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (begin_menu_icon(nullptr, "File")) {
        for (const char* id : {"new", "open"}) menu_item(id);
        if (ImGui::BeginMenu("Open Recent")) {
            bool any = false;
            for (size_t i = 0; i < settings_.recent.size(); ++i) {
                const std::string path = settings_.recent[i];
                if (std::error_code ec; !std::filesystem::exists(u8path(path), ec)) continue;  // hide files that have gone
                any = true;
                std::string label = std::to_string(i + 1) + "  " + file_name(path);
                if (ImGui::MenuItem(label.c_str())) guard_unsaved([this, path] { load_project_file(path); });
                ImGui::SetItemTooltip("%s", path.c_str());
            }
            if (!any) ImGui::MenuItem("(no recent projects)", nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent", nullptr, false, !settings_.recent.empty())) {
                settings_.recent.clear();
                save_settings();
            }
            ImGui::EndMenu();
        }
        for (const char* id : {"save", "save_as", "save_to_library"}) menu_item(id);
        ImGui::Separator();
        menu_item("import_bvh");
        menu_item("import_anim");
        menu_item("import_retarget");
        menu_item("batch_retarget");
        menu_item("import_prop");
        menu_item("load_audio");
        ImGui::Separator();
        for (const char* id : {"export_anim", "export_bvh", "export_bvh_all", "export_all_clips"}) menu_item(id);
        if (!host_.world_view()) {  // 08 LM, the app only: the viewer renders no thumbnails
            if (menu_item_icon(icon::kListing, "Export Listing Media...")) show_listing_ = true;
            ImGui::SetItemTooltip("The animation as an animated GIF or numbered PNG pictures, for a Marketplace listing");
        }
        if (host_.can_upload()) menu_item("upload"), menu_item("upload_all_clips");
        ImGui::Separator();
        menu_item("quit");
        ImGui::EndMenu();
    }
    if (begin_menu_icon(nullptr, "Edit")) {
        menu_item("undo");
        menu_item("redo");
        ImGui::Separator();
        for (const char* id : {"key", "key_all", "tween", "delete_key", "delete_frame"}) menu_item(id);
        ImGui::Separator();
        draw_key_tag_menu_items();  // 08 KT
        ImGui::Separator();
        for (const char* id : {"reset_bone", "reset_hip", "reset_pose"}) menu_item(id);
        ImGui::Separator();
        for (const char* id : {"copy", "paste", "save_clip"}) menu_item(id);
        if (ImGui::BeginMenu("Time")) {
            draw_time_menu_items();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (const char* id : {"mirror_l2r", "mirror_r2l", "flip_pose", "mirror_bone"}) menu_item(id);
        ImGui::Separator();
        draw_pose_tool_menu_items();  // Scratch Pose, Propagate Pose (PT-2, PT-3)
        ImGui::Separator();
        menu_item("reverse");
        if (menu_item_icon(icon::kSimplify, "Simplify Curves...", nullptr, false, !doc_.history.is_open())) open_simplify();
        ImGui::SetItemTooltip("Fewer keys on the selected bones' curves (or all), within a tolerance");
        ImGui::Separator();
        menu_item("shortcuts");
        menu_item("prefs");
        ImGui::EndMenu();
    }
    if (begin_menu_icon(nullptr, "Playback")) {
        for (const char* id : {"play", "next_frame", "prev_frame", "next_key", "prev_key", "start", "end"}) menu_item(id);
        ImGui::EndMenu();
    }
    if (begin_menu_icon(nullptr, "View")) {  // order of spec 06 section 3.4; Camera Views, zoom and reset are additions
        for (const char* id : {"view_front", "view_back", "view_right", "view_left", "view_top"}) menu_item(id);
        if (menu_item_icon(icon::kOrtho, "Orthographic", key_hint("view_ortho").c_str(), camera_.ortho)) run_action("view_ortho");
        ImGui::Separator();
        for (const char* id : {"frame_selected", "frame_all", "zoom_in", "zoom_out", "reset_camera"}) menu_item(id);
        if (ImGui::BeginMenu("Camera Views")) {
            for (const char* id : {"cam_1", "cam_2", "cam_3", "cam_4"}) menu_item(id);
            ImGui::Separator();
            for (const char* id : {"store_cam_1", "store_cam_2", "store_cam_3", "store_cam_4"}) menu_item(id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menu_item("graph");
        menu_item("dope_sheet");
        ImGui::Separator();
        static const char* cats[] = {"Show Body Bones", "Show Hand Bones",  "Show Face Bones",  "Show Wing Bones",
                                     "Show Tail Bones", "Show Hind Limb Bones", "Show Groin Bones", "Show Attachment Points"};
        for (int c = 0; c < 8; ++c) ImGui::MenuItem(cats[c], nullptr, &show_category_[c]);
        ImGui::Separator();
        ImGui::MenuItem("Show Collision Volumes", nullptr, &show_volumes_);
        ImGui::MenuItem("Centre of Mass", nullptr, &show_com_);  // 08 CM-1
        ImGui::SetItemTooltip("The body's centre of mass over the planted feet; red when it falls outside them");
        if (ImGui::BeginMenu("Onion Skin")) {
            draw_onion_settings();
            draw_pinned_ghost_menu();  // 08 ON-5
            ImGui::EndMenu();
        }
        if (begin_menu_icon(icon::kMotionPath, "Motion Path")) {  // 08 MP
            draw_motion_path_menu();
            ImGui::EndMenu();
        }
        if (menu_item_icon(icon::kReference, "Reference...", nullptr, show_reference_)) show_reference_ = !show_reference_;  // 08 RF
        ImGui::SetItemTooltip("A picture or picture sequence behind the avatar, to pose or animate over");
        if (menu_item_icon(icon::kSlPreview, "Preview as SL Plays It", nullptr, sl_preview_)) sl_preview_ = !sl_preview_;
        ImGui::SetItemTooltip("Plays the exported .anim as Second Life will, with your animation as a ghost");
        if (host_.world_view() && menu_item_icon(icon::kInWorld, "As It Plays In-World", nullptr, in_world_)) set_in_world(!in_world_);
        if (host_.world_view())
            ImGui::SetItemTooltip("Your AO, the default motions and avatar physics play; your animation at its own priorities "
                                  "on the joints it keys, with a table of who wins each joint");
        if (menu_item_icon(icon::kFaceCam, "Face Cam", nullptr, face_cam_)) face_cam_ = !face_cam_;
        ImGui::SetItemTooltip("A cutout of a face driven by your face tracking, to move and resize anywhere; your avatar "
                              "is not animated by the tracking meanwhile");
        if (begin_menu_icon(icon::kTreadmill, "Treadmill")) {  // 08 LP-8
            draw_treadmill_menu();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Body")) {
            // SL defaults first: they are what people see in-world.
            const bool linden = !mesh_body();
            for (Body b : {Body::SLDefault, Body::SLDefaultMale, Body::Female, Body::Male, Body::SkeletonOnly})
                if (ImGui::MenuItem(kBodyNames[int(b)], nullptr, linden && body_ == b)) {
                    set_body(int(b));
                    if (!linden) use_mesh_body("");
                }
            if (!bodies_.empty()) {
                ImGui::SeparatorText("Mesh bodies");
                for (int k = 0; k < int(bodies_.size()); ++k) {  // two bodies may share a name
                    const MeshBody& mb = bodies_[k];
                    ImGui::PushID(k);
                    if (ImGui::MenuItem(mb.name.c_str(), nullptr, settings_.mesh_body == mb.id)) use_mesh_body(mb.id);
                    ImGui::PopID();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Bones in Front (X-ray)", nullptr, &xray_);
        if (host_.world_view() && ImGui::MenuItem("Show Other Avatars", nullptr, settings_.viewer_show_others)) {
            settings_.viewer_show_others = !settings_.viewer_show_others;  // the viewer's (spec 09 U5), saved
            save_settings();
        }
        ImGui::EndMenu();
    }
    draw_light_menu();
    if (begin_menu_icon(nullptr, "Select")) {
        for (const char* id : {"select_all", "select_keyed_frame", "select_all_keyed", "select_none"}) menu_item(id);
        ImGui::Separator();
        for (const char* id : {"select_parent", "select_child", "next_sibling", "prev_sibling"}) menu_item(id);
        ImGui::EndMenu();
    }
    if (begin_menu_icon(nullptr, "Tools")) {
        for (const char* id : {"tool_select", "tool_move", "tool_rotate", "tool_scale"}) menu_item(id);
        ImGui::Separator();
        menu_item("orientation");
        ImGui::Separator();
        for (const char* id : {"ik_toggle", "follow_target", "pin_world", "pin_bone", "unpin", "delete_pin", "foot_lock"}) menu_item(id);
        if (begin_menu_icon(icon::kLoop, "Loop Tools")) {
            draw_loop_tools_menu();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menu_item("hands");
        if (menu_item_icon(icon::kDynamics, "Dynamics...", nullptr, show_dynamics_)) show_dynamics_ = !show_dynamics_;
        if (menu_item_icon(icon::kIdle, "Idle Layer...", nullptr, show_idle_)) show_idle_ = !show_idle_;
        if (menu_item_icon(icon::kOverlap, "Overlap...", nullptr, show_overlap_)) show_overlap_ = !show_overlap_;
        if (menu_item_icon(icon::kBalance, "Auto-Balance...", nullptr, show_auto_balance_)) show_auto_balance_ = !show_auto_balance_;  // 08 CM-2
        if (menu_item_icon(icon::kJumpArc, "Jump Arc...", nullptr, show_jump_arc_)) show_jump_arc_ = !show_jump_arc_; // 08 JA-1
        if (menu_item_icon(icon::kTransition, "Make Transition...", nullptr, show_transition_))  // 08 PM-3
            show_transition_ = !show_transition_, transition_.to = int(std::round(frame_));
        if (menu_item_icon(icon::kRagdoll, "Ragdoll...", nullptr, show_ragdoll_)) show_ragdoll_ = !show_ragdoll_;
        if (menu_item_icon(icon::kFace, "Face...", nullptr, show_face_)) show_face_ = !show_face_;
        if (menu_item_icon(icon::kActors, "Actors (Couples and Groups)...", nullptr, show_actors_)) show_actors_ = !show_actors_;
        if (menu_item_icon(icon::kClips, "Clips (AO Sets)...", nullptr, show_clips_)) show_clips_ = !show_clips_;
        if (menu_item_icon(icon::kRecord, "Motion Capture...", nullptr, show_mocap_)) show_mocap_ = !show_mocap_;
        if (menu_item_icon(icon::kSplit, "Split Dance at Beats...", nullptr, show_split_dance_))  // 08 TE-6
            show_split_dance_ = !show_split_dance_;
        ImGui::Separator();
        if (menu_item_icon(icon::kCheck, "Animation Check...", nullptr, show_check_)) show_check_ = !show_check_;
        if (menu_item_icon(icon::kQuality, "Motion Quality...", nullptr, show_quality_)) show_quality_ = !show_quality_;
        if (menu_item_icon(icon::kPlanner, "Priority Planner...", nullptr, show_planner_)) show_planner_ = !show_planner_;
        ImGui::EndMenu();
    }
    if (ui::Host::HostUi* h = host_.host_ui()) {  // the viewer's own UI beside the editor (spec 09 U4b)
        const int n = h->unread_notices();
        const std::string count = n > 0 ? " (" + std::to_string(n) + ")" : "";
        if (begin_menu_icon(nullptr, ("Viewer" + count + "###host_menu").c_str())) {
            ImGui::MenuItem(h->pane_title(), nullptr, &show_host_pane_);
            if (ImGui::MenuItem(("Notifications" + count).c_str())) h->toggle_notices();
            if (ImGui::MenuItem(h->reveal_label(), h->reveal_shortcut(), h->revealed())) h->reveal(!h->revealed());
            ImGui::Separator();
            menu_item("quit");
            ImGui::EndMenu();
        }
    }
    if (begin_menu_icon(nullptr, "Help")) {
        for (const char* id : {"help_contents", "tutorials", "help", "welcome", "about"}) menu_item(id);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------------------------
// Frame

void App::set_frame(double f) { frame_ = std::clamp(f, 0.0, double(doc_.clip().end_frame)); }

std::vector<std::string> App::selected_tracks() const {
    std::vector<std::string> out;
    for (int s : selection_) {
        out.push_back(skel_[s].name);
        std::string pin = "pin:" + skel_[s].name;  // AM-31, AM-88: a pinned point's offset track
        if (doc_.clip().curves.count(pin) || pin_at(doc_.clip(), *rig_, s, frame_) >= 0) out.push_back(pin);
    }
    for (auto& h : handles_) {
        std::string t = "ik." + rig_->limbs()[h.limb].name;
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

void App::step_key(int dir) {
    std::vector<double> frames;
    auto collect = [&](const std::string& track) {
        for (double k : key_frames(doc_.clip(), track)) frames.push_back(std::round(k));  // fractional keys (pitfall 2)
    };
    if (selection_.empty() && handles_.empty())
        for (auto& [name, t] : doc_.clip().curves) collect(name);
    else
        for (auto& t : selected_tracks()) collect(t);
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    if (dir > 0) {
        auto it = std::upper_bound(frames.begin(), frames.end(), frame_ + 1e-6);
        if (it != frames.end()) set_frame(*it);
    } else {
        auto it = std::lower_bound(frames.begin(), frames.end(), frame_ - 1e-6);
        if (it != frames.begin()) set_frame(*(it - 1));
    }
}

void App::select_handle(HandleRef h, bool toggle) {
    auto it = std::find(handles_.begin(), handles_.end(), h);
    if (toggle) {
        if (it != handles_.end()) handles_.erase(it);
        else handles_.push_back(h);
    } else {
        selection_.clear();
        handles_.assign(1, h);
    }
    handle_primary_ = !handles_.empty();
    const LimbInfo& l = rig_->limbs()[h.limb];
    status(l.label + (h.pole ? " IK pole" : " IK target"));
}

int App::limb_for_action() const {
    if (auto* h = primary_handle()) return h->limb;
    int p = primary();
    return p < 0 ? -1 : rig_->limb_of_bone(p);
}

void App::select(int node, bool toggle) {
    handle_primary_ = false;
    selected_prop_ = -1;  // bones and a prop are never selected together (VP-27)
    auto it = std::find(selection_.begin(), selection_.end(), node);
    if (toggle) {
        if (it != selection_.end())
            selection_.erase(it);
        else
            selection_.push_back(node);
    } else {
        selection_.assign(1, node);
        handles_.clear();
    }
}

void App::message(const std::string& title, const std::string& text) {
    if (headless_) {  // scripted runs: no popups over the screenshot, the text goes to stderr
        std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
        return;
    }
    message_title_ = title;
    message_text_ = text;
}

void App::draw_message_popup(bool in_export_dialog) {
    // Opened beside the Export dialog (a modal) each popup would close the other, so while it is open the message
    // opens inside it.
    if (show_export_dialog_ != in_export_dialog) return;
    if (!message_title_.empty() && !ImGui::IsPopupOpen("##message")) ImGui::OpenPopup("##message");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(640, 600));
    if (ImGui::BeginPopupModal("##message", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(message_title_.c_str());
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36);
        ImGui::TextUnformatted(message_text_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(96, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            message_title_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::draw_dockspace() {
    // In the viewer the centre stays empty and lets the pointer through to the world (spec 09 U3).
    const bool world = host_.world_view();
    ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), world ? ImGuiDockNodeFlags_PassthruCentralNode : 0);
    dockspace_id_ = dock;
    if (first_frame_) {
        first_frame_ = false;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        bool known = ImGui::FindWindowSettingsByID(ImHashStr("Graph")) && ImGui::FindWindowSettingsByID(ImHashStr("Inventory"));
        if (!node || node->IsEmpty() || !known) {
            ImGui::DockBuilderRemoveNode(dock);
            ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
            // Default proportions, widened to what the text needs at larger interface sizes.
            const ImVec2 ws = ImGui::GetMainViewport()->WorkSize;
            const float fs = ImGui::GetFontSize(), row = ImGui::GetFrameHeightWithSpacing();
            const float timeline_h = std::max(0.36f * 0.38f * ws.y, 3.9f * row);
            const float bottom_h = std::min(std::max(0.38f * ws.y, timeline_h + 7 * row), 0.55f * ws.y);
            const float left_w = std::min(std::max(0.18f * ws.x, 17 * fs), 0.3f * ws.x);
            const float right_w = std::min(std::max(0.24f * ws.x, 22 * fs), 0.32f * ws.x);
            ImGuiID main = dock, left, right, bottom, timeline;
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, bottom_h / ws.y, &bottom, &main);
            ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Down, std::min(timeline_h / bottom_h, 0.6f), &timeline, &bottom);
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, left_w / ws.x, &left, &main);
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, right_w / (ws.x - left_w), &right, &main);
            ImGui::DockBuilderDockWindow("Bones", left);
            ImGui::DockBuilderDockWindow("Picker", left);
            ImGui::DockBuilderDockWindow("Inventory", left);
            ImGui::DockBuilderDockWindow("Properties", right);
            ImGui::DockBuilderDockWindow("Graph", bottom);
            ImGui::DockBuilderDockWindow("Dope Sheet", bottom);  // spec 08 DS: a tab beside the graph
            if (ui::Host::HostUi* h = host_.host_ui()) ImGui::DockBuilderDockWindow(h->pane_title(), bottom);
            ImGui::DockBuilderDockWindow("Timeline", timeline);
            if (!world) ImGui::DockBuilderDockWindow("Viewport", main);
            ImGui::DockBuilderFinish(dock);
        }
    }
}

bool App::frame() {
    {
        // The viewer's skin (Host::skin_colours): restyle when it changes.
        HostColours now{};
        const bool has = host_.skin_colours(now);
        if (has != has_host_colours_ || (has && std::memcmp(&now, &host_colours_, sizeof now) != 0)) apply_look();
    }
    {
        std::vector<std::pair<Dialog, std::string>> results;
        {
            std::lock_guard<std::mutex> lock(dialog_mutex_);
            results.swap(dialog_results_);
        }
        for (auto& [kind, path] : results) {
            if (path.empty()) {  // cancelled: an action waiting for Save As is dropped with it
                if (kind == Dialog::SaveAs) after_save_as_ = nullptr;
                continue;
            }
            switch (kind) {
                case Dialog::Open: guarded(path, [&] { load_project_file(path); }); break;
                case Dialog::SaveAs:
                    if (auto after = std::exchange(after_save_as_, nullptr); save(with_extension(path, "vat")) && after) after();
                    break;
                case Dialog::ImportAnim:
                case Dialog::ImportBvh: guarded(path, [&] { import_file(path); }); break;
                case Dialog::ImportProp: guarded(path, [&] { import_prop(path); }); break;
                case Dialog::ImportRetarget: guarded(path, [&] { open_retarget(path); }); break;
                case Dialog::LoadAudio: guarded(path, [&] { load_audio(path); }); break;
                case Dialog::ImportBody: {
                    std::vector<std::string> parts;
                    std::stringstream ss(path);
                    for (std::string line; std::getline(ss, line);) parts.push_back(line);
                    guarded(parts.empty() ? path : parts[0], [&] { import_body(parts); });
                    break;
                }
                case Dialog::LoadActor: guarded(path, [&] { load_actor_file(file_actor_, path); }); break;
                case Dialog::SaveActor: save_actor(file_actor_, with_extension(path, "vat"), false); break;
                case Dialog::ExportActor: save_actor(file_actor_, with_extension(path, "anim"), true); break;
                case Dialog::SitLines: save_sit_lines(with_extension(path, "txt")); break;
                case Dialog::AoNotecard: save_ao_notecard(with_extension(path, "txt")); break;
                case Dialog::ExpressionPack: export_expression_pack(path); break;
                case Dialog::Rhubarb: guarded(path, [&] { import_rhubarb(path); }); break;
                case Dialog::LoadReference: guarded(path, [&] { load_reference(path, reference_pick_sequence_); }); break;
                case Dialog::ListingMedia: export_listing_media(with_extension(path, listing_png_ ? "png" : "gif")); break;
                case Dialog::ExportFolder:
                    doc_.clip().export_settings.set("folder", path);
                    mark_dirty();
                    if (int after = std::exchange(export_after_folder_, -1); after >= 0)
                        export_now(after > 0, after == 2, std::exchange(export_all_after_folder_, false));
                    break;
            }
        }
    }

    autosave_tick();
    const std::uint64_t now = host_.ticks_ns();
    if (playing_) {
        const Clip& c = doc_.clip();
        double lo = c.loop ? c.loop_in : 0, hi = c.loop ? c.loop_out : c.end_frame;
        if (hi <= lo) hi = lo + 1;
        frame_ += double(now - last_tick_) * 1e-9 * c.fps;
        if (frame_ > hi) frame_ = lo + std::fmod(frame_ - lo, hi - lo);  // from before loop_in it just runs on (TG-13)
    }
    update_audio();  // follows the playhead: plays, loops, scrub snippets (AU-3)
    update_camera_animation(double(now - last_tick_) * 1e-9);
    last_tick_ = now;

    scratch_tick();  // PT-2: before the pose is evaluated, so a scrub away from a scratch pose waits on the question
    loop_assist_tick();  // 08 LP-7, LP-8
    viewer_tools_tick();  // spec 09 build 20: the in-world claims, the walk test, the furniture click
    sl_export_tick();  // the shared in-memory export, refreshed when idle (08 SP, UM)
    { VATS_PROFILE("evaluate"); evaluate(); }
    { VATS_PROFILE("dock+menus"); draw_dockspace(); draw_menus(); }
    { VATS_PROFILE("panel bones"); draw_bones_panel(); }
    draw_picker_panel();  // 08 PK
    { VATS_PROFILE("panel inventory"); draw_inventory_panel(); }
    { VATS_PROFILE("panel properties"); draw_properties_panel(); }
    draw_dynamics_panel();
    draw_idle_panel();
    draw_overlap_panel();
    draw_auto_balance_panel();
    draw_jump_arc_panel();
    draw_match_poses_window();  // 08 PM
    draw_transition_window();
    draw_ragdoll_panel();
    draw_face_panel();
    draw_loop_assist_window();
    draw_foot_lock_window();
    draw_actors_panel();
    draw_clips_panel();
    draw_mocap_panel();  // every frame: it polls the socket while listening
    draw_check_panel();  // every frame: it re-checks after edits
    draw_quality_panel();
    draw_planner_panel();
    draw_in_world_window();  // spec 09 build 20
    draw_walk_test_window();
    draw_face_cam();
    draw_sl_preview_window();
    draw_split_dance_window();  // 08 TE-6
    draw_reference_window();
    draw_listing_window();
    { VATS_PROFILE("panel timeline"); draw_timeline_panel(); }
    if (!pending_tab_.empty() && ImGui::GetFrameCount() > 2)  // once the dock layout exists
        ImGui::SetWindowFocus(std::exchange(pending_tab_, "").c_str());
    { VATS_PROFILE("panel graph"); draw_graph_panel(); }
    draw_dope_panel();
    { VATS_PROFILE("viewport"); draw_viewport(); }
    draw_host_pane();
    draw_hand_poser();
    { VATS_PROFILE("status bar"); draw_status_bar(); }
    draw_message_popup();
    draw_name_prompt();
    draw_file_prompt();
    draw_time_prompt();
    draw_preferences();
    draw_export_dialog();
    draw_follow_dialog();
    draw_about();
    draw_bvh_prompt();
    draw_scratch_prompt();
    draw_simplify_dialog();
    draw_body_prompt();
#ifdef VATS_LEGACY_IMPORT
    draw_migration_prompt();
#endif
    draw_retarget_dialog();
    draw_batch_retarget();
    draw_controls_help();
    draw_shortcuts();
    draw_welcome();
    draw_help_browser();
    draw_recovery();
    handle_shortcuts();
    return !quit_;
}

void App::evaluate() {
    if (multi_actor()) sync_actor_timing(doc_.project);  // GR-3: one timeline for every actor
    rig_->external = multi_actor() ? actor_resolver(doc_.project.active) : ExternalTarget{};
    Evaluation e = [&] { VATS_PROFILE("eval rig"); return vats::evaluate(*rig_, doc_.clip(), frame_, shape()); }();
    VATS_PROFILE("eval previews");
    apply_ragdoll_preview(e);
    apply_idle_preview(e);
    apply_dynamics_preview(e);
    apply_mocap_preview(e);
    apply_sl_preview(e);
    pose_ = std::move(e.pose);
    globals_ = std::move(e.globals);
    limb_states_ = std::move(e.limbs);
    if (host_.world_view() && editing_other()) {
        // The worn avatar is your actor (the first): it plays its own animation while another is edited.
        const Project& p = doc_.project;
        Evaluation yours = evaluate_actor(0, frame_);
        host_.drive_avatar(skel_, yours.pose, actor_clip(p, 0), frame_);
        host_.set_view_frame(p.actors[0].placement().inverse() * p.actors[p.active].placement());
    } else {
        host_.drive_avatar(skel_, sl_ghost_.empty() ? pose_ : sl_pose_, doc_.clip(), frame_);  // SL preview (08 SP-1)
        host_.set_view_frame({});
    }
    // Handles of limbs that lost their IK data at this frame drop out of the selection (VP-27).
    handles_.erase(std::remove_if(handles_.begin(), handles_.end(),
                                  [&](const HandleRef& h) {
                                      return h.limb >= int(limb_states_.size()) || !limb_states_[h.limb].uses_ik;
                                  }),
                   handles_.end());
}

// ---------------------------------------------------------------------------------------------
// Autosave and crash recovery (UI-9)

std::string App::autosave_base() const {
    std::string root = host_.paths().user;
    return root.empty() ? root : root + "autosave/" + session_id_;
}

void App::autosave_tick() {
    if (headless_ || !doc_.dirty || doc_.history.is_open()) return;
    // The host may sleep while idle, so it is asked for a frame when the next autosave is due.
    auto wake_in = [this](std::uint64_t ms) { host_.wake(double(ms + 100) * 1e-3); };
    const std::uint64_t now = host_.ticks_ns() / 1000000;
    if (!autosave_due_) {  // the first dirty frame starts the clock: the first copy a minute later
        autosave_due_ = now + 60000;
        wake_in(60000);
        return;
    }
    if (now < autosave_due_) return;
    autosave_due_ = now + 120000;  // still dirty in two minutes: write again, which also keeps the file fresh
    wake_in(120000);
    write_autosave();
}

bool App::write_autosave() {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    if (session_id_.empty()) session_id_ = std::to_string(std::random_device{}()) + std::to_string(host_.ticks_ns() / 1000000);
    const std::string base = autosave_base();
    if (base.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(u8path(base.substr(0, base.find_last_of('/'))), ec);
    const std::string text = save_project(doc_.project);  // prop paths stay absolute, so it opens from anywhere
    return write_file(base + ".vat", text.data(), text.size()) &&
           write_file(base + ".path", doc_.path.data(), doc_.path.size());
}

void App::clear_autosave() {
    autosave_due_ = 0;
    if (session_id_.empty()) return;
    const std::string base = autosave_base();
    std::remove((base + ".vat").c_str());
    std::remove((base + ".path").c_str());
}

std::string App::quicksave_base() const {
    const std::string& root = host_.paths().user;
    return root.empty() ? root : root + "quicksave";
}

bool App::quit_to_quicksave() {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    const std::string base = quicksave_base();
    const std::string text = save_project(doc_.project);
    const std::string state = (doc_.dirty ? "1" : "0") + doc_.path;  // unsaved flag, then the path ("" = Untitled)
    if (base.empty() || !write_file(base + ".vat", text.data(), text.size()) ||
        !write_file(base + ".path", state.data(), state.size()))
        return false;
    quit_ = true;
    return true;
}

// The host closed the editor on its own last time: the same document comes back, no Recover prompt.
void App::reopen_quicksave() {
    const std::string base = quicksave_base();
    std::string state, err;
    if (headless_ || base.empty() || !read_file(base + ".path", state) || state.empty()) return;
    if (!open_recovered(base + ".vat", state.substr(1), state[0] == '1', err))
        return message("Could not reopen the work saved when the editor closed", err + "\n\nThe file is " + base + ".vat");
    std::remove((base + ".vat").c_str());
    std::remove((base + ".path").c_str());
    status("Reopened " + (doc_.path.empty() ? std::string("Untitled") : file_name(doc_.path)) + " as it was when the editor closed");
}

bool App::open_recovered(const std::string& file, const std::string& original, bool dirty, std::string& err) {
    std::string text;
    Project p;
    std::ifstream f(file, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(f), {});
    bool loaded = false;
    try {
        loaded = load_project(text, p, err, file);
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!loaded) return false;
    new_document();
    doc_.project = std::move(p);
    clip_replaced();  // history and body follow the recovered active actor
    doc_.path = original;
    doc_.dirty = dirty;
    update_title();
    return true;
}

// Autosaves left by a session that ended without cleaning up. A running instance rewrites its file every two
// minutes while dirty, so one untouched for five minutes belongs to a session that is gone.
// ponytail: age-based liveness; a lock file per session if two instances ever fight over it.
void App::find_recoverable() {
    if (headless_) return;
    namespace fs = std::filesystem;
    std::string dir = autosave_base();
    if (dir.empty()) return;
    dir = dir.substr(0, dir.find_last_of('/'));
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".vat") continue;
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(e.path(), ec);
        long long minutes = std::chrono::duration_cast<std::chrono::minutes>(age).count();
        if (ec || minutes < 5) continue;
        std::string original;
        read_file(fs::path(e.path()).replace_extension(".path").string(), original);
        recoverable_.push_back({e.path().string(), original, minutes});
    }
}

}  // namespace vats
