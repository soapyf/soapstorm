// Viewport Avatar Toolset - the Inventory panel: saved poses and clips.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/02 AM-91..95, docs/spec/03 section 3.6, docs/spec/06 section 4.2.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "vats/pose_presets.h"
#include "theme.h"

namespace vats {
namespace {

int prop_to_rename = -1;  // set by the prop grid, opened by the panel's Rename prompt

std::string new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    char b[40];
    std::snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)rng(), (unsigned long long)rng());
    return b;
}

}  // namespace

std::string App::library_path() const {
    const ui::Paths& paths = host_.paths();
    std::string dir = paths.library.empty() ? (paths.user.empty() ? "" : paths.user + "library/") : paths.library;
    std::error_code ec;
    if (!dir.empty()) std::filesystem::create_directories(u8path(dir), ec);
    return dir + "poses.json";
}

void App::load_library() {
    load_prop_libraries();
    load_bodies();
    std::ifstream f(library_path(), std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!vats::load_library(ss.str(), library_, err)) {
        // Keep the damaged file aside instead of overwriting it on the next save (03 P10).
        std::string aside = library_path() + ".corrupt-" + std::to_string(host_.ticks_ns() / 1000000);
        std::rename(library_path().c_str(), aside.c_str());
        library_ = Library();
        message("Pose library damaged", "It could not be read (" + err + ") and was renamed to\n" + aside);
    }
}

void App::save_library() {
    std::string text = vats::save_library(library_), path = library_path(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) return message("Could not save the pose library", path);
    }
    std::rename(tmp.c_str(), path.c_str());
}

void App::store_library_item(LibraryItem item) {
    item.id = new_id();
    library_.items.push_back(std::move(item));
    save_library();
}

// The user's prop library (library.json beside poses.json) and the read-only starter props shipped in
// assets/props/props.json: [{slug, name, file, category, suggested_point, pos, rot}]. A two-handed prop may add
// support_grip: where the other hand's fist goes, in metres in the suggested point's frame (for tutorials).
void App::load_prop_libraries() {
    std::string path = library_dir() + "library.json", err;
    if (std::ifstream f{path, std::ios::binary}) {
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!load_prop_library(ss.str(), prop_library_, err)) {
            std::string aside = path + ".corrupt-" + std::to_string(host_.ticks_ns() / 1000000);
            std::rename(path.c_str(), aside.c_str());
            prop_library_.clear();
            message("Prop library damaged", "It could not be read (" + err + ") and was renamed to\n" + aside);
        }
    }
    std::string dir = assets_dir_ + "/props/";
    std::ifstream f(dir + "props.json", std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    Json doc;
    std::vector<Prop> props;
    if (!parse_json(ss.str(), doc, err) || !props_from_json(doc, props, err)) {
        std::fprintf(stderr, "starter props: %s\n", err.c_str());
        return;
    }
    auto text = [](const Prop& p, const char* key) {
        const Json* j = p.extra.find(key);
        return j && j->is_string() ? j->str : std::string();
    };
    for (Prop& p : props) {
        std::string slug = text(p, "slug"), file = text(p, "file");
        if (slug.empty() || file.empty()) continue;
        p.path = dir + file;
        p.point = text(p, "suggested_point");
        starter_props_.push_back({"starter-" + slug, std::move(p)});
    }
}

// The frame range for "Save Clip": the timeline range, or else the span of the selected graph keys.
bool App::clip_range(double& a, double& b) const {
    if (range_a_ >= 0 && range_a_ != range_b_) {
        a = std::min(range_a_, range_b_);
        b = std::max(range_a_, range_b_);
        return true;
    }
    return graph_.key_span(doc_.clip(), a, b);
}

