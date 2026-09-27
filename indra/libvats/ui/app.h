// Viewport Avatar Toolset - the application: document, selection, commands and panels.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gizmo.h"
#include "graph_editor.h"
#include "host.h"
#include "imgui.h"
#include "settings.h"
#include "vats/anim_convert.h"
#include "vats/audio.h"
#include "vats/avatar_mesh.h"
#include "vats/dae.h"
#include "vats/dynamics.h"
#include "vats/history.h"
#include "vats/onion.h"
#include "vats/pose_ops.h"
#include "vats/project.h"
#include "vats/rig.h"
#include "vats/skeleton.h"
#include "vats/time_edit.h"
#include "view_math.h"

namespace vats {

std::string key_label(ImGuiKeyChord chord);  // "Ctrl+Up", "]": shortcut text for menus and tooltips

// Settings ids and menu names, indexed by Body.
inline constexpr int kBodyCount = 5;
inline constexpr const char* kBodyIds[kBodyCount] = {"female", "male", "none", "sl-default", "sl-default-male"};
inline constexpr const char* kBodyNames[kBodyCount] = {"Female", "Male", "Skeleton Only", "SL Default",
                                                       "SL Default (Male)"};

enum class Tool { Select, Move, Rotate, Scale };  // Scale: static props only (VP-40)
enum class Orientation { Local, World, Gimbal };

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
    void open_path(const std::string& path);  // command line and drag-and-drop
    // Command-line helpers for scripted checks: apply a built-in pose ("builtin:<slug>" or its slug) at
    // frame 0 (hand poses on both hands), and frame the selection.
    bool apply_builtin_pose(const std::string& slug);
    void focus_selection() { run_action("frame_selected"); }
    void set_camera_distance(double d) { camera_.distance = d; }
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
    void select_prop(int index) {
        if (index < 0 || index >= int(doc_.clip().props.size())) return;
        clear_selection();
        selected_prop_ = index;
    }
    void show_tab(const std::string& tab) { pending_tab_ = tab == "bones" ? "Bones" : "Inventory"; }  // library = Inventory
    bool set_preset(const std::string& name) {
        if (!preset_from_name(name, settings_.preset)) return false;
        apply_preset();
        if (settings_.preset == Preset::SecondLife) tool_ = Tool::Move;
        return true;
    }
    void fit_graph() {
        GraphContext g = graph_context();
        graph_.frame_all(g);
    }
    void show_points() { show_category_[7] = true; }
    void select_by_name(const std::string& name) {
        int i = skel_.find(name);
        if (i >= 0) select(i, false);
    }
    // True while something animates on its own, so the main loop must not sleep.
    bool busy() const { return playing_ || dragging_gizmo_ || modal_ != Modal::None || cam_anim_t_ >= 0 || cam_keys_held_ || mocap_busy() || cube_alpha_ != (cube_hover_ ? 1.f : 0.5f); }

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
    void load_project_file(const std::string& path);
    void import_file(const std::string& path);
    int anim_bytes(AnimExportResult& r, std::vector<std::uint8_t>& bytes);
    bool export_anim(const std::string& path);
    void upload_now();  // the viewer's direct upload (Host::can_upload): every file Export would write
    void upload_next();  // the next queued upload, after the previous one's confirmation
    std::deque<std::pair<std::string, std::vector<std::uint8_t>>> upload_queue_;  // name, bytes
    bool export_bvh(const std::string& path, bool all_bones);
    // Runs then() now, or after the user has dealt with unsaved changes.
    void guard_unsaved(std::function<void()> then);

