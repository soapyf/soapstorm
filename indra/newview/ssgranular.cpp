/**
 * @file ssgranular.cpp
 * @brief Atmo Magic: granular transport - lift, creep, deposit and spill on the surface field.
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

#include "ssgranular.h"

#include "llmath.h"

#include <algorithm>
#include <cmath>

// How far a slope walk may reach for a neighbour height before it counts as a wall - the
// settle path in sssurfacefield.cpp carries the same figure as SLOPE_STEP_MAX. Kept here rather
// than shared so the transport stays free of that file's internals.
static const F32 GRANULAR_SLOPE_STEP_MAX = 3.f;

// A step may never move more than this fraction of a cell's depth, however hard the wind blows -
// a CFL-style cap against one gust overshooting the store and machine-gunning the cascades
// (doc/atmo_magic_snow.md, granular runoff).
static const F32 CREEP_CFL_CAP = 0.25f;

// Below this depth a cell is dust, not a drift; lift has nothing to grip.
static const F32 MIN_DRIFT_SNOW = 1.0e-4f;

namespace
{
    F32 smooth01(F32 x)
    {
        x = llclamp(x, 0.f, 1.f);
        return x * x * (3.f - 2.f * x);
    }

    // Cell's depth-ceiling scale over terrain: 1 at grade, easing to the params' structure
    // fraction from half the structure height up. Same grade-vs-structure reading the settle
    // path takes from the same capture channel, so a drift and its snowfall agree on how much
    // a tower deck may hold.
    F32 structScale(const SSSurfaceField::Geometry& geom, size_t i, const SSGranularParams& p)
    {
        const F32 h = llmax(p.mStructAboveH, 1.f);
        const F32 t = smooth01((geom.above(i) - h * 0.5f) / (h * 0.5f));
        return lerp(1.f, llclamp(p.mStructDepth, 0.f, 1.f), t);
    }
}

// A cell of this slope's share of its ceiling - the settle path's lieHere() standalone, so
// deposit and creep pile against the same repose rule settle does.
F32 SSGranular::roomAt(const SSSurfaceField::Geometry& geom, S32 index, F32 ceiling, F32 repose_rad)
{
    const S32 n = geom.mN;
    if (n < 1 || index < 0 || index >= n * n) return 0.f;

    const S32 x = index % n;
    const S32 y = index / n;
    const F32 z0 = geom.mZ[index];
    const F32 limit = geom.mCell * GRANULAR_SLOPE_STEP_MAX;

    auto slopeAlong = [&](S32 lo, S32 hi, bool have_lo, bool have_hi)
    {
        F32 d_lo = 0.f, d_hi = 0.f;
        bool ok_lo = false, ok_hi = false;
        if (have_lo && geom.solid(lo) && fabsf(geom.mZ[lo] - z0) < limit)
        {
            d_lo = z0 - geom.mZ[lo];
            ok_lo = true;
        }
        if (have_hi && geom.solid(hi) && fabsf(geom.mZ[hi] - z0) < limit)
        {
            d_hi = geom.mZ[hi] - z0;
            ok_hi = true;
        }
        if (ok_lo && ok_hi) return (d_lo + d_hi) * 0.5f / geom.mCell;
        if (ok_lo) return d_lo / geom.mCell;
        if (ok_hi) return d_hi / geom.mCell;
        return 0.f;
    };

    const F32 gx = slopeAlong(index - 1, index + 1, x > 0, x < n - 1);
    const F32 gy = slopeAlong(index - n, index + n, y > 0, y < n - 1);
    const F32 angle = atanf(sqrtf(gx * gx + gy * gy));

    return ceiling * llclamp(1.f - angle / llmax(repose_rad, 0.01f), 0.f, 1.f);
}

// One fixed step. Stages run as separate grid passes - order-independent, cache friendly, and
// honest about what each consumes: lift reads flow and depth, creep reads lift and depth,
// deposit reads flow and the room the repose rule grants.
void SSGranular::step(SSSurfaceField::Field& fld, const SSSurfaceField::Geometry& geom,
                      const SSGranularParams& p, F32 dt)
{
    const S32 n = geom.mN;
    const size_t cells = (size_t)n * n;
    if (n < 1 || !geom.valid() || fld.mSnow.size() != cells || !p.mFlow) return;

    if (fld.mLift.size() != cells)   fld.mLift.assign(cells, 0.f);
    if (fld.mInflow.size() != cells) fld.mInflow.assign(cells, 0.f);

    // Wet snow does not blow: the gate runs to zero across the last degree and a half.
    const F32 temp_scale = llclamp(1.f - p.mTemperatureC / llmax(p.mLiftTemp, 0.1f), 0.f, 1.f);
    const F32 deposit_gate = p.mLiftLo * llclamp(p.mDepositGap, 0.1f, 1.f);

    // Lift. Always computed while a flow grid exists - the drift tier's spawn walk reads this
    // figure even when the preset's rates are zero - plus the erosion it pays for when asked.
    for (size_t i = 0; i < cells; ++i)
    {
        F32 lift = 0.f;
        if (geom.solid(i) && !geom.water(i) && fld.mSnow[i] > MIN_DRIFT_SNOW)
        {
            const LLVector4& f = p.mFlow[i];
            const F32 speed = sqrtf(f.mV[0] * f.mV[0] + f.mV[1] * f.mV[1]);
            if (speed > p.mLiftLo)
            {
                const F32 band = smooth01((speed - p.mLiftLo) / llmax(p.mLiftHi - p.mLiftLo, 0.1f));
                lift = band * temp_scale;
            }
        }
        fld.mLift[i] = lift;

        // Exponential decay, not a fixed flux. A hard rate would strip a cell to zero in one
        // step at gale strength, flickering the lift figure as the cell oscillates between snow
        // and bare (observed: orange tiles strobing, lift cells vanishing under the spawn walk's
        // own floor). Proportional erosion leaves a thin residual at equilibrium - settle divided
        // by the decay constant - so a gale keeps a dusting on the ground and the air carries
        // the rest: a ground blizzard.
        if (lift > 0.f && p.mLiftRate > 0.f && p.mGust > 0.f)
        {
            const F32 k = p.mLiftRate * 10.f * lift * p.mGust;
            fld.mSnow[i] -= fld.mSnow[i] * (1.f - expf(-k * dt));
        }
    }

    // Creep. A downwind exchange on the flow's dominant axis - the on-grid stand-in for
    // continuous advection, good enough at these cell sizes and impossible to overshoot. Mass
    // arriving at an eave cell bypasses the field and feeds the shed store: one ledger, debited
    // here, paid out as cascades by the shed cursor.
    if (p.mCreepRate > 0.f && p.mGust > 0.f)
    {
        std::fill(fld.mInflow.begin(), fld.mInflow.end(), 0.f);

        for (S32 y = 0; y < n; ++y)
        {
            for (S32 x = 0; x < n; ++x)
            {
                const S32 i = y * n + x;
                if (fld.mLift[i] <= 0.f || fld.mSnow[i] <= MIN_DRIFT_SNOW) continue;

                const LLVector4& f = p.mFlow[i];
                if (fabsf(f.mV[0]) + fabsf(f.mV[1]) < 0.05f) continue;

                S32 dx = 0, dy = 0;
                if (fabsf(f.mV[0]) > fabsf(f.mV[1])) dx = (f.mV[0] > 0.f) ? 1 : -1;
                else                                 dy = (f.mV[1] > 0.f) ? 1 : -1;

                const S32 nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                const S32 j = ny * n + nx;
                const F32 flux = llmin(fld.mSnow[i] * CREEP_CFL_CAP,
                                       p.mCreepRate * fld.mLift[i] * fld.mSnow[i] * p.mGust * dt);
                if (flux <= 0.f) continue;

                fld.mSnow[i] -= flux;
                if (geom.solid(j) && geom.mEdge[j])
                {
                    fld.mStore[j] += flux;
                }
                else
                {
                    fld.mInflow[j] += flux;
                }
            }
        }

        for (size_t i = 0; i < cells; ++i)
        {
            if (fld.mInflow[i] > 0.f && geom.solid(i) && !geom.water(i))
            {
                fld.mSnow[i] += fld.mInflow[i];
            }
        }

        // Slump: anything past its repose room sheds one cell downwind (or into the store at an
        // eave) per step - a drift's multi-stage pour over a lip is this pass iterating.
        for (S32 y = 0; y < n; ++y)
        {
            for (S32 x = 0; x < n; ++x)
            {
                const S32 i = y * n + x;
                if (!geom.solid(i) || geom.water(i)) continue;

                const F32 room = roomAt(geom, i, p.mSnowDepth * structScale(geom, i, p), p.mReposeRad);
                const F32 over = fld.mSnow[i] - room;
                if (over <= MIN_DRIFT_SNOW) continue;

                const LLVector4& f = p.mFlow[i];
                if (fabsf(f.mV[0]) + fabsf(f.mV[1]) < 0.05f) continue;

                S32 dx = 0, dy = 0;
                if (fabsf(f.mV[0]) > fabsf(f.mV[1])) dx = (f.mV[0] > 0.f) ? 1 : -1;
                else                                 dy = (f.mV[1] > 0.f) ? 1 : -1;

                const S32 nx = x + dx, ny = y + dy;
                const S32 j = (nx >= 0 && ny >= 0 && nx < n && ny < n) ? ny * n + nx : -1;

                const bool spill = (j >= 0 && geom.solid(j) && geom.mEdge[j]);
                if (spill)
                {
                    fld.mStore[j] += over;
                    fld.mSnow[i] = room;
                }
                else if (j >= 0 && geom.solid(j) && !geom.water(j))
                {
                    fld.mSnow[i] -= over;
                    fld.mInflow[j] += over;
                }
                // nowhere to go (region border, open cell): hold the surplus; future settle's
                // room cap keeps it from growing without bound
            }
        }

        for (size_t i = 0; i < cells; ++i)
        {
            if (fld.mInflow[i] > 0.f && geom.solid(i) && !geom.water(i))
            {
                fld.mSnow[i] += fld.mInflow[i];
            }
        }
    }

    // Deposit. Banks in the lee once wind drops under the hysteresis gate - strictly below the
    // lift threshold, so a cell cannot erode and bank in the same step range - weighted by how
    // sheltered the flow says the cell is.
    if (p.mDepositRate > 0.f)
    {
        for (size_t i = 0; i < cells; ++i)
        {
            if (!geom.solid(i) || geom.water(i) || fld.mSnow[i] <= 0.f) continue;

            const LLVector4& f = p.mFlow[i];
            const F32 speed = sqrtf(f.mV[0] * f.mV[0] + f.mV[1] * f.mV[1]);
            if (speed >= deposit_gate) continue;

            const F32 room = roomAt(geom, (S32)i, p.mSnowDepth * structScale(geom, i, p), p.mReposeRad);
            const F32 spare = room - fld.mSnow[i];
            if (spare <= 0.f) continue;

            const F32 shelter = llclamp(1.f - f.mV[3], 0.f, 1.f);
            const F32 flux = llmin(spare, p.mDepositRate * shelter * dt);
            fld.mSnow[i] += flux;
        }
    }
}

// A landing clump: credited against the cell's repose room, overflow discarded.
void SSGranular::depositAt(SSSurfaceField::Field& fld, const SSSurfaceField::Geometry& geom,
                           S32 index, F32 depth, F32 ceiling, F32 repose_rad)
{
    const S32 n = geom.mN;
    if (n < 1 || index < 0 || index >= n * n) return;
    if (!geom.solid(index) || geom.water(index) || depth <= 0.f) return;

    const F32 room = roomAt(geom, index, ceiling, repose_rad);
    fld.mSnow[index] = llmin(room, fld.mSnow[index] + depth);
}
