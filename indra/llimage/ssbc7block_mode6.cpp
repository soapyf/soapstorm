/**
 * @file ssbc7block_mode6.cpp
 * @brief BC7 block backend: a real mode 6 encoder - see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssbc7encoder.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

// BC7 mode 6: one partition, RGBA endpoints of seven bits plus one shared p-bit each, and four bit indices.
//
// Mode 6 is the single mode implemented here because its endpoints are nearly exact. A channel is stored as (q7 << 1) | p, so any eight bit value whose low bit matches the endpoint's p-bit survives untouched, and the other channels are off by at most one. Nearly all of a mode 6 block's error therefore comes from quantising each texel to one of sixteen positions along the endpoint line, which makes a single-mode encoder far better than it sounds and keeps the whole backend auditable.
//
// What is given up by not implementing the partitioned modes is a 4x4 containing two unrelated colour regions - a hard edge - which fits one line badly. That is this backend's known quality ceiling and the reason the seam exists.
//
// Bit layout, LSB first: 7 bits of mode (six zeros then a one), then R0 R1 G0 G1 B0 B1 A0 A1 at seven bits each, then P0 and P1, then 63 index bits - the first index stores three bits because the anchor rule implies its high bit is zero.

namespace
{
    const U32 SSBC7_TEXELS = 16;

    // The four bit interpolation weights from the BC7 specification.
    const U8 SSBC7_WEIGHT4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

    inline U8 ssInterp(U8 e0, U8 e1, U8 w)
    {
        const S32 t = SSBC7_WEIGHT4[w];
        return (U8)(((64 - t) * (S32)e0 + t * (S32)e1 + 32) >> 6);
    }

    // Best seven bit quantisation of v given a fixed p-bit: the representable values are those with low bit equal to p.
    inline U8 ssQuant7(S32 v, U8 p)
    {
        S32 q = (v - (S32)p) >> 1;
        q = llclamp(q, 0, 127);
        const S32 lo = (q << 1) | p;
        const S32 hi = ((llmin(q + 1, 127)) << 1) | p;
        return (abs(v - lo) <= abs(v - hi)) ? (U8)q : (U8)llmin(q + 1, 127);
    }

    inline U8 ssDecode7(U8 q, U8 p) { return (U8)((q << 1) | p); }

    // One p-bit covers all four channels of an endpoint, so it is chosen to minimise the total error over those four rather than taken from any single channel.
    U8 ssChooseP(const S32* target)
    {
        S32 err[2] = { 0, 0 };
        for (U8 p = 0; p < 2; ++p)
        {
            for (U32 c = 0; c < 4; ++c)
            {
                const S32 got = (S32)ssDecode7(ssQuant7(target[c], p), p);
                err[p] += abs(target[c] - got);
            }
        }
        return (err[1] < err[0]) ? 1 : 0;
    }

    class BitWriter
    {
    public:
        explicit BitWriter(U8* out) : mOut(out), mBit(0) { memset(mOut, 0, 16); }

        void put(U32 value, U32 count)
        {
            for (U32 i = 0; i < count; ++i)
            {
                if ((value >> i) & 1u)
                {
                    mOut[mBit >> 3] = (U8)(mOut[mBit >> 3] | (1u << (mBit & 7u)));
                }
                ++mBit;
            }
        }

    private:
        U8* mOut;
        U32 mBit;
    };

    inline S32 ssTexelError(const U8* texel, const U8* a0, const U8* a1, U8 w)
    {
        S32 err = 0;
        for (U32 c = 0; c < 4; ++c)
        {
            const S32 d = (S32)texel[c] - (S32)ssInterp(a0[c], a1[c], w);
            err += d * d;
        }
        return err;
    }

    inline U8 ssBestIndex(const U8* texel, const U8* a0, const U8* a1)
    {
        U8 best_w = 0;
        S32 best_err = 0x7FFFFFFF;
        for (U8 w = 0; w < 16; ++w)
        {
            const S32 err = ssTexelError(texel, a0, a1, w);
            if (err < best_err)
            {
                best_err = err;
                best_w = w;
            }
        }
        return best_w;
    }
}

// The scalar backend simply loops. Keeping the per-block body in its own function means the batch
// entry point is the only thing that differs from a SIMD backend.
static void ssBC7EncodeOneBlock(const U8* rgba_4x4, U8* out_block)
{
    // Endpoints start as the two texels furthest apart in RGBA space. That is 120 comparisons for a 4x4, and it beats an axis aligned bounding box on the correlated colours that dominate real textures: a shaded surface runs diagonally through the colour cube, where a bounding box would put both endpoints off the data entirely.
    S32 t0[4], t1[4];
    S32 best_d2 = -1;
    U32 bi = 0, bj = 0;

    for (U32 i = 0; i < SSBC7_TEXELS; ++i)
    {
        for (U32 j = i + 1; j < SSBC7_TEXELS; ++j)
        {
            S32 d2 = 0;
            for (U32 c = 0; c < 4; ++c)
            {
                const S32 d = (S32)rgba_4x4[i * 4 + c] - (S32)rgba_4x4[j * 4 + c];
                d2 += d * d;
            }
            if (d2 > best_d2)
            {
                best_d2 = d2;
                bi = i;
                bj = j;
            }
        }
    }

    for (U32 c = 0; c < 4; ++c)
    {
        t0[c] = (S32)rgba_4x4[bi * 4 + c];
        t1[c] = (S32)rgba_4x4[bj * 4 + c];
    }

    U8 indices[SSBC7_TEXELS];
    U8 a0[4], a1[4];   // endpoints as they will actually decode, which is what indices must be chosen against

    // A uniform block needs no refinement, and running the least squares fit on it would divide by a zero spread.
    U32 rounds = (best_d2 == 0) ? 0 : 2;

    for (U32 round = 0; round < rounds; ++round)
    {
        // Assign against the quantised endpoints so each round sees the same values the decoder will.
        const U8 p0 = ssChooseP(t0);
        const U8 p1 = ssChooseP(t1);
        for (U32 c = 0; c < 4; ++c)
        {
            a0[c] = ssDecode7(ssQuant7(t0[c], p0), p0);
            a1[c] = ssDecode7(ssQuant7(t1[c], p1), p1);
        }

        for (U32 i = 0; i < SSBC7_TEXELS; ++i)
        {
            indices[i] = ssBestIndex(&rgba_4x4[i * 4], a0, a1);
        }

        // Least squares refit of the pair that minimises the summed squared error given those weights.
        F64 sum_tt = 0.0, sum_uu = 0.0, sum_tu = 0.0;
        F64 sum_tx[4] = { 0, 0, 0, 0 };
        F64 sum_ux[4] = { 0, 0, 0, 0 };

        for (U32 i = 0; i < SSBC7_TEXELS; ++i)
        {
            const F64 t = (F64)SSBC7_WEIGHT4[indices[i]] / 64.0;
            const F64 u = 1.0 - t;
            sum_tt += t * t;
            sum_uu += u * u;
            sum_tu += t * u;
            for (U32 c = 0; c < 4; ++c)
            {
                const F64 x = (F64)rgba_4x4[i * 4 + c];
                sum_tx[c] += t * x;
                sum_ux[c] += u * x;
            }
        }

        const F64 det = sum_uu * sum_tt - sum_tu * sum_tu;
        if (fabs(det) < 1e-9)
        {
            // Every texel landed on the same weight, so there is no line to fit and the current endpoints are as good as this method gets.
            break;
        }

        for (U32 c = 0; c < 4; ++c)
        {
            const F64 a = (sum_tt * sum_ux[c] - sum_tu * sum_tx[c]) / det;
            const F64 b = (sum_uu * sum_tx[c] - sum_tu * sum_ux[c]) / det;
            t0[c] = llclamp((S32)floor(a + 0.5), 0, 255);
            t1[c] = llclamp((S32)floor(b + 0.5), 0, 255);
        }
    }

    // Final quantisation, then a final index pass against exactly what will be written.
    U8 p0 = ssChooseP(t0);
    U8 p1 = ssChooseP(t1);
    U8 q0[4], q1[4];
    for (U32 c = 0; c < 4; ++c)
    {
        q0[c] = ssQuant7(t0[c], p0);
        q1[c] = ssQuant7(t1[c], p1);
        a0[c] = ssDecode7(q0[c], p0);
        a1[c] = ssDecode7(q1[c], p1);
    }

    for (U32 i = 0; i < SSBC7_TEXELS; ++i)
    {
        indices[i] = (best_d2 == 0) ? 0 : ssBestIndex(&rgba_4x4[i * 4], a0, a1);
    }

    // Anchor rule: the first index is stored in three bits, so its high bit must be zero. Swapping the endpoints and mirroring every index is exactly equivalent and costs nothing.
    if (indices[0] > 7)
    {
        for (U32 c = 0; c < 4; ++c)
        {
            const U8 tq = q0[c]; q0[c] = q1[c]; q1[c] = tq;
        }
        const U8 tp = p0; p0 = p1; p1 = tp;
        for (U32 i = 0; i < SSBC7_TEXELS; ++i)
        {
            indices[i] = (U8)(15 - indices[i]);
        }
    }

    BitWriter w(out_block);
    w.put(1u << 6, 7);
    for (U32 c = 0; c < 4; ++c)
    {
        w.put((U32)q0[c], 7);
        w.put((U32)q1[c], 7);
    }
    w.put((U32)p0, 1);
    w.put((U32)p1, 1);

    w.put((U32)indices[0], 3);
    for (U32 i = 1; i < SSBC7_TEXELS; ++i)
    {
        w.put((U32)indices[i], 4);
    }
}

// <SS:Nexii> Squeeze adaptive quality - the profile is accepted and ignored, because this backend implements one mode with one search and there is no dial to turn. Ignoring it is not a failure, and the version below deliberately does not move with it: a request that changed nothing must never invalidate a cache. ssBC7BackendHasQualityProfiles() is what stops newview from spending a controller on a backend with one setting.
void ssBC7EncodeBlocksRGBA(U32 num_blocks, const U8* rgba_blocks, U8* out_blocks, SSBC7Quality)
{
    for (U32 i = 0; i < num_blocks; ++i)
    {
        ssBC7EncodeOneBlock(rgba_blocks + (size_t)i * 64, out_blocks + (size_t)i * 16);
    }
}

U32 ssBC7BlockBackendVersion()
{
    return 2;
}

const char* ssBC7BlockBackendName()
{
    return "ss-mode6-lsq";
}

// <SS:Nexii> Squeeze adaptive quality - one encoder, so every level names the same thing. Saying "mode6" three times is more honest than inventing three names for one code path.
const char* ssBC7QualityName(SSBC7Quality)
{
    return "mode6";
}

bool ssBC7BackendHasQualityProfiles()
{
    return false;
}
// </SS:Nexii>

// Nothing third party is linked when this backend is the one compiled, so there is nobody to credit.
const char* ssBC7BlockBackendAttribution()
{
    return NULL;
}
