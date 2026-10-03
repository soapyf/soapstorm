// Viewport Avatar Toolset - the help browser: the shipped wiki (docs/wiki, vats/wiki.h) drawn with ImGui.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>

#include "app.h"
#include "imgui_internal.h"
#include "vats/gif.h"
#include "vats/wiki.h"
#include "theme.h"
#include "widgets.h"

namespace vats {

struct HelpUi {
    wiki::Library lib;
    std::string dir;
    ui::Host* host = nullptr;  // opens web links, loads images
    std::function<void(const std::string&)> open_example;  // an example link's project file, in <dir>/examples
    std::function<void(const std::string&)> show_target;   // a target link's: shown as the target ghost
    bool loaded = false, open = false, focus = false;
    bool modal = false;  // opened from a modal dialog: drawn as a nested modal there, not as a window
    bool pages = false;  // docked narrow: the contents list shows in place of the page
    std::vector<std::pair<std::string, std::string>> history;  // page file, heading
    int at = -1;
    bool scroll = false;  // move to the heading (or the top) after the next draw
    std::map<std::string, float> anchors;  // anchor key -> y in the page view, from the last draw
    char query[128] = "";
    std::string searched;
    std::vector<wiki::Hit> hits;
    const void* hovered = nullptr;  // the link span under the mouse last frame, so all its words underline
    const void* hovering = nullptr;
    // The shown page's images by path, loaded as first drawn and freed when another page shows or the help closes.
    // A GIF holds memory only while on screen: it is decoded (every frame, stb_image) when it scrolls into view and
    // shown through one texture updated in place; frames and texture go once it has been off screen for kGifKeepNs,
    // and scrolling back decodes it again. Peak: the visible GIFs' frames, w x h x 4 bytes each (rotate-drag.gif, 96
    // frames at 360x290: 40 MB).
    struct Image {
        ImTextureID tex = 0;  // a GIF's: its frame `shown`
        int w = 0, h = 0;
        bool gif = false, failed = false;  // failed: no decode, or the host makes no textures; the alt text shows
        GifImage px;                       // a GIF's frames, while on screen
        int at = 0, shown = -1;
        std::uint64_t next_ns = 0;  // when frame `at` ends; 0 = not playing
        std::uint64_t seen_ns = 0;  // when last on screen
        bool paused = false;        // by a click
    };
    static constexpr std::uint64_t kGifKeepNs = 2'000'000'000;
    std::map<std::string, Image> images;
    std::string images_page;
    // GIFs play only while the app has the system's focus and the help window has ImGui's; live_before is last frame's.
    bool app_focused = true, live = false, live_before = false;

    ~HelpUi() { free_images(); }  // App::shutdown drops the help before the host's GL context goes
    void free_images() {
        for (auto& [path, im] : images)
            if (im.tex) host->free_texture(im.tex);
        images.clear();
        images_page.clear();
    }
    // Once a frame, drawn or not (a collapsed help draws no page): drops the GIFs off screen for kGifKeepNs.
    void release_idle_gifs() {
        const std::uint64_t now = host->ticks_ns();
        for (auto& [path, im] : images) {
            if (!im.gif || (!im.tex && im.px.rgba.empty())) continue;
            if (now - im.seen_ns >= kGifKeepNs) {
                if (im.tex) host->free_texture(im.tex);
                im.tex = 0, im.shown = -1, im.px = {}, im.next_ns = 0;
            } else {
                host->wake(double(im.seen_ns + kGifKeepNs - now) * 1e-9);  // a sleeping host still gets to free it
            }
        }
    }

    void load() {
        if (loaded) return;
        loaded = true;
        lib.load_dir(dir);
    }
    void go(const std::string& file, const std::string& anchor) {
        history.resize(size_t(at + 1));
        history.emplace_back(file, anchor);
        at = int(history.size()) - 1;
        scroll = true, pages = false;
    }
    const wiki::Page* home() const {
        if (const wiki::Page* p = lib.find("vats")) return p;
        return lib.pages().empty() ? nullptr : &lib.pages()[0];
    }
    const wiki::Page* page() const { return at >= 0 ? lib.find(history[size_t(at)].first) : home(); }
};

namespace {

// Colours with a meaning, kept in every theme like the timeline's key and pin colours.
constexpr ImU32 kNote = IM_COL32(96, 156, 232, 255);
constexpr ImU32 kTip = IM_COL32(104, 192, 124, 255);
constexpr ImU32 kWarning = IM_COL32(236, 150, 64, 255);
constexpr ImU32 kBroken = IM_COL32(232, 104, 92, 255);

ImU32 with_alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT); }

