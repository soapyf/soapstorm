/**
 * @file ssbc7manifestfile.cpp
 * @brief Squeeze region texture manifests - the file format and the cap policy, with no viewer attached, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7manifest.h"

#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "ssserial.h"

#include <algorithm>

// Everything in this file is deliberately free of gSavedSettings, gAgent, gTextureList, the store and the thread pool, so the offline harness compiles and runs THIS source rather than a re-implementation of it. The three things that would be expensive to get wrong live here and nowhere else: whether a torn file can be mistaken for a whole one, whether a corrupt length can become an allocation, and which end of a full manifest is sacrificed.

// ---------------------------------------------------------------------------
// Verdict names
// ---------------------------------------------------------------------------

namespace
{
    const char* s_manifest_verdict_names[SSBC7_MANIFEST_VERDICT_COUNT] =
    {
        "ok",
        "no manifest yet",
        "unreadable",
        "shorter than a header",
        "not a manifest",
        "written by a different version",
        "header checksum failed",
        "count past the cap",
        "truncated or torn",
        "payload checksum failed",
        "names a different region",
        "write failed"
    };
}

const char* ssBC7ManifestVerdictName(ESSBC7ManifestVerdict verdict)
{
    return (verdict >= 0 && verdict < SSBC7_MANIFEST_VERDICT_COUNT) ? s_manifest_verdict_names[verdict] : "unknown";
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

namespace
{
    // Written field by field rather than by struct punning for the same reason the store and the ROC write theirs that way: packing and endianness are then never left to the compiler, and the offline harness sees exactly the bytes the viewer wrote.
    void writeHeaderBytes(const SSBC7ManifestHeader& hdr, U32 count, U32 payload_crc, U8* out64)
    {
        std::vector<U8> tmp;
        tmp.reserve(SSBC7_MANIFEST_HEADER_SIZE);
        ssserial::Writer w(tmp);
        w.pod<U32>(SSBC7_MANIFEST_MAGIC);
        w.pod<U32>(SSBC7_MANIFEST_VERSION);
        w.pod<U64>(hdr.mRegionHandle);
        w.pod<U64>(hdr.mLastSeen);
        w.pod<U32>(count);
        w.pod<U32>(hdr.mDropped);
        w.pod<U32>(hdr.mVisits);
        w.pod<U32>(payload_crc);
        w.zeros(SSBC7_MANIFEST_HEADER_SIZE - 40);   // reserved tail INCLUDING the four checksum bytes sealRecord is about to overwrite, so the buffer is always exactly one header long before it is sealed

        memcpy(out64, tmp.data(), SSBC7_MANIFEST_HEADER_SIZE);
        ssserial::sealRecord(out64, SSBC7_MANIFEST_HEADER_SIZE);
    }

    // Reads the fixed header and checks everything that can be checked without the payload in hand. Split out because the directory LRU wants every manifest's mLastSeen and none of their uuids, and reading 80 KB per file to learn one timestamp would make the once-per-session prune cost more than the feature saves.
    ESSBC7ManifestVerdict parseHeaderBytes(const U8* data, size_t size, SSBC7ManifestHeader& out)
    {
        if (size < SSBC7_MANIFEST_HEADER_SIZE) return SSBC7_MANIFEST_DECLINE_SHORT;

        U32 magic = 0;
        memcpy(&magic, data, sizeof(U32));
        if (magic != SSBC7_MANIFEST_MAGIC) return SSBC7_MANIFEST_DECLINE_MAGIC;

        // NOTHING BELOW THE CHECKSUM IS BELIEVED BEFORE IT PASSES, and the count is why. A flipped bit in the count field of an otherwise plausible header is the one corruption that turns a bad file into a bad allocation, so the checksum is tested before the count is read rather than after. The magic above is checked first only because it makes "some other program's file" a distinguishable log line from "our file, damaged"; it is a hint, and it decides nothing.
        if (!ssserial::checkRecord(data, SSBC7_MANIFEST_HEADER_SIZE)) return SSBC7_MANIFEST_DECLINE_HEADER_CRC;

        ssserial::Reader r(data, SSBC7_MANIFEST_HEADER_SIZE);
        out.mMagic        = r.pod<U32>();
        out.mVersion      = r.pod<U32>();
        out.mRegionHandle = r.pod<U64>();
        out.mLastSeen     = r.pod<U64>();
        out.mCount        = r.pod<U32>();
        out.mDropped      = r.pod<U32>();
        out.mVisits       = r.pod<U32>();
        out.mPayloadCRC   = r.pod<U32>();
        if (!r.ok()) return SSBC7_MANIFEST_DECLINE_SHORT;

        // Manifests are DERIVED DATA and a version bump orphaning them is the established precedent in this tree, not a shortcut: the store itself wipes on encoder-version mismatch, and the whole cost of losing a manifest is one region visit spent re-learning it. Stated here rather than smuggled into a silent reformat.
        if (out.mVersion != SSBC7_MANIFEST_VERSION) return SSBC7_MANIFEST_DECLINE_VERSION;

        if (out.mCount > SSBC7_MANIFEST_MAX_ENTRIES) return SSBC7_MANIFEST_DECLINE_COUNT;

        return SSBC7_MANIFEST_OK;
    }
}

void ssBC7ManifestSerialise(const SSBC7ManifestHeader& hdr, const std::vector<SSBC7ManifestEntry>& entries, std::vector<U8>& out)
{
    const U32 count = (U32)std::min<size_t>(entries.size(), (size_t)SSBC7_MANIFEST_MAX_ENTRIES);

    std::vector<U8> payload;
    payload.reserve((size_t)count * SSBC7_MANIFEST_ENTRY_SIZE);
    {
        ssserial::Writer w(payload);
        for (U32 i = 0; i < count; ++i)
        {
            w.uuid(entries[i].mUUID);
            w.pod<U32>(entries[i].mWeight);
        }
    }

    const U32 payload_crc = ssserial::crc32Of(payload.empty() ? nullptr : payload.data(), payload.size());

    out.clear();
    out.resize(SSBC7_MANIFEST_HEADER_SIZE);
    writeHeaderBytes(hdr, count, payload_crc, out.data());
    out.insert(out.end(), payload.begin(), payload.end());
}

ESSBC7ManifestVerdict ssBC7ManifestParse(const U8* data, size_t size, SSBC7ManifestHeader& out_hdr, std::vector<SSBC7ManifestEntry>& out_entries)
{
    out_hdr = SSBC7ManifestHeader();
    out_entries.clear();

    SSBC7ManifestHeader hdr;
    const ESSBC7ManifestVerdict head = parseHeaderBytes(data, size, hdr);
    if (head != SSBC7_MANIFEST_OK) return head;

    // THE TORN WRITE CHECK, and it is an equality rather than an "at least". A file longer than its header claims is just as untrustworthy as a short one - it means a previous, larger manifest was overwritten in place by a shorter one and the tail survived - and reading the prefix would produce a manifest that checksums, loads, and describes a region the user has not been to.
    const size_t expect = (size_t)SSBC7_MANIFEST_HEADER_SIZE + (size_t)hdr.mCount * SSBC7_MANIFEST_ENTRY_SIZE;
    if (size != expect) return SSBC7_MANIFEST_DECLINE_TRUNCATED;

    const U8* payload = data + SSBC7_MANIFEST_HEADER_SIZE;
    const size_t payload_bytes = size - SSBC7_MANIFEST_HEADER_SIZE;
    if (ssserial::crc32Of(payload_bytes ? payload : nullptr, payload_bytes) != hdr.mPayloadCRC)
    {
        return SSBC7_MANIFEST_DECLINE_PAYLOAD_CRC;
    }

    std::vector<SSBC7ManifestEntry> entries;
    entries.reserve(hdr.mCount);
    {
        ssserial::Reader r(payload, payload_bytes);
        for (U32 i = 0; i < hdr.mCount; ++i)
        {
            SSBC7ManifestEntry e;
            e.mUUID   = r.uuid();
            e.mWeight = r.pod<U32>();
            if (!r.ok()) return SSBC7_MANIFEST_DECLINE_TRUNCATED;   // unreachable given the size equality above, and kept because the reader is the thing that actually knows
            if (e.mUUID.isNull()) continue;                          // a null uuid names nothing and would only be looked up and declined once per visit
            entries.push_back(e);
        }
    }

    out_hdr = hdr;
    out_entries.swap(entries);
    return SSBC7_MANIFEST_OK;
}

ESSBC7ManifestVerdict ssBC7ManifestRead(const std::string& path, U64 expect_handle, SSBC7ManifestHeader& out_hdr, std::vector<SSBC7ManifestEntry>& out_entries)
{
    out_hdr = SSBC7ManifestHeader();
    out_entries.clear();

    LLFILE* fp = LLFile::fopen(path, "rb");
    if (!fp) return SSBC7_MANIFEST_DECLINE_NO_FILE;

    // The size is taken from the handle rather than from a stat, so the bound applied below is the size of the bytes actually about to be read and not the size the file was a moment ago.
    if (fseek(fp, 0, SEEK_END) != 0) { LLFile::close(fp); return SSBC7_MANIFEST_DECLINE_UNREADABLE; }
    const long end = ftell(fp);
    if (end < 0) { LLFile::close(fp); return SSBC7_MANIFEST_DECLINE_UNREADABLE; }

    const size_t max_bytes = (size_t)SSBC7_MANIFEST_HEADER_SIZE + (size_t)SSBC7_MANIFEST_MAX_ENTRIES * SSBC7_MANIFEST_ENTRY_SIZE;
    if ((size_t)end > max_bytes) { LLFile::close(fp); return SSBC7_MANIFEST_DECLINE_COUNT; }
    if ((size_t)end < SSBC7_MANIFEST_HEADER_SIZE) { LLFile::close(fp); return SSBC7_MANIFEST_DECLINE_SHORT; }

    std::vector<U8> buf((size_t)end);
    if (fseek(fp, 0, SEEK_SET) != 0) { LLFile::close(fp); return SSBC7_MANIFEST_DECLINE_UNREADABLE; }
    const size_t got = fread(buf.data(), 1, buf.size(), fp);
    LLFile::close(fp);
    if (got != buf.size()) return SSBC7_MANIFEST_DECLINE_UNREADABLE;

    const ESSBC7ManifestVerdict verdict = ssBC7ManifestParse(buf.data(), buf.size(), out_hdr, out_entries);
    if (verdict != SSBC7_MANIFEST_OK) return verdict;

    // The filename is the ONLY thing tying a manifest to a region, and a filename is precisely the part of a cache that survives being copied, renamed or restored from a backup of somewhere else. Warming the wrong region would not fail loudly - it would quietly spend disk reads and video memory on textures nothing here uses - so the region is asserted rather than assumed.
    if (out_hdr.mRegionHandle != expect_handle)
    {
        out_hdr = SSBC7ManifestHeader();
        out_entries.clear();
        return SSBC7_MANIFEST_DECLINE_HANDLE;
    }

    return SSBC7_MANIFEST_OK;
}

ESSBC7ManifestVerdict ssBC7ManifestWrite(const std::string& path, const SSBC7ManifestHeader& hdr, const std::vector<SSBC7ManifestEntry>& entries)
{
    std::vector<U8> bytes;
    ssBC7ManifestSerialise(hdr, entries, bytes);

    // TEMPORARY PLUS RENAME, so the torn file the reader above is built to refuse is one this writer cannot produce. Refusing corrupt input is the safety net; not creating it is the plan. A failed write leaves the previous manifest exactly as it was, which is right - an older list of uuids is worth far more than no list.
    const std::string tmp = path + ".tmp";

    LLFILE* fp = LLFile::fopen(tmp, "wb");
    if (!fp) return SSBC7_MANIFEST_DECLINE_WRITE;

    const size_t put = fwrite(bytes.data(), 1, bytes.size(), fp);
    const bool flushed = (fflush(fp) == 0);
    LLFile::close(fp);

    if (put != bytes.size() || !flushed)
    {
        LLFile::remove(tmp, 1);
        return SSBC7_MANIFEST_DECLINE_WRITE;
    }

    // LLFile::rename does not replace on every platform's underlying call, so the old file goes first. The window between the two is the one moment a region has no manifest, and it costs exactly one un-warmed visit if the viewer dies inside it.
    LLFile::remove(path, 1);
    if (LLFile::rename(tmp, path, 1) != 0)
    {
        LLFile::remove(tmp, 1);
        return SSBC7_MANIFEST_DECLINE_WRITE;
    }

    return SSBC7_MANIFEST_OK;
}

std::string ssBC7ManifestFileName(const std::string& grid_id, U64 region_handle)
{
    // GRID SCOPED because SL and OpenSim region coordinates collide outright - the same handle names different places on different grids - and the grid id is folded to a safe alphabet rather than trusted, because it arrives from a login URI and ends up in a path.
    std::string safe;
    safe.reserve(grid_id.size());
    for (char c : grid_id)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe += ok ? c : '_';
    }
    if (safe.empty()) safe = "unknown";
    if (safe.size() > 48) safe.resize(48);

    // The handle IS the grid position: the high word is the global x in metres and the low word the global y, so dividing by 256 gives the region coordinates a person would recognise in a log line.
    const U32 gx = (U32)(region_handle >> 32) / 256;
    const U32 gy = (U32)(region_handle & 0xFFFFFFFFu) / 256;

    return llformat("r_%s_%u_%u%s", safe.c_str(), gx, gy, SSBC7_MANIFEST_EXT);
}

U32 ssBC7ManifestPruneDir(const std::string& dir, U32 keep)
{
    if (dir.empty()) return 0;

    struct Aged
    {
        std::string mPath;
        U64         mLastSeen;
    };

    std::vector<Aged> kept;
    U32 removed = 0;

    LLDirIterator iter(dir, "r_*.uml");
    std::string name;
    while (iter.next(name))
    {
        const std::string path = gDirUtilp->add(dir, name);

        U8 head[SSBC7_MANIFEST_HEADER_SIZE] = {0};
        size_t got = 0;
        if (LLFILE* fp = LLFile::fopen(path, "rb"))
        {
            got = fread(head, 1, sizeof(head), fp);
            LLFile::close(fp);
        }

        SSBC7ManifestHeader hdr;
        if (got != sizeof(head) || parseHeaderBytes(head, got, hdr) != SSBC7_MANIFEST_OK)
        {
            // An unreadable manifest is removed rather than kept, because leaving it would let dead weight occupy a slot in the cap forever and would make the cap describe files rather than regions.
            LLFile::remove(path, 1);
            ++removed;
            continue;
        }

        kept.push_back(Aged{path, hdr.mLastSeen});
    }

    if (kept.size() <= (size_t)keep) return removed;

    // LRU by the manifests' OWN mLastSeen rather than by file mtime, generalising the VOCache mTime LRU. Their own timestamp is what a semantic eviction pin would later read, and a timestamp inside the file cannot be reset by a backup tool, a virus scanner or a copy between machines.
    std::sort(kept.begin(), kept.end(), [](const Aged& a, const Aged& b) { return a.mLastSeen < b.mLastSeen; });

    const size_t kill = kept.size() - (size_t)keep;
    for (size_t i = 0; i < kill; ++i)
    {
        LLFile::remove(kept[i].mPath, 1);
        ++removed;
    }

    return removed;
}

// ---------------------------------------------------------------------------
// The bounded per-region set
// ---------------------------------------------------------------------------

SSBC7ManifestSet::SSBC7ManifestSet(U32 cap)
:   mCap(cap ? cap : 1),
    mDropped(0)
{
}

void SSBC7ManifestSet::clear()
{
    mWeights.clear();
    // mDropped is NOT reset: it is a session total, and the whole point of it is that a readout can say "at least this many uuids were forgotten" across every region visited. Resetting it per region would make the number describe only the last place the user stood.
}

void SSBC7ManifestSet::note(const LLUUID& id, U32 weight)
{
    if (id.isNull()) return;

    U32& slot = mWeights[id];
    if (weight > slot) slot = weight;

    pruneToCap();
}

void SSBC7ManifestSet::mergeDecayed(const std::vector<SSBC7ManifestEntry>& in)
{
    for (const SSBC7ManifestEntry& e : in)
    {
        if (e.mUUID.isNull()) continue;
        const U32 decayed = e.mWeight >> 1;
        U32& slot = mWeights[e.mUUID];
        if (decayed > slot) slot = decayed;
    }

    pruneToCap();
}

void SSBC7ManifestSet::take(std::vector<SSBC7ManifestEntry>& out) const
{
    out.clear();
    out.reserve(mWeights.size());
    for (const auto& kv : mWeights) out.push_back(SSBC7ManifestEntry{kv.first, kv.second});

    // Heaviest first, uuid breaking the tie so the written file is a deterministic function of its contents rather than of unordered_map iteration order - which is what lets the harness compare two serialisations byte for byte.
    std::sort(out.begin(), out.end(),
              [](const SSBC7ManifestEntry& a, const SSBC7ManifestEntry& b)
              {
                  if (a.mWeight != b.mWeight) return a.mWeight > b.mWeight;
                  return a.mUUID < b.mUUID;
              });
}

void SSBC7ManifestSet::pruneToCap()
{
    // HYSTERESIS, because the prune is a sort and the insert is a hash. Pruning on every insert past the cap would turn a walk through a texture-heavy region into an O(n log n) sort per texture on the main thread; letting the set overshoot by an eighth and then cutting back to the cap amortises it to one sort per cap/8 insertions.
    const size_t slack = (size_t)mCap + (size_t)mCap / 8;
    if (mWeights.size() <= slack) return;

    std::vector<SSBC7ManifestEntry> ordered;
    take(ordered);

    for (size_t i = (size_t)mCap; i < ordered.size(); ++i)
    {
        mWeights.erase(ordered[i].mUUID);
        mLastDropped = ordered[i].mUUID;
        ++mDropped;
    }
}
