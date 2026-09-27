// Viewport Avatar Toolset - one-time import of the reference app's settings and libraries (IO-54).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 3.7 and 3.8.2. Only data files are read: settings.cfg (a Godot ConfigFile,
// parsed as plain key=value lines) and the library JSON files, which VATs' loaders already accept.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "app.h"
#include "imgui.h"

namespace vats {
namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Godot's user:// folder for the reference app, or "" when it is not there.
std::string reference_data_dir() {
    const std::string app = "app_userdata/Hexton SL Animator 2026/";
#if defined(_WIN32)
    std::string base = env("APPDATA") + "/Godot/";
#elif defined(__APPLE__)
    std::string base = env("HOME") + "/Library/Application Support/Godot/";
#else
    std::string xdg = env("XDG_DATA_HOME");
    std::string base = (xdg.empty() ? env("HOME") + "/.local/share" : xdg) + "/godot/";
#endif
    std::error_code ec;
    return fs::is_directory(base + app, ec) ? base + app : "";
}

// "section.key" -> raw value text, from key=value lines under [section] headers.
std::map<std::string, std::string> read_config(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line, section;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == ';') continue;
        out[section + "." + line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

std::string unquote(const std::string& v) {
    return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
}

// A single-line [ "a", "b" ] array of strings.
std::vector<std::string> string_array(const std::string& v) {
    std::vector<std::string> out;
    for (size_t i = v.find('"'); i != std::string::npos; i = v.find('"', i)) {
        size_t j = v.find('"', i + 1);
        if (j == std::string::npos) break;
        out.push_back(v.substr(i + 1, j - i - 1));
        i = j + 1;
    }
    return out;
}

}  // namespace

void App::offer_migration(bool first_run) {
    if (!first_run || settings_.migration_offered) return;
    migration_dir_ = reference_data_dir();
    show_migration_ = !migration_dir_.empty();
}

std::string App::migrate_reference_data() {
    std::string report;
    auto cfg = read_config(migration_dir_ + "settings.cfg");
    auto get = [&](const char* key) -> const std::string* {
        auto it = cfg.find(key);
        return it == cfg.end() ? nullptr : &it->second;
    };
    auto number = [&](const char* key, float& v, float lo, float hi) {
        if (auto* s = get(key))
            if (float x = float(std::atof(s->c_str())); std::isfinite(x)) v = std::clamp(x, lo, hi);  // "nan" stays out
    };
    auto boolean = [&](const char* key, bool& v) {
        if (auto* s = get(key)) v = *s == "true";
    };
    if (!cfg.empty()) {
        if (auto* s = get("input.preset")) preset_from_name(unquote(*s), settings_.preset);
        boolean("input.emulate_3_button", settings_.emulate_3_button);
        number("ui.scale", settings_.interface_size, 0.5f, 3.f);
        number("ui.gizmo_size", settings_.gizmo_size, 50, 220);
        number("ui.snap_degrees", settings_.snap_degrees, 1, 90);
        number("view.view_cube_size", settings_.view_cube_size, 60, 260);
        boolean("ui.show_graph", settings_.show_graph);
        boolean("ui.show_welcome", settings_.show_welcome);
        if (auto* s = get("ui.orientation")) {
            std::string o = unquote(*s);
            if (o == "local" || o == "world" || o == "gimbal") settings_.orientation = o;
        }
        if (auto* s = get("ui.theme")) settings_.theme = unquote(*s) == "Maya Gray" ? "Studio Grey" : "Dusk";
        if (auto* s = get("view.body")) {
            std::string b = unquote(*s);
            if (b == "male" || b == "female") settings_.body = b;
        }
        if (auto* s = get("files.recent"))
            for (auto& r : string_array(*s))
                if (fs::exists(r) && settings_.recent.size() < 10) settings_.recent.push_back(r);
        report += "- Preferences and recent files\n";
    }
    // Libraries: copied only when VATs has none of its own yet, so nothing is overwritten.
    // ponytail: thumbnails are not copied; VATs renders its own on first view.
    for (const char* name : {"poses.json", "library.json"}) {
        std::string from = migration_dir_ + "library/" + name, to = library_dir() + name;
        std::error_code ec;
        if (!fs::exists(from) || fs::exists(to)) continue;
        if (fs::copy_file(from, to, ec)) report += std::string("- ") + (name[0] == 'p' ? "Pose library" : "Prop library") + "\n";
    }
    settings_.migration_offered = true;
    save_settings();
    apply_look();
    apply_settings();
    mesh_.build(body_);
    library_ = {};
    prop_library_ = {};
    starter_props_ = {};
    load_library();
    return report;
}

void App::draw_migration_prompt() {
    if (!show_migration_ || headless_) return;
    const char* title = "Import from Hexton SL Animator?";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("VATs found data from Hexton SL Animator on this computer.");
    ImGui::TextUnformatted("It can bring over your pose library, prop library, recent files and preferences.");
    ImGui::TextDisabled("%s", migration_dir_.c_str());
    ImGui::Spacing();
    auto close = [&] {
        show_migration_ = false;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("Import")) {
        std::string report;
        const bool ok = guarded(migration_dir_ + "settings.cfg", [&] { report = migrate_reference_data(); });
        close();
        if (ok) message("Imported from Hexton SL Animator", report.empty() ? "Nothing new to import." : report);
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't import") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        settings_.migration_offered = true;
        save_settings();
        close();
    }
    ImGui::SetItemTooltip("VATs will not ask again");
    ImGui::EndPopup();
}

}  // namespace vats
