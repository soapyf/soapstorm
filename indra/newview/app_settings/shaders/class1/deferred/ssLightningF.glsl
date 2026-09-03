/**
 * @file ssLightningF.glsl
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_texcoord0;
in vec2 vary_texcoord1;
in vec4 vary_color;
in vec3 vary_aux;
in vec4 vary_ctl;

// An electric-line texture tiled along the ribbon (u across the width, v down the length). ss_use_tex 0 falls back to the procedural core below, so the bolt draws with no asset configured.
uniform sampler2D diffuseMap;
uniform float ss_use_tex;

// A COPY of the scene depth (SSVolCloud takes it, the lightning pass shares it) under the reserved name the binder needs - see the depthMap note in ssVolCloudF.glsl. ss_soft_on gates every read
// so the flash pass, which runs before any copy exists, never samples an unbound unit. ss_clip is the projection's near and far (the CONSTANT far plane, not the draw distance).
uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec2 ss_clip;
uniform float ss_soft_on;

// The bloom dial, the shared clock (wrapped on the CPU so the float keeps sub-millisecond steps) and the live bolt's beading. The dissolve's flow amplitude is not a uniform: it rides tangent.z per vertex, beside the noise LOD in tangent.y, because only the plasma copies carry it.
uniform float ss_glow;
uniform float ss_time;
uniform float ss_bead;

// <SS:Nexii> The plasma's colour walk over its life, from the recorded frames: the column in the air goes white to white-cyan to grey (never green - the green stays in the foot), the amber foot goes
// white-yellow through yellow-green to a yellow knot that outshines the wisps, and the hot flare centre is over-exposed orange-white. doc/atmo_magic_lightning_strike.md
const vec3 RAMP_AIR0 = vec3(1.00, 1.00, 1.00);
const vec3 RAMP_AIR1 = vec3(0.85, 0.98, 1.00);
const vec3 RAMP_AIR2 = vec3(0.78, 0.82, 0.86);
const vec3 RAMP_AIR3 = vec3(0.60, 0.60, 0.58);
const vec3 RAMP_GND0 = vec3(1.00, 0.85, 0.50);
const vec3 RAMP_GND1 = vec3(0.85, 1.00, 0.55);
const vec3 RAMP_GND2 = vec3(0.98, 0.95, 0.45);
const vec3 RAMP_GND3 = vec3(0.70, 0.45, 0.20);
const vec3 KNOT_COLOR = vec3(1.00, 0.95, 0.45);
const vec3 HOT_COLOR = vec3(1.00, 0.93, 0.68);

// Eye-space distance from a depth-buffer reading. The projection is the ordinary one, so this is just its inverse.
float ss_eye_z(float d)
{
    float ndc = d * 2.0 - 1.0;
    return (2.0 * ss_clip.x * ss_clip.y)
         / (ss_clip.y + ss_clip.x - ndc * (ss_clip.y - ss_clip.x));
}

// Integer hash noise for the plasma: deterministic in ribbon space, so every client and every frame agrees on where the wisps are.
uint ss_uh(uvec2 p)
{
    p *= uvec2(1597334677u, 3812015801u);
    uint h = (p.x ^ p.y) * 1597334677u;
    h ^= h >> 16;
    return h;
}

float ss_h21(vec2 p)
{
    return float(ss_uh(uvec2(ivec2(floor(p)) + 0x7fff))) * (1.0 / 4294967296.0);
}

float ss_vnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(ss_h21(i), ss_h21(i + vec2(1.0, 0.0)), f.x),
               mix(ss_h21(i + vec2(0.0, 1.0)), ss_h21(i + vec2(1.0, 1.0)), f.x), f.y);
}

vec3 ss_ramp4(vec3 c0, vec3 c1, vec3 c2, vec3 c3, float u)
{
    if (u < 0.35) return mix(c0, c1, u / 0.35);
    if (u < 0.65) return mix(c1, c2, (u - 0.35) / 0.30);
    return mix(c2, c3, clamp((u - 0.65) / 0.25, 0.0, 1.0));
}

void main()
{
    // The fragment mode rides tangent.w: 0 live core ribbon (and the plasma copy, which is the same ribbon dissolving - normal.z carries its age), 1 sheath ribbon, 2 aura / flare / fire disc (fraction = the flare share), 3 plain ribbon (sparks), 4 sky flash disc, 5 flat fill (wash, markers), 6 occlusion box, 7 steam puff (the one alpha-blended element, drawn in its own batch).
    // <SS:Nexii> The epsilon is what makes this dispatch survive the rasteriser. ctl.w is one constant across a quad's four vertices, but perspective-correct interpolation of a constant is only exact in exact arithmetic: a 5.0 arrives as 4.9999995 on some pixels and 5.0000005 on others, and a bare floor() reads the first as mode 4. Every mode then draws as itself on half its pixels and as the mode BELOW it on the rest - a marker's flat fill as a flash disc, a flash disc as a plain ribbon (its hard quad edge and all), the sheath as the beaded core - which is the per-pixel stipple that speckled the discs, the sheath and the debug markers while the core, whose -1 still falls through to the ribbon path, stayed clean. doc/atmo_magic_lightning_strike.md
    int mode = int(floor(vary_ctl.w + 0.001));
    float bright = vary_ctl.x;
    vec3 col = vary_color.rgb;
    float a8 = vary_color.a;

    if (mode == 6)
    {
        // The occlusion query's box: every fragment must count, so nothing here may discard or read depth.
        frag_color = vec4(0.0);
        return;
    }

    if (mode == 5)
    {
        // A flat fill: the fullscreen amber wash (a8 0 - a veil is never a bloom seed) and the debug markers.
        frag_color = vec4(col * bright, a8);
        return;
    }

    if (mode == 7)
    {
        // <SS:Nexii> The steam burst: the one element of this pass that is NOT a light. Boiled water scatters what is already in the air rather than emitting, so it draws alpha-blended in its own batch and writes no bloom seed at all - an additive white puff over a night storm reads as a second flash, which is exactly wrong. Broken up by two octaves turning slowly in the disc's own frame so it billows instead of presenting a soft circle, and eaten away from the rim as it ages (aux.y, 0 at the boil and 1 at the end of its life) so the cloud thins and tatters rather than fading as a whole. doc/atmo_magic_lightning_strike.md
        vec2 d = vary_texcoord0 * 2.0 - 1.0;
        float rr = length(d);
        if (rr >= 1.0) discard;

        float age = vary_aux.y;
        float s = vary_aux.x * 71.0;
        float n = ss_vnoise(d * 2.3 + s + vec2(0.0, -ss_time * 0.5)) * 0.65
                + ss_vnoise(d * 5.1 + s * 1.7 + vec2(ss_time * 0.35, 0.0)) * 0.35;

        float body = pow(1.0 - rr, 1.25) * mix(0.55, 1.45, n);
        body *= smoothstep(0.0, 0.35, 1.0 - age * (0.45 + 0.75 * n));

        float a = clamp(body * bright, 0.0, 1.0);
        if (a < 2.0 / 255.0) discard;
        frag_color = vec4(col, a);
        return;
    }

    if (mode == 4)
    {
        // The sky flash: a soft disc of light where the discharge is, what the air and cloud around a channel actually do. Cubed rather than linear so it reads as a glow with a centre
        // instead of a painted circle with an edge. The height fade (aux.y, height above the strike's surface in radii) dissolves the lowest trunk disc around the ground, where its plane
        // otherwise ends in a hard chord across the terrain; forks and sheets pass a surface far below and never fade.
        vec2 d = vary_texcoord0 * 2.0 - 1.0;
        float r = clamp(1.0 - length(d), 0.0, 1.0);
        float soft = r * r * r;
        soft *= smoothstep(-0.35, 0.25, vary_aux.y);
        frag_color = vec4(col * bright * soft, ss_glow * a8 * soft);
        return;
    }

    if (mode == 2)
    {
        // Aura, flare and fire discs: a soft skirt, and the flare's share (the fraction of ctl.w) laid on with a power-law spike at the contact.
        // Three fades against the world: the per-vertex height above the surface (aux.y) dissolves the bottom edge instead of the depth test's hard chord; the anchor compare (aux.x, the true
        // strike point's view-axis depth) fades pixels where the scene sits well in front of the point - lenient, because at a grazing view the road under the disc is nearer than the point
        // without hiding it; and the soft-particle compare against the disc's own drawn depth softens whatever it still passes through.
        vec2 d = vary_texcoord0 * 2.0 - 1.0;
        float rr = length(d);
        if (rr >= 1.0) discard;
        float r = 1.0 - rr;

        float skirt = pow(r, 1.5);

        // <SS:Nexii> Taken against the mode the dispatch actually chose, never fract(): the same interpolation slop puts a flare-less disc's 2.0 at 1.9999999, where fract returns 0.9999999 and hands a disc with NO flare share the full spike on those pixels.
        float q = clamp((vary_ctl.w - float(mode)) * 2.0, 0.0, 1.0);
        float spike = (q > 0.0) ? 1.6 * q * pow(r, 6.0) : 0.0;

        float bf = smoothstep(-0.10, 0.45, vary_aux.y);

        float occ = 1.0;
        if (ss_soft_on > 0.5 && vary_ctl.z > 0.0)
        {
            float depth = texture(depthMap, gl_FragCoord.xy / screen_res).r;
            if (depth < 1.0)
            {
                float scene_z = ss_eye_z(depth);
                float anchor = vary_aux.x;
                occ = smoothstep(anchor * 0.45, anchor * 0.7, scene_z);
                float frag_z = ss_eye_z(gl_FragCoord.z);
                occ *= clamp((scene_z - frag_z) / vary_ctl.z, 0.0, 1.0);
            }
        }

        float I = (skirt + spike) * bright * bf * occ;
        float sp = clamp(spike, 0.0, 1.0);
        vec3 rgb = mix(col, HOT_COLOR, sp) * I;
        if (max(max(rgb.r, rgb.g), rgb.b) < 2.0 / 255.0) discard;
        frag_color = vec4(min(rgb, vec3(3.0)), ss_glow * (a8 + 0.35 * sp) * min(I, 1.0));
        return;
    }

    // Ribbons. Across the strip: 0 at one edge, 1 at the other, core in the middle.
    float across = abs(vary_texcoord0.x * 2.0 - 1.0);
    float x = across;
    float along = vary_texcoord0.y;
    float u = vary_aux.z;
    float w = vary_aux.y;
    float s = vary_aux.x * 97.0;
    float bead_mul = vary_texcoord1.x;
    float up_along = vary_texcoord1.y;
    float lod = vary_ctl.y;

    float mask;
    if (mode == 3 || mode == 1 || (u <= 0.0 && ss_bead <= 0.0))
    {
        // The cheap path, first: sparks, the sheath always, and the intact core with beading off - one power, no hashes. The fourth power makes the middle read as a discharge rather than
        // a painted stripe (the falloff from a line source really is this steep); the sheath and the amber foot lie softer.
        float e = (mode == 1) ? 3.0 : ((mode == 3) ? 4.0 : mix(4.0, 2.5, w));
        mask = pow(1.0 - x, e);
        if (ss_use_tex > 0.5 && mode == 0)
        {
            // The texture's own alpha is the channel shape; its red carries filament detail the author drew. Multiplied by the width falloff so a rectangular texture still ends softly at the
            // ribbon's edge.
            vec4 t = texture(diffuseMap, vary_texcoord0);
            mask = t.a * max(t.r, 0.35) * (1.0 - x * x);
        }
    }
    else
    {
        // The live core with beading: the strip's own profile lumps along its length, because a re-lit column beads where the old wisps had pinched.
        float b = ss_bead * bead_mul;
        float bead = ss_vnoise(vec2(along * 0.8 + s, 0.0));
        float wmul = 1.0 - b * 0.5 + b * bead;
        x /= max(wmul, 0.05);

        // The live core softens toward the amber foot; the plasma copy steepens straight back to the channel's own falloff, because the wisps come off the THIN core, not the glow gradient
        // around it - the wide glow is the sheath's and the foot's to fade out.
        float body = pow(max(0.0, 1.0 - x), mix(mix(4.0, 2.5, w), 4.0, smoothstep(0.0, 0.2, u)));
        if (ss_use_tex > 0.5)
        {
            vec4 t = texture(diffuseMap, vec2(clamp(0.5 + 0.5 * x, 0.0, 1.0), vary_texcoord0.y));
            body = t.a * max(t.r, 0.35) * (1.0 - x * x);
        }
        // The edge guard on the ORIGINAL across keeps a beaded edge from ending in the quad's straight cut.
        body *= smoothstep(1.0, 0.8, across);
        mask = body * (0.6 + 0.4 * mix(1.0, bead, b));

        // <SS:Nexii> The dissolve, laid OVER the bolt that is already there rather than replacing it: the same glowing channel, taken away by an animated alpha mask read through a flow map.
        // The flow is the physics. A vortex field - two noise channels read as a vector, rolling so the curl is alive rather than a fixed distortion - plus a steady convection term that lifts
        // along whichever way world up runs in this strip's frame (texcoord1.y, +1 for a channel running straight down, 0 where it lies flat and there is nowhere along the strip to rise).
        // That vector displaces the coordinate the mask is sampled at, and the displacement grows with age, so early on the column is barely disturbed and late on it is curling and climbing.
        // It stays SMALL on purpose: what the recorded frames show is a column coming apart roughly where it stood, not a cloud thrown sideways.
        // The mask is what makes it dissolve rather than fade (the CPU only cools the brightness ~30% over the whole phase - the mask is the death). A threshold rising with age eats the noise
        // field wherever it is thinnest, so the channel tears into small clumps that each shrink and go out on their own, and the hot middle of the strip survives longest because the profile
        // weights it. The field itself BOILS like a cloud detail texture: three octaves scrolling against each other at different convection speeds, so at any fixed age the wisps churn and
        // climb in place instead of freezing into a printed pattern. doc/atmo_magic_lightning_strike.md
        // Gated on the age alone, never on the LOD: the LOD only fades the fine octave, and a far bolt whose core has shrunk under a pixel would otherwise skip the mask entirely and leave a plasma copy that never dissolves at all.
        if (u > 0.0)
        {
            // <SS:Nexii> Ribbon space made near-isotropic BEFORE any noise reads it: one along unit is two core widths where one across unit is half of one, so the old 0.55 stretched every cell ~5x down the channel - the texture pulled along the whole bolt that made the dissolve read as one wave. And it is read on the SIGNED across, not the folded |across| the profile uses: folding mirrored every wisp about the centreline, which the eye picks out instantly as a printed symmetry.
            float sx = vary_texcoord0.x * 2.0 - 1.0;
            vec2 fuv = vec2(sx * 6.0, along * 10.4) + s;
            float t = ss_time;
            float f1 = ss_vnoise(fuv * 1.3 + vec2(0.0, t * 0.5));
            float f2 = ss_vnoise(fuv * 1.3 + vec2(4.7, -2.3 - t * 0.5));
            vec2 flow = vec2(f1 - 0.5, f2 - 0.5) * 2.0;
            flow.y += up_along * 1.25;

            vec2 muv = fuv + flow * (u * vary_ctl.z * 0.45);
            vec2 conv = vec2(0.0, up_along * t * 1.2);
            float n = ss_vnoise(muv * 1.7 + conv * 0.8 + vec2(t * 0.11, 0.0)) * 0.45
                    + ss_vnoise(muv * 3.9 + conv * 1.5 + vec2(-t * 0.19, 0.0) + 13.0) * 0.35
                    + ss_vnoise(muv * 8.3 + conv * 2.3 + vec2(t * 0.27, 0.0) + 37.0) * 0.20 * lod;
            n /= 0.80 + 0.20 * lod;

            // The narrow band is what makes clumps: each wisp holds its edge until the threshold reaches it, then shrinks and goes out on its own. The terminal smoothstep exists only because
            // the copy stops drawing at u 1: the few wisps the threshold never quite reaches must be gone before the geometry is.
            float keep = smoothstep(0.0, 0.14, n * (1.25 - 0.55 * across) - u * 0.80);
            keep *= smoothstep(1.0, 0.85, u);
            mask *= keep;

            vec3 air = ss_ramp4(RAMP_AIR0, RAMP_AIR1, RAMP_AIR2, RAMP_AIR3, u);
            vec3 gnd = ss_ramp4(RAMP_GND0, RAMP_GND1, RAMP_GND2, RAMP_GND3, u);
            col = mix(col, mix(air, gnd, w), smoothstep(0.0, 0.25, u));

            // The knot: the last of the amber foot outlives and outshines the wisps around it.
            float k = pow(keep, 3.0) * smoothstep(0.3, 0.8, w) * u;
            col = mix(col, KNOT_COLOR, 0.5 * k);
            bright *= 1.0 + 3.0 * k;
        }
    }

    if (mask < 2.0 / 255.0) discard;

    // Drawn additively, so colour IS brightness and the alpha is the bloom seed (screen alpha is the glow mask in this pass): rgb may run above white for the amber foot and the knot, clamped
    // so the tonemapper is not blown out; the seed is the per-element bloom fraction times the profile, never inflated by the HDR brightness.
    frag_color = vec4(min(col * bright * mask, vec3(3.0)), ss_glow * a8 * mask * min(bright, 1.0));
}
