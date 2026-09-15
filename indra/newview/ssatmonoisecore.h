/**
 * @file ssatmonoisecore.h
 * @brief Atmo Magic: the shared deterministic noise, header-only core.
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

#ifndef SS_ATMONOISECORE_H
#define SS_ATMONOISECORE_H

// <SS:Nexii> A CORE header: depends on lldefs.h/<cmath> only, so V:\Scratch\atmo tests compile it in seconds without the viewer build (llmath.h is off limits here - it drags llsimdmath -> llmemory -> llerror -> boost). Moved out of ssatmomagic.h/.cpp on 2026-09-05 BIT-IDENTICAL: llfloor(F32) is (S32)floor(f) on 64-bit builds and the viewer's lerp IS std::lerp (llmath.h: using std::lerp), so every consumer - gusts, area factor, cloud noise, snow, precipitation - keeps the exact same numbers. This is the one hash every cross-client-synced pattern rides on; change nothing here without updating the golden pins in V:\Scratch\atmo\tests\noisecore.cpp. [interaction: every SSAtmoNoise consumer]
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSAtmoNoise
{
    inline U32 hashU32(U32 x)
    {
        x = x * 747796405u + 2891336453u;
        U32 w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
        return (w >> 22u) ^ w;
    }

    inline U32 combine(U32 a, U32 b) { return hashU32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2))); }

    inline F32 hash01(U32 x) { return (F32)(hashU32(x) & 0x00ffffffu) / (F32)0x01000000; }

    namespace detail
    {
        // 1D lattice hash to [-1,1].
        inline F32 latticeGrad(U32 seed, S32 ix)
        {
            return hash01(combine(seed, (U32)ix)) * 2.f - 1.f;
        }

        // 2D lattice hash to [-1,1].
        inline F32 latticeGrad2(U32 seed, S32 ix, S32 iy)
        {
            return hash01(combine(seed, combine((U32)ix, (U32)iy * 0x27d4eb2fu))) * 2.f - 1.f;
        }

        // Quintic fade.
        inline F32 quintic(F32 t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

        // The 64-bit llfloor, spelled out so this header needs no llmath.h.
        inline S32 floorS32(F32 f) { return (S32)floor(f); }
    }

    // 1D value noise.
    inline F32 value1(F32 x, U32 seed)
    {
        const S32 ix = detail::floorS32(x);
        const F32 fx = (F32)ix;
        F32 t = detail::quintic(x - fx);
        return std::lerp(detail::latticeGrad(seed, ix), detail::latticeGrad(seed, ix + 1), t);
    }

    // 2D value noise.
    inline F32 value2(F32 x, F32 y, U32 seed)
    {
        const S32 ix = detail::floorS32(x);
        const S32 iy = detail::floorS32(y);
        const F32 fx = (F32)ix;
        const F32 fy = (F32)iy;
        F32 tx = detail::quintic(x - fx);
        F32 ty = detail::quintic(y - fy);
        F32 a = std::lerp(detail::latticeGrad2(seed, ix, iy),     detail::latticeGrad2(seed, ix + 1, iy),     tx);
        F32 b = std::lerp(detail::latticeGrad2(seed, ix, iy + 1), detail::latticeGrad2(seed, ix + 1, iy + 1), tx);
        return std::lerp(a, b, ty);
    }

    // 1D fractal noise - the deterministic wobble everything shares.
    inline F32 fbm1(F32 x, U32 seed, S32 octaves = 3)
    {
        F32 sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
        for (S32 i = 0; i < octaves; ++i)
        {
            sum += amp * value1(x * freq, combine(seed, (U32)i));
            norm += amp;
            amp *= 0.5f;
            freq *= 2.03f;
        }
        return sum / norm;
    }

    // 2D fractal noise.
    inline F32 fbm2(F32 x, F32 y, U32 seed, S32 octaves = 3)
    {
        F32 sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
        for (S32 i = 0; i < octaves; ++i)
        {
            sum += amp * value2(x * freq, y * freq, combine(seed, (U32)i));
            norm += amp;
            amp *= 0.5f;
            freq *= 2.03f;
        }
        return sum / norm;
    }
}

#endif
