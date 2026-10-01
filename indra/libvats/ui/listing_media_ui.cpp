// Viewport Avatar Toolset - listing media: the animation as an animated GIF or PNG pictures (spec 08 LM).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Each frame is rendered offscreen through the thumbnail target (Host::scene_begin with SceneTarget::Thumbnail), so the
// view is untouched: the avatar, the other actors and the props, lit by the studio light, from the view's camera. The
// GIF encoder is in the core (vats/gif.h). The app only: the viewer's host renders no thumbnails (its snapshots are
// the viewer's own).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "app.h"
#include "imgui.h"
#include "vats/gif.h"
#include "vats/loop_tools.h"

namespace vats {

struct ListingUi {
    int width = 512, height = 512;
    bool turntable = true;
    int fps = 0;  // 0 = the animation's, at most 50 for a GIF
    float background[3] = {0.16f, 0.17f, 0.19f};  // the GIF's; PNG pictures keep a clear background
};

namespace {

constexpr int kMaxFrames = 1200;
constexpr double kPi2 = 6.283185307179586;

std::string lower_extension(const std::string& path) {
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
    std::string e = dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : path.substr(dot + 1);
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

}  // namespace

bool App::export_listing_media(const std::string& path) {
    if (!listing_ui_) listing_ui_ = std::make_shared<ListingUi>();
    const ListingUi& o = *listing_ui_;
    const bool gif = lower_extension(path) != "png";
    if (body_ == Body::SkeletonOnly && !mesh_body()) {
        message("Nothing to film", "Listing media shows the body: choose one in View > Body first.");
        return false;
    }
    const Clip& c = doc_.clip();
    const LoopRange range = loop_range(c);
    const int fps = std::clamp(o.fps > 0 ? o.fps : c.fps, 1, gif ? 50 : 120);
    const double step = double(std::max(c.fps, 1)) / fps;  // timeline frames per picture
    const int span = std::max(range.out - range.in, 0);
    // A loop leaves out its last frame, the first again; a clip without one ends on its last frame.
    const int count = std::clamp(int(std::lround(span / step)) + (c.loop ? 0 : 1), 1, kMaxFrames);
    const int w = std::clamp(o.width, 16, 2048), h = std::clamp(o.height, 16, 2048);

    const std::string stem = gif ? path : path.substr(0, path.size() - 4);
    const double keep_frame = frame_;
    const Camera base = camera_;
    GifWriter writer(w, h);
    std::vector<std::uint8_t> px;
    std::string failed;
    for (int i = 0; i < count && failed.empty(); ++i) {
        frame_ = range.in + i * step;
        evaluate();
        Camera cam = base;
        if (o.turntable) cam.yaw += kPi2 * i / count;  // once round over the whole export, so the loop is seamless
        if (!host_.scene_begin(ui::SceneTarget::Thumbnail, w, h, cam, scene_colours())) {
            failed = "This program cannot render offscreen.";
            break;
        }
        static std::vector<Vertex> verts;
        static std::vector<std::uint32_t> indices;
        if (mesh_body()) draw_mesh_body(verts, indices);
        else draw_avatar(false, globals_, scene_colours());
        draw_other_actors(scene_colours());
        draw_props(verts, indices);
        host_.scene_end();
        if (!gif) {
            char name[32];
            std::snprintf(name, sizeof name, "_%04d.png", i + 1);
            if (!host_.save_thumbnail_png(stem + name)) failed = "Could not write " + stem + name;
            continue;
        }
        int pw = 0, ph = 0;
        if (!host_.thumbnail_pixels(px, pw, ph) || pw != w || ph != h) {
            failed = "Could not read the rendered picture back.";
            break;
        }
        for (size_t k = 0; k < px.size(); k += 4) {  // over the background colour
            const float a = px[k + 3] / 255.f;
            for (int ch = 0; ch < 3; ++ch)
                px[k + size_t(ch)] = std::uint8_t(std::lround(px[k + size_t(ch)] * a + o.background[ch] * 255 * (1 - a)));
        }
        writer.add_frame(px.data(), gif_delay_cs(i, fps));
    }
    frame_ = keep_frame;
    evaluate();
    if (failed.empty() && gif) {
        const std::vector<std::uint8_t> bytes = writer.finish();
        std::ofstream f(u8path(path), std::ios::binary);
        if (!f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) failed = "Could not write " + path;
    }
    if (!failed.empty()) {
        message("Listing media", failed);
        return false;
    }
    const std::string what = std::to_string(count) + (count == 1 ? " frame" : " frames") + " at " + std::to_string(w) + " x " +
                             std::to_string(h);
    status(gif ? "Wrote " + path + " (" + what + ")" : "Wrote " + stem + "_0001.png ... (" + what + ")");
    std::printf("listing: %s\n", what.c_str());  // scripted runs (--listing) read it
    return true;
}

void App::draw_listing_window() {
    if (!show_listing_) return;
    if (!listing_ui_) listing_ui_ = std::make_shared<ListingUi>();
    ListingUi& o = *listing_ui_;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 27, em * 18), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Export Listing Media", &show_listing_)) return ImGui::End();
    const float label_w = em * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
    };
    label("Format");
    int format = listing_png_ ? 1 : 0;
    ImGui::RadioButton("Animated GIF", &format, 0);
    ImGui::SameLine();
    ImGui::RadioButton("PNG Pictures", &format, 1);
    listing_png_ = format == 1;
    ImGui::SetItemTooltip("Numbered pictures (name_0001.png, ...) on a clear background, for your own editor");
    label("Size");
    ImGui::SetNextItemWidth(em * 8);
    int size[2] = {o.width, o.height};
    if (ImGui::InputInt2("##listing_size", size)) o.width = std::clamp(size[0], 16, 2048), o.height = std::clamp(size[1], 16, 2048);
    for (int s : {256, 512, 1024}) {
        ImGui::SameLine();
        if (ImGui::SmallButton(std::to_string(s).c_str())) o.width = o.height = s;
    }
    label("Frame rate");
    ImGui::SetNextItemWidth(em * 8);
    ImGui::InputInt("##listing_fps", &o.fps);
    o.fps = std::clamp(o.fps, 0, 120);
    ImGui::SameLine();
    ImGui::TextDisabled(o.fps ? "fps" : "the animation's");
    ImGui::SetItemTooltip("0 = the animation's own; a GIF plays at most 50");
    ImGui::Checkbox("Turntable", &o.turntable);
    ImGui::SetItemTooltip("The camera goes once round the avatar over the whole animation");
    if (!listing_png_) {
        label("Background");
        ImGui::ColorEdit3("##listing_bg", o.background, ImGuiColorEditFlags_NoInputs);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Filmed from the view's camera: the loop, or the whole animation without one.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (ImGui::Button("Export...")) show_dialog(Dialog::ListingMedia);
    ImGui::End();
}

}  // namespace vats
