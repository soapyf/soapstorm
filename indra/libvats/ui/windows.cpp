// Viewport Avatar Toolset - Preferences, Controls help, Welcome and the Export settings.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/06 sections 4.4, 4.9, 4.10 and 5.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "vats_version.h"
#include "app.h"
#include "vats/anim_convert.h"
#include "vats/bvh.h"
#include "imgui_internal.h"
#include "vats/export_name.h"
#include "theme.h"

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
void App::place_tool_window(int slot, float w_em, float h_em) {
    const float step = ImGui::GetFrameHeight();
    const float top = ImGui::GetMainViewport()->WorkPos.y + 2.5f * step + slot * step;  // below the view's tab
    ImGui::SetNextWindowPos(ImVec2(viewport_max_.x - 12 - slot * step, top), ImGuiCond_FirstUseEver, ImVec2(1, 0));
    if (w_em > 0) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 size = window_size(w_em, h_em);
        size.y = std::min(size.y, vp->WorkPos.y + vp->WorkSize.y - top - step);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    }
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
    int preset = int(settings_.preset);
    const char* presets[] = {"Industry (Maya-style)", "Blender", "QAvimator", "Second Life"};
    if (ImGui::Combo("##preset", &preset, presets, 4)) {
        settings_.preset = Preset(preset);
        if (settings_.preset == Preset::SecondLife) tool_ = Tool::Move;  // SL edits with the move arrows
        apply_preset();
        save_settings();
        status(std::string("Controls: ") + presets[preset]);
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
    if (ImGui::SliderFloat("##gizmo", &settings_.gizmo_size, 50, 220, "%.0f px")) gizmo_size_ = settings_.gizmo_size;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    row(settings_.preset == Preset::SecondLife ? "Rotation snap (G)" : "Rotation snap (Ctrl)");
    if (ImGui::SliderFloat("##snap", &settings_.snap_degrees, 1, 90, "%.0f°")) snap_deg_ = settings_.snap_degrees;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    row("BVH import");
    if (ImGui::Checkbox("Reduce keys after import", &settings_.bvh_reduce)) save_settings();
    ImGui::SetItemTooltip("Drops keys that linear playback reproduces within 0.05 degrees and 0.5 mm. "
                          "Off keeps a key on every frame.");
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
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Hotkeys for this preset");
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

std::string App::bake_shape_label(const std::string& key) const {
    if (key.rfind("mesh:", 0) == 0) {
        const MeshBody* b = find_mesh_body(key.substr(5));
        return b ? "Mesh body: " + b->name : "Mesh body (missing)";
    }
    return key == "sl-default-male" ? "SL Default (Male)" : "SL Default";
}

const Shape* App::export_shape() const {
    const std::string key = json_str(doc_.clip().export_settings, "shape", "sl-default");
    const Shape* female = &mesh_.sl_default(false).shape;
    if (key.rfind("mesh:", 0) == 0)  // BD-3: a mesh body's joints over the SL default
        if (const MeshBody* b = find_mesh_body(key.substr(5))) return mesh_body_shape(*b, female);
    return key == "sl-default-male" ? &mesh_.sl_default(true).shape : female;
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
        const std::string key = json_str(ex, "shape", "sl-default");
        const std::string current = bake_shape_label(key);
        if (ImGui::BeginCombo("##eshape", current.c_str())) {
            auto pick = [&](const std::string& id, const std::string& text) {
                if (ImGui::Selectable(text.c_str(), key == id) && key != id) set("shape", id);
            };
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
                              "joint positions it was rigged to.");
    }
    bool both = json_bool(ex, "both"), count_up = json_bool(ex, "count_up"), mirrored = doc_.clip().mirror_export;
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Also export the other side (mirrored)", &both)) set("both", both);
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
    bool bvh_positions = json_bool(ex, "bvh_positions");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("BVH: include bone positions", &bvh_positions)) set("bvh_positions", bvh_positions);
    ImGui::SetItemTooltip("Writes position channels for moved bones other than the hip. Many tools "
                          "expect rotation only below the hip, so this is off by default.");
    // IO-14: key-reduction tolerances, stored as [degrees, metres].
    float rot_deg = 0.05f, pos_mm = 0.5f;
    if (const Json* r = ex.find("reduce"); r && r->is_array() && r->arr.size() == 2 && r->arr[0].is_number() && r->arr[1].is_number())
        rot_deg = float(r->arr[0].num), pos_mm = float(r->arr[1].num * 1000);
    label("Reduce keys");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::SetNextItemWidth(half);
    bool reduce_changed = ImGui::DragFloat("##erot", &rot_deg, 0.005f, 0, 5, "%.3f deg");
    ImGui::SetItemTooltip("Rotation tolerance. 0 and 0 keep a key on every frame.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    reduce_changed |= ImGui::DragFloat("##epos", &pos_mm, 0.05f, 0, 50, "%.2f mm");
    ImGui::SetItemTooltip("Position tolerance. 0 and 0 keep a key on every frame.");
    if (reduce_changed) {
        Json a = Json::array();
        a.push(double(std::max(rot_deg, 0.f)));
        a.push(double(std::max(pos_mm, 0.f)) / 1000);
        set("reduce", a);
    }
    std::string folder = json_str(ex, "folder");
    label("Folder");
    ImGui::TextDisabled("%s", folder.empty() ? "(asks the first time)" : folder.c_str());
    ImGui::SetCursorPosX(label_w);
    if (ImGui::SmallButton("Choose...")) host_.open_folder_dialog(folder, dialog_result(Dialog::ExportFolder));

    ExportNaming naming{json_str(ex, "name"), json_int(ex, "number", 1), json_str(ex, "side"),
                        json_str(ex, "pattern", "[NAME]_[#]_[SIDE]"), ""};
    std::string stem = doc_.path.empty() ? "" : doc_.path.substr(doc_.path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));
    label("Saves as");
    // GR-3: one file per actor, plus a placement note.
    std::vector<std::string> actor_names{""};
    if (multi_actor()) {
        actor_names.clear();
        for (const Actor& a : doc_.project.actors) actor_names.push_back(a.name);
    }
    for (size_t k = 0; k < actor_names.size(); ++k) {
        naming.actor = actor_names[k];
        if (k) ImGui::SetCursorPosX(label_w);
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "%s", export_file_name(naming, stem, mirrored, "anim").c_str());
        if (both) {
            ImGui::SetCursorPosX(label_w);
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "%s", export_file_name(naming, stem, !mirrored, "anim").c_str());
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
    }
    if (ImGui::Button("Export SL .anim", ImVec2(-1, 0))) export_now(false, false);
    ImGui::SetItemTooltip("%s", folder.empty() ? "Asks for a folder the first time" : ("Writes to " + folder).c_str());
    if (ImGui::Button("Export BVH (Animated Bones)...", ImVec2(-1, 0))) export_now(true, false);  // the menu's names
    ImGui::SetItemTooltip("Animated bones only");
    if (ImGui::Button("Export BVH (All Bento Bones)...", ImVec2(-1, 0))) export_now(true, true);
    ImGui::SetItemTooltip("Every Bento bone, keyed or not");
    hint("Attachment points and moved bones only survive in .anim.");
}