// An icon grid of library props (06 section 4.2): double-click adds, drag onto the view attaches, right-click
// for the rest. Starter props (user = false) cannot be renamed, saved over or deleted.
void App::draw_prop_grid(std::vector<PropLibraryItem>& items, bool user) {
    const float font = ImGui::GetFontSize(), thumb = font * 6, cell = thumb + font * 0.9f;
    const float cell_h = thumb + ImGui::GetTextLineHeight() * 2 + 6, gap = ImGui::GetStyle().ItemSpacing.x;
    const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
    int remove = -1, col = 0, variant = -1;
    std::string category;
    for (int i = 0; i < int(items.size()); ++i) {
        PropLibraryItem& it = items[i];
        const Prop& p = it.prop;
        if (!inv_match(p.name)) continue;
        const Json* cat = p.extra.find("category");
        if (cat && cat->is_string() && cat->str != category) {  // starter props come grouped by category
            category = cat->str;
            std::string title = category;
            title[0] = char(std::toupper(static_cast<unsigned char>(title[0])));
            ImGui::TextDisabled("%s", title.c_str());
            col = 0;
        }
        ImGui::PushID(it.id.c_str());
        if (col++ % cols) ImGui::SameLine();
        ImTextureID tex = prop_thumbnail(it);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##cell", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cell, cell_h)) &&
            ImGui::IsMouseDoubleClicked(0))
            add_library_prop(it, "", "", true);
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(p.name.c_str());
            if (p.rigged) ImGui::TextDisabled("(rigged)");
            else ImGui::Text("Parent: %s", !p.point.empty() ? p.point.c_str() : !p.bone.empty() ? p.bone.c_str() : "World");
            if (const Json* c = p.extra.find("category"); c && c->is_string()) ImGui::Text("Category: %s", c->str.c_str());
            ImGui::TextDisabled("%s", p.path.c_str());
            ImGui::TextDisabled("Double-click to add, or drag onto a bone in the view");
            ImGui::EndTooltip();
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("VATS_PROP", it.id.c_str(), it.id.size() + 1);
            if (tex) ImGui::Image(ImTextureID(tex), ImVec2(font * 3, font * 3));
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add to Scene")) add_library_prop(it, "", "", true);
            if (ImGui::BeginMenu("Attach to Point", !p.rigged)) {
                std::vector<std::string> points;
                for (int n = skel_.joint_count(); n < skel_.size(); ++n) points.push_back(skel_[n].name);
                std::sort(points.begin(), points.end());
                for (const std::string& pt : points)
                    if (ImGui::MenuItem(pt.c_str())) add_library_prop(it, "", pt, pt == p.point);
                ImGui::EndMenu();
            }
            int bone = primary();
            std::string attach = bone >= 0 ? "Attach to " + skel_[bone].name : "Attach to Selected Bone (select one first)";
            if (ImGui::MenuItem(attach.c_str(), nullptr, false, !p.rigged && bone >= 0)) {
                bool point = skel_[bone].attachment;
                std::string b = point ? "" : skel_[bone].name, pt = point ? skel_[bone].name : "";
                add_library_prop(it, b, pt, b == p.bone && pt == p.point);
            }
            const auto& scene = doc_.clip().props;
            bool can_save = selected_prop_ >= 0 && selected_prop_ < int(scene.size()) && !scene[selected_prop_].rigged;
            ImGui::Separator();
            if (ImGui::MenuItem("Save As Variant", nullptr, false, can_save && scene[selected_prop_].path == p.path))
                variant = i;
            ImGui::SetItemTooltip("A new Inventory item from the selected prop (same mesh): its parent, offset and scale");
            if (user) {
                if (ImGui::MenuItem("Save (from Selected Prop)", nullptr, false, can_save)) {
                    Prop q = scene[selected_prop_];
                    q.lib_id.clear(), q.visible = true, q.name = p.name;
                    it.prop = q;
                    std::remove((library_dir() + it.id + ".png").c_str());  // the scale may have changed
                    forget_thumbnail(it.id);
                    save_prop_library();
                    status("Saved " + p.name + " from the selected prop");
                }
                ImGui::SetItemTooltip("Overwrite this item's mesh, parent, position, rotation and scale with the selected prop's");
                ImGui::Separator();
                if (menu_item_icon(icon::kRename, "Rename...")) prop_to_rename = i;
                if (menu_item_icon(icon::kDelete, "Delete from Inventory")) remove = i;
            }
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a(p0.x + (cell - thumb) / 2, p0.y + 2), b(a.x + thumb, a.y + thumb);
        if (tex) {
            dl->AddImage(ImTextureID(tex), a, b);
        } else {  // generic prop icon until the thumbnail lands (or when the mesh is missing)
            float r = thumb * 0.22f;
            ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
            ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y - r / 2), ImVec2(c.x, c.y), ImVec2(c.x - r, c.y - r / 2), col, 1.5f);
            dl->AddLine(ImVec2(c.x - r, c.y - r / 2), ImVec2(c.x - r, c.y + r / 2), col, 1.5f);
            dl->AddLine(ImVec2(c.x + r, c.y - r / 2), ImVec2(c.x + r, c.y + r / 2), col, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y + r), col, 1.5f);
            dl->AddLine(ImVec2(c.x - r, c.y + r / 2), ImVec2(c.x, c.y + r), col, 1.5f);
            dl->AddLine(ImVec2(c.x + r, c.y + r / 2), ImVec2(c.x, c.y + r), col, 1.5f);
        }
        ImVec4 clip(p0.x, b.y, p0.x + cell, p0.y + cell_h);
        dl->AddText(ImGui::GetFont(), font, ImVec2(p0.x + 3, b.y + 3), ImGui::GetColorU32(ImGuiCol_Text), p.name.c_str(),
                    nullptr, cell - 6, &clip);
        ImGui::PopID();
    }
    if (variant >= 0) {  // after the loop: adding to prop_library_ may move the items being drawn
        PropLibraryItem v{new_library_id(), doc_.clip().props[selected_prop_]};
        v.prop.lib_id.clear(), v.prop.visible = true;
        v.prop.extra = items[variant].prop.extra;
        v.prop.name = items[variant].prop.name + " variant";
        // VP-90: the variant starts with the source's picture.
        std::error_code ec;  // no source picture yet: the queue renders one
        std::filesystem::copy_file(library_dir() + items[variant].id + ".png", library_dir() + v.id + ".png",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        prop_library_.push_back(std::move(v));
        save_prop_library();
        status("Saved " + prop_library_.back().prop.name);
    }
    if (remove >= 0) {
        std::string name = items[remove].prop.name, id = items[remove].id;
        std::string text = "Delete \"" + name + "\" from the Inventory?\nCopies already in scenes are kept.";
        // The host may answer later, so the item is found again by its id (items is a library member).
        host_.ask("Viewport Avatar Toolset", text, {"Delete", "Cancel"}, [this, list = &items, name, id](int choice) {
            auto it = std::find_if(list->begin(), list->end(), [&](const PropLibraryItem& x) { return x.id == id; });
            if (choice != 0 || it == list->end()) return;
            std::remove((library_dir() + id + ".png").c_str());
            forget_thumbnail(id);
            list->erase(it);
            save_prop_library();
            status("Deleted " + name);
        });
    }
}

