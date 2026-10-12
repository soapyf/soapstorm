// Viewport Avatar Toolset - the application: document, selection, commands and panels.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gizmo.h"
#include "dope_sheet.h"
#include "graph_editor.h"
#include "host.h"
#include "icon_button.h"
#include "imgui.h"
#include "keymap.h"
#include "pie.h"
#include "settings.h"
#include "workspaces.h"
#include "vats/anim_convert.h"
#include "vats/audio.h"
#include "vats/avatar_mesh.h"
#include "vats/balance.h"
#include "vats/dae.h"
#include "vats/dynamics.h"
#include "vats/export_name.h"
#include "vats/face_anim.h"
#include "vats/fluid_pose.h"
#include "vats/history.h"
#include "vats/jump_arc.h"
#include "vats/loop_assist.h"
#include "vats/motion_path.h"
#include "vats/onion.h"
#include "vats/overlap.h"
#include "vats/pose_match.h"
#include "vats/pose_ops.h"
#include "vats/project.h"
#include "vats/motion_quality.h"
#include "vats/rig.h"
#include "vats/rig_map.h"
#include "vats/simplify.h"
#include "vats/skeleton.h"
#include "vats/suggest_limits.h"
#include "vats/time_edit.h"
#include <unordered_set>
#include "view_math.h"

namespace vats {

struct FitOptions;
struct RetargetOptions;
struct RigTable;
// The Retarget dialog's rest pose, foot clean-up and fitting settings (retarget_ui.cpp); Batch Retarget shares them.
void retarget_settings_ui(RetargetOptions& opt, FitOptions& fit, bool& lock_feet, bool* heel_toe = nullptr,
                          bool* to_ground = nullptr);

std::string key_label(ImGuiKeyChord chord);  // "Ctrl+Up", "]": shortcut text for menus and tooltips
std::string folder_url(const std::string& dir);  // a file:// URL of a folder for Host::open_url (file_library_ui.cpp)
std::string undeformer_path(const std::string& anim_path);  // 09 0l: "<dir>/<stem>_undeform.anim" (app.cpp)

// Settings ids and menu names, indexed by Body.
inline constexpr int kBodyCount = 5;
inline constexpr const char* kBodyIds[kBodyCount] = {"female", "male", "none", "sl-default", "sl-default-male"};
inline constexpr const char* kBodyNames[kBodyCount] = {"Female", "Male", "Skeleton Only", "SL Default",
                                                       "SL Default (Male)"};

enum class Tool { Select, Move, Rotate, Scale };  // Scale: static props only (VP-40)
enum class Orientation { Local, World, Gimbal };

// The audio track's beats between frames f0 and f1 as lines from y0 to y1 (AU-2): the timeline's and the dope
// sheet's grid. x_of maps a timeline frame to a pixel.
void draw_beat_grid(ImDrawList* dl, const Clip& c, const std::function<float(double)>& x_of, double f0, double f1, float y0, float y1);

// The last item was let go with Enter: a value typed in and confirmed, whether or not it changed (a deliberate
// "key it as it is"). ImGui reports the deactivation in the frame the key goes down.
inline bool item_entered() {
    return ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
}

// A command with its menu label and shortcut, shared by the menus and the key dispatcher.
struct Action {
    const char* label;
    ImGuiKeyChord key = 0, key2 = 0;  // the bindings of the active preset
    bool repeat = false;  // fires again while the key is held
    std::function<void()> run;
    std::function<const char*()> unavailable;  // why it can't run now, or nullptr
};

class App {
public:
    explicit App(ui::Host& host) : host_(host), camera_(host.camera()) {}
    // display_scale: the UI scale the host's display asks for (window coordinates).
    bool init(float display_scale, std::string& err);
    void shutdown();
    // Draws one frame of UI. Returns false once the user has chosen to quit.
    bool frame();
    void quit_unattended();  // the system asked the program to stop: no prompt, unsaved work kept as an autosave
    void request_quit();
    // A host closing the editor on its own (the viewer, spec 09 U4b): the document (every actor, the path and
    // whether it is unsaved) goes to a quicksave that the next init reopens as it was, with no prompt. False
    // when there is nowhere to write it; the editor then stays open.
    bool quit_to_quicksave();
    bool viewer_reset_joints() const { return settings_.viewer_reset_joints; }  // the viewer's Preferences toggle
    bool viewer_show_others() const { return settings_.viewer_show_others; }    // View > Show Other Avatars (the viewer)
    const Skeleton& skeleton() const { return skel_; }
    // The viewer's face-positions check (spec 09 §5a): the bytes an upload of the active actor sends, the same with
    // Bake shape SL Default (the negative control), and the pose at each sampled frame (0, the face tracks' keys, the
    // last; at most 40) as the preview shows it without live previews. False with the reason when nothing exports.
    struct FaceCheck {
        std::vector<std::uint8_t> bytes, control;
        double fps = 30;
        std::vector<double> frames;
        std::vector<Pose> poses;
    };
    bool face_check(FaceCheck& out, std::string& why);
    void show_status(const std::string& s) { status(s); }
    void open_path(const std::string& path);  // command line and drag-and-drop
    // Command-line helpers for scripted checks: apply a built-in pose ("builtin:<slug>" or its slug) at
    // frame 0 (hand poses on both hands), and frame the selection.
    bool apply_builtin_pose(const std::string& slug);
    void focus_selection() { run_action("frame_selected"); }
    void set_camera_distance(double d) { camera_.distance = d; }
    void set_camera_pitch(double p) { camera_.pitch = p; }
    // --view front|back|right|left|top|ortho: that View menu action, with the camera's swing finished at once.
    bool cli_view(const std::string& name) {
        const std::string id = "view_" + name;
        for (auto& [aid, a] : actions_)
            if (aid == id) return run_action(id.c_str()), update_camera_animation(1), true;
        return false;
    }
    void set_inventory_filter(const std::string& text) { inv_filter_ = text; }  // --filter
    void set_bone_filter(const std::string& text) { bone_filter_ = text; }  // --bone-filter
    void cli_sit() { sit_on(-1); }  // --sit: Tools > Sit on Seat
    void set_headless(bool h) { headless_ = h, show_welcome_ = show_welcome_ && !h; }  // scripted runs: no dialogs
    void start_playing() { playing_ = true; }  // --bench
    void select_all() { run_action("select_all"); }  // --select-all
    void set_tool(const std::string& t) {
        tool_ = t == "select" ? Tool::Select : t == "move" ? Tool::Move : t == "scale" ? Tool::Scale : Tool::Rotate;
    }
    void set_body(int b, bool remember = true);  // a Body value; --body passes remember = false
    // More command-line helpers (UI-10).
    void goto_frame(double f) { set_frame(f); }
    // Opens the help browser at a page (title or file stem; "" = the front page, VATs) and heading.
    void open_help(const std::string& page = "", const std::string& anchor = "");
    void cli_import_prop(const std::string& path) { guarded(path, [&] { import_prop(path); }); }
    // --import-body: the files as one body, shown (headless runs: the report goes to stderr).
    void cli_import_body(const std::vector<std::string>& paths) { guarded(paths.empty() ? "" : paths[0], [&] { import_body(paths); }); }
    void cli_export_rig(const std::string& path) { guarded(path, [&] { export_rig(path); }); }  // --export-rig
    void select_prop(int index) {
        if (index < 0 || index >= int(doc_.clip().props.size())) return;
        clear_selection();
        selected_prop_ = index;
    }
    // library = Inventory; poses, props, bodies = Inventory at Poses, Meshes, Bodies; actors, check open the Actors, Animation Check windows,
    // face the Face window, export the Export dialog, sl-preview the SL preview
    void show_tab(const std::string& tab) {
        if (tab == "actors") show_actors_ = true;
        else if (tab == "check") show_check_ = true;
        else if (tab == "face") show_face_ = true;
        else if (tab == "export") show_export_dialog_ = true;
        else if (tab == "sl-preview") sl_preview_ = true;
        else pending_tab_ = tab == "bones" ? "Bones" : "Inventory";
        inv_scroll_to_ = tab == "poses" ? "Poses" : tab == "props" ? "Meshes" : tab == "bodies" ? "Bodies" : "";
    }
    void planner_add(const std::string& path);           // --plan-clip: the Priority Planner with this context clip
    void batch_retarget_folder(const std::string& dir);  // --batch-retarget: Batch Retarget run on this folder
    // --window: opens a tool window or brings a panel to the front, by its --window name; false when unknown.
    bool show_window(const std::string& name);
    bool set_theme(const std::string& name) {  // --theme: by its name in Preferences, this run only; false when unknown
        if (find_theme(name) == 0 && name != theme_name(0)) return false;
        if (!session_theme_) session_theme_ = settings_.theme;  // the file keeps the theme chosen in Preferences
        settings_.theme = name;
        apply_look();
        return true;
    }
    bool set_preset(const std::string& name) {
        Preset p;
        if (!preset_from_name(name, p) || (host_.world_view() && p != Preset::SecondLife)) return false;
        settings_.preset = p;
        apply_preset();
        if (settings_.preset == Preset::SecondLife) tool_ = Tool::Move;
        return true;
    }
    void fit_graph() {
        GraphContext g = graph_context();
        graph_.frame_all(g);
    }
    void show_points() { show_category_[7] = true; }
    void cli_physics() { settings_.avatar_physics = true; }  // --physics: Tools > Avatar Physics Preview for this run
    // --mesh-body <file.dae|.fbx>: shows a rigged file as the mesh body for this run only, kept out of the library.
    void cli_mesh_body(const std::string& path);
    // --picker <page>[/<view>] and --picker-style <silhouette|avatar|rest> (08 PK): the Picker tab at a page and view
    // ("hands/palm"), and its backdrop for this run; false when unknown.
    bool cli_picker(const std::string& page_view);
    bool cli_picker_style(const std::string& style);
    // --bones <stick|hidden>: View > Bones > Style for this run, not saved; false when unknown.
    bool cli_bone_style(const std::string& style) {
        if (style != "stick" && style != "hidden") return false;
        bone_style_run_ = style;
        return true;
    }
    bool cli_select_group(const std::string& name);  // --select-group: a picker group by its name ("Right Arm")
    // --light <noon|key|rim|dusk|night|studio> and --backdrop (08 LT): the Light menu from the command line.
    bool set_light(const std::string& id);
    void show_backdrop() { backdrop_ = true; }
    // --reference <png> (08 RF): a picture as the backdrop; --listing <file.gif|file.png> (08 LM): writes listing
    // media with the window's settings (its size, turntable on) and says what it wrote.
    void cli_reference(const std::string& path) { guarded(path, [&] { load_reference(path, false); }); }
    void cli_target(const std::string& path) { guarded(path, [&] { load_target(path); }); }  // --target: the target ghost
    bool export_listing_media(const std::string& path);
    void select_by_name(const std::string& name) {
        int i = skel_.find(name);
        if (i >= 0) reveal_node(i), select(i, false);  // its group shown, as a pick from the Bones list shows it
    }
    // True while something animates on its own, so the main loop must not sleep.
    bool busy() const { return playing_ || dragging_gizmo_ || modal_ != Modal::None || pose_blend_ || cam_glide_.active || cam_keys_held_ || mocap_busy() || cube_alpha_ != (cube_hover_ ? 1.f : 0.5f) || (follow_through_ && !follow_through_->settled()) || (physics_sim_ && !physics_settled_) || pie_.open; }

private:
    // Document
    struct Document {
        Project project;
        std::string path;
        bool dirty = false;
        History history;
        Clip& clip() { return project.clip; }
        const Clip& clip() const { return project.clip; }
    };
    template <class F>
    void edit(const std::string& label, F&& change) {
        doc_.history.begin(doc_.clip());
        change(doc_.clip());
        if (doc_.history.commit(label, doc_.clip())) mark_dirty();
    }
    void mark_dirty();
    void update_title();
    void new_document();
    bool save(const std::string& path);
    // example: a help page's example project, opened as an untitled copy that is not added to the recent files.
    void load_project_file(const std::string& path, bool example = false);
    void import_file(const std::string& path);
    int anim_bytes(AnimExportResult& r, std::vector<std::uint8_t>& bytes);
    int export_in_memory(AnimExportResult& r, std::vector<std::uint8_t>& bytes);  // anim_bytes without a message
    std::vector<std::uint8_t> undeformer_bytes(const AnimExportResult& r);  // 09 0l, empty = none
    AnimExportOptions anim_export_options();  // the active actor's, as every .anim export uses them
    Clip anim_export_clip() const;            // the active clip as it exports: mirrored with Export mirrored
    std::vector<std::string> reset_position_joints() const;  // the joints Reset joint positions keys at rest
    bool export_anim(const std::string& path);
    // The viewer's direct upload (Host::can_upload): every file Export would write; all_clips: of every clip (08 CL-4).
    void upload_now(bool all_clips = false);
    void upload_next();  // the next queued upload, after the previous one's confirmation
    std::deque<std::pair<std::string, std::vector<std::uint8_t>>> upload_queue_;  // name, bytes
    bool export_bvh(const std::string& path, bool all_bones);
    // Runs then() now, or after the user has dealt with unsaved changes.
    void guard_unsaved(std::function<void()> then);

