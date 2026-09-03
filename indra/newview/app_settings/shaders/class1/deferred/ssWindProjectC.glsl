/**
 * @file ssWindProjectC.glsl
 * @brief Atmo Magic wind flowmap: subtract the pressure gradient to leave a
 *        divergence-free field, then measure how exposed each cell is.
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

// A partial rebuild projects only inside its solve box; the preserved field
// outside it is left exactly as it was.
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

layout(r8,      binding = 1) uniform readonly image3D uSolid;
layout(rgba16f, binding = 2) uniform image3D uVel;
layout(r32f,    binding = 4) uniform readonly image3D uPIn;

uniform int   uShelterSteps;    // how far upwind to look for cover, in cells
uniform float uShelterAmount;   // how much of the wind a full lee takes away
uniform float uStrength;        // how far the solved field may depart from ambient
uniform float uMaxGain;         // ceiling on local speed-up, in ambients

// Same wall rule the solve used. A gradient taken against a solid cell's stored pressure would push air straight back into the geometry the solve just steered it around.
float loadP(ivec3 c, float here)
{
    if (c.x < 0 || c.y < 0 || c.x >= uRes || c.y >= uRes) return 0.0;
    if (c.z >= uSlices) return 0.0;
    if (c.z < 0) return here;

    float solid = imageLoad(uSolid, c).r;
    return mix(imageLoad(uPIn, c).r, here, solid);
}

float loadSolid(ivec3 c)
{
    if (c.x < 0 || c.y < 0 || c.x >= uRes || c.y >= uRes) return 0.0;
    int k = clamp(c.z, 0, uSlices - 1);
    return imageLoad(uSolid, ivec3(c.xy, k)).r;
}

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;
    if (!inBox(c)) return;

    float solid = imageLoad(uSolid, c).r;
    vec3 v = imageLoad(uVel, c).xyz;

    float here = imageLoad(uPIn, c).r;
    float d = cellSize();
    float du = sliceGapUp(c.z);
    float dd = sliceGapDown(c.z);

    vec3 grad;
    grad.x = (loadP(c + ivec3(1, 0, 0), here) - loadP(c - ivec3(1, 0, 0), here)) / (2.0 * d);
    grad.y = (loadP(c + ivec3(0, 1, 0), here) - loadP(c - ivec3(0, 1, 0), here)) / (2.0 * d);
    grad.z = (loadP(c + ivec3(0, 0, 1), here) - loadP(c - ivec3(0, 0, 1), here)) / (du + dd);

    v -= grad;
    v *= (1.0 - solid);

    // Lee sheltering. Projection alone slows the air in front of an obstacle and squeezes it through gaps, but it has no memory - the calm pocket behind a building never appears. Marching upwind
    // and counting cover puts it back for the cost of a few texel reads.
    vec3 amb = uAmbient[c.z];
    float amb_len = length(amb);
    float shelter = 0.0;

    if (amb_len > 0.01 && uShelterSteps > 0)
    {
        vec3 step_dir = -normalize(amb);     // toward where the wind came from
        for (int i = 1; i <= uShelterSteps; ++i)
        {
            vec3 p = vec3(c) + step_dir * float(i);
            float blocked = loadSolid(ivec3(round(p)));

            // Nearer cover shelters more than distant cover
            float weight = 1.0 - float(i - 1) / float(uShelterSteps);
            shelter = max(shelter, blocked * weight);
        }
    }

    v *= (1.0 - clamp(uShelterAmount, 0.0, 1.0) * shelter);

    // Above is the physical answer. Strength exaggerates how far it departs from the ambient wind, so deflections, lees and jets grow together without the wind changing speed.
    // Turning and slowing are scaled separately rather than extrapolating the vector - which looks equivalent and is not: a sheltered cell solving to half the ambient wind maps to exactly
    // zero at strength 2 and to a reversed vector beyond. Sheltered cells are the point of the setting, so the naive form nulls out precisely the passages and lees it was turned up to
    // make visible.
    float v_len = length(v);

    if (amb_len > 0.01)
    {
        vec3 amb_dir = amb / amb_len;
        vec3 v_dir = (v_len > 1e-4) ? v / v_len : amb_dir;

        // Exaggerate the turn. Renormalising keeps this a rotation however far it is pushed - it can swing past the solved heading but never collapse to nothing.
        vec3 turned = amb_dir + (v_dir - amb_dir) * max(uStrength, 0.0);
        vec3 dir = (length(turned) > 1e-4) ? normalize(turned) : v_dir;

        // Exaggerate the speed change, floored at zero: a lee can be brought to a standstill but never turned inside out.
        float len = max(0.0, amb_len + (v_len - amb_len) * max(uStrength, 0.0));

        v = dir * len;
    }

    v *= (1.0 - solid);     // solids stay dead however far it was pushed

    // A gap between two buildings accelerates air through it, and with a coarse mask that jet can run away. Cap it in ambients rather than m/s so it means the same thing at any wind speed.
    float speed = length(v);
    float ceiling = amb_len * max(uMaxGain, 0.0);
    if (ceiling > 0.0 && speed > ceiling)
    {
        v *= ceiling / speed;
    }

    // Exposure: how much of the ambient wind actually reaches here. This is what the audio mix rides, so a courtyard reads calm and an alley lined up with the wind reads louder than open ground.
    float exposure = (amb_len > 0.01) ? clamp(length(v) / amb_len, 0.0, 2.0) : 0.0;
    exposure *= (1.0 - solid);

    imageStore(uVel, c, vec4(v, exposure));
}

