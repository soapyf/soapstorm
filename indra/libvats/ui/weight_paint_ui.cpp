// Viewport Avatar Toolset - Paint Weights: touching up a rigged body's weights with a brush in the view.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 6 (RG-15). The brush is the core's (weight_paint.h). It paints the selected bone on the
// body as you see it, posed or playing: a hit on the posed mesh is carried back to the rest pose through the triangle
// it lands on, so a bent elbow is painted where it bends. Each stroke is one undo step (beside the project's own, as
// the share slider's are) and is saved in the model's mapping at once. The weight glow shows the bone painted.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/fluid_pose.h"
#include "vats/rig_export.h"
#include "vats/rig_map.h"
#include "vats/weight_paint.h"
#include "widgets.h"

namespace vats {

struct PaintUi {
    bool on = true;  // a left drag on the body paints (off: the view picks and poses as usual)
    // A press on a bone waits: let go without moving and it picks that bone; move and it paints from where it was pressed.
    struct Press {
        bool on = false;
        ImVec2 at;
        int bone = -1;
        std::string path;
        Vec3 rest;
        int seed = -1;
    } press;
    PaintBrush brush;
    bool mirror = true;
    bool local_mesh = false;  // the viewer: shows the body on your avatar through Local Mesh after each stroke
    // A part's working copy: its source with its shape keys applied and every part kept, so it numbers its vertices as
    // the source does; the brush, its neighbours, and which vertices belong to hidden parts (left alone).
    struct Work {
        std::string key;
        DaeModel model;
        PaintMesh mesh;
        std::vector<char> hidden;
        std::vector<std::uint32_t> source_of;  // per vertex of the shown model: the source vertex
    };
    std::map<std::string, Work> work;
    // The stroke under way.
    bool stroking = false;
    std::string path, body;  // the part and the body painted
    int joint = -1;
    PaintOp op = PaintOp::Add;
    std::vector<int> jb;
    std::vector<float> wb;
    Vec3 last;
    int dabs = 0;
    // The brush under the pointer: where on the posed body, and its size there in pixels.
    bool hover = false;
    Vec3 hover_at;
    float hover_px = 0;
    struct Step {
        std::string body, path;
        PaintStroke stroke;
        size_t depth = 0;
    };
    std::vector<Step> undo, redo;
    std::string note;
};

namespace {

std::string stem_of(const std::string& path) {
    const std::string name = path.substr(path.find_last_of("/\\") + 1);
    return name.substr(0, name.rfind('.'));
}

const char* op_name(PaintOp op) { return op == PaintOp::Add ? "Add" : op == PaintOp::Subtract ? "Subtract" : "Smooth"; }

}  // namespace

bool App::painting() const { return show_paint_ && paint_visible_ && paint_ui_ && paint_ui_->on && mesh_body(); }

namespace {

// The parts of the shown body the brush can paint: any rigged part loaded from a file (rigged from scratch here, mapped
// with Map Rig, or rigged to SL's own names). The painted weights are kept in the mapping file beside it, over the
// file's own; the file itself is never changed.
std::vector<std::string> paintable(const std::vector<std::string>& body_parts, const std::map<std::string, DaeReport>& reports,
                                   const std::map<std::string, std::unique_ptr<DaeModel>>& sources) {
    std::vector<std::string> out;
    for (const std::string& path : body_parts) {
        const auto r = reports.find(path);
        const auto s = sources.find(path);
        std::error_code ec;
        if (r != reports.end() && s != sources.end() && s->second && s->second->rigged && std::filesystem::is_regular_file(path, ec))
            out.push_back(path);
    }
    return out;
}


}  // namespace

// Keeps a part's painted weights in its mapping file: the rig from scratch's own weights, or an overlay on a body rigged
// any other way. A mapping file that is there but unreadable is left alone rather than overwritten.
bool App::save_painted(const std::string& path, const DaeModel& src, std::string& err) {
    const std::string file = rig_map_path(path);
    RigMap map;
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !read_rig_map_file(file, map, err)) return false;
    map.scratch.wjoints = src.joints, map.scratch.weights = src.weights, map.scratch.painted = true;
    return write_text(file, write_rig_map_json(map), false, err);
}