    // File dialogs run asynchronously; results come back through this queue.
    enum class Dialog { Open, SaveAs, ImportAnim, ImportBvh, ImportProp, ImportBody, ImportRetarget, LoadAudio, ExportFolder,
                        LoadActor, SaveActor, ExportActor, SitLines, AoNotecard, ExpressionPack, Rhubarb, LoadReference,
                        ListingMedia, LoadTarget, ExportFile, ExportRig, MapRig, RigScratch };
    // the *Actor ones act on file_actor_; SitLines on sit_format_, AoNotecard on ao_format_
    // A dialog's answer, queued for the next frame (hosts may answer on another thread). Several files
    // (body parts) arrive joined with newlines; "" = cancelled.
    ui::FilesChosen dialog_result(Dialog kind);
    std::string export_summary_;
    std::optional<RawAnim> raw_import_;  // IO-22: the last .anim import, for byte-exact re-export
    // Autosave and crash recovery (UI-9): a dirty document is written to autosave/<session>.vat a minute
    // after the first change, then every two minutes, and removed when it is saved, replaced or the app
    // quits; leftovers are offered back on launch.
    std::string autosave_base() const;  // "" when there is no preference folder
    void autosave_tick();
    void clear_autosave();
    bool write_autosave();
    bool keep_autosave_ = false;  // quit_unattended() left work to recover
    // File readers can throw on a hostile or broken file (std::length_error, std::bad_alloc): this reports it
    // as "Could not read <file>" and returns false instead of ending the app with unsaved work.
    bool guarded(const std::string& path, const std::function<void()>& f);
    unsigned doc_generation_ = 0;  // bumped by new_document(), which every open, import and recover goes through
    static bool write_text(const std::string& path, const std::string& text, bool backup, std::string& why);  // now, regardless of the clock; false when nothing was written
    void find_recoverable();
    void draw_recovery();
    std::string quicksave_base() const;  // "" when there is no preference folder
    void reopen_quicksave();
    // Loads an autosave or quicksave as the document, as it was: its original path and unsaved state.
    bool open_recovered(const std::string& file, const std::string& original, bool dirty, std::string& err);
    std::string session_id_;
    std::uint64_t autosave_due_ = 0;  // ms (ticks_ns() / 1e6) of the next autosave; 0 = the clock has not started
    struct Recoverable {
        std::string file, original;  // the autosave and the project it came from ("" = never saved)
        long long age_minutes = 0;
    };
    std::vector<Recoverable> recoverable_;  // the last export's bones, length, priority and points (UI-34)
    int export_after_folder_ = -1;  // export waiting for a folder: 0 .anim, 1 BVH, 2 BVH all bones
    bool export_all_after_folder_ = false;  // ... of every clip (08 CL-4)
    // after_save: with SaveAs, what runs once the file is saved (a New, Open or Quit that asked to save first).
    void show_dialog(Dialog kind, std::function<void()> after_save = {});
    std::function<void()> after_save_as_;
    std::mutex dialog_mutex_;
    std::vector<std::pair<Dialog, std::string>> dialog_results_;

    // Commands and panels
    void build_actions();
    void apply_preset();          // rebinds every action for settings_.preset
    void apply_look();            // theme and interface size
    HostColours host_colours_{};  // the host's skin as last applied (Host::skin_colours)
    bool has_host_colours_ = false;
    const CameraView* project_camera(int slot) const;
    void store_project_camera(int slot, const CameraView& v);
    // --body shows a body for this run only: the file keeps the body chosen in the app (session_body_).
    void save_settings() {
        Settings s = settings_;
        if (session_body_) s.body = session_body_->first, s.mesh_body = session_body_->second;
        if (saved_preset_) s.preset = *saved_preset_;
        if (session_theme_) s.theme = *session_theme_;
        s.save(host_.paths().settings);
    }
    std::optional<std::string> session_theme_;  // the saved theme under --theme, until Preferences picks one
    std::optional<std::pair<std::string, std::string>> session_body_;
    std::optional<Preset> saved_preset_;  // the saved preset when this host does not offer it (apply_settings)  // the saved body and mesh body under --body
    std::string nav_hint() const;
    std::string graph_nav_hint() const;
    std::string key_hint(const char* action) const;  // "Home", "Ctrl+E" or "" for the active preset
    // Right Alt held: each toolbar button and workspace tab shows its key on a small badge. Left Alt stays the camera.
    bool key_badges_ = false;
    void key_badge(const char* action);  // on the last item, while key_badges_
    void round_docked_corners();
    const DaeModel* prop_model(const std::string& path);
    // globals/world: another actor's pose and placement (GR); defaults: the active actor.
    Xform prop_frame(const Prop& p, const std::vector<Xform>* globals = nullptr, const Xform& world = {}) const;
    // A rigged file that looks like a whole avatar body asks whether to use it as the body (as_prop skips that).
    void import_prop(const std::string& path, bool as_prop = false);
    std::string body_prompt_path_;
    void draw_body_prompt();
    void draw_props(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices);
    int pick_prop(ImVec2 mouse) const;
    void draw_prop_section();
    // Sit on This: the pose onto the seat of world prop k (-1: whichever is under the thighs), its feet held on the floor;
    // the Sitting starter pose first when the thighs are not level yet.
    void sit_on(int k);
    std::vector<int> seat_props() const;  // the world props a pose can sit on (static, visible), in scene order
    void draw_preferences();
    void draw_controls_help();
    void draw_shortcuts();  // Edit > Keyboard Shortcuts... (shortcuts_ui.cpp)
    void draw_shortcut_table(bool editable, const char* search, float height);  // its table; Help > Controls shows it too
    void set_action_keys(const std::string& id, const KeyPair& keys);  // records an override, saved at once
    void draw_welcome();
    void draw_help_browser();
    void help_button(const char* page);  // "?" in a tool window's title bar, opens its help page
    void draw_export_section(float body_h = 0);  // windows.cpp: 0 inline, < 0 fills a panel, > 0 a dialog's most
    void draw_export_dialog();
    void draw_follow_dialog();
    void draw_about();
    void draw_bvh_prompt();
    // Export tab: writes straight to the export folder; all_clips: every clip in turn (08 CL-4), in the active one's folder.
    void export_now(bool bvh, bool all_bones, bool all_clips = false);
    // The export naming of the active actor's clip k (-1 = the active clip), with [CLIP] when the project has several
    // clips (08 CL-4).
    ExportNaming export_naming(int k = -1) const;
    const Clip& active_actor_clip(int k) const;  // the active actor's clip k (08 CL), without switching to it
    std::string export_stem() const;  // the project file's stem, [NAME]'s fallback
    void handle_shortcuts();
    void menu_item(const char* id);
    void draw_menus();
    void draw_menu_bar();  // draw_menus, crawled for Find a Tool's index or to run a menu item (tool_search.cpp)
    std::vector<MenuEntry> menu_index_;  // every menu item and its path, from a crawl at the start
    bool menu_indexed_ = false;
    std::optional<std::pair<std::string, std::string>> menu_run_;  // a menu item to run (path, label) next frame
    void draw_dockspace();
    void draw_bones_panel();
    void draw_properties_panel();
    void draw_timeline_panel();
    void draw_graph_panel();
    void draw_hand_poser();
    void draw_inventory_panel();
    // A section header that keeps its open state in the settings. While the Inventory is filtered, a section with no
    // match (matches false) is left out, header and all, so the hits are not a long scroll down.
    bool inventory_section(const char* name, bool matches = true);
    void open_context_menu(int node);  // node -1 = the whole avatar
    void draw_context_menu();
    std::vector<std::string> part_tracks(const BodyPart& part) const;
    void run_action(const char* id);
    void draw_name_prompt();
    std::string library_path() const;
    void load_library();
    void save_library();
    void store_library_item(LibraryItem item);
    void use_library_item(int index, bool mirrored);
    void apply_library_item(const LibraryItem& item, bool mirrored);
    bool clip_range(double& a, double& b) const;
    void clip_replaced() {
        graph_.clip_replaced();
        pose_blend_.reset();  // its before and after belong to the old clip
        doc_.history.set_actor(doc_.project.active);
        sync_active_body();
    }
    GraphContext graph_context();
    template <class F>
    void with_graph(F&& f) {
        GraphContext g = graph_context();
        f(g);
    }
    void draw_status_bar();
    void draw_host_pane();  // Host::host_ui's pane (spec 09 U4b)
    bool show_host_pane_ = true;
    void draw_viewport();
    // The world view's other actors, ghosts and collision volumes as lines (spec 09 U4).
    void draw_world_extras(ImDrawList* dl);
    // The world view's triangles (spec 09 U5): other actors' bodies and props, which the host draws with the world.
    void render_world_scene();
    ImGuiID dockspace_id_ = 0;
    void draw_message_popup(bool in_export_dialog = false);  // inside the Export dialog while it is open

    // Viewport
    void evaluate();
    ImTextureID render_scene(int w, int h);  // the view's picture, or 0 when the host draws none
    int pick_bone(ImVec2 mouse, std::vector<int>* ranked = nullptr) const;
    int pick_mesh_bone(ImVec2 mouse, double* out_t = nullptr) const;
    void draw_view_cube(ImDrawList* dl, ImVec2 vp_min, bool viewport_hovered);  // top left of the view
    void look_from(const Vec3& direction);  // animated (VP-63)
    void focus_camera_on(const Vec3& point);  // Second Life Alt+click, eased like the viewer's focus swing
    void update_camera_animation(double dt);
    // The first surface under the cursor: the skinned body, then the ground within 20 m.
    bool pick_surface(ImVec2 mouse, Vec3& point) const;
    // Second Life preset (viewport.cpp): what an Alt+click focuses on, the drag and the wheel.
    slcam::Focus sl_avatar_focus(int actor) const;  // actor -1: the one being edited
    double ray_prop(int prop, const Vec3& origin, const Vec3& dir) const;  // the hit's ray parameter, or 1e30
    bool sl_focus_at(ImVec2 mouse);
    void sl_camera_drag(ImVec2 delta, float view_width);
    slcam::Focus sl_focus_;
    bool sl_valid_click_ = false, sl_outside_slop_x_ = false, sl_outside_slop_y_ = false;
    float sl_accum_x_ = 0, sl_accum_y_ = 0;
    void viewport_input(const ImVec2& origin, const ImVec2& size, bool hovered);
    bool place_gizmo();
    // A tool window's first-open place and size, from windows.cpp, before its Begin(title): over the 3D view (not
    // the panels), beside the avatar rather than over it, over other open windows as little as it can; w_em wide,
    // as tall as its content (h_em to place it), at most 70% of the screen's height.
    void place_tool_window(const char* title, float w_em, float h_em);
    float look_scale_ = 0, dock_scale_ = 1;  // the UI scale applied last; the change the dock has yet to take
    // Mesh bodies from devkits (bodies.cpp, spec 08 BD).
    struct MeshBody {
        std::string id, name;
        std::vector<std::string> parts;  // rigged .dae files, absolute paths
        std::map<std::string, std::string> labels;  // RM-8: a reused (spare-chain) joint -> what it holds ("scarf")
    };
    std::vector<MeshBody> bodies_;
    // RM-8: the shown body's reused joints and what each holds: its parts' mappings, then the body's own edits. Kept in
    // Skeleton::set_reused each frame, so mirroring and limit templates leave them alone.
    void sync_reused_bones();
    std::map<std::string, std::string> bone_labels_;
    // "mWing2Right (scarf)" for a reused joint, else its name and, with View > Bones > Plain Names on, its plain name
    // ("mHipLeft · Left Thigh"; plain = false leaves it out); picker_label likewise after the picker's name.
    std::string bone_label(int node, bool plain = true) const;
    std::string bone_label(const std::string& bone, bool plain = true) const;
    std::string plain_label(int node) const;  // the plain name bone_label adds, or ""
    std::string picker_label(int node) const;
    // Properties: what a reused joint's chain holds, renamed: saved with the body and in its parts' mappings.
    void set_bone_label(const std::string& joint, const std::string& label);
    std::string holds_for_;  // the joint holds_ was filled from
    char holds_[64] = "";
    void load_bodies();
    void save_bodies() const;
    const MeshBody* mesh_body() const;  // the one shown instead of the Linden body, or null
    // The world view's body swap (spec 09 build 32): the mesh body shown in your avatar's place, on your screen only, or
    // null (none chosen, or none of its parts loads). It stays the export's "Your avatar" while swap_shown() is off.
    const MeshBody* swap_body() const;
    // The modes that play your real avatar in the world: As It Plays In-World, the walk test, Place on Furniture Point.
    bool real_avatar_mode() const { return in_world_ || walk_test_ || seat_pick_; }
    // It shows now: always with View > Body > Keep in Real-Avatar Modes (build 34, the default), else not while a real-avatar
    // mode runs (your real avatar shows then, as in build 32). The app: always false.
    bool swap_shown() const { return swap_body() && (settings_.viewer_keep_swap || !real_avatar_mode()); }
    const Shape* worn_shape() const { return swap_shown() ? nullptr : host_.body_shape(); }  // the worn avatar's, unless swapped
    // Build 34: while it shows in a real-avatar mode, the body follows what your avatar does in the world (the region's and
    // your AO's animations, the walk, the sit), not the editor's pose: your avatar's live pose (Host::live_pose) on the
    // body's own joints, in your actor's space. Null otherwise, or when the host cannot say (the editor's pose then).
    const std::vector<Xform>* swap_live_globals();
    std::vector<Xform> swap_live_;
    // What a real-avatar mode's status line adds while a body is swapped in: it stays and follows you, or your avatar shows.
    std::string real_mode_swap_note() const;
    void use_mesh_body(const std::string& id);
    void import_body(const std::vector<std::string>& paths);
    // globals: a pose of your actor's other than the editor's (swap_live_globals); null = the editor's.
    void draw_mesh_body(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices,
                        const std::vector<Xform>* globals = nullptr, bool keep_live = false);
    const MeshBody* find_mesh_body(const std::string& id) const;
    // BD-3: base with the joints the body's parts were rigged to, and their rig axes (base itself when they have neither).
    void harmonize_body(const MeshBody& b) const;
    const Shape* mesh_body_shape(const MeshBody& b, const Shape* base) const;
    mutable std::map<std::pair<std::string, const Shape*>, std::optional<Shape>> body_shapes_;
    void draw_bodies_section();
    void apply_settings();
#ifdef VATS_LEGACY_IMPORT
    // IO-54: the first-run legacy import (migrate.cpp).
    void offer_migration(bool first_run);
    std::string migrate_reference_data();  // returns a list of what was imported
    void draw_migration_prompt();
    std::string migration_dir_;
#endif
    bool show_migration_ = false;  // the first-run import offer is up (only with VATS_LEGACY_IMPORT)
    void keyboard_camera();  // Second Life preset: Alt + arrow keys
    bool cam_keys_held_ = false;
    void apply_gizmo_drag(ImVec2 mouse, bool snap);
    void capture_edit_start();
    void apply_delta(const Quat& r, const Vec3& t, int gimbal_axis = -1, double gimbal_angle = 0);
    // Blender preset: G / R over the view start a transform without clicking (spec 04 VP-51).
    enum class Modal { None, Move, Rotate, Trackball, Tween };  // Tween: spec 08 TW-1's key drag
    void start_modal(Modal kind);
    bool modal_input(ImVec2 mouse);  // true while a modal transform consumed the frame
    void end_modal(bool confirm);
    bool modal_pivot(Vec3& pivot, Quat& local) const;

