// Viewport Avatar Toolset - Preferences, Controls help, Welcome and the Export settings.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/06 sections 4.4, 4.9, 4.10 and 5.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "vats_version.h"
#include "app.h"
#include "vats/anim_convert.h"
#include "vats/bvh.h"
#include "imgui_internal.h"
#include "vats/clips.h"
#include "vats/deformer.h"
#include "vats/export_name.h"
#include "vats/height_variant.h"
#include "vats/world_reduce.h"
#include "theme.h"
#include "widgets.h"

namespace vats {
namespace {

constexpr const char* kVersion = VATS_VERSION;

// A small Markdown subset for the bundled text: # and ## headings, "- " bullets, **bold** markers.
void markdown(const std::string& text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::string plain;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line.compare(i, 2, "**") == 0) {
                ++i;
                continue;
            }
            plain += line[i];
        }
        if (plain.rfind("## ", 0) == 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent_colour()), "%s", plain.c_str() + 3);
        } else if (plain.rfind("# ", 0) == 0) {
            ImGui::Spacing();
            ImGui::TextUnformatted(plain.c_str() + 2);
            ImGui::Separator();
        } else if (plain.rfind("- ", 0) == 0) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", plain.c_str() + 2);
        } else if (!plain.empty()) {
            ImGui::TextWrapped("%s", plain.c_str());
        }
    }
}

std::string json_str(const Json& obj, const char* key, const std::string& fallback = "") {
    const Json* v = obj.find(key);
    return v && v->is_string() ? v->str : fallback;
}
bool json_bool(const Json& obj, const char* key) {
    const Json* v = obj.find(key);
    return v && v->is_bool() && v->b;
}
int json_int(const Json& obj, const char* key, int fallback) {
    const Json* v = obj.find(key);
    return v && v->is_number() ? int(v->num) : fallback;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Preferences

// Tool windows first open at the 3D view's top-right, stepped so two never land on the same spot.
void App::place_tool_window(float w_em, float h_em) {
    // ImGui takes this only the first time the window opens (then layout.ini remembers where it was).
    ImGuiContext& g = *GImGui;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const Box work{vp->WorkPos.x, vp->WorkPos.y, vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y};
    // The avatar stands in the middle of the view (the dockspace's central node; the app's Viewport panel is there).
    const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);
    const float avatar_x = central && central->Size.x > 8 ? central->Pos.x + central->Size.x / 2 : (work.x0 + work.x1) / 2;
    // The other windows open over the dock: the tool windows already floating there.
    std::vector<Box> others;
    for (ImGuiWindow* w : g.Windows)
        if ((w->Active || w->WasActive) && !w->Hidden && !w->DockIsActive && !w->ParentWindow &&
            !(w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu |
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground)) &&
            w->Size.x < work.w() * 0.95f)
            others.push_back({w->Pos.x, w->Pos.y, w->Pos.x + w->Size.x, w->Pos.y + w->Size.y});
    const ImVec2 size = window_size(w_em, h_em);
    const float step = ImGui::GetFrameHeight();
    const Box at = place_window(work, size.x, size.y, avatar_x, others, 0.5f * step, step);
    ImGui::SetNextWindowPos(ImVec2(at.x0, at.y0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(at.w(), at.h()), ImGuiCond_FirstUseEver);
}

