/**
 * @file ssbc7encoder.cpp
 * @brief Squeeze BC7 encoder - one full resolution image to a complete BC7 mip chain in store order, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssbc7encoder.h"

#include "llimage.h"

#include <cstring>

namespace
{
    // Moves whenever anything in THIS file changes the bytes it emits. The backend contributes the low half of the encoder version separately, so the two evolve without stepping on each other.
    const U32 SS_BC7ENC_PIPELINE_VERSION = 1;

    // Number of BC7 blocks along one axis. The clamp to four matches ssBC7LevelBytes: a two by two or one by one mip still occupies a whole block, and the partial block is filled by edge clamping rather than left undefined, so two runs over the same pixels can never disagree.
    inline U32 blocksAcross(U32 dim)
    {
        const U32 d = (dim < 4) ? 4 : dim;
        return (d + 3) / 4;
    }

    // Gathers one 4x4 out of a level and expands it to RGBA, which is the only shape the block backend accepts. Component expansion follows the GL formats llimagegl.cpp:1641-1658 picks for each component count: one is luminance, two is luminance plus alpha, three is opaque RGB.
    void gatherBlockRGBA(const U8* level, U32 width, U32 height, U32 components, U32 block_x, U32 block_y, U8* out)
    {
        for (U32 y = 0; y < 4; ++y)
        {
            const U32 ty = block_y * 4 + y;
            const U32 sy = (ty < height) ? ty : (height - 1);
            for (U32 x = 0; x < 4; ++x)
            {
                const U32 tx = block_x * 4 + x;
                const U32 sx = (tx < width) ? tx : (width - 1);
                const U8* p = level + ((size_t)sy * width + sx) * components;
                U8* d = out + ((size_t)y * 4 + x) * 4;
                switch (components)
                {
                case 1:  d[0] = p[0]; d[1] = p[0]; d[2] = p[0]; d[3] = 255;  break;
                case 2:  d[0] = p[0]; d[1] = p[0]; d[2] = p[0]; d[3] = p[1]; break;
                case 3:  d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255;  break;
                default: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3]; break;
                }
            }
        }
    }

    // Blocks are emitted in raster order, which is the order GL reads a compressed level in.
    //
    // One row of blocks is gathered and handed to the backend per call. A row is a natural batch: it is contiguous in the output, it keeps the gathered working set small enough to stay in cache, and for every level above 4 texels tall it is comfortably past the few dozen blocks a SIMD backend wants. The smallest levels fall below that, but they are a rounding error of the total work.
    void encodeLevel(const U8* level, U32 width, U32 height, U32 components, SSBC7Quality quality, SSBC7EncodeScratch& scratch, U8* dst)
    {
        const U32 bw = blocksAcross(width);
        const U32 bh = blocksAcross(height);
        if (bw == 0 || bh == 0) return;

        const size_t gathered_bytes = (size_t)bw * SSBC7ENC_BLOCK_TEXELS * 4;
        if (scratch.mBlockBatch.size() < gathered_bytes) scratch.mBlockBatch.resize(gathered_bytes);

        const size_t out_bytes = (size_t)bw * SSBC7ENC_BLOCK_BYTES;
        if (scratch.mOutBatch.size() < out_bytes) scratch.mOutBatch.resize(out_bytes);

        for (U32 by = 0; by < bh; ++by)
        {
            for (U32 bx = 0; bx < bw; ++bx)
            {
                gatherBlockRGBA(level, width, height, components, bx, by,
                                scratch.mBlockBatch.data() + (size_t)bx * SSBC7ENC_BLOCK_TEXELS * 4);
            }

            ssBC7EncodeBlocksRGBA(bw, scratch.mBlockBatch.data(), scratch.mOutBatch.data(), quality);
            memcpy(dst + (size_t)by * bw * SSBC7ENC_BLOCK_BYTES, scratch.mOutBatch.data(), out_bytes);
        }
    }
}

SSBC7EncodeResult::SSBC7EncodeResult()
:   mWidth(0),
    mHeight(0),
    mPayloadBytes(0),
    mMipCount(0),
    mSrcComponents(0),
    // <SS:Nexii> Squeeze adaptive quality - BALANCED rather than zero, so a result nobody filled in never claims to be the one profile with a known blind spot.
    mQuality((U8)SSBC7_QUALITY_BALANCED),
    mEncodedTexels(0)
    // </SS:Nexii>
{
}

// ---------------------------------------------------------------------------
// Geometry - kept byte for byte in step with ssbc7store.cpp, which the offline test pins
// ---------------------------------------------------------------------------

U32 ssBC7EncLevelBytes(U32 width, U32 height)
{
    const U32 w = (width  < 4) ? 4 : width;
    const U32 h = (height < 4) ? 4 : height;
    return ((w + 3) / 4) * ((h + 3) / 4) * SSBC7ENC_BLOCK_BYTES;
}

U32 ssBC7EncPayloadBytes(U32 width, U32 height, U32 mip_count)
{
    if (width == 0 || height == 0 || mip_count == 0 || mip_count > SSBC7ENC_MAX_MIPS) return 0;

    U32 total = 0;
    for (U32 level = 0; level < mip_count; ++level)
    {
        const U32 w = (width  >> level) ? (width  >> level) : 1;
        const U32 h = (height >> level) ? (height >> level) : 1;
        total += ssBC7EncLevelBytes(w, h);
    }
    return total;
}

U32 ssBC7EncLevelOffset(U32 width, U32 height, U32 mip_count, U32 store_index)
{
    if (store_index >= mip_count) return 0;

    U32 offset = 0;
    for (U32 i = 0; i < store_index; ++i)
    {
        const U32 level = mip_count - 1 - i;
        const U32 w = (width  >> level) ? (width  >> level) : 1;
        const U32 h = (height >> level) ? (height >> level) : 1;
        offset += ssBC7EncLevelBytes(w, h);
    }
    return offset;
}

U32 ssBC7EncMipCount(U32 width, U32 height)
{
    if (width == 0 || height == 0) return 0;

    // Halving stops while BOTH axes are still at least two, because generateMip reads a source of exactly twice the destination in both axes; one more step on a level that is already one texel tall would read the row past the end of the buffer.
    U32 count = 1;
    U32 w = width;
    U32 h = height;
    while (count < SSBC7ENC_MAX_MIPS && w >= 2 && h >= 2)
    {
        w >>= 1;
        h >>= 1;
        ++count;
    }
    return count;
}

U32 ssBC7EncoderVersion()
{
    // High half is the pipeline, low half is whichever block backend is linked, so swapping the backend translation unit invalidates every cached blob without anyone having to remember to bump a constant.
    return (SS_BC7ENC_PIPELINE_VERSION << 16) | (ssBC7BlockBackendVersion() & 0xFFFFu);
}

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------

bool ssBC7EncodeMipChain(const U8* src, U32 width, U32 height, U32 components,
                         SSBC7Quality quality,
                         SSBC7EncodeScratch& scratch,
                         std::vector<U8>& out_payload,
                         SSBC7EncodeResult& out_result)
{
    out_payload.clear();
    out_result = SSBC7EncodeResult();          // <SS:Nexii/> Squeeze adaptive quality - the texel tally accumulates through the loop below, so it has to start from a fresh result rather than from whatever the caller reused

    if (!src || width == 0 || height == 0) return false;
    if (components < 1 || components > 4) return false;
    if (width > 0xFFFFu || height > 0xFFFFu) return false;   // the store keeps width and height in a U16 each

    const U32 mip_count = ssBC7EncMipCount(width, height);
    const U32 total = ssBC7EncPayloadBytes(width, height, mip_count);
    if (mip_count == 0 || total == 0) return false;

    out_payload.assign(total, 0);

    // Level zero is the caller's buffer and is never copied or written to; every level after it is built into scratch, ping-ponging so a level is never generated on top of the one it is reading from.
    const U8* level = src;
    U32 lw = width;
    U32 lh = height;

    for (U32 i = 0; i < mip_count; ++i)
    {
        if (i > 0)
        {
            const U32 nw = lw >> 1;
            const U32 nh = lh >> 1;
            std::vector<U8>& dst = (i & 1u) ? scratch.mMipA : scratch.mMipB;
            dst.resize((size_t)nw * nh * components);
            LLImageBase::generateMip(level, dst.data(), (S32)nw, (S32)nh, (S32)components);
            level = dst.data();
            lw = nw;
            lh = nh;
        }

        // Store order is smallest first, so the base level lands last and each level goes straight to its final offset - no second pass and no reversal buffer.
        const U32 store_index = mip_count - 1 - i;
        const U32 offset = ssBC7EncLevelOffset(width, height, mip_count, store_index);
        encodeLevel(level, lw, lh, components, quality, scratch, out_payload.data() + offset);

        // <SS:Nexii> Squeeze adaptive quality - counted here rather than derived from the base size afterwards, because what the backend was actually handed is the padded block grid and a 1x1 mip costs a whole 4x4 of it. Deriving it would flatter the machine on small textures and understate the cost of the chain's tail.
        out_result.mEncodedTexels += blocksAcross(lw) * 4 * blocksAcross(lh) * 4;
        // </SS:Nexii>
    }

    out_result.mWidth         = width;
    out_result.mHeight        = height;
    out_result.mPayloadBytes  = total;
    out_result.mMipCount      = (U8)mip_count;
    out_result.mSrcComponents = (U8)components;   // the ORIGINAL count, not BC7's intrinsic four, because this byte is what later keeps opaque textures out of the alpha pool
    out_result.mQuality       = (U8)quality;      // <SS:Nexii/> Squeeze adaptive quality - echoed back so the store records the profile the bytes were made at, rather than whatever the controller has moved on to by the time the append happens
    return true;
}