    // Selection and editing helpers
    int primary() const { return selection_.empty() || handle_primary_ ? -1 : selection_.back(); }
    void select(int node, bool toggle);
    struct HandleRef {
        int limb = -1;
        bool pole = false;
        bool operator==(const HandleRef&) const = default;
    };
    void select_handle(HandleRef h, bool toggle);
    void clear_selection() {
        selected_prop_ = -1;
        selection_.clear();
        handles_.clear();
        handle_primary_ = false;
    }
    const HandleRef* primary_handle() const { return handle_primary_ && !handles_.empty() ? &handles_.back() : nullptr; }
    bool handle_shown(int limb, bool pole) const;
    bool handle_screen(int limb, bool pole, ImVec2& out) const;
    int pick_handle(ImVec2 mouse, bool& pole) const;
    void draw_handles(ImDrawList* dl) const;
    const Shape* shape() const;  // the view's body; a mesh body's own proportions when one is shown (BD-3)
    const Shape* view_body_shape() const;  // the app's own body (the Linden body or the mesh body shown)
    const Shape* export_shape() const;  // IK and pins bake against this, not the viewport body (IO-13)
    double face_move_scale() const;     // FaceSettings::scale for every face move, from the head the export plays on
    std::string current_body_id() const;
    const RigConstraints* constraints() const;
    bool is_joint_weighted(int node) const;
    // The shown body's mesh posed by globals on sh: a mesh body's parts, else the Linden body; none for Skeleton
    // Only, or in the world view unless a swapped body is shown (the worn avatar's mesh is the host's).
    std::vector<std::vector<float>> shown_body_skin(const std::vector<Xform>& globals, const Shape* sh) const;
    const MeshContactFloor& current_contact_floor() const;  // CM-1, of the shown body in the current pose
    void invalidate_floor_cache() const { floor_cache_valid_ = false; }
    double contact_height() const;
    double rest_floor() const;  // where the shown body's soles are at rest: the ground grid's height
    double swap_lift() const;   // the world view: how far a swapped body is raised to stand where yours does
    // The joint positions position keys are written from: the worn avatar's with "Your avatar", read live from the
    // host and never stored; else null (IO-11).
    const Shape* export_positions() const;
    // The export settings' "shape", or the default: "avatar" in the viewer while the worn avatar has mesh joint
    // positions, else "sl-default".
    // yours = false (another actor than your avatar, the first, unless "Use Your avatar for every actor"): never "avatar".
    std::string bake_shape_key(const Json& export_settings, bool yours = true) const;
    // During a multi-actor export or upload: the actor the user was editing, whose settings apply; -1 otherwise.
    int export_home_ = -1;
    const Json& export_home_settings() const;  // that actor's export settings (the active one's when not exporting)
    mutable int reset_joints_count_ = 0;
    mutable unsigned long long reset_joints_serial_ = ~0ULL;
    mutable int reset_joints_active_clip_ = -1;
    mutable unsigned reset_joints_doc_gen_ = ~0U;
    mutable std::string reset_joints_mode_;
    // HV (spec 08 section 20, ui/height_variant_ui.cpp): while a height variant exports, the body it bakes on (for
    // the active actor and the cross-actor pin targets); null otherwise.
    const Shape* height_shape_ = nullptr;
    BodyShape height_body_;
    void set_export_height(double height_m);  // 0 = none; SL Default (Male) bake shapes get the male body
    double export_hip_lift() const;           // how much higher the hips stand on height_shape_ than on the bake shape
    void draw_height_variants(float label_w);  // Properties > Export, "Also export for heights"
    bool exporting_yours() const;               // the actor being exported (the active one) may use Your avatar
    std::string bake_shape_label(const std::string& key) const;  // "SL Default", "Mesh body: <name>", ...
    int limb_for_action() const;  // the limb of the primary handle or bone, or -1
    std::vector<std::string> selected_tracks() const;  // bones, their pin: tracks, selected handles' ik. tracks
    // Shown in View > Bones (its group); node_visible also leaves out what Hide Unused Bones hides.
    bool node_shown(int node) const {
        return skel_[node].volume ? show_volumes_ : show_category_[static_cast<int>(skel_[node].category)];
    }
    bool node_visible(int node) const {
        return node_shown(node) && (!settings_.hide_unused_bones || bone_used(node));
    }
    // A bone picked from the Bones list or the picker shows in the view, so it has its stick and gizmo: its group is
    // shown, and Hide Unused Bones turns off when it hides the bone. Returns what changed, for the status ("" if nothing).
    std::string reveal_node(int node);
    // Why a hidden bone is hidden, and what a click on it does (tooltips).
    const char* hidden_hint(int node) const {
        return node_shown(node) ? "Hidden by View > Bones > Hide Unused Bones: a click turns that off"
                                : "Hidden in Bones > Show: a click shows its group";
    }
    // View > Bones > Hide Unused Bones: whether the shown mesh body moves with this bone, directly or through a
    // bone below it (so the chain down to a used bone stays). Always true for the SL avatar and attachment points.
    bool bone_used(int node) const;
    mutable std::vector<char> used_bones_;
    mutable std::vector<char> weighted_nodes_;  // is_joint_weighted, per node, for weighted_key_
    mutable std::string weighted_key_;
    mutable int weighted_frame_ = -1;
    mutable std::string used_bones_key_;
    std::string revealed_groups_key_;  // the body whose rigged bone groups were last switched on
    std::array<bool, 9> auto_shown_{};  // bone groups (8: collision volumes) VATs switched on for a body, not you
    void reveal_rigged_groups();       // a newly shown mesh body: switch on each bone group it is weighted to
    void set_frame(double f);
    void step_key(int direction);
    void status(const std::string& s) { status_ = s; }
    void message(const std::string& title, const std::string& text);

    ui::Host& host_;
    std::string data_dir_;
    Skeleton skel_;
    AvatarMesh mesh_;
    Body body_ = Body::SLDefault;
    Document doc_;

    std::vector<int> selection_;  // primary last
    double frame_ = 0;
    double evaluated_frame_ = -1;  // the frame pose_ and globals_ were last evaluated at
    bool playing_ = false;
    std::uint64_t last_tick_ = 0;
    std::array<bool, 8> show_category_{true, true, false, false, false, false, false, false};
    bool xray_ = true;
    bool show_volumes_ = false;  // collision volumes, hidden by default (README decision 12)
    Tool tool_ = Tool::Rotate;
    Orientation orientation_ = Orientation::Local;
    float snap_deg_ = 5, gizmo_size_ = 90;

    // Evaluated every frame
    Pose pose_;
    std::vector<Xform> globals_;
    std::vector<LimbState> limb_states_;
    std::unique_ptr<Rig> rig_;
    std::vector<HandleRef> handles_;  // selected IK handles; primary is the last when handle_primary_
    bool handle_primary_ = false;
    int hover_handle_ = -1;
    bool hover_handle_pole_ = false;
    int selected_prop_ = -1;  // a selected prop is exclusive with bones and handles (VP-27)
    // What each mesh shows (shown_model of its source under its look): everything draws, picks, stands and exports this.
    std::map<std::string, std::unique_ptr<DaeModel>> prop_models_;
    std::map<std::string, DaeReport> prop_reports_;  // each loaded mesh's import report (the rig export check reads it)
    // Spec 08 SK: each mesh as loaded, with all its parts and shape keys; the rig export's fixes and the soft-body share
    // edit this, and the shown model is rebuilt from it. prop_looks_: the look each shown model was built with.
    std::map<std::string, std::unique_ptr<DaeModel>> prop_sources_;
    std::map<std::string, MeshLook> prop_looks_;
    DaeModel* part_source(const std::string& path);      // loads the mesh when it is not yet
    void part_source_changed(const std::string& path);   // rebuilds what it shows, and what depends on that
    MeshLook mesh_look(const MeshBody& b) const;         // the project's, else what its files' mappings say
    MeshLook look_for_path(const std::string& path) const;
    void sync_mesh_looks();          // each frame: a mesh whose look changed (an edit, an undo, a project) is rebuilt
    void save_looks_to_mappings();   // the shown body's look into its files' mappings, each file's own parts and keys
    void record_look_step(const std::string& label, std::map<std::string, MeshLook> before);
    void draw_body_look_section(bool in_properties = false);  // Parts and Shape Keys: Inventory > Bodies, and Properties
    void draw_body_parts_menu();  // View > Body: the shown body's parts, ticked when shown
    void fit_face_pose_to_key(const std::string& key);  // SK-5: a shape key as a face pose in the pose library
    const DaeModel* shape_key_model(const std::string& key);  // the shown body's file that moves most with it
    // A key whose motion is mostly on face-weighted vertices: only those offer a face pose. By body id and key.
    bool face_shape_key(const std::string& key);
    std::map<std::string, bool> face_keys_;
    std::map<std::string, MeshLook> look_drag_before_;  // the looks as a shape key drag started
    bool look_drag_open_ = false;
    std::string shape_key_filter_;
    unsigned look_generation_ = 0;   // bumped as a shown mesh is rebuilt (the rest floor follows)
    std::vector<float> prop_skin_pos_, prop_skin_nrm_;
    std::map<std::string, std::vector<float>> mesh_body_skin_pos_;
    std::map<std::string, std::vector<float>> live_body_skin_pos_;  // the swapped body in your live pose (world view)
    std::vector<Xform> mesh_skin_globals_;  // the pose, shape and body mesh_body_skin_pos_ was skinned in
    const Shape* mesh_skin_shape_ = nullptr;
    std::string mesh_skin_body_;
    Prop drag_start_prop_;
    CameraGlide cam_glide_;
    float cube_alpha_ = 0.5f, cube_press_size_ = 110;
    bool cube_hover_ = false, cube_moved_ = false;
    int cube_drag_ = 0, cube_press_region_ = -1;  // drag: 1 orbit, 2 resize
    ImVec2 cube_press_;
    Modal modal_ = Modal::None;
    int modal_axis_ = -1;  // -1 view axis, else 0..2
    bool modal_local_ = false, viewport_hovered_ = false;
    ImVec2 modal_press_;
    std::string modal_readout_;
    bool snap_on_ = false;  // Second Life preset: G toggles snapping instead of holding Ctrl
    Tool drag_tool_ = Tool::Rotate;  // the tool a running gizmo drag uses
    Tool effective_tool() const;  // Second Life: Ctrl held = Rotate
    int euler_drag_bone_ = -1, euler_drag_axis_ = 0;  // QAvimator modifier drags
    ImVec2 euler_drag_press_;
    Vec3 euler_drag_start_;
    Xform drag_start_target_;
    Vec3 drag_start_pole_;
    std::vector<float> skin_pos_, skin_nrm_;
    mutable bool floor_cache_valid_ = false;
    mutable MeshContactFloor cached_floor_;
    mutable std::vector<Xform> floor_globals_;  // the pose cached_floor_ was skinned in
    mutable const Shape* floor_shape_ = nullptr;
    mutable double rest_floor_ = 0;
    mutable std::string rest_floor_key_ = "-";
    mutable const Shape* rest_floor_shape_ = nullptr;
    mutable std::optional<double> default_floor_;  // SL's default body's soles at rest

