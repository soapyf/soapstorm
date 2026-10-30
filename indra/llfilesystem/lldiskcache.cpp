/**
 * @file lldiskcache.cpp
 * @brief The disk cache implementation.
 *
 * Note: Rather than keep the top level function comments up
 * to date in both the source and header files, I elected to
 * only have explicit comments about each function and variable
 * in the header - look there for details. The same is true for
 * description of how this code is supposed to work.
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2020, Linden Research, Inc.
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

#include "linden_common.h"
#include "llapp.h"
#include "llassettype.h"
#include "lldir.h"
#include "fsyspath.h"
#include <boost/filesystem.hpp>
#include <chrono>

#include "lldiskcache.h"
#include "ssstrata.h"   // <SS:Nexii/> Strata asset volumes

// <SS:Nexii> purge() keeps every path it collects as UTF-8, because the packer opens them through LLFile, which takes UTF-8. boost's path::string() returns the ANSI code page on Windows instead, so on a profile such as C:\Users\Jos\u00e9 the packer could not open a single loose file and Strata silently never packed anything. These two helpers are the only places a purge path crosses between the two encodings.
namespace
{
    std::string ss_utf8_path(const boost::filesystem::path& p)
    {
        return fsyspath(std::filesystem::path(p.native())).string();
    }

    boost::filesystem::path ss_native_path(const std::string& utf8)
    {
        return boost::filesystem::path(fsyspath(utf8).native());
    }
}
// </SS:Nexii>

 /**
  * The prefix inserted at the start of a cache file filename to
  * help identify it as a cache file. It's probably not required
  * (just the presence in the cache folder is enough) but I am
  * paranoid about the cache folder being set to something bad
  * like the users' OS system dir by mistake or maliciously and
  * this will help to offset any damage if that happens.
  */
static const std::string CACHE_FILENAME_PREFIX("sl_cache");

std::string LLDiskCache::sCacheDir;

// <FS:Ansariel> Optimize asset simple disk cache
static const char* subdirs = "0123456789abcdef";

LLDiskCache::LLDiskCache(const std::string& cache_dir,
                         const uintmax_t max_size_bytes,
                         const bool enable_cache_debug_info
// <FS:Beq> Add High/Low water mark support
                         ,const F32 highwater_mark_percent
                         ,const F32 lowwater_mark_percent
// </FS:Beq>
                         ) :
    mMaxSizeBytes(max_size_bytes),
    mEnableCacheDebugInfo(enable_cache_debug_info)
{
    sCacheDir = cache_dir;
    LLFile::mkdir(cache_dir);

    // <FS:Ansariel> Optimize asset simple disk cache
    for (S32 i = 0; i < 16; i++)
    {
        std::string dirname = cache_dir + gDirUtilp->getDirDelimiter() + subdirs[i];
        LLFile::mkdir(dirname);
    }
    // </FS:Ansariel>
    // <SS:Nexii> Strata - brought up BEFORE the static assets are copied in. prepopulateCacheWithStatic decides what to copy by asking whether the loose file exists, so the store has to be answering questions by then or it would re-copy every static asset on every startup.
    SSStrataStore::instance().initStore(cache_dir, (U64)max_size_bytes);
    // </SS:Nexii>

    // <FS:Beq> add static assets into the new cache after clear.
    // Only missing entries are copied on init, skiplist is setup
    // For everything we populate FS specific assets to allow future updates
    prepopulateCacheWithStatic();
    // </FS:Beq>
}

// WARNING: purge() is called by LLPurgeDiskCacheThread. As such it must
// NOT touch any LLDiskCache data without introducing and locking a mutex!

// Interaction through the filesystem itself should be safe. Let's say thread
// A is accessing the cache file for reading/writing and thread B is trimming
// the cache. Let's also assume using llifstream to open a file and
// boost::filesystem::remove are not atomic (which will be pretty much the
// case).

// Now, A is trying to open the file using llifstream ctor. It does some
// checks if the file exists and whatever else it might be doing, but has not
// issued the call to the OS to actually open the file yet. Now B tries to
// delete the file: If the file has been already marked as in use by the OS,
// deleting the file will fail and B will continue with the next file. A can
// safely continue opening the file. If the file has not yet been marked as in
// use, B will delete the file. Now A actually wants to open it, operation
// will fail, subsequent check via llifstream.is_open will fail, asset will
// have to be re-requested. (Assuming here the viewer will actually handle
// this situation properly, that can also happen if there is a file containing
// garbage.)

