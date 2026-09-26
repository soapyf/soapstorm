/**
 * @file ssHiZC.glsl
 * @brief Soapstorm GPU culling: build the Hi-Z depth pyramid. Pass 0 copies the
 *        main depth buffer into level 0, pass 1 max-reduces each level from the
 *        one above it (dispatched once per level with a barrier in between).
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

// <SS:Nexii> GPU culling Hi-Z build
layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D uDepthTex;        // main scene depth (uMode 0 only)
uniform sampler2D uHiZTex;          // this pyramid, level uLevel-1 (uMode 1)
layout(r32f, binding = 0) uniform writeonly image2D uHiZImg; // this pyramid, level uLevel

uniform int uMode;      // 0 = copy depth into level 0, 1 = reduce level uLevel-1 into uLevel
uniform int uLevel;
uniform ivec2 uSrcSize; // source level size in texels
uniform ivec2 uDstSize; // destination level size in texels

// Depth is standard GL [0..1] with farthest = largest, so the pyramid keeps the
// max: a box can only be occluded where its nearest point is behind the
// farthest depth seen anywhere over its screen rect.
void main()
{
    ivec2 dst = ivec2(gl_GlobalInvocationID.xy);
    if (dst.x >= uDstSize.x || dst.y >= uDstSize.y) return;

    float d;
    if (uMode == 0)
    {
        d = texelFetch(uDepthTex, dst, 0).r;
    }
    else
    {
        ivec2 s0 = min(dst * 2, uSrcSize - 1);
        ivec2 s1 = min(dst * 2 + ivec2(1), uSrcSize - 1);
        d = max(
            max(texelFetch(uHiZTex, s0, uLevel - 1).r, texelFetch(uHiZTex, s1, uLevel - 1).r),
            max(texelFetch(uHiZTex, ivec2(s0.x, s1.y), uLevel - 1).r, texelFetch(uHiZTex, ivec2(s1.x, s0.y), uLevel - 1).r));
    }
    imageStore(uHiZImg, dst, vec4(d, 0.0, 0.0, 1.0));
}
