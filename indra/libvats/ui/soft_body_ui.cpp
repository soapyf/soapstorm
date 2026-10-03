// Viewport Avatar Toolset - soft-body volumes in the editor: the share slider and avatar physics (spec 08 RM-10).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <cmath>
#include <cstdint>

#include "app.h"
#include "theme.h"
#include "widgets.h"
#include "vats/fbx.h"
#include "vats/soft_body.h"

namespace vats {

bool App::share_ready(int node) {
    const MeshBody* b = mesh_body();
    if (!b || node < 0 || !skel_[node].volume || !soft_body_name(skel_[node].name)) return false;
    int volume = -1;
    for (size_t v = 0; v < skel_.volumes().size(); ++v)
        if (skel_.volumes()[v].node == node) volume = dae_volume(skel_, int(v));
    std::string key = b->id + "|" + skel_[node].name;
    for (const std::string& path : b->parts)  // a part loaded again (Map Rig) is a new model
        key += "|" + std::to_string(reinterpret_cast<std::uintptr_t>(prop_model(path)));
    if (key == share_.key) return !share_.parts.empty();
    share_ = ShareState{key, volume, "", {}};
    for (const std::string& path : b->parts) {
        const DaeModel* m = part_source(path);  // every part's weights, hidden or not
        RigMap map;
        std::string err;
        if (!m || !read_rig_map_file(rig_map_path(path), map, err)) continue;  // only a mapped part keeps a share
        bool weighted = false;
        for (size_t i = 0; i < m->joints.size() && !weighted; ++i) weighted = m->joints[i] == volume;
        if (!weighted) continue;
        // The weights as the rig splits them: loaded again without this volume's share. Once per selection.
        RigMap plain = map;
        plain.share.erase(skel_[node].name);
        DaeModel base;
        DaeReport report;
        if (!load_rig_mapped(path, skel_, plain, base, report, err) || base.joints.size() != m->joints.size()) continue;
        if (share_.partner.empty()) share_.partner = soft_body_partner_name(skel_, soft_body_partner(skel_, base, volume));
        share_.parts.push_back({path, std::move(map), std::move(base.joints), std::move(base.weights)});
    }
    if (share_.partner.empty()) share_.parts.clear();  // weighted, but sharing no flesh with any joint
    return !share_.parts.empty();
}

void App::apply_share(double value, bool save) {
    const std::string& volume = skel_.volumes()[size_t(share_.volume - dae_root(skel_) - 1)].name;
    for (SharePart& part : share_.parts) {
        auto it = prop_sources_.find(part.path);
        if (it == prop_sources_.end() || !it->second) continue;
        DaeModel& m = *it->second;
        m.joints = part.joints, m.weights = part.weights;
        share_soft_body(skel_, m, share_.volume, value);
        part_source_changed(part.path);  // the glow, Hide Unused Bones and the floor follow
        if (!save) continue;
        if (std::fabs(value - 0.5) < 1e-6) part.map.share.erase(volume);
        else part.map.share[volume] = value;
        // The file as it is now (a look or a label may have been saved since the selection), with this share.
        RigMap now = part.map;
        std::string err;
        read_rig_map_file(rig_map_path(part.path), now, err);
        now.share = part.map.share;
        if (!write_text(rig_map_path(part.path), write_rig_map_json(now), false, err)) status("Not saved in the mapping: " + err);
    }
}

void App::draw_share_slider(int node, float label_w) {
    if (!share_ready(node)) return;
    const std::string& volume = skel_[node].name;
    const auto it = share_.parts.front().map.share.find(volume);
    const double was = it != share_.parts.front().map.share.end() ? it->second : 0.5;
    if (!share_dragging_) share_value_ = float(was);
    float& value = share_value_;
    ImGui::TextUnformatted(("Share with " + share_.partner).c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", soft_body_name(volume));
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    const std::string fmt = value <= 0.001f   ? "0: none, all on the joint"
                            : value >= 0.999f ? "1: all of it"
                            : std::fabs(value - 0.5f) < 0.001f ? "0.50: as rigged"
                                                               : "%.2f";
    if (slider_float("##share", &value, 0.f, 1.f, fmt.c_str())) apply_share(value, false);
    if (ImGui::IsItemActivated()) share_drag_before_ = was, share_dragging_ = true;
    if (ImGui::IsItemDeactivated()) {
        share_dragging_ = false;
        apply_share(value, true);
        if (std::fabs(value - share_drag_before_) > 1e-6) {
            share_undo_.push_back({mesh_body()->id, volume, share_drag_before_, value, doc_.history.undo_steps().size()});
            share_redo_.clear();
            status("Share with " + share_.partner + ": " + std::to_string(int(std::lround(value * 100))) +
                   "% of the shared flesh on " + soft_body_name(volume) + ", saved in the mapping");
        }
    }
    ImGui::SetItemTooltip("Share with %s: how much of the flesh weighted to both %s and %s follows %s rather than %s.\n"
                          "0: all of it follows %s; 0.5: as the rig splits it; 1: all of it follows %s.\n"
                          "Saved in the model's mapping, so it holds when the model loads again; flesh weighted to one "
                          "of them alone stays.",
                          share_.partner.c_str(), volume.c_str(), share_.partner.c_str(), soft_body_name(volume),
                          share_.partner.c_str(), share_.partner.c_str(), soft_body_name(volume));
}

// Share drags undo on their own (the mapping is not in the project), in turn with the project's steps.
bool App::undo_share(bool redo) {
    std::vector<ShareStep>& from = redo ? share_redo_ : share_undo_;
    std::vector<ShareStep>& to = redo ? share_undo_ : share_redo_;
    const MeshBody* b = mesh_body();
    if (from.empty() || from.back().depth != doc_.history.undo_steps().size() || !b || b->id != from.back().body) return false;
    const ShareStep s = from.back();
    const int node = skel_.find(s.volume);
    if (!share_ready(node)) return false;
    from.pop_back();
    to.push_back(s);
    apply_share(redo ? s.after : s.before, true);
    status(std::string(redo ? "Redid" : "Undid") + " Share with " + share_.partner + " on " + soft_body_name(s.volume));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Avatar physics: SL's bounce on BELLY, BUTT and the PECs (dynamics.h), previewed and baked.

namespace {

// SL's Edit Physics tabs, part by part.
struct PartTabs {
    const char* part;
    PhysicsPart AvatarPhysics::*settings;
    std::vector<std::pair<const char*, PhysicsAxis PhysicsPart::*>> tabs;
};
const std::vector<PartTabs>& part_tabs() {
    static const std::vector<PartTabs> t = {
        {"Breasts", &AvatarPhysics::breasts,
         {{"Breast Bounce", &PhysicsPart::updown}, {"Breast Cleavage", &PhysicsPart::inout}, {"Breast Sway", &PhysicsPart::leftright}}},
        {"Belly", &AvatarPhysics::belly, {{"Belly Bounce", &PhysicsPart::updown}}},
        {"Butt", &AvatarPhysics::butt, {{"Butt Bounce", &PhysicsPart::updown}, {"Butt Sway", &PhysicsPart::leftright}}}};
    return t;
}

Json part_json(const PhysicsPart& p) {
    Json o = Json::object();
    o.set("mass", p.mass), o.set("gravity", p.gravity), o.set("drag", p.drag);
    for (auto [name, a] : {std::pair{"updown", &p.updown}, {"inout", &p.inout}, {"leftright", &p.leftright}}) {
        Json x = Json::object();
        x.set("max_effect", a->max_effect), x.set("spring", a->spring), x.set("gain", a->gain), x.set("damping", a->damping);
        o.set(name, std::move(x));
    }
    return o;
}

void read_part(const Json* o, PhysicsPart& p) {
    if (!o || !o->is_object()) return;
    auto num = [](const Json* j, const char* key, double& out) {
        if (const Json* v = j->find(key); v && v->is_number() && std::isfinite(v->num)) out = v->num;
    };
    num(o, "mass", p.mass), num(o, "gravity", p.gravity), num(o, "drag", p.drag);
    for (auto [name, a] : {std::pair{"updown", &p.updown}, {"inout", &p.inout}, {"leftright", &p.leftright}})
        if (const Json* x = o->find(name); x && x->is_object())
            num(x, "max_effect", a->max_effect), num(x, "spring", a->spring), num(x, "gain", a->gain), num(x, "damping", a->damping);
}

}  // namespace

AvatarPhysics& App::avatar_physics() {
    if (!physics_loaded_) {
        physics_loaded_ = true;
        for (const auto& [name, p] : avatar_physics_presets())
            if (name == "Natural") physics_ = p;
        const Json& j = settings_.physics;
        if (const Json* preset = j.find("preset"); preset && preset->is_string()) physics_preset_ = preset->str;
        read_part(j.find("breasts"), physics_.breasts);
        read_part(j.find("belly"), physics_.belly);
        read_part(j.find("butt"), physics_.butt);
    }
    return physics_;
}

void App::save_avatar_physics() {
    Json j = Json::object();
    j.set("preset", physics_preset_);
    j.set("breasts", part_json(physics_.breasts)), j.set("belly", part_json(physics_.belly)), j.set("butt", part_json(physics_.butt));
    settings_.physics = std::move(j);
    save_settings();
}

std::vector<std::string> App::physics_volumes() const {
    std::vector<std::string> out;
    for (const std::string& v : avatar_physics_volumes())
        if (const int n = skel_.find(v); n >= 0 && (!mesh_body() || is_joint_weighted(n))) out.push_back(v);
    return out;
}

// While playing, the bounce runs in real time as the viewer's does. Scrubbing steps it by the clip time scrubbed; a jump
// to a frame further away (or back) plays the second before it, so the frame shows the bounce it would have when played.
// On a still frame (a drag, or settling) it runs in real time. Headless runs (screenshots) never take real time, so the
// picture is the clip's.
void App::apply_avatar_physics_preview(Evaluation& e) {
    if (!settings_.avatar_physics) {
        physics_sim_.reset();
        physics_settled_ = true;
        return;
    }
    std::vector<DynChain> chains;
    for (const std::string& v : physics_volumes()) chains.push_back(avatar_physics_chain(v, avatar_physics()));
    const Clip& clip = doc_.clip();
    const double fps = std::max(clip.fps, 1), step = 1.0 / DynSim::kStepsPerSecond;
    const double moved = frame_ - physics_frame_;
    const auto now = std::chrono::steady_clock::now();
    const double real = std::clamp(std::chrono::duration<double>(now - physics_time_).count(), 0.0, 0.1);
    physics_time_ = now;
    auto at = [&](double f) { return vats::evaluate(*rig_, clip, f, shape(), constraints()).globals; };
    if (!physics_sim_ || chains != physics_chains_ || shape() != physics_shape_ || physics_doc_ != doc_generation_ ||
        (!playing_ && (moved < -2 || moved > 10))) {
        physics_sim_ = std::make_unique<DynSim>(skel_, chains, shape());
        physics_chains_ = chains, physics_shape_ = shape(), physics_doc_ = doc_generation_;
        const double from = std::max(0.0, frame_ - fps), roll = 1.0 / 60;  // SL's own rate is the viewer's frame rate
        physics_sim_->reset(at(from));
        for (double f = from + fps * roll; f < frame_ - 1e-9; f += fps * roll) physics_sim_->step(at(f), roll);
        physics_sim_->step(e.globals, roll);
    } else if (!playing_ && moved != 0) {
        const int n = std::max(1, int(std::lround(std::fabs(moved) / fps / step)));
        for (int s = 1; s <= n; ++s) physics_sim_->step(s == n ? e.globals : at(physics_frame_ + moved * s / n), std::fabs(moved) / fps / n);
    } else if (!headless_) {
        physics_sim_->step(e.globals, real);
    }
    physics_frame_ = frame_;
    physics_settled_ = physics_sim_->max_speed() < 0.002;
    physics_sim_->apply(e.globals, e.pose);
    e.globals = skel_.global_pose(e.pose, shape());
}

void App::draw_avatar_physics() {
    subheading("Avatar physics (SL)");
    hint("SL bounces these in-world with each wearer's Physics; preview it here.");
    if (ImGui::Checkbox("Preview avatar physics", &settings_.avatar_physics)) save_settings();
    ImGui::SetItemTooltip("While playing, scrubbing or dragging (Tools > Avatar Physics Preview). Nothing is keyed.");
    AvatarPhysics& ph = avatar_physics();
    for (const auto& [name, preset] : avatar_physics_presets()) {
        if (&name != &avatar_physics_presets().front().first) ImGui::SameLine();
        const bool on = physics_preset_ == name;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(name.c_str())) ph = preset, physics_preset_ = name, save_avatar_physics();
        if (on) ImGui::PopStyleColor();
    }
    const float label_w = ImGui::GetFontSize() * 5.5f;
    auto slider = [&](const char* label, double& v, float lo, float hi, const char* fmt, const char* tip) {
        float f = float(v);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (slider_float((std::string("##") + label).c_str(), &f, lo, hi, fmt)) v = f, physics_preset_.clear();
        ImGui::SetItemTooltip("%s", tip);
        if (ImGui::IsItemDeactivatedAfterEdit()) save_avatar_physics();
    };
    for (const PartTabs& part : part_tabs()) {
        PhysicsPart& p = ph.*part.settings;
        for (const auto& [tab, axis] : part.tabs) {
            ImGui::PushID(tab);
            if (section_header(tab, false)) {
                PhysicsAxis& a = p.*axis;
                slider("Max Effect", a.max_effect, 0, 3, "%.2f", "How far it may move; 0 turns it off (SL's default)");
                slider("Spring", a.spring, 0, 100, "%.1f", "How hard it springs back to rest");
                slider("Gain", a.gain, 1, 100, "%.1f", "How much the body's motion throws it");
                slider("Damping", a.damping, 0, 1, "%.2f", "How soon it calms down");
            }
            ImGui::PopID();
        }
    }
    if (section_header("Advanced Parameters", false)) {
        for (const PartTabs& part : part_tabs()) {
            PhysicsPart& p = ph.*part.settings;
            ImGui::PushID(part.part);
            ImGui::TextDisabled("%s", part.part);
            slider("Mass", p.mass, 0.1f, 1, "%.2f", "Heavier parts move more slowly");
            slider("Gravity", p.gravity, 0, 30, "%.1f", "How much it sags");
            slider("Drag", p.drag, 0, 10, "%.2f", "Air resistance: the faster the body moves, the more it pushes back");
            ImGui::PopID();
        }
    }
    const std::vector<std::string> volumes = physics_volumes();
    std::string names;
    for (const std::string& v : volumes) names += (names.empty() ? "" : ", ") + v;
    ImGui::BeginDisabled(volumes.empty());
    if (ImGui::Button("Bake Bounce into Keys")) {
        graph_.snapshot_curves(doc_.clip());  // PT-4
        int n = 0;
        edit("Bake Bounce", [&](Clip& c) { n = bake_avatar_physics(c, *rig_, export_shape(), ph, volumes, spare_match_loop_); });
        status("Baked the bounce on " + std::to_string(n) + " soft-body volumes as position keys (one undo step)");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Off unless you want it: SL's own avatar physics already bounces these in-world with each "
                          "wearer's settings,\nand baked keys add to that. Bake for a creature or a pose where the bounce "
                          "is part of the motion.\nWrites position keys on %s only; again: baked afresh from the keys "
                          "before. Unbake in the list below.", names.empty() ? "none of them" : names.c_str());
    ImGui::Separator();
}

}  // namespace vats
