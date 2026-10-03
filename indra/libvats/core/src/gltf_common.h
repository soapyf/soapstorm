// Viewport Avatar Toolset - glTF 2.0 / GLB container and accessor reading, shared by the animation and mesh
// readers (core-private).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "guard.h"
#include "vats/json.h"

namespace vats::gltf {

inline std::vector<std::uint8_t> base64(std::string_view s) {
    std::vector<std::uint8_t> out;
    int val = 0, bits = -8;
    for (char c : s) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
        if (d < 0) continue;
        val = (val << 6) | d;
        if ((bits += 6) >= 0) {
            out.push_back(std::uint8_t((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

inline double num(const Json* j, double fallback = 0) { return j && j->is_number() ? j->num : fallback; }
// An index or count from the file; out of range (or not a number) gives the fallback.
inline int inum(const Json* j, int fallback = -1) {
    double v = num(j, fallback);
    return std::isfinite(v) && v >= -1e9 && v <= 1e9 ? int(v) : fallback;
}

struct Reader {
    const Json& doc;
    std::vector<std::vector<std::uint8_t>> buffers;
    std::string err;
    double budget = kMaxSamples * 8;  // floats all accessors may read together (256 MB)

    // Accessor as floats, `width` per element (normalised integers scaled to -1..1 / 0..1). A sparse accessor's
    // values replace the elements it lists (morph targets are usually stored so).
    bool floats(int index, std::vector<float>& out, int& width) {
        const Json* acc = doc.find("accessors");
        if (!acc || index < 0 || index >= int(acc->arr.size())) return err = "bad accessor", false;
        const Json& a = acc->arr[index];
        static const std::pair<const char*, int> types[] = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
        width = 0;
        if (auto* t = a.find("type"); t && t->is_string())
            for (auto& [n, w] : types)
                if (t->str == n) width = w;
        const int ct = inum(a.find("componentType"), 0), count = inum(a.find("count"), -1);
        const bool normalized = a.find("normalized") && a.find("normalized")->b;
        if (!width || !component_size(ct) || count < 0 || double(count) * width > budget) return err = "unsupported accessor", false;
        budget -= double(count) * width;
        out.assign(size_t(count) * width, 0.f);  // no bufferView: all zeros
        if (const Json* bv = a.find("bufferView"); bv && !read_view(inum(bv), num(a.find("byteOffset")), ct, width, count, normalized, true, out.data()))
            return false;
        const Json* sp = a.find("sparse");
        if (!sp) return true;
        const Json *si = sp->find("indices"), *sv = sp->find("values");
        const int n = inum(sp->find("count"), -1), ict = si ? inum(si->find("componentType"), 0) : 0;
        if (!si || !sv || n < 0 || n > count || (ict != 5121 && ict != 5123 && ict != 5125)) return err = "bad sparse accessor", false;
        std::vector<float> idx(static_cast<size_t>(n)), val(static_cast<size_t>(n) * width);
        if (!read_view(inum(si->find("bufferView")), num(si->find("byteOffset")), ict, 1, n, false, false, idx.data()) ||
            !read_view(inum(sv->find("bufferView")), num(sv->find("byteOffset")), ct, width, n, normalized, false, val.data()))
            return false;
        for (int e = 0; e < n; ++e)
            if (idx[size_t(e)] >= 0 && idx[size_t(e)] < float(count))
                std::copy_n(val.begin() + size_t(e) * width, width, out.begin() + size_t(idx[size_t(e)]) * width);
        return true;
    }

    static int component_size(int ct) { return ct == 5126 || ct == 5125 ? 4 : ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 0; }

    // count elements of width components of type ct from a bufferView, at acc_off into it, into out. strided: the
    // view's byteStride applies (sparse indices and values are always tightly packed).
    bool read_view(int vi, double acc_off, int ct, int width, int count, bool normalized, bool strided, float* out) {
        const Json* views = doc.find("bufferViews");
        if (!views || vi < 0 || vi >= int(views->arr.size())) return err = "bad bufferView", false;
        const Json& v = views->arr[vi];
        int bi = inum(v.find("buffer"));
        if (bi < 0 || bi >= int(buffers.size())) return err = "bad buffer", false;
        const auto& buf = buffers[bi];
        const int size = component_size(ct);
        // Offsets and stride are checked as doubles before any size_t arithmetic, so nothing wraps.
        const size_t elem = size_t(size) * width;
        const double view_off = num(v.find("byteOffset"));
        const double off = view_off + acc_off, st = strided ? num(v.find("byteStride"), double(elem)) : double(elem);
        const double n = double(buf.size());
        if (!std::isfinite(off) || !std::isfinite(st) || view_off < 0 || acc_off < 0 || off > n || st < double(elem) || st > n)
            return err = "accessor runs past its buffer", false;
        const size_t base = size_t(off), stride = size_t(st);
        if (count > 0 && (base + elem > buf.size() || size_t(count - 1) > (buf.size() - base - elem) / stride))
            return err = "accessor runs past its buffer", false;
        for (int e = 0; e < count; ++e)
            for (int c = 0; c < width; ++c) {
                size_t at = base + size_t(e) * stride + size_t(c) * size;
                const std::uint8_t* p = buf.data() + at;
                double x = 0;
                switch (ct) {
                    case 5126: { float f; std::memcpy(&f, p, 4); x = f; break; }
                    case 5125: { std::uint32_t u; std::memcpy(&u, p, 4); x = u; break; }
                    case 5120: x = normalized ? std::max(std::int8_t(*p) / 127.0, -1.0) : std::int8_t(*p); break;
                    case 5121: x = normalized ? *p / 255.0 : *p; break;
                    case 5122: { std::int16_t s; std::memcpy(&s, p, 2); x = normalized ? std::max(s / 32767.0, -1.0) : s; break; }
                    case 5123: { std::uint16_t s; std::memcpy(&s, p, 2); x = normalized ? s / 65535.0 : s; break; }
                }
                out[size_t(e) * width + c] = float(x);
            }
        return true;
    }
};

// Splits a GLB container (or takes a .gltf text), parses the JSON into doc and loads every buffer (the GLB's binary
// chunk, data: URIs, or a regular file beside the .gltf, never above it). Returns a Reader over doc.
inline bool open(const std::vector<std::uint8_t>& bytes, const std::string& dir, Json& doc,
                 std::vector<std::vector<std::uint8_t>>& buffers, std::string& err) {
    std::string_view json_text;
    std::vector<std::uint8_t> bin;
    auto u32 = [&](size_t at) { std::uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v; };
    if (bytes.size() >= 20 && std::memcmp(bytes.data(), "glTF", 4) == 0) {  // GLB container
        size_t at = 12;
        while (at + 8 <= bytes.size()) {
            std::uint32_t len = u32(at), type = u32(at + 4);
            if (at + 8 + len > bytes.size()) return err = "truncated GLB", false;
            if (type == 0x4E4F534A) json_text = std::string_view(reinterpret_cast<const char*>(bytes.data() + at + 8), len);
            if (type == 0x004E4942) bin.assign(bytes.begin() + at + 8, bytes.begin() + at + 8 + len);
            at += 8 + len;
        }
    } else {
        json_text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    if (!parse_json(json_text, doc, err)) return false;
    buffers.clear();
    if (auto* bufs = doc.find("buffers"))
        for (auto& b : bufs->arr) {
            const Json* uri = b.find("uri");
            if (!uri || !uri->is_string()) {
                buffers.push_back(bin);
            } else if (uri->str.rfind("data:", 0) == 0) {
                buffers.push_back(base64(std::string_view(uri->str).substr(uri->str.find(',') + 1)));
            } else {
                // Only a regular file beside the .gltf (or below it), and not a huge one.
                std::filesystem::path rel(uri->str);
                bool up = false;
                for (const auto& part : rel) up = up || part == "..";
                std::error_code ec;
                const std::filesystem::path file = std::filesystem::path(dir) / rel;
                if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory() || up ||
                    !std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > (1u << 30) || ec)
                    return err = "buffer file must be a file beside the .gltf: " + uri->str, false;
                std::ifstream f(file, std::ios::binary);
                if (!f) return err = "missing buffer file " + uri->str, false;
                buffers.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
        }
    return true;
}

}  // namespace vats::gltf
