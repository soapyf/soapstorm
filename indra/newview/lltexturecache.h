/**
 * @file lltexturecache.h
 * @brief Object for managing texture cachees.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLTEXTURECACHE_H
#define LL_LLTEXTURECACHE_H

#include "lldir.h"
#include "llstl.h"
#include "llstring.h"
#include "lluuid.h"

#include "llworkerthread.h"

#include "ssstrata.h"   // <SS:Nexii/> Strata - SSStrataLooseFile is by value in the maintenance signatures below

#include <atomic>       // <SS:Nexii/> the J2C budget is movable mid-session by the arbiter, see sCacheMaxTexturesSize

class LLImageFormatted;
class LLTextureCacheWorker;
class LLImageRaw;

class LLTextureCache : public LLWorkerThread
{
    friend class LLTextureCacheWorker;
    friend class LLTextureCacheRemoteWorker;
    friend class LLTextureCacheLocalFileWorker;

private:

#if LL_WINDOWS
#pragma pack(push,1)
#endif

    // Entries
    static const U32 sHeaderEncoderStringSize = 32;
    struct EntriesInfo
    {
        EntriesInfo() : mVersion(0.f), mAdressSize(0), mEntries(0) { memset(mEncoderVersion, 0, sHeaderEncoderStringSize); }
        F32 mVersion;
        U32 mAdressSize;
        char mEncoderVersion[sHeaderEncoderStringSize];
        U32 mEntries;
    };
    struct Entry
    {
            Entry() :
                mBodySize(0),
            mImageSize(0),
            mTime(0)
        {
        }
        Entry(const LLUUID& id, S32 imagesize, S32 bodysize, U32 time) :
            mID(id), mImageSize(imagesize), mBodySize(bodysize), mTime(time) {}
        void init(const LLUUID& id, U32 time) { mID = id, mImageSize = 0; mBodySize = 0; mTime = time; }
        Entry& operator=(const Entry& entry) {mID = entry.mID, mImageSize = entry.mImageSize; mBodySize = entry.mBodySize; mTime = entry.mTime; return *this;}
        LLUUID mID; // 16 bytes
        S32 mImageSize; // total size of image if known
        S32 mBodySize; // size of body file in body cache
        U32 mTime; // seconds since 1/1/1970
    };

#if LL_WINDOWS
#pragma pack(pop)
#endif

public:

    class Responder : public LLResponder
    {
    public:
        virtual void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal) = 0;
    };

    class ReadResponder : public Responder
    {
    public:
        ReadResponder();
        void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal);
        void setImage(LLImageFormatted* image) { mFormattedImage = image; }
    protected:
        LLPointer<LLImageFormatted> mFormattedImage;
        S32 mImageSize;
        bool mImageLocal;
    };

    class WriteResponder : public Responder
    {
        void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal)
        {
            // not used
        }
    };

    LLTextureCache(bool threaded);
    ~LLTextureCache();

    /*virtual*/ size_t update(F32 max_time_ms);

    void purgeCache(ELLPath location, bool remove_dir = true);
    void setReadOnly(bool read_only) ;
    S64 initCache(ELLPath location, S64 maxsize, bool texture_cache_mismatch);

    handle_t readFromCache(const std::string& local_filename, const LLUUID& id, S32 offset, S32 size,
                           ReadResponder* responder);

    handle_t readFromCache(const LLUUID& id, S32 offset, S32 size,
                           ReadResponder* responder);
    bool readComplete(handle_t handle, bool abort);
    handle_t writeToCache(const LLUUID& id, const U8* data, S32 datasize, S32 imagesize, LLPointer<LLImageRaw> rawimage, S32 discardlevel,
                          WriteResponder* responder);
    LLPointer<LLImageRaw> readFromFastCache(const LLUUID& id, S32& discardlevel);
    bool writeComplete(handle_t handle, bool abort = false);
    void prioritizeWrite(handle_t handle);

    bool removeFromCache(const LLUUID& id);

    // For LLTextureCacheWorker::Responder
    LLTextureCacheWorker* getReader(handle_t handle);
    LLTextureCacheWorker* getWriter(handle_t handle);
    void lockWorkers() { mWorkersMutex.lock(); }
    void unlockWorkers() { mWorkersMutex.unlock(); }

    // debug
    S32 getNumReads() { return static_cast<S32>(mReaders.size()); }
    S32 getNumWrites() { return static_cast<S32>(mWriters.size()); }
    S64Bytes getUsage() { return S64Bytes(mTexturesSizeTotal); }
    S64Bytes getMaxUsage() { return S64Bytes(sCacheMaxTexturesSize.load()); }   // <SS:Nexii/> the budget is an atomic because the arbiter moves it mid-session, see ssSetTexturesBudget below
    U32 getEntries() { return mHeaderEntriesInfo.mEntries; }
    U32 getMaxEntries() { return sCacheMaxEntries; };
    bool isInCache(const LLUUID& id) ;
    bool isInLocal(const LLUUID& id) ; //not thread safe at the moment
    LLMutex* getFastCacheMutex() { return &mFastCacheMutex; }

    // <SS:Nexii> Squeeze - the BC7 sidecar store lives inside this directory, so it reads the name from here rather than recomputing it; two places deriving the same path independently is how a cache wipe ends up missing half of what it was meant to clear.
    const std::string& getTexturesDirName() const { return mTexturesDirName; }
    // </SS:Nexii>

    // <SS:Nexii> Strata - the J2C body tier. Every `texturecache/[0-f]/<uuid>.texture` file the viewer has ever written is a body, and on the machine this was built against there are 15,090 of them holding 1,255 MB. They are folded into a handful of volume files by a second Strata tenant; see doc/strata.md for why raising TEXTURE_CACHE_ENTRY_SIZE - the obvious cheap alternative - was measured and rejected.
    //
    // THE FOUR BODY HELPERS BELOW ARE THE WHOLE SEAM. Body files are touched in exactly six places in this file and nowhere else in the viewer, and routing all six through these keeps the Strata knowledge in one place instead of spreading a null check through the fetch state machine. Each one behaves exactly as the loose path did when the tier is off or the object was never packed.
    // `pool` is a REQUIRED parameter rather than a default, because which APR pool is safe here depends on the calling thread and the existing code already gets that right at every call site: the header mutex holders must use mHeaderAPRFilePoolp and the workers must use their own. Hiding the choice inside this helper is how the two would eventually be mixed up.
    S32  bodySize(const LLUUID& id, const std::string& filename, LLVolatileAPRPool* pool);               // packed record size, or the loose file's size, or 0
    // <SS:Nexii> Squeeze promotion - `pool` added for exactly the reason bodySize's comment above already gives. The loose fallback used to reach for this object's own LLThread pool, which is the CACHE THREAD's, and the promotion engine reads bodies from a BC7 pool worker; two threads sharing one LLVolatileAPRPool is the bug that comment exists to prevent. Passing null selects the process-wide pool, which is NOT safe off the main thread: LLVolatileAPRPool's mutex guards only its ref counts, not the apr pool underneath, so a background caller must pass a pool of its own.
    S32  readBody(const LLUUID& id, const std::string& filename, S32 offset, U8* dst, S32 bytes, LLVolatileAPRPool* pool);   // bytes read, or -1 for a hard failure
    // </SS:Nexii>
    bool writeBody(const LLUUID& id, const std::string& filename, const U8* src, S32 bytes);
    void forgetBody(const LLUUID& id);                                                                   // the object is going away; revoke any packed record so its bytes are eventually reclaimed

    // Runs on LLPurgeDiskCacheThread and nowhere else, once a minute. Folds settled body files into volumes and, if the volumes have outgrown their share, kills the coldest one and drops the entries that went with it.
    void strataMaintenance();

    // The budget arbiter's lever on this tier, and the reason doc/strata.md stage 2 needs no worker protocol at all: both purge paths and the Strata maintenance tick re-read sCacheMaxTexturesSize live, so moving this one variable moves the tier at the next pass without touching the documented mutex ordering and without any thread being told anything.
    //
    // `total_bytes` is the tier's WHOLE share, exactly as initCache's max_size is: the entry table's cost comes off it here so that the caller never has to know what a header entry costs. Called on the main thread only; the readers are relaxed loads on other threads, which is why the variable is atomic rather than a plain S64.
    void ssSetTexturesBudget(S64 total_bytes);
    // </SS:Nexii>

    // <SS:Nexii> Squeeze promotion - the promotion engine's only question of the J2C tier: is the whole asset already on this disk, and if it is, hand me the bytes. It lives here rather than as a reach into the entry table from outside because completeness is decided by the partial sentinel THIS file writes (total+1, lltexturefetch.cpp:2079-2088) and nothing else in the tree knows that rule.
    //
    // BACKGROUND THREADS ONLY. It does blocking file IO exactly as every LLTextureCacheWorker read does, so the frame thread must never call it. `pool` is required for the same reason bodySize's is; null gives the calling thread a pool of its own, because the process-wide one is not safe off the main thread.
    //
    // What is held under mHeaderMutex is ONE fixed-size entry read, the same one getHeaderCacheEntry already makes from the cache thread. The multi-megabyte body read happens with no lock held at all, because the caller is a background-QoS pool worker whose disk IO Windows deliberately deprioritises - holding the header mutex across that would let a deprioritised thread stall the texture cache's own thread.
    //
    // NO ENTRY IS MUTATED. Deliberately not routed through getHeaderCacheEntry, whose timestamp touch would make every texture the promotion scan merely LOOKED at rank as freshly used in the J2C LRU - a background sweep quietly rewriting the eviction order of the cache it is reading.
    enum ESSJ2CProbe
    {
        SS_J2C_PROBE_COMPLETE = 0,  // the whole asset is on disk; out_data holds it when bytes were asked for
        SS_J2C_PROBE_NO_ENTRY,      // this uuid was never cached here, or its entry has since been reclaimed, so there is nothing to promote and nothing to continue
        SS_J2C_PROBE_UNKNOWN,       // an entry exists but never recorded the asset's total size, so completeness cannot be decided in either direction
        SS_J2C_PROBE_PARTIAL,       // some bytes, not all - out_have and out_total say how far short, and this is the set network promotion exists for
        SS_J2C_PROBE_READ_FAILED,   // the entry claims bytes the disk did not produce
        SS_J2C_PROBE_RACED,         // the entry changed underneath the read, so the bytes in hand may belong to whatever texture took the index over
        SS_J2C_PROBE_COUNT
    };
    static const char* ssJ2CProbeName(ESSJ2CProbe probe);
    ESSJ2CProbe ssProbeJ2C(const LLUUID& id, bool want_bytes, std::vector<U8>& out_data, S32& out_have, S32& out_total, LLVolatileAPRPool* pool);
    // </SS:Nexii>
