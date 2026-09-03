/**
 * @file ssPrecipRainV.glsl
 * @brief Atmo Magic rain particle vertex shader (SS:Nexii)
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

uniform mat4 modelview_matrix;
uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec3 normal;
in vec4 tangent;
in vec4 diffuse_color;
in vec2 texcoord0;

void calcAtmospherics(vec3 inPositionEye);

out vec3 vary_position;
out vec3 vary_normal;
out vec3 vary_axis;
out vec4 vertex_color;
out vec2 vary_texcoord0;

void main()
{
    vec4 vert = vec4(position.xyz, 1.0);
    vec4 pos = modelview_matrix * vert;
    gl_Position = modelview_projection_matrix * vert;

    vary_position = pos.xyz;
    // Billboards carry the direction to the eye here, ripples the normal of the surface they are lying on; both want the plain rotation into view space, so the modelview matrix does it rather than a
    // normal matrix.
    vary_normal = (modelview_matrix * vec4(normal, 0.0)).xyz;

    // The quad's own long axis in the world - the way the drop is falling for a streak, the surface's own axis for a ripple. The rain shader builds its droplet normal around this rather than around
    // the screen, so the water reads by how the drop sits in the scene instead of by where the camera happens to be pointing.
    vary_axis = (modelview_matrix * vec4(tangent.xyz, 0.0)).xyz;
    vary_texcoord0 = texcoord0;

    calcAtmospherics(pos.xyz);

    vertex_color = diffuse_color;
}
