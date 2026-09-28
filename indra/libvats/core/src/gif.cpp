// Viewport Avatar Toolset - the animated GIF writer (see vats/gif.h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/gif.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace vats {

namespace {

void put16(std::vector<std::uint8_t>& o, int v) {
    o.push_back(std::uint8_t(v & 0xFF));
    o.push_back(std::uint8_t((v >> 8) & 0xFF));
}

// A 15-bit colour (5 bits a channel) and how many pixels have it, with their full colours summed for the mean.
struct Bin {
    std::uint16_t key;
    std::uint32_t count;
    std::uint64_t sum[3];
};

int channel(std::uint16_t key, int c) { return (key >> (10 - 5 * c)) & 31; }

// Median cut: the box with the widest channel (weighted by its pixels) is split at its median until there are 256
// boxes or none can split. Returns the palette (mean colours) and each bin's palette index in lut (by key).
std::vector<std::array<std::uint8_t, 3>> median_cut(std::vector<Bin>& bins, std::vector<std::uint8_t>& lut) {
    struct Box {
        size_t begin, end;
        int ch = 0;          // its widest channel
        double score = 0;    // that channel's range times the square root of its pixels; 0 = cannot split
    };
    auto make = [&](size_t begin, size_t end) {
        Box b{begin, end};
        int best = 0;
        std::uint64_t n = 0;
        for (int c = 0; c < 3; ++c) {
            int lo = 31, hi = 0;
            for (size_t i = begin; i < end; ++i) lo = std::min(lo, channel(bins[i].key, c)), hi = std::max(hi, channel(bins[i].key, c));
            if (hi - lo > best) best = hi - lo, b.ch = c;
        }
        for (size_t i = begin; i < end; ++i) n += bins[i].count;
        b.score = double(best) * std::sqrt(double(n));
        return b;
    };
    std::vector<Box> boxes{make(0, bins.size())};
    while (boxes.size() < 256) {
        int pick = -1;
        for (int k = 0; k < int(boxes.size()); ++k)
            if (boxes[k].score > 0 && (pick < 0 || boxes[k].score > boxes[pick].score)) pick = k;
        if (pick < 0) break;
        const Box b = boxes[pick];
        std::sort(bins.begin() + long(b.begin), bins.begin() + long(b.end),
                  [&](const Bin& x, const Bin& y) { return channel(x.key, b.ch) < channel(y.key, b.ch); });
        std::uint64_t total = 0, run = 0;
        for (size_t i = b.begin; i < b.end; ++i) total += bins[i].count;
        size_t mid = b.begin + 1;
        for (size_t i = b.begin; i + 1 < b.end; ++i) {
            run += bins[i].count;
            mid = i + 1;
            if (run * 2 >= total) break;
        }
        boxes[pick] = make(b.begin, mid);
        boxes.push_back(make(mid, b.end));
    }
    std::vector<std::array<std::uint8_t, 3>> palette;
    for (const Box& b : boxes) {
        std::uint64_t n = 0, s[3] = {};
        for (size_t i = b.begin; i < b.end; ++i) {
            n += bins[i].count;
            for (int c = 0; c < 3; ++c) s[c] += bins[i].sum[c];
            lut[bins[i].key] = std::uint8_t(palette.size());
        }
        palette.push_back({std::uint8_t(s[0] / n), std::uint8_t(s[1] / n), std::uint8_t(s[2] / n)});
    }
    return palette;
}

// Codes of up to 12 bits, packed low bit first into sub-blocks of at most 255 bytes.
struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::vector<std::uint8_t> block;
    std::uint32_t acc = 0;
    int bits = 0;
    void code(int c, int size) {
        acc |= std::uint32_t(c) << bits;
        bits += size;
        while (bits >= 8) byte(std::uint8_t(acc & 0xFF)), acc >>= 8, bits -= 8;
    }
    void byte(std::uint8_t b) {
        block.push_back(b);
        if (block.size() == 255) flush();
    }
    void flush() {
        if (block.empty()) return;
        out.push_back(std::uint8_t(block.size()));
        out.insert(out.end(), block.begin(), block.end());
        block.clear();
    }
    void finish() {
        if (bits > 0) byte(std::uint8_t(acc & 0xFF));
        flush();
        out.push_back(0);  // block terminator
    }
};

