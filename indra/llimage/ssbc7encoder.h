/**
 * @file ssbc7encoder.h
 * @brief Squeeze BC7 encoder - one full resolution image to a complete BC7 mip chain in store order, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7ENCODER_H
#define SS_BC7ENCODER_H

#include "stdtypes.h"

#include <vector>

// This is the only place that turns pixels into the payload ssbc7store.h serialises, so it owns two invariants the store cannot check for itself: the chain is COMPLETE from the base level down, and it is laid out SMALLEST MIP FIRST because that is the order LLImageGL::setImage walks backwards from.
//
// Mips are built on the CPU with LLImageBase::generateMip. glGenerateMipmap is illegal on a compressed target, so a chain that is not built here does not exist at all.

// Mirrors SSBC7_MAX_MIPS in indra/newview/ssbc7store.h. It is duplicated rather than included because llimage sits below newview in the link order and must not reach upward; the offline test asserts the two constants are equal, which is the only place both headers are visible at once.
constexpr U32 SSBC7ENC_MAX_MIPS    = 6;
constexpr U32 SSBC7ENC_BLOCK_BYTES = 16;
constexpr U32 SSBC7ENC_BLOCK_TEXELS = 16;

// Caller-owned working buffers, reused across every texture a worker encodes so a busy encode thread does no allocation per image and none whatsoever per block. One instance per thread, never shared.
struct SSBC7EncodeScratch
{
    std::vector<U8> mMipA;              // ping and pong for the mip chain; level 0 is never copied, it stays the caller's immutable source
    std::vector<U8> mMipB;
    // A run of gathered 4x4 blocks, each 64 bytes of RGBA, passed to the backend in one call. Batched because a SIMD encoder's entire advantage is filling a vector with independent blocks, and feeding it one at a time would waste most of that.
    std::vector<U8> mBlockBatch;
    std::vector<U8> mOutBatch;
};

// <SS:Nexii> Squeeze adaptive quality - the profile is a PARAMETER of an encode rather than a property of the process, because the adaptive controller in newview changes it while the session runs and a record now carries the profile it was made at. Declared here, above everything that takes one, rather than beside the backend seam where it used to live.
//
// The portable mode 6 backend has only one setting and ignores this. bc7f maps them onto its own presets, where each step brings in more of the BC7 format: FAST is mode 6 plus the alpha modes, BALANCED adds the two-subset modes, and HIGH adds the three-subset and dual-plane modes, so a block holding several unrelated colours improves at every rung.
//
// The ordering is by MEASURED cost, not by name, and these values are the on-disk meaning of SSBC7Record::mQuality - so they may be extended but must never be renumbered.
enum SSBC7Quality
{
    SSBC7_QUALITY_FAST     = 0,
    SSBC7_QUALITY_BALANCED = 1,
    SSBC7_QUALITY_HIGH     = 2,
    SSBC7_QUALITY_COUNT    = 3
};
// </SS:Nexii>

struct SSBC7EncodeResult
{
    SSBC7EncodeResult();

    U32 mWidth;             // echoed back so a caller that clamped or rejected the geometry does not have to recompute it
    U32 mHeight;
    U32 mPayloadBytes;      // always equals ssBC7PayloadBytes(mWidth, mHeight, mMipCount)
    U8  mMipCount;
    U8  mSrcComponents;     // components of the ORIGINAL image, 1/2/3/4, NOT BC7's intrinsic four - this byte is what later keeps opaque textures out of the alpha pool
    // <SS:Nexii> Squeeze adaptive quality - the profile this chain was ACTUALLY encoded at, echoed back so the caller stores it beside the bytes rather than re-reading a setting that may have moved since it asked, and the texel count the encoder really put through the backend, which is the numerator of every throughput figure the controller measures. Padded texels are counted because padding is work the machine genuinely did, and the whole chain is counted rather than the base level because the mips are about a third of it and pretending otherwise would understate the machine by that much.
    U8  mQuality;           // SSBC7Quality
    U32 mEncodedTexels;
    // </SS:Nexii>
};

// Encodes src, which must be width * height * components tightly packed bytes, into a complete BC7 chain. Returns false and leaves out_payload empty on any geometry the store cannot represent. Touches no globals and no GL, so it is safe on any thread.
//
// <SS:Nexii> Squeeze adaptive quality - `quality` is passed per call and reaches the backend unchanged. Nothing is latched and nothing is synchronised, so two workers may be encoding at two different profiles in the same instant, which is exactly what lets the controller change its mind without first draining the pool.
bool ssBC7EncodeMipChain(const U8* src, U32 width, U32 height, U32 components,
                         SSBC7Quality quality,
                         SSBC7EncodeScratch& scratch,
                         std::vector<U8>& out_payload,
                         SSBC7EncodeResult& out_result);

// Stamped into every blob header. Any change to the pipeline or to the block backend must move this, because a cached blob is only meaningful next to the encoder that produced it.
U32 ssBC7EncoderVersion();

// Geometry, mirroring ssBC7LevelBytes/ssBC7PayloadBytes/ssBC7LevelOffset in indra/newview/ssbc7store.h for the same reason the mip ceiling is mirrored. The offline test pins them against the store's originals across a sweep of sizes.
U32 ssBC7EncLevelBytes(U32 width, U32 height);
U32 ssBC7EncPayloadBytes(U32 width, U32 height, U32 mip_count);
U32 ssBC7EncLevelOffset(U32 width, U32 height, U32 mip_count, U32 store_index);

// Number of levels the chain will actually contain for a given base size, capped at SSBC7ENC_MAX_MIPS and stopping before either axis reaches one texel because generateMip reads a source of exactly twice the destination in both axes.
U32 ssBC7EncMipCount(U32 width, U32 height);

// ---------------------------------------------------------------------------
// Block backend seam
//
// Everything above is the pipeline and is independent of encode quality. Everything below is implemented in exactly one translation unit - ssbc7block_bc7f.cpp by default, ssbc7block_mode6.cpp when SS_BC7F is off - chosen by indra/llimage/CMakeLists.txt. Nothing above the seam knows which one it got.
// ---------------------------------------------------------------------------

// Compresses a run of 4x4 RGBA blocks. `rgba_blocks` holds num_blocks consecutive blocks of 64 bytes each, row major within a block; `out_blocks` receives num_blocks consecutive 16 byte BC7 blocks.
//
// Batched rather than one block at a time because a SIMD backend fills its vector lanes with independent blocks - the reason the bc7e backend this seam was first built for asked for dozens per call. A scalar backend such as bc7f simply loops, and the batching costs it nothing.
//
// Determinism required of a backend is per machine, not universal: the same bytes encoded twice on one machine must give the same block, because that is what makes a stored blob comparable against the version stamped beside it. A SIMD backend dispatching on the host's instruction set, or compiled with fast maths, may legitimately differ from another machine's - which costs nothing here, because a blob is only ever read back by the installation that wrote it.
//
// <SS:Nexii> Squeeze adaptive quality - `quality` is a per-call argument and this function MUST stay free of locks. A backend that offers profiles builds every one of its parameter blocks once and thereafter only reads them, so selecting between them costs an array index on a path that runs once per row of blocks.
void ssBC7EncodeBlocksRGBA(U32 num_blocks, const U8* rgba_blocks, U8* out_blocks, SSBC7Quality quality);

// Identifies the backend so ssBC7EncoderVersion changes automatically when the backend file is replaced.
//
// <SS:Nexii> Squeeze adaptive quality - THE QUALITY IS NO LONGER FOLDED IN HERE, and that removal is what the rest of this feature rests on. It used to be, so that flipping the setting re-encoded the cache; but once the profile can vary WITHIN a session that scheme wipes the whole store every time the controller changes its mind. The profile travels in the record instead, which is strictly better than what it replaced: the store may hold a mixture, and a texture encoded in a hurry becomes something the idle upgrade pass can improve later rather than something that is simply wrong until the next wipe.
U32         ssBC7BlockBackendVersion();
const char* ssBC7BlockBackendName();

// <SS:Nexii> Squeeze adaptive quality - what the backend actually does with each profile, for the log line that reports a change. The portable backend answers the same string for all three, which is the honest answer when it only has one encoder.
const char* ssBC7QualityName(SSBC7Quality quality);

// False when the linked backend ignores the profile entirely, which is how newview knows adaptive selection would be theatre and says so once at startup rather than logging changes that change nothing.
bool ssBC7BackendHasQualityProfiles();
// </SS:Nexii>

// The attribution this backend requires in the About box, or null if it is entirely our own code and needs none. The About box reads packages-info.txt, which is generated from the autobuild packages and therefore cannot know about a backend chosen at configure time, so the notice has to come from the backend itself.
//
// Asking the backend rather than testing a build flag is deliberate: whichever one is linked is the one that answers, so the credit shown can never describe code that is not actually in the binary.
const char* ssBC7BlockBackendAttribution();

#endif // SS_BC7ENCODER_H