// The viewer (spec 09 section 4): every file Export would write (each actor, and the mirrored copy when "both"
// is on), under the export names, uploaded one after another; the host confirms the price of each.
void App::upload_now() {
    if (!upload_queue_.empty()) return status("An upload is already waiting for its confirmation");
    const Json& ex = doc_.clip().export_settings;
    ExportNaming naming{json_str(ex, "name"), json_int(ex, "number", 1), json_str(ex, "side"),
                        json_str(ex, "pattern", "[NAME]_[#]_[SIDE]"), ""};
    std::string stem = doc_.path.empty() ? "" : doc_.path.substr(doc_.path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));
    const bool mirrored = doc_.clip().mirror_export;
    std::vector<bool> variants{mirrored};
    if (json_bool(ex, "both")) variants.push_back(!mirrored);
    Project& pr = doc_.project;
    const int home = pr.active, actors = multi_actor() ? int(pr.actors.size()) : 1;
    std::deque<std::pair<std::string, std::vector<std::uint8_t>>> queue;
    std::string problems;
    for (int a = 0; a < actors && problems.empty(); ++a) {
        if (actors > 1) {
            set_active_actor(pr, a);
            naming.actor = pr.actors[a].name;
        }
        for (bool m : variants) {
            std::string name = export_file_name(naming, stem, m, "anim");
            name = name.substr(0, name.rfind('.'));
            const bool saved = doc_.clip().mirror_export;
            doc_.clip().mirror_export = m;  // anim_bytes reads it
            AnimExportResult r;
            std::vector<std::uint8_t> bytes;
            const int made = anim_bytes(r, bytes);
            doc_.clip().mirror_export = saved;
            if (!made) {
                problems = "-";  // anim_bytes already said why
                break;
            }
            for (auto& e : validate_anim(r.file, skel_, true)) problems += "- " + name + ": " + e + "\n";  // 60 s, 250000 bytes
            if (json_bool(ex, "save_to_library")) anim_to_library(name + ".anim", bytes);
            queue.emplace_back(name, std::move(bytes));
        }
    }
    if (actors > 1) set_active_actor(pr, home);
    if (problems == "-") return;
    if (!problems.empty()) return message("Cannot upload", problems);
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

