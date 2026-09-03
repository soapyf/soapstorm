/**
 * @file ssWindDivC.glsl
 * @brief Atmo Magic wind flowmap: divergence of the seeded velocity field.
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

// ---------------------------------------------------------------------------
// Shared domain description. A camera-centred box snapped to a texel grid so it translates in whole cells rather than jittering every frame. Horizontally a uniform grid: uRes
// texels across uExtent metres. Vertically uSlices adaptive slabs whose boundaries live in uSliceZ, placed where the captured height field has detail. That makes the z spacing
// non-uniform, so every vertical difference is weighted by the real slab thickness rather than a constant step. Declared in full in each pass: separately compiled GLSL units do not share
// uniform declarations, so there is nothing to gain from a common file.
// ---------------------------------------------------------------------------

uniform int   uRes;             // texels per horizontal axis
uniform int   uSlices;          // active slabs, 2..16
uniform float uExtent;          // domain width in metres
uniform float uSliceZ[17];      // uSlices+1 boundary altitudes, ascending
uniform vec3  uAmbient[16];      // ambient wind per slab; sky tracks differ

// A partial rebuild prints divergence only inside its solve box; the preserved
// field outside it is ~divergence-free by construction and read as-is.
uniform ivec2 uBoxMin;
uniform ivec2 uBoxMax;

bool inBox(ivec3 c)
{
    return c.x >= uBoxMin.x && c.y >= uBoxMin.y
        && c.x <= uBoxMax.x && c.y <= uBoxMax.y;
}

float cellSize() { return uExtent / float(uRes); }

float sliceCentre(int k) { return 0.5 * (uSliceZ[k] + uSliceZ[k + 1]); }

float sliceThickness(int k) { return max(uSliceZ[k + 1] - uSliceZ[k], 0.01); }

float sliceGapUp(int k)
{
    return (k + 1 < uSlices) ? max(sliceCentre(k + 1) - sliceCentre(k), 0.01)
                             : sliceThickness(k);
}

float sliceGapDown(int k)
{
    return (k > 0) ? max(sliceCentre(k) - sliceCentre(k - 1), 0.01)
                   : sliceThickness(k);
}

bool inBounds(ivec3 c)
{
    return c.x >= 0 && c.y >= 0 && c.z >= 0
        && c.x < uRes && c.y < uRes && c.z < uSlices;
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, binding = 2) uniform readonly  image3D uVel;
layout(r32f,    binding = 3) uniform writeonly image3D uDiv;

// Outside the domain the air is undisturbed, so reads clamp to the nearest slab's ambient wind rather than zero - clamping to zero would make the domain edge a wall and stall the
// whole field.
vec3 loadVel(ivec3 c)
{
    int k = clamp(c.z, 0, uSlices - 1);
    if (c.x < 0 || c.y < 0 || c.x >= uRes || c.y >= uRes)
    {
        return uAmbient[k];
    }
    return imageLoad(uVel, ivec3(c.xy, k)).xyz;
}

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;
    if (!inBox(c)) return;

    float d = cellSize();
    float du = sliceGapUp(c.z);
    float dd = sliceGapDown(c.z);

    vec3 xp = loadVel(c + ivec3(1, 0, 0));
    vec3 xm = loadVel(c - ivec3(1, 0, 0));
    vec3 yp = loadVel(c + ivec3(0, 1, 0));
    vec3 ym = loadVel(c - ivec3(0, 1, 0));
    vec3 zp = loadVel(c + ivec3(0, 0, 1));
    vec3 zm = loadVel(c - ivec3(0, 0, 1));

    float div = (xp.x - xm.x) / (2.0 * d)
              + (yp.y - ym.y) / (2.0 * d)
              + (zp.z - zm.z) / (du + dd);

    imageStore(uDiv, c, vec4(div, 0.0, 0.0, 0.0));
}

