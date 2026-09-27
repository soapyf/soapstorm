// Viewport Avatar Toolset - Tools > Motion Capture...: live VMC capture, preview and recording.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 3 (MC-2..MC-4). Parsing, mapping and writing keys are in the core
// (vats/mocap.h); this owns the socket, the window, the live preview and the undo step.
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <future>
#include <sstream>

#include "app.h"
#include "firewall.h"
#include "theme.h"
#include "imgui.h"
#include "vats/mocap.h"
#include "vats/pose_ops.h"
#include "udp.h"

namespace vats {

struct MocapUi {
    UdpReceiver sock;
    int source = 0;    // 0 VMC, 1 Rokoko Studio Live (JSON v3), 2 iFacialMocap (face only)
    int port = 39539;  // VMC's usual port
    std::string actor;  // Rokoko: the actor streamed (the first with a body)
    // Accept senders on other devices, remembered per source. A phone is always another device.
    bool lan[3] = {false, false, true};
    std::string error;

    RigTable table;
    std::string table_error;
    // Face (MC-5): VMC blendshapes arrive on the VMC port; iFacialMocap streams to its own.
    FaceTable face;
    std::string face_error;
    FaceSettings face_settings;
    bool face_on = true, face_only = false;
    int face_preset = 0;
    std::string phone_ip;
    VmcState state, rest;
    bool have_data = false, rest_from_pose = false, drive = true;

    MocapRecorder rec;
    int take_actor = 0;           // the take belongs to this actor of this document (App::doc_generation_)
    unsigned take_generation = 0;
    bool commit_pending = false;  // the take ended during another edit: merged once that edit is done
    int from = 0, to = 30;
    bool punch_out = false, selected_only = false;
    float countdown = 3;
    std::vector<std::string> only;  // tracks a selected-parts recording writes
    MocapCleanup clean;
    std::vector<std::string> report;

    // Connection indicator.
    int count = 0, pps = 0;
    double window_start = 0, last_packet = -1, listen_start = 0;
    std::string sender;
    bool remote_seen = false;  // a packet came from another device, so nothing is blocking

