// Viewport Avatar Toolset - the reference picture: View > Reference..., its texture, and drawing it (spec 08 RF).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The settings, the sequence's files and the quads are in the core (vats/reference.h); the picture lives on the clip,
// so it is saved with the project and every change is one undo step.
#include <algorithm>
#include <cmath>
#include <iterator>

#include "app.h"
#include "imgui.h"
#include "vats/reference.h"
#include "vats/wiki.h"

namespace vats {

struct ReferenceUi {
    ui::Host* host = nullptr;
    std::string listed;              // the path (and whether a sequence) files was made for
    std::vector<std::string> files;  // the sequence's pictures, or the one picture
    std::string loaded;              // the picture tex holds
    ImTextureID tex{};
    int w = 0, h = 0, index = 0;
    bool host_drew = false;  // the scene plane went through Host::scene_image this frame
    ~ReferenceUi() {
        if (tex) host->free_texture(tex);
    }
};

namespace {

std::string base_name(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

}  // namespace

void App::load_reference(const std::string& path, bool sequence) {
    int w = 0, h = 0;
    if (!wiki::png_size(path, w, h))
        return message("Could not load the reference",
                       path + "\n\nThe reference must be a PNG picture. Save other pictures as PNG first; a video becomes PNG "
                              "pictures with ffmpeg (Help: Reference images).");
    const int fps = doc_.clip().fps;
    edit(sequence ? "Load Reference Sequence" : "Load Reference", [&](Clip& c) {
        const bool fresh = !c.reference;
        Reference r = c.reference.value_or(Reference{});
        r.path = path, r.sequence = sequence, r.hidden = false;
        if (fresh || !sequence) r.fps = fps;  // a sequence made at the animation's rate lines up frame for frame
        c.reference = r;
    });
    show_reference_ = true;
    if (reference_ui_) reference_ui_->listed.clear();  // loaded again: the folder is listed again
    if (!sequence) return status("Reference: " + base_name(path));
    const size_t n = sequence_files(path).size();
    status("Reference sequence: " + std::to_string(n) + (n == 1 ? " picture" : " pictures"));
}

ImTextureID App::reference_texture(int& width, int& height) {
    const Clip& c = doc_.clip();
    if (!c.reference || c.reference->path.empty()) return ImTextureID{};
    if (!reference_ui_) reference_ui_ = std::make_shared<ReferenceUi>(), reference_ui_->host = &host_;
    ReferenceUi& ui = *reference_ui_;
    const Reference& r = *c.reference;
    const std::string listed = r.path + (r.sequence ? "\n#" : "");
    if (ui.listed != listed) {  // ponytail: listed once per path; pictures added later need the sequence loaded again
        ui.listed = listed;
        ui.files = r.sequence ? sequence_files(r.path) : std::vector<std::string>{r.path};
    }
    ui.index = r.sequence ? sequence_index(r, frame_, c.fps, int(ui.files.size())) : 0;
    const std::string& want = ui.files[size_t(ui.index)];
    if (want != ui.loaded) {  // ponytail: decodes on each change, one picture held; cache ahead if playback stutters
        if (ui.tex) host_.free_texture(ui.tex), ui.tex = ImTextureID{};
        ui.loaded = want;
        if (wiki::png_size(want, ui.w, ui.h)) ui.tex = host_.load_texture(want);
    }
    width = ui.w, height = ui.h;
    return ui.tex;
}

bool App::draw_reference(bool backdrop, double view_aspect) {
    if (reference_ui_ && !backdrop) reference_ui_->host_drew = false;
    const Clip& c = doc_.clip();
    if (!c.reference || c.reference->in_scene == backdrop || !reference_shows(*c.reference, -camera_.forward())) return true;
    int w = 0, h = 0;
    const ImTextureID tex = reference_texture(w, h);
    if (!tex || h <= 0) return true;
    const Reference& r = *c.reference;
    const double aspect = double(w) / h;
    const auto corners = backdrop ? reference_backdrop_quad(r, view_aspect, aspect) : reference_scene_quad(r, {}, aspect);
    const bool drawn = host_.scene_image(tex, corners, float(r.opacity), backdrop);
    if (!backdrop) reference_ui_->host_drew = drawn;
    return drawn;
}

// The world view (the viewer): the picture over the world and under the editor's windows, see-through by its opacity.
void App::draw_reference_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    const Clip& c = doc_.clip();
    if (!c.reference || !reference_shows(*c.reference, -camera_.forward())) return;
    const Reference& r = *c.reference;
    if (r.in_scene && reference_ui_ && reference_ui_->host_drew) return;
    int w = 0, h = 0;
    const ImTextureID tex = reference_texture(w, h);
    if (!tex || h <= 0) return;
    ImVec2 p[4];
    if (r.in_scene) {
        const auto q = reference_scene_quad(r, {}, double(w) / h);
        for (int i = 0; i < 4; ++i) {
            double x = 0, y = 0;
            if (!projector_.to_screen(q[size_t(i)], x, y)) return;  // behind the camera
            p[i] = ImVec2(float(x), float(y));
        }
    } else {
        const auto q = reference_backdrop_quad(r, double(size.x) / size.y, double(w) / h);
        for (int i = 0; i < 4; ++i)
            p[i] = ImVec2(origin.x + float(q[size_t(i)].x + 1) * 0.5f * size.x, origin.y + float(1 - q[size_t(i)].y) * 0.5f * size.y);
    }
    const ImU32 tint = IM_COL32(255, 255, 255, int(std::lround(std::clamp(r.opacity, 0.0, 1.0) * 255)));
    dl->AddImageQuad(tex, p[0], p[1], p[2], p[3], ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0), ImVec2(0, 0), tint);
}