// Spans laid out within width w at scale x the font size, as draw_spans draws them.
wiki::Layout lay_out(const std::vector<wiki::Span>& spans, float w, float scale, bool bold) {
    const float size = ImGui::GetFontSize() * scale;
    return wiki::lay_out(spans, w, size * 0.15f, size * 0.6f, size * 0.3f, [&](const wiki::Span& s, std::string_view t) {
        return (s.bold || bold ? bold_font() : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0, t.data(), t.data() + t.size()).x;
    });
}

// Word-wrapped runs from the cursor within width w, at scale x the font size. Returns the clicked link.
const wiki::Span* draw_spans(HelpUi& ui, const std::vector<wiki::Span>& spans, float w, float scale = 1,
                             bool bold = false, const char* prefix = nullptr, ImU32 colour = 0) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    const float size = ImGui::GetFontSize() * scale, line_h = size * 1.35f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImU32 text = colour ? colour : ImGui::GetColorU32(ImGuiCol_Text);
    // A prefix lays out as a bold span of its own before the others.
    std::vector<wiki::Span> with_prefix;
    if (prefix) {
        with_prefix.resize(1), with_prefix[0].text = prefix, with_prefix[0].bold = true;
        with_prefix.insert(with_prefix.end(), spans.begin(), spans.end());
    }
    const std::vector<wiki::Span>& all = prefix ? with_prefix : spans;
    auto font_of = [&](const wiki::Span& s) { return s.bold || bold ? bold_font() : ImGui::GetFont(); };
    const float button_pad = size * 0.6f;
    const wiki::Layout lay = lay_out(all, w, scale, bold);
    const float text_dy = (line_h - size) * 0.5f;
    // Code backgrounds first, so no text is drawn over; each covers its words on a line and pads them.
    for (const wiki::Layout::Box& bx : lay.boxes) {
        const float y = start.y + float(bx.line) * line_h + text_dy;
        dl->AddRectFilled(ImVec2(start.x + bx.x0, y - 1), ImVec2(start.x + bx.x1, y + size + 1), ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    }
    const wiki::Span* clicked = nullptr;
    for (const wiki::Layout::Piece& pc : lay.pieces) {
        const size_t i = prefix ? pc.span : pc.span + 1;  // spans[i - 1]; 0 is the prefix
        const wiki::Span& s = i == 0 ? all[0] : spans[i - 1];
        ImFont* font = font_of(s);
        const char *p = s.text.c_str() + pc.begin, *q = s.text.c_str() + pc.end;
        const float x = start.x + pc.x, y = start.y + float(pc.line) * line_h;
        ImGui::PushID(int(i));
        const ImGuiID id = win->GetID(int(pc.begin));
        ImGui::PopID();
        if (s.example) {  // a button
            const ImRect bb(ImVec2(x, y + 1), ImVec2(x + pc.width, y + line_h - 1));
            ImGui::ItemAdd(bb, id);
            bool hov = false, held = false;
            if (ImGui::ButtonBehavior(bb, id, &hov, &held) && i > 0) clicked = &s;
            dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : hov ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                              ImGui::GetStyle().FrameRounding);
            dl->AddText(font, size, ImVec2(x + button_pad, y + text_dy), text, p, q);
            if (hov && s.ghost) ImGui::SetTooltip("Shows %s as a green see-through target over your avatar; your project stays open", s.target.c_str());
            else if (hov) ImGui::SetTooltip("Opens %s as a new, untitled project", s.target.c_str());
            continue;
        }
        const bool broken = s.link && !s.external && !s.target.empty() && !ui.lib.find(s.target);
        const ImU32 col = s.link ? (broken ? kBroken : ImGui::GetColorU32(ImGuiCol_TextLink))
                        : s.italic ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : text;
        const ImVec2 a(x, y + text_dy), b(x + pc.width, a.y + size);
        dl->AddText(font, size, a, col, p, q);
        if (s.link) {
            const ImRect bb(a, b);
            ImGui::ItemAdd(bb, id);
            bool hov = false, held = false;
            if (ImGui::ButtonBehavior(bb, id, &hov, &held) && i > 0) clicked = &s;  // the prefix is never a link
            if (hov) {
                ui.hovering = &s;
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (s.external) ImGui::SetTooltip("%s", s.target.c_str());
                else if (broken) ImGui::SetTooltip("No help page named \"%s\"", s.target.c_str());
            }
            if (ui.hovered == &s) dl->AddLine(ImVec2(a.x, b.y), b, col);
        }
    }
    ImGui::Dummy(ImVec2(lay.right, float(lay.lines) * line_h));
    return clicked;
}