void App::forget_paint(const std::string& path) {
    if (!paint_ui_) return;
    PaintUi& ui = *paint_ui_;
    if (ui.stroking && (path.empty() || ui.path == path)) ui.stroking = false;
    auto gone = [&](const PaintUi::Step& s) { return path.empty() || s.path == path; };
    std::erase_if(ui.undo, gone);
    std::erase_if(ui.redo, gone);
    if (path.empty()) ui.work.clear();
    else ui.work.erase(path);
}

bool App::undo_paint(bool redo) {
    if (!paint_ui_ || paint_ui_->stroking) return false;  // a stroke under way finishes first (on release)
    PaintUi& ui = *paint_ui_;
    std::vector<PaintUi::Step>& from = redo ? ui.redo : ui.undo;
    std::vector<PaintUi::Step>& to = redo ? ui.undo : ui.redo;
    const MeshBody* body = mesh_body();
    if (from.empty() || from.back().depth != doc_.history.undo_steps().size() || !body || body->id != from.back().body) return false;
    PaintUi::Step s = from.back();
    DaeModel* src = part_source(s.path);
    if (!src) return false;
    from.pop_back();
    undo_stroke(s.stroke, *src, redo);
    ui.work.erase(s.path);
    part_source_changed(s.path);
    std::string err;
    if (!save_painted(s.path, *src, err)) status("Not saved in the mapping: " + err);
    to.push_back(std::move(s));
    status(std::string(redo ? "Redid" : "Undid") + " a weight stroke");
    return true;
}

bool App::can_undo_paint(bool redo) const {
    if (!paint_ui_) return false;
    const std::vector<PaintUi::Step>& s = redo ? paint_ui_->redo : paint_ui_->undo;
    const MeshBody* body = mesh_body();
    return !paint_ui_->stroking && !s.empty() && s.back().depth == doc_.history.undo_steps().size() && body && body->id == s.back().body;
}

namespace {

// The working copy of a part for the brush, made again when the part or its look changes.
PaintUi::Work& work_for(PaintUi& ui, const std::string& path, const DaeModel& src, const MeshLook& look, unsigned generation) {
    // The model's address and the app's mesh generation (bumped as a mesh is loaded or rebuilt): a reload that lands
    // at the same address is still a new model.
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&src)) + "@" + std::to_string(generation) + "|";
    for (const std::string& h : look.hidden) key += h + ",";
    for (const auto& [k, v] : look.keys) key += k + "=" + std::to_string(v) + ",";
    PaintUi::Work& w = ui.work[path];
    if (w.key == key && w.model.joints.size() == src.joints.size()) return w;
    w.key = key;
    MeshLook keys = look;
    keys.hidden.clear();
    w.model = shown_model(src, keys);
    w.mesh = paint_mesh(w.model);
    w.hidden.assign(size_t(w.model.vertex_count()), 0);
    for (const DaePart& p : src.parts)
        if (look.hidden.count(p.name))
            for (std::uint32_t v = p.first_vertex; v < p.first_vertex + p.vertex_count && v < w.hidden.size(); ++v) w.hidden[v] = 1;
    w.source_of = shown_vertex_sources(src, look);
    return w;
}

// A dab, and its mirror on the same part when mirroring: seed is the source vertex the brush is on.
void dab_on(const Skeleton& skel, PaintUi::Work& w, int joint, const Vec3& at, int seed, const PaintBrush& brush, bool mirror) {
    paint_dab(skel, w.model, w.mesh, joint, at, brush, seed);
    // A bone on the middle (mChest) mirrors onto itself: a dab reaching the middle would paint there twice.
    if (!mirror || (mirror_joint(skel, joint) == joint && std::fabs(at.y) < brush.radius)) return;
    const DaePart* part = nullptr;
    for (const DaePart& p : w.model.parts)
        if (seed >= 0 && std::uint32_t(seed) >= p.first_vertex && std::uint32_t(seed) < p.first_vertex + p.vertex_count) part = &p;
    paint_dab(skel, w.model, w.mesh, mirror_joint(skel, joint), mirror_point(at), brush, nearest_vertex(w.model, mirror_point(at), part));
}

}  // namespace

