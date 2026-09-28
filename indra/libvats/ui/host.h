// Viewport Avatar Toolset - what the shared UI (vats_ui) needs from the program it runs in.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Two hosts implement this: the standalone app (app/sdl_host.*: SDL3 window, its own OpenGL scene
// renderer) and an SL viewer (spec 09 stages U2-U3). The UI never includes SDL or a renderer;
// everything platform- or renderer-specific comes through here. Spec: docs/spec/09 section 0b.
//
// Not here on purpose:
// - Clipboard: ImGui's own (ImGui::GetClipboardText / SetClipboardText) through its PlatformIO hooks,
//   which each host's ImGui platform glue already sets.
// - Input, frame pacing and the ImGui context: the host creates the context, feeds input, and calls
//   App::frame() between ImGui::NewFrame() and ImGui::Render(). App::frame() returning false is the
//   UI's "quit" (the viewer closes the editor); App::request_quit() is the host's close button.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "firewall.h"
#include "imgui.h"
#include "scene.h"
#include "vats/priority_plan.h"
#include "vats/project.h"
#include "vats/skeleton.h"
#include "theme.h"
#include "view_math.h"

namespace vats::ui {

// Where the UI reads and writes, fixed for the session.
struct Paths {
    std::string data;       // Linden character data (<data>/character) and rig tables (<data>/retarget)
    std::string assets;     // fonts, starter props (<assets>/props), welcome.md
    std::string help;       // the wiki pages the Help window shows
    std::string user;       // libraries, autosaves, window layout; ends with a separator; "" = nowhere to keep them
    std::string library;    // pose, prop and body libraries; ends with a separator; "" = <user>library/
    std::string settings;   // the settings.json file
    std::string character;  // the Linden character files; "" = <data>/character (the viewer ships its own)
};

// A file-type filter for the file dialogs: {"Mesh", "dae;fbx"}.
struct FileFilter {
    std::string name, patterns;
};

// The chosen paths; empty when cancelled or failed.
using FilesChosen = std::function<void(std::vector<std::string> files)>;

// The two scene targets: the 3D view, and the offscreen picture thumbnails are rendered into.
enum class SceneTarget { View, Thumbnail };

class Host {
public:
    virtual ~Host() = default;

    virtual const Paths& paths() const = 0;

    // --- 3D scene -------------------------------------------------------------------------------
    // The app rasterises what the UI sends; the viewer draws nothing (the world is the view) and
    // returns false from scene_begin, so the UI builds no geometry for it.
    // projection overrides cam's own (thumbnails use their own lens). A Thumbnail target starts clear
    // and transparent; the View starts with the backdrop.
    virtual bool scene_begin(SceneTarget target, int width, int height, const Camera& cam, const SceneColours& colours,
                             const Mat4* projection = nullptr) = 0;
    virtual void scene_ground(const Vec3& focus) = 0;  // grid and contact shadow; focus = the pelvis
    // translucent: back faces culled and no depth writes, for see-through overlays.
    virtual void scene_triangles(const std::vector<Vertex>& verts, const std::vector<std::uint32_t>& indices,
                                 bool depth_test, float gloss = 0.f, bool translucent = false) = 0;
    virtual ImTextureID scene_end() = 0;  // the target's picture, for ImDrawList::AddImage (bottom-up rows)
    // Saves the last Thumbnail picture as a PNG with straight alpha; false when it cannot.
    virtual bool save_thumbnail_png(const std::string& path) = 0;
    // Textures for thumbnails: a PNG loaded for ImGui::Image; 0 when missing or unreadable.
    virtual ImTextureID load_texture(const std::string& png) = 0;
    virtual void free_texture(ImTextureID texture) = 0;

    // --- Reference picture and listing media (08 RF, LM) -------------------------------------------
    // A load_texture picture on a quad, corners bottom-left, bottom-right, top-right, top-left of the picture, see-through
    // by opacity (0..1). backdrop: the corners are clip-space x, y and the picture lies behind everything drawn after it
    // (the UI sends it right after scene_begin); else they are in the scene, depth-tested, writing no depth. False when
    // the host cannot: the UI then draws the picture over the view itself, on ImGui's background list (the viewer, for
    // now; spec 09 §0e).
    virtual bool scene_image(ImTextureID texture, const std::array<Vec3, 4>& corners, float opacity, bool backdrop) {
        (void)texture, (void)corners, (void)opacity, (void)backdrop;
        return false;
    }
    // The last Thumbnail picture as straight-alpha RGBA, top row first; false when it cannot (listing media's GIF).
    virtual bool thumbnail_pixels(std::vector<std::uint8_t>& rgba, int& width, int& height) {
        (void)rgba, (void)width, (void)height;
        return false;
    }