// A Note, Tip, Warning, quote or Related box: tinted background, coloured bar on the left.
const wiki::Span* draw_box(HelpUi& ui, const wiki::Block& b, float w) {
    const char* label = b.kind == wiki::Block::Note ? "Note: " : b.kind == wiki::Block::Tip ? "Tip: "
                      : b.kind == wiki::Block::Warning ? "Warning: " : b.kind == wiki::Block::Related ? "Related articles: " : nullptr;
    const ImU32 bar = b.kind == wiki::Block::Note ? kNote : b.kind == wiki::Block::Tip ? kTip
                    : b.kind == wiki::Block::Warning ? kWarning : ImGui::GetColorU32(ImGuiCol_Border);
    const float pad = ImGui::GetFontSize() * 0.55f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad * 1.6f, p0.y + pad * 0.6f));
    const wiki::Span* clicked = draw_spans(ui, b.spans, w - pad * 2.6f, 1, false, label);
    const ImVec2 p1(p0.x + w, ImGui::GetItemRectMax().y + pad * 0.6f);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(p0, p1, with_alpha(bar, b.kind == wiki::Block::Quote || b.kind == wiki::Block::Related ? 24 : 34), 3);
    dl->AddRectFilled(p0, ImVec2(p0.x + 3, p1.y), bar);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, p1.y - p0.y));
    return clicked;
}

// A GIF on screen: decodes it if it holds no frames, moves it on to the frame due now and puts that frame in its
// texture; returns the texture, 0 when it cannot be shown.
ImTextureID gif_now(HelpUi& ui, HelpUi::Image& im, const std::string& path) {
    const std::uint64_t now = ui.host->ticks_ns();
    im.seen_ns = now;
    if (im.px.rgba.empty()) {
        std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!read_gif(bytes.data(), bytes.size(), im.px) || im.px.width != im.w || im.px.height != im.h) {
            im.px = {}, im.failed = true;
            return 0;
        }
        if (im.at >= im.px.frames()) im.at = 0;
    }
    const int n = im.px.frames();
    // Browsers show a delay under 20 ms as 100 ms; so do we, so a GIF plays here as it does on the web.
    auto delay_ns = [&](int i) {
        const int ms = im.px.delays_ms[size_t(i)];
        return std::uint64_t(ms < 20 ? 100 : ms) * 1000000u;
    };
    if (!ui.live) {
        im.at = 0, im.next_ns = 0;  // still, on the first frame
    } else if (!im.paused && n > 1) {
        if (!im.next_ns || now > im.next_ns + 1000000000u) im.next_ns = now + delay_ns(im.at);  // start, or back from a stall
        while (now >= im.next_ns) im.at = (im.at + 1) % n, im.next_ns += delay_ns(im.at);
        ui.host->wake(double(im.next_ns - now) * 1e-9);
    } else {
        im.next_ns = 0;  // paused: resumes with the whole of this frame
    }
    if (im.shown != im.at) {
        const std::uint8_t* frame = &im.px.rgba[size_t(im.at) * size_t(im.w) * size_t(im.h) * 4];
        if (!im.tex || !ui.host->update_texture(im.tex, frame, im.w, im.h)) {  // a host that cannot update makes anew
            if (im.tex) ui.host->free_texture(im.tex);
            im.tex = ui.host->make_texture(frame, im.w, im.h);
        }
        im.shown = im.tex ? im.at : -1;
        if (!im.tex) im.px = {}, im.failed = true;
    }
    return im.tex;
}