// A pose or clip's picture: its thumbnail when one exists (VP-I12), else a line icon for its kind (VP-91).
// Clips get a play mark in the corner either way.
void App::draw_library_icon(ImDrawList* dl, ImVec2 at, float size, const LibraryItem& it) {
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImVec2 b(at.x + size, at.y + size);
    dl->AddRectFilled(at, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
    if (ImTextureID tex = pose_thumbnail(it)) {
        dl->AddImage(ImTextureID(tex), at, b);
    } else {
        auto P = [&](float x, float y) { return ImVec2(at.x + x * size, at.y + y * size); };
        const float w = std::max(1.2f, size / 22);
        if (it.kind == "hand") {
            dl->AddRect(P(0.3f, 0.5f), P(0.66f, 0.82f), col, 3, 0, w);
            for (int f = 0; f < 4; ++f) dl->AddLine(P(0.34f + f * 0.1f, 0.5f), P(0.34f + f * 0.1f, 0.2f + (f == 0 || f == 3) * 0.08f), col, w);
            dl->AddLine(P(0.66f, 0.62f), P(0.8f, 0.45f), col, w);
        } else if (it.kind == "arm") {
            const ImVec2 pts[3] = {P(0.2f, 0.3f), P(0.55f, 0.62f), P(0.82f, 0.3f)};
            dl->AddPolyline(pts, 3, col, 0, w);
            for (auto& q : pts) dl->AddCircleFilled(q, w * 1.6f, col);
        } else if (it.kind == "leg" || it.kind == "hindleg") {
            const ImVec2 pts[4] = {P(0.45f, 0.15f), P(0.55f, 0.5f), P(0.45f, 0.82f), P(0.72f, 0.84f)};
            dl->AddPolyline(pts, 4, col, 0, w);
            for (int k = 0; k < 3; ++k) dl->AddCircleFilled(pts[k], w * 1.6f, col);
        } else if (it.kind == "head" || it.kind == "face") {
            dl->AddCircle(P(0.5f, 0.45f), size * 0.22f, col, 0, w);
            dl->AddLine(P(0.5f, 0.67f), P(0.5f, 0.85f), col, w);
        } else {  // whole pose, selection, wing, tail: a figure
            dl->AddCircle(P(0.5f, 0.2f), size * 0.08f, col, 0, w);
            dl->AddLine(P(0.5f, 0.28f), P(0.5f, 0.58f), col, w);
            dl->AddLine(P(0.25f, 0.4f), P(0.75f, 0.4f), col, w);
            dl->AddLine(P(0.5f, 0.58f), P(0.34f, 0.86f), col, w);
            dl->AddLine(P(0.5f, 0.58f), P(0.66f, 0.86f), col, w);
        }
    }
    if (it.clip) {
        float r = size * 0.16f;
        ImVec2 c(b.x - r - 2, b.y - r - 2);
        dl->AddCircleFilled(c, r + 1, IM_COL32(20, 22, 26, 200));
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.4f, c.y - r * 0.55f), ImVec2(c.x - r * 0.4f, c.y + r * 0.55f),
                              ImVec2(c.x + r * 0.6f, c.y), IM_COL32(240, 240, 240, 255));
    }
}