protected:
    // Accessed by LLTextureCacheWorker
    std::string getLocalFileName(const LLUUID& id);
    std::string getTextureFileName(const LLUUID& id);
    void addCompleted(Responder* responder, bool success);

protected:
    //void setFileAPRPool(apr_pool_t* pool) { mFileAPRPool = pool ; }

private:
    void setDirNames(ELLPath location);
    void readHeaderCache();
    void clearCorruptedCache();
    void purgeAllTextures(bool purge_directories);
    void purgeTexturesLazy(F32 time_limit_sec);
    void purgeTextures(bool validate);
    LLAPRFile* openHeaderEntriesFile(bool readonly, S32 offset);
    void closeHeaderEntriesFile();
    void readEntriesHeader();
    void setEntriesHeader();
    void writeEntriesHeader();
    S32 openAndReadEntry(const LLUUID& id, Entry& entry, bool create);
    bool updateEntry(S32& idx, Entry& entry, S32 new_image_size, S32 new_body_size);
    void updateEntryTimeStamp(S32 idx, Entry& entry) ;
    U32 openAndReadEntries(std::vector<Entry>& entries);
    void writeEntriesAndClose(const std::vector<Entry>& entries);
    void readEntryFromHeaderImmediately(S32& idx, Entry& entry) ;
    void writeEntryToHeaderImmediately(S32& idx, Entry& entry, bool write_header = false) ;
    void removeEntry(S32 idx, Entry& entry, std::string& filename);
    void removeCachedTexture(const LLUUID& id) ;
    S32 getHeaderCacheEntry(const LLUUID& id, Entry& entry);
    S32 setHeaderCacheEntry(const LLUUID& id, Entry& entry, S32 imagesize, S32 datasize);
    void writeUpdatedEntries() ;
    void updatedHeaderEntriesFile() ;
    void lockHeaders() { mHeaderMutex.lock(); }
    void unlockHeaders() { mHeaderMutex.unlock(); }

    // <SS:Nexii> Strata - the two halves of a maintenance tick, split so the lock discipline is visible: the first takes mHeaderMutex only long enough to copy a bounded slice of mTexturesSizeMap, the second takes it again to retire entries whose bodies died with a reclaimed volume. Neither introduces a new lock ordering - mHeaderMutex is the outermost lock in this class and Strata's own mutexes are leaves.
    bool strataCollectCandidates(std::vector<SSStrataLooseFile>& out);   // true when the rotation wrapped, which is the only moment a lap total is complete
    void strataDropEntries(const std::vector<LLUUID>& ids);
    // </SS:Nexii>

    void openFastCache(bool first_time = false);
    void closeFastCache(bool forced = false);
    bool writeToFastCache(LLUUID image_id, S32 cache_id, LLPointer<LLImageRaw> raw, S32 discardlevel);