    // 3D view
    Camera& camera_;  // the host's (ui::Host::camera)
    Gizmo gizmo_;
    Projector projector_;
    Gizmo::Part gizmo_hover_ = Gizmo::None;
    int hover_bone_ = -1;
    bool dragging_gizmo_ = false;
    Xform drag_start_global_, drag_parent_global_;
    Vec3 drag_start_offset_, drag_start_euler_;
    ImVec2 last_click_{-100, -100};

    GraphEditor graph_;
    DopeSheet dope_{graph_};  // spec 08 DS: shares graph_'s key selection
    bool show_hands_ = false;
    Library library_;
    bool apply_mirrored_ = false;
    enum class NameAction { None, SavePose, SaveClip, SavePartPose, SavePartClip };
    int context_node_ = -1;
    BodyPart context_part_;
    PoseClipboard pose_clipboard_, part_clipboard_;
    NameAction name_action_ = NameAction::None;
    std::string name_prompt_;
    char name_buf_[128] = {};
    int range_a_ = -1, range_b_ = -1;  // timeline frame range (Shift-drag), -1 = none
    int hand_drag_side_ = -1, hand_drag_dot_ = -1;
    ImVec2 hand_press_;
    Clip hand_start_clip_;
    RigConstraints hand_limits_;  // the anatomical finger limits, for a body that sets none (Respect Joint Limits)
    ImVec2 viewport_min_{0, 0}, viewport_max_{800, 600};  // the 3D view's rectangle, last frame
    bool show_graph_ = true;
    std::vector<std::pair<std::string, Action>> actions_;
    std::map<std::string, std::pair<ImGuiKeyChord, ImGuiKeyChord>> industry_keys_;
    std::map<std::string, KeyPair> preset_keys_;  // the active preset's keys, before the user's overrides
    // Keyboard Shortcuts: the search text, and the key cell waiting for a key (capture_slot_ -1: none).
    bool show_shortcuts_ = false;
    char shortcut_search_[64] = {};
    std::string capture_id_, conflict_with_;
    int capture_slot_ = -1, conflict_slot_ = -1;
    ImGuiKeyChord conflict_chord_ = 0;
    Settings settings_;
    std::string assets_dir_;
    float display_scale_ = 1;
    bool show_prefs_ = false, show_help_ = false, show_welcome_ = false;
    bool show_export_dialog_ = false, show_follow_ = false, show_about_ = false;
    int bvh_prompt_ = 0;  // BVH export waiting for the loss choice: 1 animated bones, 2 all bones
    bool bvh_confirmed_ = false;
    std::vector<std::string> bvh_lost_;
    int follow_f0_ = 0, follow_f1_ = 30;
    bool follow_keep_offset_ = true;
    std::string status_ = "Ready";
    float status_tail_w_ = 0;  // the status bar's chips after the message, last frame: the message leaves them room
    std::string message_title_, message_text_;
    std::string bone_filter_;
    int fps_edit_ = 30;            // the frame-rate field's value until the change is confirmed
    bool fps_edit_active_ = false;
    bool props_nothing_ = false;  // Properties had nothing selected last frame (Animation opens as that starts)
    int bones_seen_primary_ = -1;  // reveal a bone selected elsewhere once (UI-23)
    bool bones_clicked_ = false;   // the selection came from the tree itself: no scroll
    // The bone filter's best hit, outlined in the tree and scrolled to once as the text changes; Enter selects it.
    std::string bone_filter_seen_;
    int bone_filter_best_ = -1;
    bool bone_filter_scroll_ = false;
    bool quit_ = false;
    bool skip_shortcuts_ = false;
    bool menu_open_ = false;  // a menu or other non-modal popup was open at the end of the last frame
    std::string pending_tab_;           // --tab: focused once the panels exist
    std::string inv_scroll_to_;         // --tab poses or props: the Inventory section to scroll to once laid out
    // Undo/Redo pressed in a text field: the field is released first and commits its own step (UI-15).
    const char* deferred_action_ = nullptr;
    int deferred_frames_ = 0;
    bool headless_ = false;  // a key already handled this frame (e.g. Esc cancelling a drag)
    bool first_frame_ = true;
    bool reset_layout_ = false;  // View > Reset Layout: the default dock layout at the next frame

    // --- props ---
    // Prop library (06 section 4.2, 03 section 3.6), thumbnails (04 VP-90) and Inventory drops (VP-83).
    std::string library_dir() const;  // the folder of library_path(), with a trailing '/'
    void load_prop_libraries();  // from load_library()
    void save_prop_library();
    std::string add_to_prop_library(const Prop& p);  // after an import (IO-39); returns the item id
    // Adds a library prop to the scene; with keep_offset the item's stored parent, position and rotation
    // are used, otherwise bone/point with a zero offset.
    void add_library_prop(const PropLibraryItem& it, const std::string& bone, const std::string& point, bool keep_offset);
    const PropLibraryItem* find_prop_item(const std::string& id) const;
    // globals/shape/world: another actor's pose and placement (GR); dim darkens it. Defaults: the active actor.
    void draw_prop(const Prop& p, std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices,
                   const std::vector<Xform>* globals = nullptr, const Shape* shape = nullptr, float dim = 1.f,
                   const Xform& world = {}, const float* tint = nullptr,  // tint: RGBA, drawn see-through (ghosts)
                   const std::vector<float>* heat = nullptr);    // per vertex: a colour and how much of it (weight_heat)
    bool render_thumbnail(const Prop& p, const std::string& png);
    ImTextureID prop_thumbnail(const PropLibraryItem& it);  // 0 until ready or when it can't be made
    void draw_prop_grid(std::vector<PropLibraryItem>& items, bool user);
    void viewport_drop_target(ImVec2 origin, ImVec2 size);  // called by draw_viewport after viewport_input
    std::vector<PropLibraryItem> prop_library_, starter_props_;
    std::map<std::string, ImTextureID> thumbs_;  // item id -> texture

    // --- viewport ---
    // The Scale tool (VP-40, props only). ponytail: stands in for Tool::Scale until the enum gains it.
    Tool last_effective_tool_ = Tool::Rotate;  // shows the "bones never scale" hint on the switch to Scale
    // VP-26: pressing a bone with the Rotate tool, then dragging, turns it about the view axis.
    int bone_drag_ = -1;
    bool bone_drag_started_ = false;
    ImVec2 bone_drag_press_;
    // Auto IK (spec 08 AI): a Move drag of a bone (the gizmo, Blender's G, or its joint dot with any tool) turns the
    // bones above it by IK and keys them. auto_ik_ is set at the drag's start (capture_edit_start) when it applies.
    struct AutoIkDrag {
        bool on = false;
        int node = -1;
        AutoIkChain chain;
        Evaluation start;  // the pose at the press; its pose then follows each step's solve
        std::optional<Vec3> grab;  // FP-2: the body point pressed, in world space at the press
        Vec3 from;                 // what the pointer drags: the grabbed point or the joint's head
    };
    void auto_ik_set_chain(int len);
    std::optional<Vec3> hover_grab_;
    bool draw_suggest_limits_was_open_ = false;
    const RigConstraints* edited_constraints() const;
    std::string limit_revealed_joint_;  // JL: the joint the Suggest Limits list last scrolled to  // JL: focus the Suggest Limits window when it opens  // FP-2: the body point under the cursor when hover_bone_ came from the mesh
    AutoIkDrag auto_ik_;
    BodyDrag body_drag_;
    bool body_drag_on_ = false;
    std::unique_ptr<FollowThrough> follow_through_;
    std::chrono::steady_clock::time_point follow_through_last_time_;
    void apply_follow_through_preview(Evaluation& e);
    std::map<int, int> auto_ik_length_;  // chain lengths changed during drags, per joint, for this run
    bool auto_ik_applies(int node) const;  // on, and node has a chain now
    void auto_ik_begin(int node);
    bool auto_ik_input();  // the wheel and [ ] during a drag: a longer or shorter chain; true when it took them
    void auto_ik_status();
    int dot_hover_ = -1;  // the joint dot under the pointer (a bone with an Auto IK chain)
    int dot_drag_ = -1;   // the joint dot pressed, dragged in the view plane once it moves 4 px
    bool dot_drag_started_ = false;
    ImVec2 dot_drag_press_;
    int pick_dot(ImVec2 mouse) const;
    Vec3 rig_euler_ref_;  // Properties' rotation in rig axes: the angles last shown, so they never flip
    int rig_euler_node_ = -1;
    // Box selection (ui/box_select.h): a left drag from empty space, or Blender's B then a drag. box_hits_ are the
    // nodes inside while it runs, highlighted live.
    bool box_ = false, box_moved_ = false, box_from_b_ = false, box_click_clears_ = false, box_armed_ = false;
    ImVec2 box_press_;
    std::vector<int> box_hits_;
    void start_box(ImVec2 m, bool from_b, bool click_clears);
    bool box_input(ImVec2 m, bool hovered);  // true while the box (or B, armed) owns the mouse this frame
    // Bind to... (Tools, a bone's right-click menu): the bone to pin waits for a click on the bone it rides, in the view.
    int bind_pick_ = -1;
    bool bind_pick_input(bool hovered);  // true while it owns the mouse this frame
    bool hot(int node) const {  // hovered, or inside a running box
        return node == hover_bone_ || std::find(box_hits_.begin(), box_hits_.end(), node) != box_hits_.end();
    }
    // The Local gizmo frame of a bone: its global rotation x the display bone frame (SK-21), or a mesh body's rig axes.
    Quat local_axes(int node) const { return globals_[node].rot * skel_.bone_axes(node, shape()); }
    std::string bone_style_run_;                // --bones
    bool bones_hidden() const;                  // View > Bones > Style, Hidden
    bool stick_bones() const;                   // View > Bones > Style, Stick (default true unless hidden)
    std::vector<StickSegment> sticks() const;   // the shown joints' sticks at the view's pose
    void draw_stick_bones(ImDrawList* dl);
    void draw_collision_volumes(std::vector<Vertex>& verts);  // VP-10, SK-I5
    // RM-10 weight glow: the joint whose skin weights tint the shown body (the hovered one, else the selected; -1 for
    // none), and per vertex of a body part ("" for the Linden body) a heat colour and how much of it shows (4 floats).
    // Made again only when the joint, the body or its weights change.
    int glow_joint() const;
    const std::vector<float>* weight_heat(const std::string& part);
    std::map<std::string, std::vector<float>> heat_;
    std::string heat_key_;
    unsigned weights_generation_ = 0;  // bumped when a body's weights change in place (the share slider)
    // RM-10 Share (soft_body_ui.cpp): Properties' "Share with <joint>" for a soft-body volume, on the shown body's parts
    // that load through a mapping. Each part's weights without the volume's share are kept while it is selected, so a
    // drag shares them afresh every frame; the value goes in the mapping on release, one undo step per drag (kept
    // beside the project's steps, as the joint-limit suggestions' are).
    struct SharePart {
        std::string path;
        RigMap map;
        std::vector<int> joints;  // the weights as the rig splits them (this volume's share left out)
        std::vector<float> weights;
    };
    struct ShareState {
        std::string key;  // the body, the volume and its parts' models
        int volume = -1;  // SK-40
        std::string partner;
        std::vector<SharePart> parts;
    };
    ShareState share_;
    struct ShareStep {
        std::string body, volume;
        double before = 0.5, after = 0.5;
        size_t depth = 0;
    };
    std::vector<ShareStep> share_undo_, share_redo_;
    double share_drag_before_ = 0.5;
    float share_value_ = 0.5f;
    bool share_dragging_ = false;
    bool share_ready(int node);  // share_ for this volume on the shown body; false when no part shares it
    void apply_share(double value, bool save);
    bool undo_share(bool redo);
    bool can_undo_share(bool redo) const {
        const std::vector<ShareStep>& s = redo ? share_redo_ : share_undo_;
        return !s.empty() && s.back().depth == doc_.history.undo_steps().size();
    }
    void draw_share_slider(int node, float label_w);

