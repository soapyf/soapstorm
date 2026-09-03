/**
 * @file ssWindRestrictC.glsl
 * @brief Atmo Magic wind flowmap: halve a solid mask horizontally for the next
 *        level down the pressure pyramid.
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

// Averaged rather than max-pooled, which would close an alley narrower than two coarse cells - and the coarse levels exist precisely to work out how much air the long lanes carry - an alley
// that seals shut on the grid that decides that is worse than one reading half-open. A fractional mask is already what the fine level uses for a roofline between slabs, so the solver
// needs nothing new to understand it.
layout(r8, binding = 6) uniform readonly  image3D uFine;
layout(r8, binding = 7) uniform writeonly image3D uCoarse;

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;

    ivec3 f = ivec3(c.x * 2, c.y * 2, c.z);

    float sum = imageLoad(uFine, f).r
              + imageLoad(uFine, f + ivec3(1, 0, 0)).r
              + imageLoad(uFine, f + ivec3(0, 1, 0)).r
              + imageLoad(uFine, f + ivec3(1, 1, 0)).r;

    imageStore(uCoarse, c, vec4(sum * 0.25, 0.0, 0.0, 0.0));
}

