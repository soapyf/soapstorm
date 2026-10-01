// Viewport Avatar Toolset - an animated GIF writer, for listing media (spec 08 LM), and a reader
// for the help's GIFs.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// GIF89a that loops forever. Each frame carries its own palette of up to 256 colours (median cut over 5 bits a
// channel), no dithering, no transparency, and is LZW-compressed in full; written from the format's definition.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vats {

class GifWriter {
public:
    GifWriter(int width, int height);
    // rgba: width * height * 4 bytes, top row first; alpha is ignored. delay_cs: hundredths of a second.
    void add_frame(const std::uint8_t* rgba, int delay_cs);
    // The finished file; the writer is empty afterwards.
    std::vector<std::uint8_t> finish();
    int frames() const { return frames_; }

private:
    int w_, h_, frames_ = 0;
    std::vector<std::uint8_t> out_;
};

// How long frame i shows at fps, in hundredths of a second, so the running total stays on time (30 fps: 3, 3, 4, ...).
int gif_delay_cs(int i, double fps);

// A whole GIF read back (the vendored stb_image): every frame composited to width x height straight-alpha RGBA,
// top row first, and how long each shows as the file says (0 is common for "as fast as possible").
struct GifImage {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // frames() * width * height * 4 bytes
    std::vector<int> delays_ms;      // one per frame
    int frames() const { return int(delays_ms.size()); }
};
bool read_gif(const std::uint8_t* data, std::size_t size, GifImage& out);

// True when the bytes are a PNG in which every pixel is fully transparent: a thumbnail that drew nothing (the viewer's
// build 25 wrote them so), which the Inventory renders again. False for anything that does not decode.
bool png_blank(const std::uint8_t* data, std::size_t size);

}  // namespace vats