// An image scaled down to the column width, or its alt text in a frame when the file is missing; then its caption.
// A GIF plays in place, looping, and a click pauses and resumes it.
const wiki::Span* draw_image(HelpUi& ui, const wiki::Page& page, const wiki::Block& b, float w) {
    if (ui.images_page != page.file) ui.free_images(), ui.images_page = page.file;
    auto [it, fresh] = ui.images.try_emplace(b.image);
    HelpUi::Image& im = it->second;
    const bool inside = !b.image.empty() && b.image[0] != '/' && b.image.find("..") == std::string::npos;
    const std::string path = ui.dir + "/" + b.image;
    if (fresh && inside) {
        im.gif = b.image.size() > 4 && b.image.compare(b.image.size() - 4, 4, ".gif") == 0;
        if (im.gif) wiki::gif_size(path, im.w, im.h);
        else if (wiki::png_size(path, im.w, im.h)) im.tex = ui.host->load_texture(path);
    }
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const float dw = im.w > 0 ? std::min(w, float(im.w) * ImGui::GetFontSize() / 15.f) : 0;  // shots are at 100%: 15 px text (load_fonts)
    const ImVec2 p = ImGui::GetCursorScreenPos(), sz(dw, im.w > 0 ? dw * float(im.h) / float(im.w) : 0);
    const bool gif = im.gif && im.w > 0 && !im.failed;
    const ImTextureID tex = gif ? (ImGui::IsRectVisible(sz) ? gif_now(ui, im, path) : 0) : im.tex;
    if (tex || (gif && !im.failed)) {  // a GIF off screen: its frame only
        if (im.gif) {
            if (ImGui::InvisibleButton("gif", sz) && ui.live_before) im.paused = !im.paused, im.next_ns = 0;
            if (tex) ImGui::GetWindowDrawList()->AddImage(tex, p, ImVec2(p.x + sz.x, p.y + sz.y));
            if (im.paused && im.px.frames() > 1) {  // a play sign in the middle
                const float r = ImGui::GetFontSize() * 1.1f;
                const ImVec2 c(p.x + sz.x * 0.5f, p.y + sz.y * 0.5f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 140));
                dl->AddTriangleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.5f), ImVec2(c.x - r * 0.35f, c.y + r * 0.5f),
                                      ImVec2(c.x + r * 0.55f, c.y), IM_COL32(255, 255, 255, 230));
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && im.px.frames() > 1)
                ImGui::SetTooltip("%s%sClick to %s", b.alt.c_str(), b.alt.empty() ? "" : "\n", im.paused ? "play" : "pause");
            else if (!b.alt.empty()) ImGui::SetItemTooltip("%s", b.alt.c_str());
        } else {
            ImGui::Image(tex, sz);
            if (!b.alt.empty()) ImGui::SetItemTooltip("%s", b.alt.c_str());
        }
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), border);
    } else {
        const float pad = ImGui::GetFontSize() * 0.8f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
        std::vector<wiki::Span> alt(1);
        alt[0].text = b.alt.empty() ? b.image : b.alt;
        draw_spans(ui, alt, w - pad * 2, 1, false, nullptr, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        const ImVec2 p1(p0.x + w, ImGui::GetItemRectMax().y + pad);
        ImGui::GetWindowDrawList()->AddRect(p0, p1, border, 3);
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(w, p1.y - p0.y));
        ImGui::SetItemTooltip("Missing image %s", b.image.c_str());
    }
    return b.spans.empty() ? nullptr : draw_spans(ui, b.spans, w);
}

