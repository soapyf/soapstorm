/**
 * @file ssWindSeedC.glsl
 * @brief Atmo Magic wind flowmap: seed a level's velocity field from its solid
 *        mask. The finest level gets this for free out of the init pass, which
 *        reads the height capture; the coarser levels have no height field of
 *        their own, only a mask restricted down from the one above.
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

uniform vec3 uAmbient[16];      // ambient wind per slab; sky tracks differ

layout(r8,      binding = 1) uniform readonly  image3D uSolid;
layout(rgba16f, binding = 2) uniform writeonly image3D uVel;

void main()
{
    ivec3 c = ivec3(gl_GlobalInvocationID);
    if (!inBounds(c)) return;

    float solid = imageLoad(uSolid, c).r;
    imageStore(uVel, c, vec4(uAmbient[c.z] * (1.0 - solid), 0.0));
}