// Other situation: B is trimming the cache and A wants to read a file that is
// about to get deleted. boost::filesystem::remove does whatever it is doing
// before actually deleting the file. If A opens the file before the file is
// actually gone, the OS call from B to delete the file will fail since the OS
// will prevent this. B continues with the next file. If the file is already
// gone before A finally gets to open it, this operation will fail and the
// asset will have to be re-requested.

// <SS:Nexii> Strata - this function now drives THREE things off one directory walk: the pack pass that folds settled loose files into volumes, the accounting that makes the budget cover both tiers, and the drain that takes the cache back under its watermark. The walk is the expensive part on a cache of this size, and giving the packer its own would have cost more than the container saves.
//
// The drain is the only genuinely new policy here. Loose files are individually deletable and a volume is not, so the two tiers are drained oldest-first against each other: the coldest sealed volume is offered its own bytes-weighted mean use day against the mtime of the oldest loose file still standing, and whichever is older goes. The store declines by NAME - TOO_YOUNG, ALL_HOT, COOLDOWN - so a purge that could not free anything says which of those it was instead of silently doing nothing.
void LLDiskCache::purge()
{
    LL_PROFILE_ZONE_SCOPED;

    if (mEnableCacheDebugInfo)
    {
        LL_INFOS() << "Total dir size before purge is " << dirFileSize(sCacheDir) << LL_ENDL;
    }

    boost::system::error_code ec;
    auto start_time = std::chrono::high_resolution_clock::now();

    // <SS:Nexii> Publishes the walk's cost on EVERY exit, including the under-the-watermark early return below, which is the path this pass usually takes and the one the pre-existing timing never reached.
    //
    // Only the ASSET tier's walk is timed here. The whole-iteration figure is taken in LLPurgeDiskCacheThread::run instead, because the texture tier's pack and reclaim runs after this function returns on the same thread, and it is the larger of the two tenants - timing only this half would have reported the smaller share of the sweep as though it were all of it.
    struct SSStrataScanTimer
    {
        U32 mScanMs{0};
        U32 mScanFiles{0};

        ~SSStrataScanTimer()
        {
            SSStrataStore* pub = SSStrataStore::live();
            if (!pub) return;
            pub->metrics().mLastScanFiles = mScanFiles;
            pub->metrics().mLastScanMs    = mScanMs;
        }
    } lap;

    // One entry per loose asset file, carrying everything both the packer and the drain need. This replaces the old file_info pair-of-pairs: the packer needs the uuid and the static-asset pin as well as the time, size and path, and threading four more parallel containers through the same loop is how those eventually disagree.
    std::vector<SSStrataLooseFile> loose;

#if LL_WINDOWS
    std::wstring cache_path(ll_convert<std::wstring>(sCacheDir));
#else
    std::string cache_path(sCacheDir);
#endif
    uintmax_t file_size_total = 0;

    if (boost::filesystem::is_directory(cache_path, ec) && !ec.failed())
    {
        boost::filesystem::recursive_directory_iterator iter(cache_path, ec);
        while (iter != boost::filesystem::recursive_directory_iterator() && !ec.failed())
        {
            if (!LLApp::isRunning())
            {
                return;
            }
            if (boost::filesystem::is_regular_file(*iter, ec) && !ec.failed())
            {
                const std::string file_path = ss_utf8_path((*iter).path());
                if (file_path.find(CACHE_FILENAME_PREFIX) != std::string::npos)
                {
                    // Both of these used to `continue` without advancing the iterator, so a single file whose size or timestamp could not be read spun this loop forever on the purge thread. Advancing first is what makes a per-file failure cost one file rather than the session.
                    const uintmax_t file_size = boost::filesystem::file_size(*iter, ec);
                    const bool size_ok = !ec.failed();
                    const std::time_t file_time = size_ok ? boost::filesystem::last_write_time(*iter, ec) : (std::time_t)0;
                    const bool time_ok = size_ok && !ec.failed();

                    if (time_ok)
                    {
                        file_size_total += file_size;

                        // The uuid and the static-asset pin are derived here, once, because the filename convention is LLDiskCache::metaDataToFilepath's business and Strata must not grow a second copy of it.
                        LLUUID id;
                        std::string uuid_as_string = gDirUtilp->getBaseFileName(file_path, true);
                        if (uuid_as_string.size() >= CACHE_FILENAME_PREFIX.size() + 1 + 36)
                        {
                            uuid_as_string = uuid_as_string.substr(CACHE_FILENAME_PREFIX.size() + 1, 36);
                            if (!id.set(uuid_as_string, false))
                            {
                                id.setNull();
                            }
                        }

                        const bool pinned = !id.isNull() && std::find(mSkipList.begin(), mSkipList.end(), uuid_as_string) != mSkipList.end();
                        loose.emplace_back(file_time, (U64)file_size, file_path, id, pinned);
                    }
                    else
                    {
                        ec.clear();
                    }
                }
            }
            iter.increment(ec);
        }
    }

    lap.mScanFiles = (U32)loose.size();
    lap.mScanMs    = (U32)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - start_time).count();

    // The pack pass. It marks every entry it consumed, so nothing below counts or deletes a file that is now inside a volume.
    SSStrataStore* strata = SSStrataStore::live();
    ESSStrataPackVerdict pack_verdict = SSSTRATA_PACK_DISABLED;
    uintmax_t packed_bytes = 0;
    U32 packed_files = 0;
    uintmax_t strata_bytes = 0;

    if (strata)
    {
        strata->setBudgetBytes((U64)mMaxSizeBytes);   // FSDiskCacheSize is movable mid-session through setMaxSizeBytes, and a budget read once at startup would ignore that
        pack_verdict = strata->packPass(loose);

        for (const SSStrataLooseFile& lf : loose)
        {
            if (!lf.mPacked) continue;
            packed_bytes += lf.mSize;
            ++packed_files;
        }
        file_size_total -= llmin(file_size_total, packed_bytes);
        strata_bytes = (uintmax_t)strata->allocatedBytes();
    }
    const uintmax_t governed_total = file_size_total + strata_bytes;

    // <SS:Nexii> Published from the same scan that builds the line below, because this is the number a user sees in Explorer and it was the one that made the feature look broken during a cold cache burst while only ever appearing in a log. Recomputing it for the overlay would mean a second full directory walk on the purge thread.
    if (SSStrataStore* pub = SSStrataStore::live())
    {
        pub->metrics().mLooseFiles = (U32)(loose.size() - packed_files);
        pub->metrics().mLooseBytes = (U64)file_size_total;
    }
    // </SS:Nexii>

    LL_INFOS("LLDiskCache") << "Strata pack: " << ssStrataPackVerdictName(pack_verdict) << ", folded " << packed_files << " files ("
                            << (packed_bytes / (1024 * 1024)) << " MB); loose now " << (loose.size() - packed_files) << " files / "
                            << (file_size_total / (1024 * 1024)) << " MB, volumes " << (strata_bytes / (1024 * 1024)) << " MB" << LL_ENDL;

    LL_DEBUGS("LLDiskCache") << "Cache is " << (S32)(((F32)governed_total) / (F32)mMaxSizeBytes * 100.0f) << "% full" << LL_ENDL;
    if (governed_total < (uintmax_t)(mMaxSizeBytes * (mHighPercent / 100)))
    {
        LL_DEBUGS("LLDiskCache") << "Not exceeded high water - do nothing" << LL_ENDL;
        updateCacheSize(governed_total);
        return;
    }

    // Oldest first, which is what lets the drain below take one loose file at a time in age order and compare each one against the coldest volume.
    std::sort(loose.begin(), loose.end(), [](const SSStrataLooseFile& x, const SSStrataLooseFile& y)
    {
        return x.mTime < y.mTime;
    });

    const uintmax_t target_size = (uintmax_t)(mMaxSizeBytes * (mLowPercent / 100));
    LL_INFOS("LLDiskCache") << "Purging cache to a maximum of " << target_size << " bytes" << LL_ENDL;

    // freed is what the budget no longer carries, which includes files that were already gone when we got to them. deleted_bytes and reclaimed_bytes are what this pass actually removed, so the summary never claims credit for a file somebody else deleted.
    uintmax_t freed = 0;
    uintmax_t deleted_bytes = 0;
    uintmax_t missing_bytes = 0;
    uintmax_t reclaimed_bytes = 0;
    U32 del = 0;
    U32 skip = 0;
    U32 failed = 0;
    U32 missing = 0;
    U32 volumes_killed = 0;

    // One byte per loose file, so the debug listing can say what happened to each one without keeping a second copy of every path.
    enum : U8 { SS_PURGE_KEEP = 0, SS_PURGE_DELETE, SS_PURGE_FAILED, SS_PURGE_MISSING };
    std::vector<U8> outcome(loose.size(), (U8)SS_PURGE_KEEP);
    size_t li = 0;
    bool first_reclaim = true;
    ESSStrataReclaimVerdict last_verdict = SSSTRATA_RECLAIM_NOT_NEEDED;

    // Asking the store on every single loose deletion would rescan its whole index each time, which on a full
    // cache is tens of thousands of records against a drain that can delete thousands of files. Two facts bound
    // it instead: the list is sorted oldest first, so the day being offered is non-decreasing and only a changed
    // day can turn a TOO_YOUNG into an eligible volume; and the verdicts that do not depend on the day at all -
    // every volume warm, nothing sealed to take, the tier switched off - cannot change within one pass. So the
    // store is consulted once per distinct day, once more after each kill, and never otherwise.
    U32 asked_day = 0x10000u;   // deliberately outside the U16 range so the first iteration always asks
    bool reclaim_exhausted = false;

    // Phrased as an addition rather than as a subtraction on purpose: both sides are unsigned, and freed can in principle reach governed_total, at which point a subtraction wraps to something enormous and this loop never ends.
    while (LLApp::isRunning() && governed_total > freed + target_size)
    {
        while (li < loose.size() && (loose[li].mPacked || loose[li].mPinned || loose[li].mPath.empty()))
        {
            if (loose[li].mPinned)
            {
                // A static asset is never dropped, and its timestamp is pushed forward so it sorts to the back next time rather than being re-examined at the head of every pass.
                boost::filesystem::last_write_time(ss_native_path(loose[li].mPath), std::time(nullptr), ec);
                ec.clear();
                ++skip;
            }
            ++li;
        }

        // 0xFFFF means there is no loose file left to compete with, so any volume the store is willing to give up is fair game.
        const U16 loose_day = (li < loose.size()) ? (U16)(loose[li].mTime / 86400) : (U16)0xFFFF;

        if (strata && !reclaim_exhausted && (U32)loose_day != asked_day)
        {
            asked_day = (U32)loose_day;

            U64 got = 0;
            last_verdict = strata->reclaimColdest(loose_day, first_reclaim, got);
            first_reclaim = false;

            if (last_verdict == SSSTRATA_RECLAIM_RAN)
            {
                freed += (uintmax_t)got;
                reclaimed_bytes += (uintmax_t)got;
                ++volumes_killed;
                asked_day = 0x10000u;   // the coldest volume is a different one now, so the same day is worth offering again
                continue;
            }

            // TOO_YOUNG is the only refusal a later, newer loose file can overturn. Everything else is a
            // property of the tier rather than of the day, so re-asking would only cost another index scan.
            if (last_verdict != SSSTRATA_RECLAIM_TOO_YOUNG) reclaim_exhausted = true;
        }
        else if (!strata)
        {
            last_verdict = SSSTRATA_RECLAIM_DISABLED;
            reclaim_exhausted = true;
        }

        if (li >= loose.size())
        {
            // Nothing left this pass can free. Saying WHY is the whole point of the verdict names: "over the watermark and every volume is still warm" and "over the watermark and the feature is off" must never look alike from a log.
            LL_INFOS("LLDiskCache") << "Purge stopped " << ((governed_total - freed) / (1024 * 1024)) << " MB above target: no loose files left and Strata declined with "
                                    << ssStrataReclaimVerdictName(last_verdict) << LL_ENDL;
            break;
        }

        SSStrataLooseFile& lf = loose[li];
        const bool removed = boost::filesystem::remove(ss_native_path(lf.mPath), ec);
        if (ec.failed())
        {
            // Locked or otherwise refused. The file is still on disk, so it stays in the budget and the drain moves on to the next candidate.
            LL_WARNS() << "Failed to delete cache file " << lf.mPath << ": " << ec.message() << LL_ENDL;
            ec.clear();
            outcome[li] = SS_PURGE_FAILED;
            ++failed;
        }
        else if (removed)
        {
            freed += lf.mSize;
            deleted_bytes += lf.mSize;
            outcome[li] = SS_PURGE_DELETE;
            ++del;
        }
        else
        {
            // Already gone - a concurrent remove got there between the scan and here. It no longer counts against the budget, but this pass did not delete it.
            freed += lf.mSize;
            missing_bytes += lf.mSize;
            outcome[li] = SS_PURGE_MISSING;
            ++missing;
        }
        ++li;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto execute_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    if (mEnableCacheDebugInfo)
    {
        // Logged afterwards so it does not affect the time measurement - logging thousands of file results can take hundreds of milliseconds.
        // The running total starts with any volume kills already applied, so the last line agrees with the governed size reported below.
        uintmax_t detail_remaining = governed_total > reclaimed_bytes ? governed_total - reclaimed_bytes : 0;
        for (size_t i = 0; i < loose.size(); ++i)
        {
            if (!LLApp::isRunning()) return;

            const SSStrataLooseFile& lf = loose[i];
            const char* action = "KEEP";
            if (lf.mPacked)                             action = "PACKED";
            else if (outcome[i] == SS_PURGE_DELETE)     action = "DELETE";
            else if (outcome[i] == SS_PURGE_FAILED)     action = "FAILED";
            else if (outcome[i] == SS_PURGE_MISSING)    action = "MISSING";
            else if (lf.mPinned)                        action = "STATIC";

            if (outcome[i] == SS_PURGE_DELETE || outcome[i] == SS_PURGE_MISSING)
            {
                detail_remaining -= llmin(detail_remaining, (uintmax_t)lf.mSize);
            }

            std::ostringstream line;
            line << action << "  " << lf.mTime << "  " << lf.mSize << "  " << lf.mPath
                 << " (" << detail_remaining << "/" << mMaxSizeBytes << ")";
            LL_INFOS() << line.str() << LL_ENDL;
        }
    }

    const size_t handled = (size_t)del + skip + failed + missing + packed_files;
    const uintmax_t newCacheSize = updateCacheSize(governed_total > freed ? governed_total - freed : 0);
    LL_INFOS("LLDiskCache") << "Total governed size after purge is " << newCacheSize << LL_ENDL;
    LL_INFOS("LLDiskCache") << "Cache purge took " << execute_time << " ms to execute for " << loose.size() << " loose files" << LL_ENDL;
    // This line's format is matched by the upstream tests in llfilesystem/tests, so the Strata-only figures go on a line of their own rather than into it.
    LL_INFOS("LLDiskCache") << "Deleted: " << del << " Skipped: " << skip << " Kept: " << (loose.size() > handled ? loose.size() - handled : (size_t)0)
                            << " Failed: " << failed << " Missing: " << missing << LL_ENDL;
    if (missing)
    {
        LL_INFOS("LLDiskCache") << "Already absent files account for " << missing_bytes << " bytes." << LL_ENDL;
    }
    if (newCacheSize > target_size)
    {
        LL_DEBUGS("LLDiskCache") << "Purge target not reached; protected, locked or still-warm data remains." << LL_ENDL;
    }
    LL_INFOS("LLDiskCache") << "Total of " << (deleted_bytes + reclaimed_bytes) << " bytes removed." << LL_ENDL;
    if (strata)
    {
        LL_INFOS("LLDiskCache") << "Strata: killed " << volumes_killed << " volumes (" << reclaimed_bytes << " bytes), " << packed_files << " files packed this pass" << LL_ENDL;
        LL_INFOS("LLDiskCache") << strata->metricsString() << LL_ENDL;
    }
}
// </SS:Nexii>


