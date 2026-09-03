/**
 * @file ssCelestialF.glsl
 * @brief Atmo Magic: celestial discs - sphere phase shading, eclipse
 *        dimming, and emissive bodies. See ssCelestialV.glsl.
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

// <SS:Nexii> Atmo Magic celestial discs

out vec4 frag_data[4];

uniform sampler2D diffuseMap;

// Per-body state. Everything that varies from one disc to the next, and nothing that does not - the look constants are hardcoded below. Tint. Usually white: see the note in lldrawpoolwlsky.cpp on
// why the sky's own interpolated body colour is NOT what arrives here.
uniform vec4 ss_disc_color;
uniform vec3 ss_body_dir;        // unit, observer -> body
uniform vec3 ss_sun_dir;         // unit, body -> its star
uniform vec3 ss_quad_right;      // quad's +u axis, world space
uniform vec3 ss_quad_up;         // quad's +v axis, world space
uniform float ss_sunlight;       // 0 eclipsed, 1 in open sunlight
uniform float ss_emissive;       // 1 the body makes its own light
uniform float ss_phase_shaded;   // 1 shade it as a sphere
uniform float ss_daylight;       // 0 the observer is in night, 1 in full day
uniform vec2 ss_face_rot;        // (cos, sin) of the body's roll on its quad
uniform float ss_disc_fraction;  // the art's disc as a fraction of the quad (1 = full-bleed)

in vec2 vary_texcoord0;

// How much brighter than its art an emissive disc is drawn. Enough to clear the haze glow around it and reach the bloom threshold: a disc that merely matched its own glow still reads as a hole in
// the sky, which is what a plain textured sun looked like against EEP's scattering.
const float SS_EMISSIVE_GAIN = 4.0;

// How lit the unlit side is left, at its very best: a new moon, at night. Not zero - a new moon is not a hole in the sky, it is a disc lit by the light its own planet throws back at it. Generous at
// 0.06. Real earthshine is a fraction of a percent of the sunlit side, and rendering it at that would be rendering nothing: it is visible to us at all only because an eye adapted to a dark sky has
// enormous range, and none of that survives being written into a frame buffer. So the value is what it takes to read as a faintly lit disc rather than a bite out of the sky. Which is exactly why it
// cannot be left standing in every other case. Held at 0.06 through the day it becomes a real error: the unlit part of a daytime moon is not dim, it is GONE - the sky is thousands of times brighter
// than earthshine, so a photograph shows a lit crescent with nothing beside it, its dark side indistinguishable from the blue around it. Adding the disc to the sky gets most of the way there on its
// own; the gates below close the rest, because the exaggeration that makes the night case readable is the one thing standing in the way of the day case.
const float SS_EARTHSHINE = 0.06;

// The phase this side of which earthshine is not worth drawing, as a fraction of the PLANET's lit face seen from the body. Half - so a half moon shows none of it, and only crescents do.
const float SS_EARTHSHINE_ONSET = 0.5;

// How much brighter a reflecting body is drawn than its own art. Modest, because lunar art is a PHOTOGRAPH of a correctly exposed full moon - mid-grey maria, near-white highlands - not a measurement
// of its 0.12 albedo. It already is the moon as an eye sees it, so it needs lifting only enough to sit clearly above a twilight sky rather than rebuilding from rock.
const float SS_LUNAR_GAIN = 1.5;

// Terminator softness, in cosine either side of the boundary. A hard N.L cut on a disc a few dozen pixels across reads as a bite taken out of it; real ones are softened by the star's angular size
// anyway.
const float SS_TERMINATOR_SOFT = 0.15;

// Aerial perspective, done by ADDING the disc to the sky rather than by mixing the two: seen = own * T + airlight,   drawn as  dst + src * T The whole atmosphere is in front of a celestial body -
// there is no air behind the moon - so the airlight over its disc is the entire column, which is precisely the sky already drawn there. Adding to it is therefore not an approximation of the physics,
// it IS the physics, and it costs a blend mode instead of a second copy of the haze model. It also fixes the thing every previous attempt here got wrong. A disc composited as `own * T + haze * (1 -
// T)` replaces the sky it covers, so its dark parts are dark: the maria came out as grey patches sitting in front of a bright sky, and the whole moon read as a sticker. Added, the dark parts
// contribute nothing and the sky simply shows at full strength - which is why a daytime half-moon's unlit half is not a dark half, it is no half at all, indistinguishable from the sky around it.
// Nothing in the sky can ever be darker than the sky, and additive is what makes that true by construction instead of by tuning. The pass sets BT_ADD_WITH_ALPHA for this - see renderHeavenlyBodies.
// The attachments this shader does not contribute to are written as zero so the same add leaves them exactly as the sky dome left them, which is how the G-buffer flags survive being blended.

// Per-channel extinction through one airmass. Blue scatters out hardest, which is why a low body is orange and a high one keeps its own colour.
const vec3 SS_EXTINCTION = vec3(0.06, 0.13, 0.28);

// How much air a body's light comes through, as a multiple of straight up. The real relation runs away at the horizon; this is the standard 1/sin with a floor, which tops out around 12 airmasses -
// enough to redden a setting sun hard without driving it to black.
float ss_airmass(float sin_alt)
{
    return 1.0 / max(sin_alt, 0.085);
}

void main()
{
    // The art, turned by the body's roll on the quad - see ss_face_rot in lldrawpoolwlsky.cpp. About the disc's centre, so the disc itself is unmoved (a circle rotated about its middle is the same
    // circle) and only the features on it turn. The SURFACE only. Everything geometric below - the sphere normal, and so the terminator - keeps using the raw texcoord, because that is the quad's
    // actual shape in the world and rolling the art does not change where the quad is. Rotating both would turn the terminator with the maria and leave the lit side no longer facing the sun.
    vec2 uv = vary_texcoord0.xy - 0.5;
    uv = vec2(uv.x * ss_face_rot.x - uv.y * ss_face_rot.y,
              uv.x * ss_face_rot.y + uv.y * ss_face_rot.x) + 0.5;

    // A rotated corner samples outside the art. Those corners are the quad's transparent margin either way, and dropping them is cheaper and surer than depending on how the sampler was left clamped.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        discard;
    }

    vec4 c = texture(diffuseMap, uv);

    // The stock moon art carries transparent pixels at <0x55,0x55,0x55,0x00>; dropping them rather than blending keeps the quad's corners from hazing over whatever is behind them. This threshold is
    // also what decides which pixels OCCLUDE, and it has to stay tied to the art's alpha rather than to how bright the result comes out. The pass writes depth (see renderHeavenlyBodies) and the
    // stars are drawn after it, so a disc hides the stars behind it by failing their depth test - nothing to do with the colour blend. A new moon's dark limb adds no light at all yet is still a
    // solid body: it has full alpha here, so it still writes depth, and the stars behind it still go out. Discarding on luminance instead would make the night sky show straight through the moon.
    if (c.a <= 2.0 / 255.0)
    {
        discard;
    }

    c.rgb *= ss_disc_color.rgb;

    // How much atmosphere this body is being seen through. Its own elevation is the whole of it - a body overhead is one airmass, one on the horizon is a dozen.
    float airmass = ss_airmass(max(ss_body_dir.z, 0.0));
    vec3 transmittance = exp(-SS_EXTINCTION * airmass);

    if (ss_emissive > 0.5)
    {
        // A star is the light: no terminator, no eclipse, and nothing the atmosphere does to it either. Not because that is physically true - a setting sun really is reddened and dimmed - but
        // because it is not what anyone SEES. The disc stays far past what an eye can hold right down to the horizon, so it reads as flat bright white throughout (sunset photographs agree: the
        // core clips white with the colour living in the sky around it), and every attempt here to model the physics - including a briefly-shipped horizon-only extinction fade - made it worse.
        // What changes at sunset is the SKY, and that is where the work belongs.
        c.rgb *= SS_EMISSIVE_GAIN;
    }
    else
    {
        // Shaded as the sphere the disc stands in for. The billboard already carries the normal implicitly: the disc IS the projection of a sphere, so the UV gives it. Padded art puts that sphere
        // in the central ss_disc_fraction of the quad, so the coordinate is normalised into the art's disc before it maps a normal - with full-bleed art the fraction is 1 and this is the raw
        // coordinate, bit for bit.
        if (ss_phase_shaded > 0.5)
        {
            vec2 p = (vary_texcoord0.xy * 2.0 - 1.0) / max(ss_disc_fraction, 0.001);
            float r2 = min(dot(p, p), 1.0);

            // At the centre of the disc the surface faces the observer, which is -ss_body_dir; the rest is the quad's own two axes.
            vec3 n = normalize(ss_quad_right * p.x
                             + ss_quad_up * p.y
                             + (-ss_body_dir) * sqrt(1.0 - r2));

            float lit = smoothstep(-SS_TERMINATOR_SOFT, SS_TERMINATOR_SOFT,
                                   dot(n, ss_sun_dir));

            // Earthshine, gated by the two things that actually govern it. A dark sky, first - see SS_EARTHSHINE. Then the body's own phase, because earthshine is the light of a PLANET, and that
            // planet has a phase too - exactly the complement of this one. Seen from a full moon the Earth is new and lights nothing; seen from a new moon it is full and throws back enough to fill
            // the whole unlit face. That is why the old moon in the new moon's arms is a thin crescent's companion and never a half's. dot(sun_dir, body_dir) carries the geometry directly: +1 when
            // the star is behind the body (this body new, its planet full), -1 when the star is behind the observer (this body full, its planet new). Taken through a threshold rather than straight,
            // though. The planet's lit fraction is linear in that dot, but what can be SEEN of the light it sends back is not: the wider this body's own crescent grows, the more its glare drowns the
            // ash-grey beside it, in an eye and in a lens alike. Half is where that has already won.
            float planet_phase = (1.0 + dot(ss_sun_dir, ss_body_dir)) * 0.5;
            float earthshine = SS_EARTHSHINE * (1.0 - ss_daylight)
                             * smoothstep(SS_EARTHSHINE_ONSET, 1.0, planet_phase);
            c.rgb *= mix(earthshine, 1.0, lit);
        }

        // ...and dimmed by whatever its planet's shadow leaves of the light reaching it at all.
        c.rgb *= ss_sunlight;

        // Lifted out of its own albedo - see SS_LUNAR_GAIN. Before the atmosphere gets at it, so a low moon still washes toward the haze rather than staying stubbornly bright.
        c.rgb *= SS_LUNAR_GAIN;
    }

    // Extinction, for reflecting bodies only, and taken literally now. Literally, because the failure it used to cause cannot happen any more. Full extinction once made a horizon moon come out
    // darker than the sky in front of it - impossible, and the reason a fudge factor sat here pulling most of the dimming back out. Adding the disc to the sky makes that structural: however hard the
    // air dims a moon, the worst it can reach is contributing nothing, and a moon that contributes nothing is a moon that has become the sky. Which is what a moon lost in daylight haze actually
    // does. A star skips this for its own reasons - see the emissive branch.
    if (ss_emissive <= 0.5)
    {
        c.rgb *= transmittance;
    }


    // Zero, not the flag: this pass ADDS, so anything written here would be added to what the sky dome already put in the G-buffer. The sky has written SKIP_ATMOS across every pixel a disc can land
    // on - the discs are drawn after the haze dome and only ever over it - so contributing nothing leaves exactly the right flag in place, where contributing the flag itself would double it.
    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0);
    frag_data[2] = vec4(0);

#if defined(HAS_EMISSIVE)
    frag_data[3] = c;
#else
    frag_data[0] = c;
#endif
    // c.a stays as the art's own: the blend multiplies the contribution by it, so a soft disc edge fades into the sky instead of ending on one.
}

