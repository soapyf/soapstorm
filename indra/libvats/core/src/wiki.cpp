// Viewport Avatar Toolset - the shipped help wiki. See vats/wiki.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/wiki.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>

namespace vats::wiki {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string spans_text(const std::vector<Span>& spans) {
    std::string out;
    for (const Span& s : spans) out += s.text;
    return out;
}

// Cells of one "| a | b |" table row. The | of a [[Page|label]] link stays in its cell.
std::vector<std::string> cells(std::string_view line) {
    std::string t = trim(line);
    if (!t.empty() && t.front() == '|') t.erase(0, 1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    std::vector<std::string> out;
    std::string cur;
    bool in_link = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (starts(std::string_view(t).substr(i), "[[")) in_link = true;
        else if (starts(std::string_view(t).substr(i), "]]")) in_link = false;
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') cur += '|', ++i;
        else if (t[i] == '|' && !in_link) out.push_back(trim(cur)), cur.clear();
        else cur += t[i];
    }
    out.push_back(trim(cur));
    return out;
}

bool separator_row(std::string_view line) {
    for (char c : line)
        if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
    return line.find('-') != std::string_view::npos;
}

}  // namespace

bool png_size(const std::string& path, int& width, int& height) {
    unsigned char h[24] = {};
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);  // UTF-8
    if (!f.read(reinterpret_cast<char*>(h), sizeof h)) return false;
    static const unsigned char sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    if (!std::equal(sig, sig + 16, h)) return false;
    auto be = [&](int at) { return int(h[at]) << 24 | int(h[at + 1]) << 16 | int(h[at + 2]) << 8 | int(h[at + 3]); };
    width = be(16), height = be(20);
    return width > 0 && height > 0;
}

bool gif_size(const std::string& path, int& width, int& height) {
    unsigned char h[10] = {};
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(h), sizeof h)) return false;
    if (std::string_view(reinterpret_cast<const char*>(h), 6) != "GIF89a" &&
        std::string_view(reinterpret_cast<const char*>(h), 6) != "GIF87a")
        return false;
    width = h[6] | h[7] << 8, height = h[8] | h[9] << 8;
    return width > 0 && height > 0;
}

Layout lay_out(const std::vector<Span>& spans, float w, float pad, float button_pad, float gap,
               const std::function<float(const Span&, std::string_view)>& measure) {
    Layout out;
    float x = 0;
    int line = 0;
    float box = -1;  // the open code background's left edge on this line; < 0: none
    auto end_box = [&](float x1) {
        if (box >= 0) out.boxes.push_back({line, box, x1});
        box = -1;
    };
    for (size_t i = 0; i < spans.size(); ++i) {
        const Span& s = spans[i];
        const std::string_view t = s.text;
        if (s.example) {  // a button, never split across lines
            const float bw = measure(s, t) + button_pad * 2;
            if (x > 0) x += gap;
            if (x + bw > w && x > 0) x = 0, ++line;
            out.pieces.push_back({i, 0, t.size(), x, bw, line});
            x += bw, out.right = std::max(out.right, x);
            continue;
        }
        for (size_t p = 0; p < t.size();) {
            size_t q = p;
            while (q < t.size() && t[q] == ' ') ++q;
            while (q < t.size() && t[q] != ' ') ++q;
            if (x == 0)
                while (p < q && t[p] == ' ') ++p;
            // Room the word needs besides its text: code's pad before it on a fresh line, and always the pad after
            // it, so a line's code background can end wherever the line does.
            float extra = s.code ? (box < 0 ? pad : 0) + pad : 0;
            float ww = measure(s, t.substr(p, q - p));
            // A span's last word goes to the next line with the start of the spans after it when no space parts
            // them, so "`code`," or "(**Ctrl+Q**)" never leaves punctuation alone at either end of a line.
            float glue = 0;
            for (size_t j = i + 1; q == t.size() && t.back() != ' ' && j < spans.size() && !spans[j].example; ++j) {
                const Span& n = spans[j];
                const size_t g = std::min(n.text.find(' '), n.text.size());
                if (g == 0) break;
                glue += measure(n, std::string_view(n.text).substr(0, g)) + (n.code ? pad * 2 : 0);
                if (g < n.text.size()) break;
            }
            if (x + ww + extra + glue > w && x > 0) {  // wrap, dropping the spaces
                end_box(x + pad);
                x = 0, ++line;
                while (p < q && t[p] == ' ') ++p;
                extra = s.code ? pad * 2 : 0;
                ww = measure(s, t.substr(p, q - p));
            }
            if (x == 0 && ww + extra > w && q - p > 1) {  // wider than a line: break it
                size_t fit = p;
                for (size_t e = p + 1; e <= q; ++e) {
                    if (e < q && (static_cast<unsigned char>(t[e]) & 0xC0) == 0x80) continue;  // inside a UTF-8 character
                    if (measure(s, t.substr(p, e - p)) + extra > w) break;
                    fit = e;
                }
                if (fit == p) {  // not even one character: take one anyway
                    fit = p + 1;
                    while (fit < q && (static_cast<unsigned char>(t[fit]) & 0xC0) == 0x80) ++fit;
                }
                for (size_t e = fit; fit < q && e > p + 1; --e)
                    if (std::string_view("/\\_-.").find(t[e - 1]) != std::string_view::npos) {
                        fit = e;
                        break;
                    }
                q = fit;
                ww = measure(s, t.substr(p, q - p));
            }
            if (p < q) {
                if (s.code && box < 0) box = x, x += pad;
                out.pieces.push_back({i, p, q, x, ww, line});
                x += ww;
            }
            p = q;
        }
        if (box >= 0) x += pad, end_box(x);
        out.right = std::max(out.right, x);
    }
    out.lines = line + 1;
    return out;
}