const std::string LLDiskCache::metaDataToFilepath(const LLUUID& id, LLAssetType::EType at)
{
    // <FS:Ansariel> Store assets in subfolders
    //return llformat("%s%s%s_%s_0.asset", sCacheDir.c_str(), gDirUtilp->getDirDelimiter().c_str(), CACHE_FILENAME_PREFIX.c_str(), id.asString().c_str());
    char id_string[36]{};
    return llformat("%s%s%c%s%s_%s_0.asset", sCacheDir.c_str(), gDirUtilp->getDirDelimiter().c_str(), id.toStringFast(id_string)[0], gDirUtilp->getDirDelimiter().c_str(), CACHE_FILENAME_PREFIX.c_str(), id.asString().c_str());
    // <FS:Ansariel>
}

const std::string LLDiskCache::getCacheInfo()
{
    LL_PROFILE_ZONE_SCOPED; // <FS:Beq/> add some instrumentation
    std::ostringstream cache_info;

    F32 max_in_mb = (F32)mMaxSizeBytes / (1024.0f * 1024.0f);
    // <FS:Beq> stall prevention. We still need to make sure this initialised when called at startup.
    F32 percent_used;
    if (mStoredCacheSize > 0)
    {
        percent_used = ((F32)mStoredCacheSize / (F32)mMaxSizeBytes) * 100.0f;
    }
    else
    {
        percent_used = ((F32)dirFileSize(sCacheDir) / (F32)mMaxSizeBytes) * 100.0f; 
    }
    // </FS:Beq>
    cache_info << std::fixed;
    cache_info << std::setprecision(1);
    cache_info << "Max size " << max_in_mb << " MB ";
    cache_info << "(" << percent_used << "% used)";
    if (SSStrataStore* strata = SSStrataStore::live())
    {
        cache_info << " [" << strata->statusString() << "]";
    }   // <SS:Nexii/> Strata - the volumes are most of the cache once packing has caught up, so leaving them out of the About box would make the reported number meaningless

    return cache_info.str();
}