    // Setup checklist: this computer's addresses, the firewall, and the one-click rule.
    std::vector<LanAddress> addrs;
    double addrs_at = -100;
    Firewall firewall = Firewall::Unknown;
    std::future<Firewall> detect_job;
    bool detected = false;
    std::future<int> allow_job;
    int allowed_port = 0;  // the port a rule was added for this session
    std::string allow_message;
    bool allow_failed = false;
};

namespace {
bool ready(const std::future<int>& f) { return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
}  // namespace

namespace {


// The window's choices as saved in settings.json ("mocap"). The per-take ones (frames, punch-out,
// Face Only, Selected Body Parts Only) are not kept.
Json mocap_settings(const MocapUi& ui) {
    auto map = [](const std::map<std::string, double>& m) {
        Json o = Json::object();
        for (auto& [k, v] : m) o.set(k, v);
        return o;
    };
    const FaceSettings& f = ui.face_settings;
    const MocapCleanup& c = ui.clean;
    Json lan = Json::array();
    for (bool b : ui.lan) lan.push(b);
    Json j = Json::object();
    j.set("source", ui.source), j.set("port", ui.port), j.set("allow_other_devices", lan), j.set("phone_ip", ui.phone_ip);
    j.set("drive", ui.drive), j.set("face", ui.face_on), j.set("face_preset", ui.face_preset);
    j.set("face_gain", f.gain), j.set("eye_gain", f.eye_gain), j.set("eye_yaw_max", f.eye_yaw_max);
    j.set("eye_pitch_max", f.eye_pitch_max), j.set("head", f.head), j.set("positions", f.positions), j.set("shape_gains", map(f.gains));
    j.set("neutral_face", map(f.neutral)), j.set("countdown", double(ui.countdown));
    j.set("smooth", c.smooth), j.set("reduce", c.reduce), j.set("reduce_deg", c.rot_deg), j.set("reduce_m", c.pos_m);
    j.set("edge_blend", c.blend), j.set("foot_lock", c.lock_feet);
    return j;
}

void load_mocap_settings(MocapUi& ui, const Json& j) {
    auto num = [&](const char* k, auto& v, double lo, double hi) {
        if (auto* x = j.find(k); x && x->is_number() && std::isfinite(x->num))
            v = static_cast<std::remove_reference_t<decltype(v)>>(std::clamp(x->num, lo, hi));
    };
    auto flag = [&](const char* k, bool& v) {
        if (auto* x = j.find(k); x && x->is_bool()) v = x->b;
    };
    auto map = [&](const char* k, std::map<std::string, double>& m) {
        if (auto* x = j.find(k); x && x->is_object())
            for (auto& [name, v] : x->obj)
                if (v.is_number() && std::isfinite(v.num)) m[name] = std::clamp(v.num, 0.0, 2.0);
    };
    FaceSettings& f = ui.face_settings;
    MocapCleanup& c = ui.clean;
    num("source", ui.source, 0, 2), num("port", ui.port, 1, 65535);
    if (auto* x = j.find("allow_other_devices"); x && x->is_array())
        for (size_t i = 0; i < x->arr.size() && i < 3; ++i)
            if (x->arr[i].is_bool()) ui.lan[i] = x->arr[i].b;
    if (auto* x = j.find("phone_ip"); x && x->is_string() && x->str.size() < 64) ui.phone_ip = x->str;
    flag("drive", ui.drive), flag("face", ui.face_on), num("face_preset", ui.face_preset, 0, 64);
    num("face_gain", f.gain, 0, 2), num("eye_gain", f.eye_gain, 0, 2), num("eye_yaw_max", f.eye_yaw_max, 5, 45);
    num("eye_pitch_max", f.eye_pitch_max, 5, 45), flag("head", f.head), flag("positions", f.positions), map("shape_gains", f.gains);
    map("neutral_face", f.neutral), num("countdown", ui.countdown, 0, 5);
    num("smooth", c.smooth, 0, 5), flag("reduce", c.reduce), num("reduce_deg", c.rot_deg, 0.05, 5);
    num("reduce_m", c.pos_m, 0.0001, 0.02), num("edge_blend", c.blend, 0, 15), flag("foot_lock", c.lock_feet);
}

}  // namespace

bool App::mocap_busy() const {  // listening, or waiting on a firewall check or change: keep frames coming
    return mocap_ui_ && (mocap_ui_->sock.is_open() || mocap_ui_->detect_job.valid() || mocap_ui_->allow_job.valid());
}

void App::apply_mocap_preview(Evaluation& e) {
    MocapUi* ui = mocap_ui_.get();
    if (!ui || !ui->drive || !ui->have_data || !ui->sock.is_open() || (ui->table.bones.empty() && ui->face.shapes.empty()))
        return;
    // The live pose replaces the mapped tracks of a copy of the clip; IK and pins would fight it.
    // ponytail: copies the whole clip every frame; keep a scratch clip if big projects stutter.
    Clip overlay = doc_.clip();
    for (auto it = overlay.curves.begin(); it != overlay.curves.end();)
        it = it->first.rfind("ik.", 0) == 0 ? overlay.curves.erase(it) : std::next(it);
    overlay.pins.clear();
    Clip live = live_pose(skel_, ui->table, ui->rest, ui->state, shape(), ui->face_on ? &ui->face : nullptr,
                          ui->face_settings);
    const bool parts = ui->rec.active() && (ui->selected_only || ui->face_only);
    for (auto& [name, tr] : live.curves)
        if (!parts || std::find(ui->only.begin(), ui->only.end(), name) != ui->only.end()) overlay.curves[name] = tr;
    e = vats::evaluate(*rig_, overlay, frame_, shape());
}

void App::draw_mocap_panel() {
    if (!mocap_ui_) load_mocap_settings(*(mocap_ui_ = std::make_shared<MocapUi>()), settings_.mocap);
    MocapUi& ui = *mocap_ui_;
    const double now = double(host_.ticks_ns()) * 1e-9;
    // Scripted checks: VATS_MOCAP_LISTEN=<port> starts with the window open and listening (VMC, this
    // computer only), like VATS_FAKE_FIREWALL for the setup checklist.
    if (static bool checked = false; !checked) {
        checked = true;
        if (const char* port = std::getenv("VATS_MOCAP_LISTEN")) {
            show_mocap_ = true;
            ui.port = std::atoi(port);
            ui.sock.open(ui.port, false, ui.error);
            ui.window_start = ui.listen_start = now;
        }
    }

    // Poll the socket every frame, open window or not.
    if (ui.sock.is_open()) {
        std::uint8_t buf[65536];
        std::vector<OscMessage> msgs;
        std::string from;
        for (int n, guard = 0; guard < 4000 && (n = ui.sock.receive(buf, sizeof buf, &from)) > 0; ++guard) {
            try {  // packets come from the network: a hostile one is dropped, never fatal
                if (ui.source == 2) {
                    apply_ifacialmocap(std::string_view(reinterpret_cast<const char*>(buf), size_t(n)), ui.state);
                } else if (ui.source == 1) {
                    std::string err;
                    if (!apply_rokoko(buf, size_t(n), ui.state, "", ui.actor, err)) ui.error = "Rokoko: " + err;
                    else if (ui.error.rfind("Rokoko: ", 0) == 0) ui.error.clear();
                } else {
                    msgs.clear();
                    parse_osc(buf, size_t(n), msgs);
                    for (auto& m : msgs) apply_vmc(m, ui.state);
                }
            } catch (const std::exception& e) {
                ui.error = std::string("Dropped a packet that could not be read: ") + e.what();
                continue;
            }
            ++ui.count;
            ui.last_packet = now;
            ui.sender = from;
            if (from.rfind("127.", 0) != 0) ui.remote_seen = true;
        }
        if (!ui.have_data && ui.state.bones.empty() && (!ui.state.blend.empty() || ui.state.has_face_head))
            ui.have_data = true;  // a face-only sender
        if (!ui.have_data && !ui.state.bones.empty()) {
            ui.have_data = true;
            // VMC: positions from the first frame, T-pose rotations. Rokoko sends each joint in its own
            // axes, so the first frame is the rest until the performer's T-pose is captured.
            ui.rest = ui.source == 1 ? ui.state : vmc_t_pose(ui.state);
            ui.rest.root.rot = Quat{};
        }
        for (auto& [name, x] : ui.state.bones)  // bones a sender starts sending later
            if (!ui.rest.bones.count(name)) ui.rest.bones[name] = Xform{Quat{}, x.pos};
        if (now - ui.window_start >= 1) ui.pps = ui.count, ui.count = 0, ui.window_start = now;
    }

    // Recording: frames follow wall time at the clip's rate; the playhead follows the take.
    auto commit = [&] {
        if (doc_.history.is_open()) {  // a drag is running: merging now would clobber its undo snapshot
            ui.commit_pending = true;
            return;
        }
        ui.commit_pending = false;
        std::vector<VmcState> frames = std::move(ui.rec.frames);
        ui.rec.frames.clear();
        if (frames.empty()) return;
        edit("Record Motion Capture", [&](Clip& c) {
            ui.report = merge_recording(c, skel_, ui.table, ui.rest, frames, ui.rec.from,
                                        ui.selected_only || ui.face_only ? ui.only : std::vector<std::string>{}, ui.clean,
                                        export_shape(), ui.face_on ? &ui.face : nullptr, ui.face_settings);
        });
        set_frame(ui.rec.from);  // back to the start of the take, ready to play
        status(ui.report.empty() ? "Recorded" : "Motion capture: " + ui.report[0]);
    };
    // A take belongs to the document and actor it started on: a New, Open, Import, Recover or a switch to
    // another actor cancels it instead of merging it into the wrong animation.
    if ((ui.rec.active() || ui.commit_pending) &&
        (ui.take_generation != doc_generation_ || ui.take_actor != doc_.project.active)) {
        ui.rec.stop();
        ui.rec.frames.clear();
        ui.commit_pending = false;
        status("Motion capture take cancelled: the document or the edited actor changed");
    }
    if (ui.commit_pending) commit();
    if (ui.rec.active()) {
        bool more = ui.rec.feed(now, ui.state);
        // The take may run past the clip's end, so the playhead is set directly (set_frame clamps).
        if (!ui.rec.frames.empty()) frame_ = ui.rec.from + int(ui.rec.frames.size()) - 1;
        if (!more) commit();
    }

    if (!show_mocap_) return;
    // Saved as soon as a change is finished (not on every step of a slider drag).
    if (Json j = mocap_settings(ui); !headless_ && !ImGui::IsAnyItemActive() && j != settings_.mocap)
        settings_.mocap = std::move(j), save_settings();
    place_tool_window(3, 28, 44);
    if (!ImGui::Begin("Motion Capture", &show_mocap_)) return ImGui::End();
    help_button("motion-capture");
    if (ui.table.bones.empty() && ui.table_error.empty()) {
        std::ifstream f(data_dir_ + "/retarget/vrm-humanoid.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (!parse_rig_table(ss.str(), ui.table, ui.table_error) && ui.table_error.empty())
            ui.table_error = "data/retarget/vrm-humanoid.json is missing";
    }
    if (ui.face.shapes.empty() && ui.face_error.empty()) {
        std::ifstream f(data_dir_ + "/retarget/face-arkit.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (!parse_face_table(ss.str(), ui.face, ui.face_error) && ui.face_error.empty())
            ui.face_error = "data/retarget/face-arkit.json is missing";
        auto natural = ui.face.presets.find("Natural");  // the preset the default settings match
        if (natural != ui.face.presets.end() && !settings_.mocap.find("face_preset"))  // unless one was saved
            ui.face_preset = int(std::distance(ui.face.presets.begin(), natural));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", ui.source == 2
        ? "Receives iFacialMocap from an iPhone or iPad with Face ID. Enter the address the app shows, "
          "listen, then press Connect to iPhone. Face and head only: add a body sender separately."
        : ui.source == 1
        ? "Receives Rokoko Studio Live. In Studio, add a Custom streaming target with this computer's address, "
          "this port and the JSON v3 data format."
        : "Receives the VMC protocol, which webcam and VR tracker apps send (for example XR Animator, "
          "VSeeFace, or VirtualMotionCapture). Point the app's VMC sender at this computer and port.");
    ImGui::PopStyleColor();
    if (!ui.table_error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ui.table_error.c_str());
    if (!ui.face_error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ui.face_error.c_str());

    // Labels sit in a left column, as in the rest of the app; w = 0 fills the row.
    const float label_w = ImGui::GetFontSize() * 7.5f;
    auto label = [&](const char* text, float w = 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(w > 0 ? w : -1);
    };
    auto indent = [&] { ImGui::SetCursorPosX(label_w); };

    // Setup checklist: everything a phone needs, with its live state, so nobody types firewall commands.
    {
        const bool listening = ui.sock.is_open();
        if (now - ui.addrs_at > 5) ui.addrs = lan_addresses(), ui.addrs_at = now;  // Wi-Fi can change
        if (!ui.detected && !ui.detect_job.valid())
            ui.detect_job = std::async(std::launch::async, [run = host_.command_runner()] { return detect_firewall(run); });
        if (ui.detect_job.valid() && ui.detect_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            ui.firewall = ui.detect_job.get(), ui.detected = true;
        if (ready(ui.allow_job)) {
            const int rc = ui.allow_job.get();
            ui.allow_failed = rc != 0;
            ui.allow_message = rc == 0     ? "Done: your home network can now reach port " + std::to_string(ui.port) + "."
                               : rc == 126 ? "Cancelled: nothing was changed."
                                           : "The firewall was not changed (code " + std::to_string(rc) +
                                                 "). You can run the command below yourself.";
            if (rc == 0) ui.allowed_port = ui.port;
        }

        ImGui::SeparatorText("Setup");
        auto row = [&](int state, const std::string& text) {  // 0 done, 1 needs attention, 2 not yet
            const ImU32 c = state == 0 ? IM_COL32(110, 210, 120, 255)
                            : state == 1 ? IM_COL32(230, 170, 70, 255)
                                         : IM_COL32(120, 120, 120, 255);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float r = ImGui::GetFontSize() * 0.3f;
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() / 2), r, c);
            ImGui::Dummy(ImVec2(r * 2.5f, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", text.c_str());
        };

        if (ui.addrs.empty()) {
            row(1, "No network address found. Connect this computer to Wi-Fi or Ethernet.");
        } else {
            ImGui::TextDisabled("Your computer:");
            for (size_t i = 0; i < ui.addrs.size(); ++i) {
                ImGui::PushID(int(i));
                ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.5f);
                ImGui::TextUnformatted(ui.addrs[i].ip.c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy")) {
                    ImGui::SetClipboardText(ui.addrs[i].ip.c_str());
                    status("Copied " + ui.addrs[i].ip);
                }
                ImGui::SetItemTooltip("Enter this address in the sending app, on the same Wi-Fi (%s).",
                                      ui.addrs[i].name.c_str());
                ImGui::PopID();
            }
        }

        const bool got = ui.last_packet >= 0;
        row(listening ? 0 : 2, listening ? "Listening on port " + std::to_string(ui.port)
                                         : "Not listening yet: press Listen below.");
        const bool lan = ui.lan[ui.source];
        if (lan) {
            row(0, "Other devices can send");
        } else {
            row(listening && !got && now - ui.listen_start > 5 ? 1 : 2,
                "Only apps on this computer can send. Turn on Allow Other Devices for a phone or headset.");
            if (listening && !got && now - ui.listen_start > 5) {
                if (ImGui::SmallButton("Allow Other Devices and Listen Again")) {
                    ui.lan[ui.source] = true;
                    ui.sock.close();
                    ui.sock.open(ui.port, true, ui.error);
                    ui.listen_start = now;
                }
            }
        }

        // Firewall.
        const bool fw_on = ui.firewall == Firewall::Ufw || ui.firewall == Firewall::Firewalld;
        const bool blocking = fw_on && listening && lan && !ui.remote_seen && ui.allowed_port != ui.port &&
                              now - ui.listen_start > 5 && !got;
        if (ui.remote_seen)
            row(0, "Firewall: open (a device on your network got through)");
        else if (!ui.detected)
            row(2, "Firewall: checking...");
        else if (ui.firewall == Firewall::Off)
            row(0, "Firewall: none found, nothing to change");
        else if (ui.firewall == Firewall::Unknown) {
#if defined(_WIN32)
            row(2, "Firewall: Windows asks the first time you press Listen. If you pressed Cancel, allow VATs in "
                   "Windows Security > Firewall & network protection > Allow an app through firewall.");
#elif defined(__APPLE__)
            row(2, "Firewall: macOS asks the first time you press Listen. If you pressed Don't Allow, allow VATs in "
                   "System Settings > Network > Firewall > Options.");
#else
            row(2, "Firewall: unknown");
#endif
        } else if (blocking) {
            row(1, "Your firewall (" + std::string(firewall_name(ui.firewall)) + ") is probably blocking the phone.");
        } else if (ui.allowed_port == ui.port) {
            row(0, "Firewall: your home network may send to port " + std::to_string(ui.port));
        } else {
            row(2, "Firewall: " + std::string(firewall_name(ui.firewall)) +
                       " is on. If nothing arrives, VATs offers to let your home network in.");
        }
        if (blocking && !ui.addrs.empty()) {
            const std::string subnet = ui.addrs.front().subnet;  // ponytail: first network only; add a picker if people have two
            ImGui::BeginDisabled(ui.allow_job.valid());
            if (ImGui::Button(ui.allow_job.valid() ? "Waiting for Your Password..." : "Allow on My Home Network")) {
                const std::string cmd = allow_command(ui.firewall, ui.port, subnet, "pkexec");
                ui.allow_message.clear();
                ui.allow_job = std::async(std::launch::async, [cmd, run = host_.command_runner()] {
                    std::string out;
                    return run(cmd, out);
                });
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Lets devices on %s send to UDP port %d. Your system asks for your password.",
                                  subnet.c_str(), ui.port);
            std::string typed = allow_command(ui.firewall, ui.port, subnet, "sudo");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Or type it yourself:");
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Command")) {
                ImGui::SetClipboardText(typed.c_str());
                status("Copied the firewall command");
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##fwcmd", typed.data(), typed.size() + 1, ImGuiInputTextFlags_ReadOnly);
        }
        if (!ui.allow_message.empty())
            ImGui::TextColored(ui.allow_failed ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.45f, 0.82f, 0.5f, 1), "%s",
                               ui.allow_message.c_str());

        if (ui.source == 2)
            row(got ? 0 : ui.phone_ip.empty() ? 1 : 2,
                ui.phone_ip.empty() ? "Phone address: type the address the iFacialMocap app shows, then press Connect to iPhone."
                                    : "Phone address: " + ui.phone_ip + (got ? "" : " (press Connect to iPhone)"));
        if (!listening) row(2, "Receiving: not listening");
        else if (!got) row(listening && now - ui.listen_start > 5 ? 1 : 2, "Receiving: waiting for the sender...");
        else if (now - ui.last_packet > 1)
            row(1, "Receiving: nothing for " + std::to_string(int(now - ui.last_packet)) + " s (last from " + ui.sender + ")");
        else
            row(0, "Receiving " + std::to_string(ui.pps) + " packets/s from " + ui.sender);
    }

    // Connection.
    ImGui::SeparatorText("Connection");
    const bool open = ui.sock.is_open();
    ImGui::BeginDisabled(open);
    label("Source", ImGui::GetFontSize() * 11);
    const int previous = ui.source;
    if (ImGui::Combo("##source", &ui.source, "VMC protocol\0Rokoko Studio Live\0iFacialMocap (iPhone)\0")) {
        const int defaults[3] = {39539, kRokokoPort, kIFacialMocapPort};
        if (ui.port == defaults[previous]) ui.port = defaults[ui.source];  // keep a port the user chose
        ui.rest_from_pose = false;
    }
    label("Port", ImGui::GetFontSize() * 6);
    ImGui::InputInt("##port", &ui.port, 0);
    ui.port = std::clamp(ui.port, 1, 65535);
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.source == 2);  // iFacialMocap always runs on a phone
    ImGui::Checkbox("Allow Other Devices", &ui.lan[ui.source]);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Listen on every network interface, for a phone or headset on your network.\n"
                          "Off: only apps on this computer can send.");
    ImGui::EndDisabled();
    indent();
    if (ImGui::Button(open ? "Stop Listening" : "Listen")) {
        if (open) {
            ui.rec.stop();
            ui.rec.frames.clear();
            ui.sock.close();
            ui.have_data = false;
            ui.state = {};
            ui.pps = ui.count = 0;
        } else {
            ui.error.clear();
            ui.sock.open(ui.port, ui.lan[ui.source], ui.error);
            ui.window_start = ui.listen_start = now;
            ui.remote_seen = false;
            ui.last_packet = -1;
        }
    }
    ImGui::SameLine();
    const bool live = open && ui.last_packet >= 0 && now - ui.last_packet < 1;
    ImVec2 dot = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetFontSize() * 0.3f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + r, dot.y + ImGui::GetFrameHeight() / 2), r,
                                                !open ? IM_COL32(120, 120, 120, 255)
                                                : live ? IM_COL32(110, 210, 120, 255)
                                                       : IM_COL32(230, 170, 70, 255));
    ImGui::Dummy(ImVec2(r * 2.5f, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (!open) ImGui::TextDisabled("Not listening");
    else if (ui.last_packet < 0) ImGui::TextUnformatted("Waiting for a sender...");
    else if (!live) ImGui::Text("No data for %.0f s (last: %s)", now - ui.last_packet, ui.sender.c_str());
    else ImGui::Text("%d packets/s from %s", ui.pps, ui.sender.c_str());
    if (!ui.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ui.error.c_str());
    if (open && ui.state.loaded == 0) ImGui::TextDisabled("The sender says no model is loaded.");
    if (ui.source == 2) {
        label("Phone address", ImGui::GetFontSize() * 9);
        char ip[64];
        std::snprintf(ip, sizeof ip, "%s", ui.phone_ip.c_str());
        if (ImGui::InputTextWithHint("##phone", "192.168.1.20", ip, sizeof ip)) ui.phone_ip = ip;
        ImGui::SameLine();
        ImGui::BeginDisabled(!open || ui.phone_ip.empty());
        if (ImGui::Button("Connect to iPhone")) {
            ui.error.clear();
            if (ui.sock.send_to(ui.phone_ip, kIFacialMocapPort, kIFacialMocapHello, ui.error))
                status("Asked the iPhone to start streaming");
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("iFacialMocap starts sending to this computer once it receives this request.");
    }

    // Live view and rest pose.
    ImGui::SeparatorText("Live");
    indent();
    ImGui::Checkbox("Drive the Avatar", &ui.drive);
    ImGui::SetItemTooltip("Shows the incoming motion on the avatar while listening. The clip is not changed until you record.");
    label("Rest pose");
    ImGui::TextDisabled("%s", ui.rest_from_pose     ? "captured from the performer"
                              : ui.source == 1     ? "first frame: stand in a T-pose and capture"
                                                   : "T-pose (VRM models)");
    if (open && ui.source == 1 && !ui.actor.empty()) {
        label("Actor");
        ImGui::TextUnformatted(ui.actor.c_str());
    }
    ImGui::BeginDisabled(!ui.have_data);
    if (ImGui::Button("Capture Rest Pose Now")) {
        ui.rest = ui.state;
        ui.rest.root.rot = Quat{};
        ui.rest_from_pose = true;
    }
    ImGui::SetItemTooltip("Stand in a T-pose and press this if the arms or legs come in twisted.");
    if (ui.source == 0) {  // a Rokoko joint's T-pose is not the identity, so there is nothing to reset to
        ImGui::SameLine();
        if (ImGui::Button("Reset to T-Pose")) {
            ui.rest = vmc_t_pose(ui.state);
            ui.rest_from_pose = false;
        }
    }
    ImGui::EndDisabled();

    // Face (MC-5).
    ImGui::SeparatorText("Face");
    indent();
    ImGui::Checkbox("Use Face Tracking", &ui.face_on);
    ImGui::SetItemTooltip("Turns blendshapes from the sender into Bento face-bone motion (jaw, lips, eyelids, brows,\n"
                          "cheeks, tongue). Needs a mesh head rigged to the Bento face bones to show in-world.");
    ImGui::BeginDisabled(!ui.face_on || ui.face.shapes.empty());
    label("Shapes");
    if (ui.state.blend.empty()) ImGui::TextDisabled("none received");
    else ImGui::Text("%zu from the sender", ui.state.blend.size());
    std::vector<const char*> presets;
    for (auto& [name, g] : ui.face.presets) presets.push_back(name.c_str());
    if (!presets.empty()) {
        label("Preset", ImGui::GetFontSize() * 9);
        if (ImGui::Combo("##facepreset", &ui.face_preset, presets.data(), int(presets.size()))) {
            auto it = std::next(ui.face.presets.begin(), ui.face_preset);
            ui.face_settings.gain = it->second.count("*") ? it->second.at("*") : 1.0;
            ui.face_settings.gains.clear();
            for (auto& [shape, g] : it->second)
                if (shape != "*") ui.face_settings.gains[shape] = g / std::max(ui.face_settings.gain, 1e-6);
        }
    }
    label("Strength");
    float gain = float(ui.face_settings.gain);
    if (ImGui::SliderFloat("##facegain", &gain, 0, 2, "%.2f")) ui.face_settings.gain = gain;
    label("Eye Strength");
    float eye = float(ui.face_settings.eye_gain);
    if (ImGui::SliderFloat("##eyegain", &eye, 0, 2, "%.2f")) ui.face_settings.eye_gain = eye;
    ImGui::SetItemTooltip("How far the eyes follow the tracked gaze. The eyelids follow the eyes up and down.");
    label("Eye Limit");
    float yaw = float(ui.face_settings.eye_yaw_max), pitch = float(ui.face_settings.eye_pitch_max);
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::SetNextItemWidth(half);
    if (ImGui::SliderFloat("##eyeyaw", &yaw, 5, 45, "Side %.0f°")) ui.face_settings.eye_yaw_max = yaw;
    ImGui::SetItemTooltip("The farthest the eyes turn left or right, in degrees.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    if (ImGui::SliderFloat("##eyepitch", &pitch, 5, 45, "Up/Down %.0f°")) ui.face_settings.eye_pitch_max = pitch;
    ImGui::SetItemTooltip("The farthest the eyes turn up or down, in degrees.");
    label("");
    ImGui::Checkbox("Move face bones", &ui.face_settings.positions);
    ImGui::SetItemTooltip("Moves face bones as well as turning them: smiles, brows, cheeks and lip shapes. The moves are "
                          "made for the Second Life default head. Untick it for a mesh head with its own face shape "
                          "(a furry or stylised head), which moving bones would pull out of shape.");
    if (ImGui::TreeNode("Shape Strengths")) {
        for (auto& [shape, motions] : ui.face.shapes) {
            float g = float(ui.face_settings.gains.count(shape) ? ui.face_settings.gains[shape] : 1.0);
            label(shape.c_str());
            if (ImGui::SliderFloat(("##g" + shape).c_str(), &g, 0, 2, "%.2f")) ui.face_settings.gains[shape] = g;
        }
        ImGui::TreePop();
    }
    indent();
    ImGui::BeginDisabled(ui.state.blend.empty());
    if (ImGui::Button("Capture Neutral Face")) {
        ui.face_settings.neutral.clear();
        for (auto& [name, v] : face_weights(ui.face, ui.state.blend, FaceSettings{}))
            ui.face_settings.neutral[name] = v;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Relax your face and press this: your resting expression becomes the rest pose.");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) ui.face_settings.neutral.clear();
    if (ui.source == 2) {
        indent();
        ImGui::Checkbox("Head from iPhone", &ui.face_settings.head);
        ImGui::SetItemTooltip("Keys the head's turn from iFacialMocap's head tracking.");
    }
    ImGui::EndDisabled();

    // Recording.
    ImGui::SeparatorText("Record");
    ImGui::BeginDisabled(ui.rec.active());
    label("Start at frame", ImGui::GetFontSize() * 6);
    ImGui::InputInt("##from", &ui.from);
    ui.from = std::max(ui.from, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Current")) ui.from = int(std::lround(frame_));
    label("Stop at frame", 0);
    ImGui::Checkbox("##punch", &ui.punch_out);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui.punch_out);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputInt("##to", &ui.to);
    ui.to = std::max(ui.to, ui.from);
    ImGui::EndDisabled();
    label("Countdown");
    ImGui::SliderFloat("##countdown", &ui.countdown, 0, 5, "%.0f s");
    indent();
    ImGui::Checkbox("Selected Body Parts Only", &ui.selected_only);
    if (ui.selected_only) ui.face_only = false;
    indent();
    ImGui::BeginDisabled(!ui.face_on);
    ImGui::Checkbox("Face Only", &ui.face_only);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Records only the face bones (and the head from an iPhone), over the animation already there.");
    if (ui.face_only) ui.selected_only = false;
    ImGui::SetItemTooltip("Records only the parts of the selected bones (for example the arms over an existing walk);\n"
                          "every other track is left as it is.");
    ImGui::EndDisabled();

    indent();
    if (!ui.rec.active()) {
        ImGui::BeginDisabled(!ui.have_data || (ui.table.bones.empty() && ui.face.shapes.empty()) ||
                             (ui.selected_only && selection_.empty()));
        if (ImGui::Button("Record")) {
            ui.only.clear();
            if (ui.face_only) {
                ui.only = ui.face.bones();
                if (ui.source == 2 && ui.face_settings.head) ui.only.push_back("mHead");
            }
            for (int n : ui.selected_only ? selection_ : std::vector<int>{}) {
                BodyPart part = body_part_of(skel_, n);
                part.bones.push_back(n);
                for (int b : part.bones)
                    if (std::find(ui.only.begin(), ui.only.end(), skel_[b].name) == ui.only.end()) ui.only.push_back(skel_[b].name);
            }
            playing_ = false;
            ui.report.clear();
            ui.rec.begin(now, ui.countdown, doc_.clip().fps, ui.from, ui.punch_out ? ui.to : -1);
            ui.take_actor = doc_.project.active;
            ui.take_generation = doc_generation_;
        }
        ImGui::EndDisabled();
        if (ui.selected_only && selection_.empty()) ImGui::TextDisabled("Select a bone of each part to record.");
    } else if (ui.rec.counting_down(now)) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 2.5f);
        ImGui::Text("%d", int(std::ceil(ui.rec.start - now)));
        ImGui::PopFont();
        if (ImGui::Button("Cancel")) ui.rec.stop(), ui.rec.frames.clear();
    } else {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "Recording frame %d", ui.rec.from + int(ui.rec.frames.size()) - 1);
        if (ImGui::Button("Stop")) {
            ui.rec.stop();
            commit();
        }
    }

    // Clean-up (MC-4).
    ImGui::SeparatorText("Clean-up");
    label("Smoothing");
    ImGui::SliderInt("##smooth", &ui.clean.smooth, 0, 5, ui.clean.smooth ? "%d frames" : "off");
    ImGui::SetItemTooltip("Averages each rotation with its neighbours to calm tracker jitter.");
    label("Reduce keys", 0);
    ImGui::Checkbox("##reduce", &ui.clean.reduce);
    ImGui::BeginDisabled(!ui.clean.reduce);
    float rot = float(ui.clean.rot_deg), pos = float(ui.clean.pos_m * 1000);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::DragFloat("##deg", &rot, 0.05f, 0.05f, 5, "%.2f deg")) ui.clean.rot_deg = rot;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::DragFloat("##mm", &pos, 0.1f, 0.1f, 20, "%.1f mm")) ui.clean.pos_m = pos / 1000;
    ImGui::EndDisabled();
    label("Edge blend");
    ImGui::SliderInt("##blend", &ui.clean.blend, 0, 15, ui.clean.blend ? "%d frames" : "off");
    ImGui::SetItemTooltip("Eases the start and end of a punched-in take from the animation around it, so the edges do not jump.");
    indent();
    ImGui::Checkbox("Clean Up Foot Sliding", &ui.clean.lock_feet);
    ImGui::SetItemTooltip("Holds planted feet still with leg IK where the take had them on the ground.");

    if (!ui.report.empty()) {
        ImGui::SeparatorText("Last take");
        for (auto& line : ui.report) ImGui::BulletText("%s", line.c_str());
    }
    ImGui::End();
}

}  // namespace vats