    // --- formats/viewport ---
    // VP-84 / VP-85: Copy / Paste with a static prop selected; true when they handled it.
    bool prop_sl_copy();
    bool prop_sl_paste();
    void draw_prop_sl_popup();  // from draw_viewport
    std::string prop_sl_string(const Prop& p, int what);  // 0 position, 1 rotation, 2 size
    int prop_sl_popup_ = 0;  // 1 copy chooser, 2 paste chooser
    Vec3 prop_sl_value_;
    // Thumbnails: props (VP-90) and poses (VP-I12), queued a few per frame, cached as PNGs in the library folder.
    bool render_pose_thumbnail(const LibraryItem& it, const std::string& png);
    ImTextureID thumbnail(const std::string& key, const std::string& png, const std::function<bool()>& render);
    static std::string thumb_png(const std::string& stem);  // stem + the thumbnail version + ".png"
    void forget_thumbnail(const std::string& key);
    ImTextureID pose_thumbnail(const LibraryItem& it);  // 0 until ready or when there is no body
    void draw_library_icon(ImDrawList* dl, ImVec2 at, float size, const LibraryItem& it);  // VP-91 / VP-I12
    bool library_row(const LibraryItem& it, const std::string& label);
    int pick_node(ImVec2 mouse, std::vector<int>* ranked, bool with_points) const;
    std::vector<std::string> missing_prop_meshes();  // IO-42
    // view: the 3D view (its skinned positions are kept for picking) rather than a thumbnail.
    void draw_avatar(bool view, const std::vector<Xform>& globals, const SceneColours& colours);
public:
    void free_thumbnails();  // the host's textures, before the host goes (App::shutdown)

private:
    // --- dynamics ---
    // Spec 08 DY: the Dynamics window and the live preview while playing (dynamics_ui.cpp).
    void draw_dynamics_panel();
    void apply_dynamics_preview(Evaluation& e);  // from evaluate(), before e is moved
    bool show_dynamics_ = false;
    bool dyn_preview_ = true;
    int dyn_selected_ = -1;
    std::unique_ptr<DynSim> dyn_sim_;
    std::vector<DynChain> dyn_chains_;  // the chains dyn_sim_ was built for
    const Shape* dyn_shape_ = nullptr;  // and the body shape
    bool spare_match_loop_ = true;      // RM-8 follow-through: a looping clip ends where it starts
    std::map<std::string, std::string> spare_kind_;  // RM-8: per spare slot, the motion preset picked
    void draw_spare_follow_through();   // the Dynamics window's one-click bake for the body's spare chains
    double dyn_last_frame_ = 0;
    // RM-10 avatar physics (soft_body_ui.cpp): SL's bounce on BELLY, BUTT and the PECs, previewed while playing,
    // scrubbing or dragging with the settings below (saved in settings_.physics), and baked on request.
    AvatarPhysics physics_;
    bool physics_loaded_ = false;
    std::string physics_preset_ = "Natural";  // the preset last picked, "" once edited by hand
    AvatarPhysics& avatar_physics();
    void save_avatar_physics();
    std::vector<std::string> physics_volumes() const;  // the ones the shown body uses (all four on the Linden body)
    void apply_avatar_physics_preview(Evaluation& e);
    void draw_avatar_physics();  // the Dynamics window's section
    std::unique_ptr<DynSim> physics_sim_;
    std::vector<DynChain> physics_chains_;
    const Shape* physics_shape_ = nullptr;
    double physics_frame_ = 0;
    unsigned physics_doc_ = ~0u;
    std::chrono::steady_clock::time_point physics_time_;
    bool physics_settled_ = true;

    // --- idle / overlap ---
    // Spec 08 IL: Tools > Idle Layer... and its live preview while playing (idle_ui.cpp); spec 08 OV:
    // Tools > Overlap... (overlap_ui.cpp).
    void draw_idle_panel();
    void apply_idle_preview(Evaluation& e);  // from evaluate(), before the dynamics preview
    bool show_idle_ = false;
    bool idle_preview_ = true;
    int idle_selected_ = -1;
    void draw_overlap_panel();
    bool show_overlap_ = false;
    int overlap_length_ = 3;  // bones in the chain from the selected one
    OverlapSettings overlap_;

    // --- simplify curves / motion quality ---
    // Spec 08 SC: Edit > Simplify Curves..., a dialog whose live preview is an open history step (simplify_ui.cpp);
    // spec 08 MQ: Tools > Motion Quality..., the last clean-up step's numbers before and after (motion_quality_ui.cpp).
    void open_simplify();
    void preview_simplify();
    void draw_simplify_dialog();
    bool simplify_open_ = false, simplify_all_ = false, simplify_planted_ = false;
    Clip simplify_before_;
    std::vector<std::string> simplify_tracks_;  // the selected tracks when the dialog opened
    std::vector<int> simplify_contacts_;        // frames where a foot plants or lifts in simplify_before_
    SimplifyOptions simplify_;
    SimplifyResult simplify_result_;
    float simplify_dim_ = 0;  // the style's modal dimming, put back on close
    void draw_quality_panel();
    bool show_quality_ = false;
    unsigned long long quality_serial_ = ~0ull;  // History::serial() when last measured
    std::string quality_label_;                  // the clean-up step shown; "" = none, quality_[1] is the clip now
    Clip quality_after_;
    MotionQuality quality_[2];                   // before, after
    // --- balance, jump arc, reach (spec 08 section 21, balance_ui.cpp) ---
    void draw_balance(ImDrawList* dl) const;  // View > Centre of Mass
    void draw_auto_balance_panel();           // Tools > Auto-Balance...
    void draw_jump_arc_panel();               // Tools > Jump Arc...
    void draw_ik_target_properties(int limb);  // Properties > Bone with an IK target selected: its Pull
    bool reach_after_drag(int limb);           // a released target drag pulls the spine and hips (RC-1); true if it did
    bool show_com_ = true;
    bool show_auto_balance_ = false;
    bool show_jump_arc_ = false;
    AutoBalanceOptions balance_;
    JumpArcOptions jump_;

    using LimitPreviewMode = LimitPreview;

    bool show_suggest_limits_ = false;
    bool show_tool_search_ = false, tool_search_focus_ = false;  // Find a Tool (F3)
    bool show_undo_history_ = false;  // Edit > Undo History
    void draw_undo_history();
    void go_to_history_step(size_t undo_count);  // undo or redo until that many steps are done
    std::string tool_search_;
    int tool_search_sel_ = 0;
    void draw_tool_search();
    struct ToolHit {
        std::string label, key, why;  // why: the reason it is unavailable now, or ""
        int action = -1;              // index into actions_
        const char* window = nullptr;  // or a tool window
        std::string path;             // the menu it is in ("Tools > Loop Tools"), "" when none is known
        bool menu = false;            // or a menu item that is neither: run through a menu crawl (icon_button.h)
    };
    std::vector<ToolHit> tool_hits(const std::string& query) const;
    void run_tool_hit(const ToolHit& hit);
    std::string menu_search_, menu_search_menu_;  // the typed search and the menu it was typed in
    int menu_search_sel_ = 0;
    bool menu_search(const char* menu);
    bool begin_top_menu(const char* label);  // a menu bar menu, with the search at its top
    SuggestLimitsOptions suggest_options_;
    std::vector<SuggestedLimitRow> suggested_limits_;
    PendingLimits pending_limits_;  // the suggestions under review, for one body
    LimitPreviewMode limit_preview_mode_ = LimitPreviewMode::Suggested;
    std::unordered_set<std::string> selected_limit_joints_;
    std::unordered_map<std::string, bool> joint_limit_checked_;
    std::array<bool, kLimitGroupCount> group_checked_ = {true, true, true, true, true, true, true, true, true, true, false, true};
    std::string limit_search_filter_;
    bool filter_low_confidence_ = false;
    bool filter_changed_ = false;
    bool filter_not_applied_ = false;
    std::string last_clicked_limit_joint_;
    float limit_actions_h_ = 0;  // the height of Suggest Limits' apply buttons, for the list above them

    ClampReport last_clamp_report_;
    double clamp_flash_time_ = -1.0;
    void report_limit_clamp(const ClampedJoint& clamped);
    void report_limit_clamp(const ClampReport& report);

    int test_sweep_node_ = -1;
    double test_sweep_time_ = 0.0;
    double test_sweep_duration_ = 1.6;
    Quat test_sweep_orig_rot_;
    void update_joint_limit_test_sweep(double dt);

    void focus_on_joint_limit(const std::string& joint);
    void start_joint_limit_test(const std::string& joint);
    void reset_joint_limit_to_suggestion(const std::string& joint);
    void apply_selected_limits();
    void apply_group_limits(LimitGroup group);
    void apply_all_limits();
    void discard_suggested_limits();
    bool editing_pending_limits() const;  // the limit tools edit the suggestions, not the applied limits
    bool can_undo_pending_limits(bool redo) const;
    bool undo_pending_limits(bool redo);  // Ctrl+Z / Ctrl+Y on an edit to the suggestions; false if it isn't theirs
    void record_pending_limit_edit(RigConstraints before);
    void select_other_side_limits();
    int count_pending_changes(LimitGroup group) const;
    int count_all_pending_changes() const;
    int count_selected_pending_changes() const;
    bool joint_limit_is_checked(const std::string& joint) const;
    void set_joint_limit_checked(const std::string& joint, bool checked);
    void set_group_checked(LimitGroup group, bool checked);

    void draw_suggest_limits_panel();
    void draw_joint_limit_viewport(ImDrawList* dl) const;
    void draw_joint_limit_badges(ImDrawList* dl) const;
    bool has_limit_badge(int node) const;  // the Bones list shows a limit dot for it
    void draw_bone_limit_badge(int node, ImDrawList* dl, ImVec2 pos) const;
    void draw_joint_limits_section(int node);
    void set_joint_limit_from_pose(int node);
    void track_limit_edit(const char* label);
    bool limit_drag_open_ = false;
    std::unordered_map<std::string, RigConstraints> limit_drag_before_;

    enum class LimitHandle {
        None,
        HingeMin,
        HingeMax,
        HingeAxis,
        ConeRim,
        ConeEdge,
        TwistMin,
        TwistMax
    };

    bool edit_limits_mode_ = false;
    bool mirror_limits_ = true;
    LimitHandle hover_limit_handle_ = LimitHandle::None;
    LimitHandle drag_limit_handle_ = LimitHandle::None;
    ImVec2 limit_handle_press_{0, 0};
    LimitDragSnapshot limit_drag_snapshot_;
    JointLimit limit_drag_start_;  // the limit as it was at the handle press: drags change it relative to there

    void toggle_edit_limits();
    LimitHandle pick_limit_handle(ImVec2 m) const;
    void start_limit_handle_drag(LimitHandle handle, ImVec2 m);
    void update_limit_handle_drag(ImVec2 m);
    void finish_limit_handle_drag();
    void cancel_limit_handle_drag();
public:
    void open_suggest_limits();
    void suggest_limits();
private:

    // --- retarget ---
    // Spec 07: File > Import Animation (Retarget)... and its mapping/report dialog (retarget_ui.cpp).
public:
    void open_retarget(const std::string& path);  // reads the file, guesses the rig, opens the dialog (also --retarget)
private:
    void draw_retarget_dialog();
    void draw_retarget_split(struct RetargetUi& ui);  // RT-10.4 split / trim
    void split_into_parts(const Clip& clip, const std::string& source, const FitOptions& fit, bool overwrite,
                          std::string& confirm, const std::string& advice);  // RT-10.4, also Fit to 250 KB's fallback
    std::string save_parts(std::vector<Clip> parts, const std::string& source, bool overwrite, std::string& confirm);
    std::shared_ptr<struct RetargetUi> retarget_ui_;  // defined in retarget_ui.cpp
    std::shared_ptr<struct HelpUi> help_ui_;          // defined in help_ui.cpp
    std::vector<RigTable> retarget_tables() const;    // <data>/retarget, then the user's saved mappings (RT-12)
    // File > Batch Retarget Folder... (batch_retarget_ui.cpp; spec 07 RT-13, RT-14).
    void draw_batch_retarget();
    bool show_batch_retarget_ = false;
    std::shared_ptr<struct BatchUi> batch_ui_;  // defined in batch_retarget_ui.cpp

