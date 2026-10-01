// Viewport Avatar Toolset - a small XML reader for the Linden character files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/xml.h"

#include <algorithm>
#include <cctype>

namespace vats {
namespace {

struct Reader {
    std::string_view s;
    size_t i = 0;
    std::string err;

    bool eof() const { return i >= s.size(); }
    bool starts(std::string_view p) const { return s.substr(i, p.size()) == p; }
    void skip_ws() { while (!eof() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }

    bool fail(const char* what) {
        err = std::string(what) + " at byte " + std::to_string(i);
        return false;
    }

    // Skips comments, processing instructions and doctype until the next element tag. Character data
    // and CDATA are appended to text when it is given.
    bool skip_misc(std::string* text = nullptr) {
        while (!eof()) {
            if (starts("<![CDATA[")) {
                size_t e = s.find("]]>", i + 9);
                if (e == std::string_view::npos) return fail("unterminated CDATA");
                if (text) text->append(s.substr(i + 9, e - i - 9));
                i = e + 3;
            } else if (starts("<!--")) {
                size_t e = s.find("-->", i + 4);
                if (e == std::string_view::npos) return fail("unterminated comment");
                i = e + 3;
            } else if (starts("<?") || starts("<!")) {
                size_t e = s.find('>', i);
                if (e == std::string_view::npos) return fail("unterminated declaration");
                i = e + 1;
            } else if (s[i] == '<') {
                return true;
            } else {
                size_t e = std::min(s.find('<', i), s.size());
                std::string_view run = s.substr(i, e - i);
                if (text && run.find_first_not_of(" \t\r\n") != std::string_view::npos) *text += decode(run);
                i = e;
            }
        }
        return true;
    }

    std::string name() {
        size_t b = i;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-' ||
                          s[i] == ':' || s[i] == '.'))
            ++i;
        return std::string(s.substr(b, i - b));
    }

    static std::string decode(std::string_view v) {
        std::string out;
        out.reserve(v.size());
        for (size_t k = 0; k < v.size(); ++k) {
            if (v[k] == '&') {
                static constexpr std::pair<std::string_view, char> ents[] = {
                    {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
                bool hit = false;
                for (auto& [e, c] : ents)
                    if (v.substr(k, e.size()) == e) {
                        out += c;
                        k += e.size() - 1;
                        hit = true;
                        break;
                    }
                if (hit) continue;
            }
            out += v[k];
        }
        return out;
    }

    bool element(XmlNode& node, int depth) {
        if (depth > 256) return fail("nesting too deep");
        ++i;  // '<'
        node.name = name();
        if (node.name.empty()) return fail("bad element name");
        for (;;) {
            skip_ws();
            if (eof()) return fail("unterminated tag");
            if (starts("/>")) {
                i += 2;
                return true;
            }
            if (s[i] == '>') {
                ++i;
                break;
            }
            std::string key = name();
            if (key.empty()) return fail("bad attribute name");
            skip_ws();
            if (eof() || s[i] != '=') return fail("expected '='");
            ++i;
            skip_ws();
            if (eof() || (s[i] != '"' && s[i] != '\'')) return fail("expected quote");
            char q = s[i++];
            size_t e = s.find(q, i);
            if (e == std::string_view::npos) return fail("unterminated attribute");
            node.attrs.emplace_back(std::move(key), decode(s.substr(i, e - i)));
            i = e + 1;
        }
        for (;;) {
            if (!skip_misc(&node.text)) return false;
            if (eof()) return fail("missing closing tag");
            if (starts("</")) {
                i += 2;
                if (name() != node.name) return fail("mismatched closing tag");
                skip_ws();
                if (eof() || s[i] != '>') return fail("expected '>'");
                ++i;
                return true;
            }
            node.children.emplace_back();
            if (!element(node.children.back(), depth + 1)) return false;
        }
    }
};

}  // namespace

bool parse_xml(std::string_view text, XmlNode& root, std::string& err) {
    Reader r{text, 0, {}};
    root = {};
    if (!r.skip_misc() || r.eof()) {
        err = r.err.empty() ? "no root element" : r.err;
        return false;
    }
    if (!r.element(root, 0)) {
        err = r.err;
        return false;
    }
    return true;
}

}  // namespace vats
