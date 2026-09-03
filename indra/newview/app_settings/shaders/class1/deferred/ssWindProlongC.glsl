/**
 * @file ssWindProlongC.glsl
 * @brief Atmo Magic wind flowmap: carry a solved coarse pressure field up to
 *        the next finer level as its starting guess.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// <SS:Nexii> Atmo Magic wind flowmap

uniform int uRes;               // texels per horizontal axis at this level
uniform int uSlices;            // active slabs, 2..16

bool inBounds(ivec3 c)
{
    return c.x >= 0 && c.y >= 0 && c.z >= 0
        && c.x < uRes && c.y < uRes && c.z < uSlices;
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// uRes is the fine resolution here; the coarse grid is half of it. Bilinear, not nearest: a blocky starting guess carries steps the fine level then spends its whole iteration budget
// smoothing out - the budget the pyramid exists to save.
layout(r32f, binding = 6) uniform readonly  image3D uCoarse;
layout(r32f, binding = 7) uniform writeonly image3D uFine;

float loadCoarse(ivec2 p, int k)
{
    int hi = uRes / 2 - 1;
    return imageLoad(uCoarse, ivec3(clamp(p, ivec2(0), ivec2(hi)), k)).r;
}

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;

    // Cell centres: fine centre (x + 0.5) sits at coarse coordinate (x + 0.5) / 2 - 0.5 measured in coarse cell centres.
    vec2 f = (vec2(c.xy) + 0.5) * 0.5 - 0.5;
    ivec2 base = ivec2(floor(f));
    vec2 t = f - vec2(base);

    float a = loadCoarse(base,                  c.z);
    float b = loadCoarse(base + ivec2(1, 0),    c.z);
    float d = loadCoarse(base + ivec2(0, 1),    c.z);
    float e = loadCoarse(base + ivec2(1, 1),    c.z);

    float p = mix(mix(a, b, t.x), mix(d, e, t.x), t.y);
    imageStore(uFine, c, vec4(p, 0.0, 0.0, 0.0));
}