void draw_page(HelpUi& ui, const wiki::Page& page) {
    const float fs = ImGui::GetFontSize();
    const float w = std::min(ImGui::GetContentRegionAvail().x, fs * 46);  // a readable line length
    const wiki::Span* clicked = nullptr;
    auto take = [&](const wiki::Span* s) { clicked = s ? s : clicked; };
    ui.anchors.clear();
    ui.hovered = ui.hovering, ui.hovering = nullptr;
    std::vector<wiki::Span> title(1);
    title[0].text = page.title;
    draw_spans(ui, title, w, 1.6f, true);
    ImGui::Separator();
    for (size_t i = 0; i < page.blocks.size(); ++i) {
        const wiki::Block& b = page.blocks[i];
        ImGui::PushID(int(i));
        switch (b.kind) {
        case wiki::Block::Heading: {
            ImGui::Dummy(ImVec2(0, fs * (b.level <= 2 ? 0.6f : 0.25f)));
            std::string text;
            for (auto& s : b.spans) text += s.text;
            ui.anchors[wiki::anchor_key(text)] = ImGui::GetCursorPosY();
            draw_spans(ui, b.spans, w, b.level <= 2 ? 1.3f : 1.1f, true);
            if (b.level <= 2) {
                const ImVec2 a = ImGui::GetItemRectMin();
                const float y = ImGui::GetItemRectMax().y;
                ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(a.x + w, y), ImGui::GetColorU32(ImGuiCol_Separator));
            }
            break;
        }
        case wiki::Block::Paragraph:
            take(draw_spans(ui, b.spans, w));
            break;
        case wiki::Block::Bullet:
        case wiki::Block::Numbered: {
            const float indent = fs * (1.4f + 1.4f * b.level);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float mid = p.y + fs * 1.35f * 0.5f;
            ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            if (b.kind == wiki::Block::Bullet) {
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + indent - fs * 0.6f, mid), fs * 0.15f, dim);
            } else {
                const std::string n = std::to_string(b.number) + ".";
                const float nw = ImGui::CalcTextSize(n.c_str()).x;
                ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + indent - fs * 0.35f - nw, mid - fs * 0.5f), dim, n.c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x + indent, p.y));
            take(draw_spans(ui, b.spans, w - indent));
            break;
        }
        case wiki::Block::Code: {
            ImGui::BeginChild("code", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_FrameStyle,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(b.code.c_str());
            ImGui::EndChild();
            break;
        }
        case wiki::Block::Table: {
            int cols = 0;
            for (auto& r : b.rows) cols = std::max(cols, int(r.size()));
            // Columns fit their text, the last one taking the rest of the width; when the text is too wide for that,
            // every column wraps, narrowed in proportion to how wide it would be (wiki::column_widths).
            std::vector<float> natural(static_cast<size_t>(cols)), least(static_cast<size_t>(cols));
            for (size_t r = 0; r < b.rows.size(); ++r)
                for (size_t c = 0; c < b.rows[r].size(); ++c) {
                    const wiki::Layout lay = lay_out(b.rows[r][c], FLT_MAX, 1, r == 0);
                    natural[c] = std::max(natural[c], lay.right + 1);  // + 1: never wraps at its own width
                    for (const wiki::Layout::Piece& pc : lay.pieces) least[c] = std::max(least[c], pc.width + fs * 0.3f);
                }
            const float cell_pad = ImGui::GetStyle().CellPadding.x * 2 + 1;  // either side, and a border
            const std::vector<float> widths = wiki::column_widths(natural, least, w - float(cols) * cell_pad - 1);
            if (cols && ImGui::BeginTable("table", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(w, 0))) {
                for (int c = 0; c < cols; ++c)
                    ImGui::TableSetupColumn(nullptr, c + 1 < cols ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch,
                                            c + 1 < cols ? widths[size_t(c)] : 0);
                for (size_t r = 0; r < b.rows.size(); ++r) {
                    ImGui::TableNextRow();  // not a Headers row: those don't count towards fitting the columns
                    if (r == 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
                    for (size_t c = 0; c < b.rows[r].size(); ++c) {
                        ImGui::TableSetColumnIndex(int(c));
                        take(draw_spans(ui, b.rows[r][c], ImGui::GetContentRegionAvail().x, 1, r == 0));
                    }
                }
                ImGui::EndTable();
            }
            break;
        }
        case wiki::Block::Image:
            take(draw_image(ui, page, b, w));
            break;
        default:  // Note, Tip, Warning, Quote, Related
            take(draw_box(ui, b, w));
            break;
        }
        const bool list = b.kind == wiki::Block::Bullet || b.kind == wiki::Block::Numbered;
        const bool next_list = i + 1 < page.blocks.size() && (page.blocks[i + 1].kind == wiki::Block::Bullet ||
                                                              page.blocks[i + 1].kind == wiki::Block::Numbered);
        if (!(list && next_list) && b.kind != wiki::Block::Heading) ImGui::Dummy(ImVec2(0, fs * 0.2f));
        ImGui::PopID();
    }
    if (!page.category.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Category: %s", page.category.c_str());
    }

    if (ui.scroll) {  // after a link or Back: the heading, or the top
        const std::string& anchor = ui.history.empty() ? std::string() : ui.history[size_t(ui.at)].second;
        auto it = ui.anchors.find(wiki::anchor_key(anchor));
        ImGui::SetScrollY(!anchor.empty() && it != ui.anchors.end() ? it->second - fs * 0.5f : 0);
        ui.scroll = false;
    }
    if (clicked) {
        if (clicked->ghost) {
            if (ui.show_target) ui.show_target(clicked->target);
        } else if (clicked->example) {
            if (ui.open_example) ui.open_example(clicked->target);
        } else if (clicked->external) {
            // Only web pages leave the app, never file: or other schemes.
            if (clicked->target.rfind("https://", 0) == 0 || clicked->target.rfind("http://", 0) == 0)
                ui.host->open_url(clicked->target);
        } else if (const wiki::Page* to = clicked->target.empty() ? &page : ui.lib.find(clicked->target)) {
            ui.go(to->file, clicked->anchor);
        }
    }
}