// <FS:Beq> Copy static items into cache and add to the skip list that prevents their purging
// Note that there is no de-duplication nor other validation of the list.
void LLDiskCache::prepopulateCacheWithStatic()
{
    mSkipList.clear();

    std::vector<std::string> from_folders;
    from_folders.emplace_back(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "fs_static_assets"));
#ifdef OPENSIM
    from_folders.emplace_back(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "static_assets"));
#endif

    for (const auto& from_folder : from_folders)
    {
        if (gDirUtilp->fileExists(from_folder))
        {
            auto assets_to_copy = gDirUtilp->getFilesInDir(from_folder);
            for (auto from_asset_file : assets_to_copy)
            {
                from_asset_file = from_folder + gDirUtilp->getDirDelimiter() + from_asset_file;
                // we store static assets as UUID.asset_type the asset_type is not used in the current simple cache format
                auto uuid_as_string{ gDirUtilp->getBaseFileName(from_asset_file, true) };
                LLUUID uuid{ uuid_as_string };
                auto to_asset_file = metaDataToFilepath(uuid, LLAssetType::AT_UNKNOWN);
                if (!gDirUtilp->fileExists(to_asset_file))
                {
                    if (mEnableCacheDebugInfo)
                    {
                        LL_INFOS("LLDiskCache") << "Copying static asset " << from_asset_file << " to cache from " << from_folder << LL_ENDL;
                    }
                    if (!LLFile::copy(from_asset_file, to_asset_file))
                    {
                        LL_WARNS("LLDiskCache") << "Failed to copy " << from_asset_file << " to " << to_asset_file << LL_ENDL;
                    }
                }
                if (std::find(mSkipList.begin(), mSkipList.end(), uuid_as_string) == mSkipList.end())
                {
                    if (mEnableCacheDebugInfo)
                    {
                        LL_INFOS("LLDiskCache") << "Adding " << uuid_as_string << " to skip list" << LL_ENDL;
                    }
                    mSkipList.emplace_back(uuid_as_string);
                }
            }
        }
    }
}
// </FS:Beq>