void App::draw_preferences() {
    if (!show_prefs_) return;
    ImGui::SetNextWindowSize(window_size(41, 37), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Preferences", &show_prefs_, ImGuiWindowFlags_NoDocking)) return ImGui::End();
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) show_prefs_ = false;
    const float label_w = ImGui::GetFontSize() * 11;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    row("Navigation & hotkeys");
    const std::vector<Preset> offered = offered_presets(host_.world_view());
    int preset = int(std::find(offered.begin(), offered.end(), settings_.preset) - offered.begin());
    std::vector<const char*> labels;
    for (Preset p : offered) labels.push_back(preset_label(p));
    if (offered.size() == 1) {  // the viewer: its own controls, no picker
        ImGui::TextUnformatted(labels[0]);
    } else if (ImGui::Combo("##preset", &preset, labels.data(), int(labels.size()))) {
        settings_.preset = offered[preset];
        if (settings_.preset == Preset::SecondLife) tool_ = Tool::Move;  // SL edits with the move arrows
        apply_preset();
        save_settings();
        status(std::string("Controls: ") + labels[preset]);
    }
    if (!settings_.key_overrides.empty()) {  // your own keys stay over any preset (Edit > Keyboard Shortcuts...)
        ImGui::SetCursorPosX(label_w);
        ImGui::AlignTextToFramePadding();
        const size_t n = settings_.key_overrides.size();
        ImGui::TextDisabled("%zu %s your own keys, over any preset", n, n == 1 ? "command keeps" : "commands keep");
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit...")) show_shortcuts_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            settings_.key_overrides.clear();
            apply_preset();
            save_settings();
            status("Keyboard shortcuts: the preset's keys");
        }
        ImGui::SetItemTooltip("Every command back to this preset's keys");
    }
    if (settings_.preset == Preset::Blender) {
        ImGui::SetCursorPosX(label_w);
        if (ImGui::Checkbox("Emulate 3-button mouse (Alt + left-drag = middle-drag)", &settings_.emulate_3_button))
            save_settings();
    }
    row("Colour theme");
    int theme = find_theme(settings_.theme);
    if (has_host_colours_) {
        ImGui::TextDisabled("The viewer's skin");  // Host::skin_colours
        ImGui::SetItemTooltip("Inside the viewer the editor takes its colours from the viewer's skin");
    } else if (ImGui::BeginCombo("##theme", theme_name(theme))) {
        for (int i = 0; i < theme_count(); ++i)
            if (ImGui::Selectable(theme_name(i), i == theme)) {
                settings_.theme = theme_name(i);
                apply_look();
                save_settings();
            }
        ImGui::EndCombo();
    }
    row("Interface size");
    static const float sizes[] = {0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f};
    char current[16];
    std::snprintf(current, sizeof current, "%d%%", int(settings_.interface_size * 100 + 0.5f));
    if (ImGui::BeginCombo("##size", current)) {
        for (float s : sizes) {
            char b[16];
            std::snprintf(b, sizeof b, "%d%%", int(s * 100 + 0.5f));
            if (ImGui::Selectable(b, std::fabs(s - settings_.interface_size) < 0.01f)) {
                settings_.interface_size = s;
                apply_look();
                save_settings();
            }
        }
        ImGui::EndCombo();
    }
    row("Gizmo size");
    if (slider_float("##gizmo", &settings_.gizmo_size, 50, 220, "%.0f px")) gizmo_size_ = settings_.gizmo_size;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    row(settings_.preset == Preset::SecondLife ? "Rotation snap (G)" : "Rotation snap (Ctrl)");
    if (slider_float("##snap", &settings_.snap_degrees, 1, 90, "%.0f°")) snap_deg_ = settings_.snap_degrees;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    row("BVH import");
    if (ImGui::Checkbox("Reduce keys after import", &settings_.bvh_reduce)) save_settings();
    ImGui::SetItemTooltip("Drops keys that linear playback reproduces within 0.05 degrees and 0.5 mm. "
                          "Off keeps a key on every frame.");
    row("Posing");  // spec 08 PT
    if (ImGui::Checkbox("Mirror centre bones in place", &settings_.mirror_centre)) save_settings();
    ImGui::SetItemTooltip("With Mirror on, posing the spine or head keeps it symmetric: a nod stays, a turn or lean is "
                          "cancelled. Off: centre bones pose as usual.");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Only key channels that already have keys", &settings_.scratch_existing_only)) save_settings();
    ImGui::SetItemTooltip("Keeping a scratch pose keys only the channels that were animated before it");
    ImGui::SetCursorPosX(label_w);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    const char* scrub[] = {"ask", "keep", "discard"};
    const char* scrub_names[] = {"Ask", "Keep as keys", "Discard"};
    int sc = 0;
    for (int i = 0; i < 3; ++i) sc = settings_.scratch_scrub == scrub[i] ? i : sc;
    if (ImGui::Combo("Leaving a scratch pose", &sc, scrub_names, 3)) settings_.scratch_scrub = scrub[sc], save_settings();
    row("Start screen");
    if (ImGui::Button("Show Now")) {
        show_prefs_ = false;
        show_welcome_ = true;
    }

    row("Project files");
    if (ImGui::Button("Open .vat Files with VATs")) {
        std::string msg;
        bool ok = host_.associate_file_types(true, msg);
        ok ? status(msg) : message("File association", msg);
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        std::string msg;
        host_.associate_file_types(false, msg);
        status(msg);
    }

    if (host_.world_view()) {  // settings only the viewer uses (spec 09 U4b)
        ImGui::SeparatorText("In the viewer");
        row("Opening the editor");
        if (ImGui::Checkbox("Reset joint positions when the editor opens", &settings_.viewer_reset_joints)) save_settings();
        ImGui::SetItemTooltip("Resets your avatar's skeleton on your screen only, as the viewer's Reset Skeleton does: joint "
                              "positions left by animations that stopped go back. Your mesh body's own joint offsets stay.");
        row("While the editor is open");
        if (ImGui::Checkbox("Show other avatars", &settings_.viewer_show_others)) save_settings();
        ImGui::SetItemTooltip("Off: every other avatar, with its attachments and name tag, is hidden on your screen only, as the "
                              "viewer's Render Only Friends does (friends too). Also View > Show Other Avatars.");
    }

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hotkeys for this preset");
    ImGui::SameLine();
    if (ImGui::SmallButton("Change Shortcuts...")) show_shortcuts_ = true;
    ImGui::BeginChild("##keys", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("Mouse: %s", nav_hint().c_str());
    if (ImGui::BeginTable("##keytable", 2, ImGuiTableFlags_RowBg)) {
        for (auto& [id, a] : actions_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.label);
            ImGui::TableNextColumn();
            std::string keys = a.key ? key_label(a.key) : "-";
            if (a.key2) keys += std::string(", ") + key_label(a.key2);
            ImGui::TextDisabled("%s", keys.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::draw_controls_help() {
    if (!show_help_) return;
    ImGui::SetNextWindowSize(window_size(35, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Controls", &show_help_, ImGuiWindowFlags_NoDocking)) return ImGui::End();
    ImGui::TextWrapped("%s", nav_hint().c_str());
    ImGui::TextWrapped("Click a bone to select it; click the same spot again to reach bones underneath. Right-click a "
                       "bone for its body-part menu. Esc or right-click cancels a drag.");
    ImGui::TextWrapped("%s", settings_.preset == Preset::SecondLife
                                 ? "Hold Ctrl to switch the gizmo to rotation (Ctrl+Shift: scale props); G toggles snapping. "
                                   "Keyboard camera: Alt+Left/Right orbits, Alt+Up/Down zooms, Ctrl+Alt+Up/Down orbits up "
                                   "and down, Ctrl+Alt+Shift+arrows pan."
                                 : "Hold Ctrl while dragging the gizmo to snap.");
    ImGui::TextWrapped("%s", graph_nav_hint().c_str());
    if (ImGui::Button("Change Shortcuts...")) show_shortcuts_ = true;
    ImGui::SetItemTooltip("Edit > Keyboard Shortcuts...: give any command keys of your own");
    ImGui::Separator();
    for (auto& [id, a] : actions_) {
        if (!a.key) continue;
        std::string keys = key_label(a.key);
        if (a.key2) keys += std::string(", ") + key_label(a.key2);
        ImGui::TextUnformatted(a.label);
        ImGui::SameLine(ImGui::GetFontSize() * 16);
        ImGui::TextDisabled("%s", keys.c_str());
    }
    ImGui::End();
}

void App::draw_welcome() {
    if (!show_welcome_) return;
    static std::string text;
    if (text.empty()) {
        std::ifstream f(assets_dir_ + "/welcome.md");
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    ImGui::SetNextWindowSize(window_size(43, 35), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Welcome to Viewport Avatar Toolset", &show_welcome_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
        return ImGui::End();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 10, p.y - 4), ImVec2(p.x - 7, p.y + 44), accent_colour());
    ImGui::TextUnformatted("Viewport Avatar Toolset");
    ImGui::TextDisabled("Second Life animation, open source  \xC2\xB7  Version %s", kVersion);
    ImGui::Separator();
    ImGui::BeginChild("##news", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2));
    markdown(text);
    ImGui::EndChild();
    if (const std::string host = host_.host_name(); host.empty())
        ImGui::TextDisabled("Built with SDL, Dear ImGui and the LGPL Second Life viewer's skeleton data.");
    else
        ImGui::TextDisabled("Running inside %s, with Dear ImGui and the LGPL Second Life viewer's skeleton data.", host.c_str());
    if (ImGui::Checkbox("Show this at startup", &settings_.show_welcome)) save_settings();
    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
    if (ImGui::Button("Close", ImVec2(80, 0))) show_welcome_ = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Export settings and immediate export

std::string App::bake_shape_key(const Json& ex, bool yours) const {
    const bool fitted = swap_body() || (host_.body_shape() && !host_.joint_overrides().empty());
    const std::string key = json_str(ex, "shape", fitted ? "avatar" : "sl-default");
    return key == "avatar" && !yours ? "sl-default" : key;  // Your avatar belongs to the actor that is your avatar
}

const Json& App::export_home_settings() const {
    const Project& p = doc_.project;
    return export_home_ >= 0 && export_home_ < int(p.actors.size()) ? actor_clip(p, export_home_).export_settings
                                                                      : doc_.clip().export_settings;
}

// Your avatar is the first actor, whichever actor is being edited or exported from.
bool App::exporting_yours() const {
    return !multi_actor() || doc_.project.active == 0 || json_bool(export_home_settings(), "avatar_all");
}

std::string App::bake_shape_label(const std::string& key) const {
    if (key == "avatar") {
        if (const MeshBody* b = swap_body()) return "Your avatar, swapped: " + b->name;  // spec 09 build 32
        return host_.body_shape() ? "Your avatar" : "Your avatar (viewer only: SL Default here)";
    }
    if (key.rfind("mesh:", 0) == 0) {
        const MeshBody* b = find_mesh_body(key.substr(5));
        return b ? "Mesh body: " + b->name : "Mesh body (missing)";
    }
    return key == "sl-default-male" ? "SL Default (Male)" : "SL Default";
}

const Shape* App::export_shape() const {
    // "avatar": IK and pins bake on SL Default; only the moving joints' positions come from the worn avatar
    // (export_positions), so nothing else of it (scales, other joints) reaches the file.
    // With the body swap (spec 09 build 32), "avatar" is the swapped body: IK, pins and positions bake on its own joints.
    const std::string key = bake_shape_key(doc_.clip().export_settings, exporting_yours());
    const Shape* female = &mesh_.sl_default(false).shape;
    if (const MeshBody* b = key == "avatar" ? swap_body() : nullptr) return mesh_body_shape(*b, female);
    if (key.rfind("mesh:", 0) == 0)  // BD-3: a mesh body's joints over the SL default
        if (const MeshBody* b = find_mesh_body(key.substr(5))) return mesh_body_shape(*b, female);
    return key == "sl-default-male" ? &mesh_.sl_default(true).shape : female;
}

// Read live from the host at each export or upload; the project keeps only the choice "avatar".
const Shape* App::export_positions() const {
    if (bake_shape_key(doc_.clip().export_settings, exporting_yours()) != "avatar") return nullptr;
    return swap_body() ? export_shape() : host_.body_shape();  // the swapped body's joints, or the worn avatar's
}

void App::draw_export_section() {
    Json& ex = doc_.clip().export_settings;
    if (!ex.is_object()) ex = Json::object();
    const float label_w = ImGui::GetFontSize() * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    // Each change is an undo step (UI-28). ponytail: typing in a text field makes one step per keystroke;
    // merge consecutive "Export Settings" steps in History if that gets noisy.
    auto set = [&](const char* key, Json v) {
        edit("Export Settings", [&](Clip& c) { c.export_settings.set(key, std::move(v)); });
    };
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", json_str(ex, "name").c_str());
    label("Name");
    if (ImGui::InputTextWithHint("##ename", "(project name)", buf, sizeof buf)) set("name", std::string(buf));
    int number = json_int(ex, "number", 1);
    label("Number");
    if (ImGui::InputInt("##enum", &number)) set("number", std::clamp(number, 0, 999));
    label("Side");
    const char* sides[] = {"(none)", "Left", "Right"};
    std::string side = json_str(ex, "side");
    int si = side == "Left" ? 1 : side == "Right" ? 2 : 0;
    if (ImGui::Combo("##eside", &si, sides, 3)) set("side", std::string(si ? sides[si] : ""));
    std::snprintf(buf, sizeof buf, "%s", json_str(ex, "pattern", "[NAME]_[#]_[SIDE]").c_str());
    label("Pattern");
    if (ImGui::InputTextWithHint("##epat", "[NAME]_[#]_[SIDE]", buf, sizeof buf)) set("pattern", std::string(buf));
    label("Bake shape");
    {
        const std::string key = bake_shape_key(ex);
        const std::string current = bake_shape_label(key);
        if (ImGui::BeginCombo("##eshape", current.c_str())) {
            auto pick = [&](const std::string& id, const std::string& text) {
                if (ImGui::Selectable(text.c_str(), key == id) && key != id) set("shape", id);
            };
            if (host_.body_shape() || swap_body()) pick("avatar", bake_shape_label("avatar"));  // the viewer: the worn avatar, or the swap
            pick("sl-default", "SL Default");
            pick("sl-default-male", "SL Default (Male)");
            for (int k = 0; k < int(bodies_.size()); ++k) {  // two bodies may share a name
                ImGui::PushID(k);
                pick("mesh:" + bodies_[k].id, "Mesh body: " + bodies_[k].name);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("IK and pins are baked against this body, whatever the view shows. A mesh body uses the "
                              "joint positions it was rigged to.%s",
                              swap_body() ? "\nYour avatar, swapped: View > Body shows a mesh body in your avatar's place, so "
                                            "IK, pins and position keys bake on that body's own joint positions. Choose "
                                            "Your Avatar in View > Body to bake on the avatar you wear."
                              : host_.body_shape() ? "\nYour avatar: positions fitted to the head and body you wear now; "
                                                   "other heads may look different. Only bones that move get positions; "
                                                   "IK and pins bake on SL Default." : "");
        if (const MeshBody* b = key == "avatar" ? swap_body() : nullptr) {  // spec 09 build 32: said plainly, not only on hover
            ImGui::SetCursorPosX(label_w);
            hint(("Swapped body: bakes on " + b->name + "'s own joint positions, not your worn avatar's").c_str());
        }
    }
    if (host_.world_view()) {  // the viewer: Your avatar is for the actor that is your avatar, unless this is on
        bool every = json_bool(ex, "avatar_all");
        ImGui::SetCursorPosX(label_w);
        if (ImGui::Checkbox("Use Your avatar for every actor", &every)) set("avatar_all", every);
        ImGui::SetItemTooltip("Off: only your avatar's actor (the first in the Actors window) bakes on Your avatar; the other "
                              "actors of a couple or group use SL Default instead. On: every actor bakes against your worn avatar (or the "
                              "body View > Body swaps in).");
    }
    bool both = json_bool(ex, "both"), count_up = json_bool(ex, "count_up"), mirrored = doc_.clip().mirror_export;
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Also export the other side (mirrored)", &both)) set("both", both);
    draw_height_variants(label_w);  // HV (ui/height_variant_ui.cpp)
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Count the number up after each export", &count_up)) set("count_up", count_up);
    bool to_library = json_bool(ex, "save_to_library");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Also save to Animations library", &to_library)) set("save_to_library", to_library);
    ImGui::SetItemTooltip("Each exported%s .anim is also copied to the Inventory's Animations, replacing one of the same name",
                          host_.can_upload() ? " or uploaded" : "");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Export mirrored (left and right swapped)", &mirrored)) {
        edit("Export Mirrored", [&](Clip& c) { c.mirror_export = mirrored; });
    }
    ImGui::SetItemTooltip("Swaps the sides in the exported file only; the project is unchanged");
    bool leave_static = json_bool(ex, "leave_static");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Leave out bones that don't move", &leave_static)) set("leave_static", leave_static);
    ImGui::SetItemTooltip("The .anim gets no rotation keys for bones (other than the hip) that stay at rest all through, "
                          "within the rotation tolerance below, so other animations, such as an AO's blinks, still move "
                          "them. Off: every keyed bone is written.");
    bool bvh_positions = json_bool(ex, "bvh_positions");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("BVH: include bone positions", &bvh_positions)) set("bvh_positions", bvh_positions);
    ImGui::SetItemTooltip("Writes position channels for moved bones other than the hip. Many tools "
                          "expect rotation only below the hip, so this is off by default.");
    // 09 0l, the deformer tool: shown once a bone other than the hip has position keys (or an option is on).
    bool end_rest = json_bool(ex, "end_at_rest"), hold = json_bool(ex, "hold_no_sink"), undeform = json_bool(ex, "undeformer");
    bool moved = end_rest || hold || undeform;
    for (auto& [name, track] : doc_.clip().curves)
        moved = moved || (name != "mPelvis" && skel_.find(name) > 0 && doc_.clip().has_channels(name, kPosChannels));
    if (moved) {
        label("Deformer");
        if (ImGui::Checkbox("End at rest", &end_rest)) set("end_at_rest", end_rest);
        ImGui::SetItemTooltip("Adds a frame after the last one with every moved bone (the hip aside) back at its rest "
                              "position. A bone keeps an animation's position after it stops, so the shape then lasts only "
                              "while the animation plays. A looping animation stops wherever it is: pair it with an "
                              "undeformer instead.");
        ImGui::SetCursorPosX(label_w);
        if (ImGui::Checkbox("Hold without sinking", &hold)) set("hold_no_sink", hold);
        ImGui::SetItemTooltip("Adds mSkull position keys that keep Second Life's avatar height at rest height on every "
                              "frame, so a longer neck or body does not sink the wearer into the ground (nor a shorter one "
                              "lift them), while it plays and after. mSkull's own position keys are kept and the counter-move "
                              "added to them. Anything rigged to mSkull, such as system hair, may shift.");
        if (hold) {
            const Shape* s = export_positions() ? export_positions() : export_shape();  // deformer_body
            const int head = skel_.find("mHead");
            char text[160];
            if (s && head >= 0 && size_t(head) < s->scale.size())
                std::snprintf(text, sizeof text, "Counters for a head scale of %.3g (%s)", s->scale[size_t(head)].z,
                              export_positions() && !swap_body() ? "your avatar's" : "the bake shape's");
            else
                std::snprintf(text, sizeof text, "Counters for a head scale of 1 (no bake shape to take it from)");
            ImGui::SetCursorPosX(label_w);
            hint(text);
        }
        ImGui::SetCursorPosX(label_w);
        if (ImGui::Checkbox("Also export an undeformer", &undeform)) set("undeformer", undeform);
        ImGui::SetItemTooltip("Writes [name]_undeform.anim beside the animation: the same bones at their rest positions, "
                              "at the same priority, %.1f s long. Playing it puts a deformer's bones back.%s",
                              double(kUndeformSeconds), host_.can_upload() ? " Upload Animation uploads it too." : "");
    }
    // IO-14: key-reduction tolerances, stored as [degrees, metres].
    float rot_deg = 0.05f, pos_mm = 0.5f;
    if (const Json* r = ex.find("reduce"); r && r->is_array() && r->arr.size() == 2 && r->arr[0].is_number() && r->arr[1].is_number())
        rot_deg = float(r->arr[0].num), pos_mm = float(r->arr[1].num * 1000);
    label("Reduce keys");
    // IO-14w (08 WR): per bone, or by the world-space error anywhere on the body ("reduce_mode" "world").
    const bool world = json_str(ex, "reduce_mode") == "world";
    int mode = world;
    const char* modes[] = {"Per bone", "Anywhere on the body"};
    if (ImGui::Combo("##ermode", &mode, modes, 2)) set("reduce_mode", std::string(mode ? "world" : "bone"));
    ImGui::SetItemTooltip("Per bone: each bone keeps the keys its own rotation and position need, within the two "
                          "tolerances. Anywhere on the body: keys go while no point of the body moves further than the "
                          "distance set, so a finger, which moves little of the body, loses more keys than a shoulder.");
    ImGui::SetCursorPosX(label_w);
    bool reduce_changed = false;
    if (world) {
        const Json* w = ex.find("reduce_world");
        float world_mm = float((w && w->is_number() && w->num > 0 ? w->num : kReduceWorldDefault) * 1000);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##eworld", &world_mm, 0.02f, 0.05f, 50, "%.2f mm anywhere on the body",
                             ImGuiSliderFlags_AlwaysClamp))
            set("reduce_world", double(world_mm) / 1000);
        ImGui::SetItemTooltip("The farthest any point of the body may move from your animation where keys are left "
                              "out. Your keys, the first and last frames and a key every 60 frames are always kept.");
    } else {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        ImGui::SetNextItemWidth(half);
        reduce_changed = ImGui::DragFloat("##erot", &rot_deg, 0.005f, 0, 5, "%.3f deg");
        ImGui::SetItemTooltip("Rotation tolerance. 0 and 0 keep a key on every frame.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        reduce_changed |= ImGui::DragFloat("##epos", &pos_mm, 0.05f, 0, 50, "%.2f mm");
        ImGui::SetItemTooltip("Position tolerance. 0 and 0 keep a key on every frame.");
    }
    if (reduce_changed) {
        Json a = Json::array();
        a.push(double(std::max(rot_deg, 0.f)));
        a.push(double(std::max(pos_mm, 0.f)) / 1000);
        set("reduce", a);
    }
    draw_upload_meter();  // UM: live size, what costs the most, Fit to 250 KB (sl_preview_ui.cpp)
    std::string folder = json_str(ex, "folder");
    label("Folder");
    ImGui::TextDisabled("%s", folder.empty() ? "(asks the first time)" : folder.c_str());
    ImGui::SetCursorPosX(label_w);
    if (ImGui::SmallButton("Choose...")) host_.open_folder_dialog(folder, dialog_result(Dialog::ExportFolder));

    ExportNaming naming = export_naming();
    const std::string stem = export_stem();
    label("Saves as");
    // GR-3: one file per actor, plus a placement note.
    std::vector<std::string> actor_names{""};
    if (multi_actor()) {
        actor_names.clear();
        for (const Actor& a : doc_.project.actors) actor_names.push_back(a.name);
    }
    const std::vector<AnimVariant> variants = anim_variants(mirrored, both, export_heights(ex));  // HV-4
    for (size_t k = 0; k < actor_names.size(); ++k) {
        naming.actor = actor_names[k];
        for (size_t v = 0; v < variants.size(); ++v) {
            if (k || v) ImGui::SetCursorPosX(label_w);
            const std::string file = variant_file_name(naming, stem, variants[v], "anim");
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "%s", file.c_str());
            if (undeform) {  // 09 0l
                ImGui::SetCursorPosX(label_w);
                ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "%s", undeformer_path(file).c_str());
            }
        }
    }
    if (multi_actor()) {
        naming.actor.clear();
        std::string note = export_file_name(naming, stem, false, "txt");
        ImGui::SetCursorPosX(label_w);
        ImGui::TextDisabled("%s_placement.txt (where each actor stands)", note.substr(0, note.size() - 4).c_str());
    }
    ImGui::Spacing();
    if (host_.can_upload()) {
        if (ImGui::Button("Upload Animation...", ImVec2(-1, 0))) upload_now();
        ImGui::SetItemTooltip("Uploads the animation to the grid you are on under the name above; the viewer asks to confirm the price");
        if (const ui::Host::Grid g = host_.grid(); !g.name.empty()) hint(ui::grid_note(g).c_str());  // spec 09 item 56
    }
    if (ImGui::Button("Export SL .anim", ImVec2(-1, 0))) export_now(false, false);
    ImGui::SetItemTooltip("%s", folder.empty() ? "Asks for a folder the first time" : ("Writes to " + folder).c_str());
    if (ImGui::Button("Export BVH (Animated Bones)...", ImVec2(-1, 0))) export_now(true, false);  // the menu's names
    ImGui::SetItemTooltip("Animated bones only");
    if (ImGui::Button("Export BVH (All Bento Bones)...", ImVec2(-1, 0))) export_now(true, true);
    ImGui::SetItemTooltip("Every Bento bone, keyed or not");
    if (const int n = clip_count(doc_.project); n > 1) {  // 08 CL-4
        ImGui::SeparatorText("Every clip");
        if (ImGui::Button(("Export All " + std::to_string(n) + " Clips (.anim)").c_str(), ImVec2(-1, 0))) export_now(false, false, true);
        ImGui::SetItemTooltip("Each clip with its own export settings, named with [CLIP], into the folder above");
        if (host_.can_upload() && ImGui::Button(("Upload All " + std::to_string(n) + " Clips...").c_str(), ImVec2(-1, 0)))
            upload_now(true);
    }
    hint("Attachment points and moved bones only survive in .anim.");
}

// The viewer (spec 09 section 4): every file Export would write (each actor, and the mirrored copy when "both"
// is on), under the export names, uploaded one after another; the host confirms the price of each. all_clips (08 CL-4):
// the same for every clip, each with its own export settings.
void App::upload_now(bool all_clips) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    if (!upload_queue_.empty()) return status("An upload is already waiting for its confirmation");
    Project& pr = doc_.project;
    const int home_clip = pr.active_clip, clips = all_clips ? clip_count(pr) : 1;
    std::deque<std::pair<std::string, std::vector<std::uint8_t>>> queue;
    std::string problems, warnings;
    for (int k = 0; k < clips && problems.empty(); ++k) {
        if (all_clips) set_active_clip(pr, k);
        const Json ex = doc_.clip().export_settings;  // a copy: switching actors moves the clip
        ExportNaming naming = export_naming();
        const std::string stem = export_stem();
        const bool mirrored = doc_.clip().mirror_export;
        // HV-4: the mirrored copy and the height variants, each baked on its own body.
        const std::vector<AnimVariant> variants = anim_variants(mirrored, json_bool(ex, "both"), export_heights(ex));
        const int home = pr.active, actors = multi_actor() ? int(pr.actors.size()) : 1;
        if (actors > 1) export_home_ = home;
        for (int a = 0; a < actors && problems.empty(); ++a) {
            if (actors > 1) {
                set_active_actor(pr, a);
                naming.actor = pr.actors[a].name;
            }
            for (const AnimVariant& v : variants) {
                std::string name = variant_file_name(naming, stem, v, "anim");
                name = name.substr(0, name.rfind('.'));
                const bool saved = doc_.clip().mirror_export;
                doc_.clip().mirror_export = v.mirrored;  // anim_bytes reads it
                set_export_height(v.height);
                AnimExportResult r;
                std::vector<std::uint8_t> bytes;
                const int made = anim_bytes(r, bytes);
                set_export_height(0);
                doc_.clip().mirror_export = saved;
                if (!made) {
                    problems = "-";  // anim_bytes already said why
                    break;
                }
                for (auto& e : validate_anim(r.file, skel_, true)) problems += "- " + name + ": " + e + "\n";  // 60 s, 250000 bytes
                for (auto& w : r.warnings) warnings += "- " + name + ": " + w + "\n";
                if (json_bool(ex, "save_to_library")) anim_to_library(name + ".anim", bytes);
                std::vector<std::uint8_t> undo = undeformer_bytes(r);  // 09 0l: uploaded after its deformer
                queue.emplace_back(name, std::move(bytes));
                if (!undo.empty()) {
                    const std::string uname = undeformer_name(name);
                    if (json_bool(ex, "save_to_library")) anim_to_library(uname + ".anim", undo);
                    queue.emplace_back(uname, std::move(undo));
                }
            }
        }
        if (actors > 1) set_active_actor(pr, home);
        export_home_ = -1;
    }
    if (all_clips) set_active_clip(pr, home_clip);
    if (problems == "-") return;
    if (!problems.empty()) return message("Cannot upload", problems);
    if (!warnings.empty()) message("Uploading with warnings", warnings + "\nCancel at the price question to stop an upload.");
    upload_queue_ = std::move(queue);
    upload_next();
}

void App::upload_next() {
    if (upload_queue_.empty()) return;
    const std::string name = upload_queue_.front().first;
    const size_t left = upload_queue_.size();
    status("Upload " + name + ": waiting for the confirmation" + (left > 1 ? " (" + std::to_string(left - 1) + " more after it)" : ""));
    std::vector<std::uint8_t> bytes = std::move(upload_queue_.front().second);
    host_.upload_anim(bytes, name, [this](const std::string& s) {
        status(s);
        if (!upload_queue_.empty()) upload_queue_.pop_front();
        upload_next();  // cancelling one still asks about the rest
    });
}

const Clip& App::active_actor_clip(int k) const {
    const Project& p = doc_.project;
    return k < 0 || k == p.active_clip || k >= int(p.clips.size()) ? p.clip : p.clips[k].clip;
}

ExportNaming App::export_naming(int k) const {
    const Project& p = doc_.project;
    if (k < 0) k = p.active_clip;
    const Json& ex = active_actor_clip(k).export_settings;
    ExportNaming n{json_str(ex, "name"), json_int(ex, "number", 1), json_str(ex, "side"),
                   json_str(ex, "pattern", "[NAME]_[#]_[SIDE]"), "", ""};
    // 08 CL-4: several clips need the clip in the name; a single named clip only where the pattern asks for it.
    if (p.clips.size() >= 2 || (!p.clips.empty() && n.pattern.find("[CLIP]") != std::string::npos)) n.clip = clip_name(p, k);
    return n;
}

std::string App::export_stem() const {
    std::string stem = doc_.path.empty() ? "" : doc_.path.substr(doc_.path.find_last_of('/') + 1);
    return stem.substr(0, stem.rfind('.'));
}

void App::export_now(bool bvh, bool all_bones, bool all_clips) {
    ScratchAside aside(*this);  // PT-2: the document, not a scratch pose
    // BVH: say what the format will lose before anything is written (IO-29).
    if (bvh && !bvh_confirmed_) {
        BvhExportOptions opt;
        opt.all_bones = all_bones;
        opt.joint_positions = json_bool(doc_.clip().export_settings, "bvh_positions");
        opt.shape = export_shape();
        if (multi_actor()) opt.external = actor_resolver(doc_.project.active);
        bvh_lost_ = vats::export_bvh(skel_, doc_.clip(), opt).lost;
        if (!bvh_lost_.empty()) {
            bvh_prompt_ = all_bones ? 2 : 1;
            return;
        }
    }
    const std::string folder = json_str(doc_.clip().export_settings, "folder");  // every clip's goes here (CL-4)
    if (std::error_code ec; folder.empty() || !std::filesystem::exists(u8path(folder), ec)) {
        // No folder yet (UI-32): a Save dialog with the pattern name, starting in the project folder. The folder
        // chosen there becomes the export folder (IO-45).
        if (headless_) return status("No export folder set");
        export_after_folder_ = bvh ? (all_bones ? 2 : 1) : 0;
        export_all_after_folder_ = all_clips;
        std::string dir = doc_.path.empty() ? "" : doc_.path.substr(0, doc_.path.find_last_of('/') + 1);
        std::string name = dir + export_file_name(export_naming(), export_stem(), doc_.clip().mirror_export, bvh ? "bvh" : "anim");
        // The Save dialog's folder becomes the export folder, and a name typed there the export's Name.
        host_.save_file_dialog({bvh ? ui::FileFilter{"BVH motion", "bvh"} : ui::FileFilter{"SL animation", "anim"}}, name,
                               dialog_result(Dialog::ExportFile));
        return;
    }
    int replaced = 0;
    std::string names;
    bool to_library = false;
    Project& pr = doc_.project;
    const int home_clip = pr.active_clip, clips = all_clips ? clip_count(pr) : 1;
    bool ok = true;
    for (int k = 0; k < clips && ok; ++k) {
        if (all_clips) set_active_clip(pr, k);
        const Json ex = doc_.clip().export_settings;  // a copy: switching actors moves the clip
        ExportNaming naming = export_naming();
        const std::string stem = export_stem();
        const bool mirrored = doc_.clip().mirror_export;
        // HV-4: the mirrored copy and, for .anim, the height variants, each baked on its own body.
        const std::vector<AnimVariant> variants =
            anim_variants(mirrored, json_bool(ex, "both"), bvh ? std::vector<double>{} : export_heights(ex));
        std::map<double, std::vector<double>> lifts;  // height -> each actor's hip lift, for the placement note
        const bool count_up = json_bool(ex, "count_up"), library = !bvh && json_bool(ex, "save_to_library");
        to_library |= library;
        // GR-3: one file per actor, all with the active actor's export settings; the actor is switched in turn.
        const int home = pr.active, actors = multi_actor() ? int(pr.actors.size()) : 1;
        if (actors > 1) export_home_ = home;
        for (int a = 0; a < actors && ok; ++a) {
            if (actors > 1) {
                set_active_actor(pr, a);
                naming.actor = pr.actors[a].name;
            }
            for (const AnimVariant& v : variants) {
                std::string path = folder + "/" + variant_file_name(naming, stem, v, bvh ? "bvh" : "anim");
                std::error_code ec;
                replaced += std::filesystem::exists(u8path(path), ec);
                bool saved = doc_.clip().mirror_export;
                doc_.clip().mirror_export = v.mirrored;  // export_anim/export_bvh read it
                set_export_height(v.height);
                if (v.height > 0 && v.mirrored == mirrored) lifts[v.height].push_back(export_hip_lift());
                ok = bvh ? export_bvh(path, all_bones) : export_anim(path);
                set_export_height(0);
                doc_.clip().mirror_export = saved;
                if (!ok) break;  // the export already explained why
                if (library) anim_file_to_library(path);
                if (std::error_code uec; library && json_bool(ex, "undeformer") && std::filesystem::exists(u8path(undeformer_path(path)), uec))
                    anim_file_to_library(undeformer_path(path));
                names += (names.empty() ? "" : ", ") + path.substr(path.find_last_of('/') + 1);
            }
        }
        export_home_ = -1;
        if (actors > 1) {
            set_active_actor(pr, home);
            if (ok) {
                ExportNaming base = naming;
                base.actor.clear();
                std::string note = export_file_name(base, stem, false, "txt");
                write_sit_note(folder, note.substr(0, note.size() - 4), lifts);
            }
        }
        if (ok && !bvh && count_up) {
            Json& ex_home = doc_.clip().export_settings;
            ex_home.set("number", std::min(json_int(ex_home, "number", 1) + 1, 999));
            mark_dirty();
        }
    }
    if (all_clips) set_active_clip(pr, home_clip);
    if (!ok) return;
    status("Exported " + names + " to " + folder + (replaced ? " (" + std::to_string(replaced) + " replaced)" : "") +
           (to_library ? ", and to the Animations library" : "") +
           (export_summary_.empty() || all_clips ? "" : ": " + export_summary_));  // UI-34
}

}  // namespace vats