std::vector<float> column_widths(const std::vector<float>& natural, const std::vector<float>& least, float avail) {
    float nat = 0, lo = 0;
    std::vector<float> out(natural.size());
    for (size_t c = 0; c < natural.size(); ++c) {
        out[c] = std::min(c < least.size() ? least[c] : 0.f, natural[c]);
        nat += natural[c], lo += out[c];
    }
    if (nat <= avail) return natural;
    if (lo >= avail) {  // columns with short words keep them; the rest share what is left alike and break words
        std::vector<size_t> by(out.size());
        std::iota(by.begin(), by.end(), size_t(0));
        std::sort(by.begin(), by.end(), [&](size_t a, size_t b) { return out[a] < out[b]; });
        float left = std::max(avail, 0.f);
        for (size_t k = 0; k < by.size(); ++k) {
            out[by[k]] = std::min(out[by[k]], left / float(by.size() - k));
            left -= out[by[k]];
        }
        return out;
    }
    for (size_t c = 0; c < out.size(); ++c) out[c] += (natural[c] - out[c]) * (avail - lo) / (nat - lo);
    return out;
}

std::string anchor_key(std::string_view heading) {
    std::string out;
    bool dash = false;
    for (char c : heading) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || u >= 0x80) {
            if (dash && !out.empty()) out += '-';
            out += char(std::tolower(u));
            dash = false;
        } else if (c == ' ' || c == '-' || c == '_') {
            dash = true;
        }
    }
    return out;
}

std::vector<Span> parse_inline(std::string_view text) {
    std::vector<Span> out;
    Span cur;
    auto flush = [&] {
        if (!cur.text.empty()) out.push_back(cur);
        cur.text.clear();
    };
    auto plain = [&] {
        Span s;
        s.bold = cur.bold, s.italic = cur.italic;
        return s;
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                flush();
                Span c = plain();
                c.code = true, c.text = std::string(text.substr(i + 1, end - i - 1));
                out.push_back(c);
                i = end + 1;
                continue;
            }
        } else if (starts(text.substr(i), "**")) {
            flush();
            cur.bold = !cur.bold;
            i += 2;
            continue;
        } else if (text[i] == '*') {
            // Opens only before a non-space, so "2 * 3" stays literal.
            const bool opens = !cur.italic && i + 1 < text.size() && text[i + 1] != ' ';
            if (opens || cur.italic) {
                flush();
                cur.italic = !cur.italic;
                ++i;
                continue;
            }
        } else if (starts(text.substr(i), "[[")) {
            size_t end = text.find("]]", i + 2);
            if (end != std::string_view::npos) {
                std::string inner(text.substr(i + 2, end - i - 2)), label;
                if (size_t bar = inner.find('|'); bar != std::string::npos) label = trim(inner.substr(bar + 1)), inner.resize(bar);
                Span l = plain();
                l.link = true;
                if (size_t hash = inner.find('#'); hash != std::string::npos) {
                    l.anchor = trim(inner.substr(hash + 1));
                    inner.resize(hash);
                }
                l.target = trim(inner);
                l.text = !label.empty() ? label : !l.target.empty() ? l.target : l.anchor;
                flush();
                out.push_back(l);
                i = end + 2;
                continue;
            }
        } else if (text[i] == '[') {
            size_t close = text.find(']', i + 1);
            if (close != std::string_view::npos && close + 1 < text.size() && text[close + 1] == '(') {
                size_t end = text.find(')', close + 2);
                if (end != std::string_view::npos) {
                    Span l = plain();
                    l.link = true;
                    l.target = std::string(text.substr(close + 2, end - close - 2));
                    l.external = l.target.find("://") != std::string::npos;
                    if (starts(l.target, "example:")) l.example = true, l.target.erase(0, 8);
                    l.text = std::string(text.substr(i + 1, close - i - 1));
                    flush();
                    out.push_back(l);
                    i = end + 1;
                    continue;
                }
            }
        }
        cur.text += text[i++];
    }
    flush();
    return out;
}