void LLDiskCache::clearCache()
{
    LL_INFOS() << "clearing cache " << sCacheDir << LL_ENDL;
    /**
     * See notes on performance in dirFileSize(..) - there may be
     * a quicker way to do this by operating on the parent dir vs
     * the component files but it's called infrequently so it's
     * likely just fine
     */
    boost::system::error_code ec;
#if LL_WINDOWS
    std::wstring cache_path(ll_convert<std::wstring>(sCacheDir));
#else
    std::string cache_path(sCacheDir);
#endif
    if (boost::filesystem::is_directory(cache_path, ec) && !ec.failed())
    {
        // <FS:Ansariel> Optimize asset simple disk cache
        //boost::filesystem::directory_iterator iter(cache_path, ec);
        //while (iter != boost::filesystem::directory_iterator() && !ec.failed())
        boost::filesystem::recursive_directory_iterator iter(cache_path, ec);
        while (iter != boost::filesystem::recursive_directory_iterator() && !ec.failed())
        // </FS:Ansariel>
        {
            if (boost::filesystem::is_regular_file(*iter, ec) && !ec.failed())
            {
                if (ss_utf8_path((*iter).path()).find(CACHE_FILENAME_PREFIX) != std::string::npos)
                {
                    boost::filesystem::remove(*iter, ec);
                    if (ec.failed())
                    {
                        LL_WARNS() << "Failed to delete cache file " << *iter << ": " << ec.message() << LL_ENDL;
                    }
                }
            }
            iter.increment(ec);
        }
        // <FS:Beq> add static assets into the new cache after clear
    LL_INFOS() << "prepopulating new cache " << LL_ENDL;
        prepopulateCacheWithStatic();
    }

    // <SS:Nexii> Strata - outside the is_directory branch on purpose. The volumes live in a child directory of their own, so a cache directory that has gone missing is exactly the case where they still need wiping rather than the case where they can be skipped.
    SSStrataStore::instance().purgeAll();
    // </SS:Nexii>
    LL_INFOS() << "Cleared cache " << sCacheDir << LL_ENDL;
}

