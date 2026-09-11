/**
 * @file ssVortexV.glsl
 * @brief Atmo Magic vortex funnels - Z-billboarded collar quads.
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

// <SS:Nexii> Atmo Magic vortex funnels - collar quads for tornadoes/waterspouts/landspouts/satellites, drawn in
// the sky forward pass right after the volumetric deck (ssvortexrender.cpp). Exactly ssVolCloudV.glsl's own
// far-field squash (also the idiom ssLightningV.glsl repeats): CPU submits the TRUE, unsquashed position of every
// collar-span vertex - already subdivided by SSVortex::squashSegments so a tall span still bends smoothly through
// the knee - and this pulls each one radially toward the camera, keeping its exact ray, so only depth compresses.

uniform mat4 modelview_projection_matrix;

uniform vec3 ss_squash;   // x knee, y cap, z virtual field radius - SSVolCloud's own, shared so a funnel inside a
                           // far cloud stays inside it in drawn depth too.
uniform vec3 ss_cam_pos;

in vec3 position;
in vec2 texcoord0;   // x: u across the collar's width [0,1] (theta's own input in the fragment stage); y: h01,
                      // this vertex's height fraction of the WHOLE funnel (0 contact, 1 wall cloud)
in vec4 diffuse_color; // r: lit-rim facing term for THIS corner (ssvortexrender.cpp computes it per corner, the
                        // same CPU-structure-term precedent SSVolCloud::Puff::mForm sets); g: the vortex's own live
                        // condensation (SSVortex::State::mCondensation, one number for the whole funnel); b: this
                        // funnel's slot / (SSVortex::MAX_ACTIVE - 1), for indexing ss_vortex_multi in the fragment
                        // stage; a: base alpha (overall funnel opacity)

out vec2 vary_texcoord0;
out vec4 vary_color;

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
    vary_color = diffuse_color;
}