bool App::paint_input(bool hovered) {
    if (!paint_ui_) return false;
    PaintUi& ui = *paint_ui_;
    ui.hover = false;
    if (!painting()) {
        if (ui.stroking) end_paint_stroke();  // the window closed or the body changed mid-stroke: keep what it did
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const MeshBody* b = mesh_body();
    const std::vector<std::string> parts = paintable(b ? b->parts : std::vector<std::string>{}, prop_reports_, prop_sources_);
    if (parts.empty()) return false;
    // The body under the pointer: the posed triangle, and the rest point it stands for.
    Vec3 o, d;
    projector_.ray(camera_, io.MousePos.x, io.MousePos.y, o, d);
    std::string hit_path;
    SurfaceHit hit;
    for (const std::string& path : parts) {
        const auto pos = mesh_body_skin_pos_.find(path);
        const auto shown = prop_models_.find(path);
        if (pos == mesh_body_skin_pos_.end() || shown == prop_models_.end() || !shown->second) continue;
        const SurfaceHit h = ray_surface(o, d, pos->second, shown->second->indices);
        if (h.triangle >= 0 && h.t < hit.t) hit = h, hit_path = path;
    }
    Vec3 rest;
    int seed = -1;
    PaintUi::Work* w = nullptr;
    if (!hit_path.empty()) {
        DaeModel* src = part_source(hit_path);
        const DaeModel* shown = prop_models_[hit_path].get();
        w = src ? &work_for(ui, hit_path, *src, look_for_path(hit_path), paint_generation_) : nullptr;
        if (w) {
            const std::uint32_t* tri = &shown->indices[size_t(hit.triangle) * 3];
            const double bary[3] = {1 - hit.u - hit.v, hit.u, hit.v};
            double most = -1;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t s = w->source_of.size() > tri[k] ? w->source_of[tri[k]] : tri[k];
                rest += Vec3{w->model.positions[s * 3], w->model.positions[s * 3 + 1], w->model.positions[s * 3 + 2]} * bary[k];
                if (bary[k] > most) most = bary[k], seed = int(s);
            }
            ui.hover = true;
            ui.hover_at = o + d * hit.t;
            ui.hover_px = float(ui.brush.radius / std::max(projector_.world_per_pixel(camera_, ui.hover_at), 1e-9));
        }
    }
    auto dab = [&](const Vec3& at) {
        DaeModel* src = part_source(ui.path);
        if (!src) return;
        PaintUi::Work& wk = work_for(ui, ui.path, *src, look_for_path(ui.path), paint_generation_);
        PaintBrush brush = ui.brush;
        brush.op = ui.op;
        dab_on(skel_, wk, ui.joint, at, seed, brush, ui.mirror);
        for (size_t v = 0; v < wk.hidden.size(); ++v)  // a hidden part keeps its weights
            if (wk.hidden[v])
                for (size_t k = 0; k < 4; ++k) wk.model.joints[v * 4 + k] = src->joints[v * 4 + k], wk.model.weights[v * 4 + k] = src->weights[v * 4 + k];
        src->joints = wk.model.joints, src->weights = wk.model.weights;
        part_source_changed(ui.path);
        ui.last = at;
        ++ui.dabs;
    };
    auto start_stroke = [&](const std::string& path, const Vec3& at, int from) {
        // Ctrl paints the other way, Shift smooths, for the stroke.
        ui.op = io.KeyShift ? PaintOp::Smooth : io.KeyCtrl ? (ui.brush.op == PaintOp::Add ? PaintOp::Subtract : ui.brush.op == PaintOp::Subtract ? PaintOp::Add : ui.brush.op)
                                                           : ui.brush.op;
        ui.stroking = true;
        ui.path = path;
        ui.body = b ? b->id : "";
        ui.dabs = 0;
        DaeModel* src = part_source(ui.path);
        ui.wb = begin_stroke(*src, ui.jb);
        PaintUi::Work& wk = work_for(ui, path, *src, look_for_path(path), paint_generation_);
        wk.model.joints = src->joints, wk.model.weights = src->weights;  // as the body has them now (a fix, a share)
        seed = from;
        dab(at);
    };
    auto pick = [&](int node) {
        select(node, false);
        status("Painting " + bone_label(node) + ": drag on the body to paint it");
    };
    if (ui.press.on) {
        if (!ImGui::IsMouseDown(0)) {  // a click on a bone: paint that bone next
            ui.press.on = false;
            pick(ui.press.bone);
            return true;
        }
        const ImVec2 m = io.MousePos;
        if (std::hypot(m.x - ui.press.at.x, m.y - ui.press.at.y) < 4) return true;
        ui.press.on = false;  // a drag that began on a bone paints, from where it began
        if (ui.press.path.empty()) return true;
        if (sk40_of_node(skel_, primary()) < 0) pick(ui.press.bone);
        ui.joint = sk40_of_node(skel_, primary());
        if (ui.joint >= 0) start_stroke(ui.press.path, ui.press.rest, ui.press.seed);
        return true;
    }
    if (ui.stroking) {
        if (ImGui::IsMouseDown(0)) {
            // A dab each time the brush has moved a fifth of its size over the same part.
            if (w && hit_path == ui.path && (rest - ui.last).length() >= 0.2 * ui.brush.radius) dab(rest);
            return true;
        }
        end_paint_stroke();
        return true;
    }
    if (!hovered || io.KeyAlt || !ImGui::IsMouseClicked(0)) return false;
    // A press on a bone's stick or dot: a click picks it, a drag paints (Press above).
    if (hover_bone_ >= 0 && hover_bone_ < skel_.size()) {
        ui.press = {true, io.MousePos, hover_bone_, w ? hit_path : std::string(), rest, seed};
        return true;
    }
    if (!w) return true;  // a click beside the body keeps the bone being painted (it doesn't deselect)
    ui.joint = sk40_of_node(skel_, primary());
    if (ui.joint < 0) {  // no bone yet: a click on the body picks the bone that carries it there
        const int node = node_of_sk40(skel_, seed >= 0 ? dominant_joint(w->model, seed) : -1);
        if (node >= 0) pick(node);
        else status("Paint Weights: pick a bone first (click it in the view, the Bones list or the picker)");
        return true;
    }
    start_stroke(hit_path, rest, seed);
    return true;
}

