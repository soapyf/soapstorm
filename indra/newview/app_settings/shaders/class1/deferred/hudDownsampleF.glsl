/**
 * @file hudDownsampleF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform sampler2D worldDepthMap;

uniform int hud_supersample; // supersample factor, 2 or 4
uniform vec2 hud_texel_size; // 1 / source target dimensions

in vec2 vary_fragcoord;

// <SS:Nexii> Box resolve of the supersampled HUD target. The source already holds the finished composite of background plus HUD, so this is a plain unweighted average with no alpha handling to get right. Sampling is point filtered, so every tap lands on exactly one source texel and the block is counted once. Rationale in doc/hud_supersampling.md.
void main()
{
    // vary_fragcoord is the destination pixel centre in [0,1]. Walk back to the leading edge of the factor x factor source block it covers, then in by half a texel to land on the first texel's centre.
    vec2 base = vary_fragcoord - (0.5 * float(hud_supersample) - 0.5) * hud_texel_size;

    vec4 sum = vec4(0.0);
    float nearest = 1.0;

    for (int y = 0; y < hud_supersample; ++y)
    {
        for (int x = 0; x < hud_supersample; ++x)
        {
            vec2 tc = base + vec2(float(x), float(y)) * hud_texel_size;

            sum += texture(diffuseRect, tc);
            nearest = min(nearest, texture(depthMap, tc).r);
        }
    }

    frag_color = sum / float(hud_supersample * hud_supersample);

    // Hand the HUD's depth back to the default framebuffer so anything drawn afterwards that depth tests -- avatar
    // nametags, non-HUD hover text, look-at indicators -- is still occluded by HUD attachments. Depth cannot be averaged
    // the way colour can, so take the nearest sample in the block: a partially covered edge pixel occludes, which errs
    // on the side of the HUD hiding what is behind it.
    // Where no HUD attachment was drawn (or where world geometry is nearer), preserve the world depth from worldDepthMap
    // so subsequent 3D elements (look-at indicators, beacons, hover text) remain properly occluded by world objects and terrain.
    float world_depth = texture(worldDepthMap, vary_fragcoord.xy).r;
    gl_FragDepth = min(nearest, world_depth);
}