void draw_help(HelpUi& ui) {
    ui.load();
    // The system focus from the platform glue's events (ImGui keeps only a one-frame "lost" flag). A click counts as
    // focus back, for glue that sends only the loss (the viewer's); glue that sends none counts as focused.
    for (const ImGuiInputEvent& e : GImGui->InputEventsTrail)
        if (e.Type == ImGuiInputEventType_Focus) ui.app_focused = e.AppFocused.Focused;
        else if (e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Down) ui.app_focused = true;
    ui.live_before = ui.live;
    ui.live = ui.app_focused && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const float fs = ImGui::GetFontSize();
    const wiki::Page* page = ui.page();

    // Docked into a narrow panel there is no room for both panes: a Pages button swaps the contents list for the page.
    const bool narrow = ImGui::GetContentRegionAvail().x < fs * 34;
    if (!narrow) ui.pages = false;

    auto toolbar = [&] {
        ImGui::BeginDisabled(ui.at <= 0);
        if (ImGui::ArrowButton("##back", ImGuiDir_Left)) --ui.at, ui.scroll = true, ui.pages = false;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Back");
        ImGui::SameLine();
        ImGui::BeginDisabled(ui.at + 1 >= int(ui.history.size()));
        if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) ++ui.at, ui.scroll = true, ui.pages = false;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Forward");
        ImGui::SameLine();
        if (ImGui::Button("Contents") && ui.home()) ui.go(ui.home()->file, "");
        if (!narrow) return;
        ImGui::SameLine();
        if (ImGui::Button(ui.pages ? "Page" : "Pages")) ui.pages = !ui.pages;
        ImGui::SetItemTooltip(ui.pages ? "Back to the page" : "Search and the list of pages");
    };
    auto contents = [&] {
        ImGui::SetNextItemWidth(-1);
        filter_input("##search", "Search help", ui.query, sizeof ui.query);
        if (ui.searched != ui.query) ui.searched = ui.query, ui.hits = ui.lib.search(ui.query);
        ImGui::BeginChild("list");
        if (ui.query[0]) {
            if (ui.hits.empty()) ImGui::TextDisabled("No pages match.");
            for (size_t i = 0; i < ui.hits.size(); ++i) {
                const wiki::Hit& h = ui.hits[i];
                ImGui::PushID(int(i));
                std::string label = h.page->title + (h.heading.empty() ? "" : "  \xE2\x80\xBA  " + h.heading);
                if (ImGui::Selectable(label.c_str(), page == h.page)) ui.go(h.page->file, h.heading);
                if (!h.snippet.empty()) hint(h.snippet.c_str());
                ImGui::PopID();
            }
        } else {
            for (auto& [category, pages] : ui.lib.contents()) {
                subheading(category.empty() ? "Other" : category.c_str());
                for (const wiki::Page* p : pages)
                    if (ImGui::Selectable(p->title.c_str(), page == p)) ui.go(p->file, "");
            }
        }
        ImGui::EndChild();
    };
    auto page_view = [&] {
        ImGui::BeginChild("page", ImVec2(0, 0), 0, ImGuiWindowFlags_NoSavedSettings);
        if (page) draw_page(ui, *page);
        else hint(("No help pages were found in " + ui.dir + ".").c_str());
        // The mouse's back and forward buttons.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            if (ImGui::IsMouseClicked(3) && ui.at > 0) --ui.at, ui.scroll = true;
            if (ImGui::IsMouseClicked(4) && ui.at + 1 < int(ui.history.size())) ++ui.at, ui.scroll = true;
        }
        ImGui::EndChild();
    };

    if (narrow) {
        toolbar();
        if (!ui.pages) return page_view();
        ImGui::BeginChild("nav narrow", ImVec2(0, 0), ImGuiChildFlags_Borders);
        contents();
        ImGui::EndChild();
        return;
    }
    ImGui::BeginChild("nav", ImVec2(fs * 15, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    contents();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    toolbar();
    page_view();
    ImGui::EndGroup();
}


}  // namespace

