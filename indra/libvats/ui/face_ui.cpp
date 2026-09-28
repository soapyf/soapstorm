// Viewport Avatar Toolset - the Face window: expression sliders, the blink/saccade/look-at layer and the look-at
// tool (spec 08 FA).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "vats/face_anim.h"

namespace vats {

struct FaceUi {
    std::string head = "\x01";  // the head `table` was loaded for (no real head at first)
    FaceTable table;
    std::string error;
    std::vector<std::string> heads;  // the heads folder's tables, by name
    std::map<std::string, double> weights;  // the sliders: ARKit shapes and VRM presets
    std::vector<double> seen;               // the face bones the sliders were last checked against
    char filter[64] = "";
    char pose_name[64] = "";
    FaceLayer tool;  // the look-at tool's target (look, point, prop, actor, bone)
    int tool_from = 0, tool_to = -1;
    float tool_max = 60, tool_weight = 1;
};

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The groups the sliders are listed in, by the ARKit name's start.
constexpr std::pair<const char*, const char*> kGroups[] = {{"Eyes", "eye"},  {"Brows", "brow"}, {"Cheeks", "cheek"},
                                                           {"Nose", "nose"}, {"Jaw", "jaw"},    {"Mouth", "mouth"},
                                                           {"Tongue", "tongue"}};

}  // namespace

std::string App::face_heads_dir() const {
    const std::string& user = host_.paths().user;
    return user.empty() ? "" : user + "faces/";
}

bool App::load_face_table(const std::string& head, FaceTable& out, std::string& err) const {
    const std::string path = head.empty() ? data_dir_ + "/retarget/face-arkit.json" : face_heads_dir() + head + ".json";
    const std::string text = read_file(path);
    if (text.empty()) return err = (head.empty() ? "data/retarget/face-arkit.json" : head + ".json") + " is missing", false;
    if (!parse_face_table(text, out, err)) return err = (head.empty() ? "The default head: " : head + ".json: ") + err, false;
    return true;
}

LookTarget App::look_target(const FaceLayer& t) {
    if (t.look == "point") return [p = t.point](double, Vec3& w) { return w = p, true; };
    if (t.look == "camera") return [e = camera_.eye()](double, Vec3& w) { return w = e, true; };
    if (t.look == "prop") {
        const Clip& clip = doc_.clip();
        auto it = std::find_if(clip.props.begin(), clip.props.end(), [&](const Prop& p) { return p.name == t.prop; });
        if (it == clip.props.end()) return {};
        // The prop's origin; one on a bone moves with the animation as it is now (a copy, not the keys being baked).
        return [this, prop = *it, snap = clip](double f, Vec3& w) {
            if (prop.bone.empty() && prop.point.empty()) return w = prop_frame(prop).pos, true;
            const Evaluation e = vats::evaluate(*rig_, snap, f, export_shape());
            return w = prop_frame(prop, &e.globals).pos, true;
        };
    }
    if (t.look == "actor") {
        const int i = actor_index(t.actor), bone = t.bone.empty() ? -1 : skel_.find(t.bone);
        const int le = skel_.find("mEyeLeft"), re = skel_.find("mEyeRight");
        if (i < 0 || i == doc_.project.active || (!t.bone.empty() && bone < 0) || le < 0 || re < 0) return {};
        return [this, i, bone, le, re](double f, Vec3& w) {  // "" = between their eyes
            const Evaluation e = evaluate_actor(i, f);
            return w = actor_rel(i).apply(bone >= 0 ? e.globals[bone].pos : (e.globals[le].pos + e.globals[re].pos) * 0.5), true;
        };
    }
    return {};
}