private:
    // Internal
    LLMutex mWorkersMutex;
    LLMutex mHeaderMutex;
    LLMutex mHeaderIDMapMutex; // To avoid deadlocks, never lock mFastCacheMutex after mHeaderIDMapMutex.
    LLMutex mListMutex;
    LLMutex mFastCacheMutex;
    LLAPRFile* mHeaderAPRFile;
    LLVolatileAPRPool* mFastCachePoolp;

    // mLocalAPRFilePoolp is not thread safe and is meant only for workers
    // howhever mHeaderEntriesFileName is accessed not from workers' threads
    // so it needs own pool (not thread safe by itself, relies onto header's mutex)
    LLVolatileAPRPool*   mHeaderAPRFilePoolp;

    typedef std::map<handle_t, LLTextureCacheWorker*> handle_map_t;
    handle_map_t mReaders;
    handle_map_t mWriters;

    typedef std::vector<handle_t> handle_list_t;
    handle_list_t mPrioritizeWriteList;

    typedef std::vector<std::pair<LLPointer<Responder>, bool> > responder_list_t;
    responder_list_t mCompletedList;

    bool mReadOnly;

    std::string mCacheParentDirName;

    // HEADERS (Include first mip)
    std::string mHeaderEntriesFileName;
    std::string mHeaderDataFileName;
    std::string mFastCacheFileName;
    EntriesInfo mHeaderEntriesInfo;
    std::set<S32> mFreeList; // deleted entries
    std::set<LLUUID> mLRU;
    typedef std::map<LLUUID, S32> id_map_t;
    id_map_t mHeaderIDMap;

    LLAPRFile*   mFastCachep;
    LLFrameTimer mFastCacheTimer;
    U8*          mFastCachePadBuffer;

    // BODIES (TEXTURES minus headers)
    std::string mTexturesDirName;
    typedef std::map<LLUUID,S32> size_map_t;
    size_map_t mTexturesSizeMap;
    S64 mTexturesSizeTotal;
    LLAtomicBool mDoPurge;

    // <SS:Nexii> Strata - where the last maintenance tick stopped walking mTexturesSizeMap. A tick takes a bounded slice rather than the whole map, because building tens of thousands of paths under mHeaderMutex would stall the main thread's own purge behind it once a minute; the cursor is what makes the next tick continue rather than restart.
    LLUUID mStrataScanCursor;

    // Accumulated across the ticks of one lap and published only when the lap closes. A tick sees a slice, so
    // publishing its count directly would report "4,000 loose files" forever on a cache that has three - and a
    // metric that is permanently wrong is worse than one that updates every few minutes.
    U32    mStrataLapFiles{0};
    U64    mStrataLapBytes{0};
    // </SS:Nexii>

    typedef std::map<S32, Entry> idx_entry_map_t;
    idx_entry_map_t mUpdatedEntryMap;
    typedef std::vector<std::pair<S32, Entry> > idx_entry_vector_t;
    idx_entry_vector_t mPurgeEntryList;

    // Statics
    static F32 sHeaderCacheVersion;
    static U32 sHeaderCacheAddressSize;
    static std::string sHeaderCacheEncoderVersion;
    static U32 sCacheMaxEntries;
    // <SS:Nexii> Atomic because the budget arbiter moves this mid-session from the main thread while the two purge paths and the Strata maintenance tick read it from the cache and purge threads. Every access is relaxed on purpose: nothing is ordered against it, a reader that sees the old number for one pass simply purges to the old ceiling once more, and the alternative - a mutex around the eviction target - would be a new lock in the middle of the documented ordering for no correctness gained.
    static std::atomic<S64> sCacheMaxTexturesSize;
    // </SS:Nexii>
};

extern const S32 TEXTURE_CACHE_ENTRY_SIZE;

#endif // LL_LLTEXTURECACHE_H
