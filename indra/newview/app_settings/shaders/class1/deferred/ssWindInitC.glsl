/**
 * @file ssWindInitC.glsl
 * @brief Atmo Magic wind flowmap: turn the captured height field into a solid
 *        mask per slab and seed the velocity field with the ambient wind.
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
uniform vec2  uOrigin;          // agent-space XY of texel (0,0)

// A partial rebuild re-derives the mask only where an edit landed. The box is
// those cells, inclusive; everything else keeps the field it already had, so
// this pass must not touch it.
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

// The tallest surface over each column. Everything below starts out solid - right for a building, wrong under a skyway - and the oblique probes below carve back whatever they can
// prove is open.
uniform float uSolidCurve;      // exponent on the fractional occupancy

layout(r32f,   binding = 0) uniform readonly  image2D uHeight;
layout(r8,     binding = 1) uniform writeonly image3D uSolid;
layout(rgba16f,binding = 2) uniform writeonly image3D uVel;

// ---------------------------------------------------------------------------
// Oblique probes. Four ortho depth captures, one per cardinal direction, each tilted down. A point nearer a probe than its first hit has an unobstructed line out of the world,
// so it is air - nothing between it and open sky along that ray. The test is one-sided in the direction that matters: it can only turn solid into air, on evidence, so a passage
// the probes fail to see keeps the conservative heightmap answer rather than becoming a false hole. A wall is never wrong. A probe ray stops at the first surface it meets,
// so a half-metre wall shadows everything behind it just as completely as a thick one, and a building interior stays filled.
// ---------------------------------------------------------------------------
// Unit 3 rather than a fresh one: GL only guarantees eight image units, so the passes have to share 0..7. Init never reads the divergence volume that lives at 3 elsewhere, and the
// divergence pass rebinds the unit before its own dispatch, so the two never overlap in time.
layout(r32f, binding = 3) uniform readonly image2DArray uProbe;

uniform mat4  uProbeView[4];    // world to probe view space
uniform int   uProbeCount;      // usable probes, packed to the front; 0 disables carving
uniform int   uProbeRes;        // probe texels per axis; finer than the mask, not equal to it
uniform float uProbeHalf[4];    // ortho half-width per probe; the rings differ
uniform float uProbeBias;       // slack against the probe's own surface

const float PROBE_MISS = 1.0e6; // ray left the world without hitting anything

bool visibleFrom(int i, vec3 world)
{
    vec3 v = (uProbeView[i] * vec4(world, 1.0)).xyz;

    // glm::lookAt looks down -Z, so distance along the ray is -v.z
    float dist = -v.z;
    if (dist < 0.0) return false;

    vec2 uv = v.xy / uProbeHalf[i] * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return false;   // outside this probe's footprint: it knows nothing
    }

    ivec2 t = clamp(ivec2(uv * float(uProbeRes)), ivec2(0), ivec2(uProbeRes - 1));
    float hit = imageLoad(uProbe, ivec3(t, i)).r;

    // A miss is absence of evidence, not evidence of absence. The ray left the captured volume without meeting anything, and that volume is bounded by a frustum and whatever happened to be
    // loaded - so a miss says nothing about this point and must not carve. Reading it as air is fatal because the probes are OR'd: most of each probe image is legitimately sky, so nearly every
    // cell in the region misses in at least one of the four directions, and one miss would be enough to open it - the mask dissolves and the wind sails through the buildings. Nothing is
    // lost by being strict. A passage is found from a ray entering one end and hitting something beyond the far end - a real hit at a greater distance, exactly the case below.
    if (hit >= PROBE_MISS * 0.5) return false;

    return dist < hit - uProbeBias;
}

bool anyProbeSees(vec3 world)
{
    for (int i = 0; i < uProbeCount; ++i)
    {
        if (visibleFrom(i, world)) return true;
    }
    return false;
}

// Walk the boundary between an open and blocked sample down to a fraction of the sample spacing. Counting samples quantises the answer to the spacing, which made a six metre arch
// inside a six metre slab read as a quarter solid because the slab boundary did not line up with the opening.
float refineEdge(vec2 xy, float z_open, float z_blocked)
{
    for (int k = 0; k < 4; ++k)
    {
        float mid = 0.5 * (z_open + z_blocked);
        if (anyProbeSees(vec3(xy, mid))) z_open = mid;
        else                             z_blocked = mid;
    }
    return 0.5 * (z_open + z_blocked);
}

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;
    if (!inBox(c)) return;

    float top = imageLoad(uHeight, c.xy).r;

    float lo = uSliceZ[c.z];
    float hi = uSliceZ[c.z + 1];

    // Soft occupancy: the fraction of this slab under the surface. A fraction rather than a hard in/out so a roofline between two slabs eases the flow instead of popping it, and
    // so raising the camera out of the geometry fades the obstacles away smoothly.
    float overlap = max(0.0, min(hi, top) - lo);
    float solid = clamp(overlap / max(hi - lo, 0.01), 0.0, 1.0);

    // Carve by measuring headroom, not counting samples. What matters to the flow is the slab's vertical clearance and where a passage's floor and ceiling sit inside
    // it. Scan the covered part, find the open span, then bisect its two edges so the clearance resolves far finer than the scan spacing. A six metre arch inside a six metre slab
    // comes out open whether or not the boundary lines up with it, instead of losing a quarter of the slab to whichever sample fell in the stonework.
    if (solid > 0.0 && uProbeCount > 0)
    {
        float cell = cellSize();
        vec2 xy = uOrigin + (vec2(c.xy) + 0.5) * cell;

        float covered_hi = min(hi, top);
        float thickness = max(covered_hi - lo, 0.01);

        // Scan at roughly the horizontal resolution, so the carve's vertical detail matches what the mask can hold
        int steps = clamp(int(ceil(thickness / max(cell, 0.01))), 4, 12);
        float dz = thickness / float(steps);

        int first = -1;
        int last = -1;
        for (int n = 0; n < steps; ++n)
        {
            float z = lo + (float(n) + 0.5) * dz;
            if (anyProbeSees(vec3(xy, z)))
            {
                if (first < 0) first = n;
                last = n;
            }
        }

        if (first >= 0)
        {
            float z_first = lo + (float(first) + 0.5) * dz;
            float z_last  = lo + (float(last) + 0.5) * dz;

            // An open span running off either end of the slab continues into the neighbouring one; there is no edge to find inside this slab.
            float open_lo = (first == 0)
                ? lo : refineEdge(xy, z_first, z_first - dz);
            float open_hi = (last == steps - 1)
                ? covered_hi : refineEdge(xy, z_last, z_last + dz);

            solid *= 1.0 - clamp((open_hi - open_lo) / thickness, 0.0, 1.0);
        }
    }

    // The true fraction treats a building filling a third of a thick slab as a third of a wall, which is air the flow simply pushes through. Bending the curve lets partial cover stand up as a wall
    // without waiting for the slab count to catch up with the roofline.
    if (solid > 0.0 && uSolidCurve != 1.0)
    {
        solid = clamp(pow(solid, uSolidCurve), 0.0, 1.0);
    }

    imageStore(uSolid, c, vec4(solid, 0.0, 0.0, 0.0));

    // Seed with this slab's ambient wind, killed inside solids. The projection that follows makes it flow around them.
    vec3 v = uAmbient[c.z] * (1.0 - solid);
    imageStore(uVel, c, vec4(v, 0.0));
}

