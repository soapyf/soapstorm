// Viewport Avatar Toolset - a small JSON reader and writer for project files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Strict RFC 8259 parsing (plus a skipped UTF-8 BOM). Objects keep their key order and reject
// duplicate keys. Strings are UTF-8 bytes, passed through unchecked; \u escapes become UTF-8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// X11's Xlib.h does "#define Bool int"; the SL viewer's precompiled header includes it before this file.
#pragma push_macro("Bool")
#undef Bool

namespace vats {

struct Json {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    Array arr;
    Object obj;

    Json() = default;
    Json(bool v) : type(Type::Bool), b(v) {}
    Json(int v) : type(Type::Number), num(v) {}
    Json(double v) : type(Type::Number), num(v) {}
    Json(const char* v) : type(Type::String), str(v) {}
    Json(std::string v) : type(Type::String), str(std::move(v)) {}
    static Json array() { Json j; j.type = Type::Array; return j; }
    static Json object() { Json j; j.type = Type::Object; return j; }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Object member lookup; null when absent or when this is not an object.
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    // Replaces the member's value, or appends the member. Turns a null into an object.
    Json& set(std::string_view key, Json v);
    bool erase(std::string_view key);
    // Appends to an array. Turns a null into an array.
    Json& push(Json v);

    bool operator==(const Json&) const = default;
};

inline constexpr int kJsonMaxDepth = 256;

// Parses one JSON value filling the whole text. On failure returns false and sets err
// ("... at byte N") and, when given, offset.
bool parse_json(std::string_view text, Json& out, std::string& err, size_t* offset = nullptr);

// Tab-indented, LF line ends, a final LF. Arrays holding no array or object stay on one line.
// Numbers use the shortest text that reads back to the same double (integers have no ".0").
std::string write_json(const Json& v);

}  // namespace vats

#pragma pop_macro("Bool")