void App::stop_painting(const std::string& why) {
    if (!paint_ui_ || !paint_ui_->on) return;
    if (paint_ui_->stroking) end_paint_stroke();
    paint_ui_->on = false;
    status(why);
}

void App::draw_paint_overlay(ImDrawList* dl) {
    if (!paint_ui_ || !painting()) return;
    {  // The mode, said where the eye is: a badge at the top of the view while a drag paints instead of posing.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            return stop_painting("Painting stopped: drags pick and pose again");
        const int node = primary();
        std::string text = "Painting " + (node >= 0 ? bone_label(node) : std::string("(pick a bone)")) + "  \xc2\xb7  Esc to stop";
        const ImVec2 lo = dl->GetClipRectMin(), hi = dl->GetClipRectMax();
        const float pad = ImGui::GetFontSize() * 0.5f;
        // A narrow view (the Rig workspace's) cut the badge at both ends: the Esc reminder goes first.
        if (ImGui::CalcTextSize(text.c_str()).x + 3 * pad > hi.x - lo.x) text = text.substr(0, text.find("  \xc2\xb7  Esc"));
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const ImVec2 a{(lo.x + hi.x - ts.x) * 0.5f - pad, lo.y + pad}, b{a.x + ts.x + 2 * pad, a.y + ts.y + pad};
        dl->AddRectFilled(a, b, IM_COL32(12, 13, 16, 215), ts.y);
        dl->AddRect(a, b, IM_COL32(255, 120, 90, 200), ts.y, 0, 1.5f);
        dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.5f), IM_COL32(255, 238, 230, 255), text.c_str());
    }
    if (!paint_ui_->hover) return;
    const PaintUi& ui = *paint_ui_;
    double x, y;
    if (!projector_.to_screen(ui.hover_at, x, y)) return;
    const PaintOp op = ui.stroking ? ui.op : ui.brush.op;
    const ImU32 col = op == PaintOp::Add ? IM_COL32(255, 120, 90, 230) : op == PaintOp::Subtract ? IM_COL32(110, 170, 255, 230)
                                                                                             : IM_COL32(230, 230, 230, 230);
    const ImVec2 c{float(x), float(y)};
    const float r = std::clamp(ui.hover_px, 3.f, 2000.f);
    dl->AddCircle(c, r, IM_COL32(10, 12, 14, 200), 0, 3.5f);
    dl->AddCircle(c, r, col, 0, 1.8f);
    dl->AddCircle(c, r * 0.5f, IM_COL32(10, 12, 14, 90), 0, 1.f);  // where the falloff is half way (smooth)
    dl->AddCircleFilled(c, 2.f, col);
}

