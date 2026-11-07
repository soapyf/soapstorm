/**
 * @file ssbc7store.cpp
 * @brief Squeeze BC7 sidecar store, format half - every byte the store puts on disk, and nothing that touches a disk, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7store.h"

// Split from ssbc7storeio.cpp deliberately: this half reaches for no viewer global and no filesystem, so the exact bytes the store writes can be built, parsed and corrupted in an offline test that links this file alone. The half that owns directories, handles and settings lives next door.

// <SS:Nexii> STAGE 1 of doc/strata.md, the shared serialisation lift. The Writer, the Reader and the CRC32 wrapper that used to be declared in an anonymous namespace here were a verbatim copy of the ones in ssroccache.cpp - the comment above them said so - and they now live once, in indra/llcommon/ssserial.h. llcommon sits below both newview and llfilesystem, so nothing gained a link edge and the offline test still links this file alone.
//
// PURELY MECHANICAL: the lifted classes are byte for byte the same code with the same explicit little-endian field writes, so every record and every blob this file produces is identical to the one it produced before. The volume manager was deliberately NOT lifted, for the reasons ssserial.h states at length.
#include "ssserial.h"

#include <cstring>

using ssserial::Writer;
using ssserial::Reader;
using ssserial::crc32Of;
// </SS:Nexii>

SSBC7Record::SSBC7Record()
:   mBlobOffset(0),
    mBlobSize(0),
    mPickMaskBytes(0),
    mDataCRC(0),
    mSegment(0),
    mWidth(0),
    mHeight(0),
    mLastUseDay(0),
    mFlags(0),
    mBaseDiscard(0),
    mMipCount(0),
    mFormat(SSBC7_FMT_BC7_UNORM),
    mSrcComponents(4),
    // <SS:Nexii/> Squeeze adaptive quality - BALANCED rather than zero, so a record that predates a field being filled in never claims to have been made at the one profile with a known blind spot and so never invites a pointless re-encode
    mQuality((U8)1),
    mRecordCRC(0),
    // <SS:Nexii> Squeeze eviction - in memory only, so deserializeRecord's fresh SSBC7Record leaves a reloaded record looking untouched rather than inheriting whatever was on the stack.
    mTouchTick(0),
    mTouchSecond(0)
    // </SS:Nexii>
{
}

SSBC7BlobHeader::SSBC7BlobHeader()
:   mWidth(0),
    mHeight(0),
    mBaseDiscard(0),
    mMipCount(0),
    mFormat(SSBC7_FMT_BC7_UNORM),
    mSrcComponents(4),
    mFlags(0),
    mQuality((U8)1),               // <SS:Nexii/> Squeeze adaptive quality
    mPickMaskBytes(0),
    mPayloadBytes(0),
    mPickMaskCRC(0),
    mPayloadCRC(0),
    mEncoderVersion(0)
{
    memset(mMipCRC, 0, sizeof(mMipCRC));
}

SSBC7Encoded::SSBC7Encoded()
:   mWidth(0),
    mHeight(0),
    mMipCount(0),
    mSrcComponents(4),
    mQuality((U8)1),               // <SS:Nexii/> Squeeze adaptive quality
    mFlags(0)
{
}

// ---------------------------------------------------------------------------
// Level arithmetic
// ---------------------------------------------------------------------------

U32 ssBC7LevelBytes(U32 width, U32 height)
{
    // BC7 stores one sixteen byte block per four by four texel group, and a level smaller than a block still occupies a whole one. This mirrors the clamp added to LLImageGL::dataFormatBytes; getting it wrong desynchronises the backward mip walk in the upload loop rather than failing loudly.
    const U32 w = (width  < 4) ? 4 : width;
    const U32 h = (height < 4) ? 4 : height;
    return ((w + 3) / 4) * ((h + 3) / 4) * 16;
}

U32 ssBC7PayloadBytes(U32 width, U32 height, U32 mip_count)
{
    if (width == 0 || height == 0 || mip_count == 0 || mip_count > SSBC7_MAX_MIPS) return 0;

    U32 total = 0;
    for (U32 level = 0; level < mip_count; ++level)
    {
        const U32 w = (width  >> level) ? (width  >> level) : 1;
        const U32 h = (height >> level) ? (height >> level) : 1;
        total += ssBC7LevelBytes(w, h);
    }
    return total;
}

U32 ssBC7LevelOffset(U32 width, U32 height, U32 mip_count, U32 store_index)
{
    if (store_index >= mip_count) return 0;

    // Store order is smallest first, so store index 0 is the coarsest level, which is discard mip_count-1 counting down from the base.
    U32 offset = 0;
    for (U32 i = 0; i < store_index; ++i)
    {
        const U32 level = mip_count - 1 - i;
        const U32 w = (width  >> level) ? (width  >> level) : 1;
        const U32 h = (height >> level) ? (height >> level) : 1;
        offset += ssBC7LevelBytes(w, h);
    }
    return offset;
}

// ---------------------------------------------------------------------------
// Blob serialisation
// ---------------------------------------------------------------------------

bool ssBC7BuildBlob(const SSBC7Encoded& src, U32 encoder_version, std::vector<U8>& out_blob, SSBC7Record& out_record)
{
    if (src.mWidth == 0 || src.mHeight == 0) return false;
    if (src.mMipCount == 0 || src.mMipCount > SSBC7_MAX_MIPS) return false;
    if (src.mSrcComponents < 1 || src.mSrcComponents > 4) return false;

    const U32 expect = ssBC7PayloadBytes(src.mWidth, src.mHeight, src.mMipCount);
    if (expect == 0 || src.mPayload.size() != (size_t)expect) return false;

    const U32 payload_bytes   = (U32)src.mPayload.size();
    const U32 pickmask_bytes  = (U32)src.mPickMask.size();

    out_blob.clear();
    out_blob.reserve(SSBC7_BLOB_HEADER_SIZE + payload_bytes + pickmask_bytes);

    // Per-level checksums are recorded in STORE order so a prefix read - which is how any coarser discard is served - can verify exactly the levels it actually read.
    U32 mip_crc[SSBC7_MAX_MIPS];
    memset(mip_crc, 0, sizeof(mip_crc));
    for (U32 i = 0; i < src.mMipCount; ++i)
    {
        const U32 level = src.mMipCount - 1 - i;
        const U32 w = (src.mWidth  >> level) ? (U32)(src.mWidth  >> level) : 1u;
        const U32 h = (src.mHeight >> level) ? (U32)(src.mHeight >> level) : 1u;
        const U32 off = ssBC7LevelOffset(src.mWidth, src.mHeight, src.mMipCount, i);
        const U32 len = ssBC7LevelBytes(w, h);
        if ((size_t)off + len > src.mPayload.size()) return false;
        mip_crc[i] = crc32Of(src.mPayload.data() + off, len);
    }

    std::vector<U8> header;
    header.reserve(SSBC7_BLOB_HEADER_SIZE);
    {
        Writer w(header);
        w.pod<U32>(SSBC7_BLOB_MAGIC);
        w.uuid(src.mUUID);
        w.pod<U16>(src.mWidth);
        w.pod<U16>(src.mHeight);
        w.pod<U8>(0);                      // base discard, always zero while the store is full-res-only
        w.pod<U8>(src.mMipCount);
        w.pod<U8>(SSBC7_FMT_BC7_UNORM);
        w.pod<U8>(src.mSrcComponents);
        w.pod<U16>(src.mFlags);
        // <SS:Nexii/> Squeeze adaptive quality - the first of the two reserved0 bytes becomes the profile. It fits without moving one other field, which is the reason the reserved bytes were there.
        w.pod<U8>(src.mQuality);
        w.pod<U8>(0);                      // reserved0, one byte left
        w.pod<U32>(pickmask_bytes);
        w.pod<U32>(payload_bytes);
        w.pod<U32>(pickmask_bytes ? crc32Of(src.mPickMask.data(), pickmask_bytes) : 0);
        for (U32 i = 0; i < SSBC7_MAX_MIPS; ++i) w.pod<U32>(mip_crc[i]);
        w.pod<U32>(crc32Of(src.mPayload.data(), payload_bytes));
        w.pod<U32>(0);                     // reserved1
        w.pod<U32>(0);                     // reserved2
        w.pod<U32>(0);                     // reserved3
        w.pod<U32>(0);                     // reserved4
        w.pod<U32>(encoder_version);
        w.pod<U32>(crc32Of(header.data(), header.size()));   // header crc over the first 92 bytes
    }

    if (header.size() != SSBC7_BLOB_HEADER_SIZE) return false;

    out_blob.insert(out_blob.end(), header.begin(), header.end());
    out_blob.insert(out_blob.end(), src.mPayload.begin(), src.mPayload.end());
    if (pickmask_bytes) out_blob.insert(out_blob.end(), src.mPickMask.begin(), src.mPickMask.end());

    out_record = SSBC7Record();
    out_record.mUUID          = src.mUUID;
    out_record.mBlobSize      = (U32)out_blob.size();
    out_record.mPickMaskBytes = pickmask_bytes;
    out_record.mDataCRC       = crc32Of(out_blob.data(), out_blob.size());
    out_record.mWidth         = src.mWidth;
    out_record.mHeight        = src.mHeight;
    out_record.mFlags         = (U16)(src.mFlags | (pickmask_bytes ? SSBC7_FLAG_HAS_PICKMASK : 0));
    out_record.mBaseDiscard   = 0;
    out_record.mMipCount      = src.mMipCount;
    out_record.mFormat        = SSBC7_FMT_BC7_UNORM;
    out_record.mSrcComponents = src.mSrcComponents;
    out_record.mQuality       = src.mQuality;    // <SS:Nexii/> Squeeze adaptive quality - the index record and the blob header carry the same byte, so a rebuilt index knows what the upgrade pass still owes without reading a single payload
    return true;
}

bool ssBC7ParseBlobHeaderOnly(const U8* data, size_t size, SSBC7BlobHeader& out)
{
    if (!data || size < SSBC7_BLOB_HEADER_SIZE) return false;

    // The stored checksum covers the first 92 bytes, so a header that fails this is rejected before any of its length fields are trusted.
    const U32 stored_crc = crc32Of(data, SSBC7_BLOB_HEADER_SIZE - 4);

    Reader r(data, size);
    if (r.pod<U32>() != SSBC7_BLOB_MAGIC) return false;

    out.mUUID          = r.uuid();
    out.mWidth         = r.pod<U16>();
    out.mHeight        = r.pod<U16>();
    out.mBaseDiscard   = r.pod<U8>();
    out.mMipCount      = r.pod<U8>();
    out.mFormat        = r.pod<U8>();
    out.mSrcComponents = r.pod<U8>();
    out.mFlags         = r.pod<U16>();
    out.mQuality       = r.pod<U8>();           // <SS:Nexii/> Squeeze adaptive quality
    r.skip(1);                                  // reserved0, one byte left
    out.mPickMaskBytes = r.pod<U32>();
    out.mPayloadBytes  = r.pod<U32>();
    out.mPickMaskCRC   = r.pod<U32>();
    for (U32 i = 0; i < SSBC7_MAX_MIPS; ++i) out.mMipCRC[i] = r.pod<U32>();
    out.mPayloadCRC    = r.pod<U32>();
    r.skip(16);                                 // reserved1..4
    out.mEncoderVersion = r.pod<U32>();
    const U32 header_crc = r.pod<U32>();

    if (!r.ok()) return false;
    if (header_crc != stored_crc) return false;
    if (out.mMipCount == 0 || out.mMipCount > SSBC7_MAX_MIPS) return false;
    if (out.mWidth == 0 || out.mHeight == 0) return false;

    // The declared sizes must agree with the geometry, or a crafted header could make a reader walk past the end of the segment.
    if (out.mPayloadBytes != ssBC7PayloadBytes(out.mWidth, out.mHeight, out.mMipCount)) return false;

    return true;
}

bool ssBC7ParseBlobHeader(const U8* data, size_t size, SSBC7BlobHeader& out)
{
    if (!ssBC7ParseBlobHeaderOnly(data, size, out)) return false;

    // The whole-blob contract, kept here rather than in the header parse so a deliberately truncated prefix read can share every other check without being able to weaken this one for the callers that do have the complete blob.
    if ((U64)SSBC7_BLOB_HEADER_SIZE + out.mPayloadBytes + out.mPickMaskBytes > (U64)size) return false;

    return true;
}

// <SS:Nexii> Squeeze read path
U32 ssBC7PrefixBytes(U32 width, U32 height, U32 mip_count, U32 levels)
{
    if (levels == 0 || mip_count == 0 || mip_count > SSBC7_MAX_MIPS || levels > mip_count) return 0;

    // ssBC7LevelOffset(levels) is by definition the sum of every level BEFORE store index `levels`, which is the same number as the size of the first `levels` levels. The whole-chain case has to come from ssBC7PayloadBytes because ssBC7LevelOffset refuses an index at or past the end.
    if (levels == mip_count) return ssBC7PayloadBytes(width, height, mip_count);
    return ssBC7LevelOffset(width, height, mip_count, levels);
}

bool ssBC7VerifyBlobPrefix(const U8* data, size_t size, const LLUUID& expect_uuid, U32 levels)
{
    SSBC7BlobHeader h;
    if (!ssBC7ParseBlobHeaderOnly(data, size, h)) return false;

    // Checked first and unconditionally, for the reason spelled out on ssBC7VerifyBlob: internal consistency is a property a blob for a completely different texture also has.
    if (h.mUUID != expect_uuid) return false;

    if (levels == 0 || levels > h.mMipCount) return false;

    const U32 prefix = ssBC7PrefixBytes(h.mWidth, h.mHeight, h.mMipCount, levels);
    if (prefix == 0) return false;
    if ((U64)SSBC7_BLOB_HEADER_SIZE + prefix > (U64)size) return false;

    // Per-level checksums are what make a short read verifiable at all: the payload CRC covers bytes this read deliberately never fetched, so checking it would reject every prefix. Levels are indexed in STORE order, which is the order they were written in.
    const U8* payload = data + SSBC7_BLOB_HEADER_SIZE;
    for (U32 i = 0; i < levels; ++i)
    {
        const U32 level = h.mMipCount - 1 - i;
        const U32 w = (h.mWidth  >> level) ? (U32)(h.mWidth  >> level) : 1u;
        const U32 hh = (h.mHeight >> level) ? (U32)(h.mHeight >> level) : 1u;
        const U32 off = ssBC7LevelOffset(h.mWidth, h.mHeight, h.mMipCount, i);
        const U32 len = ssBC7LevelBytes(w, hh);
        if ((U64)off + len > (U64)prefix) return false;
        if (crc32Of(payload + off, len) != h.mMipCRC[i]) return false;
    }

    return true;
}
// </SS:Nexii>

bool ssBC7VerifyBlob(const U8* data, size_t size, const LLUUID& expect_uuid)
{
    SSBC7BlobHeader h;
    if (!ssBC7ParseBlobHeader(data, size, h)) return false;

    // <SS:Nexii> Squeeze eviction - checked FIRST and unconditionally. Every other check in this function only proves the bytes are internally consistent, which a blob for a completely different texture also is; once a segment id can be reclaimed and reused, that is the difference between serving the right texture and serving a well formed wrong one to anything holding an offset from before the kill.
    if (h.mUUID != expect_uuid) return false;
    // </SS:Nexii>

    const U8* payload = data + SSBC7_BLOB_HEADER_SIZE;
    if (crc32Of(payload, h.mPayloadBytes) != h.mPayloadCRC) return false;

    if (h.mPickMaskBytes)
    {
        const U8* mask = payload + h.mPayloadBytes;
        if (crc32Of(mask, h.mPickMaskBytes) != h.mPickMaskCRC) return false;
    }

    // Each level is checked independently as well, because serving a coarser discard reads only a prefix and must be able to trust just that prefix.
    for (U32 i = 0; i < h.mMipCount; ++i)
    {
        const U32 level = h.mMipCount - 1 - i;
        const U32 w = (h.mWidth  >> level) ? (U32)(h.mWidth  >> level) : 1u;
        const U32 hh = (h.mHeight >> level) ? (U32)(h.mHeight >> level) : 1u;
        const U32 off = ssBC7LevelOffset(h.mWidth, h.mHeight, h.mMipCount, i);
        const U32 len = ssBC7LevelBytes(w, hh);
        if (crc32Of(payload + off, len) != h.mMipCRC[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Record serialisation
// ---------------------------------------------------------------------------

void SSBC7Store::serializeRecord(const SSBC7Record& rec, U8* out)
{
    std::vector<U8> buf;
    buf.reserve(SSBC7_RECORD_SIZE);
    {
        Writer w(buf);
        w.uuid(rec.mUUID);
        w.pod<U64>(rec.mBlobOffset);
        w.pod<U32>(rec.mBlobSize);
        w.pod<U32>(rec.mPickMaskBytes);
        w.pod<U32>(rec.mDataCRC);
        w.pod<U16>(rec.mSegment);
        w.pod<U16>(rec.mWidth);
        w.pod<U16>(rec.mHeight);
        w.pod<U16>(rec.mLastUseDay);
        w.pod<U16>(rec.mFlags);
        w.pod<U8>(rec.mBaseDiscard);
        w.pod<U8>(rec.mMipCount);
        w.pod<U8>(rec.mFormat);
        w.pod<U8>(rec.mSrcComponents);
        // <SS:Nexii/> Squeeze adaptive quality - the fields above total fifty bytes and the checksum takes four, so ten of the sixty-four were spare and this spends one of them. Appended at the end of the written fields rather than beside mSrcComponents so that nothing already on disk moves, which is the entire reason the reserved run exists.
        w.pod<U8>(rec.mQuality);
        w.zeros(SSBC7_RECORD_SIZE - 4 - buf.size());   // reserved, so a later field can be added without moving anything already written
        w.pod<U32>(crc32Of(buf.data(), buf.size()));
    }
    memcpy(out, buf.data(), SSBC7_RECORD_SIZE);
}

bool SSBC7Store::deserializeRecord(const U8* in, SSBC7Record& out)
{
    // The checksum covers the first 60 bytes and is verified BEFORE any length field is believed, because a torn tail record is the normal outcome of a kill mid-append and must be rejected without following its offsets anywhere.
    const U32 expect = crc32Of(in, SSBC7_RECORD_SIZE - 4);

    Reader r(in, SSBC7_RECORD_SIZE);
    out = SSBC7Record();
    out.mUUID          = r.uuid();
    out.mBlobOffset    = r.pod<U64>();
    out.mBlobSize      = r.pod<U32>();
    out.mPickMaskBytes = r.pod<U32>();
    out.mDataCRC       = r.pod<U32>();
    out.mSegment       = r.pod<U16>();
    out.mWidth         = r.pod<U16>();
    out.mHeight        = r.pod<U16>();
    out.mLastUseDay    = r.pod<U16>();
    out.mFlags         = r.pod<U16>();
    out.mBaseDiscard   = r.pod<U8>();
    out.mMipCount      = r.pod<U8>();
    out.mFormat        = r.pod<U8>();
    out.mSrcComponents = r.pod<U8>();
    // <SS:Nexii> Squeeze adaptive quality - CLAMPED, never rejected. loadIndex stops at the first record that fails, so returning false here over a byte that only steers a background upgrade pass would throw away every record written after the bad one - for a field that has no bearing on whether the pixels are readable. An out of range value degrades to the best profile, which merely means the upgrade pass leaves that record alone.
    out.mQuality       = r.pod<U8>();
    if (out.mQuality >= (U8)SSBC7_QUALITY_LEVELS) out.mQuality = (U8)(SSBC7_QUALITY_LEVELS - 1);
    // </SS:Nexii>
    r.skip(SSBC7_RECORD_SIZE - 4 - r.tell());
    const U32 stored = r.pod<U32>();

    if (!r.ok() || stored != expect) return false;
    if (out.mSegment >= SSBC7_MAX_SEGMENTS) return false;
    if ((out.mBlobOffset & (SSBC7_ALIGN - 1)) != 0) return false;
    if (out.mBlobOffset + out.mBlobSize > SSBC7_SEGMENT_CAP) return false;
    out.mRecordCRC = stored;

    // A tombstone carries no geometry, so the geometry checks only apply to live records.
    if (!out.isTombstone())
    {
        if (out.mBlobSize < SSBC7_BLOB_HEADER_SIZE) return false;
        if (out.mWidth == 0 || out.mHeight == 0) return false;
        if (out.mMipCount == 0 || out.mMipCount > SSBC7_MAX_MIPS) return false;
        if (out.mSrcComponents < 1 || out.mSrcComponents > 4) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Container headers
//
// Built and parsed here rather than in the IO half so that every byte of the on-disk format lives in one
// translation unit. The values that decide whether a stored blob still means anything are passed in rather
// than looked up, which is what keeps this file free of viewer globals.
// ---------------------------------------------------------------------------

void SSBC7Store::buildIndexHeader(U32 encoder_version, U32 j2c_version, std::vector<U8>& out)
{
    out.clear();
    out.reserve(SSBC7_HEADER_SIZE);

    Writer w(out);
    w.pod<U32>(SSBC7_IDX_MAGIC);
    w.pod<U32>(SSBC7_FORMAT_VERSION);
    w.pod<U32>(encoder_version);
    w.pod<U32>(j2c_version);                    // the J2C cache is the source of truth, so a bump there invalidates everything derived from it
    w.pod<U32>((U32)(sizeof(void*) * 8));       // a 32 bit and a 64 bit viewer sharing a cache directory must not read each other's index
    w.pod<U32>(SSBC7_RECORD_SIZE);
    w.zeros(SSBC7_HEADER_SIZE - 4 - out.size());
    w.pod<U32>(crc32Of(out.data(), out.size()));
}

bool SSBC7Store::parseIndexHeader(const U8* data, U32 encoder_version, U32 j2c_version)
{
    // Everything a blob's meaning depends on is checked here: change any of it and every stored byte becomes uninterpretable, which is why the answer is always a wipe and never a migration.
    const U32 expect = crc32Of(data, SSBC7_HEADER_SIZE - 4);

    Reader r(data, SSBC7_HEADER_SIZE);
    const U32 magic          = r.pod<U32>();
    const U32 format_version = r.pod<U32>();
    const U32 encoder_ver    = r.pod<U32>();
    const U32 j2c_ver        = r.pod<U32>();
    const U32 address_size   = r.pod<U32>();
    const U32 record_size    = r.pod<U32>();
    r.skip(SSBC7_HEADER_SIZE - 4 - r.tell());
    const U32 stored         = r.pod<U32>();

    return r.ok()
        && stored == expect
        && magic == SSBC7_IDX_MAGIC
        && format_version == SSBC7_FORMAT_VERSION
        && encoder_ver == encoder_version
        && j2c_ver == j2c_version
        && address_size == (U32)(sizeof(void*) * 8)
        && record_size == SSBC7_RECORD_SIZE;
}

void SSBC7Store::buildSegmentHeader(U32 segment, std::vector<U8>& out)
{
    out.clear();
    out.reserve(SSBC7_SEG_HEADER_SIZE);

    Writer w(out);
    w.pod<U32>(SSBC7_SEG_MAGIC);
    w.pod<U32>(SSBC7_FORMAT_VERSION);
    w.pod<U32>(segment);
    w.zeros(SSBC7_SEG_HEADER_SIZE - 4 - out.size());
    w.pod<U32>(crc32Of(out.data(), out.size()));
}