Page parse_page(std::string_view markdown, std::string file) {
    Page page;
    page.file = std::move(file);
    std::vector<std::string> lines;
    {
        std::string line;
        for (char c : markdown) {
            if (c == '\n') lines.push_back(line), line.clear();
            else if (c != '\r') line += c;
        }
        if (!line.empty()) lines.push_back(line);
    }

    // The block being gathered: its kind and raw text; closed by a blank line or a different line kind.
    Block open;
    std::string raw;
    bool is_open = false;
    auto close = [&] {
        if (!is_open) return;
        open.spans = parse_inline(trim(raw));
        page.blocks.push_back(open);
        open = Block{};
        raw.clear();
        is_open = false;
    };
    auto start = [&](Block::Kind kind, std::string text, int level = 0, int number = 0) {
        close();
        open.kind = kind, open.level = level, open.number = number;
        raw = std::move(text);
        is_open = true;
    };

    for (size_t n = 0; n < lines.size(); ++n) {
        const std::string& line = lines[n];
        const std::string t = trim(line);
        if (starts(t, "```")) {  // fenced code
            close();
            Block code;
            code.kind = Block::Code;
            for (++n; n < lines.size() && !starts(trim(lines[n]), "```"); ++n) code.code += lines[n] + "\n";
            if (!code.code.empty()) code.code.pop_back();
            page.blocks.push_back(code);
            continue;
        }
        if (t.empty()) {
            close();
            continue;
        }
        if (starts(t, "Category:")) {
            close();
            page.category = trim(t.substr(9));
            continue;
        }
        if (t[0] == '#') {
            size_t level = t.find_first_not_of('#');
            if (level != std::string::npos && level <= 3 && t[level] == ' ') {
                close();
                const std::string text = trim(t.substr(level));
                if (level == 1 && page.title.empty()) {
                    page.title = text;
                    continue;
                }
                Block h;
                h.kind = Block::Heading, h.level = int(level), h.spans = parse_inline(text);
                page.headings.push_back(spans_text(h.spans));
                page.blocks.push_back(h);
                continue;
            }
        }
        if (starts(t, "![") && t.back() == ')') {  // an image on its own line, then an optional *caption* line
            const size_t mid = t.find("](");
            if (mid != std::string::npos) {
                close();
                Block img;
                img.kind = Block::Image;
                img.alt = trim(std::string_view(t).substr(2, mid - 2));
                img.image = trim(std::string_view(t).substr(mid + 2, t.size() - mid - 3));
                if (n + 1 < lines.size()) {
                    const std::string c = trim(lines[n + 1]);
                    if (c.size() > 2 && c[0] == '*' && c[1] != '*' && c[1] != ' ' && c.back() == '*') img.spans = parse_inline(c), ++n;
                }
                page.blocks.push_back(img);
                continue;
            }
        }
        if (t[0] == '|') {  // table: this and the following "|" lines
            close();
            Block table;
            table.kind = Block::Table;
            for (; n < lines.size() && starts(trim(lines[n]), "|"); ++n) {
                if (separator_row(trim(lines[n]))) continue;
                std::vector<std::vector<Span>> row;
                for (const std::string& c : cells(lines[n])) row.push_back(parse_inline(c));
                table.rows.push_back(std::move(row));
            }
            --n;
            page.blocks.push_back(table);
            continue;
        }
        if (t[0] == '>') {
            std::string q = trim(t.substr(1));
            struct Box { const char* prefix; Block::Kind kind; };
            static const Box boxes[] = {{"**Note:**", Block::Note}, {"**Tip:**", Block::Tip},
                                        {"**Warning:**", Block::Warning}, {"Related articles:", Block::Related}};
            bool boxed = false;
            for (const Box& b : boxes)
                if (starts(q, b.prefix)) {
                    start(b.kind, trim(q.substr(std::string_view(b.prefix).size())));
                    boxed = true;
                    break;
                }
            if (!boxed) {
                const bool continues = is_open && (open.kind == Block::Note || open.kind == Block::Tip ||
                                                   open.kind == Block::Warning || open.kind == Block::Quote);
                if (continues) raw += " " + q;
                else start(Block::Quote, q);
            }
            continue;
        }
        const size_t indent = line.find_first_not_of(" \t");
        const int level = indent >= 2 ? 1 : 0;
        if (starts(t, "- ") || starts(t, "* ")) {
            start(Block::Bullet, t.substr(2), level);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(t[0]))) {
            size_t dot = t.find_first_not_of("0123456789");
            if (dot != std::string::npos && dot + 1 < t.size() && t[dot] == '.' && t[dot + 1] == ' ') {
                start(Block::Numbered, t.substr(dot + 2), level, std::stoi(t.substr(0, dot)));
                continue;
            }
        }
        if (is_open && open.kind != Block::Code) {
            raw += " " + t;  // wrapped paragraph, list item or box text
            continue;
        }
        start(Block::Paragraph, t);
    }
    close();

    std::string plain = page.title + "\n";
    for (const Block& b : page.blocks) {
        plain += b.alt + (b.alt.empty() ? "" : " ") + spans_text(b.spans) + b.code + "\n";
        for (const auto& row : b.rows)
            for (const auto& cell : row) plain += spans_text(cell) + " ";
    }
    page.plain = lower(plain);
    return page;
}