// One pose or clip row: its icon, then the label. True when clicked (same as a Selectable).
bool App::library_row(const LibraryItem& it, const std::string& label) {
    const float icon = ImGui::GetFontSize() * 2.2f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable(("##" + it.id).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, icon));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_library_icon(dl, at, icon, it);
    dl->AddText(ImVec2(at.x + icon + 8, at.y + (icon - ImGui::GetFontSize()) / 2), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    return clicked;
}

void App::draw_inventory_panel() {
    if (!ImGui::Begin("Inventory")) return ImGui::End();
    {
        ImGui::SetNextItemWidth(-1);
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", inv_filter_.c_str());
        if (ImGui::InputTextWithHint("##invfilter", "Filter by name...", buf, sizeof buf)) inv_filter_ = buf;
    }
    draw_file_library();  // Projects and Animations (file_library_ui.cpp)
    draw_bodies_section();
    if (section_header("Meshes")) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Drag onto a bone to attach, double-click to add, right-click for more.");
        ImGui::PopStyleColor();
        draw_prop_grid(prop_library_, true);
        if (icon_label_button(icon::kImport, "Import .dae / .fbx...")) run_action("import_prop");
        if (!starter_props_.empty()) {
            ImGui::SeparatorText("Starter props");
            draw_prop_grid(starter_props_, false);
        }
    }
    ImGui::SeparatorText("Poses");
    if (inv_scroll_poses_) {  // --tab poses: the panel is docked to size after the first frames
        ImGui::SetScrollHereY(0);
        if (ImGui::GetFrameCount() > 3) inv_scroll_poses_ = false;
    }
    if (icon_label_button(icon::kAddToLibrary, "Save Pose...")) {
        name_prompt_ = selection_.empty() ? "Whole pose" : "Selected bones";
        name_action_ = NameAction::SavePose;
    }
    ImGui::SetItemTooltip("Save the pose at this frame: the selected bones, or the whole body when nothing is selected");
    ImGui::SameLine();
    double a, b;
    bool range = clip_range(a, b);
    ImGui::BeginDisabled(!range || selection_.empty());
    if (icon_label_button(icon::kAddToLibrary, "Save Clip...")) {
        name_prompt_ = "Clip";
        name_action_ = NameAction::SaveClip;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", range && !selection_.empty() ? "Save the selected bones over the chosen frames"
                                                             : "Select bones, then Shift-drag a frame range on the timeline "
                                                               "or select keys in the graph");
    ImGui::Checkbox("Apply mirrored", &apply_mirrored_);
    ImGui::Separator();

    int remove = -1, rename = -1;
    for (int pass = 0; pass < 2; ++pass) {
        ImGui::TextDisabled(pass == 0 ? "Poses" : "Clips");
        for (int i = 0; i < int(library_.items.size()); ++i) {
            LibraryItem& it = library_.items[i];
            if (it.clip != (pass == 1) || !inv_match(it.name)) continue;
            ImGui::PushID(i);
            std::string label = it.name + "   ";
            if (!it.side.empty()) label += it.side + " ";
            label += it.kind;
            if (it.clip) label += " (" + std::to_string(int(it.length)) + " f)";
            if (library_row(it, label) && ImGui::IsMouseDoubleClicked(0)) use_library_item(i, apply_mirrored_);
            ImGui::SetItemTooltip("Double-click to %s at frame %d, or drag onto the view", it.clip ? "paste" : "apply", int(frame_));
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("VATS_POSE", it.id.c_str(), it.id.size() + 1);
                ImGui::TextUnformatted(it.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(it.clip ? "Paste at This Frame" : "Apply at This Frame")) use_library_item(i, false);
                if (ImGui::MenuItem(it.clip ? "Paste Mirrored" : "Apply Mirrored")) use_library_item(i, true);
                if (it.clip && ImGui::MenuItem("Paste, Matching Poses...")) open_match_poses(library_clip(it, apply_mirrored_), it.name);
                if (!it.clip && ImGui::MenuItem("Show as Ghost")) pin_pose_ghost(it);  // 08 ON-5
                ImGui::Separator();
                if (menu_item_icon(icon::kRename, "Rename...")) rename = i;
                if (menu_item_icon(icon::kDelete, "Delete")) remove = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    // Rename (06 section 4.2): a small prompt of its own, so it never mixes with the save prompt.
    static int renaming = -1;
    static bool renaming_prop = false;
    static char rename_buf[128];
    if (rename >= 0 || prop_to_rename >= 0) {
        renaming_prop = prop_to_rename >= 0;
        renaming = renaming_prop ? prop_to_rename : rename;
        prop_to_rename = -1;
        const std::string& name = renaming_prop ? prop_library_[renaming].prop.name : library_.items[renaming].name;
        std::snprintf(rename_buf, sizeof rename_buf, "%s", name.c_str());
        ImGui::OpenPopup("Rename");
    }
    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool ok = ImGui::InputText("##rename", rename_buf, sizeof rename_buf, ImGuiInputTextFlags_EnterReturnsTrue);
        ok = ImGui::Button("Rename", ImVec2(90, 0)) || ok;
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        bool valid = renaming >= 0 && renaming < int(renaming_prop ? prop_library_.size() : library_.items.size());
        if (ok && rename_buf[0] && valid) {
            if (renaming_prop) {
                prop_library_[renaming].prop.name = rename_buf;
                save_prop_library();
            } else {
                library_.items[renaming].name = rename_buf;
                save_library();
            }
        }
        if ((ok && rename_buf[0]) || cancel || !valid) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Built-in starter poses: hand shapes (authored for the left hand) and a few body poses.
    if (section_header("Starter poses")) {
        hint("Click a hand pose for the left hand, Shift+click for the right.");
        for (const LibraryItem& it : builtin_poses(skel_)) {
            if (!inv_match(it.name)) continue;
            ImGui::PushID(it.id.c_str());
            std::string label = it.name + (it.kind == "hand" ? "   hand" : "   body");
            if (library_row(it, label)) {
                bool right = ImGui::GetIO().KeyShift;
                apply_library_item(it, it.kind == "hand" && right);
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("VATS_POSE", it.id.c_str(), it.id.size() + 1);
                ImGui::TextUnformatted(it.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (it.kind == "hand") {
                    if (ImGui::MenuItem("Left Hand")) apply_library_item(it, false);
                    if (ImGui::MenuItem("Right Hand")) apply_library_item(it, true);
                    if (ImGui::MenuItem("Both Hands")) {
                        Clip before = doc_.clip();
                        edit("Apply Pose", [&](Clip& c) {
                            apply_pose(c, skel_, it, std::round(frame_), false);
                            apply_pose(c, skel_, it, std::round(frame_), true);
                        });
                        offer_pose_blend(std::move(before), std::round(frame_));
                    }
                } else if (ImGui::MenuItem("Apply at This Frame")) {
                    apply_library_item(it, false);
                }
                if (ImGui::MenuItem("Show as Ghost")) pin_pose_ghost(it);  // 08 ON-5
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (remove >= 0) {
        std::string name = library_.items[remove].name;
        std::string text = "Delete \"" + name + "\" from the Inventory? This cannot be undone.";
        // The host may answer later, so the item is checked to still be there.
        // ponytail: by position and name; an id match if pose ids ever need to be unique.
        host_.ask("Viewport Avatar Toolset", text, {"Delete", "Cancel"}, [this, remove, name](int choice) {
            if (choice != 0 || remove >= int(library_.items.size()) || library_.items[remove].name != name) return;
            library_.items.erase(library_.items.begin() + remove);
            save_library();
            status("Deleted " + name);
        });
    }
    ImGui::End();
}

void App::use_library_item(int index, bool mirrored) { apply_library_item(library_.items[index], mirrored); }

void App::apply_library_item(const LibraryItem& it, bool mirrored) {
    if (it.clip) {
        std::vector<std::string> warnings;
        edit("Paste Clip", [&](Clip& c) { paste_clip(c, skel_, it, std::round(frame_), mirrored, &warnings); });
        status("Pasted " + it.name + " at frame " + std::to_string(int(frame_)) + (mirrored ? " (mirrored)" : ""));
        if (!warnings.empty()) {
            std::string t;
            for (auto& w : warnings) t += "- " + w + "\n";
            message("Clip pasted", t);
        }
    } else {
        Clip before = doc_.clip();
        edit("Apply Pose", [&](Clip& c) { apply_pose(c, skel_, it, std::round(frame_), mirrored); });
        offer_pose_blend(std::move(before), std::round(frame_));
        status("Applied " + it.name + " at frame " + std::to_string(int(frame_)) + (mirrored ? " (mirrored)" : ""));
    }
}

void App::draw_name_prompt() {
    if (name_action_ == NameAction::None) return;
    if (!ImGui::IsPopupOpen("Name")) {
        ImGui::OpenPopup("Name");
        std::snprintf(name_buf_, sizeof name_buf_, "%s", name_prompt_.c_str());
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Name", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    bool is_clip = name_action_ == NameAction::SaveClip || name_action_ == NameAction::SavePartClip;
    ImGui::TextUnformatted(is_clip ? "Name for this clip" : "Name for this pose");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool ok = ImGui::InputText("##name", name_buf_, sizeof name_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
    ok = ImGui::Button("Save", ImVec2(90, 0)) || ok;
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (ok && name_buf_[0]) {
        std::vector<int> bones;
        std::string kind = selection_.empty() ? "pose" : "selection";
        if (name_action_ == NameAction::SavePartPose) {
            const std::string kind = context_part_.kind == PartKind::Arm ? "arm" : context_part_.kind == PartKind::Hand ? "hand"
                                   : context_part_.kind == PartKind::Leg ? "leg" : context_part_.kind == PartKind::Wing ? "wing"
                                   : context_part_.kind == PartKind::HindLeg ? "hindleg" : context_part_.kind == PartKind::Tail ? "tail"
                                   : "head";
            LibraryItem item = make_pose(skel_, pose_, context_part_.bones, kind, context_part_.side, false);
            item.name = name_buf_;
            store_library_item(std::move(item));
            status("Saved " + context_part_.label + " pose " + std::string(name_buf_));
        } else if (name_action_ == NameAction::SavePartClip) {
            double a, b;
            if (clip_range(a, b)) {
                std::vector<std::string> tracks = part_tracks(context_part_);
                tracks.insert(tracks.end(), context_part_.ik_tracks.begin(), context_part_.ik_tracks.end());
                if (context_part_.kind == PartKind::Torso) {  // AM-94: the torso carries every track
                    tracks.clear();
                    for (auto& [name, t] : doc_.clip().curves) tracks.push_back(name);
                }
                const std::string kind = context_part_.kind == PartKind::Arm ? "arm" : context_part_.kind == PartKind::Hand ? "hand"
                                       : context_part_.kind == PartKind::Leg ? "leg" : context_part_.kind == PartKind::Wing ? "wing"
                                       : context_part_.kind == PartKind::HindLeg ? "hindleg" : context_part_.kind == PartKind::Tail ? "tail"
                                       : context_part_.kind == PartKind::Head ? "head"
                                       : context_part_.kind == PartKind::Torso ? "torso" : "pose";
                LibraryItem item = make_clip(doc_.clip(), tracks, a, b, kind, context_part_.side);
                item.name = name_buf_;
                store_library_item(std::move(item));
                status("Saved " + context_part_.label + " clip " + std::string(name_buf_));
            }
        } else if (name_action_ == NameAction::SavePose) {
            if (selection_.empty()) {
                for (int i = 0; i < skel_.joint_count(); ++i)
                    if (node_visible(i) || doc_.clip().curves.count(skel_[i].name)) bones.push_back(i);
            } else {
                bones = selection_;
            }
            LibraryItem item = make_pose(skel_, pose_, bones, kind, "", selection_.empty());
            item.name = name_buf_;
            store_library_item(std::move(item));
            status("Saved pose " + std::string(name_buf_));
        } else {
            double a, b;
            if (clip_range(a, b)) {
                std::vector<std::string> tracks = selected_tracks();  // bones, their pins, selected IK handles
                LibraryItem item = make_clip(doc_.clip(), tracks, a, b, "selection", "");
                item.name = name_buf_;
                store_library_item(std::move(item));
                status("Saved clip " + std::string(name_buf_) + " (" + std::to_string(int(b - a)) + " frames)");
            }
        }
    }
    if ((ok && name_buf_[0]) || cancel) {
        name_action_ = NameAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace vats