    // --- Lighting (08 LT-1) -----------------------------------------------------------------------
    // The Light menu's preset, or null for the host's own lighting. The app lights its scene with the key and fill;
    // the viewer sets a local sky from them (only this viewer sees it) and puts the sky it had back with null, and
    // when the editor closes.
    virtual void set_light(const LightPreset* preset) { (void)preset; }

    // --- Avatar ---------------------------------------------------------------------------------
    // The evaluated pose, once a frame: every node's rotation and offset from rest (previews included).
    // The app draws its own body from it through the scene calls, so it ignores this; the viewer drives
    // the worn avatar with it.
    virtual void drive_avatar(const Skeleton& skel, const Pose& pose, const Clip& clip, double frame) = 0;

    // --- Camera and picking ---------------------------------------------------------------------
    // The camera the view shows and the UI's navigation edits (orbit, pan, zoom, view cube, focus,
    // camera views). The viewer keeps it in step with its own camera, fov included.
    // camera().ortho is the view's projection (View > Orthographic, spec 04 VP-67). Ortho or not, distance is the
    // zoom and fov the lens: the ortho view is 2 x distance x tan(fov / 2) tall, as tall at the target as the
    // perspective one, so zoom and framing mean the same in both. A host keeping camera() in step with its own
    // camera writes these logical values back, not the lens it draws ortho with.
    virtual Camera& camera() = 0;
    // The UI turns ortho on and off only through here; false = this host has no ortho view (the default, so a host
    // that does not implement it never has camera().ortho set under a perspective picture). The app sets the flag
    // and draws a true ortho projection (Camera::projection). The viewer (spec 09 section 0e "TODO (viewer):
    // orthographic") sets it and switches its own lens to a telephoto near-ortho, with projector() still matching.
    virtual bool set_orthographic(bool on) { (void)on; return false; }
    // The projection of the view shown in this rectangle (the Viewport panel, window coordinates):
    // to_screen for markers, gizmos and picking, ray (with camera()) for clicks. The viewer maps the
    // whole window instead, since its world fills it.
    virtual Projector projector(ImVec2 origin, ImVec2 size) = 0;

    // --- The world as the view (the viewer) -------------------------------------------------------
    // True when the host's own 3D world is the view (spec 09 U3): the UI shows no Viewport panel, draws
    // bones, markers and gizmos over the dockspace's empty centre through projector(), takes the pointer only
    // over them, and leaves camera navigation to the host (whose camera is authoritative; the UI's camera
    // edits, such as Frame Selected, go back through camera()).
    virtual bool world_view() const { return false; }
    // With world_view: false while the host's own UI (a viewer floater or menu) is under the pointer.
    virtual bool pointer_on_world() const { return true; }
    // With world_view: where the actor being edited stands in the frame of the host's avatar (the project's first
    // actor, your avatar), once a frame. The UI works in the edited actor's space, so the host maps that space through
    // this; the identity while you edit your own actor.
    virtual void set_view_frame(const Xform& edited_in_yours) { (void)edited_in_yours; }
    // With world_view: the proportions of the body the host shows (the worn avatar), which the view's
    // evaluation uses instead of the UI's own body; null = the UI's own.
    virtual const Shape* body_shape() const { return nullptr; }
    // The joints whose position a worn mesh overrides (its joint positions, e.g. a mesh head's face bones),
    // by skeleton name; empty = none or unknown. Only names: export warns with it, and never writes the positions.
    virtual std::vector<std::string> joint_overrides() const { return {}; }
    // The animations running on the host's own avatar (never another's), for the Priority Planner (spec 08 PP-5):
    // each one's name and the priority of every joint it has keys for, in the order they started (the last started
    // last). Names and priorities only, never keyframes; the editor's own playback is left out. Empty = none or
    // unknown (the app).
    virtual std::vector<PlanClip> running_motions() const { return {}; }
    // The names of what the host's own avatar wears on an attachment point (avatar_lad.xml id), for the point's hover
    // label (spec 09 build 19, item 55). Names only; empty = nothing worn there, or unknown (the app).
    virtual std::vector<std::string> worn_on(int attach_id) const { (void)attach_id; return {}; }
    // The grid the host is on, for the status bar and the upload section (spec 09 build 19, item 56); an empty name =
    // none (the app, or not logged in).
    struct Grid {
        std::string name;          // e.g. "Second Life", "Second Life Beta", an OpenSim grid's name
        bool test_grid = false;    // not Second Life's main grid (Aditi or OpenSim): uploads may be free there
        int upload_cost = -1;      // L$ per animation upload on this grid, -1 = not known
    };
    virtual Grid grid() const { return {}; }

