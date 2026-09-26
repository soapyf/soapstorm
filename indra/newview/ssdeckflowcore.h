/**
 * @file ssdeckflowcore.h
 * @brief Atmo Magic: the puff CARD'S OWN FRAME, the advected octave's SIGN RULE, its edge mask and its per-puff swirl. Header-only core. CONTRACT.
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

#ifndef SS_DECKFLOWCORE_H
#define SS_DECKFLOWCORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath>, <cstdint> only). THE FINDING IT EXISTS FOR (fifth build report: "all of the cloud puffs are exhibiting a weird flowing mask which is moving against the headwind", "the flow map is going in reverse direction", "the motion ... is not handling the different angles and instead seems to be picking from a few different presets which dont mash well together"): ssVolCloudF.glsl's SS_G2_BILLOW block built the puff's meridional frame from `ref = (abs(nrm.z) < 0.95) ? +Z : +X; tan_u = normalize(cross(ref, nrm)); tan_v = cross(nrm, tan_u)`, while ssvolcloud.cpp's render() builds the CARD the fragment is drawn on from a DIFFERENT construction - the view ray FLATTENED toward the pole over |n.z| in [0.6, 0.95], and `ref` BLENDED from +Z to +X over the same band. The two frames are the same only at the two ends of that band; in between they disagree, and the disagreement is not a small angle - it is a sign. Measured over a sweep of the sky (tests/unit_deckflow.cpp, frame_regimes): dot(card.right, shader.tan_u) is +1.0000 below 35 degrees of elevation, -1.0000 from 50 to 70 degrees due east/west, 0.0003 to 0.13 at 55-70 degrees due north/south, and +1.0000 again above 75 degrees where the hard 0.95 switch lands back on the blend's own endpoint. So the advected octave's "outward" ran OUTWARD near the horizon, SIDEWAYS in one azimuth band, and INWARD - against the drift the deck is travelling on - in another, with a hard flip across the 0.95 cone. Three qualitatively different motions in one sky, selected by where a puff happens to sit relative to the camera: "a few different presets which dont mash well together", and "the flow map is going in reverse direction", exactly.
// <SS:Nexii> THE RULE, stated once: THE FRAGMENT'S CARD FRAME IS THE CARD'S FRAME. Any per-fragment quantity that resolves the quad's own (p.x, p.y) into world directions - the flow's radial mapping here, and in principle the fake sphere's tangents - must be built by the SAME construction the CPU oriented the quad with, not by a second, independently spelled one that happens to agree at some view angles. cardFrame() below is that one construction; ssvolcloud.cpp's render() calls it and ssVolCloudF.glsl transliterates it. This is PLAN.md lesson 9 one level out: two spellings of one frame agree only where you tested them.
// <SS:Nexii> THE SIGN RULE, stated once (advectUV/featureWorldOffset/driftWorldOffset below): the deck's air frame is air = world - drift, and the builder places a puff at world = airCell + drift, so a FIXED PATTERN FEATURE - a texel of the noise map - is seen at world = texel + drift, i.e. it travels DOWNWIND at the drift's own rate. The advected octave must agree, and it does exactly when the sample point is uv - flow * phase: the texel at uv0 is then read at air uv0 + flow * phase, i.e. seen at world uv0 + drift + flow * phase. Feature offset +flow, sample-point offset -flow. Sampling at uv + flow * phase (reversedAdvectUV, the FAILING CONTROL kept below) runs the feature UPWIND of the frame that carries it.
// Bodies here are formulas; ssvolcloud.cpp drives them and ssVolCloudF.glsl transliterates cardFrame, the sign rule and the swirl rotation.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSDeckFlow
{
    struct Vec2 { F32 x = 0.f; F32 y = 0.f; };
    struct Vec3 { F32 x = 0.f; F32 y = 0.f; F32 z = 0.f; };

    inline Vec3 v3(F32 x, F32 y, F32 z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
    inline Vec3 cross3(const Vec3& a, const Vec3& b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    inline F32  dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline F32  len3(const Vec3& a) { return std::sqrt(dot3(a, a)); }
    inline Vec3 scale3(const Vec3& a, F32 s) { return v3(a.x * s, a.y * s, a.z * s); }
    inline Vec3 add3(const Vec3& a, const Vec3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }

    // ---------------------------------------------------------------------------------------- the card's own frame

    // The flatten band. A camera-facing quad seen from near the zenith or the nadir stands edge-on to the deck and
    // reads as a disc of card rather than as cloud, so render() turns it horizontal over this band. LO is where the
    // turn starts (|n.z| = 0.6, ~37 degrees of elevation), SPAN its width (to 0.95, ~72 degrees). These are the
    // numbers that were spelled in ssvolcloud.cpp's render(); they live here now because the fragment stage has to
    // spell the same ones (see THE RULE above), and a constant duplicated across a CPU/GLSL pair is a constant that
    // wants an owner.
    constexpr F32 CARD_FLATTEN_LO   = 0.6f;
    constexpr F32 CARD_FLATTEN_SPAN = 0.35f;

    // The cross-product length below which the frame is called degenerate and the caller's fallback is taken. The
    // CPU's own guard value, unchanged (LLVector3::normalize returns the pre-normalize length and render() tested it
    // against this).
    constexpr F32 FRAME_EPS = 0.001f;

    // cardFrame(toCam): the puff card's orientation, from the (not necessarily unit) vector from the puff to the
    // camera. `normal` is the card's plane normal - the view ray eased toward +-world-Z across the flatten band -
    // and `right`/`up` span the card, with `right` horizontal wherever the blended ref allows it. `degenerate` is
    // true when ref and normal are within FRAME_EPS of parallel, in which case `right` takes FALLBACK_RIGHT.
    // Invariants: normal/right/up are unit and mutually orthogonal; the result is a pure function of toCam's
    // DIRECTION (scaling toCam changes nothing - measured over 6552 sky directions, agreement 1.000000); normal ==
    // normalize(toCam) exactly when |normalize(toCam).z| <= CARD_FLATTEN_LO; normal == (0,0,sign(z)) exactly when
    // |normalize(toCam).z| >= CARD_FLATTEN_LO + CARD_FLATTEN_SPAN; every component is continuous in toCam except
    // across the degenerate direction below.
    // THE FALLBACK CHANGED, and this is the one behaviour difference against the code this replaces: render() used
    // the CAMERA'S OWN right vector, which a fragment stage cannot reproduce, so the two sides could not have been
    // made one frame while it stood. It is a fixed world axis now. What that costs, measured
    // (tests/unit_deckflow.cpp frame_near_degenerate): ref sweeps +Z -> +X across the same band the normal sweeps
    // toward +-Z, and the two become exactly parallel at ONE direction - 46.3355 degrees of elevation, bearing
    // exactly +X (and its nadir/-X mirror). |cross| falls below FRAME_EPS only within 0.0098 degrees of that
    // direction: a solid-angle share of 6.7e-7, i.e. a puff has to land inside a disc a hundredth of a degree
    // across for the fallback to be reached at all.
    // NEAR-DEGENERATE, stated because it is measured and NOT fixed here: the twist AROUND that direction is much
    // wider than the guard. |cross| is 0.0083 one degree of azimuth away and 0.166 at twenty, so `right` rotates
    // through most of a half turn over a few degrees of sky at 46.3 degrees of elevation due east - a small
    // pinwheel in the card orientations there. That is a property of the SHIPPED card geometry (the ref BLEND
    // rotating into the normal), not of this header; removing it means choosing a different ref law, which rotates
    // cards on screen everywhere and is a separate, visible change. Recorded so the next reader does not have to
    // rediscover it.
    constexpr F32 FALLBACK_RIGHT_X = 0.f;
    constexpr F32 FALLBACK_RIGHT_Y = 1.f;
    constexpr F32 FALLBACK_RIGHT_Z = 0.f;
    struct CardFrame
    {
        Vec3 normal;
        Vec3 right;
        Vec3 up;
        F32  flatten    = 0.f;
        bool degenerate = false;
    };

    inline CardFrame cardFrame(const Vec3& toCam)
    {
        CardFrame f;
        const F32 l = len3(toCam);
        Vec3 n = (l > FRAME_EPS) ? scale3(toCam, 1.f / l) : v3(0.f, 0.f, 1.f);

        f.flatten = llclamp((std::fabs(n.z) - CARD_FLATTEN_LO) / CARD_FLATTEN_SPAN, 0.f, 1.f);
        if (f.flatten > 0.f)
        {
            const F32 sgn = (n.z >= 0.f) ? 1.f : -1.f;
            Vec3 blended = add3(scale3(n, 1.f - f.flatten), scale3(v3(0.f, 0.f, sgn), f.flatten));
            const F32 bl = len3(blended);
            n = (bl > FRAME_EPS) ? scale3(blended, 1.f / bl) : v3(0.f, 0.f, sgn);
        }
        f.normal = n;

        Vec3 ref = add3(scale3(v3(0.f, 0.f, 1.f), 1.f - f.flatten), scale3(v3(1.f, 0.f, 0.f), f.flatten));
        const F32 rl = len3(ref);
        ref = (rl > FRAME_EPS) ? scale3(ref, 1.f / rl) : v3(0.f, 0.f, 1.f);

        Vec3 r = cross3(ref, n);
        const F32 crl = len3(r);
        f.degenerate = !(crl > FRAME_EPS);
        // FALLBACK_RIGHT is exactly perpendicular to the normal wherever this branch can be reached: ref has no y
        // component at any flatten, so ref and n can only be parallel when n itself lies in the xz plane.
        f.right = f.degenerate ? v3(FALLBACK_RIGHT_X, FALLBACK_RIGHT_Y, FALLBACK_RIGHT_Z) : scale3(r, 1.f / crl);
        f.up    = cross3(n, f.right);
        return f;
    }

    // <SS:Nexii> THE FAILING CONTROL for cardFrame, kept here rather than in the test because it is the SHIPPED
    // spelling and the rung's job is to show in numbers what it did: ssVolCloudF.glsl's SS_G2_BILLOW frame before
    // this header existed - a hard +Z/+X switch at 0.95 on the UNFLATTENED view ray. A test that wants to
    // demonstrate the reversal compares this against cardFrame over a sweep of the sky.
    inline CardFrame oldBillowFrame(const Vec3& toCam)
    {
        CardFrame f;
        const F32 l = len3(toCam);
        const Vec3 n = (l > FRAME_EPS) ? scale3(toCam, 1.f / l) : v3(0.f, 0.f, 1.f);
        f.normal = n;
        const Vec3 ref = (std::fabs(n.z) < 0.95f) ? v3(0.f, 0.f, 1.f) : v3(1.f, 0.f, 0.f);
        Vec3 r = cross3(ref, n);
        const F32 crl = len3(r);
        f.degenerate = !(crl > FRAME_EPS);
        f.right = f.degenerate ? v3(FALLBACK_RIGHT_X, FALLBACK_RIGHT_Y, FALLBACK_RIGHT_Z) : scale3(r, 1.f / crl);
        f.up    = cross3(n, f.right);
        f.flatten = 0.f;
        return f;
    }

    // ---------------------------------------------------------------------------------------------- the sign rule

    // advectUV(uv, flow, phase): the ONE sign site. See THE SIGN RULE at the top of this header. Invariants: the
    // identity at phase 0; linear in phase; advectUV(uv, flow, p) == advectUV(uv, -flow, -p).
    inline Vec2 advectUV(const Vec2& uv, const Vec2& flow, F32 phase)
    {
        Vec2 o; o.x = uv.x - flow.x * phase; o.y = uv.y - flow.y * phase; return o;
    }

    // featureWorldOffset(flow, phase): where the feature that sits at uv0 in the map is SEEN, relative to the frame
    // that carries it - the exact inverse of advectUV's displacement. This is the quantity that must agree in sign
    // with driftWorldOffset below. Invariants: == -(advectUV(0,flow,p) displacement); linear in phase.
    inline Vec2 featureWorldOffset(const Vec2& flow, F32 phase)
    {
        Vec2 o; o.x = flow.x * phase; o.y = flow.y * phase; return o;
    }

    // driftWorldOffset(windDir, speedMS, dtS): the GROUND TRUTH the sign rule is anchored on - how far the air frame
    // itself has carried everything in it, downwind, in dtS seconds. ssatmoenvapplier.cpp integrates exactly this
    // and ssvolcloud.cpp's builder adds it to every air-frame cell centre to get a world placement.
    // Invariants: parallel to windDir with a non-negative coefficient; zero at dtS 0.
    inline Vec2 driftWorldOffset(const Vec2& windDir, F32 speedMS, F32 dtS)
    {
        const F32 k = llmax(0.f, speedMS) * llmax(0.f, dtS);
        Vec2 o; o.x = windDir.x * k; o.y = windDir.y * k; return o;
    }

    // <SS:Nexii> THE FAILING CONTROL for the sign rule: sampling at uv + flow * phase. A test asserts that this
    // sends the feature the OTHER way from driftWorldOffset - i.e. that the sign in advectUV is load-bearing and
    // not a convention either way round.
    inline Vec2 reversedAdvectUV(const Vec2& uv, const Vec2& flow, F32 phase)
    {
        Vec2 o; o.x = uv.x + flow.x * phase; o.y = uv.y + flow.y * phase; return o;
    }

    // ---------------------------------------------------------------------------------------------- the edge mask

    // maskedReach(reach, rim): the advected octave's travel, faded out by the puff's OWN radial falloff - `rim` is
    // ssVolCloudF.glsl's `1.0 - smoothstep(SS_PUFF_RIM, 1.0, r)`, the same term the carve already multiplies the
    // density by, so the flow reaches zero exactly where the card's own silhouette does. WHAT THIS FIXES (fifth
    // build report, "looking quite strange due to lack of masking around edges"): the puff's silhouette is decided
    // in the band r in [SS_PUFF_RIM, 1] where rim ramps, and the flow used to run at FULL magnitude right through
    // that band, so the edge of the mask writhed by the whole advection distance while the interior only shuffled
    // its texture. Fading the travel with rim leaves the core boiling and holds the silhouette still.
    // NOT what it fixes, stated because the obvious reading is wrong: the flow was never able to draw the card's
    // square outline. rim is already zero for every r >= 1, and every point of the quad's boundary has r >= 1 (the
    // corners reach 1.414), so alpha was already exactly zero all the way round - measured, tests/unit_deckflow.cpp
    // perimeter_alpha. Whatever square boundaries a build shows are not this.
    // Invariants: 0 at rim 0; == reach at rim 1; monotone non-decreasing in rim; never exceeds reach for rim in [0,1].
    inline F32 maskedReach(F32 reach, F32 rim)
    {
        return reach * llclamp(rim, 0.f, 1.f);
    }

    // ------------------------------------------------------------------------------------------------- the swirl

    // The per-puff swirl: a CONTINUOUS angle, sampled from a SPATIALLY CORRELATED field at the puff's own air-frame
    // position, that rotates the billow's outflow azimuth within the card's plane. WHY A FIELD AND NOT A HASH
    // (fifth build report, "which dont mash well together or look in unison"): a per-puff hash gives every puff an
    // independent direction, and a cloud field's motion is correlated over hundreds of metres - neighbours lean the
    // same way. Value noise on a SWIRL_CELL_M lattice gives exactly that: continuous in position (so the histogram
    // of directions over the field has no clusters), and correlated inside one lattice cell (so near neighbours
    // agree and distant ones do not). SWIRL_CELL_M is three deck cells - large enough that a 260 m neighbour is
    // strongly correlated, small enough that the whole visible field is not one lean.
    // SWIRL_MAX_RAD is under pi/2 on purpose: the rotation must never turn the outflow into an inflow, so the
    // radial component keeps its sign at every value the field can produce (cos(0.9) = 0.62 > 0).
    constexpr F32 SWIRL_CELL_M  = 780.f;
    constexpr F32 SWIRL_MAX_RAD = 0.9f;
    constexpr U32 SWIRL_SALT    = 1319u;   // its own untaken slot, aliasing none of the builder's (977, and the jitter/radius slots below it)

    // hashUnit: the builder's own hash, character for character (ssvolcloud.cpp hashCell, ssVolCloudF.glsl
    // ss_hash_unit) - two's complement wrap in uint arithmetic, 24 bits of mantissa out.
    inline F32 hashUnit(S32 x, S32 y, U32 salt)
    {
        U32 h = (U32)x * 374761393u ^ (U32)y * 668265263u ^ salt * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        return (F32)(h & 0x00ffffffu) / 16777216.f;
    }

    // swirlUnit(airX, airY, salt): the correlated field, in [-1, 1]. Cubic-eased bilinear value noise on the
    // SWIRL_CELL_M lattice - the same octave shape the cell gate's clusterOctave uses on both sides, so nothing new
    // has to be replicated. Invariants: in [-1, 1]; continuous everywhere (no lattice seam); a pure function of the
    // AIR-frame position, so it carries no camera, no clock and no drift.
    inline F32 swirlUnit(F32 airX, F32 airY, U32 salt)
    {
        const F32 fx = airX / SWIRL_CELL_M;
        const F32 fy = airY / SWIRL_CELL_M;
        const S32 ix = (S32)std::floor(fx);
        const S32 iy = (S32)std::floor(fy);
        F32 tx = fx - (F32)ix;
        F32 ty = fy - (F32)iy;
        tx = tx * tx * (3.f - 2.f * tx);
        ty = ty * ty * (3.f - 2.f * ty);
        const F32 c00 = hashUnit(ix,     iy,     salt);
        const F32 c10 = hashUnit(ix + 1, iy,     salt);
        const F32 c01 = hashUnit(ix,     iy + 1, salt);
        const F32 c11 = hashUnit(ix + 1, iy + 1, salt);
        const F32 a = c00 + (c10 - c00) * tx;
        const F32 b = c01 + (c11 - c01) * tx;
        return (a + (b - a) * ty) * 2.f - 1.f;
    }

    // swirlAngleRad(unit): the field's value spent as an angle. Invariants: odd in unit; |result| <= SWIRL_MAX_RAD.
    inline F32 swirlAngleRad(F32 unit)
    {
        return llclamp(unit, -1.f, 1.f) * SWIRL_MAX_RAD;
    }

    // rotate2(p, angRad): the rotation the fragment stage applies to the quad point BEFORE the billow's radial
    // mapping. Rotating p rather than the world result is what keeps the swirl in the CARD'S plane at every view
    // angle, and it is free of consequence for the meridional field itself, which depends only on |p| (the ring
    // cores and the updraft are functions of r alone) - so this rotates the outflow's azimuth and nothing else.
    // Invariants: preserves length exactly in exact arithmetic; the identity at angRad 0.
    inline Vec2 rotate2(const Vec2& p, F32 angRad)
    {
        const F32 c = std::cos(angRad);
        const F32 s = std::sin(angRad);
        Vec2 o; o.x = c * p.x - s * p.y; o.y = s * p.x + c * p.y; return o;
    }

    // <SS:Nexii> THE FAILING CONTROL for the swirl's spatial correlation: an independent per-puff hash, which is
    // what a "give every puff its own angle" fix looks like when it is written without a field. A test asserts that
    // this has NO excess correlation between near neighbours over distant ones, where swirlUnit does.
    inline F32 uncorrelatedSwirlUnit(S32 cellX, S32 cellY, U32 salt)
    {
        return hashUnit(cellX, cellY, salt) * 2.f - 1.f;
    }
}

#endif