namespace vats {

void App::draw_export_dialog() {
    if (!show_export_dialog_) return;
    if (!ImGui::IsPopupOpen("Export SL .anim")) ImGui::OpenPopup("Export SL .anim");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Export SL .anim", &show_export_dialog_)) return;
    const Clip& c = doc_.clip();
    if (clip_count(doc_.project) > 1)  // 08 CL: the settings below are this clip's
        ImGui::TextDisabled("Clip %s (switch in Tools > Clips)", clip_name(doc_.project, doc_.project.active_clip).c_str());
    double seconds = std::max(c.end_frame, 1) / double(c.fps);
    ImGui::TextDisabled("Length %.2f s, priority %d%s, ease %.2f / %.2f s", seconds, c.priority,
                        c.loop ? ", looping" : "", c.ease_in, c.ease_out);
    if (seconds > 60) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Over SL's 60 s limit: SL will refuse it.");
    if (multi_actor()) {  // GR-3: one file per actor, each baked with its own settings
        ImGui::SeparatorText("Actors");
        const Project& pr = doc_.project;
        for (int k = 0; k < int(pr.actors.size()); ++k) {
            const Json& ek = actor_clip(pr, k).export_settings;
            ImGui::BulletText("%s%s: bakes on %s", pr.actors[k].name.c_str(), k == pr.active ? " (settings below)" : "",
                              bake_shape_label(bake_shape_key(ek, k == 0 || json_bool(doc_.clip().export_settings, "avatar_all"))).c_str());
        }
        hint("Each actor keeps its own bake shape and key reduction; select an actor to change them. Naming, the folder "
             "and the mirrored copy come from the actor you export from.");
    }
    ImGui::Separator();
    draw_export_section();
    draw_message_popup(true);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && message_title_.empty()) show_export_dialog_ = false;
    if (!show_export_dialog_) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_bvh_prompt() {
    if (!bvh_prompt_) return;
    if (!ImGui::IsPopupOpen("BVH loses some of this")) ImGui::OpenPopup("BVH loses some of this");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("BVH loses some of this", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("BVH cannot carry everything in this animation:");
    for (auto& l : bvh_lost_) ImGui::BulletText("%s", l.c_str());
    ImGui::Spacing();
    int all = bvh_prompt_;
    auto done = [&] {
        bvh_prompt_ = 0;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("Export .anim Instead")) {
        done();
        export_now(false, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export BVH Anyway")) {
        done();
        bvh_confirmed_ = true;
        export_now(true, all == 2);
        bvh_confirmed_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
    ImGui::EndPopup();
}

// Follow Target (Bake), spec 02 AM-70/71: the second selected item follows the first.
void App::draw_follow_dialog() {
    if (!show_follow_) return;
    if (!ImGui::IsPopupOpen("Follow Target")) {
        ImGui::OpenPopup("Follow Target");
        double a, b;
        if (clip_range(a, b)) follow_f0_ = int(a), follow_f1_ = int(b);
        else follow_f0_ = int(frame_), follow_f1_ = doc_.clip().end_frame;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Follow Target", &show_follow_, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (selection_.size() != 2) {
        ImGui::TextWrapped("Select the bone to follow, then Shift-click the bone or point that follows it.");
    } else {
        int target = selection_[0], follower = selection_[1];
        ImGui::Text("%s follows %s", skel_[follower].name.c_str(), skel_[target].name.c_str());
        ImGui::InputInt("From frame", &follow_f0_);
        ImGui::InputInt("To frame", &follow_f1_);
        follow_f0_ = std::clamp(follow_f0_, 0, doc_.clip().end_frame);
        follow_f1_ = std::clamp(follow_f1_, follow_f0_, doc_.clip().end_frame);
        ImGui::Checkbox("Keep the current offset", &follow_keep_offset_);
        ImGui::SetItemTooltip("Off: snap onto the target, orientation included");
        hint("Keys every frame in the range; delete keys where it should fly free.");
        if (ImGui::Button("Bake", ImVec2(100, 0))) {
            std::string why;
            bool ok = false;
            edit("Follow Target", [&](Clip& c) {
                ok = follow_bake(c, *rig_, target, follower, follow_f0_, follow_f1_, follow_keep_offset_, shape(), why);
            });
            if (ok) status(skel_[follower].name + " follows " + skel_[target].name + " over frames " +
                           std::to_string(follow_f0_) + "-" + std::to_string(follow_f1_));
            else message("Follow Target", why);
            show_follow_ = false;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) show_follow_ = false;
    if (!show_follow_) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_about() {
    if (!show_about_) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("About Viewport Avatar Toolset", &show_about_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::Text("Viewport Avatar Toolset %s", kVersion);
        ImGui::TextDisabled("An open-source animation editor for Second Life. LGPL-2.1.");
        ImGui::TextDisabled("VATs is free. If you paid for it, you were misled: the official downloads are at");
        ImGui::TextDisabled("github.com/squeedledorf/viewport-avatar-toolset/releases");
        ImGui::Separator();
        ImGui::BulletText("Skeleton, attachment points and avatar meshes: Second Life viewer data,");
        ImGui::TextDisabled("    (C) Linden Research, Inc., LGPL-2.1");
        if (const std::string host = host_.host_name(); host.empty())
            ImGui::BulletText("Dear ImGui (MIT), SDL 3 (zlib), Inter font (SIL OFL 1.1)");
        else
            ImGui::BulletText("Dear ImGui (MIT), Inter font (SIL OFL 1.1); running inside %s (LGPL-2.1)", host.c_str());
        ImGui::BulletText("Starter props: see app/assets/props/CREDITS.md");
    }
    ImGui::End();
}

// Offers autosaves from a session that did not end cleanly (UI-9).
void App::draw_recovery() {
    if (recoverable_.empty()) return;
    // Wait for any other modal (the first-run import offer, a message): opening a second root modal on
    // the same frame closes the first, and the two would keep replacing each other, blocking all input.
    if (!ImGui::IsPopupOpen("Recover unsaved work") && (ImGui::GetTopMostPopupModal() || show_migration_)) return;
    if (!ImGui::IsPopupOpen("Recover unsaved work")) ImGui::OpenPopup("Recover unsaved work");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Recover unsaved work", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("VATs closed without saving these. Recover one to keep working on it.");
    ImGui::Spacing();
    auto forget = [](const Recoverable& r) {
        std::remove(r.file.c_str());
        std::remove((r.file.substr(0, r.file.rfind('.')) + ".path").c_str());
    };
    for (size_t i = 0; i < recoverable_.size(); ++i) {
        const Recoverable r = recoverable_[i];
        ImGui::PushID(int(i));
        const std::string name = r.original.empty() ? "Untitled" : r.original.substr(r.original.find_last_of('/') + 1);
        ImGui::Text("%s", name.c_str());
        ImGui::SameLine();
        long long m = r.age_minutes;
        ImGui::TextDisabled("autosaved %s ago", m < 120 ? (std::to_string(m) + " min").c_str() : (std::to_string(m / 60) + " h").c_str());
        if (!r.original.empty()) ImGui::SetItemTooltip("%s", r.original.c_str());
        if (ImGui::Button("Recover")) {
            std::string err;
            if (!open_recovered(r.file, r.original, true, err)) {  // unsaved: it still has to be saved
                message("Could not recover", err);
            } else {
                status("Recovered " + name + ": save it to keep it");
                // The old autosave goes only once this session's own copy exists, so a second crash
                // before the next save still leaves something to recover.
                if (write_autosave()) forget(r);
                recoverable_.clear();  // one document at a time; the rest are offered next launch
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            forget(r);
            recoverable_.erase(recoverable_.begin() + i);
        }
        ImGui::PopID();
        if (recoverable_.empty() || i >= recoverable_.size()) break;
    }
    ImGui::Spacing();
    if (ImGui::Button("Later") || ImGui::IsKeyPressed(ImGuiKey_Escape)) recoverable_.clear();  // kept for next launch
    if (recoverable_.empty()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool App::show_window(const std::string& name) {
    static const std::map<std::string, bool App::*> windows = {
        {"graph", &App::show_graph_},       {"mocap", &App::show_mocap_},         {"actors", &App::show_actors_}, {"clips", &App::show_clips_},
        {"dynamics", &App::show_dynamics_}, {"ragdoll", &App::show_ragdoll_},     {"preferences", &App::show_prefs_},
        {"hands", &App::show_hands_},       {"export", &App::show_export_dialog_}, {"controls", &App::show_help_},
        {"about", &App::show_about_},       {"quality", &App::show_quality_},
        {"split-dance", &App::show_split_dance_}, {"planner", &App::show_planner_},
        {"batch-retarget", &App::show_batch_retarget_}, {"foot-lock", &App::show_foot_lock_},
        {"auto-balance", &App::show_auto_balance_}, {"jump-arc", &App::show_jump_arc_},
        {"reference", &App::show_reference_}, {"listing-media", &App::show_listing_},
        {"face-cam", &App::face_cam_},      {"keys", &App::show_shortcuts_},

        {"check", &App::show_check_},      {"idle", &App::show_idle_},           {"overlap", &App::show_overlap_},
        {"face", &App::show_face_},        {"loop-assist", &App::show_loop_assist_}, {"sl-preview", &App::sl_preview_},
        {"treadmill", &App::treadmill_on_}};
    static const std::map<std::string, const char*> panels = {
        {"graph", "Graph"}, {"properties", "Properties"}, {"timeline", "Timeline"}, {"bones", "Bones"}, {"inventory", "Inventory"},
        {"picker", "Picker"}};
    bool known = false;
    if (auto w = windows.find(name); w != windows.end()) this->*(w->second) = true, known = true;
    if (auto p = panels.find(name); p != panels.end()) pending_tab_ = p->second, known = true;  // docked: to the front
    if (name == "help") open_help(), known = true;
    if (name == "simplify") open_simplify(), known = true;
    if (name == "dope-sheet") show_dope_ = true, pending_tab_ = "Dope Sheet", known = true;  // 08 DS
    if (name == "motion-path") motion_path_.on = true, known = true;                        // 08 MP
    if (name == "transition") show_transition_ = true, transition_.to = int(std::round(frame_)), known = true;  // 08 PM-3
    if (name == "match-poses") open_match_poses(doc_.clip(), "a copy of the clip"), known = true;  // 08 PM-1, for screenshots
    if (name == "idle") idle_selected_ = 0;  // the first layer, if any, with its settings (clamped when there is none)
    if (name == "loop-assist") loop_find_now_ = true;  // with Find pressed, so the candidates show
    if (name == "sit") show_actors_ = sit_scroll_ = true, known = true;  // Actors, scrolled to Sit systems
    if (name == "insert-frames" || name == "stretch-range") time_prompt_ = name == "insert-frames" ? 1 : 2, known = true;
    return known;
}

}  // namespace vats
