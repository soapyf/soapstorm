// Viewport Avatar Toolset - the shipped help wiki: a Markdown-subset parser, page index, links and search.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The subset is the one docs/wiki/STYLE.md allows: # to ### headings, paragraphs, **bold**, *italic*,
// `code`, fenced code blocks, bullet and numbered lists (one level of nesting), pipe tables, the Note / Tip /
// Warning boxes and the "Related articles" line, [[Page#Section|label]] and [text](https://...) links, images
// (![alt](images/<page>/<name>.png or .gif) on a line of its own, an optional *caption* line under it), example links
// ([text](example:<file>.vat), a project in examples/) and a closing "Category: ..." line. No UI here: the app (ImGui) and the viewer (LLUI) draw the same blocks.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vats::wiki {

struct Span {
    std::string text;
    bool bold = false, italic = false, code = false;
    // Links: a wiki page (target = page title, anchor = heading, may be empty) or an external URL.
    // Example links: target = the project's file name in <help>/examples/.
    bool link = false, external = false, example = false;
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
    // Categories in the fixed order of the style guide, then any others alphabetically; pages by title.
    std::vector<std::pair<std::string, std::vector<const Page*>>> contents() const;
    // Every word of the query must appear; title matches first, then headings, then body.
    std::vector<Hit> search(std::string_view query, size_t max_hits = 30) const;

private:
    std::vector<Page> pages_;
};

}  // namespace vats::wiki
