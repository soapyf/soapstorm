/**
 * @file ssbc7promotepolicy.cpp
 * @brief Squeeze promotion engine - the decisions, with no viewer attached, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7promote.h"

#include <algorithm>

// Everything in this file is deliberately free of gSavedSettings, gTextureList, the store and the thread pool, so the offline harness compiles and runs THIS source rather than a re-implementation of it. The three things worth getting wrong here - the partial sentinel arithmetic, which end of the want list is sacrificed, and whether a throttle can be talked into spending more than it was given - are all decided in this file and nowhere else.

// ---------------------------------------------------------------------------
// Verdict names
// ---------------------------------------------------------------------------

namespace
{
    const char* s_promote_verdict_names[SSBC7_PROMOTE_VERDICT_COUNT] =
    {
        "ran, local",
        "ran, scan posted",
        "ran, network",
        "idle, nothing wanted",
        "feature off",
        "store not ready",
        "encode pool busy",
        "scan already running",
        "post refused",
        "pinned bytes budget full",
        "store over budget",
        "network promotion off",
        "teleporting",
        "not logged in",
        "texture fetcher busy",
        "bandwidth throttle",
        "session byte cap reached",
        "continuation fetches at cap",
        "no eligible network target",
        "ran, re-encoding old records better",  // <SS:Nexii/> Squeeze adaptive quality
        "no spare worker",                      // <SS:Nexii/> Squeeze capacity-driven promotion
        "encoder not keeping up"
    };
}

const char* ssBC7PromoteVerdictName(ESSBC7PromoteVerdict verdict)
{
    if ((U32)verdict >= (U32)SSBC7_PROMOTE_VERDICT_COUNT) return "unknown";
    return s_promote_verdict_names[verdict];
}

// ---------------------------------------------------------------------------
// J2C completeness
// ---------------------------------------------------------------------------

ESSBC7J2CExtent ssBC7J2CExtent(S32 image_size, S32 body_size, S32 entry_size, S32& out_have, S32& out_total)
{
    out_have  = 0;
    out_total = 0;

    // A nonsense record size makes every answer below meaningless rather than merely wrong, so it is refused outright instead of producing a confident number.
    if (entry_size <= 0)  return SSBC7_J2C_UNKNOWN;

    // image_size is -1 for a brand-new entry the writer has not filled in yet and 0 when the fetch never learned the asset's total size - a UDP fetch that stopped before the header told it. Neither can be classified, and treating them as partial would send tier (b) chasing a length nobody knows.
    if (image_size <= 0)  return SSBC7_J2C_UNKNOWN;
    if (body_size  <  0)  return SSBC7_J2C_UNKNOWN;

    // The header record is written in full, padded with zeros, for every entry that exists - so a texture smaller than one record still counts as having a whole record's worth on disk and correctly reads as complete.
    const S64 have = (S64)entry_size + (S64)body_size;

    if (have >= (S64)image_size)
    {
        out_total = image_size;
        out_have  = image_size;
        return SSBC7_J2C_COMPLETE;
    }

    // The total reported here is one byte high for anything the fetch marked partial, because the sentinel IS total+1 and nothing on disk records which of the two it is. One byte of slop in a bandwidth estimate is not worth a second field to remove.
    out_total = image_size;
    out_have  = (S32)have;
    return SSBC7_J2C_PARTIAL;
}

// ---------------------------------------------------------------------------
// Want list
// ---------------------------------------------------------------------------

SSBC7WantList::SSBC7WantList(size_t cap)
:   mCap(cap ? cap : 1),
    mDropped(0)
{
}

bool SSBC7WantList::add(const LLUUID& id, LLUUID& out_dropped, bool& out_did_drop)
{
    out_did_drop = false;

    if (id.isNull()) return false;
    if (mIndex.find(id) != mIndex.end()) return false;

    // Make room BEFORE inserting, so the list is never momentarily one over its cap and a caller reading size() from another pass can never see a number the cap says is impossible.
    while (mIndex.size() >= mCap && !mOrder.empty())
    {
        out_dropped  = mOrder.front();
        out_did_drop = true;
        mIndex.erase(mOrder.front());
        mOrder.pop_front();
        ++mDropped;
    }

    mOrder.push_back(id);
    mIndex[id] = std::prev(mOrder.end());
    return true;
}

bool SSBC7WantList::erase(const LLUUID& id)
{
    auto it = mIndex.find(id);
    if (it == mIndex.end()) return false;

    mOrder.erase(it->second);
    mIndex.erase(it);
    return true;
}

bool SSBC7WantList::contains(const LLUUID& id) const
{
    return mIndex.find(id) != mIndex.end();
}

size_t SSBC7WantList::take(size_t max_count, std::vector<LLUUID>& out)
{
    out.clear();
    out.reserve(std::min(max_count, mIndex.size()));

    while (out.size() < max_count && !mOrder.empty())
    {
        const LLUUID id = mOrder.back();
        mOrder.pop_back();
        mIndex.erase(id);
        out.push_back(id);
    }
    return out.size();
}

void SSBC7WantList::snapshot(std::vector<LLUUID>& out) const
{
    out.clear();
    out.reserve(mOrder.size());
    for (const LLUUID& id : mOrder) out.push_back(id);
}

// ---------------------------------------------------------------------------
// Network budget
// ---------------------------------------------------------------------------

SSBC7NetBudget::SSBC7NetBudget()
:   mRate(0),
    mSessionCap(0),
    mTokens(0),
    mSpent(0),
    mLastSecond(0.0),
    mStarted(false)
{
}

void SSBC7NetBudget::configure(U64 bytes_per_second, U64 session_cap_bytes)
{
    mRate       = bytes_per_second;
    mSessionCap = session_cap_bytes;

    // Held credit is clamped down the instant the rate is lowered, otherwise turning the slider from fast to slow would still let the next few passes spend at the old rate - which is the one moment a user is most explicitly asking for it not to.
    const U64 burst = mRate * SSBC7_PROMOTE_BURST_SECONDS;
    if (mTokens > burst) mTokens = burst;
}

void SSBC7NetBudget::advance(F64 now_seconds)
{
    if (!mStarted)
    {
        mLastSecond = now_seconds;
        mStarted    = true;
        return;
    }

    // A clock that jumped backwards - a resync, a suspend, a DST step - resets the reference rather than accruing a negative or an enormous positive. Losing a second of credit costs nothing; gaining an hour of it would spend the ceiling in one pass.
    if (now_seconds < mLastSecond)
    {
        mLastSecond = now_seconds;
        return;
    }

    const F64 elapsed = now_seconds - mLastSecond;
    mLastSecond = now_seconds;

    if (mRate == 0)
    {
        mTokens = 0;
        return;
    }

    const F64 gained = elapsed * (F64)mRate;
    const U64 burst  = mRate * SSBC7_PROMOTE_BURST_SECONDS;

    if (gained >= (F64)burst) mTokens = burst;
    else                      mTokens = std::min<U64>(burst, mTokens + (U64)gained);
}

bool SSBC7NetBudget::canSpend(U64 bytes) const
{
    if (mRate == 0) return false;
    if (bytes == 0) return false;
    if (sessionCapReached()) return false;
    if (mSessionCap != 0 && mSpent + bytes > mSessionCap) return false;
    return mTokens >= bytes;
}

bool SSBC7NetBudget::spend(U64 bytes)
{
    if (!canSpend(bytes)) return false;
    mTokens -= bytes;
    mSpent  += bytes;
    return true;
}

void SSBC7NetBudget::refund(U64 bytes)
{
    const U64 burst = mRate * SSBC7_PROMOTE_BURST_SECONDS;
    mSpent  = (mSpent > bytes) ? (mSpent - bytes) : 0;
    mTokens = std::min<U64>(burst, mTokens + bytes);
}