    // --- priority planner (priority_plan_ui.cpp; spec 08 PP) ---
    void draw_planner_panel();  // Tools > Priority Planner...; call every frame
    bool show_planner_ = false;
    std::shared_ptr<struct PlannerUi> planner_ui_;  // defined in priority_plan_ui.cpp
    // PP-2 tint: the winning clip's colour for a node while the planner shows one; false = leave c alone.
    bool planner_colour(int node, Rgb& c) const;
    void planner_row_mark(int node);                              // Bones list: after the node's row
    void draw_planner_band(ImDrawList* dl, ImVec2 a, ImVec2 b);  // Timeline: the primary bone's winner

    // --- mocap ---
    // Spec 08 MC: Tools > Motion Capture... (VMC over UDP, mocap_ui.cpp).
    void draw_mocap_panel();                  // also polls the socket and records: call every frame
    void apply_mocap_preview(Evaluation& e);  // from evaluate(), after the dynamics preview
    bool mocap_busy() const;                  // listening: keep frames coming (busy())
    bool show_mocap_ = false;
    std::shared_ptr<struct MocapUi> mocap_ui_;  // defined in mocap_ui.cpp

    // --- ragdoll ---
    // Spec 08 RD: Tools > Ragdoll... (ragdoll_ui.cpp): settings, a simulated preview to scrub, bake.
    void draw_ragdoll_panel();
    void apply_ragdoll_preview(Evaluation& e);  // from evaluate(): shows the simulated preview frames
    bool show_ragdoll_ = false;
    std::vector<Pose> rd_frames_;               // the last simulated preview, frames 0..end
    std::optional<Ragdoll> rd_for_;             // the settings and tracks that preview was made from
    std::map<std::string, Track> rd_curves_for_;
    // --- face --- (face_ui.cpp, spec 08 FA)
    // Tools > Face...: expression sliders, the blink/saccade/look-at layer and the look-at tool.
    void draw_face_panel();
    bool show_face_ = false;
    std::shared_ptr<struct FaceUi> face_ui_;  // defined in face_ui.cpp
    std::string face_heads_dir() const;       // <user>faces/, where per-head tables live; "" = none
    // The face table of a head: "" = the default head (data/retarget/face-arkit.json), else <heads dir><head>.json.
    bool load_face_table(const std::string& head, FaceTable& out, std::string& err) const;
    // Shared with Motion Capture (mocap_ui.cpp keeps them in its settings): Move face bones and the head.
    bool face_positions();
    void set_face_positions(bool on);
    std::string face_head();
    void set_face_head(const std::string& head);
    // Where a look-at target is at each frame, in the active actor's space; empty when it cannot be found.
    LookTarget look_target(const FaceLayer& target);
    void look_at_partner(int actor);  // Actors: the head and eyes look at another actor's eyes, the whole clip
    // --- expression packs and lip sync --- (expression_pack_ui.cpp, lip_sync_ui.cpp; spec 08 EX, LS)
    // The Face window's Export Expression Pack... dialog (opened by its button), with the window's head and Move face bones.
    void draw_expression_pack(const FaceTable& table, bool positions);
    void export_expression_pack(const std::string& folder);  // the folder chosen for it
    std::shared_ptr<struct ExpressionPackUi> pack_ui_;       // defined in expression_pack_ui.cpp
    void draw_lip_sync(bool positions);                      // the Face window's Lip Sync section
    void import_rhubarb(const std::string& path);
    bool lip_tables(std::string& err);  // loads the head's face table and data/retarget/lip-shapes.json into lip_ui_
    // The mouth shapes on the timeline between y0 and y1: drawn, and dragged to nudge them. True while a drag owns
    // the timeline's press.
    bool lip_sync_timeline(ImDrawList* dl, float x0, float x1, float y0, float y1, int last, bool hovered);
    std::shared_ptr<struct LipSyncUi> lip_ui_;  // defined in lip_sync_ui.cpp

    // --- clips --- (clips_ui.cpp, spec 08 CL): several named clips, Export All Clips and the AO notecards.
    bool show_clips_ = false;
    void draw_clips_panel();
    void clip_edit(const std::string& label, const std::function<void(Project&)>& change);  // scene_edit, stops playback
    std::string ao_notecard_text(int fmt, std::vector<std::string>* warnings = nullptr);  // 0 Firestorm, 1 ZHAO-II
    void save_ao_notecard(const std::string& path);  // Save as .txt, the format in ao_format_
    int ao_format_ = 0;
    int clip_renaming_ = -1;  // the clip whose name is being typed
    char clip_name_buf_[64] = {};