void LLDiskCache::removeOldVFSFiles()
{
    //VFS files won't be created, so consider removing this code later
    static const char CACHE_FORMAT[] = "inv.llsd";
    static const char DB_FORMAT[] = "db2.x";

    boost::system::error_code ec;
#if LL_WINDOWS
    std::wstring cache_path(ll_convert<std::wstring>(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "")));
#else
    std::string cache_path(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, ""));
#endif
    if (boost::filesystem::is_directory(cache_path, ec) && !ec.failed())
    {
        boost::filesystem::directory_iterator iter(cache_path, ec);
        while (iter != boost::filesystem::directory_iterator() && !ec.failed())
        {
            if (boost::filesystem::is_regular_file(*iter, ec) && !ec.failed())
            {
                const std::string file_name = ss_utf8_path((*iter).path());
                if ((file_name.find(CACHE_FORMAT) != std::string::npos) ||
                    (file_name.find(DB_FORMAT) != std::string::npos))
                {
                    boost::filesystem::remove(*iter, ec);
                    if (ec.failed())
                    {
                        LL_WARNS() << "Failed to delete cache file " << *iter << ": " << ec.message() << LL_ENDL;
                    }
                }
            }
            iter.increment(ec);
        }
    }
}

// <FS:Beq> Lets not scan every single time if we can avoid it eh?
// uintmax_t LLDiskCache::dirFileSize(const std::string& dir)
// {
uintmax_t LLDiskCache::updateCacheSize(const uintmax_t newsize)
{
    mStoredCacheSize = newsize;
    mLastScanTime = system_clock::now();
    return mStoredCacheSize;
}

