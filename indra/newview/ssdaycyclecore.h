/**
 * @file ssdaycyclecore.h
 * @brief Atmo Magic: the day-cycle phase mapping and its inverse - one wall-clock second <-> one [0,1) day phase, shared by every track and by the storm scheduler's forced-cue conversion. Header-only core. CONTRACT.
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

#ifndef SS_DAYCYCLECORE_H
#define SS_DAYCYCLECORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath> only). 7b F3 (lesson 22): SSAtmoEnvTrack::dayCyclePhaseAt and
// ::wallTimeAtPhase were an fmod and its inverse spelled as shell arithmetic the harness could not compile against
// (PLAN.md lesson 2) - moved here VERBATIM from their old bodies (the tests agent pins this move bit-identical
// against the OLD ssatmoenvasset.cpp text). SSAtmoEnvTrack's two methods become one-line forwards that hand in
// their own mDayLengthSeconds/mDayOffsetSeconds; the storm scheduler's forced-cue conversion (SSStormCells::
// wallTimeAtPhase, ssstormcells.cpp) calls phaseAt/wallTimeAtPhase directly so an authored cue phase converts back
// to wall time through the identical formula it was read through, real or preview. Pure functions of their
// arguments only - no clock read, no track object.
#include "lldefs.h"

#include <cmath>

namespace SSDayCycle
{
    // The day-cycle phase at UTC second utc, given a day length and an offset: frac((utc - offsetS) / dayLenS).
    // Invariants: pure; 0 when dayLenS <= 0; result always in [0, 1); phaseAt(offsetS + k * dayLenS, dayLenS,
    // offsetS) == 0 for any integer k.
    inline F64 phaseAt(F64 utc, F64 dayLenS, F64 offsetS)
    {
        if (dayLenS <= 0.0) return 0.0;

        F64 t = fmod(utc - offsetS, dayLenS);
        if (t < 0.0) t += dayLenS;
        return t / dayLenS;
    }

    // The inverse of phaseAt - the UTC second nearest nearUtc whose phase (mod 1) equals phase. The instant with
    // this phase nearest phase 0 is offsetS + frac(phase) * dayLenS; every other instant sharing the phase is that
    // plus an integer number of day lengths, so round the offset from nearUtc to the nearest whole day length and
    // add it back. Invariants: phaseAt(wallTimeAtPhase(phase, nearUtc, dayLenS, offsetS), dayLenS, offsetS) ==
    // frac(phase) for any finite nearUtc and any dayLenS > 0; |wallTimeAtPhase(...) - nearUtc| <= 0.5 * dayLenS
    // (the nearest occurrence, never an arbitrary one); returns nearUtc unchanged when dayLenS <= 0; pure.
    inline F64 wallTimeAtPhase(F64 phase, F64 nearUtc, F64 dayLenS, F64 offsetS)
    {
        if (dayLenS <= 0.0) return nearUtc;

        F64 p = fmod(phase, 1.0);
        if (p < 0.0) p += 1.0;
        const F64 base = offsetS + p * dayLenS;
        const F64 k = std::round((nearUtc - base) / dayLenS);
        return base + k * dayLenS;
    }
}

#endif
