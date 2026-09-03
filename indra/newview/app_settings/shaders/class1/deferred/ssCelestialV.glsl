/**
 * @file ssCelestialV.glsl
 * @brief Atmo Magic: one shader for every celestial disc it draws - the
 *        body in EEP's sun slot, the body in its moon slot, and the
 *        billboards for everything else.
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

// <SS:Nexii> Atmo Magic celestial discs: its own shader, not uniforms bolted onto the stock sun/moon ones. Three reasons, in order of how much they matter: 1. A GL uniform nobody sets is ZERO - "how bright"/"how far to drop the quad" on a stock shader means every call site that binds it (now and future, ours and upstream's) must set them or draw a black disc at the wrong height; a separate program cannot be bound by accident. 2. Atmo Magic wants fixed constants, not tunable ones - no legacy 50m drop here (see below), terminator softness and emissive gain are fixed looks - hardcoding says so and keeps them by the code that reads them. 3. Upstream shaders stay untouched: stock renders byte-identically and merges stay clean. What still arrives by uniform is per-BODY state - star position, quad facing, self-lighting - since that genuinely differs per disc.

uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;

out vec2 vary_texcoord0;

void main()
{
    // No vertex offset: sunDiscV.glsl subtracts vec3(0, 0, 50) here - a legacy sky fudge, the same 50 the cloud shader carries as vec3(0, 50, 0). At the sun's ~1004m that is 2.85 degrees
    // of elevation - invisible when nothing says where the sun ought to be, glaring when an authored orbit does: the disc drew nearly three degrees below where it was placed,
    // while the haze glow around it (from the atmosphere shader and the true direction) stayed put.
    vec4 pos = modelview_projection_matrix * vec4(position.xyz, 1.0);

    // The sky's depth layers, and this disc's place in them. Every skybox pass runs under LLGLSPipelineSkyBox, whose LLGLSquashToFarClip replaces the projection's z row with its w row times 0.99999
    // (llgl.cpp, setProjectionMatrix) - so the haze dome and, by default, everything else in the sky land on that value whatever their geometry says. The dome's real ~5000m never reaches the
    // depth buffer at all, which is why arguing where 5000m falls on the depth curve only produced numbers that happened to work. With cloudsV.glsl now taking 0.99998 for itself, the sky
    // reads: 0.99998   cloud layer 0.99999   haze dome, and these discs 1.0       stars (starsV.glsl) A disc must sit clear of the clouds so they can cover it - it is ADDED to the sky, so depth is
    // the only thing that can hide it - and clear of the stars so its depth write still clips them. Sharing the haze dome's exact value is the thing to watch. LEQUAL passes on equality, and the
    // disc is drawn after the dome, so it draws. But the dome reaches 0.99999 via the rewritten projection while this reaches it by multiplying, and if they disagree in the last bits the disc
    // will speckle against the HAZE - the same failure, moved one layer along. If it shows up, move these discs to 0.999995, midway between the dome and the stars, rather
    // than to move the clouds again.
    pos.z = pos.w * 0.99999;
    gl_Position = pos;

    vary_texcoord0 = texcoord0;
}