    // --- groups --- (actors_ui.cpp, spec 08 GR)
    bool show_actors_ = false;
    void draw_actors_panel();
    bool multi_actor() const { return doc_.project.actors.size() >= 2; }
    // Your avatar is the first actor (in the viewer, the worn avatar plays its animation). The one edited may be another.
    bool editing_other() const { return multi_actor() && doc_.project.active != 0; }
    // Per-actor files (GR-6): load an animation into an actor (-1: a new actor), save or export one actor alone.
    std::string file_actor_;  // the actor a LoadActor, SaveActor or ExportActor dialog is for, by name ("" = a new one)
    int actor_index(const std::string& name) const;  // -1 when there is no such actor
    void load_actor_file(const std::string& actor, const std::string& path);
    void put_clip_in_actor(const std::string& actor, const std::string& path, Clip clip, const Actor* from);
    void save_actor(const std::string& actor, const std::string& path, bool anim);
    void actor_file_menu(int i, bool load);  // actor i's Load Animation... (load) and Save/Export items
    void draw_actor_body(int i, const std::vector<Xform>& globals, const SceneColours& colours, bool edited);
    Xform actor_rel(int i) const;              // actor i's placement in the active actor's space
    std::string actor_body_key(int i) const;   // body id or "mesh:<id>"; "" in the file (None) = the view's, for its shape
    void sync_active_body();                   // the view shows the active actor's own Linden body
    const Shape* actor_shape(int i) const;     // the shape actor i evaluates with
    ExternalTarget actor_resolver(int self);   // GR-4 pin targets as seen by actor `self`
    Evaluation evaluate_actor(int i, double frame);
    void activate_actor(int i);                // make i the active actor; the view stays put
    void scene_edit(const std::string& label, const std::function<void(Project&)>& change);  // one undo step
    void apply_restore(History::Restore r);    // after undo/redo
    void draw_other_actors(const SceneColours& colours);
    int pick_actor(ImVec2 m) const;            // another actor's body under the cursor, or -1
    // lifts: per height variant (HV), each actor's export_hip_lift, listed in the note with the variant's lines.
    void write_sit_note(const std::string& folder, const std::string& stem,
                        const std::map<double, std::vector<double>>& lifts = {});
    // Sit-system lines (GR-2, ui/sit_export_ui.cpp): 0 AVsitter2 AVpos, 1 nPose V4, for every actor. For a height
    // variant (HV): its file names, each actor raised by lift[actor].
    std::string sit_lines(int fmt, double height = 0, const std::vector<double>& lift = {}) const;
    void draw_sit_export();                  // the Actors panel's section
    void save_sit_lines(const std::string& path);  // Save as .txt, the format in sit_format_
    int sit_format_ = 0;
    bool sit_scroll_ = false;  // --window sit: the Actors window scrolls to the sit section once
    std::vector<std::vector<float>> actor_pick_pos_;  // skinned positions of the other actors (world, active space)
    std::vector<const std::vector<std::uint32_t>*> actor_pick_idx_;
    std::map<std::string, std::unique_ptr<AvatarMesh>> actor_meshes_;  // Linden bodies other actors use
    int pin_actor_ = -1, pin_bone_ = -1;       // the actors panel's "Bind to" choice
    std::string pin_bone_filter_;              // and its bone list's filter
    // Other actors with body "Skeleton Only": their globals (active space) and colour, drawn as bones.
    struct OtherSkeleton {
        std::vector<Xform> globals;
        std::array<float, 3> colour;
        int actor;
        bool drawn = true;  // false: your avatar in the world view, picked by its bones but drawn by the host
    };
    std::vector<OtherSkeleton> other_skeletons_;
    // Its sticks, as the edited actor's are drawn: the groups View > Bones shows, but not the edited body's Hide Unused
    // Bones set, which says nothing about another actor's body.
    std::vector<StickSegment> other_sticks(const OtherSkeleton& s) const;
    // The view's placement gizmo on another actor (the "place" button).
    Gizmo actor_gizmo_;
    int place_actor_ = -1;
    bool actor_dragging_ = false, actor_drag_rotate_ = false;
    Gizmo::Part actor_gizmo_hover_ = Gizmo::None;
    Actor actor_drag_start_;
    // The Actors panel's placement fields and colour picker edit the active actor live; the change becomes
    // one undo step on release. While any of these run, undo, redo and actor switches wait (scene_busy()).
    int field_drag_actor_ = -1, colour_actor_ = -1;
    Actor field_drag_start_;
    std::array<float, 3> colour_start_{};
    void finish_scene_drags();
    bool scene_busy() const { return actor_dragging_ || field_drag_actor_ >= 0 || colour_actor_ >= 0; }
    bool place_actor_gizmo();                          // places it for this frame; false when not shown
    bool actor_gizmo_input(ImVec2 mouse, bool hovered);  // true when it took the mouse
    // --- onion + loop tools (loop_ui.cpp, viewport.cpp; spec 08 ON, LP) ---
    struct OnionView {
        bool on = false, bones_only = false;
        OnionSettings s;
    };
    OnionView onion_view() const;
    void set_onion_view(const OnionView& v);
    void draw_onion_settings();
    void draw_onion(const SceneColours& colours);
    std::vector<OnionGhost> ghost_poses();  // onion ghosts plus the Filter Curves pre-filter pose (viewport.cpp)
    void draw_ghost(const std::vector<Xform>& globals, const Rgb& colour, float alpha, bool bones);
    void draw_loop_tools_menu();
    void draw_loop_seam_mark(ImDrawList* dl, float x_out, float y, bool hovered);
    int loop_blend_ = kBlendWholeLoop;
    float loop_travel_ = 1.f;
    // --- Clean Up Foot Sliding (footlock_ui.cpp; spec 07 RT-9, 08 FC) ---
    void draw_foot_lock_window();
    bool show_foot_lock_ = false, foot_heel_toe_ = true, foot_to_ground_ = false;
    Clip foot_measured_;  // the clip foot_ground_ was measured on
    bool foot_measured_heel_toe_ = true;
    int foot_measured_legs_ = -1;
    double foot_ground_ = 0;  // m above the rest floor
    std::vector<std::string> foot_report_;
    // --- pinned ghosts (pinned_ghosts_ui.cpp; spec 08 ON-5): a view aid, not saved and not undone ---
    struct PinnedGhost {
        std::string label;
        std::string actor;                // whose frame: an actor's name, "" = the active actor
        double frame = 0;
        std::optional<LibraryItem> pose;  // instead: this library pose on the active actor at the current frame
    };
    std::vector<PinnedGhost> pinned_ghosts_;
    void pin_ghost(int actor, double frame);
    void pin_pose_ghost(const LibraryItem& pose);
    std::vector<std::vector<Xform>> pinned_ghost_poses();  // in the active actor's space
    void draw_pinned_ghost_menu();
    // --- the target ghost (pinned_ghosts_ui.cpp): another animation over the avatar, a pose to match by eye ---
    // A view aid like the pinned ghosts: not saved, not undone, kept when another project opens. Frame n of the edited
    // clip shows frame n of the target (target_frame), and it plays along.
    // A multi-actor target shows every actor at its own placement: the one named as the actor you edit (else the
    // target's active one) over yours, the others where they stand from it.
    struct TargetGhost {
        std::string name;  // the file's name, for the status bar
        struct Actor {
            std::string name;  // "" for a one-actor target
            Clip clip;         // a .vat's clip of this actor, or an imported .anim
            Xform place;       // its placement in the target's scene
        };
        std::vector<Actor> actors;
        int active = 0;  // the target's own active actor
    };
    std::optional<TargetGhost> target_;
    bool target_on_ = false;
    float target_opacity_ = 0.35f;
    std::vector<Xform> target_globals_;  // the matching actor's pose at this frame while shown (evaluate), else empty
    std::vector<std::vector<Xform>> target_others_;  // the target's other actors, in the view's space
    int target_main_ = 0;                            // the target actor target_globals_ is
    int target_actor_for_view() const;               // which target actor matches the one you edit
    std::vector<std::pair<const Clip*, const std::vector<Xform>*>> target_ghosts() const;  // each shown, and its clip
    void load_target(const std::string& path);  // app.cpp: a .vat or .anim, shown; a message when it cannot be read
    void draw_target(const SceneColours& colours);  // the see-through body and its props
    void draw_target_bones(ImDrawList* dl);         // thin lines over the view, for the shown bone groups
    void draw_target_menu();                        // View > Target Ghost
    void draw_target_status();                      // the status bar's chip and the selected bone's distance
    void resolve_clip_paths(Clip& c, const std::string& dir);  // a loaded project's props, audio and reference files
    // --- pose-matched insertion and transitions (pose_match_ui.cpp; spec 08 PM) ---
    void open_match_poses(Clip incoming, const std::string& name);  // offers to join a clip where the poses match
    void draw_match_poses_window();
    void draw_transition_window();
    struct MatchState {
        Clip incoming;
        std::string name;
        MatchOptions o;
        PoseMatch m;
        bool stale = true;
    };
    MatchState match_;
    struct TransitionState {
        int from_pose = 0;  // 0 from a frame, 1 from a library pose
        int from = 0, to = 10, frames = 10, pose = 0;
        std::optional<EaseShape> ease = EaseShape::Sine;
    };
    TransitionState transition_;
    bool show_match_ = false, show_transition_ = false;
    // --- loop assists (loop_assist_ui.cpp; spec 08 LP-5..LP-8) ---
    void draw_loop_assist_items();  // the Loop Tools menu's part
    void draw_loop_assist_window();
    void draw_treadmill_menu();
    void draw_treadmill();  // the scrolling grid, through scene_triangles (the app and the viewer)
    void draw_light_menu();  // 08 LT-1, light_ui.cpp
    void draw_backdrop();    // 08 LT-2: a plain wall and floor behind the actor, through scene_triangles
    int light_ = -1;         // the Light menu's preset; -1 = the host's own lighting
    bool backdrop_ = false;
    void loop_assist_tick();  // every frame: loop-aware tangents, the treadmill's travel
    bool show_loop_assist_ = false;
    bool loop_find_now_ = false;  // --window loop-assist: Find as the window opens
    int loop_min_length_ = 20, loop_beats_ = 8;
    std::vector<LoopCandidate> loop_candidates_;
    LoopCandidate loop_current_{0, 0, -1};  // the clip's own loop, scored as they are at the last Find
    bool loop_joins_ = false;               // ... and it joins as well as the best of them
    bool treadmill_on_ = false;
    // --- dope sheet + motion paths (dope_sheet.cpp, motion_path_ui.cpp; spec 08 DS, MP) ---
    void draw_dope_panel();
    bool show_dope_ = true;
    bool keys_hovered() const { return graph_.hovered() || dope_.hovered(); }  // key commands go to the graph's keys
    struct MotionPathView {  // a view setting for this session: no undo step, not saved
        bool on = false, numbers = false;
        MotionPathSettings s;
        std::vector<MotionPath> cache;  // the paths as last evaluated, and for what
        std::vector<int> cache_nodes;
        MotionPathSettings cache_s;
        double cache_frame = -1;
        Clip cache_clip;
        // Phase 2: dragging a keyed dot of an IK limb's end moves the limb's IK target at that frame.
        int drag_limb = -1;
        double drag_frame = 0;
        Vec3 drag_start;      // the dot where the drag began
        Xform drag_target;    // the IK target there
        Clip drag_clip;       // the clip at the press
        bool hover = false;   // over such a dot this frame
    };
    MotionPathView motion_path_;
    std::vector<int> motion_path_nodes() const;
    const std::vector<MotionPath>& motion_paths_now();
    void draw_motion_path_menu();
    void draw_motion_paths(ImDrawList* dl);
    bool motion_path_input(bool hovered);  // before viewport_input; true when it took the mouse
    int treadmill_speed_ = 0;  // index into kSlSpeeds; its size = the custom speed
    float treadmill_custom_ = 1.5f;
    Clip gait_seen_;  // the clip the treadmill last measured, and its gait (measured again when the clip changes)
    Gait gait_;
    bool gait_have_ = false;
    double treadmill_frame_ = 0, treadmill_scroll_ = 0;  // metres the ground has moved, modulo its spacing
    // --- audio track + time editing (audio_track.cpp; spec 08 AU, TE) ---
    void load_audio(const std::string& path);
    void sync_audio_data();
    void queue_audio_at(double frame, double seconds);
    void stop_audio();
    void update_audio();
    double snapped_frame(double frame) const;  // to the nearest beat when the audio snaps (AU-2)
    void draw_audio_lane(ImDrawList* dl, float x0, float x1, float y0, float y1, double last_frame);
    std::vector<std::string> time_edit_tracks() const;  // empty = every track (nothing selected)
    void run_time_edit(const char* label, const std::function<void(Clip&, const std::vector<std::string>&)>& change);
    void draw_time_prompt();
    void add_time_actions(const std::function<void(const char*, Action)>& add);
    void draw_time_menu_items();
    void draw_audio_menu_items();
    AudioData audio_data_;
    std::string audio_loaded_path_;
    bool audio_running_ = false;
    double audio_frame_ = -1;
    KeyRange range_clipboard_;
    int time_prompt_ = 0, time_prompt_value_ = 10;  // 1 insert, 2 stretch
    bool dragging_audio_ = false;
    double audio_press_offset_ = 0, audio_press_frame_ = 0;
    // --- retime markers + split dance at beats (retime_ui.cpp; spec 08 TE-5, TE-6) ---
    void set_retime(bool on);  // off clears the markers
    // The ruler's markers in Retime mode: drawn, double-click adds, a drag retimes. true while a marker drag has
    // the strip (the timeline then skips its own press handling).
    bool retime_timeline(ImDrawList* dl, float x0, float x1, float y0, float y1, int last, bool hovered);
    void draw_split_dance_window();
    void export_dance_parts(std::vector<Clip> parts);
    bool retime_on_ = false, show_split_dance_ = false;
    std::vector<double> retime_markers_, retime_press_markers_;  // transient: never saved
    int retime_drag_ = -1, retime_view_last_ = 1;  // the dragged marker; the timeline's scale held while it moves
    Clip retime_press_clip_;
    std::string split_dance_confirm_;
    // --- project and animation library (file_library_ui.cpp; spec 08 FL) ---
    void draw_file_library();         // the Inventory's Projects and Animations sections
    void rescan_files();              // after a save or export; the Inventory also rescans on focus
    void save_to_library();           // File > Save to Library...
    void insert_anim_file(const std::string& path, bool mirrored);  // a .anim's keys pasted at the frame
    void match_anim_file(const std::string& path);  // the same through Match Poses (08 PM-1)
    Clip library_clip(const LibraryItem& it, bool mirrored) const;  // a clip item alone, from frame 0 (pose_match_ui.cpp)
    // Export's "Also save to Animations library": an exported file, or an upload's bytes, copied in.
    void anim_file_to_library(const std::string& path);
    void anim_to_library(const std::string& file_name, const std::vector<std::uint8_t>& bytes);
    void file_drop(const ImGuiPayload* payload, std::string& hint);  // a VATS_FILE drop on the view
    void draw_file_prompt();          // Rename... and Save to Library...'s name prompt, and Add Folder... answers
    bool inv_match(const std::string& name) const;  // the Inventory's filter box
    std::string inv_filter_;
    int inv_sections_ = 0;  // the Inventory sections drawn this frame (none while filtering: "nothing matches")
    std::shared_ptr<struct FileLibUi> file_lib_;  // defined in file_library_ui.cpp
    // --- tween, pose blend and easing (tween_ui.cpp; spec 08 TW) ---
    void draw_tween_controls(bool compact);  // the timeline bar's Tween slider, and Blend for a while after a pose
    void start_tween();             // the Tween shortcut: a modal drag (Modal::Tween)
    void tween_drag(ImVec2 mouse);  // modal_input's part while Modal::Tween runs
    bool begin_tween();             // opens the undo step; false when nothing selected can be keyed
    void apply_tween();             // re-keys from tween_base_ at tween_pct_
    // After a library pose, hand pose or Paste Pose changed the clip (before = the clip until then): shows
    // the Blend slider (TW-2).
    void offer_pose_blend(Clip before, double frame);
    float tween_pct_ = 50, tween_press_pct_ = 50;
    bool tween_relax_ = false;
    float tween_row_end_ = 0, timeline_row_w_ = 0;  // the timeline bar goes icon-only below timeline_row_w_
    float timeline_tail_w_ = 0;  // Set Key to Relax: wraps to a row of its own when that does not fit
    Clip tween_base_;
    std::vector<std::string> tween_on_;  // the tracks the running tween keys
    struct PoseBlend {
        Clip before, after, shown;  // shown: the clip as the last blend left it
        double frame = 0, until = 0;  // until: ImGui time the slider hides at
        int actor = 0;
        float pct = 100;
    };
    std::optional<PoseBlend> pose_blend_;
    // --- key tags and blocking mode (key_tags_ui.cpp; spec 08 KT) ---
    void draw_blocking_button(bool compact);  // the timeline bar's Blocking toggle
    void draw_key_tag_menu_items();  // Blocking, Convert Blocking to Spline, Tag Keys Here (Edit menu, timeline menu)
    void blocking_after_edit();      // mark_dirty's hook: with Blocking on, the last step's new keys become Stepped
    bool blocking_ = false;
    // --- body picker and selection sets (picker_ui.cpp; spec 08 PK, SS) ---
    void draw_picker_panel();  // the Picker tab beside Bones
    void draw_selection_sets();  // its Selection Sets section
    void load_library_sets();  // selection_sets.json beside the pose library, read once
    void save_library_sets();
    // Selects nodes: mode 0 replaces the selection, 1 adds, 2 removes. Adding shows a hidden group (Bones > Show) first.
    void select_nodes(const std::vector<int>& nodes, int mode);
    int category_size(int category) const;  // the nodes of a Show list row (8 = collision volumes)
    void select_category(int category, int mode);  // the Show list's select button
    std::vector<SelectionSet> library_sets_;
    bool library_sets_loaded_ = false;
    std::shared_ptr<struct PickerUi> picker_ui_;  // defined in picker_ui.cpp
    std::string set_name_;
    bool set_to_library_ = false;
    // The Picker's avatar render gathers the body's triangles (draw_avatar, draw_prop) instead of sending them.
    struct Triangles {
        std::vector<Vertex> verts;
        std::vector<std::uint32_t> indices;
        float gloss = 0;
    };
    std::vector<Triangles>* capture_triangles_ = nullptr;
    void scene_triangles(const std::vector<Vertex>& verts, const std::vector<std::uint32_t>& indices, bool depth_test,
                         float gloss, bool translucent = false) {
        if (capture_triangles_) {
            if (!translucent) capture_triangles_->push_back({verts, indices, gloss});
            return;
        }
        host_.scene_triangles(verts, indices, depth_test, gloss, translucent);
    }
    // --- animation check (lint_ui.cpp; spec 08 CK) ---
    void update_check();       // re-runs the check once the clip is still after an edit (from draw_check_panel)
    void run_check();          // checks the clip now and takes it as seen
    std::string active_ao_state() const;  // the active clip's AO state, "" for none
    void draw_check_panel();   // Tools > Animation Check...; call every frame
    void draw_check_badge();   // the status bar's finding count
    bool contact_bone(int node) const;  // a self-contact finding names node at this frame: tint it (08 SX)
    void draw_contact_marks(ImDrawList* dl, float x0, float x1, float y1, int last, bool hovered) const;  // timeline
    bool show_check_ = false;
    std::shared_ptr<struct CheckUi> check_ui_;  // defined in lint_ui.cpp
    // --- rig export and the joint offset inspector (rig_export_ui.cpp; spec 08 RG-4, RG-5) ---
    void draw_rig_export_window();  // File > Export Rigged Mesh for SL...
    void draw_joint_inspector();    // Rig > Joint Offset Inspector...
    void refresh_rig_check();       // the parts, findings and joint tables of the body shown
    // A fix on the body's part-th file, applied to its source (part_source); the body's shape cache dropped, re-checked.
    void rig_fix_applied(int part, const std::function<void(DaeModel&)>& fix);
    void rig_check_stale();         // a part's mesh changed: check again before the window draws
    void export_rig(const std::string& path);
    double ground_shown() const;    // the ground grid: rest_floor(), or SL's ground under the body while the export shows it
    bool show_rig_export_ = false, show_joint_inspector_ = false;
    std::shared_ptr<struct RigExportUi> rig_ui_;
    // --- rig mapping (rig_map_ui.cpp; spec 08 RM): any rigged model's own bones onto SL's skeleton ---
public:
    void open_rig_map(const std::string& path);  // Rig > Map Rig to Second Life... on a model; --map-rig
private:
    void open_example_mech();                    // a copy of data/bodies/mech in the library, opened for mapping
    void draw_rig_map_window();
    void draw_rig_map_overlay(ImDrawList* dl) const;  // the bone the list hovers or picked, in the view
    void rig_map_preview();                      // the model through the mapping as edited, shown as the mesh body
    void close_rig_map(bool applied);            // the preview goes; the body shown before comes back unless applied
    void apply_rig_map();                        // saves <model>.rigmap.json and makes the model a mesh body
    bool show_rig_map_ = false;
    std::shared_ptr<struct RigMapUi> rig_map_ui_;
    // --- rigging from scratch (rig_scratch_ui.cpp; spec 08 RG-13, RG-14): SL's skeleton placed in a mesh with none ---
public:
    void open_rig_scratch(const std::string& path);  // Rig > Rig a Model from Scratch... on a model; --rig-scratch
    // Command-line helpers for scripted checks: the optional groups ("face,tail"), a marker moved ("wrist_l=x,y,z", as
    // a drag with Mirror on), the weights worked out now on this thread, and Apply.
    bool cli_rig_groups(const std::string& list);
    bool cli_rig_marker(const std::string& spec);
    void cli_rig_weights();
    void cli_rig_apply();
private:
    void open_unrigged_example();                   // the CC0 example mech without its rig, copied to the library
    void draw_rig_scratch_window();
    void draw_rig_scratch_overlay(ImDrawList* dl);  // the markers, named by the SL joints they place
    bool rig_scratch_input(bool hovered);           // a marker's drag; true when it took the mouse
    bool rig_scratch_holds_rest() const;            // the preview stands at rest while the markers show
    void rig_scratch_preview();                     // the model rigged as the markers say, shown as the mesh body
    void close_rig_scratch(bool applied);           // the preview goes; the body shown before comes back unless applied
    void apply_rig_scratch();                       // saves the rig in <model>.rigmap.json and makes the model a mesh body
    void start_rig_weights(bool wait);              // bone heat off the UI thread (wait: on this one)
    void poll_rig_weights();                        // a finished job's weights into the rig and the preview
    bool show_rig_scratch_ = false;
    std::shared_ptr<struct RigScratchUi> rig_scratch_ui_;
    // --- weight painting (weight_paint_ui.cpp; spec 08 RG-15) ---
public:
    // --paint "<joint> <add|subtract|smooth> <x> <y> <z> <radius> <strength>": one stroke of one dab on the shown body,
    // where a brush pressed on it from in front of that rest-pose point lands (mirrored when the Paint Weights window
    // mirrors). False when it cannot paint there.
    bool cli_paint(const std::string& spec);
private:
    void draw_paint_window();                  // Rig > Paint Weights...
    bool paint_input(bool hovered);            // a brush stroke in the view; true when it took the mouse
    void draw_paint_overlay(ImDrawList* dl);   // the brush under the pointer
    bool painting() const;                     // the window is open with Paint on: the glow shows the selected bone
    bool save_painted(const std::string& path, const DaeModel& src, std::string& err);  // into its mapping file
    // The shown mesh body's nearest point under m (display coordinates), in the UI's space: the viewer's alt-cam focus
    // uses it for the body it draws in your avatar's place.
public:
    bool body_point_under(ImVec2 m, Vec3& out) const;
private:
    bool paint_visible_ = false;  // the Paint Weights window drew this frame (not hidden behind another tab)
    void stop_painting(const std::string& why);  // the brush off (the window stays), with a status line
    bool undo_paint(bool redo);                // Ctrl+Z / Ctrl+Y on a stroke; false when the last edit was not one
    void end_paint_stroke();                   // a stroke under way is finished: one undo step, saved in the mapping
    void forget_paint(const std::string& path);  // a model's weights replaced or reloaded: its strokes and caches go ("": all)
    bool can_undo_paint(bool redo) const;
    bool show_paint_ = false;
    std::shared_ptr<struct PaintUi> paint_ui_;
    unsigned paint_generation_ = 0;  // bumped as a mesh is loaded or rebuilt: the brush's caches are made again
    // RM-8 spare-chain presets saved in the library (spare-presets.json), read when first needed.
    std::vector<SparePreset>& spare_presets();
    void save_spare_presets();
    std::vector<SparePreset> spare_presets_;
    bool spare_presets_read_ = false;
    // --- reference picture (reference_ui.cpp; spec 08 RF) ---
    void load_reference(const std::string& path, bool sequence);
    void draw_reference_window();  // View > Reference...
    // Sends the picture through Host::scene_image when it shows as the backdrop (backdrop) or the scene plane; false
    // when the host could not draw it.
    bool draw_reference(bool backdrop, double view_aspect);
    void draw_reference_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size);  // the world view: over it, when the host can't
    ImTextureID reference_texture(int& width, int& height);  // the picture for the playhead, loaded when it changes
    bool show_reference_ = false, reference_pick_sequence_ = false;
    std::shared_ptr<struct ReferenceUi> reference_ui_;  // defined in reference_ui.cpp
    // --- listing media (listing_media_ui.cpp; spec 08 LM) ---
    void draw_listing_window();  // File > Export Listing Media...
    bool show_listing_ = false, listing_png_ = false;  // listing_png_: numbered PNG pictures, else a GIF
    std::shared_ptr<struct ListingUi> listing_ui_;  // defined in listing_media_ui.cpp
    // --- SL preview and upload meter (sl_preview_ui.cpp; spec 08 SP, UM) ---
    // One in-memory export of the active actor, shared by both, remade when the clip's anim_hash changes and nothing
    // is being dragged or typed.
    void sl_export_tick();                 // from frame(), before evaluate()
    void apply_sl_preview(Evaluation& e);  // from evaluate(), last: SL's playback is shown, e's globals the ghost
    void draw_sl_preview_window();         // "As SL Plays It": the deviation table
    void draw_upload_meter();              // Properties > Export
    std::string export_size_text() const;  // "572 bytes", the .anim as measured last; "" before
    bool sl_preview_ = false;              // View > Preview as SL Plays It (not saved)
    std::vector<Xform> sl_ghost_;          // your pose's globals while the preview shows SL's; empty = no preview
    Pose sl_pose_;                         // SL's pose, for Host::drive_avatar while sl_ghost_ is set
    std::shared_ptr<struct SlExport> sl_export_;  // defined in sl_preview_ui.cpp
    // --- the world tools of the viewer (viewer_tools_ui.cpp; spec 09 build 20, items 47, 48, 4 and 46) ---
    void viewer_tools_tick();      // from frame(), before evaluate(): the claims sent, the walk test, the armed click
    void set_in_world(bool on);    // View > As It Plays In-World
    void start_walk_test(int state);  // 0 stops; 1 walk, 2 run
    void draw_in_world_window();   // who wins each joint
    void draw_walk_test_window();
    void draw_seat_section();      // the Actors panel's sit section: the seat you sit on
    bool seat_click(ImVec2 m);     // the viewport's click while Place on Furniture Point is armed; true = taken
    // Moves the selected bone (or IK handle's limb) to p, the edited actor's space: its IK target where it has one, else
    // held there by a world pin. One undo step per call. False with why when nothing could be placed.
    bool place_selected_at(const Vec3& p, std::string& why);
    void settle_on_seat();         // automatic contact: each selected bone straight down onto the seat
    bool in_world_ = false;
    std::shared_ptr<struct InWorldUi> in_world_ui_;  // defined in viewer_tools_ui.cpp
    int walk_test_ = 0;            // 0 off, 1 walk, 2 run
    bool walk_was_in_world_ = false;
    bool seat_pick_ = false;       // Place on Furniture Point: the next click on the world picks the point
    // --- the face cam (face_cam_ui.cpp; spec 09 build 20, item 53) ---
    void draw_face_cam();
    bool mocap_live(Clip& live, std::map<std::string, double>& arkit);  // mocap_ui.cpp
    bool face_cam_ = false;        // View > Face Cam (not saved)
    std::shared_ptr<struct FaceCamUi> face_cam_ui_;  // defined in face_cam_ui.cpp
    // --- posing assists (pose_tools_ui.cpp; spec 08 PT) ---
    bool mirror_live_ = false;  // PT-1: the timeline's Mirror toggle, off at every start
    void mirror_edit(const std::vector<std::string>& tracks);  // after a gizmo, hand-poser or IK edit keyed tracks
    // PT-2: while a scratch session runs, doc_.clip() is the working pose and base the document's; edits record in the
    // session's own history, swapped in for it, so undo steps through the scratch edits.
    struct Scratch {
        Clip base;
        History history;
        double frame = 0;
    };
    bool scratch_on_ = false;           // Edit > Scratch Pose
    std::optional<Scratch> scratch_;
    std::vector<std::string> scratch_marks_;  // the tracks the Bones list marks "(scratch)"
    bool scratch_prompt_ = false, scratch_dont_ask_ = false;
    double scratch_target_ = 0;         // where the scrub waiting on the prompt was going
    bool scratch_changed() const { return !scratch_marks_.empty() || (scratch_ && !(scratch_->base == doc_.clip())); }
    void scratch_tick();                // before evaluate(): sessions follow the toggle, a scrub away asks
    void scratch_end(bool keep);        // keep: the scratch pose's keys as one undo step; else back to the base
    void draw_scratch_prompt();
    void draw_pose_tool_menu_items();   // Edit > Scratch Pose, Propagate Pose
    // Save, export and autosave write the document, never a scratch pose: the base is swapped in for the scope.
    int scratch_aside_ = 0;
    struct ScratchAside {
        App& app;
        explicit ScratchAside(App& a) : app(a) {
            if (app.scratch_aside_++ == 0 && app.scratch_) std::swap(app.doc_.clip(), app.scratch_->base);
        }
        ~ScratchAside() {
            if (--app.scratch_aside_ == 0 && app.scratch_) std::swap(app.doc_.clip(), app.scratch_->base);
        }
    };

public:
    // --- workspaces, a trial (workspace_ui.cpp, workspaces.h) ---
    bool cli_workspace(const std::string& id);  // --workspace: on, and that one; false when unknown
    // --pie <main|more>[/<N|NE|E|...>]: the pie open at the selected bone (or the view's centre), that slot
    // hovered, for screenshots; false when unknown.
    bool cli_pie(const std::string& spec);

private:
    Workspace workspace() const;  // All while workspaces are off
    void switch_workspace(Workspace to, bool on);  // on: workspaces on (the tabs); off is All
    // Tools that are a mode of one workspace: opened elsewhere they go to it; leaving it stops painting (workspace_ui.cpp).
    void follow_tool_workspaces();
    std::array<bool, 3> tools_were_shown_{};
    Workspace last_workspace_ = Workspace::All;
    void set_workspaces_on(bool on);  // the trial's switch: on, back in the workspace last used
    void add_workspace_actions(const std::function<void(const char*, Action)>& add);
    bool panel_shown(const char* window) const;  // the workspace shows this editor panel
    void reveal_panel(const std::string& name);  // show_window(name) of a panel the workspace leaves out: it joins it
    bool toolbar_shows(unsigned button) const { return workspace_def(workspace()).toolbar & button; }
    bool workspace_layout_known() const;  // the first frame's layout is the workspace's own
    void workspace_frame_start();         // draw_dockspace: a switch's layout, before the dockspace is submitted
    void open_workspace_tools(Workspace w);  // the tool windows w docks, opened (and closed again on leaving)
    bool* workspace_tool_flag(const std::string& window);
    void draw_workspace_tabs();   // in the menu bar
    void draw_workspace_menu();   // View > Workspaces
    void draw_export_panel();     // the Export workspace's Export panel
    std::optional<Workspace> ws_switch_from_;  // a switch waiting for the next frame's start
    // View > Maximise Panel: the panel floating over the others, filling the window, and the layout to put back.
    std::string maximised_, maximised_ini_;
    const char* maximised_ini_file_ = nullptr;  // io.IniFilename, held off meanwhile so imgui.ini keeps the layout
    bool restore_maximised_ = false, maximised_placed_ = false;
    void toggle_maximised_panel();
    void end_maximised(bool restore);  // restore: the saved layout back; else just forget it (a new layout replaces it)
    void update_maximised();           // at the frame's start
    std::vector<std::string> ws_opened_;       // tool windows the workspace opened
    FrontTabs ws_front_tabs_;                  // a switched-to layout's front tabs, put back the frame after
    std::optional<Camera> ws_face_camera_;     // the camera before Face framed the face
    bool ws_frame_face_ = false;               // Face entered: frame the face at the next frame's start
    // --- the Tab pie (pie_menu_ui.cpp, pie.h) ---
    void open_pie();       // the pie_menu action: at the pointer over the 3D view, or held open from F3
    void draw_pie_menu();  // from frame(), after the 3D view: claims Tab over it, then runs the pie while open
    void run_pie_slot(const PieSlot& slot);
    bool pie_slot_on(const PieSlot& slot) const;  // a toggle that is on, the current tool
    const char* pie_slot_why(const PieSlot& slot, bool enabled) const;  // why it can't run now, or nullptr
    struct Pie {
        bool open = false, held = false, sticky = false;  // held: by its key; sticky: open until a click
        PieRing ring = PieRing::Main;
        ImVec2 press, centre;  // where it opened, and where it is drawn (kept inside the window)
        double opened_at = 0;
        int bone = -1;  // the bone under the pointer as it opened, -1 for the selection
        ImGuiKey key = ImGuiKey_None;
        int at_bone_frames = 0;  // --pie: centre on the selected bone once the view has drawn
        std::optional<ImVec2> fake_offset;  // --pie: the pointer, for the screenshot's hover
    } pie_;
};

}  // namespace vats
