/**
 * @file ssLightningV.glsl
 * @brief Atmo Magic lightning - channel ribbons, plasma, sparks, aura and fire discs.
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

// <SS:Nexii> Atmo Magic lightning

uniform mat4 modelview_projection_matrix;

// The same far-field squash and ss_squash uniform the puff field's vertex shader applies - one mapping, so a bolt inside a far cloud stays inside it in drawn depth too. Per vertex,
// each keeps its exact ray from the camera, replacing the old CPU per-strike scale (which collapsed everything beyond a limit onto one shell; this compresses progressively and
// a 5km crawler spanning the knee bends correctly through it).
uniform vec3 ss_squash;
uniform vec3 ss_cam_pos;

// Every quad of the pass arrives through one vertex buffer (sslightningrender.cpp builds it): position and texcoord0 as before, and beside the 8-bit tint two float attributes the
// immediate-mode path could never carry - normal (the per-vertex data the fragment stage reads: noise seed / amber weight / plasma phase for ribbons, anchor depth / height above ground for discs)
// and tangent (HDR brightness above white, a mode-specific weight, the depth soft width, and the fragment mode in w).
// doc/atmo_magic_lightning_strike.md
in vec3 position;
in vec2 texcoord0;
in vec2 texcoord1;
in vec4 diffuse_color;
in vec3 normal;
in vec4 tangent;

out vec2 vary_texcoord0;
out vec2 vary_texcoord1;
out vec4 vary_color;
out vec3 vary_aux;
out vec4 vary_ctl;

void main()
{
    vec3 rel = position.xyz - ss_cam_pos;
    float d = length(rel);
    vec3 drawn_pos = position.xyz;
    if (d > ss_squash.x && ss_squash.z > ss_squash.x)
    {
        float drawn = ss_squash.x + (d - ss_squash.x) * (ss_squash.y - ss_squash.x) / (ss_squash.z - ss_squash.x);
        drawn = min(drawn, ss_squash.y * 0.999);
        drawn_pos = ss_cam_pos + rel * (drawn / d);
    }
    gl_Position = modelview_projection_matrix * vec4(drawn_pos, 1.0);

    vary_texcoord0 = texcoord0;
    vary_texcoord1 = texcoord1;
    vary_color = diffuse_color;
    vary_aux = normal;
    vary_ctl = tangent;
}