void App::draw_paint_window() {
    if (!show_paint_) {
        if (paint_ui_) paint_ui_->stroking = false;
        paint_visible_ = false;
        return;
    }
    if (!paint_ui_) paint_ui_ = std::make_shared<PaintUi>();
    PaintUi& ui = *paint_ui_;
    place_tool_window("###paint-weights", 22, 30);
    // The brush is armed only while its window is in sight: behind another tab of the Rig column, a drag poses as usual.
    // Coming into sight (opened, its tab picked, Rig entered again) arms it: leaving Rig or Esc turned it off, and the
    // painter opening unticked read as broken.
    const bool was_visible = paint_visible_;
    paint_visible_ = ImGui::Begin(dock_title("Paint Weights", "Paint", "paint-weights").c_str(), &show_paint_);
    if (!paint_visible_) return ImGui::End();
    if (!was_visible) ui.on = true;
    tab_tooltip("Paint Weights");
    help_button("rig-from-scratch");
    const MeshBody* b = mesh_body();
    const std::vector<std::string> parts = paintable(b ? b->parts : std::vector<std::string>{}, prop_reports_, prop_sources_);
    if (parts.empty()) {
        if (empty_state("No rigged mesh body shown. Show one (Inventory > Bodies), or rig a model first.",
                        "Rig a Model from Scratch..."))
            show_rig_scratch_ = true;
        return ImGui::End();
    }
    ImGui::Checkbox("Paint (left drag on the body)", &ui.on);
    ImGui::SetItemTooltip("Off: clicks in the view pick and pose bones as usual. Alt and the camera keys move the camera either way.");
    const int node = primary();
    labelled_row("Bone");
    // Wrapped: the docked column is narrow, and a cut-off "Select one in the view or the Bone" hid where to click.
    ImGui::PushTextWrapPos(0);
    if (sk40_of_node(skel_, node) >= 0) ImGui::TextUnformatted(bone_label(node).c_str());
    else ImGui::TextColored(ui.on ? warn_colour() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                            "Click one in the view or the Bones list");
    ImGui::PopTextWrapPos();
    ImGui::SetItemTooltip("The bone the brush paints: the one selected. Its weights glow on the body.");
    int op = int(ui.brush.op);
    for (int k = 0; k < 3; ++k) {
        if (k) ImGui::SameLine();
        if (ImGui::RadioButton(op_name(PaintOp(k)), op == k)) ui.brush.op = PaintOp(k);
        ImGui::SetItemTooltip("%s", k == 0 ? "More of the bone where you paint (Ctrl while painting: subtract)"
                                    : k == 1 ? "Less of the bone; the bones beside it take up the rest (Ctrl: add)"
                                             : "Evens the bone's weight with the vertices around (Shift while painting, any mode)");
    }
    const float label_w = 5 * ImGui::GetFontSize();
    float radius_cm = float(ui.brush.radius * 100), strength = float(ui.brush.strength);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Radius");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (slider_float("##radius", &radius_cm, 0.5f, 40.f, "%.1f cm")) ui.brush.radius = radius_cm / 100.0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Strength");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (slider_float("##strength", &strength, 0.02f, 1.f, "%.2f")) ui.brush.strength = strength;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Falloff");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    static const char* falloffs[] = {"Smooth", "Linear", "Constant"};
    int f = int(ui.brush.falloff);
    if (ImGui::Combo("##falloff", &f, falloffs, 3)) ui.brush.falloff = PaintFalloff(f);
    ImGui::SetItemTooltip("How the brush fades toward its edge: smooth, in a straight line, or not at all");
    ImGui::Checkbox("Mirror", &ui.mirror);
    ImGui::SetItemTooltip("Paints the same on the other side of the body, on the mirrored bone (left on right, right on left)");
    if (host_.world_view()) {
        ImGui::Checkbox("Show on my avatar (Local Mesh)", &ui.local_mesh);
        ImGui::SetItemTooltip("After each stroke the body is written out and shown on your avatar through Local Mesh, on your "
                              "screen only, so you see it on what you wear");
    }
    hint("Pose or play it while you paint; each stroke is one undo step.");
    if (!ui.note.empty()) hint(ui.note.c_str());
    ImGui::End();
}

