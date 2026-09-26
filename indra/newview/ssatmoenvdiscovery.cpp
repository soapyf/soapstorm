/**
 * @file ssatmoenvdiscovery.cpp
 * @brief See ssatmoenvdiscovery.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssatmoenvdiscovery.h"

#include "fslslbridge.h"
#include "llagent.h"
#include "llcorehttputil.h"
#include "llfilesystem.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llnotecard.h"
#include "llparcel.h"
#include "llsdserialize.h"
#include "ssatmoenvmanager.h"

#include <sstream>

namespace
{
    const char* CONFIG_TAG = "atmo:";
    const char* FETCH_COMMAND = "FetchNotecard|";
    const F32 BRIDGE_RETRY_SECONDS = 5.f;

    // <SS:Nexii> A fetch that goes out and never comes back (stale Bridge URL after a region crossing, coroutine dropped) leaves no callback to unlatch mPendingAssetId, so idle() expires it after this long and lets the normal retry path have another go.
    const F32 FETCH_TIMEOUT_SECONDS = 30.f;

    // <SS:Nexii> The Bridge answers 404 on a cold-notecard read timeout, which usually succeeds on the next try; a card that is genuinely gone would otherwise re-ask every BRIDGE_RETRY_SECONDS for the whole session, so give up after this many consecutive failures for the same id.
    const S32 FETCH_MAX_ATTEMPTS = 3;

    // Wraps fetched text in notecard format and caches it under the asset id, so later visits skip the Bridge.
    void cacheNotecardBody(const LLUUID& asset_id, const std::string& plain_body)
    {
        LLNotecard nc(LLNotecard::MAX_SIZE);
        nc.setText(plain_body);
        std::ostringstream wrapped;
        nc.exportStream(wrapped);
        const std::string wrapped_text = wrapped.str();

        LLFileSystem file(asset_id, LLAssetType::AT_NOTECARD, LLFileSystem::WRITE);
        file.write((const U8*)wrapped_text.data(), (S32)wrapped_text.size());
    }

    // Reads a cached notecard body back, unwrapping Linden notecard format when present.
    std::string readCachedNotecardBody(const LLUUID& asset_id)
    {
        if (!LLFileSystem::getExists(asset_id, LLAssetType::AT_NOTECARD)) return std::string();

        LLFileSystem file(asset_id, LLAssetType::AT_NOTECARD, LLFileSystem::READ);
        const S32 length = file.getSize();
        if (length <= 0) return std::string();

        std::vector<char> buffer(length + 1);
        file.read((U8*)buffer.data(), length);
        buffer[length] = '\0';

        std::string text(buffer.data(), length);
        if (length > 19 && strncmp(buffer.data(), "Linden text version", 19) == 0)
        {
            LLNotecard notecard;
            std::istringstream stream(text);
            if (!notecard.importStream(stream)) return std::string();
            text = notecard.getText();
        }
        return text;
    }
}

// Watches both parcel channels from construction on: the LLParcelObserver list fires on land selection (About Land description edits), gAgent's parcel-changed signal on agent parcel arrivals (login, teleport, border crossings) - processParcelProperties never notifies the observer list for those.
SSAtmoEnvDiscoveryManager::SSAtmoEnvDiscoveryManager()
{
    LLViewerParcelMgr::getInstance()->addObserver(this);
    mAgentParcelChangedConnection = gAgent.addParcelChangedCallback([this]() { changed(); });
}

// Stops watching; guarded because the parcel manager may already be gone at shutdown.
SSAtmoEnvDiscoveryManager::~SSAtmoEnvDiscoveryManager()
{
    mAgentParcelChangedConnection.disconnect();
    if (LLViewerParcelMgr::instanceExists())
    {
        LLViewerParcelMgr::getInstance()->removeObserver(this);
    }
}

// First frame: check the parcel that arrived during login, before this singleton existed; afterwards expire a fetch that never came back and retry a fetch the missing LSL Bridge deferred.
void SSAtmoEnvDiscoveryManager::idle()
{
    if (!mInitialCheckDone)
    {
        mInitialCheckDone = true;
        changed();
        return;
    }

    // <SS:Nexii> Belt and braces for the request that produces no callback at all: without this the pending latch is permanent and changed()'s `asset_id == mPendingAssetId` early-return holds this parcel's environment off for the session.
    if (mPendingAssetId.notNull() && mPendingTimer.getElapsedTimeF32() > FETCH_TIMEOUT_SECONDS)
    {
        const LLUUID stalled = mPendingAssetId;
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 fetch for " << stalled << " never returned within "
                                 << (S32)FETCH_TIMEOUT_SECONDS << "s; treating it as failed" << LL_ENDL;
        onFetchFailure(stalled, mPendingForce, mPendingSerial);
    }

    if (mDeferredAssetId.notNull() && mRetryTimer.getElapsedTimeF32() > BRIDGE_RETRY_SECONDS)
    {
        mRetryTimer.reset();
        const LLUUID asset_id = mDeferredAssetId;
        mDeferredAssetId.setNull();
        requestFetch(asset_id, mDeferredForce);
    }
}

// Finds the first valid 'atmo:<uuid>' marker in a parcel description.
LLUUID SSAtmoEnvDiscoveryManager::parseDescription(const std::string& desc)
{
    const std::string lower = utf8str_tolower(desc);
    const size_t tag_len = strlen(CONFIG_TAG);
    size_t pos = 0;

    while ((pos = lower.find(CONFIG_TAG, pos)) != std::string::npos)
    {
        size_t start = pos + tag_len;
        while (start < desc.size() && isspace((unsigned char)desc[start])) ++start;

        if (start + UUID_STR_SIZE <= desc.size())
        {
            const std::string candidate = desc.substr(start, UUID_STR_SIZE);
            if (LLUUID::validate(candidate))
            {
                return LLUUID(candidate);
            }
        }
        pos = start;
    }

    return LLUUID::null;
}

// The asset id the agent's current parcel advertises, if any.
LLUUID SSAtmoEnvDiscoveryManager::parcelAssetId()
{
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    return parseDescription(parcel ? parcel->getDesc() : LLStringUtil::null);
}

// The floater's Load From Parcel button: the deliberate, editor-visible version
// of what changed() does on its own. Already-live is a no-op so clicking twice
// cannot stomp unsaved edits, an Unload decline is lifted (the click IS the
// change of mind), and the fetch force-applies past the editor-open refusal.
bool SSAtmoEnvDiscoveryManager::loadFromParcel()
{
    const LLUUID asset_id = parcelAssetId();
    if (asset_id.isNull()) return false;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (asset_id == mAppliedAssetId && mgr->hasAsset()) return true;

    mDeclinedAssetId.setNull();
    requestFetch(asset_id, true);
    return true;
}

// Parcel changed: apply, refetch or unload the parcel environment - never while the editor floater is open.
void SSAtmoEnvDiscoveryManager::changed()
{
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const std::string desc = parcel ? parcel->getDesc() : LLStringUtil::null;
    const LLUUID asset_id = parseDescription(desc);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    const bool editing = editorIsOpen();

    // A fetch parked for the Bridge is only worth retrying while the parcel still advertises that same id.
    if (mDeferredAssetId.notNull() && mDeferredAssetId != asset_id)
    {
        mDeferredAssetId.setNull();
        mDeferredForce = false;
    }

    if (asset_id.isNull())
    {
        if (!editing && mgr->hasAsset() && mgr->cameFromParcel())
        {
            LL_INFOS("AtmoMagicEnv") << "Left the parcel that supplied the Atmo Magic"
                                        " environment; falling back to the EEP setting" << LL_ENDL;
            mgr->unload();
            mAppliedAssetId.setNull();
            mPendingAssetId.setNull();
        }
        // The parcel stopped advertising (or the agent left it): an unload by
        // hand no longer has to stick, a fresh arrival may apply again.
        mDeclinedAssetId.setNull();
        return;
    }

    // Still advertised, but the user declined it: keep the environment off
    // rather than resurrecting it on every parcel property update.
    if (asset_id == mDeclinedAssetId)
    {
        mDeferredAssetId.setNull();
        return;
    }
    // A different id than the declined one - the decline no longer applies.
    mDeclinedAssetId.setNull();

    if (asset_id == mAppliedAssetId && mgr->hasAsset())
    {
        return;
    }

    // The parcel still advertises the environment that was applied from it, but
    // none is live anymore: it was unloaded by hand (the environment floater).
    // Record the decline instead of falling through to a refetch - the cached
    // notecard would re-apply it silently, and the wind and rain ambiences the user
    // just unloaded would come straight back.
    if (asset_id == mAppliedAssetId && !mgr->hasAsset())
    {
        LL_INFOS("AtmoMagicEnv") << "Parcel still advertises the unloaded Atmo Magic"
                                    " environment; leaving it off until the parcel changes" << LL_ENDL;
        mDeclinedAssetId = asset_id;
        mAppliedAssetId.setNull();
        mPendingAssetId.setNull();
        return;
    }

    if (asset_id == mPendingAssetId) return;

    if (!editing && mgr->hasAsset() && mgr->cameFromParcel() && mAppliedAssetId != asset_id)
    {
        mgr->unload();
        mAppliedAssetId.setNull();
    }

    requestFetch(asset_id);
}

// Cache first, then the LSL Bridge; without a Bridge the environment simply stays unloaded.
// Forced fetches (the floater button) apply through the editor-open refusal; automatic ones
// never do.
void SSAtmoEnvDiscoveryManager::requestFetch(const LLUUID& asset_id, bool force)
{
    const std::string cached = readCachedNotecardBody(asset_id);
    if (!cached.empty())
    {
        mDeferredAssetId.setNull();
        // <SS:Nexii> An editor-open refusal stops here; a REJECTED cached body is poison (e.g. an old
        // Bridge's "ok" ack cached before validation) - drop it and fetch afresh so it can self-heal.
        if ((!force && editorIsOpen()) || applyText(asset_id, cached, force))
        {
            return;
        }
        LL_WARNS("AtmoMagicEnv") << "Cached Atmo v3 body for " << asset_id
                                 << " is invalid; discarding it and re-fetching" << LL_ENDL;
        LLFileSystem::removeFile(asset_id, LLAssetType::AT_NOTECARD);
    }

    if (!FSLSLBridge::instanceExists() || !FSLSLBridge::instance().canUseBridge())
    {
        // Not up yet is the normal login case (the parcel arrives seconds before the Bridge attaches): park the fetch for idle() to retry instead of giving up.
        if (mDeferredAssetId != asset_id)
        {
            LL_INFOS("AtmoMagicEnv") << "No SL Bridge available yet - deferring fetch of parcel-referenced "
                                       "Atmo v3 notecard " << asset_id << LL_ENDL;
        }
        mDeferredAssetId = asset_id;
        mDeferredForce = force;
        mRetryTimer.reset();
        return;
    }

    mDeferredAssetId.setNull();
    mPendingAssetId = asset_id;
    mPendingForce = force;
    mPendingTimer.reset();
    const U32 serial = ++mPendingSerial;

    // <SS:Nexii> Both callbacks run out of an HTTP coroutine that can outlive this singleton at shutdown, so neither captures `this` - they re-look the singleton up behind instanceExists(), the same guard the destructor uses for the parcel manager. The failure callback is the fix for the wedge: without one, any non-2xx (the Bridge's 404 on a cold-notecard read timeout) left mPendingAssetId latched and changed() refused to ever ask again.
    FSLSLBridge::instance().viewerToLSL(
        std::string(FETCH_COMMAND) + asset_id.asString(),
        [asset_id, force, serial](const LLSD& data)
        {
            if (SSAtmoEnvDiscoveryManager::instanceExists()) SSAtmoEnvDiscoveryManager::getInstance()->onFetchResult(asset_id, data, force, serial);
        },
        [asset_id, force, serial](const LLSD&)
        {
            if (SSAtmoEnvDiscoveryManager::instanceExists()) SSAtmoEnvDiscoveryManager::getInstance()->onFetchFailure(asset_id, force, serial);
        });
}

// A fetch that failed or stalled: unlatch the pending id so changed() can ask again, then park a retry on the same deferred/timer mechanism the Bridge-not-up case uses - capped, so a card that 404s forever stops asking.
void SSAtmoEnvDiscoveryManager::onFetchFailure(const LLUUID& asset_id, bool force, U32 serial)
{
    if (asset_id != mPendingAssetId || serial != mPendingSerial) return;    // an expired fetch answering late must not unlatch, or count against, the retry that replaced it
    mPendingAssetId.setNull();
    mPendingForce = false;

    if (mFailedAssetId != asset_id)
    {
        mFailedAssetId = asset_id;
        mFailedAttempts = 0;
    }
    ++mFailedAttempts;

    if (mFailedAttempts >= FETCH_MAX_ATTEMPTS)
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 fetch for " << asset_id << " failed " << mFailedAttempts
                                 << " times; giving up until the parcel changes" << LL_ENDL;
        return;
    }

    LL_WARNS("AtmoMagicEnv") << "Atmo v3 fetch for " << asset_id << " failed (attempt " << mFailedAttempts
                             << " of " << FETCH_MAX_ATTEMPTS << "); retrying in "
                             << (S32)BRIDGE_RETRY_SECONDS << "s" << LL_ENDL;
    mDeferredAssetId = asset_id;
    mDeferredForce = force;
    mRetryTimer.reset();
}

// Bridge reply: apply the fetched notecard text and cache it only once it applied; ignores stale replies.
void SSAtmoEnvDiscoveryManager::onFetchResult(const LLUUID& asset_id, const LLSD& data, bool force, U32 serial)
{
    if (asset_id != mPendingAssetId || serial != mPendingSerial) return;
    mPendingAssetId.setNull();
    mPendingForce = false;

    if (!data.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT))
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 fetch for " << asset_id << " returned no content" << LL_ENDL;
        return;
    }

    const LLSD& content = data[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT];

    std::string text;
    if (content.isMap())
    {
        std::ostringstream out;
        LLSDSerialize::toXML(content, out);
        text = out.str();
    }
    else
    {
        text = content.asString();
    }

    // <SS:Nexii> Cache only what actually applied - a Bridge ack or error reply must never poison the fetch cache.
    if (applyText(asset_id, text, force))
    {
        cacheNotecardBody(asset_id, text);
        // <SS:Nexii> A fetch that landed clears the failure budget, so a later transient 404 for this same id gets a fresh set of retries.
        if (mFailedAssetId == asset_id)
        {
            mFailedAssetId.setNull();
            mFailedAttempts = 0;
        }
    }
}

// The editor owns the environment while visible - discovery must not stomp an edit in progress.
bool SSAtmoEnvDiscoveryManager::editorIsOpen()
{
    LLFloater* floater = LLFloaterReg::findInstance("ss_atmo_env");
    return floater && floater->getVisible();
}

// Hands fetched text to the manager and records where it came from; refused while the editor
// is open unless a force fetch (the floater's own Load From Parcel button) carries it.
bool SSAtmoEnvDiscoveryManager::applyText(const LLUUID& asset_id, const std::string& text, bool force)
{
    if (!force && editorIsOpen())
    {
        LL_INFOS("AtmoMagicEnv") << "Atmo v3 environment " << asset_id
                                 << " available but not applied - floater is open" << LL_ENDL;
        return false;
    }

    const bool applied = SSAtmoEnvManager::getInstance()->applyExternalNotecardText(asset_id, text);

    if (applied)
    {
        mAppliedAssetId = asset_id;

        SSAtmoEnvManager::getInstance()->noteSource(asset_id, true);
    }
    else
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment " << asset_id << " fetched but rejected" << LL_ENDL;
    }
    return applied;
}
