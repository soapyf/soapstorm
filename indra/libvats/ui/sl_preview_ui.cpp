// Viewport Avatar Toolset - Preview as SL plays it, and the upload meter.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 10 (SP, UM). The logic is in the core (sl_preview.h). Both features read one in-memory
// export of the active actor, remade only when its anim_hash changes and nothing is being dragged.
#include <algorithm>
#include <cstdio>
#include <optional>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "widgets.h"
#include "theme.h"
#include "vats/retarget.h"
#include "vats/sl_preview.h"

namespace vats {

struct SlExport {
    std::uint64_t hash = 0;
    bool valid = false;     // made for hash
    bool has_file = false;  // there is a file with joints (it may still be over SL's limits)
    bool raw = false;       // IO-22: an unedited import, going out as it came in
    AnimExportResult r;
    std::size_t bytes = 0;
    AnimFile played;  // parsed back from the bytes: what the preview plays
    AnimCost cost;
    Clip clip;  // what it was exported from (mirrored with Export mirrored), for the deviation table
    std::optional<Shape> shape, positions;  // the bake shape and joint positions it used
    double seconds = 0;
    std::vector<BoneDeviation> dev;  // made when the preview window first needs it
    bool dev_done = false;
    std::uint64_t checked_ms = 0;
    bool edited = false;     // something was dragged since the last check: check at once when it ends
    bool meter_seen = false;  // the meter was drawn since the last tick
    std::optional<BudgetFit> fit;  // the last Fit to 250 KB
    bool keep_fit = false;         // its own tolerance change is about to remake the export: keep the report
    std::string confirm_split;     // part files an earlier split left, until replaced or cancelled
};

namespace {

std::string thousands(std::size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

// A bar towards a limit: green, amber from 90%, red when over it.
void meter_bar(double value, double limit, bool over, const char* text) {
    const ImVec4 c = over ? ImVec4(0.85f, 0.3f, 0.25f, 1) : value >= 0.9 * limit ? ImVec4(0.9f, 0.65f, 0.2f, 1)
                                                                                           : ImVec4(0.3f, 0.7f, 0.4f, 1);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
    ImGui::ProgressBar(float(std::clamp(value / limit, 0.0, 1.0)), ImVec2(-1, 0), text);
    ImGui::PopStyleColor();
}

}  // namespace

void App::sl_export_tick() {
    if (!sl_export_) sl_export_ = std::make_shared<SlExport>();
    SlExport& x = *sl_export_;
    const bool wanted = sl_preview_ || x.meter_seen;
    x.meter_seen = false;
    if (!wanted) return;  // costs nothing while neither shows
    // Never while something is dragged: the export follows once the drag is over.
    if (doc_.history.is_open() || dragging_gizmo_ || modal_ != Modal::None || scene_busy() || mocap_busy() ||
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        x.edited = true;
        return;
    }
    const std::uint64_t now = host_.ticks_ns() / 1000000;
    if (x.valid && !x.edited && now - x.checked_ms < 250) {  // at most four hash checks a second
        host_.wake(0.3);
        return;
    }
    x.edited = false;
    x.checked_ms = now;
    const AnimExportOptions opt = anim_export_options();
    // ponytail: other actors' clips are not in the hash; a cross-actor pin target edited there needs one more edit here.
    const std::uint64_t hash = anim_hash(doc_.clip(), opt.shape, opt.positions) ^ (std::uint64_t(doc_.project.active) << 1) ^
                               (raw_import_ ? 1 : 0);
    if (x.valid && hash == x.hash) return;

    // ponytail: made on the UI thread; a worker thread if long clips make the pause after an edit noticeable.
    SlExport fresh;
    fresh.hash = hash;
    fresh.valid = true;
    std::vector<std::uint8_t> bytes;
    fresh.raw = export_in_memory(fresh.r, bytes) == 2;
    fresh.bytes = bytes.size();
    std::string err;
    fresh.has_file = !fresh.r.file.joints.empty() && parse_anim(bytes, fresh.played, err);
    if (fresh.has_file) fresh.cost = anim_cost(skel_, fresh.played);
    fresh.clip = anim_export_clip();
    if (opt.shape) fresh.shape = *opt.shape;
    if (opt.positions) fresh.positions = *opt.positions;
    fresh.seconds = double(std::max(doc_.clip().end_frame, 1)) / std::max(doc_.clip().fps, 1);
    if (x.keep_fit) fresh.fit = x.fit;
    fresh.confirm_split = x.confirm_split;
    x = std::move(fresh);
}

void App::apply_sl_preview(Evaluation& e) {
    sl_ghost_.clear();
    if (!sl_preview_ || !sl_export_ || !sl_export_->has_file) return;
    // While something is dragged the view shows the edit itself; SL's playback returns with the next export. A press
    // on the view gets your pose too, so a drag starts from it and never from SL's (one frame, invisible).
    // e.pose stays yours, so Mirror and Save Pose never pick up SL's rounding; SL's goes to the host (sl_pose_).
    // ponytail: a move or rotate started from the keyboard (G/R) starts from the globals shown, SL's.
    const bool pressed = viewport_hovered_ && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                                               ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                                               ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
    if (doc_.history.is_open() || dragging_gizmo_ || modal_ != Modal::None || pressed) return;
    const SlExport& x = *sl_export_;
    Pose p = anim_pose(skel_, x.played, frame_ / std::max(x.clip.fps, 1), x.positions ? &*x.positions : nullptr);
    sl_ghost_ = std::move(e.globals);
    e.globals = skel_.global_pose(p, shape());
    sl_pose_ = std::move(p);
}

void App::draw_sl_preview_window() {
    if (!sl_preview_) return;
    place_tool_window("As SL Plays It", 24, 28);
    if (!ImGui::Begin("As SL Plays It", &sl_preview_)) return ImGui::End();
    help_button("sl-preview");
    SlExport* x = sl_export_.get();
    if (!x || !x->valid) {
        ImGui::TextDisabled("Exporting...");
        return ImGui::End();
    }
    if (!x->has_file) {
        ImGui::TextUnformatted("Nothing to play:");
        for (const std::string& e : x->r.errors) ImGui::BulletText("%s", e.c_str());
        return ImGui::End();
    }
    hint("The exported .anim as Second Life plays it; the green ghost is yours.");
    size_t rot = 0, pos = 0;
    for (const AnimJoint& j : x->played.joints) rot += j.rot.size(), pos += j.pos.size();
    ImGui::Text("%s bytes, %s, %zu rotation and %s", thousands(x->bytes).c_str(), count_noun(x->played.joints.size(), "bone").c_str(),
                rot, count_noun(pos, "position key").c_str());
    if (x->raw) ImGui::TextDisabled("Unchanged since import: plays the file as it came in");

    if (!x->dev_done) {  // once per export; rig_ carries this actor's cross-actor pin targets (evaluate())
        x->dev = anim_deviation(*rig_, x->clip, x->played, x->shape ? &*x->shape : nullptr,
                                x->positions ? &*x->positions : nullptr);
        x->dev_done = true;
    }
    subheading("Largest difference per bone");
    const int exact = int(std::count_if(x->dev.begin(), x->dev.end(), [](const BoneDeviation& d) { return d.mm < 0.005 && d.deg < 0.005; }));
    if (exact == 1) ImGui::TextDisabled("1 bone matches exactly and is not listed");
    else if (exact) ImGui::TextDisabled("%d bones match exactly and are not listed", exact);
    const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("deviation", 3, flags)) return ImGui::End();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_NoSort, 3);
    ImGui::TableSetupColumn("mm", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 1);
    ImGui::TableSetupColumn("degrees", ImGuiTableColumnFlags_PreferSortDescending, 1);
    ImGui::TableHeadersRow();
    bool by_deg = false, ascending = false;
    if (const ImGuiTableSortSpecs* s = ImGui::TableGetSortSpecs(); s && s->SpecsCount > 0) {
        by_deg = s->Specs[0].ColumnIndex == 2;
        ascending = s->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
    }
    std::stable_sort(x->dev.begin(), x->dev.end(), [&](const BoneDeviation& a, const BoneDeviation& b) {
        const double ka = by_deg ? a.deg : a.mm, kb = by_deg ? b.deg : b.mm;
        return ascending ? ka < kb : ka > kb;
    });
    for (const BoneDeviation& d : x->dev) {
        if (d.mm < 0.005 && d.deg < 0.005) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(d.node);
        const bool selected = std::find(selection_.begin(), selection_.end(), d.node) != selection_.end();
        if (ImGui::Selectable(skel_[d.node].name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
            select(d.node, false);
            set_frame(by_deg ? d.frame_deg : d.frame_mm);
        }
        ImGui::SetItemTooltip("Worst at frame %d (position) and frame %d (rotation). Click to select the bone and go "
                              "to the frame.", d.frame_mm, d.frame_deg);
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", d.mm);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", d.deg);
    }
    ImGui::EndTable();
    ImGui::End();
}