void App::look_at_partner(int actor) {
    const Project& p = doc_.project;
    if (actor < 0 || actor >= int(p.actors.size()) || actor == p.active) return;
    FaceLayer t;
    t.look = "actor", t.actor = p.actors[actor].name;
    const LookTarget look = look_target(t);
    std::vector<int> eyes;
    for (const char* e : {"mEyeLeft", "mEyeRight", "mFaceEyeAltLeft", "mFaceEyeAltRight"})
        if (int n = skel_.find(e); n > 0) eyes.push_back(n);
    std::string why;
    bool ok = false;
    edit("Look at " + t.actor, [&](Clip& c) {
        LookAtOptions head, rest;
        head.max_turn = 60, head.weight = 0.5;  // the head turns half-way, the eyes the rest
        rest.max_turn = 30;
        ok = look_at_bake(c, *rig_, {skel_.find("mHead")}, look, head, export_shape(), why) &&
             look_at_bake(c, *rig_, eyes, look, rest, export_shape(), why);
    });
    status(ok ? p.actors[p.active].name + " looks at " + t.actor + "'s eyes on every frame" : why);
}

void App::draw_face_panel() {
    if (!show_face_) return;
    if (!face_ui_) face_ui_ = std::make_shared<FaceUi>();
    FaceUi& ui = *face_ui_;
    place_tool_window(4, 24, 40);
    if (!ImGui::Begin("Face", &show_face_)) return ImGui::End();
    help_button("face-animation");
    Clip& clip = doc_.clip();

    // The head: its table maps every slider onto bones. Motion Capture uses the same one.
    const std::string head = face_head();
    auto rescan = [&] {
        ui.heads.clear();
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(u8path(face_heads_dir()), ec))
            if (e.path().extension() == ".json") {
                const std::u8string s = e.path().stem().u8string();
                ui.heads.emplace_back(s.begin(), s.end());
            }
        std::sort(ui.heads.begin(), ui.heads.end());
    };
    if (ImGui::IsWindowAppearing()) rescan();
    if (ui.head != head) {
        ui.table = FaceTable{}, ui.error.clear(), ui.seen.clear();
        load_face_table(head, ui.table, ui.error);
        ui.head = head;
    }
    const float label_w = ImGui::GetFontSize() * 6.5f;
    auto label = [&](const char* text, float w = -1) {  // label on the left (spec 06 section 1.1)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(w);
    };
    label("Head");
    if (ImGui::BeginCombo("##head", head.empty() ? "SL default head" : head.c_str())) {
        if (ImGui::Selectable("SL default head", head.empty())) set_face_head("");
        for (auto& h : ui.heads)
            if (ImGui::Selectable(h.c_str(), h == head)) set_face_head(h);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The mapping from face shapes to bones. Your own heads are JSON files in the heads folder;\n"
                          "Motion Capture uses the same head.");
    ImGui::BeginDisabled(face_heads_dir().empty());
    if (ImGui::Button("New Head")) {
        std::error_code ec;
        std::filesystem::create_directories(u8path(face_heads_dir()), ec);
        std::string name = "my head";
        for (int k = 2; std::filesystem::exists(u8path(face_heads_dir() + name + ".json"), ec); ++k)
            name = "my head " + std::to_string(k);
        std::ofstream(u8path(face_heads_dir() + name + ".json"), std::ios::binary)
            << read_file(head.empty() ? data_dir_ + "/retarget/face-arkit.json" : face_heads_dir() + head + ".json");
        rescan();
        set_face_head(name);
        status("Made " + name + ".json in the heads folder from " + (head.empty() ? "the SL default head" : head) +
               ": edit it, then press Reload");
    }
    ImGui::SetItemTooltip("Copy this head's table into the heads folder as a new head to edit");
    ImGui::SameLine();
    if (ImGui::Button("Folder")) {
        std::error_code ec;
        std::filesystem::create_directories(u8path(face_heads_dir()), ec);
        host_.open_url(folder_url(face_heads_dir()));
    }
    ImGui::SetItemTooltip("Show the heads folder (%s)", face_heads_dir().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        rescan();
        ui.head = "\x01";  // reread the table next frame
    }
    ImGui::EndDisabled();

    const bool pos = face_positions();
    bool move = pos;
    if (ImGui::Checkbox("Move face bones", &move)) set_face_positions(move), ui.seen.clear();
    ImGui::SetItemTooltip("Moves face bones as well as turning them: smiles, brows, cheeks and lip shapes. The moves are made "
                          "for the\nSecond Life default head. Off by default: leave it off for a mesh head with its own face "
                          "joint positions.\nThe same setting as in Motion Capture.");
    // "Your avatar" (03 IO-11a): position keys on a worn mesh head's face bones pull it towards the default face unless
    // the export bakes on Your avatar.
    if (pos && host_.world_view()) {
        bool face_worn = false;
        for (const std::string& j : host_.joint_overrides())
            if (int n = skel_.find(j); n >= 0 && skel_[n].category == Category::Face) face_worn = true;
        if (face_worn && bake_shape_key(clip.export_settings, exporting_yours()) != "avatar")
            ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "Your mesh head has its own face joint positions. Set Bake shape to "
                                                          "Your avatar (Properties > Export), or these moves will pull it "
                                                          "towards the default face.");
    }
    if (!ui.error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ui.error.c_str());
        ImGui::End();
        return;
    }
    const double frame = std::round(frame_);

    // Drags are one undo step each: opened on activation, committed on release (as in the Ragdoll window).
    auto track = [&](const char* step, auto& value, auto before) {
        if (ImGui::IsItemActivated()) {
            auto now = value;
            value = before;
            doc_.history.begin(clip);
            value = now;
        }
        if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, clip)) mark_dirty();
    };

    // --- Expression (FA-1..FA-4) ---
    if (ImGui::CollapsingHeader("Expression", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The sliders follow the keys: while they still give the keys at this frame they stay as set; otherwise they
        // are read back from the keys (an undo, another frame, an edit elsewhere).
        if (!doc_.history.is_open()) {
            std::vector<double> now = face_bone_values(clip, ui.table, frame, pos);
            if (now != ui.seen) {
                ui.seen = std::move(now);
                if (!face_weights_match(clip, ui.table, ui.weights, pos, frame))
                    ui.weights = read_face_weights(clip, ui.table, frame, pos);
            }
        }
        hint("Each slider keys the face bones at this frame. Poses and shapes read back from the keys.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##facefilter", "Filter shapes...", ui.filter, sizeof ui.filter);
        ImGui::SameLine();
        if (ImGui::Button("Reset Face")) {
            edit("Reset Face", [&](Clip& c) { key_face_weights(c, ui.table, {}, pos, frame); });
            ui.weights.clear();
        }
        ImGui::SetItemTooltip("Key every face bone at rest at this frame");
        const std::string filter = ui.filter;
        auto matches = [&](const std::string& name) {
            if (filter.empty()) return true;
            auto low = [](std::string s) {
                for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                return s;
            };
            return low(name).find(low(filter)) != std::string::npos;
        };
        auto slider = [&](const std::string& name, bool keys) {
            ImGui::PushID(name.c_str());
            ImGui::BeginDisabled(!keys);
            double& w = ui.weights[name];
            float v = float(w);
            label(name.c_str());
            const bool changed = ImGui::SliderFloat("##w", &v, 0, 1, "%.2f");
            if (ImGui::IsItemActivated()) doc_.history.begin(clip);  // before the first key of the drag
            if (changed) {
                const std::map<std::string, double> before = ui.weights;
                w = std::clamp(double(v), 0.0, 1.0);
                key_face_weights(clip, ui.table, ui.weights, pos, frame, &before);
                ui.seen = face_bone_values(clip, ui.table, frame, pos);
            }
            if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit("Face Shape", clip)) mark_dirty();
            ImGui::EndDisabled();
            if (!keys) ImGui::SetItemTooltip("This shape only moves face bones: turn on Move face bones to use it");
            ImGui::PopID();
        };
        for (auto& [group, prefix] : kGroups) {
            std::vector<std::string> names;
            for (auto& [shape, motions] : ui.table.shapes)
                if (shape.rfind(prefix, 0) == 0 && matches(shape)) names.push_back(shape);
            if (names.empty()) continue;
            if (!filter.empty()) ImGui::SetNextItemOpen(true);
            if (ImGui::TreeNode(group)) {
                for (auto& n : names) slider(n, face_shape_keys(ui.table, n, pos));
                ImGui::TreePop();
            }
        }
        std::vector<std::string> presets;
        for (auto& [name, shapes] : ui.table.aliases)
            if (matches(name)) presets.push_back(name);
        if (!presets.empty()) {
            if (!filter.empty()) ImGui::SetNextItemOpen(true);
            if (ImGui::TreeNode("VRM presets")) {
                for (auto& n : presets) {
                    bool keys = false;
                    for (auto& [shape, k] : ui.table.aliases.at(n)) keys |= face_shape_keys(ui.table, shape, pos);
                    slider(n, keys);
                }
                ImGui::TreePop();
            }
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##facepose", "Pose name", ui.pose_name, sizeof ui.pose_name);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ui.pose_name[0]);
        if (ImGui::Button("Save Face Pose")) {
            LibraryItem it = make_face_pose(clip, ui.table, frame, pos);
            it.name = ui.pose_name;
            store_library_item(std::move(it));
            status("Saved face pose " + std::string(ui.pose_name) + " to Inventory > Poses");
            ui.pose_name[0] = 0;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Keep this frame's face in the pose library; apply it like any pose");
    }

    // The target picker the layer and the look-at tool share. set() makes a discrete change; drags edit t in place and
    // report through dragged().
    auto target_ui = [&](FaceLayer& t, bool none, const std::function<void(const std::function<void(FaceLayer&)>&)>& set,
                         const std::function<void(const Vec3&)>& dragged) {
        static const char* const kinds[] = {"", "point", "prop", "camera", "actor"};
        static const char* const names[] = {"Nothing", "A point", "A prop", "The camera", "Another actor"};
        int k = 0;
        for (int i = 0; i < 5; ++i)
            if (t.look == kinds[i]) k = i;
        label("Look at");
        if (ImGui::BeginCombo("##lookkind", names[k])) {
            for (int i = none ? 0 : 1; i < 5; ++i)
                if (ImGui::Selectable(names[i], i == k)) set([i](FaceLayer& x) { x.look = kinds[i]; });
            ImGui::EndCombo();
        }
        if (t.look == "point") {
            const Vec3 before = t.point;
            float p[3] = {float(t.point.x), float(t.point.y), float(t.point.z)};
            label("Point");
            if (ImGui::DragFloat3("##lookpoint", p, 0.01f, -50, 50, "%.2f m")) t.point = {p[0], p[1], p[2]};
            ImGui::SetItemTooltip("Avatar space: X forward, Y left, Z up, from the ground under the hips");
            dragged(before);
            const int sel = primary();
            ImGui::BeginDisabled(sel < 0);
            if (ImGui::SmallButton("Use the Selected Bone's Position"))
                set([pt = globals_[std::max(sel, 0)].pos](FaceLayer& x) { x.point = pt; });
            ImGui::EndDisabled();
        } else if (t.look == "prop") {
            label("Prop");
            if (ImGui::BeginCombo("##lookprop", t.prop.empty() ? "(choose)" : t.prop.c_str())) {
                for (const Prop& p : clip.props)
                    if (ImGui::Selectable(p.name.c_str(), p.name == t.prop)) set([n = p.name](FaceLayer& x) { x.prop = n; });
                ImGui::EndCombo();
            }
        } else if (t.look == "camera") {
            hint("Where the camera is when you bake.");
        } else if (t.look == "actor") {
            label("Actor");
            if (ImGui::BeginCombo("##lookactor", t.actor.empty() ? "(choose)" : t.actor.c_str())) {
                const auto& as = doc_.project.actors;
                for (int i = 0; i < int(as.size()); ++i)
                    if (i != doc_.project.active && ImGui::Selectable(as[i].name.c_str(), as[i].name == t.actor))
                        set([n = as[i].name](FaceLayer& x) { x.actor = n; });
                ImGui::EndCombo();
            }
            if (!multi_actor()) ImGui::SetItemTooltip("Add a partner in Tools > Actors first");
            label("Their bone");
            if (ImGui::BeginCombo("##lookbone", t.bone.empty() ? "(between the eyes)" : t.bone.c_str(),
                                  ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("(between the eyes)", t.bone.empty())) set([](FaceLayer& x) { x.bone.clear(); });
                for (int n = 0; n < skel_.size(); ++n)
                    if (!skel_[n].volume && ImGui::Selectable(skel_[n].name.c_str(), skel_[n].name == t.bone))
                        set([b = skel_[n].name](FaceLayer& x) { x.bone = b; });
                ImGui::EndCombo();
            }
        }
    };

    // --- Blink, eye-dart and look-at layer (FA-5..FA-7) ---
    if (ImGui::CollapsingHeader("Blinks, Eye Darts and Look-At")) {
        hint("A layer that blinks, darts the eyes and looks at a target, baked onto the eyes, the eyelids and the head.");
        if (!clip.face_layer) {
            if (ImGui::Button("Add Layer")) edit("Face Layer", [](Clip& c) { c.face_layer = FaceLayer{}; });
            ImGui::SetItemTooltip("Off until you add it; nothing changes until you bake");
        } else {
            FaceLayer& L = *clip.face_layer;
            auto set = [&](const std::function<void(FaceLayer&)>& change) {
                edit("Face Layer", [&](Clip& c) { change(*c.face_layer); });
            };
            int seed = int(L.seed);
            label("Seed", ImGui::GetFontSize() * 6);
            if (ImGui::InputInt("##seed", &seed) && seed >= 0) set([seed](FaceLayer& x) { x.seed = std::uint32_t(seed); });
            ImGui::SetItemTooltip("The same seed always bakes the same blinks and eye darts");
            ImGui::SameLine();
            if (ImGui::Button("New Seed"))
                set([s = std::uint32_t(host_.ticks_ns() / 1000 % 1000000 + 1)](FaceLayer& x) { x.seed = s; });

            bool blinks = L.blinks;
            if (ImGui::Checkbox("Blinks", &blinks)) set([blinks](FaceLayer& x) { x.blinks = blinks; });
            ImGui::BeginDisabled(!L.blinks);
            {
                const double a0 = L.blink_min, b0 = L.blink_max;
                float lo = float(L.blink_min), hi = float(L.blink_max);
                label("Every");
                if (ImGui::DragFloatRange2("##blinkevery", &lo, &hi, 0.05f, 0.5f, 20, "%.1f s", "%.1f s"))
                    L.blink_min = lo, L.blink_max = std::max(lo, hi);
                ImGui::SetItemTooltip("A blink comes every this many seconds, at random in the range");
                if (ImGui::IsItemActivated()) {
                    const double a = L.blink_min, b = L.blink_max;
                    L.blink_min = a0, L.blink_max = b0;
                    doc_.history.begin(clip);
                    L.blink_min = a, L.blink_max = b;
                }
                if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit("Face Layer", clip)) mark_dirty();
                const double l0 = L.blink_length;
                float len = float(L.blink_length);
                label("Blink length");
                if (ImGui::SliderFloat("##blinklen", &len, 0.1f, 0.6f, "%.2f s")) L.blink_length = len;
                ImGui::SetItemTooltip("From the lids starting to close to open again");
                track("Face Layer", L.blink_length, l0);
            }
            ImGui::EndDisabled();

            bool darts = L.saccades;
            if (ImGui::Checkbox("Eye darts", &darts)) set([darts](FaceLayer& x) { x.saccades = darts; });
            ImGui::SetItemTooltip("Saccades: quick jumps of the eyes between still moments");
            ImGui::BeginDisabled(!L.saccades);
            {
                const double i0 = L.saccade_interval, e0 = L.eye_limit;
                float iv = float(L.saccade_interval), el = float(L.eye_limit);
                label("Hold");
                if (ImGui::SliderFloat("##darthold", &iv, 0.2f, 4, "%.2f s")) L.saccade_interval = iv;
                ImGui::SetItemTooltip("The typical time the eyes rest between darts (half are shorter)");
                track("Face Layer", L.saccade_interval, i0);
                label("Eye limit");
                if (ImGui::SliderFloat("##eyelimit", &el, 1, 30, "%.0f\xc2\xb0")) L.eye_limit = el;
                ImGui::SetItemTooltip("No dart takes the eyes further than this from where they look");
                track("Face Layer", L.eye_limit, e0);
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText("Look at");
            target_ui(L, true, set, [&](const Vec3& before) { track("Face Layer", L.point, before); });
            ImGui::BeginDisabled(L.look.empty());
            {
                const double s0 = L.head_share, m0 = L.head_max;
                float share = float(L.head_share), hm = float(L.head_max);
                label("Head turns");
                if (ImGui::SliderFloat("##headshare", &share, 0, 1, "%.2f")) L.head_share = share;
                ImGui::SetItemTooltip("The share of the turn the head takes; the eyes turn the rest (0 = eyes only)");
                track("Face Layer", L.head_share, s0);
                label("Head limit");
                if (ImGui::SliderFloat("##headmax", &hm, 0, 90, "%.0f\xc2\xb0")) L.head_max = hm;
                ImGui::SetItemTooltip("The head turns at most this far from straight ahead");
                track("Face Layer", L.head_max, m0);
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::Button(L.baked ? "Re-bake" : "Bake")) {
                const LookTarget look = look_target(L);
                edit("Bake Face Layer", [&](Clip& c) { bake_face_layer(c, *rig_, export_shape(), ui.table, pos, look); });
                status(!L.look.empty() && !look ? "Baked the face layer; the look-at target was not found, so it looks ahead"
                                                : "Baked the face layer to keys");
            }
            ImGui::SetItemTooltip("Write the layer as keys (one undo step); re-baking starts again from the keys before the "
                                  "first bake");
            ImGui::SameLine();
            if (ImGui::Button("Clear")) edit("Clear Face Layer", [&](Clip& c) {
                unbake_face_layer(c, skel_, ui.table);
                c.face_layer.reset();
            });
            ImGui::SetItemTooltip("Put back the eye, eyelid and head keys from before the bake and remove the layer");
        }
    }

    // --- Look-at tool (FA-8) ---
    if (ImGui::CollapsingHeader("Look At")) {
        hint("Turns the selected head or eyes towards a target on every frame of the range, like Follow Target.");
        FaceLayer& t = ui.tool;
        if (t.look.empty()) t.look = "point", t.point = {2, 0, 1.7};
        target_ui(t, false, [&](const std::function<void(FaceLayer&)>& change) { change(t); }, [](const Vec3&) {});
        if (ui.tool_to < 0 || ui.tool_to > clip.end_frame) ui.tool_to = clip.end_frame;
        ui.tool_from = std::clamp(ui.tool_from, 0, ui.tool_to);
        label("Frames");
        ImGui::DragIntRange2("##lookframes", &ui.tool_from, &ui.tool_to, 0.2f, 0, clip.end_frame);
        label("Max turn");
        ImGui::SliderFloat("##lookmax", &ui.tool_max, 0, 90, "%.0f\xc2\xb0");
        ImGui::SetItemTooltip("Each bone turns at most this far from straight ahead");
        label("Weight");
        ImGui::SliderFloat("##lookweight", &ui.tool_weight, 0, 1, "%.2f");
        ImGui::SetItemTooltip("How far from the animation towards the target: 1 looks straight at it");
        ImGui::BeginDisabled(selection_.empty());
        if (ImGui::Button("Look at Target")) {
            LookAtOptions opt;
            opt.from = ui.tool_from, opt.to = ui.tool_to, opt.max_turn = ui.tool_max, opt.weight = ui.tool_weight;
            const LookTarget look = look_target(t);
            std::string why;
            bool ok = false;
            edit("Look At", [&](Clip& c) { ok = look_at_bake(c, *rig_, selection_, look, opt, export_shape(), why); });
            status(ok ? "Keyed the look-at on frames " + std::to_string(ui.tool_from) + " to " + std::to_string(ui.tool_to)
                      : why);
        }
        ImGui::EndDisabled();
        if (selection_.empty()) ImGui::SetItemTooltip("Select the head, or the eyes, first");
    }
    ImGui::End();
}

}  // namespace vats