void App::export_now(bool bvh, bool all_bones) {
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
    Json& ex = doc_.clip().export_settings;
    std::string folder = json_str(ex, "folder");
    ExportNaming naming{json_str(ex, "name"), json_int(ex, "number", 1), json_str(ex, "side"),
                        json_str(ex, "pattern", "[NAME]_[#]_[SIDE]"), ""};
    std::string stem = doc_.path.empty() ? "" : doc_.path.substr(doc_.path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));
    const bool mirrored = doc_.clip().mirror_export;
    if (std::error_code ec; folder.empty() || !std::filesystem::exists(u8path(folder), ec)) {
        // No folder yet (UI-32): a Save dialog with the pattern name, starting in the project folder. The folder
        // chosen there becomes the export folder (IO-45).
        if (headless_) return status("No export folder set");
        export_after_folder_ = bvh ? (all_bones ? 2 : 1) : 0;
        std::string dir = doc_.path.empty() ? "" : doc_.path.substr(0, doc_.path.find_last_of('/') + 1);
        std::string name = dir + export_file_name(naming, stem, mirrored, bvh ? "bvh" : "anim");
        // The Save dialog's folder becomes the export folder.
        host_.save_file_dialog({bvh ? ui::FileFilter{"BVH motion", "bvh"} : ui::FileFilter{"SL animation", "anim"}}, name,
                               dialog_result(Dialog::ExportFolder, true));
        return;
    }
    std::vector<bool> variants{mirrored};
    if (json_bool(ex, "both")) variants.push_back(!mirrored);
    const bool count_up = json_bool(ex, "count_up"), to_library = !bvh && json_bool(ex, "save_to_library");
    int replaced = 0;
    std::string names;
    // GR-3: one file per actor, all with the active actor's export settings; the actor is switched in turn.
    Project& pr = doc_.project;
    const int home = pr.active, actors = multi_actor() ? int(pr.actors.size()) : 1;
    bool ok = true;
    for (int a = 0; a < actors && ok; ++a) {
        if (actors > 1) {
            set_active_actor(pr, a);
            naming.actor = pr.actors[a].name;
        }
        for (bool m : variants) {
            std::string path = folder + "/" + export_file_name(naming, stem, m, bvh ? "bvh" : "anim");
            std::error_code ec;
            replaced += std::filesystem::exists(u8path(path), ec);
            bool saved = doc_.clip().mirror_export;
            doc_.clip().mirror_export = m;  // export_anim/export_bvh read it
            ok = bvh ? export_bvh(path, all_bones) : export_anim(path);
            doc_.clip().mirror_export = saved;
            if (!ok) break;  // the export already explained why
            if (to_library) anim_file_to_library(path);
            names += (names.empty() ? "" : ", ") + path.substr(path.find_last_of('/') + 1);
        }
    }
    if (actors > 1) {
        set_active_actor(pr, home);
        if (ok) {
            ExportNaming base = naming;
            base.actor.clear();
            std::string note = export_file_name(base, stem, false, "txt");
            write_sit_note(folder, note.substr(0, note.size() - 4));
        }
    }
    if (!ok) return;
    if (!bvh && count_up) {
        Json& ex_home = doc_.clip().export_settings;
        ex_home.set("number", std::min(json_int(ex_home, "number", 1) + 1, 999));
        mark_dirty();
    }
    status("Exported " + names + " to " + folder + (replaced ? " (" + std::to_string(replaced) + " replaced)" : "") +
           (to_library ? ", and to the Animations library" : "") +
           (export_summary_.empty() ? "" : ": " + export_summary_));  // UI-34
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
                              bake_shape_label(json_str(ek, "shape", "sl-default")).c_str());
        }
        hint("Each actor keeps its own bake shape and key reduction; select an actor to change them. Naming, the folder "
             "and the mirrored copy come from the actor you export from.");
    }
    ImGui::Separator();
    draw_export_section();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) show_export_dialog_ = false;
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

}  // namespace vats
