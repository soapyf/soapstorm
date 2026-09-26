/**
 * @file ssInfoLookV.glsl
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

// <SS:Nexii> The info-view look's full-screen triangle. Identical in shape to postDeferredNoTCV/blurLightV: the
// vertex positions of gPipeline.mScreenTriangleVB are ALREADY in clip space, so no matrix is applied here and the
// pass is independent of whatever the gGL matrix stack holds - only the FRAGMENT stage reads the camera, through
// inv_proj/projection_matrix, which syncMatrices uploads from the world camera the call site loads.
in vec3 position;

out vec2 vary_fragcoord;

void main()
{
    vec4 pos = vec4(position.xyz, 1.0);
    gl_Position = pos;
    vary_fragcoord = (pos.xy * 0.5 + 0.5);
}