void App::open_help(const std::string& page, const std::string& anchor) {
    if (!help_ui_) help_ui_ = std::make_shared<HelpUi>(), help_ui_->dir = host_.paths().help, help_ui_->host = &host_;
    HelpUi& ui = *help_ui_;
    // An example opens as File > Open would, as an untitled copy: Save asks for a new name, so the shipped file
    // is never written.
    ui.open_example = [this](const std::string& file) {
        if (file.empty() || file.find_first_of("/\\") != std::string::npos || file.find("..") != std::string::npos) return;
        const std::string path = host_.paths().help + "/examples/" + file;
        guard_unsaved([this, path] { guarded(path, [&] { load_project_file(path, true); }); });
    };
    // A target link loads the same folder's project as the target ghost; the open project is not touched.
    ui.show_target = [this](const std::string& file) {
        if (file.empty() || file.find_first_of("/\\") != std::string::npos || file.find("..") != std::string::npos) return;
        cli_target(host_.paths().help + "/examples/" + file);
    };
    ui.load();
    const wiki::Page* p = page.empty() ? ui.home() : ui.lib.find(page);
    if (!p) p = ui.home();
    if (p && (ui.at < 0 || ui.history[size_t(ui.at)] != std::make_pair(p->file, anchor))) ui.go(p->file, anchor);
    ui.open = ui.focus = true;
    ui.modal = false;
}