void App::end_paint_stroke() {
    if (!paint_ui_ || !paint_ui_->stroking) return;
    PaintUi& ui = *paint_ui_;
    ui.stroking = false;
    DaeModel* src = part_source(ui.path);
    if (!src) return;
    PaintStroke stroke = end_stroke(*src, ui.jb, ui.wb);
    if (stroke.empty()) return;
    ui.undo.push_back({ui.body, ui.path, std::move(stroke), doc_.history.undo_steps().size()});
    ui.redo.clear();
    std::string err;
    if (!save_painted(ui.path, *src, err)) status("Not saved in the mapping: " + err);
    const std::string what = std::string(op_name(ui.op)) + " " + bone_label(primary());
    ui.note = what + ": " + std::to_string(ui.undo.back().stroke.vertices.size()) + " vertices changed";
    status("Painted weights (" + what + "), saved in " + stem_of(rig_map_path(ui.path)) + ".json");
    const MeshBody* b = find_mesh_body(ui.body);
    if (b && ui.local_mesh && host_.world_view()) {  // RG-3: the viewer's Local Mesh shows the body as painted
        std::vector<RigPart> rp;
        for (const std::string& path : b->parts)
            if (const DaeModel* m = prop_model(path)) rp.push_back({m, stem_of(path), {}, 0, 1});
        std::string dae, why;
        const std::string file = library_dir() + "paint-preview.dae";
        if (write_rig_dae(skel_, rp, {}, dae, why) && write_text(file, dae, false, why) && host_.local_mesh_preview(file, why)) {
            ui.note += "; shown on your avatar through Local Mesh";
        } else {
            ui.local_mesh = false;
            message("Local Mesh preview", why);
        }
    }
}

bool App::cli_paint(const std::string& spec) {
    std::istringstream in(spec);
    std::string joint, op;
    Vec3 at;
    double radius = 0.05, strength = 0.5;
    if (!(in >> joint >> op >> at.x >> at.y >> at.z)) return false;
    in >> radius >> strength;
    if (!paint_ui_) paint_ui_ = std::make_shared<PaintUi>();
    PaintUi& ui = *paint_ui_;
    int node = skel_.find(joint);
    if (const int v = skel_.find_volume(joint); node < 0 && v >= 0) node = skel_.volumes()[size_t(v)].node;
    const int sk40 = sk40_of_node(skel_, node);
    if (const MeshBody* b = mesh_body())
        for (const std::string& path : b->parts) part_source(path);  // loaded now: the view has not drawn it yet
    const std::vector<std::string> parts = paintable(mesh_body() ? mesh_body()->parts : std::vector<std::string>{}, prop_reports_, prop_sources_);
    if (sk40 < 0 || parts.empty() || (op != "add" && op != "subtract" && op != "smooth")) return false;
    // Where a brush pressed on the model from in front of the point lands: the first surface along -X through it, at rest.
    std::string best;
    double best_t = 1e30;
    const Vec3 from = at + Vec3{5, 0, 0}, dir{-1, 0, 0};
    std::uint32_t corner = 0;
    for (const std::string& path : parts)
        if (const auto shown = prop_models_.find(path); shown != prop_models_.end() && shown->second)
            if (const SurfaceHit h = ray_surface(from, dir, shown->second->positions, shown->second->indices); h.triangle >= 0 && h.t < best_t)
                best_t = h.t, best = path, corner = shown->second->indices[size_t(h.triangle) * 3];
    if (best.empty()) return false;
    at = from + dir * best_t;
    DaeModel* src = part_source(best);
    if (!src) return false;
    select(node, false);
    PaintUi::Work& wk = work_for(ui, best, *src, look_for_path(best), paint_generation_);
    wk.model.joints = src->joints, wk.model.weights = src->weights;
    std::vector<int> jb;
    const std::vector<float> wb = begin_stroke(*src, jb);
    const PaintBrush brush{op == "add" ? PaintOp::Add : op == "subtract" ? PaintOp::Subtract : PaintOp::Smooth, radius, strength, ui.brush.falloff};
    dab_on(skel_, wk, sk40, at, wk.source_of.size() > corner ? int(wk.source_of[corner]) : int(corner), brush, ui.mirror);
    src->joints = wk.model.joints, src->weights = wk.model.weights;
    part_source_changed(best);
    PaintStroke stroke = end_stroke(*src, jb, wb);
    std::fprintf(stderr, "paint: %zu vertices changed on %s\n", stroke.vertices.size(), stem_of(best).c_str());
    ui.undo.push_back({mesh_body()->id, best, std::move(stroke), doc_.history.undo_steps().size()});
    std::string err;
    if (!save_painted(best, *src, err)) std::fprintf(stderr, "paint: not saved: %s\n", err.c_str());
    show_paint_ = true;
    return true;
}

}  // namespace vats
