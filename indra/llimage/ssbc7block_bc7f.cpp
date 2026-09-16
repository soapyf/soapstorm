/**
 * @file ssbc7block_bc7f.cpp
 * @brief BC7 block backend: Binomial's bc7f, the analytical one-shot encoder inside the Basis Universal transcoder - see doc/super_compressed_textures.md
 *
 * This file is Soapstorm's own code and carries Soapstorm's licence. It only calls bc7f through the
 * transcoder's public header; no Basis Universal source is copied here. The transcoder itself lives
 * unmodified in indra/llimage/basis_universal/ under the Apache Licence 2.0, with its LICENSE and
 * NOTICE kept verbatim beside it, and is recorded in indra/newview/licenses-*.txt alongside the other
 * third party components the viewer links. See doc/super_compressed_textures.md for why it is kept at
 * arm's length rather than merged into this module.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssbc7encoder.h"

#include "basisu_transcoder.h"

#include <mutex>

// bc7f replaces the portable mode 6 encoder in ssbc7block_mode6.cpp, and the two are alternatives: exactly one is compiled in, selected by indra/llimage/CMakeLists.txt through SS_BC7F. Both implement the same seam, so nothing above this file knows which one it got.
//
// bc7f is an analytical encoder: it predicts the best BC7 configuration for a block from its statistics and encodes that one configuration, rather than searching. Measured against the portable backend (doc/super_compressed_textures.md, "bc7f and bc7e_scalar, measured 2026-09-09") it is better on every content class and 10-15x faster on textured content, and it is plain C++ with no SIMD, so every platform and every build gets it without any toolchain beyond the compiler.

namespace
{
    // The transcoder builds its lookup tables in basisu_transcoder_init, which is guarded by a plain bool rather than anything thread safe, so the call is forced through std::call_once. Encoding runs on worker threads, so "once" has to mean once across all of them.
    std::once_flag sInitFlag;

    void ssInitBC7F()
    {
        basist::basisu_transcoder_init();
    }

    // <SS:Nexii> Squeeze adaptive quality - bc7f's own preset ladder, one flag word per rung, read only after static initialisation. fast_pack_bc7_auto_rgba takes the flags by value, so an encode picks its profile with an array index and needs no mutex, no atomic and no re-init while the controller changes its mind between one texture and the next.
    //
    // Chosen from the offline benchmark at 512x512, single threaded, on textured content (Mpix/s is the whole-image figure, the multi-hue column is where the presets differ most):
    //
    //   Faster   ~110 Mpix/s   mode 6 with p-bits for opaque blocks, modes 4/5/6 for alpha; 15 dB on a 4x4 of several unrelated hues
    //   Fast      ~90 Mpix/s   adds the two-subset modes 1/3/7; 23 dB on that block
    //   Default   ~50 Mpix/s   adds the three-subset modes 0/2 and dual-plane RGB; 32 dB on that block, and never worse than the portable backend anywhere
    //
    // HIGH is Default because that is the preset the owner chose for the store: a texture is encoded once and read from disk for the life of the cache, and at fifty megapixels a second per worker the pool is supply limited many times over, so there is nothing to buy by stepping down. The cheaper rungs exist for the controller to fall back to under a real backlog. The partially and non-analytical presets above Default were measured too and are noted in the doc; they buy a few dB on smooth content for 3-10x the time and were not taken.
    const U32 sFlags[SSBC7_QUALITY_COUNT] =
    {
        basist::bc7f::cPackBC7FlagDefaultFaster,    // SSBC7_QUALITY_FAST
        basist::bc7f::cPackBC7FlagDefaultFast,      // SSBC7_QUALITY_BALANCED
        basist::bc7f::cPackBC7FlagDefault           // SSBC7_QUALITY_HIGH
    };
    // </SS:Nexii>
}

// bc7f reads a block as sixteen four byte pixels and writes sixteen bytes, and neither side carries an alignment requirement beyond a byte, so the caller's buffers are used as they are - no staging.
void ssBC7EncodeBlocksRGBA(U32 num_blocks, const U8* rgba_blocks, U8* out_blocks, SSBC7Quality quality)
{
    if (num_blocks == 0)
    {
        return;
    }

    std::call_once(sInitFlag, ssInitBC7F);

    // <SS:Nexii> Squeeze adaptive quality - a profile outside the table would index past the end of a static array, so it is clamped rather than trusted. BALANCED is the fallback because it is the cheapest preset that still has the partitioned modes, so a corrupted value degrades to something usable rather than to the one preset with a known blind spot.
    U32 profile = (U32)quality;
    if (profile >= (U32)SSBC7_QUALITY_COUNT) profile = (U32)SSBC7_QUALITY_BALANCED;
    const U32 flags = sFlags[profile];

    for (U32 i = 0; i < num_blocks; ++i)
    {
        basist::bc7f::fast_pack_bc7_auto_rgba(out_blocks + (size_t)i * SSBC7ENC_BLOCK_BYTES,
                                              reinterpret_cast<const basist::color_rgba*>(rgba_blocks + (size_t)i * SSBC7ENC_BLOCK_TEXELS * 4),
                                              flags);
    }
}

// The two hundreds are bc7f's range, the one hundreds were bc7e's and the low numbers are the portable backend's, so a blob encoded by one is never mistaken for another's output. All remain valid BC7 whatever the stamp says - what the stamp buys is that the cache is wiped rather than read by a decoder that means something else by the same bytes.
//
// <SS:Nexii> Squeeze adaptive quality - a PLAIN BACKEND ID with no quality term, and it must stay that way. The profile lives in SSBC7Record::mQuality, and a record encoded at a low profile is upgraded in place by the idle pass rather than being invalidated wholesale. This value changing again should mean the BLOCK FORMAT changed, never that a setting did.
U32 ssBC7BlockBackendVersion()
{
    return 200;
}

const char* ssBC7BlockBackendName()
{
    return "bc7f";
}

// <SS:Nexii> Squeeze adaptive quality - the bc7f preset each level actually maps to, so the log line that reports a change names the thing that changed rather than an enum ordinal.
const char* ssBC7QualityName(SSBC7Quality quality)
{
    switch (quality)
    {
        case SSBC7_QUALITY_FAST: return "faster";
        case SSBC7_QUALITY_HIGH: return "default";
        default:                 return "fast";
    }
}

bool ssBC7BackendHasQualityProfiles()
{
    return true;
}

// Apache 2.0 asks that the notices travel with the work, and Basis Universal's NOTICE file adds that its attribution notices be reproduced. The full licence text and those notices ship in licenses.txt, which is the copy that satisfies the licence; this shorter line exists so the credit is also somewhere a user will actually look. "Basis Universal" is Binomial's trademark, so it is named and not claimed.
const char* ssBC7BlockBackendAttribution()
{
    return "Basis Universal(TM) bc7f texture encoder, Copyright (C) 2016-2026 Binomial LLC, used under the Apache License 2.0.\n"
           "Basis Universal is a trademark of Binomial LLC. See licenses.txt.\n";
}
