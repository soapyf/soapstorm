/**
 * @file ssatmoenvplanetarystate.h
 * @brief Atmo Magic: celestial body resolver.
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

#ifndef SS_ATMOENVPLANETARYSTATE_H
#define SS_ATMOENVPLANETARYSTATE_H

#include "ssatmoenvasset.h"
#include "v3math.h"

#include <vector>

struct SSAtmoEnvResolvedBody
{
    S32 mBodyIndex = -1;
    LLVector3 mDirection;
    F32 mAngularDiameterDeg = 0.f;
    F32 mDistance = 0.f;
};

struct SSAtmoEnvDiurnalArc
{
    F32 mOffset = 0.f;
    F32 mAmplitude = 0.f;
    F32 mThetaZero = 0.f;
};

class SSAtmoEnvPlanetaryResolver
{
public:
    static std::vector<LLVector3> resolveWorldPositions(const SSAtmoEnvPlanetary& planetary);

    static std::vector<SSAtmoEnvResolvedBody> resolveSky(const SSAtmoEnvPlanetary& planetary);

    static LLVector3 resolveObserverDirection(const LLVector3& ecliptic_dir,
                                              F32 obliquity_deg, F32 latitude_deg,
                                              F64 phase);

    static void resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                  const std::vector<SSAtmoEnvResolvedBody>& sky_bodies,
                                  SSAtmoEnvResolvedBody& out_sun,
                                  SSAtmoEnvResolvedBody& out_moon);

    static void resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                  SSAtmoEnvResolvedBody& out_sun,
                                  SSAtmoEnvResolvedBody& out_moon);

    static SSAtmoEnvDiurnalArc diurnalArc(const LLVector3& ecliptic_dir,
                                          F32 obliquity_deg, F32 latitude_deg);

    static F64 culminationPhase(const SSAtmoEnvDiurnalArc& arc, bool highest);

    static bool phaseForElevation(const SSAtmoEnvDiurnalArc& arc,
                                  F32 sin_elevation, bool rising, F64& out_phase);

    static F64 phaseForSunDirection(const LLVector3& ecliptic_dir,
                                    F32 obliquity_deg, F32 latitude_deg,
                                    const LLVector3& target_dir);

    static bool riseSetPhases(const LLVector3& ecliptic_dir,
                              F32 obliquity_deg, F32 latitude_deg,
                              F64& out_rise, F64& out_set);

private:
    static void observerAxes(F32 latitude_deg, LLVector3& out_east,
                             LLVector3& out_north, LLVector3& out_up);

    static LLVector3 orbitOffset(F32 radius, F32 inclination_deg, F32 phase_deg);
};

#endif
