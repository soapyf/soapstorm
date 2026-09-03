/**
 * @file class1/deferred/ssSurfaceCommitF.glsl
 * @brief Atmo Magic: put a reworked specular buffer back into the gbuffer.
 *
 * The wetness pass cannot write the specular attachment while it is reading
 * it, so it works into a scratch target and this puts the result back. A draw
 * rather than a texture copy: glCopyTexSubImage2D has to be told which texture
 * it is copying into by way of whichever texture unit happens to be active,
 * and the renderer's own binding cache is entitled to skip the call that sets
 * that. Writing through the framebuffer says where the pixels go in the only
 * terms the driver cannot misread.
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

// <SS:Nexii> Atmo Magic surface field

// Four outputs so the linker's locations line up with the gbuffer's attachments. Only the second is written; the rest are masked off with glDrawBuffers at the call site, and writes to a
// buffer set to GL_NONE go nowhere, so the diffuse, normal and emissive attachments are untouched.
out vec4 frag_data[4];

in vec2 vary_fragcoord;

uniform sampler2D ssCommitSource;

// Which attachment the source lands in: 1 specular (the wetness pass), 2 normal (the flatten
// pass), 0 diffuse (the snow pass lifts albedo). The draw-buffers mask at the call site is what
// actually steers the write - this only picks which output the source is written through, and
// every other output stays masked to GL_NONE.
uniform float ssCommitTarget;

// Diagnostic. Above zero, paints the diffuse attachment flat magenta as well as writing the specular. Albedo multiplies straight into the final colour on every path, so this
// cannot be mistaken for a subtle lighting change, swallowed by an overcast sky, or argued with. It separates "nothing this pass writes lands anywhere" from "the specular buffer
// lands and the lighting does nothing with it", the only two possibilities left, wanting opposite fixes.
uniform float ssCommitDebugPaint;

void main()
{
    vec4 src = texture(ssCommitSource, vary_fragcoord.xy);

    // Every output written, every frame, unconditionally: an output the call site's
    // draw-buffers mask routes somewhere is undefined until written, and this exact hole bit
    // once - the normal commit only filled frag_data[2] while the mask routed the untouched
    // frag_data[1] into the normal attachment, so the first frame the flatten pass had real
    // work to do, the gbuffer normals became whatever the undefined output held. Black world.
    // The mask steers these; this only guarantees nothing is undefined.
    frag_data[0] = src;
    frag_data[1] = src;
    frag_data[2] = src;
    frag_data[3] = src;

    if (ssCommitTarget < 0.5)
    {
        return;
    }

    if (ssCommitDebugPaint > 0.0 && ssCommitTarget < 1.5)
    {
        frag_data[0] = vec4(1.0, 0.0, 1.0, 0.0);

        // The diffuse paint above is already proven to reach the screen (the freeze-frame test). Same proof for the OTHER channel this pass writes - the one the wetness effect actually
        // uses - never independently checked. ORM = (0 occlusion, 0 roughness, 1 metal): PBR surfaces should go a hard mirror, legacy surfaces a strange
        // blue-tinted specular. If this channel reaches the screen the same way diffuse did, this is unmistakable regardless of lighting or haze.
        frag_data[1] = vec4(0.0, 0.0, 1.0, 0.0);
    }
}

