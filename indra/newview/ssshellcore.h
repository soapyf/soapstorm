/**
 * @file ssshellcore.h
 * @brief Atmo Magic: the DOME SHELL's geometry - ray/shell intersection on both faces, the rim, the flat fallback, and the home body's own horizon reach. Header-only core. CONTRACT.
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

#ifndef SS_SHELLCORE_H
#define SS_SHELLCORE_H

// <SS:Nexii> A CORE header (lldefs.h, other ss*core.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_phase8_show.md section 5 (phase 8c, the horizon deck) and doc/atmo_magic_cloud_parallax.md. THIS IS THE AUTHORITY for every dome-shell geometry formula in the fork: cloudsF.glsl's ss_plane_base / ss_deck_edge_fade transliterate the bodies below statement for statement, and tests/twin_shell.cpp holds the two together. The mechanism was already shipping inline in cloudsF for the cirrus band; 8c needs a SECOND shell at the volumetric deck's own altitude, so the geometry moves here and the three constants that were calibrated to the cirrus band become arguments (Plane below) instead of blocking a second caller.
// <SS:Nexii> What "shell" means here: the layer is a SPHERE of radius (orbit + alt) centred on the home body, not a plane. orbit is the camera's distance from the body's centre (home radius + camera height, lldrawpoolwlsky.cpp) and alt is the layer's SIGNED height over the camera, so one function serves both faces - a deck seen from below (alt > 0, the plus root) and the same deck seen from above out of a sky build (alt < 0, the minus root). Every quantity is a pure function of (orbit, alt, ray); nothing here reads a setting, a clock or a frame.
// <SS:Nexii> [interaction: doc/atmo_magic_phase8_show.md section 5, "The reach, from the home body"] The horizon block at the end (tangentDistM / horizonReachM / curvatureDropM / dipAngleRad) is the 8c reach law and is NOT what the shader's rim melt runs on - see the honesty note above tangentSin: the shipped melt carries no refraction and ends the band at a rail well inside the geometric horizon. Both are here, named apart, because 8c's deck shell needs the reach law while the cirrus band must keep the rail it was tuned against.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSShell
{
    // The home body's constants. THE POINT OF CARRYING THEM: a track with a different planet is then a PARAMETER
    // (every function below takes the radius) and not a second formula - the doc's own rule for 8c.
    //
    // EARTH_RADIUS_M is the radius doc/atmo_magic_phase8_show.md section 5's tabulated numbers were computed
    // against (127 / 139 / 244 km of reach; 168 / 673 m of drop; 0.13 / 0.94 / 1.9 degrees of dip) - every one of
    // them reproduces to three figures at 6371 km and at no other radius, which unit_horizon_reach_spec_table and
    // unit_dip_angles_spec_table pin.
    //
    // VIEWER_DEFAULT_RADIUS_M is what the viewer ACTUALLY defaults to when a track authors no home body
    // (ssatmoenvapplier.cpp's SS_DEFAULT_PLANET_RADIUS_M). It is 5000 km, not Earth's 6371 km, although the comment
    // beside it calls it "an Earth-sized default"; doc/atmo_magic_cloud_parallax.md's own worked number (1.4 degrees
    // of tangent elevation for a 1500 m deck) is the 5000 km one. So the section 5 table describes a body 27%
    // larger than the one an unauthored track renders: the shipping reach is sqrt(5000/6371) = 0.886 of every
    // number in that table. Stated, not silently reconciled - unit_horizon_reach_default_body measures both.
    constexpr F32 EARTH_RADIUS_M          = 6371000.f;
    constexpr F32 VIEWER_DEFAULT_RADIUS_M = 5.0e6f;

    // Standard refraction: light bends toward the surface, so the geometric horizon is pushed out as if the body
    // were k times larger. 7/6 is the value doc/atmo_magic_phase8_show.md section 5 folds into its reach law.
    constexpr F32 REFRACTION_K = 7.f / 6.f;

    struct Vec2
    {
        F32 x;
        F32 y;
    };

    // THE SHELL'S CALIBRATED CONSTANTS, as arguments. Section 5's "what must be restructured first": three of these
    // were `const float` inside cloudsF's ss_plane_base, tuned against the cirrus band, and a second shell could not
    // call the same code while they were baked in. They are now one struct per shell instance.
    //
    // tileM (SS_DOME_TILE_M): metres of world per UV at the scale anchor. Pinned against the CIRRUS band's 6 km
    //   default height by doc/atmo_magic_cloud_parallax.md's own calibration (a 32 km tile reads as a 3x3-to-4x4
    //   repeat across the dome at that height). A deck shell's feature scale is the volumetric field's, not the
    //   cirrus layer's, so it must bring its own number.
    // parallaxDamp (SS_PARALLAX_DAMP): the world-anchored terms - camera travel and wind drift - run at this
    //   fraction of the plane-honest rate. The cirrus band keeps 0.125 because that is the rate the shipped vertex
    //   nudge moved at and the eye was tuned to it in a live viewer. THE DECK SHELL CANNOT TAKE IT (section 5): it
    //   has to agree with the TILED veil at the 10-14 km handoff, and the tiled veil is world-honest and
    //   drift-anchored, so a damped shell would slide against it as the camera moves. The deck shell runs
    //   UNDAMPED - see UNDAMPED below.
    // detailLoM / detailHiM (SS_DETAIL_LO_M/HI_M): where the fine octave gives up because perspective has
    //   compressed it under a degree. 100/250 km are the cirrus layer's rails at the cirrus layer's reach.
    // deckFold (SS_DECK_FOLD), throughLoM/throughHiM (SS_THROUGH_LO_M/HI_M), scaleAnchor (SS_SCALE_ANCHOR) and
    //   meltGain (the 1.6 in ss_deck_edge_fade) were never band-specific, but they live here too so that ONE struct
    //   describes a shell and no caller has to remember which of the eight it may set.
    struct Plane
    {
        F32 tileM;
        F32 scaleAnchor;
        F32 parallaxDamp;
        F32 deckFold;
        F32 throughLoM;
        F32 throughHiM;
        F32 detailLoM;
        F32 detailHiM;
        F32 meltGain;
    };

    // The cirrus band's values - TODAY'S NUMBERS, exactly the literals cloudsF.glsl carried inline before the
    // refactor. twin_shell.cpp proves the band's render is bit-identical through them (it transliterates the
    // pre-refactor GLSL as its control and SS_CHECK_BITS the two over a grid), and pins these nine against the
    // shader's own call site as text so a hand-edit to one side cannot silently diverge.
    constexpr Plane CIRRUS = { 32000.f, 0.25f, 0.125f, 0.1f, 40.f, 300.f, 100000.f, 250000.f, 1.6f };

    // The deck shell's damp, named so the reason travels with the number (section 5): world-honest, because the
    // shell must agree with the drift-anchored tiled veil where they hand over. The deck shell's OWN tileM and
    // detail rails come from the volumetric field's structure and are the deck-shell step's to author - that step
    // needs ssvolcloud.cpp, so this header deliberately does not guess them.
    constexpr F32 UNDAMPED = 1.0f;

    // GLSL smoothstep, with the harness's usual degenerate-span guard. Every span this core is called with is
    // wider than the guard (40..300 m, 100..250 km, 0.6 of a tangent sine), so the guard never changes a shipped
    // value; it only stops a caller with a zero span from producing a NaN.
    inline F32 smooth01(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / llmax(e1 - e0, 1e-6f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // GLSL step(edge, x).
    inline F32 step01(F32 edge, F32 x)
    {
        return (x < edge) ? 0.f : 1.f;
    }

    // THE DISCRIMINANT of the ray/shell intersection, and the one quantity every other geometric answer here is
    // derived from. Camera at distance a from the body's centre, shell radius r = a + alt, unit view ray whose
    // component along the local up is u. |C + t*d|^2 = r^2 expands to
    //
    //     t^2 + 2*a*u*t - (2*a*alt + alt*alt) = 0,      since r^2 - a^2 = 2*a*alt + alt*alt
    //
    // so t = -a*u +/- sqrt(a*a*u*u + 2*a*alt + alt*alt), and the bracket under the root is this function.
    //
    // The u fed in is the SHADER'S u, which is clamped to zero on the near face (alt >= 0): a down-ray under a
    // shell above the camera does meet that shell, but only after passing through the body, and planeFade has
    // already zeroed the layer for it. Clamping only ever raises u*u, and on that face the remaining terms are
    // non-negative anyway, so disc >= 0 identically - the near face cannot miss (unit_above_face_never_misses).
    inline F32 discriminant(F32 orbitM, F32 altM, F32 rayUp)
    {
        const F32 u = (altM >= 0.f) ? llmax(rayUp, 0.f) : rayUp;
        return orbitM * orbitM * u * u + 2.f * orbitM * altM + altM * altM;
    }

    // The FLAT fallback: what the mapping degenerates to when no home body is authored (orbit 0). Softened, not
    // clamped - (1+F)*|alt| / (|up| + F) is smooth in the ray everywhere, exact at the zenith, and caps the reach
    // at ~(1+F)/F layer-altitudes in the horizon fold. The hard max(up, 0.02) clamp an earlier cut shipped froze
    // the UVs into an azimuth-only stripe field below ~1.2 degrees and kinked the mip selection into a grid of tile
    // boundaries at the clamp line (doc/atmo_magic_cloud_parallax.md).
    //
    // With deckFold 0 this is exactly |alt| / |up|, which is the orbit -> infinity limit of shellReachM: the curved
    // reach -a*u + sqrt(a*a*u*u + 2*a*alt) equals a*u*(sqrt(1 + 2*alt/(a*u*u)) - 1) -> alt/u as a grows.
    // unit_flat_is_orbit_infinity_limit measures the convergence and carries the shipping orbit as its control.
    inline F32 flatReachM(F32 altM, F32 rayUp, F32 deckFold)
    {
        const F32 side = (altM >= 0.f) ? 1.f : -1.f;
        return (1.f + deckFold) * std::fabs(altM) / (llmax(rayUp * side, 0.f) + deckFold);
    }

    // THE INTERSECTION, both signs. Distance along the unit view ray to the shell at signed altitude altM over the
    // camera. Below the shell (alt >= 0) the near hit is the PLUS root; above it (alt < 0) it is the MINUS root -
    // the near face of the shell seen from above, which is the half a sky build looks down on. `side` carries the
    // sign of the root, so the two cases are one expression rather than two spellings.
    //
    // Off the shell entirely (disc < 0, which only the alt < 0 face can reach) the root is degenerate and this
    // returns -a*u, the distance to the closest approach. That value is finite, continuous and MEANINGLESS as a
    // layer distance; it is safe only because edgeFade has already taken the layer to zero everywhere disc < 0.
    // Callers that need to know must ask shellHits, not this.
    inline F32 shellReachM(F32 orbitM, F32 altM, F32 rayUp, F32 deckFold)
    {
        if (orbitM > 0.f)
        {
            const F32 side = (altM >= 0.f) ? 1.f : -1.f;
            const F32 u    = (altM >= 0.f) ? llmax(rayUp, 0.f) : rayUp;
            const F32 d    = discriminant(orbitM, altM, rayUp);
            return -orbitM * u + side * std::sqrt(llmax(d, 0.f));
        }
        return flatReachM(altM, rayUp, deckFold);
    }

    // Does this ray meet this shell on the shell's own face, ahead of the camera? TWO conditions, and only the
    // first is new: the discriminant must be non-negative (a ray flatter than the rim passes OVER a shell below the
    // camera and never touches it), and the ray must travel toward the face at all. On the flat fallback there is
    // no rim - a plane is unbounded - so only the second condition survives.
    inline S32 shellHits(F32 orbitM, F32 altM, F32 rayUp)
    {
        const F32 side = (altM >= 0.f) ? 1.f : -1.f;
        if (orbitM <= 0.f)
        {
            return (rayUp * side >= 0.f) ? 1 : 0;
        }
        if (discriminant(orbitM, altM, rayUp) < 0.f)
        {
            return 0;
        }
        return (rayUp * side >= 0.f) ? 1 : 0;
    }

    // THE RIM, DERIVED FROM THE DISCRIMINANT rather than mirrored off the near face's formula. Write the bracket as
    // disc(u) = a*a*u*u + (r*r - a*a): it is a parabola in u whose only root is |u| = sqrt(|r*r - a*a|)/a, and
    // r*r - a*a = 2*a*alt + alt*alt is exactly the term this function takes the magnitude of.
    //
    // The two faces read that root differently, and the difference is the whole of the alt < 0 fix:
    //  - alt >= 0 (shell above): r > a, the bracket is POSITIVE, disc never vanishes and every ray hits. The root
    //    is then not a miss boundary at all; it is the elevation sine at which the shell crosses the camera's own
    //    horizontal plane, i.e. the far edge of the layer as an eye-level observer sees it. That is the rail the
    //    band's melt has always run on and it is preserved bit-for-bit.
    //  - alt < 0 (shell below): r < a, the bracket is NEGATIVE, and its root is a genuine rim - rays with
    //    |u| below it have disc < 0 and pass over the shell entirely. This is why the below case needs a melt: at
    //    that sine the shell's near and far hits merge and the layer ends, exactly as it does above.
    //
    // HONESTY NOTE about the name. On the alt >= 0 face this is NOT the tangent from the eye to the layer - an eye
    // inside a sphere has no tangent to it - and the shipped melt therefore ends the cirrus band at a rail well
    // inside its geometric reach: for a 1500 m layer under a 5000 km body the melt is complete by ~51 km of ray
    // while the layer's own eye-plane crossing is at ~122 km. It is a hand-tuned look rail that happens to share
    // this algebra, and doc/atmo_magic_cloud_parallax.md's "tangent elevation" wording for it is loose. The 8c
    // reach law - the one that IS geometric - is the horizon block at the end of this header.
    inline F32 tangentSin(F32 orbitM, F32 altM)
    {
        const F32 q = 2.f * orbitM * altM + altM * altM;
        return std::sqrt(std::fabs(q)) / orbitM;
    }

    // The same rim as an ANGLE in the small-altitude limit, sqrt(2*|alt|/orbit) - the spelling both design docs
    // quote (about 1.4 degrees for a 1500 m deck under a 5000 km home planet). It drops the alt*alt term and the
    // arcsine, so it differs from tangentSin by parts in ten thousand at deck altitudes; unit_tangent_elevation
    // measures the gap rather than assuming it. Carries no refraction - see dipAngleRad for the refracted twin.
    inline F32 tangentElevRad(F32 orbitM, F32 altM)
    {
        return std::sqrt(2.f * std::fabs(altM) / orbitM);
    }

    // THE RIM FADE. The layer dissolves across the approach to its rim: zero AT the rim, full a meltGain multiple
    // of the rim sine away from it, so the edge reads as a curved cloud horizon melting into the atmosphere rather
    // than a seam or a circle.
    //
    // deckEdgeSin raises the melt's TOP on the near face only: it is the volumetric deck's perceived edge as an
    // elevation sine over the camera (lldrawpoolwlsky.cpp), and past that edge the band has no cloud in front of
    // it, so the melt spends the whole span between the deck's edge and the rim instead of running a flat sheet
    // into it. It has no meaning on the far face and is not applied there - the CPU gates it on deck_top_m > 0, so
    // it is zero by construction whenever a shell sits BELOW the camera, and the far face's own equivalent (the
    // deck's perceived edge looking down) is not computed anywhere today.
    //
    // THE alt < 0 BRANCH IS THE 8c FIX. Before it this function returned 1.0 for every ray whenever the layer sat
    // below the camera, so a deck seen from a sky build covered the entire lower hemisphere at full opacity -
    // including the whole cone of directions where, by the discriminant above, there is no shell at all. It now
    // melts on the DOWN-ness of the ray against the same rim, which puts the fade exactly on the boundary where
    // the intersection stops existing: alpha reaches 0 precisely as disc reaches 0. unit_below_rim_melt pins that
    // (and carries the old `return 1.0` as its failing control).
    inline F32 edgeFade(F32 orbitM, F32 altM, F32 rayUp, F32 deckEdgeSin, F32 meltGain)
    {
        if (orbitM <= 0.f)
        {
            return 1.f;
        }
        const F32 edge_dy = tangentSin(orbitM, altM);
        if (edge_dy <= 0.f)
        {
            return 1.f;
        }
        F32 melt_hi = edge_dy * meltGain;
        if (altM < 0.f)
        {
            return smooth01(edge_dy, melt_hi, -rayUp);
        }
        if (deckEdgeSin > melt_hi)
        {
            melt_hi = deckEdgeSin;
        }
        return smooth01(edge_dy, melt_hi, rayUp);
    }

    // How much of the layer survives the camera's own altitude. The mapping degenerates as the camera meets the
    // layer - a plane seen edge on has no finite reach - and the layer's own volume takes over exactly there, so
    // the shell dissolves across its last few hundred metres of separation. The step term drops rays heading away
    // from the layer's face.
    inline F32 planeFade(F32 altM, F32 rayUp, F32 throughLoM, F32 throughHiM)
    {
        const F32 side = (altM >= 0.f) ? 1.f : -1.f;
        return step01(0.f, rayUp * side) * smooth01(throughLoM, throughHiM, std::fabs(altM));
    }

    // The fine octave's distance give-up. Perspective compresses the layer toward its rim and the fine detail's
    // angular size collapses with it; past these rails the broad octave carries the far layer alone. The term the
    // fade multiplies is zero-mean, so this changes the far field's TEXTURE, never its coverage.
    inline F32 detailFade(F32 reachM, F32 loM, F32 hiM)
    {
        return 1.f - smooth01(loM, hiM, reachM);
    }

    // Everything one shell evaluation produces for one fragment.
    struct PlaneOut
    {
        Vec2 uv;
        F32  planeFade;
        F32  detailFade;
        F32  reachM;
    };

    // THE SHELL MAPPING, whole: intersect the true view ray with the shell, anchor the hit at the region centre,
    // subtract the wind travel, and divide by the pinned metres-per-UV. `ray` is the camera-relative unit view
    // direction in the dome mesh's Y-up local frame (renderDome's 120 degree permute: local y is world UP, local x
    // is world Y, local z is world X), which is why the horizontal components reach the layer as (ray.z, ray.x) -
    // east, north - matching regionOffsetM's (world X, world Y) order. The v axis is negated because world north
    // runs down the texture (the sign convention traced in doc/atmo_magic_cloud_parallax.md).
    //
    // `scale` is the layer's authored Scale-dial value and enters ONLY as a divisor pick: tileM * scale /
    // scaleAnchor, so an authored value equal to scaleAnchor is identity. The dial's continuous play is a
    // CROSSFADE between two endpoint scales at the call site, never an interpolated divisor - interpolating the
    // divisor drags every feature sideways as the tile zooms, which was the erratic-motion bug.
    inline PlaneOut planeBase(F32 orbitM, F32 altM, F32 rayX, F32 rayY, F32 rayZ, F32 scale,
                              Vec2 regionOffsetM, Vec2 driftM, const Plane& p)
    {
        PlaneOut o;
        o.planeFade  = planeFade(altM, rayY, p.throughLoM, p.throughHiM);
        o.reachM     = shellReachM(orbitM, altM, rayY, p.deckFold);
        o.detailFade = detailFade(o.reachM, p.detailLoM, p.detailHiM);

        const F32 deck_x  = rayZ * o.reachM;
        const F32 deck_y  = rayX * o.reachM;
        const F32 world_x = p.parallaxDamp * (regionOffsetM.x - driftM.x);
        const F32 world_y = p.parallaxDamp * (regionOffsetM.y - driftM.y);
        const F32 div     = p.tileM * scale / p.scaleAnchor;

        o.uv.x = (deck_x + world_x) / div;
        o.uv.y = (-deck_y - world_y) / div;
        return o;
    }

    // ---------------------------------------------------------------------------------------------------------
    // THE HORIZON REACH (doc/atmo_magic_phase8_show.md section 5, "The reach, from the home body"). This is the
    // GEOMETRIC law - the one 8c's deck shell is sized by - as opposed to the melt rail above, which is a look.
    // ---------------------------------------------------------------------------------------------------------

    // The tangent length from a point at height h over a body of radius R, with refraction folded in as an
    // effective radius k*R: sqrt(2*k*R*h). Negative heights are clamped to zero (a point below the surface has no
    // tangent length; the caller that could produce one is a deck authored under the datum).
    inline F32 tangentDistM(F32 radiusM, F32 heightM, F32 k)
    {
        return std::sqrt(2.f * k * radiusM * llmax(heightM, 0.f));
    }

    // THE REACH: how far a deck whose base sits at z_deck stays visible to an eye at height h_eye, both over a body
    // of radius R. It is the SUM of the two tangent lengths, and the two terms are why "how far is the horizon"
    // has no single answer: at avatar eye height the EYE's term is about 5 km - well inside the field the deck
    // already draws - while the DECK's term is over a hundred. Reading the naive single-tangent formula as the
    // layer's reach would have made the far veil worse, not better.
    //
    //     d = sqrt(2*k*R*h_eye) + sqrt(2*k*R*z_deck)
    inline F32 horizonReachM(F32 radiusM, F32 eyeHeightM, F32 deckHeightM, F32 k)
    {
        return tangentDistM(radiusM, eyeHeightM, k) + tangentDistM(radiusM, deckHeightM, k);
    }

    // The curvature drop at distance d: how far the body's surface (and anything riding it at constant altitude)
    // has fallen below the eye's horizontal ray. THE REASON 8c NEEDS NO CUTOFF RAIL - a deck of height z reaches
    // the eye plane where this equals z, which is exactly its own tangent distance, so geometry terminates the
    // layer and no authored constant has to.
    inline F32 curvatureDropM(F32 radiusM, F32 distM, F32 k)
    {
        return distM * distM / (2.f * k * radiusM);
    }

    // The DIP of the true horizon below eye level for an eye at height h: sqrt(2*h/(k*R)) radians, which is the
    // same quantity as tangentDistM/(k*R). Section 5b's sky-dimensionality step pivots the haze band, the
    // below-horizon mirror and the horizon clip about this line rather than about eye level. Unlike tangentElevRad
    // it CARRIES REFRACTION, because it describes where the eye actually sees the horizon.
    inline F32 dipAngleRad(F32 radiusM, F32 eyeHeightM, F32 k)
    {
        return std::sqrt(2.f * llmax(eyeHeightM, 0.f) / (k * radiusM));
    }
}

#endif // SS_SHELLCORE_H