void App::draw_reference_window() {
    if (!show_reference_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 24, em * 25), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Reference", &show_reference_)) return ImGui::End();
    auto pick = [&](bool sequence) {
        reference_pick_sequence_ = sequence;
        show_dialog(Dialog::LoadReference);
    };
    if (ImGui::Button("Load Picture...")) pick(false);
    ImGui::SetItemTooltip("A PNG picture: a pose to match, a character sheet, a drawing");
    ImGui::SameLine();
    if (ImGui::Button("Load Sequence...")) pick(true);
    ImGui::SetItemTooltip("Pick any picture of a numbered sequence (walk_0001.png, walk_0002.png, ...): the pictures follow "
                          "the timeline");
    Clip& clip = doc_.clip();
    if (!clip.reference) {
        ImGui::TextWrapped("No reference yet. It shows behind the avatar, saved with the project.");
        return ImGui::End();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        edit("Remove Reference", [](Clip& c) { c.reference.reset(); });
        return ImGui::End();
    }

    Reference& r = *clip.reference;
    const Reference before = r;
    int pw = 0, ph = 0;
    const ImTextureID tex = reference_texture(pw, ph);
    ImGui::TextUnformatted(base_name(r.path).c_str());
    ImGui::SetItemTooltip("%s", r.path.c_str());
    if (!tex) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "The picture could not be read");
    else if (r.sequence && reference_ui_)
        ImGui::TextDisabled("Picture %d of %d, %d x %d", reference_ui_->index + 1, int(reference_ui_->files.size()), pw, ph);
    else ImGui::TextDisabled("%d x %d", pw, ph);

    const float label_w = em * 7;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    // A drag or a slider is one undo step, from its value when it started.
    auto track = [&](const char* step) {
        if (ImGui::IsItemActivated()) {
            const Reference now = r;
            r = before;
            doc_.history.begin(clip);
            r = now;
        }
        if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, clip)) mark_dirty();
    };
    auto toggle = [&](const char* name, bool Reference::*field, const char* step) {
        bool v = r.*field;
        if (ImGui::Checkbox(name, &v)) edit(step, [&](Clip& c) { (*c.reference).*field = v; });
    };

    ImGui::SeparatorText("Placement");
    bool shown = !r.hidden;
    if (ImGui::Checkbox("Show", &shown)) edit(shown ? "Show Reference" : "Hide Reference", [&](Clip& c) { c.reference->hidden = !shown; });
    int where = r.in_scene ? 1 : 0;
    bool moved = ImGui::RadioButton("Backdrop", &where, 0);
    ImGui::SameLine();
    moved |= ImGui::RadioButton("In the Scene", &where, 1);
    if (moved) edit("Reference Placement", [&](Clip& c) { c.reference->in_scene = where == 1; });
    ImGui::SetItemTooltip("Backdrop: behind everything, fixed on the screen. In the Scene: a plane 1.5 m behind the avatar, "
                          "facing the view it is locked to (Front when none)");
    static const char* views[] = {"Every View", "Front", "Back", "Right", "Left", "Top"};
    label("Show in");
    int view = int(r.view);
    if (ImGui::Combo("##ref_view", &view, views, int(std::size(views))))
        edit("Reference View", [&](Clip& c) { c.reference->view = ReferenceView(view); });
    ImGui::SetItemTooltip("Locked to a view, the picture shows only while the camera looks from within 15 degrees of it "
                          "(View > Front, Back, Right, Left, Top)");

    ImGui::SeparatorText("Look");
    float opacity = float(r.opacity);
    label("Opacity");
    if (ImGui::SliderFloat("##ref_opacity", &opacity, 0, 1, "%.2f")) r.opacity = opacity;
    track("Reference Opacity");
    float scale = float(r.scale);
    label("Scale");
    if (ImGui::DragFloat("##ref_scale", &scale, 0.005f, 0.01f, 100, "%.3f", ImGuiSliderFlags_AlwaysClamp)) r.scale = scale;
    track("Reference Scale");
    ImGui::SetItemTooltip(r.in_scene ? "1 = 2 m tall" : "1 = the view's height");
    float offset[2] = {float(r.offset_x), float(r.offset_y)};
    label("Offset");
    if (ImGui::DragFloat2("##ref_offset", offset, 0.002f, -100, 100, "%.3f")) r.offset_x = offset[0], r.offset_y = offset[1];
    track("Reference Offset");
    ImGui::SetItemTooltip(r.in_scene ? "Right and up, in metres" : "Right and up, in view heights");
    toggle("Flip Horizontally", &Reference::flip_x, "Flip Reference");
    ImGui::SameLine();
    toggle("Flip Vertically", &Reference::flip_y, "Flip Reference");

    if (r.sequence) {
        ImGui::SeparatorText("Sequence");
        float fps = float(r.fps);
        label("Frame rate");
        if (ImGui::DragFloat("##ref_fps", &fps, 0.1f, 0.1f, 240, "%.2f fps", ImGuiSliderFlags_AlwaysClamp)) r.fps = fps;
        track("Reference Frame Rate");
        ImGui::SetItemTooltip("The rate the pictures were made at (ffmpeg's fps=)");
        label("Starts on frame");
        ImGui::DragInt("##ref_offset_frames", &r.frame_offset, 0.2f, -100000, 100000);
        track("Reference Start");
        ImGui::SetItemTooltip("The timeline frame the first picture shows on; negative skips the first pictures");
    }
    ImGui::End();
}

}  // namespace vats