int Library::load_dir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int n = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        const std::string stem = p.stem().string();
        if (stem == "STYLE" || stem == "README") continue;
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        Page page = parse_page(ss.str(), stem);
        if (page.title.empty()) page.title = stem;
        add(std::move(page));
        ++n;
    }
    return n;
}

void Library::add(Page page) { pages_.push_back(std::move(page)); }

const Page* Library::find(std::string_view title_or_file) const {
    const std::string key = lower(trim(title_or_file));
    for (const Page& p : pages_)
        if (lower(p.title) == key || lower(p.file) == key) return &p;
    return nullptr;
}

std::vector<std::pair<std::string, std::vector<const Page*>>> Library::contents() const {
    static const char* const order[] = {"Getting started", "Interface",     "Animating", "Motion",
                                        "Import and export", "Second Life", "Viewer",    "Reference",
                                        "Troubleshooting"};
    std::map<std::string, std::vector<const Page*>> by;
    for (const Page& p : pages_) by[p.category.empty() ? "Other" : p.category].push_back(&p);
    for (auto& [cat, list] : by)
        std::sort(list.begin(), list.end(), [](const Page* a, const Page* b) { return lower(a->title) < lower(b->title); });
    std::vector<std::pair<std::string, std::vector<const Page*>>> out;
    for (const char* c : order)
        if (auto it = by.find(c); it != by.end()) out.emplace_back(it->first, it->second), by.erase(it);
    for (auto& [cat, list] : by) out.emplace_back(cat, list);  // anything else, alphabetically ("Other" too)
    return out;
}

std::vector<Hit> Library::search(std::string_view query, size_t max_hits) const {
    std::vector<std::string> words;
    {
        std::istringstream in(lower(query));
        for (std::string w; in >> w;) words.push_back(w);
    }
    std::vector<Hit> hits;
    if (words.empty()) return hits;
    for (const Page& p : pages_) {
        if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) { return p.plain.find(w) != std::string::npos; }))
            continue;
        Hit h;
        h.page = &p;
        const std::string title = lower(p.title);
        for (const std::string& w : words) {
            if (title.find(w) != std::string::npos) h.score += 100;
            for (const std::string& head : p.headings)
                if (lower(head).find(w) != std::string::npos) h.score += 30;
            size_t count = 0;
            for (size_t at = p.plain.find(w); at != std::string::npos && count < 10; at = p.plain.find(w, at + w.size())) ++count;
            h.score += int(count);
        }
        // The first body block holding the first word gives the heading and snippet.
        std::string heading;
        for (const Block& b : p.blocks) {
            if (b.kind == Block::Heading) {
                heading = spans_text(b.spans);
                continue;
            }
            std::string text = spans_text(b.spans) + b.code;
            for (const auto& row : b.rows)
                for (const auto& cell : row) text += spans_text(cell) + " ";
            const size_t at = lower(text).find(words[0]);
            if (at == std::string::npos) continue;
            h.heading = heading;
            // Cut on UTF-8 character boundaries so a snippet never splits a character.
            auto cont = [&](size_t i) { return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80; };
            size_t from = at > 60 ? at - 60 : 0, to = std::min(text.size(), from + 140);
            while (from > 0 && cont(from)) --from;
            while (cont(to)) ++to;
            h.snippet = (from ? "…" : "") + text.substr(from, to - from) + (to < text.size() ? "…" : "");
            break;
        }
        hits.push_back(std::move(h));
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > max_hits) hits.resize(max_hits);
    return hits;
}

}  // namespace vats::wiki
