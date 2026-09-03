/**
 * @file ssWindJacobiC.glsl
 * @brief Atmo Magic wind flowmap: one Jacobi relaxation of the pressure
 *        Poisson equation, over the whole 3D domain in a single dispatch.
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

// A partial rebuild relaxes pressure only inside its box, warm-started from the
// previous field. The dispatch walks the box plus a one-cell ring: the ring
// copies its value through each pass instead of relaxing, so the alternating
// read/write pressure buffers both stay valid at the box's edge.
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

layout(r8,   binding = 1) uniform readonly  image3D uSolid;
layout(r32f, binding = 3) uniform readonly  image3D uDiv;
layout(r32f, binding = 4) uniform readonly  image3D uPIn;
layout(r32f, binding = 5) uniform writeonly image3D uPOut;

// Boundary conditions: horizontal edges and the top slab are open - pressure is ambient (zero), air may enter or leave freely; the ground below the lowest slab is a wall, so pressure
// mirrors (Neumann) and nothing flows through it; a solid neighbour mirrors this cell's pressure, for the same reason - what makes a building a building. Returning the pressure
// stored in a solid cell holds the wall at zero instead, an open drain: air pours into the geometry, no pressure builds against a windward face, and the field barely deflects no matter how
// fine the grid or how many passes. Mirroring puts a zero gradient across the face, so nothing crosses it and the air has to go around - where the acceleration through a gap comes
// from.
float loadP(ivec3 c, float here)
{
    if (c.x < 0 || c.y < 0 || c.x >= uRes || c.y >= uRes) return 0.0;
    if (c.z >= uSlices) return 0.0;
    if (c.z < 0) return here;

    // Occupancy is fractional, so blend rather than switch: a half-filled cell is half a wall
    float solid = imageLoad(uSolid, c).r;
    return mix(imageLoad(uPIn, c).r, here, solid);
}

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;

    float here = imageLoad(uPIn, c).r;

    // Copy the ring around the box through unchanged instead of relaxing it.
    // The box's edge then meets the previous field - its genuine boundary - and
    // the untouched region never decays into the undefined half-solved state a
    // single-buffer box would leave behind.
    if (!inBox(c))
    {
        imageStore(uPOut, c, vec4(here, 0.0, 0.0, 0.0));
        return;
    }

    // Inside a solid there is no fluid to solve for. Its stored pressure is never read as-is - loadP mirrors across the face - so this only has to leave the cell alone.
    float solid = imageLoad(uSolid, c).r;
    if (solid > 0.99)
    {
        imageStore(uPOut, c, vec4(here, 0.0, 0.0, 0.0));
        return;
    }

    float d = cellSize();
    float d2 = d * d;
    float du = sliceGapUp(c.z);
    float dd = sliceGapDown(c.z);

    float pR = loadP(c + ivec3(1, 0, 0), here);
    float pL = loadP(c - ivec3(1, 0, 0), here);
    float pF = loadP(c + ivec3(0, 1, 0), here);
    float pB = loadP(c - ivec3(0, 1, 0), here);
    float pU = loadP(c + ivec3(0, 0, 1), here);
    float pD = loadP(c - ivec3(0, 0, 1), here);

    float div = imageLoad(uDiv, c).r;

    // Non-uniform vertical spacing: the second derivative across slabs of different thickness weights each neighbour by its own gap, and the centre coefficient falls out as 2/(du*dd).
    float num = (pR + pL) / d2
              + (pF + pB) / d2
              + 2.0 / (du + dd) * (pU / du + pD / dd)
              - div;

    float den = 2.0 / d2 + 2.0 / d2 + 2.0 / (du * dd);

    imageStore(uPOut, c, vec4(num / den, 0.0, 0.0, 0.0));
}

