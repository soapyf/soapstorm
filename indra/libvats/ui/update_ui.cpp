// Viewport Avatar Toolset - Help > Check for Updates...: a new release, installed on the user's click.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Only an installed release updates itself (Host::can_update; the app does it with Velopack). The first run asks
// whether VATs may look at startup; a newer release is shown with its notes, and only Install downloads it. Restart Now
// puts it in place as VATs quits and starts the new one; Later leaves it for the next start.
#include <cfloat>
#include <sstream>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "vats_version.h"
#include "widgets.h"

namespace vats {

struct UpdateUi {
    enum class Stage { Idle, Ask, Checking, UpToDate, Found, Downloading, Downloaded, Failed };
    Stage stage = Stage::Idle;
    bool started = false;  // the startup check (or question) has run
    bool asked = false;    // Help > Check for Updates...: say "up to date" and failures too
    bool open = false;
    bool raise = false;    // brought to the front when it has something new to say
    std::shared_ptr<ui::UpdateJob> job;
    std::string version, notes, error;
};

namespace {

// Release notes as plain text: no heading marks, bold or code ticks.
std::string plain_notes(const std::string& md) {
    std::string out;
    std::istringstream in(md);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t h = 0;
        while (h < line.size() && line[h] == '#') ++h;
        if (h && h < line.size() && line[h] == ' ') line = line.substr(h + 1);
        for (const char* mark : {"**", "`"})
            for (size_t at; (at = line.find(mark)) != std::string::npos;) line.erase(at, std::string(mark).size());
        out += line + "\n";
    }
    return out;
}

}  // namespace

void App::check_for_updates(bool asked) {
    if (!update_ui_) update_ui_ = std::make_shared<UpdateUi>();
    UpdateUi& u = *update_ui_;
    u.asked = asked;
    u.open = u.raise = asked;
    // One job at a time: a check or download already running (or done) is what the window shows.
    if (u.job || u.stage == UpdateUi::Stage::Downloaded) return;
    u.job = host_.check_for_update();
    if (u.job) return void(u.stage = UpdateUi::Stage::Checking);
    u.error = "This copy of VATs cannot update itself: it was not installed from a VATs release (a build folder, or an "
              "older unpacked download). Install the latest release once, and it updates from then on.";
    u.stage = UpdateUi::Stage::Failed;
}

void App::draw_update() {
    if (ImGui::GetFrameCount() < 3) return;
    if (!update_ui_) update_ui_ = std::make_shared<UpdateUi>();
    UpdateUi& u = *update_ui_;
    using Stage = UpdateUi::Stage;
    if (!u.started) {
        u.started = true;
        if (headless_ || !host_.can_update()) return;
        if (settings_.update_check == "on") check_for_updates(false);
        else if (settings_.update_check.empty()) u.stage = Stage::Ask, u.open = u.raise = true;
    }
    auto failed = [&](const std::string& what) {
        u.error = what + (u.job && !u.job->message.empty() ? ": " + u.job->message : std::string()) + ".";
        u.stage = Stage::Failed;
        u.raise = u.open = u.open || u.asked;  // a check at startup fails quietly
        u.job.reset();
    };
    const bool done = u.job && u.job->state != ui::UpdateJob::Running, ok = u.job && u.job->state == ui::UpdateJob::Done;

    // The work in the background, whether the window shows or not.
    if (u.stage == Stage::Checking && done) {
        if (!ok) failed("Could not look for updates");
        else if (u.job->version.empty()) u.stage = Stage::UpToDate;
        else if (!u.asked && u.job->version == settings_.update_skip) u.stage = Stage::Idle;
        else u.version = u.job->version, u.notes = plain_notes(u.job->notes), u.stage = Stage::Found, u.open = u.raise = true;
        if (ok) u.job.reset();
    }
    if (u.stage == Stage::Downloading && done) {
        if (!ok) failed("The download did not finish. Nothing was changed");
        else u.stage = Stage::Downloaded, u.open = u.raise = true, u.job.reset();
    }

    if (!u.open) return;
    if (u.stage == Stage::Checking || u.stage == Stage::Downloading) host_.wake(0.2);
    place_tool_window("Updates", 34, 24);
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints(ImVec2(28 * em, 12 * em), ImVec2(FLT_MAX, FLT_MAX));  // the notes and their buttons
    if (u.raise) ImGui::SetNextWindowFocus(), u.raise = false;  // over the Welcome window on a first run
    if (!ImGui::Begin("Updates", &u.open, ImGuiWindowFlags_NoCollapse)) return ImGui::End();
    switch (u.stage) {
    case Stage::Ask:
        ImGui::TextWrapped("Should VATs look for a new version each time it starts?");
        hint("It asks GitHub, where VATs is published, for the latest release. Nothing about you or your work is sent, and "
             "nothing installs without your click. You can change this in Edit > Preferences, and Help > Check for "
             "Updates... works either way.");
        if (primary_button("Check at Startup")) settings_.update_check = "on", save_settings(), check_for_updates(false);
        ImGui::SameLine();
        if (ImGui::Button("Don't Check")) settings_.update_check = "off", save_settings(), u.open = false;
        break;
    case Stage::Checking:
        ImGui::TextUnformatted("Looking for a new version...");
        break;
    case Stage::UpToDate:
        ImGui::Text("VATs %s is the latest version.", VATS_VERSION);
        if (ImGui::Button("Close")) u.open = false;
        break;
    case Stage::Found:
        ImGui::Text("VATs %s is out. You have %s.", u.version.c_str(), VATS_VERSION);
        ImGui::BeginChild("##notes", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", u.notes.empty() ? "(no release notes)" : u.notes.c_str());
        ImGui::EndChild();
        if (primary_button("Install", "Downloads it; it is put in place when VATs restarts", 0, icon::kImport)) {
            u.job = host_.download_update();
            if (u.job) u.stage = Stage::Downloading;
            else failed("The update could not start");
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip This Version")) settings_.update_skip = u.version, save_settings(), u.open = false, u.stage = Stage::Idle;
        ImGui::SetItemTooltip("Not offered again at startup; Help > Check for Updates... still shows it");
        ImGui::SameLine();
        if (ImGui::Button("Later")) u.open = false;
        break;
    case Stage::Downloading:
        ImGui::Text("Downloading VATs %s...", u.version.c_str());
        ImGui::ProgressBar(float(u.job ? u.job->percent.load() : 0) / 100.f, ImVec2(-1, 0));
        hint("You can keep working.");
        break;
    case Stage::Downloaded:
        ImGui::Text("VATs %s is ready.", u.version.c_str());
        hint("Restart Now puts it in place and starts it (it asks about unsaved work first). Later: it is put in place the "
             "next time VATs starts.");
        if (primary_button("Restart Now"))
            guard_unsaved([this] {
                if (host_.apply_update_on_exit()) quit_ = true;  // unsaved work is settled: no autosave to offer back
                else message("Restart", "VATs could not start its update. Close VATs and open it again: it is put in place then.");
            });
        ImGui::SameLine();
        if (ImGui::Button("Later")) u.open = false;
        break;
    case Stage::Failed:
        ImGui::TextWrapped("%s", u.error.c_str());
        if (ImGui::Button("Close")) u.open = false;
        break;
    case Stage::Idle:
        u.open = false;
        break;
    }
    ImGui::End();
}

}  // namespace vats
