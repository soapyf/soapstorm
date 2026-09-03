/**
 * @file ssatmoenvplanetarystate.cpp
 * @brief See ssatmoenvplanetarystate.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssatmoenvplanetarystate.h"

#include "llquaternion.h"

#include <cmath>

// A point on an inclined circular orbit at a given phase, in ecliptic space.
LLVector3 SSAtmoEnvPlanetaryResolver::orbitOffset(F32 radius, F32 inclination_deg, F32 phase_deg)
{
    const F32 phase_rad = phase_deg * DEG_TO_RAD;
    const F32 incl_rad  = inclination_deg * DEG_TO_RAD;

    const F32 x = radius * cosf(phase_rad);
    const F32 y = radius * sinf(phase_rad) * cosf(incl_rad);
    const F32 z = radius * sinf(phase_rad) * sinf(incl_rad);
    return LLVector3(x, y, z);
}

// Places every body in ecliptic space: bound sun pairs about their barycenter, child suns around parents, all suns recentred on the system barycenter, then planets and moons.
std::vector<LLVector3> SSAtmoEnvPlanetaryResolver::resolveWorldPositions(const SSAtmoEnvPlanetary& planetary)
{
    const size_t n = planetary.mBodies.size();
    std::vector<LLVector3> positions(n, LLVector3::zero);
    std::vector<bool> resolved(n, false);

    auto placePair = [&](S32 senior, S32 junior, const LLVector3& centre)
    {
        const SSAtmoEnvCelestialBody& a_body = planetary.mBodies[senior];
        const SSAtmoEnvCelestialBody& b_body = planetary.mBodies[junior];
        const F32 total_mass = llmax(0.0001f, a_body.mMassRelative + b_body.mMassRelative);
        const F32 separation = b_body.mOrbitalRadius * planetary.mSunPlanetScale;

        const LLVector3 dir = orbitOffset(1.f, b_body.mOrbitalInclinationDeg,
                                          b_body.mOrbitalPhaseDeg);
        positions[senior] = centre - dir * (separation * b_body.mMassRelative / total_mass);
        positions[junior] = centre + dir * (separation * a_body.mMassRelative / total_mass);
        resolved[senior] = true;
        resolved[junior] = true;
    };

    auto isSun = [&](S32 i)
    {
        return i >= 0 && i < (S32)n
               && planetary.mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN;
    };
    auto pairJunior = [&](S32 i) -> S32
    {
        const S32 partner = planetary.mBodies[i].mBoundPartnerIndex;
        if (partner > i && partner < (S32)n && isSun(partner)
            && planetary.mBodies[partner].mBoundPartnerIndex == i)
        {
            return partner;
        }
        return -1;
    };

    for (S32 i = 0; i < (S32)n; ++i)
    {
        if (!isSun(i) || resolved[i]) continue;
        if (planetary.mBodies[i].mParentIndex >= 0) continue;

        const S32 junior = pairJunior(i);
        if (junior >= 0) placePair(i, junior, LLVector3::zero);
        else if (planetary.mBodies[i].mBoundPartnerIndex < 0)
        {
            positions[i] = LLVector3::zero;
            resolved[i] = true;
        }
    }

    for (S32 i = 0; i < (S32)n; ++i)
    {
        if (!isSun(i) || resolved[i]) continue;
        const S32 parent = planetary.mBodies[i].mParentIndex;
        if (!isSun(parent) || !resolved[parent]) continue;

        LLVector3 anchor = positions[parent];
        const S32 parent_partner = planetary.mBodies[parent].mBoundPartnerIndex;
        if (parent_partner >= 0 && parent_partner < (S32)n && resolved[parent_partner])
        {
            const SSAtmoEnvCelestialBody& pa = planetary.mBodies[parent];
            const SSAtmoEnvCelestialBody& pb = planetary.mBodies[parent_partner];
            const F32 total_mass = llmax(0.0001f, pa.mMassRelative + pb.mMassRelative);
            anchor = (positions[parent] * pa.mMassRelative
                      + positions[parent_partner] * pb.mMassRelative) / total_mass;
        }

        const SSAtmoEnvCelestialBody& body = planetary.mBodies[i];
        const LLVector3 centre = anchor
            + orbitOffset(body.mOrbitalRadius * planetary.mSunPlanetScale,
                          body.mOrbitalInclinationDeg, body.mOrbitalPhaseDeg);

        const S32 junior = pairJunior(i);
        if (junior >= 0) placePair(i, junior, centre);
        else
        {
            positions[i] = centre;
            resolved[i] = true;
        }
    }

    {
        LLVector3 weighted = LLVector3::zero;
        F32 total_mass = 0.f;
        for (S32 i = 0; i < (S32)n; ++i)
        {
            if (!isSun(i) || !resolved[i]) continue;
            weighted += positions[i] * planetary.mBodies[i].mMassRelative;
            total_mass += planetary.mBodies[i].mMassRelative;
        }
        if (total_mass > 0.f)
        {
            const LLVector3 barycenter = weighted / total_mass;
            for (S32 i = 0; i < (S32)n; ++i)
            {
                if (isSun(i) && resolved[i]) positions[i] -= barycenter;
            }
        }
    }

    for (S32 i = 0; i < (S32)n; ++i)
    {
        const SSAtmoEnvCelestialBody& body = planetary.mBodies[i];
        if (body.mKind != SSAtmoEnvCelestialBody::PLANET) continue;

        positions[i] = orbitOffset(body.mOrbitalRadius * planetary.mSunPlanetScale,
                                   body.mOrbitalInclinationDeg, body.mOrbitalPhaseDeg);
        resolved[i] = true;
    }

    for (S32 i = 0; i < (S32)n; ++i)
    {
        const SSAtmoEnvCelestialBody& body = planetary.mBodies[i];
        if (body.mKind != SSAtmoEnvCelestialBody::MOON || resolved[i]) continue;

        const S32 parent = planetary.effectiveParent(i);
        const LLVector3 anchor = (parent >= 0 && resolved[parent])
            ? positions[parent] : LLVector3::zero;

        positions[i] = anchor
            + orbitOffset(body.mOrbitalRadius * planetary.mPlanetMoonScale,
                          body.mOrbitalInclinationDeg, body.mOrbitalPhaseDeg);
        resolved[i] = true;
    }

    return positions;
}

// Every other body as seen from the home body: direction, distance and angular size.
std::vector<SSAtmoEnvResolvedBody> SSAtmoEnvPlanetaryResolver::resolveSky(const SSAtmoEnvPlanetary& planetary)
{
    std::vector<SSAtmoEnvResolvedBody> out;

    const S32 home_index = planetary.homeBodyIndex();
    if (home_index < 0) return out;

    const std::vector<LLVector3> positions = resolveWorldPositions(planetary);
    const LLVector3 home_pos = positions[home_index];

    for (size_t i = 0; i < planetary.mBodies.size(); ++i)
    {
        if ((S32)i == home_index) continue;

        LLVector3 offset = positions[i] - home_pos;
        const F32 distance = offset.magVec();
        if (distance < 0.0001f) continue;

        SSAtmoEnvResolvedBody resolved;
        resolved.mBodyIndex = (S32)i;
        offset.normVec();
        resolved.mDirection = offset;
        resolved.mDistance = distance;

        const F32 radius = planetary.mBodies[i].mDiameterM * 0.5f;
        resolved.mAngularDiameterDeg = RAD_TO_DEG * 2.f * atanf(llclamp(radius / distance, 0.f, 1.f));

        out.push_back(resolved);
    }

    return out;
}

// East/north/up axes for an observer at a latitude, in the home body's equatorial frame.
void SSAtmoEnvPlanetaryResolver::observerAxes(F32 latitude_deg, LLVector3& out_east,
                                              LLVector3& out_north, LLVector3& out_up)
{
    const F32 lat = llclamp(latitude_deg, -90.f, 90.f) * DEG_TO_RAD;
    const F32 c = cosf(lat);
    const F32 sn = sinf(lat);

    out_east.setVec(0.f, 1.f, 0.f);
    out_north.setVec(-sn, 0.f, c);
    out_up.setVec(c, 0.f, sn);
}

// Where a fixed ecliptic direction appears in the observer's sky at a spin phase - obliquity tilt, diurnal spin, then latitude frame.
LLVector3 SSAtmoEnvPlanetaryResolver::resolveObserverDirection(const LLVector3& ecliptic_dir,
                                                               F32 obliquity_deg,
                                                               F32 latitude_deg, F64 phase)
{
    LLQuaternion tilt;
    tilt.setAngleAxis(obliquity_deg * DEG_TO_RAD, LLVector3::x_axis);
    LLVector3 eq = ecliptic_dir * tilt;

    LLQuaternion spin;
    spin.setAngleAxis((F32)(-F_TWO_PI * phase), LLVector3::z_axis);
    eq = eq * spin;

    LLVector3 east, north, up;
    observerAxes(latitude_deg, east, north, up);

    LLVector3 out(eq * east, eq * north, eq * up);
    out.normalize();
    return out;
}

// Picks which emitters fill EEP's sun and moon slots: first found is sun unless a bigger one displaces it.
void SSAtmoEnvPlanetaryResolver::resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                                   const std::vector<SSAtmoEnvResolvedBody>& sky_bodies,
                                                   SSAtmoEnvResolvedBody& out_sun,
                                                   SSAtmoEnvResolvedBody& out_moon)
{
    out_sun = SSAtmoEnvResolvedBody();
    out_moon = SSAtmoEnvResolvedBody();

    if (planetary.homeBodyIndex() < 0) return;

    auto resolvedFor = [&sky_bodies](S32 body_index) -> const SSAtmoEnvResolvedBody*
    {
        for (const SSAtmoEnvResolvedBody& body : sky_bodies)
        {
            if (body.mBodyIndex == body_index) return &body;
        }
        return nullptr;
    };

    bool have_sun = false;
    for (const S32 emitter : planetary.lightEmitterIndices())
    {
        const SSAtmoEnvResolvedBody* resolved = resolvedFor(emitter);
        if (!resolved) continue;

        if (!have_sun)
        {
            out_sun = *resolved;
            have_sun = true;
        }
        else if (planetary.mBodies[static_cast<size_t>(emitter)].mDiameterM
                 > planetary.mBodies[static_cast<size_t>(out_sun.mBodyIndex)].mDiameterM)
        {
            out_moon = out_sun;
            out_sun = *resolved;
        }
        else
        {
            out_moon = *resolved;
        }
    }
}

// Convenience overload resolving the sky first.
void SSAtmoEnvPlanetaryResolver::resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                                   SSAtmoEnvResolvedBody& out_sun,
                                                   SSAtmoEnvResolvedBody& out_moon)
{
    resolveLightRoles(planetary, resolveSky(planetary), out_sun, out_moon);
}

namespace
{
    // Spin angle back to a 0..1 day phase.
    F64 ss_phase_from_theta(F32 theta)
    {
        F64 phase = std::fmod(-(F64)theta / F_TWO_PI, 1.0);
        if (phase < 0.0) phase += 1.0;
        return phase;
    }
}

// Reduces a body's daily path to sin-elevation = offset + amplitude*cos(theta - theta0), for closed-form rise/set math.
SSAtmoEnvDiurnalArc SSAtmoEnvPlanetaryResolver::diurnalArc(const LLVector3& ecliptic_dir,
                                                           F32 obliquity_deg, F32 latitude_deg)
{
    LLVector3 v = ecliptic_dir;
    v.normalize();

    LLQuaternion tilt;
    tilt.setAngleAxis(obliquity_deg * DEG_TO_RAD, LLVector3::x_axis);
    const LLVector3 u = v * tilt;

    LLVector3 east, north, w;
    observerAxes(latitude_deg, east, north, w);

    const LLVector3 z = LLVector3::z_axis;
    const F32 zu = z * u;
    const F32 zw = z * w;

    const F32 offset   = zu * zw;
    const F32 cos_term = (u * w) - offset;
    const F32 sin_term = (z % u) * w;

    SSAtmoEnvDiurnalArc arc;
    arc.mOffset    = offset;
    arc.mAmplitude = sqrtf(cos_term * cos_term + sin_term * sin_term);
    arc.mThetaZero = atan2f(sin_term, cos_term);
    return arc;
}

// Phase of highest (or lowest) elevation.
F64 SSAtmoEnvPlanetaryResolver::culminationPhase(const SSAtmoEnvDiurnalArc& arc, bool highest)
{
    return ss_phase_from_theta(highest ? arc.mThetaZero : (arc.mThetaZero + F_PI));
}

// Phase at which the arc crosses a given elevation, rising or setting; clamps to culmination when it never does.
bool SSAtmoEnvPlanetaryResolver::phaseForElevation(const SSAtmoEnvDiurnalArc& arc,
                                                   F32 sin_elevation, bool rising,
                                                   F64& out_phase)
{
    if (arc.mAmplitude < 1e-6f)
    {
        out_phase = 0.0;
        return false;
    }

    const F32 u = (sin_elevation - arc.mOffset) / arc.mAmplitude;
    if (u > 1.f)
    {
        out_phase = culminationPhase(arc, true);
        return false;
    }
    if (u < -1.f)
    {
        out_phase = culminationPhase(arc, false);
        return false;
    }

    const F32 delta = acosf(llclamp(u, -1.f, 1.f));
    out_phase = ss_phase_from_theta(rising ? (arc.mThetaZero + delta)
                                           : (arc.mThetaZero - delta));
    return true;
}

// The day phase that puts the body along a chosen sky direction - the editor's 'drag the sun here'.
F64 SSAtmoEnvPlanetaryResolver::phaseForSunDirection(const LLVector3& ecliptic_dir,
                                                     F32 obliquity_deg, F32 latitude_deg,
                                                     const LLVector3& target_dir)
{
    LLVector3 v = ecliptic_dir;
    LLVector3 t = target_dir;
    if (v.normalize() < 0.0001f || t.normalize() < 0.0001f) return 0.0;

    LLQuaternion tilt;
    tilt.setAngleAxis(obliquity_deg * DEG_TO_RAD, LLVector3::x_axis);
    const LLVector3 u = v * tilt;

    LLVector3 east, north, up;
    observerAxes(latitude_deg, east, north, up);
    const LLVector3 t_eq = east * t.mV[VX] + north * t.mV[VY] + up * t.mV[VZ];

    const LLVector3 z = LLVector3::z_axis;
    const F32 zu = z * u;
    const F32 zt = z * t_eq;

    const F32 cos_term = (u * t_eq) - zu * zt;
    const F32 sin_term = (z % u) * t_eq;

    if (cos_term * cos_term + sin_term * sin_term < 1e-12f)
    {
        return 0.0;
    }

    return ss_phase_from_theta(atan2f(sin_term, cos_term));
}

// Horizon crossings of the arc, when they exist.
bool SSAtmoEnvPlanetaryResolver::riseSetPhases(const LLVector3& ecliptic_dir,
                                               F32 obliquity_deg, F32 latitude_deg,
                                               F64& out_rise, F64& out_set)
{
    const SSAtmoEnvDiurnalArc arc = diurnalArc(ecliptic_dir, obliquity_deg, latitude_deg);

    F64 rise = 0.0, set = 0.0;
    if (!phaseForElevation(arc, 0.f, true, rise)) return false;
    if (!phaseForElevation(arc, 0.f, false, set)) return false;

    out_rise = rise;
    out_set = set;
    return true;
}