uintmax_t LLDiskCache::dirFileSize(const std::string& dir, bool force)
{
    using namespace std::chrono;
    const seconds cache_duration{ 120 };// A rather arbitrary number. it takes 5 seconds+ on a fast drive to scan 80K+ items. purge runs every minute and will update. so 120 should mean we never need a superfluous cache scan.

    const auto current_time = system_clock::now();

    const auto time_difference = duration_cast<seconds>(current_time - mLastScanTime);

    // Check if the cached result can be used
    if( !force && time_difference < cache_duration )
    {
        LL_DEBUGS("LLDiskCache") << "Using cached result: " << mStoredCacheSize << LL_ENDL;
        return mStoredCacheSize;
    }
// </FS:Beq>
    uintmax_t total_file_size = 0;

    /**
     * There may be a better way that works directly on the folder (similar to
     * right clicking on a folder in the OS and asking for size vs right clicking
     * on all files and adding up manually) but this is very fast - less than 100ms
     * for 10,000 files in my testing so, so long as it's not called frequently,
     * it should be okay. Note that's it's only currently used for logging/debugging
     * so if performance is ever an issue, optimizing this or removing it altogether,
     * is an easy win.
     */
    boost::system::error_code ec;
#if LL_WINDOWS
    std::wstring dir_path(ll_convert<std::wstring>(dir));
#else
    std::string dir_path(dir);
#endif
    if (boost::filesystem::is_directory(dir_path, ec) && !ec.failed())
    {
        // <FS:Ansariel> Optimize asset simple disk cache
        //boost::filesystem::directory_iterator iter(dir_path, ec);
        //while (iter != boost::filesystem::directory_iterator() && !ec.failed())
        boost::filesystem::recursive_directory_iterator iter(dir_path, ec);
        while (iter != boost::filesystem::recursive_directory_iterator() && !ec.failed())
            // </FS:Ansariel>
        {
            if (boost::filesystem::is_regular_file(*iter, ec) && !ec.failed())
            {
                if (ss_utf8_path((*iter).path()).find(CACHE_FILENAME_PREFIX) != std::string::npos)
                {
                    uintmax_t file_size = boost::filesystem::file_size(*iter, ec);
                    if (!ec.failed())
                    {
                        total_file_size += file_size;
                    }
                }
            }
            iter.increment(ec);
        }
    }

// <FS:Beq> Lets not scan every single time if we can avoid it eh?
    // return total_file_size;
    // <SS:Nexii> Strata - a volume file is not named sl_cache_*, so the prefix filter above walks straight past it. Adding the tier here rather than loosening the filter keeps the prefix guard - which is what stands between a mis-set cache path and a directory walk over somebody's documents - exactly as strict as it was.
    if (SSStrataStore* strata = SSStrataStore::live())
    {
        total_file_size += (uintmax_t)strata->allocatedBytes();
    }
    // </SS:Nexii>
    return updateCacheSize(total_file_size);
// </FS:Beq>
}