    // --- Look (the viewer) -----------------------------------------------------------------------
    // The host's own colours (the viewer's skin), asked every frame: true replaces the colour theme with them,
    // and the UI restyles whenever they change.
    virtual bool skin_colours(HostColours& out) const { (void)out; return false; }
    // The program the UI runs inside, for Welcome and About; "" = the standalone app (SDL).
    virtual std::string host_name() const { return ""; }

    // --- The host's own UI beside the editor (the viewer, spec 09 U4b) ----------------------------
    // The app has none (null): the editor then shows none of it (no pane, no Viewer menu, no badge).
    class HostUi {
    public:
        virtual ~HostUi() = default;
        // A dockable pane the host fills with a window of its own (the viewer's conversations, "Chat"): every
        // frame the editor says whether the pane shows and its inner rectangle, in display coordinates.
        virtual const char* pane_title() const = 0;
        virtual void place_pane(bool shown, ImVec2 min, ImVec2 max) = 0;
        // The world area left between the editor's docked panels (the dockspace's central node), display coordinates,
        // every frame: the viewer keeps its toasts and notifications inside it.
        virtual void place_view(ImVec2 min, ImVec2 max) { (void)min, (void)max; }
        virtual int unread_notices() const = 0;  // the host's notifications not yet seen
        virtual void toggle_notices() = 0;       // shows or hides the host's notification window
        // The host's full UI, shown over the editor until turned off again (the viewer).
        virtual const char* reveal_label() const = 0;
        virtual const char* reveal_shortcut() const { return nullptr; }  // shown beside it in the menu
        virtual bool revealed() const = 0;
        virtual void reveal(bool on) = 0;
    };
    virtual HostUi* host_ui() { return nullptr; }

    // --- Upload (the viewer) ---------------------------------------------------------------------
    // True when the host can upload an animation straight to the grid.
    virtual bool can_upload() const { return false; }
    // Uploads exported .anim bytes under a name, after the host's own cost confirmation. done gets a status
    // line (sent, cancelled, or why not), on the UI thread, and the host takes the next upload from inside it
    // (the UI uploads several animations one after another).
    virtual void upload_anim(const std::vector<std::uint8_t>& bytes, const std::string& name,
                             std::function<void(const std::string&)> done) {
        (void)bytes, (void)name;
        done("This program cannot upload");
    }

    // --- Files and dialogs ----------------------------------------------------------------------
    // done may run on any thread, before or after these return.
    virtual void open_file_dialog(const std::vector<FileFilter>& filters, bool multiple, FilesChosen done) = 0;
    virtual void save_file_dialog(const std::vector<FileFilter>& filters, const std::string& suggested, FilesChosen done) = 0;
    virtual void open_folder_dialog(const std::string& start, FilesChosen done) = 0;
    // A question with buttons; the first answers Enter, the last Esc (and a closed box). done gets the
    // button's index, on the UI thread, before or after this returns.
    virtual void ask(const std::string& title, const std::string& text, const std::vector<std::string>& buttons,
                     std::function<void(int)> done) = 0;
    // Registers (or with install = false removes) the project file types with the desktop.
    // False with the reason when this host cannot.
    virtual bool associate_file_types(bool install, std::string& message) = 0;

    // --- Audio ----------------------------------------------------------------------------------
    // One playback stream of interleaved float samples. audio_start empties it and sets the format
    // and gain; false when there is no audio device.
    virtual bool audio_start(int rate, int channels, float gain) = 0;
    virtual void audio_queue(const float* samples, std::size_t count) = 0;  // count = samples, not frames
    virtual void audio_stop() = 0;                                        // empties the stream

    // --- Time and waking ------------------------------------------------------------------------
    virtual std::uint64_t ticks_ns() const = 0;  // monotonic nanoseconds
    // Asks for another frame after this many seconds (0 = as soon as possible), for a host that
    // sleeps while nothing happens.
    virtual void wake(double seconds = 0) = 0;

    // --- The rest -------------------------------------------------------------------------------
    virtual void open_url(const std::string& url) = 0;
    virtual void set_title(const std::string& title) = 0;
    // Runs shell commands for the firewall helper (firewall.h), possibly on another thread.
    virtual CommandRunner command_runner() = 0;
};

// One line about uploading on a host's grid: its price, or that a test grid may charge nothing.
inline std::string grid_note(const Host::Grid& g) {
    if (g.test_grid)
        return "On " + g.name + ", a test or OpenSim grid, uploads may be free" +
               (g.upload_cost > 0 ? " (it lists L$" + std::to_string(g.upload_cost) + ")" : std::string());
    return "On " + g.name + (g.upload_cost >= 0 ? ", an upload costs L$" + std::to_string(g.upload_cost) : std::string());
}

}  // namespace vats::ui
