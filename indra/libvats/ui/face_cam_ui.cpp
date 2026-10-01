// Viewport Avatar Toolset - the face cam: a cutout of a face driven by face tracking, for streaming.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/09 section 0i (build 20), item 53. View > Face Cam shows a borderless window you drag and resize with
// the Linden body's head in it, posed by Tools > Motion Capture's live tracking (head, eyes and face bones) and with the
// head's own expression morphs (blinks, open mouth, smile, frown, kiss: linden_head_morphs), rendered offscreen through
// Host::scene_begin(SceneTarget::FaceCam) by both hosts. While it shows, the tracking no longer drives your avatar
// (apply_mocap_preview), so the in-world avatar keeps its own motions. The shape is your avatar's (Host::shape_params:
// the viewer's own sliders, shown only), else SL's default. Your worn mesh is not drawn: its vertices and weights are
// its creators' content, which the editor never reads (spec 08 RG-7).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "vats/facecap.h"

namespace vats {

struct FaceCamUi {
    std::unique_ptr<AvatarMesh> mesh;
    std::map<int, float> params;   // the shape the base was made from
    BodyShape base;                // that shape, without expressions
    bool have_base = false;
    std::vector<std::pair<std::string, float>> morphs;  // the expressions the mesh was last built with
    bool built = false;
    std::vector<float> pos, nrm;
    std::vector<Vertex> verts;
    std::vector<std::uint32_t> skin, eyes;
};

void App::draw_face_cam() {
    // Scripted checks: VATS_FACE_CAM=1 starts with the face cam shown (the viewer has no --window), like VATS_MOCAP_LISTEN.
    if (static bool checked = false; !checked) checked = true, face_cam_ = face_cam_ || std::getenv("VATS_FACE_CAM");
    if (!face_cam_) return;
    if (!face_cam_ui_) face_cam_ui_ = std::make_shared<FaceCamUi>();
    FaceCamUi& fc = *face_cam_ui_;
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 14, fs * 14), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 4, fs * 4), ImVec2(4096, 4096));
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - fs * 16, vp->WorkPos.y + fs * 4), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    const bool shown = ImGui::Begin("Face Cam", nullptr,
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!shown) return ImGui::End();

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    // The whole cutout drags the window (a title bar would spoil the cutout; the viewer moves windows by title bars only).
    ImGui::InvisibleButton("##face_cam_drag", ImVec2(std::max(size.x, 1.f), std::max(size.y, 1.f)));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        ImGui::SetWindowPos(ImVec2(ImGui::GetWindowPos().x + d.x, ImGui::GetWindowPos().y + d.y));
    }
    if (ImGui::BeginPopupContextItem("##face_cam_menu")) {
        if (ImGui::MenuItem("Close Face Cam")) face_cam_ = false;
        ImGui::EndPopup();
    }
    ImGui::SetItemTooltip("Drag to move, the corner to resize, right-click to close");

    if (mesh_.parts().empty()) {
        ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), "No Linden body to draw");
        return ImGui::End();
    }
    // The shape: your avatar's sliders, rebuilt only when they change.
    std::map<int, float> params = host_.shape_params();
    if (!fc.have_base || params != fc.params) {
        fc.params = std::move(params);
        fc.base = fc.params.empty() ? mesh_.sl_default(false) : evaluate_shape(skel_, mesh_.params(), fc.params);
        fc.have_base = true;
        fc.built = false;
    }
    // The tracking: the live pose and the head's expressions (quantised, so the mesh is rebuilt only when they move).
    Clip live;
    std::map<std::string, double> arkit;
    const bool tracking = mocap_live(live, arkit);
    std::vector<std::pair<std::string, float>> morphs = linden_head_morphs(arkit);
    if (!fc.mesh) fc.mesh = std::make_unique<AvatarMesh>(mesh_);
    if (!fc.built || morphs != fc.morphs) {
        // ponytail: a whole-body rebuild per expression change; a head-only morph pass if it shows in a profile.
        BodyShape bs = fc.base;
        bs.morphs[ShapeHead].insert(bs.morphs[ShapeHead].end(), morphs.begin(), morphs.end());
        fc.mesh->build(bs);
        fc.morphs = std::move(morphs);
        fc.skin.clear(), fc.eyes.clear();
        for (const MeshPart& part : fc.mesh->parts()) {
            auto& dst = part.material == Material::Eye ? fc.eyes : fc.skin;
            dst.insert(dst.end(), fc.mesh->indices().begin() + part.first_index,
                       fc.mesh->indices().begin() + part.first_index + part.index_count);
        }
        fc.built = true;
    }
    const Shape* sh = &fc.base.shape;
    const Evaluation e = vats::evaluate(*rig_, live, 0, sh);
    fc.mesh->skin(e.globals, sh, fc.pos, fc.nrm);

    // From the front (the avatar faces +X), framed on the head from its rest height, so the head turns in the frame.
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    const int w = std::clamp(int(size.x * scale), 16, 2048), h = std::clamp(int(size.y * scale), 16, 2048);
    const int head = skel_.find("mHead");
    Camera cam;
    cam.fov = 30 * kDegToRad;
    cam.target = (head >= 0 ? e.globals[size_t(head)].pos : Vec3{0, 0, 1.6}) + Vec3{0.02, 0, 0.07};
    cam.yaw = 0, cam.pitch = 0.05;
    cam.distance = 0.19 / std::tan(cam.fov / 2) * std::max(1.0, double(h) / w);  // the head and a little neck
    const Mat4 proj = perspective(cam.fov, double(w) / h, 0.05, 5.0);
    SceneColours colours = scene_colours();
    if (!host_.scene_begin(ui::SceneTarget::FaceCam, w, h, cam, colours, &proj)) {
        ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), "The face cam cannot draw here");
        return ImGui::End();
    }
    fc.verts.resize(fc.pos.size() / 3);
    for (const MeshPart& part : fc.mesh->parts()) {
        const Rgb c = part.material == Material::Eye ? colours.eye : colours.body;
        for (std::uint32_t i = part.first_vertex; i < part.first_vertex + part.vertex_count; ++i)
            fc.verts[i] = {{fc.pos[i * 3], fc.pos[i * 3 + 1], fc.pos[i * 3 + 2]},
                           {fc.nrm[i * 3], fc.nrm[i * 3 + 1], fc.nrm[i * 3 + 2]},
                           {c.r, c.g, c.b, 1}};
    }
    host_.scene_triangles(fc.verts, fc.skin, true, 0.04f);
    host_.scene_triangles(fc.verts, fc.eyes, true, 0.6f);
    const ImTextureID tex = host_.scene_end();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage(tex, at, ImVec2(at.x + size.x, at.y + size.y), ImVec2(0, 1), ImVec2(1, 0));  // bottom-up rows
    if (!tracking)
        dl->AddText(ImVec2(at.x + 4, at.y + size.y - ImGui::GetTextLineHeight() - 4), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    "No tracking: Tools > Motion Capture");
    ImGui::End();
}

}  // namespace vats