// LZW with a minimum code size of 8: clear 256, end 257; the table resets (a clear code) when it is full.
void lzw(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& px) {
    constexpr int kClear = 256, kEnd = 257;
    out.push_back(8);
    BitWriter w{out, {}};
    std::unordered_map<std::uint32_t, int> dict;
    dict.reserve(8192);
    int size = 9, next = kEnd + 1;
    w.code(kClear, size);
    int prefix = px[0];
    for (size_t i = 1; i < px.size(); ++i) {
        const std::uint32_t key = std::uint32_t(prefix) << 8 | px[i];
        if (auto it = dict.find(key); it != dict.end()) {
            prefix = it->second;
            continue;
        }
        w.code(prefix, size);
        dict.emplace(key, next);
        if (next >= (1 << size)) ++size;  // the decoder, one entry behind, widens on reading the next code
        if (++next == 4096) {
            w.code(kClear, size);
            dict.clear();
            size = 9, next = kEnd + 1;
        }
        prefix = px[i];
    }
    w.code(prefix, size);
    w.code(kEnd, size);
    w.finish();
}

}  // namespace

GifWriter::GifWriter(int width, int height) : w_(std::max(width, 1)), h_(std::max(height, 1)) {
    const char* sig = "GIF89a";
    out_.insert(out_.end(), sig, sig + 6);
    put16(out_, w_);
    put16(out_, h_);
    out_.push_back(0);  // no global colour table
    out_.push_back(0);  // background colour index
    out_.push_back(0);  // pixel aspect ratio: unspecified
    const std::uint8_t loop[] = {0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0};
    out_.insert(out_.end(), std::begin(loop), std::end(loop));  // loop forever
}

void GifWriter::add_frame(const std::uint8_t* rgba, int delay_cs) {
    const size_t n = size_t(w_) * h_;
    std::vector<Bin> bins;
    {
        std::vector<std::int32_t> slot(32768, -1);
        for (size_t i = 0; i < n; ++i) {
            const std::uint8_t* p = rgba + i * 4;
            const std::uint16_t key = std::uint16_t((p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3));
            if (slot[key] < 0) slot[key] = std::int32_t(bins.size()), bins.push_back({key, 0, {}});
            Bin& b = bins[size_t(slot[key])];
            ++b.count;
            for (int c = 0; c < 3; ++c) b.sum[c] += p[c];
        }
    }
    std::vector<std::uint8_t> lut(32768, 0);
    const auto palette = median_cut(bins, lut);
    int bits = 1;  // the table holds 2^bits entries, at least 2
    while ((1 << bits) < int(palette.size())) ++bits;

    const std::uint8_t gce[] = {0x21, 0xF9, 4, 0, std::uint8_t(delay_cs & 0xFF), std::uint8_t((delay_cs >> 8) & 0xFF), 0, 0};
    out_.insert(out_.end(), std::begin(gce), std::end(gce));
    out_.push_back(0x2C);  // image descriptor: the whole screen, a local colour table
    put16(out_, 0), put16(out_, 0), put16(out_, w_), put16(out_, h_);
    out_.push_back(std::uint8_t(0x80 | (bits - 1)));
    for (int i = 0; i < (1 << bits); ++i)
        for (int c = 0; c < 3; ++c) out_.push_back(i < int(palette.size()) ? palette[size_t(i)][size_t(c)] : 0);
    std::vector<std::uint8_t> px(n);
    for (size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = rgba + i * 4;
        px[i] = lut[(p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3)];
    }
    lzw(out_, px);
    ++frames_;
}

std::vector<std::uint8_t> GifWriter::finish() {
    out_.push_back(0x3B);
    frames_ = 0;
    return std::move(out_);
}

int gif_delay_cs(int i, double fps) {
    fps = std::max(fps, 0.01);
    return int(std::lround((i + 1) * 100.0 / fps) - std::lround(i * 100.0 / fps));
}

}  // namespace vats
