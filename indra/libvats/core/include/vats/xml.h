// Viewport Avatar Toolset - a small XML reader for the Linden character files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Handles elements, attributes, comments, processing instructions, CDATA and the five predefined
// entities. An element's text is its character data with entities decoded; runs that are only
// whitespace (indentation between child elements) are dropped.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vats {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;

    const std::string* attr(std::string_view key) const {
        for (auto& a : attrs)
            if (a.first == key) return &a.second;
        return nullptr;
    }
    std::string attr_or(std::string_view key, std::string_view fallback = {}) const {
        auto* a = attr(key);
        return a ? *a : std::string(fallback);
    }
    const XmlNode* child(std::string_view n) const {
        for (auto& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
};

// Parses a document and returns its root element. On failure returns false and sets err.
bool parse_xml(std::string_view text, XmlNode& root, std::string& err);

}  // namespace vats
