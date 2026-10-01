// Viewport Avatar Toolset - the shipped help wiki: a Markdown-subset parser, page index, links and search.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The subset is the one docs/wiki/STYLE.md allows: # to ### headings, paragraphs, **bold**, *italic*,
// `code`, fenced code blocks, bullet and numbered lists (one level of nesting), pipe tables, the Note / Tip /
// Warning boxes and the "Related articles" line, [[Page#Section|label]] and [text](https://...) links, images
// (![alt](images/<page>/<name>.png or .gif) on a line of its own, an optional *caption* line under it), example links
// ([text](example:<file>.vat), a project in examples/), target links ([text](target:<file>.vat), the same project
// shown as the target ghost) and closing "Category: ..." and "Order: <n>" lines. No UI here: the app (ImGui) and the viewer (LLUI) draw the same blocks.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vats::wiki {

struct Span {
    std::string text;
    bool bold = false, italic = false, code = false;
    // Links: a wiki page (target = page title, anchor = heading, may be empty) or an external URL.
    // Example links: target = the project's file name in <help>/examples/. A target link ([text](target:<file>.vat))
    // is an example link with ghost set: the button shows that example as the target ghost, the open project stays.
    bool link = false, external = false, example = false, ghost = false;
    std::string target, anchor;
};

struct Block {
    enum Kind { Heading, Paragraph, Bullet, Numbered, Code, Table, Note, Tip, Warning, Quote, Related, Image };
    Kind kind = Paragraph;
    int level = 0;        // heading 1-3; list nesting 0-1
    int number = 0;       // numbered list item
    std::vector<Span> spans;  // text of headings, paragraphs, list items, boxes and the Related line; image captions
    std::string code;         // code blocks
    std::vector<std::vector<std::vector<Span>>> rows;  // tables: rows of cells; row 0 is the header
    std::string image, alt;  // images: the path relative to the help folder, and the alt text
};

struct Page {
    std::string file;      // stem, e.g. "graph-editor"
    std::string title;     // from the first "# " line
    std::string category;  // from the "Category: " line, "" when absent
    int order = 0;         // from an "Order: <n>" line: its place within the category (0 = after those with one)
    std::vector<Block> blocks;
    std::vector<std::string> headings;  // "##" and "###" headings, in order (anchors)
    std::string plain;                  // lower-case text of the whole page, for search
};

Page parse_page(std::string_view markdown, std::string file);
// Inline markup of one line of text.
std::vector<Span> parse_inline(std::string_view text);

// The width and height of a PNG file from its header; false when it is not a readable PNG.
bool png_size(const std::string& path, int& width, int& height);
// The same for a GIF (its logical screen), from the header alone.
bool gif_size(const std::string& path, int& width, int& height);

// Word-wrapping of a run of spans, apart from any font or drawing: x runs from 0 to the width, lines count from 0.
struct Layout {
    struct Piece {  // text[begin, end) of spans[span], at x on its line, width wide
        size_t span = 0, begin = 0, end = 0;
        float x = 0, width = 0;
        int line = 0;
    };
    struct Box {  // the background behind a code span's words on one line
        int line = 0;
        float x0 = 0, x1 = 0;
    };
    std::vector<Piece> pieces;
    std::vector<Box> boxes;
    int lines = 1;
    float right = 0;  // the widest line's end
};
// Lays spans out within width w; measure gives the width of some text in a span's font. A word carries the spaces
// before it and a line drops its leading spaces. Code keeps `pad` of its background on either side of its text on
// every line, inside the width. An example link is one button, its text `button_pad` in from either side and `gap`
// after the text before it. A word wider than a whole line breaks after the last / \ _ - or . that fits, else after
// the last character that fits.
Layout lay_out(const std::vector<Span>& spans, float w, float pad, float button_pad, float gap,
               const std::function<float(const Span&, std::string_view)>& measure);
// Table column widths that fit avail: each column's natural (unwrapped) width when they all fit; else each gets at
// least its widest word (least) and the rest of the room is shared in proportion to how much wider each column would
// like to be. When even the widest words do not fit, the columns with the shortest keep theirs and the others share
// what is left alike.
std::vector<float> column_widths(const std::vector<float>& natural, const std::vector<float>& least, float avail);

// Normalises heading text for anchor matching: lower case, runs of spaces/hyphens/underscores as one "-".
std::string anchor_key(std::string_view heading);

struct Hit {
    const Page* page = nullptr;
    std::string heading;  // the heading the match falls under, "" for the page itself
    std::string snippet;  // plain text around the first body match
    int score = 0;
};

class Library {
public:
    // Reads every *.md in dir except STYLE.md and README.md. Returns the number of pages.
    int load_dir(const std::string& dir);
    void add(Page page);

    const std::vector<Page>& pages() const { return pages_; }
    // By title (case-insensitive) or by file stem.
    const Page* find(std::string_view title_or_file) const;
    // Categories in the fixed order of the style guide, then any others alphabetically; within one, pages by their
    // Order, then those without one by title.
    std::vector<std::pair<std::string, std::vector<const Page*>>> contents() const;
    // Every word of the query must appear; title matches first, then headings, then body.
    std::vector<Hit> search(std::string_view query, size_t max_hits = 30) const;

private:
    std::vector<Page> pages_;
};

}  // namespace vats::wiki
