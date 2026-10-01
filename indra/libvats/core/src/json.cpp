// Viewport Avatar Toolset - a small JSON reader and writer for project files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/json.h"

#include <charconv>
#include <cmath>
#include "from_chars_compat.h"
#include <system_error>

namespace vats {

const Json* Json::find(std::string_view key) const {
    for (auto& [k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}

Json* Json::find(std::string_view key) { return const_cast<Json*>(std::as_const(*this).find(key)); }

Json& Json::set(std::string_view key, Json v) {
    if (type == Type::Null) type = Type::Object;
    if (Json* m = find(key)) return *m = std::move(v);
    obj.emplace_back(std::string(key), std::move(v));
    return obj.back().second;
}

bool Json::erase(std::string_view key) {
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (it->first == key) {
            obj.erase(it);
            return true;
        }
    return false;
}

Json& Json::push(Json v) {
    if (type == Type::Null) type = Type::Array;
    arr.push_back(std::move(v));
    return arr.back();
}

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;

    bool fail(const char* what) {
        err = std::string(what) + " at byte " + std::to_string(i);
        return false;
    }
    bool eof() const { return i >= s.size(); }
    void skip_ws() {
        while (!eof() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool literal(std::string_view word) {
        if (s.substr(i, word.size()) != word) return fail("invalid literal");
        i += word.size();
        return true;
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool value(Json& out, int depth) {
        skip_ws();
        if (eof()) return fail("unexpected end of input");
        switch (s[i]) {
        case 'n': out = Json(); return literal("null");
        case 't': out = Json(true); return literal("true");
        case 'f': out = Json(false); return literal("false");
        case '"': out = Json(std::string()); return string(out.str);
        case '[': return array(out, depth);
        case '{': return object(out, depth);
        default: return number(out);
        }
    }

    bool number(Json& out) {
        size_t b = i;
        if (!eof() && s[i] == '-') ++i;
        if (eof() || !digit(s[i])) return fail("invalid value");
        if (s[i] == '0') ++i;
        else while (!eof() && digit(s[i])) ++i;
        if (!eof() && s[i] == '.') {
            ++i;
            if (eof() || !digit(s[i])) return fail("invalid number");
            while (!eof() && digit(s[i])) ++i;
        }
        if (!eof() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (!eof() && (s[i] == '+' || s[i] == '-')) ++i;
            if (eof() || !digit(s[i])) return fail("invalid number");
            while (!eof() && digit(s[i])) ++i;
        }
        double d = 0;
        auto r = from_chars(s.data() + b, s.data() + i, d);
        if (r.ec == std::errc::result_out_of_range && underflow(s.substr(b, i - b))) {
            d = s[b] == '-' ? -0.0 : 0.0;
        } else if (r.ec != std::errc() || !std::isfinite(d)) {
            i = b;
            return fail("number out of range");
        }
        out = Json(d);
        return true;
    }

    // Too small rather than too large: a negative exponent, or 0.xxx without one.
    // Some libraries (gcc 11) also report subnormals this way; those read as zero.
    static bool underflow(std::string_view num) {
        size_t e = num.find_first_of("eE");
        if (e != std::string_view::npos) return num[e + 1] == '-';
        return num.substr(num[0] == '-', 2) == "0.";
    }

    static int hex(char c) {
        return digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    }

    bool hex4(unsigned& u) {
        if (s.size() - i < 4) return fail("truncated \\u escape");
        u = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            int h = hex(c);
            if (h < 0) return --i, fail("invalid \\u escape");
            u = u * 16 + static_cast<unsigned>(h);
        }
        return true;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++i;  // opening quote
        for (;;) {
            if (eof()) return fail("unterminated string");
            char c = s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                ++i;
                continue;
            }
            if (++i >= s.size()) return fail("unterminated string");
            switch (s[i++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned u = 0;
                if (!hex4(u)) return false;
                if (u >= 0xDC00 && u <= 0xDFFF) return i -= 6, fail("unpaired low surrogate");
                if (u >= 0xD800 && u <= 0xDBFF) {
                    unsigned lo = 0;
                    if (s.substr(i, 2) != "\\u") return fail("unpaired high surrogate");
                    i += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return i -= 6, fail("unpaired high surrogate");
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, u);
                break;
            }
            default: return --i, fail("invalid escape");
            }
        }
    }

    bool array(Json& out, int depth) {
        if (depth >= kJsonMaxDepth) return fail("nesting too deep");
        out = Json::array();
        ++i;
        skip_ws();
        if (!eof() && s[i] == ']') return ++i, true;
        for (;;) {
            if (!value(out.push(Json()), depth + 1)) return false;
            skip_ws();
            if (eof()) return fail("unterminated array");
            if (s[i] == ']') return ++i, true;
            if (s[i] != ',') return fail("expected ',' or ']'");
            ++i;
        }
    }

    bool object(Json& out, int depth) {
        if (depth >= kJsonMaxDepth) return fail("nesting too deep");
        out = Json::object();
        ++i;
        skip_ws();
        if (!eof() && s[i] == '}') return ++i, true;
        for (;;) {
            skip_ws();
            if (eof() || s[i] != '"') return fail("expected a key");
            size_t at = i;
            std::string key;
            if (!string(key)) return false;
            // Linear duplicate check, O(n^2) per object: fine at project sizes.
            if (out.find(key)) return i = at, fail("duplicate key");
            skip_ws();
            if (eof() || s[i] != ':') return fail("expected ':'");
            ++i;
            out.obj.emplace_back(std::move(key), Json());
            if (!value(out.obj.back().second, depth + 1)) return false;
            skip_ws();
            if (eof()) return fail("unterminated object");
            if (s[i] == '}') return ++i, true;
            if (s[i] != ',') return fail("expected ',' or '}'");
            ++i;
        }
    }
};

void write_string(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write_value(std::string& out, const Json& v, int indent) {
    auto newline = [&](int n) {
        out += '\n';
        out.append(static_cast<size_t>(n), '\t');
    };
    switch (v.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += v.b ? "true" : "false"; break;
    case Json::Type::Number: {
        if (!std::isfinite(v.num)) {  // JSON has no NaN or infinity
            out += "null";
            break;
        }
        char buf[32];
        auto r = to_chars(buf, buf + sizeof buf, v.num);
        out.append(buf, r.ptr);
        break;
    }
    case Json::Type::String: write_string(out, v.str); break;
    case Json::Type::Array: {
        bool flat = true;
        for (auto& e : v.arr) flat = flat && !e.is_array() && !e.is_object();
        out += '[';
        for (size_t k = 0; k < v.arr.size(); ++k) {
            if (k) out += flat ? ", " : ",";
            if (!flat) newline(indent + 1);
            write_value(out, v.arr[k], indent + 1);
        }
        if (!flat && !v.arr.empty()) newline(indent);
        out += ']';
        break;
    }
    case Json::Type::Object:
        out += '{';
        for (size_t k = 0; k < v.obj.size(); ++k) {
            if (k) out += ',';
            newline(indent + 1);
            write_string(out, v.obj[k].first);
            out += ": ";
            write_value(out, v.obj[k].second, indent + 1);
        }
        if (!v.obj.empty()) newline(indent);
        out += '}';
        break;
    }
}

}  // namespace

bool parse_json(std::string_view text, Json& out, std::string& err, size_t* offset) {
    Parser p;
    p.s = text;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") p.i = 3;
    bool ok = p.value(out, 0);
    if (ok) {
        p.skip_ws();
        if (!p.eof()) ok = p.fail("trailing characters");
    }
    if (!ok) {
        err = p.err;
        if (offset) *offset = p.i;
    }
    return ok;
}

std::string write_json(const Json& v) {
    std::string out;
    write_value(out, v, 0);
    out += '\n';
    return out;
}

}  // namespace vats
