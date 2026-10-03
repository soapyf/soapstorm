// Viewport Avatar Toolset - what the UI hands a host to draw in 3D: lit, coloured triangles.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

namespace vats {

struct Vertex {
    float p[3];
    float n[3];
    float c[4];
};

struct Rgb {
    float r, g, b;
};

// A lighting preset (08 LT-1, the Light menu): a key light and an even fill, in the edited actor's space (+X where the
// avatar faces, +Y its left, Z up). The host lights its view with it (ui::Host::set_light).
struct LightPreset {
    const char* name;
    float key[3];  // towards the key light, unit length
    Rgb colour;    // the key light's colour and strength
    Rgb ambient;   // the fill from everywhere
    bool night;    // the viewer shows a night sky, the key being the moon
};

// Colours of the 3D scene; part of the UI theme.
struct SceneColours {
    Rgb backdrop_top{0.23f, 0.24f, 0.27f}, backdrop_bottom{0.09f, 0.09f, 0.11f};
    Rgb grid{0.62f, 0.65f, 0.72f};
    Rgb body{0.62f, 0.60f, 0.64f}, eye{0.92f, 0.91f, 0.88f};
    Rgb sky{0.52f, 0.56f, 0.66f}, ground{0.24f, 0.21f, 0.20f};  // hemisphere ambient
    Rgb target_ghost{0.52f, 0.88f, 0.62f};  // the target ghost: a soft green, apart from the onion ghosts
    Rgb shell{0.50f, 0.82f, 0.96f};         // collision-volume shells: a cool sky blue against the warm grey body
};

}  // namespace vats
