/**
 * @file ssCullC.glsl
 * @brief Soapstorm GPU culling: one thread per candidate bounding box. Frustum
 *        test against the camera planes, then a conservative Hi-Z occlusion
 *        test against the previous frame's depth pyramid; writes one visibility
 *        byte per candidate for the async readback that gates next frame's
 *        draw loop.
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

// <SS:Nexii> GPU culling visibility test
layout(local_size_x = 64) in;

layout(r8, binding = 0) uniform writeonly image2D uVis; // 255 = draw, 0 = occluded
uniform sampler2D uHiZ;                    // depth pyramid, max per texel
uniform sampler2D uBoxes;                  // RGBA32F, 2 texels per box: (min.xyz, max.x), (max.y, max.z, -)

uniform mat4 uViewProj;  // agent space -> clip
uniform vec4 uPlanes[6]; // agent-space frustum planes (xyz = normal, w = d), far plane omitted
uniform int uCount;
uniform vec2 uViewport;  // depth buffer size in pixels
uniform int uMips;       // usable pyramid levels
uniform float uBias;     // conservative depth slop

// Same p-vertex support-corner test LLCamera::AABBInFrustum does on the CPU.
bool inFrustum(vec3 bmin, vec3 bmax)
{
    for (int i = 0; i < 6; ++i)
    {
        vec3 n = uPlanes[i].xyz;
        vec3 support = vec3(n.x >= 0.0 ? bmax.x : bmin.x,
                            n.y >= 0.0 ? bmax.y : bmin.y,
                            n.z >= 0.0 ? bmax.z : bmin.z);
        if (dot(n, support) + uPlanes[i].w < 0.0) return false;
    }
    return true;
}

void main()
{
    int id = int(gl_GlobalInvocationID.x);
    if (id >= uCount) return;

    const int box_w = 16384;
    int t = id * 2;
    vec4 ta = texelFetch(uBoxes, ivec2(t % box_w, t / box_w), 0);
    vec4 tb = texelFetch(uBoxes, ivec2((t + 1) % box_w, (t + 1) / box_w), 0);

    vec3 bmin = ta.xyz;
    vec3 bmax = vec3(ta.w, tb.xy);
    float vis = 1.0;

    if (inFrustum(bmin, bmax))
    {
        float zmin = 2.0;
        vec2 rmin = vec2(1e30);
        vec2 rmax = vec2(-1e30);
        bool clipped = false;
        for (int i = 0; i < 8; ++i)
        {
            vec3 corner = vec3((i & 1) != 0 ? bmax.x : bmin.x,
                               (i & 2) != 0 ? bmax.y : bmin.y,
                               (i & 4) != 0 ? bmax.z : bmin.z);
            vec4 clip = uViewProj * vec4(corner, 1.0);
            if (clip.w <= 0.0)
            {
                // straddles the near plane - can't test against the pyramid, assume visible
                clipped = true;
                break;
            }
            vec3 ndc = clip.xyz / clip.w;
            zmin = min(zmin, ndc.z);
            vec2 w = (ndc.xy * 0.5 + 0.5) * uViewport;
            rmin = min(rmin, w);
            rmax = max(rmax, w);
        }

        if (!clipped && zmin < 1.0 && rmax.x > rmin.x && rmax.y > rmin.y)
        {
            vec2 lo = max(rmin, vec2(0.0));
            vec2 hi = min(rmax, uViewport - 1.0);
            vec2 dim = max(hi - lo, vec2(1.0));
            // take the level where the rect spans about 2 texels so the taps below cover it
            int lod = clamp(int(floor(log2(max(dim.x, dim.y)))) - 1, 0, uMips - 1);
            ivec2 texsz = ivec2(max(uViewport / vec2(1 << lod), vec2(1.0))) - 1;

            float dmax = 0.0;
            for (int i = 0; i < 5; ++i)
            {
                vec2 pt = (i == 0) ? lo : (i == 1) ? hi : (i == 2) ? vec2(lo.x, hi.y)
                        : (i == 3) ? vec2(hi.x, lo.y) : (lo + hi) * 0.5;
                dmax = max(dmax, texelFetch(uHiZ, clamp(ivec2(pt), ivec2(0), texsz), lod).r);
            }

            // nearest point of the box behind everything the pyramid saw over its rect
            if (zmin >= dmax - uBias) vis = 0.0;
        }
    }

    imageStore(uVis, ivec2(id, 0), vec4(vis, 0.0, 0.0, 0.0));
}
