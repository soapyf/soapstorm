// Viewport Avatar Toolset - an animated GIF writer, for listing media (spec 08 LM).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// GIF89a that loops forever. Each frame carries its own palette of up to 256 colours (median cut over 5 bits a
// channel), no dithering, no transparency, and is LZW-compressed in full; written from the format's definition.
#pragma once

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

}  // namespace vats