std::string App::export_size_text() const {
    return sl_export_ && sl_export_->valid && sl_export_->has_file ? thousands(sl_export_->bytes) + " bytes" : "";
}

void App::draw_upload_meter() {
    if (!sl_export_) sl_export_ = std::make_shared<SlExport>();
    SlExport& x = *sl_export_;
    x.meter_seen = true;
    subheading("Upload size");
    if (!x.valid) {
        host_.wake();
        ImGui::TextDisabled("Measuring...");
        return;
    }
    char buf[128];
    if (x.has_file) {
        std::snprintf(buf, sizeof buf, "%s / 250,000 bytes", thousands(x.bytes).c_str());
        meter_bar(double(x.bytes), double(kAnimMaxUploadBytes), x.bytes >= kAnimMaxUploadBytes, buf);
    } else {
        ImGui::TextDisabled("No file to measure: %s", x.r.errors.empty() ? "nothing is keyed" : x.r.errors.front().c_str());
    }
    std::snprintf(buf, sizeof buf, "%.2f / 60 s", x.seconds);
    meter_bar(x.seconds, kAnimMaxDuration, x.seconds > kAnimMaxDuration, buf);
    ImGui::SetItemTooltip("SL refuses animations of 250,000 bytes or more, or longer than 60 seconds");

    if (x.has_file && x.cost.total) {
        const double total = double(x.cost.total);
        ImGui::TextDisabled("Rotations %.0f%%, positions %.0f%%, bone names and counts %.0f%%, header %.0f%%",
                            100 * x.cost.rot / total, 100 * x.cost.pos / total, 100 * x.cost.records / total,
                            100 * x.cost.header / total);
        if (ImGui::BeginTable("cost", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Part", 0, 3);
            ImGui::TableSetupColumn("Bytes", 0, 2);
            ImGui::TableSetupColumn("Share", 0, 1.3f);
            ImGui::TableSetupColumn("Rot / pos", 0, 2);
            ImGui::TableHeadersRow();
            const size_t shown = std::min<size_t>(x.cost.parts.size(), 8);
            for (size_t i = 0; i < shown; ++i) {
                const AnimCost::Part& p = x.cost.parts[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s (%d)", p.name.c_str(), p.joints);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(thousands(p.bytes).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", 100 * p.bytes / total);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%% / %.0f%%", 100 * p.rot / total, 100 * p.pos / total);
            }
            ImGui::EndTable();
            if (x.cost.parts.size() > shown) ImGui::TextDisabled("and %zu more parts", x.cost.parts.size() - shown);
        }
    }

    // Fit to 250 KB only while over the limit (or with its result to show).
    const bool over = x.bytes >= kAnimMaxUploadBytes || x.seconds > kAnimMaxDuration;
    if (!over && !x.fit) return;
    ImGui::BeginDisabled(!over || (!x.has_file && x.seconds <= kAnimMaxDuration));
    if (ImGui::Button("Fit to 250 KB")) {
        x.fit = fit_anim_budget(*rig_, anim_export_clip(), anim_export_options());
        x.keep_fit = false;
        const BudgetFit& f = *x.fit;
        if (f.fits && f.steps) {
            if (f.world_m > 0) {  // 08 WR-5: Anywhere on the body is the tolerance raised
                edit("Fit to 250 KB", [&](Clip& c) { c.export_settings.set("reduce_world", f.world_m); });
            } else {
                Json a = Json::array();
                a.push(f.rot_deg);
                a.push(f.pos_m);
                edit("Fit to 250 KB", [&](Clip& c) { c.export_settings.set("reduce", a); });
            }
            x.keep_fit = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", over ? "Raises Reduce keys step by step until the file is under 250,000 bytes"
                                     : "The animation fits already");
    if (x.fit) {
        const BudgetFit& f = *x.fit;
        if (f.fits) {
            char at[64];
            if (f.world_m > 0) std::snprintf(at, sizeof at, "%.2f mm anywhere on the body", f.world_m * 1000);
            else std::snprintf(at, sizeof at, "%.3f deg / %.2f mm", f.rot_deg, f.pos_m * 1000);
            std::snprintf(buf, sizeof buf, "Fits at %s: %s bytes. Largest change %.1f mm (%s), %.2f deg (%s).", at,
                          thousands(f.bytes).c_str(), f.max_mm, f.worst_mm.c_str(), f.max_deg, f.worst_deg.c_str());
            hint(buf);
        } else {
            if (f.too_long)
                hint("Over 60 seconds: no key reduction helps. Split it into parts that play one after another.");
            else {
                std::snprintf(buf, sizeof buf,
                              "Still %s bytes at %s. The keys you set are always kept, so a clip keyed on "
                              "most frames needs splitting.",
                              thousands(f.bytes).c_str(), f.world_m > 0 ? "50 mm anywhere on the body" : "5 deg / 50 mm");
                hint(buf);
            }
            if (ImGui::Button((std::string(icon::kSplit) + " Split into Parts...").c_str(), ImVec2(-1, 0))) {
                FitOptions fo;
                fo.shape = export_shape();
                if (doc_.path.empty()) message("Save the project first", "The parts are saved as projects beside it.");
                else if (multi_actor()) message("Cannot split", "Split into Parts works on a project with one actor.");
                else guarded(doc_.path, [&] { split_into_parts(doc_.clip(), doc_.path, fo, false, x.confirm_split, ""); });
            }
            ImGui::SetItemTooltip("Consecutive parts, each under 60 s and the upload limit, saved as projects beside this "
                                  "one; each part is fitted as Retargeting fits a clip");
            if (!x.confirm_split.empty()) {
                ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "These parts exist already and would be replaced (kept as .bak):");
                ImGui::TextUnformatted(x.confirm_split.c_str());
                if (ImGui::Button("Replace Them")) {
                    x.confirm_split.clear();
                    FitOptions fo;
                    fo.shape = export_shape();
                    guarded(doc_.path, [&] { split_into_parts(doc_.clip(), doc_.path, fo, true, x.confirm_split, ""); });
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) x.confirm_split.clear();
            }
        }
    }
}

}  // namespace vats