void App::draw_help_browser() {
    if (help_ui_) help_ui_->release_idle_gifs();
    if (!help_ui_ || !help_ui_->open || help_ui_->modal) return;
    HelpUi& ui = *help_ui_;
    ImGui::SetNextWindowSize(window_size(62, 44), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (ui.focus) ImGui::SetNextWindowFocus(), ui.focus = false;
    if (ImGui::Begin("Help", &ui.open)) draw_help(ui);
    // Esc closes it while it has the keyboard, and only that (not Select None too); in its search box Esc leaves the box.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ui.open = false, skip_shortcuts_ = true;
    ImGui::End();
    if (!ui.open) ui.free_images();
}

void App::help_button(const char* page) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    bool pressed = false;
    // A "?" in a bar of the window's, drawn as ImGui draws the close button (imgui.cpp RenderWindowTitleBarContents),
    // right_x from its right end. Called with that bar's window current.
    auto question = [&](const ImRect& bar, float right_x) {
        ImGuiWindow* host = ImGui::GetCurrentWindow();
        const float sz = ImGui::GetFontSize();
        const ImVec2 pos(bar.Max.x - right_x - sz, (bar.Min.y + bar.Max.y - sz) * 0.5f);
        const ImRect bb(pos, ImVec2(pos.x + sz, pos.y + sz));
        const ImGuiID id = host->GetID(int(w->ID));
        ImGui::PushClipRect(bar.Min, bar.Max, false);
        ImGui::ItemAdd(bb, id);
        bool hov = false, held = false;
        pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
        if (hov)
            host->DrawList->AddCircleFilled(bb.GetCenter(), sz * 0.5f + 1,
                                            ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered));
        const float tw = ImGui::CalcTextSize("?").x;
        host->DrawList->AddText(ImVec2(bb.GetCenter().x - tw * 0.5f, bb.Min.y), ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled), "?");
        ImGui::PopClipRect();
        if (hov) ImGui::SetTooltip("Help for this window");
    };
    const ImGuiStyle& st = ImGui::GetStyle();
    if (!(w->Flags & ImGuiWindowFlags_NoTitleBar) && !w->DockIsActive) {
        // Beside the close button.
        question(w->TitleBarRect(), st.FramePadding.x + (w->HasCloseButton ? ImGui::GetFontSize() + st.ItemInnerSpacing.x : 0));
    } else if (ImGuiDockNode* node = w->DockNode; w->DockIsActive && node && node->VisibleWindow == w && node->HostWindow &&
                                                   node->TabBar && !node->IsHiddenTabBar()) {
        // Docked: at the right end of its tab strip, where the node's own close box would be (the dockspace has none),
        // while its tab is the one in front.
        const ImGuiTabBar& tabs = *node->TabBar;
        float tabs_right = tabs.BarRect.Min.x;
        for (const ImGuiTabItem& t : tabs.Tabs)  // the windows' tabs (not a "?" tab added below)
            if (!(t.Flags & ImGuiTabItemFlags_Button)) tabs_right = std::max(tabs_right, tabs.BarRect.Min.x + t.Offset + t.Width);
        const ImRect strip(tabs.BarRect.Min.x, node->Pos.y, node->Pos.x + node->Size.x, node->Pos.y + ImGui::GetFrameHeight());
        // A node that has its own close box (a floating group of tabs, a host's dockspace): the "?" goes before it.
        const float right = st.FramePadding.x + (node->HasCloseButton ? ImGui::GetFontSize() + st.ItemInnerSpacing.x : 0);
        if (tabs_right + 2 * ImGui::GetFontSize() < strip.Max.x - right) {
            ImGui::Begin(node->HostWindow->Name);  // the strip is the host's (as DockNodeBeginAmendTabBar does)
            question(strip, right);
            ImGui::End();
        } else if (ImGui::DockNodeBeginAmendTabBar(node)) {  // tabs fill the strip: a "?" tab after them, given room
            ImGui::PushID(int(w->ID));
            pressed = ImGui::TabItemButton("?", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
            ImGui::SetItemTooltip("Help for this window");
            ImGui::PopID();
            ImGui::DockNodeEndAmendTabBar();
        }
    } else if (w->DockIsActive) {  // a docked window with no tab bar: a small button on a row of its own
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("?").x - st.FramePadding.x * 2);
        pressed = ImGui::SmallButton("?");
        ImGui::SetItemTooltip("Help for this window");
    }
    const bool in_modal = (w->Flags & ImGuiWindowFlags_Modal) != 0;
    if (pressed) {
        const char* hash = std::strchr(page, '#');  // "page#heading"
        open_help(hash ? std::string(page, hash) : std::string(page), hash ? hash + 1 : "");
        if (in_modal) help_ui_->modal = true, ImGui::OpenPopup("Help##modal");
    }
    if (!in_modal || !help_ui_ || !help_ui_->modal) return;
    // A window can't rise above a modal dialog, so from one the help opens as a modal on top of it.
    ImGui::SetNextWindowSize(window_size(62, 40), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::BeginPopupModal("Help##modal", &open)) {
        draw_help(*help_ui_);
        ImGui::EndPopup();
    }
    if (!open) help_ui_->modal = help_ui_->open = false, help_ui_->free_images();
}

}  // namespace vats