    // File dialogs run asynchronously; results come back through this queue.
    enum class Dialog { Open, SaveAs, ImportAnim, ImportBvh, ImportProp, ImportBody, ImportRetarget, LoadAudio, ExportFolder };
    // A dialog's answer, queued for the next frame (hosts may answer on another thread). Several files
    // (body parts) arrive joined with newlines; "" = cancelled.
    ui::FilesChosen dialog_result(Dialog kind, bool folder_of_file = false);
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
    std::string session_id_;
    std::uint64_t autosave_due_ = 0;  // ms (ticks_ns() / 1e6) of the next autosave; 0 = the clock has not started
    struct Recoverable {
        std::string file, original;  // the autosave and the project it came from ("" = never saved)
        long long age_minutes = 0;
    };
    std::vector<Recoverable> recoverable_;  // the last export's bones, length, priority and points (UI-34)
    int export_after_folder_ = -1;  // export waiting for a folder: 0 .anim, 1 BVH, 2 BVH all bones
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
        s.save(host_.paths().settings);
    }
    std::optional<std::pair<std::string, std::string>> session_body_;  // the saved body and mesh body under --body
    std::string nav_hint() const;
    std::string graph_nav_hint() const;
    std::string key_hint(const char* action) const;  // "Home", "Ctrl+E" or "" for the active preset
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
    void draw_preferences();
    void draw_controls_help();
    void draw_welcome();
    void draw_help_browser();
    void help_button(const char* page);  // "?" in a tool window's title bar, opens its help page
    void draw_export_section();
    void draw_export_dialog();
    void draw_follow_dialog();
    void draw_about();
    void draw_bvh_prompt();
    void export_now(bool bvh, bool all_bones);  // Export tab: writes straight to the export folder
    void handle_shortcuts();
    void menu_item(const char* id);
    void draw_menus();
    void draw_dockspace();
    void draw_bones_panel();
    void draw_properties_panel();
    void draw_timeline_panel();
    void draw_graph_panel();
    void draw_hand_poser();
    void draw_inventory_panel();
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
    void draw_viewport();
    void draw_bone_lines(ImDrawList* dl) const;  // the world view's bones: the host draws no scene (spec 09 U3)
    // The world view's other actors, ghosts and collision volumes as lines (spec 09 U4).
    void draw_world_extras(ImDrawList* dl);
    ImGuiID dockspace_id_ = 0;
    void draw_message_popup();

    // Viewport
    void evaluate();
    ImTextureID render_scene(int w, int h);  // the view's picture, or 0 when the host draws none
    int pick_bone(ImVec2 mouse, std::vector<int>* ranked = nullptr) const;
    void draw_view_cube(ImDrawList* dl, ImVec2 vp_min, ImVec2 vp_max, bool viewport_hovered);
    void look_from(const Vec3& direction);  // animated (VP-63)
    void focus_camera_on(const Vec3& point);  // Second Life Alt+click, eased like the viewer's focus swing
    void update_camera_animation(double dt);
    // The first surface under the cursor: the skinned body, then the ground within 20 m.
    bool pick_surface(ImVec2 mouse, Vec3& point) const;
    void viewport_input(const ImVec2& origin, const ImVec2& size, bool hovered);
    bool place_gizmo();
    // First-open position of a tool window (windows.cpp); with a size in font units, also its first-open size,
    // kept on screen below the window's top.
    void place_tool_window(int slot, float w_em = 0, float h_em = 0);
    // IO-54: one-time import from the reference app (migrate.cpp).
    // Mesh bodies from devkits (bodies.cpp, spec 08 BD).
    struct MeshBody {
        std::string id, name;
        std::vector<std::string> parts;  // rigged .dae files, absolute paths
    };
    std::vector<MeshBody> bodies_;
    void load_bodies();
    void save_bodies() const;
    const MeshBody* mesh_body() const;  // the one shown instead of the Linden body, or null
    void use_mesh_body(const std::string& id);
    void import_body(const std::vector<std::string>& paths);
    void draw_mesh_body(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices);
    const MeshBody* find_mesh_body(const std::string& id) const;
    // BD-3: base with the joints the body's parts were rigged to (base itself when they override none).
    void harmonize_body(const MeshBody& b) const;
    const Shape* mesh_body_shape(const MeshBody& b, const Shape* base) const;
    mutable std::map<std::pair<std::string, const Shape*>, std::optional<Shape>> body_shapes_;
    void draw_bodies_section();
    void offer_migration(bool first_run);
    void apply_settings();
    std::string migrate_reference_data();  // returns a list of what was imported
    void draw_migration_prompt();
    std::string migration_dir_;
    bool show_migration_ = false;
    void keyboard_camera();  // Second Life preset: Alt + arrow keys
    bool cam_keys_held_ = false;
    void apply_gizmo_drag(ImVec2 mouse, bool snap);
    void capture_edit_start();
    void apply_delta(const Quat& r, const Vec3& t, int gimbal_axis = -1, double gimbal_angle = 0);
    // Blender preset: G / R over the view start a transform without clicking (spec 04 VP-51).
    enum class Modal { None, Move, Rotate, Trackball };
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
    const Shape* export_shape() const;  // IK and pins bake against this, not the viewport body (IO-13)
    std::string bake_shape_label(const std::string& key) const;  // "SL Default", "Mesh body: <name>", ...
    int limb_for_action() const;  // the limb of the primary handle or bone, or -1
    std::vector<std::string> selected_tracks() const;  // bones, their pin: tracks, selected handles' ik. tracks
    bool node_visible(int node) const {
        return skel_[node].volume ? show_volumes_ : show_category_[static_cast<int>(skel_[node].category)];
    }
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
    std::map<std::string, std::unique_ptr<DaeModel>> prop_models_;
    std::vector<float> prop_skin_pos_, prop_skin_nrm_;
    Prop drag_start_prop_;
    double cam_anim_t_ = -1, cam_anim_from_yaw_ = 0, cam_anim_from_pitch_ = 0, cam_anim_to_yaw_ = 0, cam_anim_to_pitch_ = 0;
    Vec3 cam_anim_from_target_, cam_anim_to_target_;
    double cam_anim_from_dist_ = 0, cam_anim_to_dist_ = 0;
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
    ImVec2 viewport_max_{800, 600};
    bool show_graph_ = true;
    std::vector<std::pair<std::string, Action>> actions_;
    std::map<std::string, std::pair<ImGuiKeyChord, ImGuiKeyChord>> industry_keys_;
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
    std::string message_title_, message_text_;
    std::string bone_filter_;
    int fps_edit_ = 30;            // the frame-rate field's value until the change is confirmed
    bool fps_edit_active_ = false;
    int bones_seen_primary_ = -1;  // reveal a bone selected elsewhere once (UI-23)
    bool bones_clicked_ = false;   // the selection came from the tree itself: no scroll
    bool quit_ = false;
    bool skip_shortcuts_ = false;
    std::string pending_tab_;           // --tab: focused once the panels exist
    // Undo/Redo pressed in a text field: the field is released first and commits its own step (UI-15).
    const char* deferred_action_ = nullptr;
    int deferred_frames_ = 0;
    bool headless_ = false;  // a key already handled this frame (e.g. Esc cancelling a drag)
    bool first_frame_ = true;

    // --- props agent ---
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
                   const Xform& world = {}, const float* tint = nullptr);  // tint: RGBA, drawn see-through (ghosts)
    bool render_thumbnail(const Prop& p, const std::string& png);
    ImTextureID prop_thumbnail(const PropLibraryItem& it);  // 0 until ready or when it can't be made
    void draw_prop_grid(std::vector<PropLibraryItem>& items, bool user);
    void viewport_drop_target(ImVec2 origin, ImVec2 size);  // called by draw_viewport after viewport_input
    std::vector<PropLibraryItem> prop_library_, starter_props_;
    std::map<std::string, ImTextureID> thumbs_;  // item id -> texture

    // --- viewport agent ---
    // The Scale tool (VP-40, props only). ponytail: stands in for Tool::Scale until the enum gains it.
    Tool last_effective_tool_ = Tool::Rotate;  // shows the "bones never scale" hint on the switch to Scale
    // VP-26: pressing a bone with the Rotate tool, then dragging, turns it about the view axis.
    int bone_drag_ = -1;
    bool bone_drag_started_ = false;
    ImVec2 bone_drag_press_;
    // The Local gizmo frame of a bone: its global rotation x the display bone frame (SK-21).
    Quat local_axes(int node) const { return globals_[node].rot * skel_.bone_frame(node); }
    void draw_collision_volumes(std::vector<Vertex>& verts);  // VP-10, SK-I5

    // --- formats/viewport agent ---
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
    // --- dynamics agent ---
    // Spec 08 DY: the Dynamics window and the live preview while playing (dynamics_ui.cpp).
    void draw_dynamics_panel();
    void apply_dynamics_preview(Evaluation& e);  // from evaluate(), before e is moved
    bool show_dynamics_ = false;
    bool dyn_preview_ = true;
    int dyn_selected_ = -1;
    std::unique_ptr<DynSim> dyn_sim_;
    std::vector<DynChain> dyn_chains_;  // the chains dyn_sim_ was built for
    double dyn_last_frame_ = 0;

    // --- retarget agent ---
    // Spec 07: File > Import Animation (Retarget)... and its mapping/report dialog (retarget_ui.cpp).
    void open_retarget(const std::string& path);  // reads the file, guesses the rig, opens the dialog
    void draw_retarget_dialog();
    void draw_retarget_split(struct RetargetUi& ui);  // RT-10.4 split / trim
    std::shared_ptr<struct RetargetUi> retarget_ui_;  // defined in retarget_ui.cpp
    std::shared_ptr<struct HelpUi> help_ui_;          // defined in help_ui.cpp

    // --- mocap agent ---
    // Spec 08 MC: Tools > Motion Capture... (VMC over UDP, mocap_ui.cpp).
    void draw_mocap_panel();                  // also polls the socket and records: call every frame
    void apply_mocap_preview(Evaluation& e);  // from evaluate(), after the dynamics preview
    bool mocap_busy() const;                  // listening: keep frames coming (busy())
    bool show_mocap_ = false;
    std::shared_ptr<struct MocapUi> mocap_ui_;  // defined in mocap_ui.cpp

    // --- ragdoll agent ---
    // Spec 08 RD: Tools > Ragdoll... (ragdoll_ui.cpp): settings, a simulated preview to scrub, bake.
    void draw_ragdoll_panel();
    void apply_ragdoll_preview(Evaluation& e);  // from evaluate(): shows the simulated preview frames
    bool show_ragdoll_ = false;
    std::vector<Pose> rd_frames_;               // the last simulated preview, frames 0..end
    std::optional<Ragdoll> rd_for_;             // the settings and tracks that preview was made from
    std::map<std::string, Track> rd_curves_for_;
    // --- groups agent --- (actors_ui.cpp, spec 08 GR)
    bool show_actors_ = false;
    void draw_actors_panel();
    bool multi_actor() const { return doc_.project.actors.size() >= 2; }
    Xform actor_rel(int i) const;              // actor i's placement in the active actor's space
    std::string actor_body_key(int i) const;   // body id or "mesh:<id>"; "" in the file = the view's choice
    void sync_active_body();                   // the view shows the active actor's own Linden body
    const Shape* actor_shape(int i) const;     // the shape actor i evaluates with
    ExternalTarget actor_resolver(int self);   // GR-4 pin targets as seen by actor `self`
    Evaluation evaluate_actor(int i, double frame);
    void activate_actor(int i);                // make i the active actor; the view stays put
    void scene_edit(const std::string& label, const std::function<void(Project&)>& change);  // one undo step
    void apply_restore(History::Restore r);    // after undo/redo
    void draw_other_actors(const SceneColours& colours);
    int pick_actor(ImVec2 m) const;            // another actor's body under the cursor, or -1
    void write_sit_note(const std::string& folder, const std::string& stem);
    std::vector<std::vector<float>> actor_pick_pos_;  // skinned positions of the other actors (world, active space)
    std::vector<const std::vector<std::uint32_t>*> actor_pick_idx_;
    std::map<std::string, std::unique_ptr<AvatarMesh>> actor_meshes_;  // Linden bodies other actors use
    int pin_actor_ = -1, pin_bone_ = -1;       // the actors panel's "Bind to" choice
    // Other actors with body "Skeleton Only": their globals (active space) and colour, drawn as bones.
    std::vector<std::pair<std::vector<Xform>, std::array<float, 3>>> other_skeletons_;
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
    void draw_loop_tools_menu();
    void draw_loop_seam_mark(ImDrawList* dl, float x_out, float y, bool hovered);
    int loop_blend_ = 0;
    float loop_travel_ = 1.f;
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
};

}  // namespace vats