std::function<void()> LLPurgeDiskCacheThread::sExtraMaintenance;   // <SS:Nexii/> Strata - the texture body tenant's once-a-minute tick, registered from newview

LLPurgeDiskCacheThread::LLPurgeDiskCacheThread() :
    LLThread("PurgeDiskCacheThread", nullptr)
{
}

void LLPurgeDiskCacheThread::run()
{
    constexpr std::chrono::seconds CHECK_INTERVAL{60};

    while (LLApp::instance()->sleep(CHECK_INTERVAL))
    {
        // <SS:Nexii/> Times the whole sweep, both tenants, because the texture tier below is the larger one and timing only the asset purge would report a fraction of the cost as the whole of it. The sleep above is a fixed gap taken before every pass, so this duration is what the sweep occupies of each (60s + duration) cycle - it is NOT a budget the work has to fit inside.
        const auto ss_lap_start = std::chrono::high_resolution_clock::now();

        LLDiskCache::instance().purge();

        // <SS:Nexii> Strata - AFTER the asset purge rather than before it, and in the same tick rather than on a timer of its own. Two tiers doing their heavy directory work at the same instant on the same disk is the one arrangement that makes background maintenance something the user can feel; serialising them costs nothing, because neither has a deadline. Any exception is swallowed here on purpose: a maintenance pass that threw must not take the thread down and silently stop the asset purge with it.
        if (sExtraMaintenance)
        {
            try
            {
                sExtraMaintenance();
            }
            catch (const std::exception& e)
            {
                LL_WARNS("Strata") << "The texture tier's maintenance tick threw: " << e.what() << "; the purge thread continues" << LL_ENDL;
            }
        }

        const auto ss_now = std::chrono::high_resolution_clock::now();

        if (SSStrataStore* pub = SSStrataStore::live())
        {
            const U32 lap_ms = (U32)std::chrono::duration_cast<std::chrono::milliseconds>(ss_now - ss_lap_start).count();
            pub->metrics().mLastLapMs = lap_ms;
            ++pub->metrics().mLaps;
            if (lap_ms >= 60000) ++pub->metrics().mSlowLaps;
        }

        // <SS:Nexii> The read RATE, per tenant, differenced across the whole cycle rather than across the sweep - the reads happen while the sweep is asleep, so dividing by the sweep duration would report a number many times too large. Everything below is on this one thread, so the snapshots can be plain statics.
        {
            static bool  ss_rate_primed = false;
            static std::chrono::high_resolution_clock::time_point ss_prev_at;
            static U32   ss_prev_reads[SSSTRATA_TENANT_COUNT] = { 0 };
            static U64   ss_prev_bytes[SSSTRATA_TENANT_COUNT] = { 0 };

            const F64 elapsed = ss_rate_primed
                ? std::chrono::duration<F64>(ss_now - ss_prev_at).count()
                : 0.0;

            for (U32 t = 0; t < SSSTRATA_TENANT_COUNT; ++t)
            {
                SSStrataStore* tier = SSStrataStore::live((ESSStrataTenant)t);
                if (!tier) continue;

                const U32 reads = tier->metrics().mReadsServed.load();
                const U64 bytes = tier->metrics().mReadBytes.load();

                if (ss_rate_primed && elapsed > 0.0)
                {
                    // Counters only ever climb, but a purgeAll resets the store, so a negative delta is clamped rather than wrapped into an enormous rate.
                    const U32 dr = (reads > ss_prev_reads[t]) ? (reads - ss_prev_reads[t]) : 0;
                    const U64 db = (bytes > ss_prev_bytes[t]) ? (bytes - ss_prev_bytes[t]) : 0;
                    tier->metrics().mReadsPerSec  = (U32)((F64)dr / elapsed);
                    tier->metrics().mReadKBPerSec = (U32)(((F64)db / 1024.0) / elapsed);
                }

                ss_prev_reads[t] = reads;
                ss_prev_bytes[t] = bytes;
            }

            ss_prev_at    = ss_now;
            ss_rate_primed = true;
        }
        // </SS:Nexii>
        // </SS:Nexii>
    }
}
