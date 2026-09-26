/**
 * @file ssatmolandscape.cpp
 * @brief Atmo Magic: the landscape world - owns the live scenery set.
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

#include "ssatmolandscape.h"

#include "llagent.h"
#include "llagentdata.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"

#include "llmeshrepository.h"
#include "llpermissionsflags.h"
#include "llselectmgr.h"
#include "rlvhandler.h"     // <SS:Nexii> the convert refuses up front when RLV would veto the derez
#include "rlvcommon.h"

#include "pipeline.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvmanager.h"

namespace
{
    // The capture throttle: the reconcile funnel writes the working asset at a few Hz, so a
    // mid-drag object churns the notecard save only when the author actually rests.
    constexpr F32 SS_LANDSCAPE_CAPTURE_INTERVAL = 0.25f;

    // <SS:Nexii> The conversion job's patience with the sim: object properties and task inventory
    // are a round trip per prim, and a 32-prim linkset on a busy sim is not instant. Past this the
    // job gives up (perms) or proceeds with a warning (contents).
    constexpr F32 SS_LANDSCAPE_CONVERT_TIMEOUT = 10.f;

    // One ObjectSelect re-send while the family's properties are still missing - a dropped select
    // would otherwise burn the whole timeout waiting for a reply that will never come.
    constexpr F32 SS_LANDSCAPE_CONVERT_NUDGE = 3.f;
}

void ss_landscape_persist_name(const LLUUID& record_id, const std::string& name, const std::string& desc)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return;
    }
    SSAtmoEnvAsset& asset = mgr->editable();

    // The object is always bound to the ACTIVE track's record while live, but the record
    // may sit in another track (estate layout reuse); find whichever track holds this id so
    // a rename is never silently dropped.
    SSAtmoEnvLandscape* found = nullptr;
    S32 found_track = -1;
    S32 active = SSAtmoEnvApplier::getInstance()->primaryTrackIndex();
    const S32 first_scan = (active >= 0 && active < (S32)asset.mTracks.size()) ? active : 0;
    const S32 tracks = (S32)asset.mTracks.size();
    for (S32 pass = 0; pass < tracks; ++pass)
    {
        const S32 t = (first_scan + pass) % tracks;
        for (SSAtmoEnvLandscape& record : asset.mTracks[static_cast<size_t>(t)].mLandscapes)
        {
            if (record.mRecordId == record_id)
            {
                found = &record;
                found_track = t;
                break;
            }
        }
        if (found)
        {
            break;
        }
    }
    if (!found)
    {
        return;
    }

    found->mName = name;
    found->mDesc = desc;
    const SSAtmoEnvLandscape copy = *found;

    // Refresh the live object's capture baseline (no placement re-apply - a rename must
    // never snap an in-flight transform). Only live objects - those are active-track.
    SSAtmoLandscapeWorld* world = SSAtmoLandscapeWorld::getInstance();
    for (S32 i = 0; i < world->objectCount(); ++i)
    {
        SSAtmoLandscapeObject* objp = world->objectAt(i);
        if (objp && objp->recordId() == record_id)
        {
            objp->syncAuthored(copy);
            break;
        }
    }
}

const SSAtmoEnvLandscape* ss_landscape_record_for(const LLUUID& record_id)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    if (!mgr->hasAsset() || !applier->isActive())
    {
        return nullptr;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        return nullptr;
    }
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    for (const SSAtmoEnvLandscape& record : records)
    {
        if (record.mRecordId == record_id)
        {
            return &record;
        }
    }
    return nullptr;
}

void ss_seed_local_select_node(LLSelectNode* nodep)
{
    if (!nodep || !nodep->getObject() || !nodep->getObject()->ssIsLocalContent())
    {
        return;
    }

    // Records are keyed by record id; root and child parts all carry it.
    const SSAtmoLandscapeObject* landscape = dynamic_cast<const SSAtmoLandscapeObject*>(nodep->getObject());
    const SSAtmoEnvLandscape* record = landscape
        ? ss_landscape_record_for(landscape->recordId()) : nullptr;
    if (!record)
    {
        return;
    }

    // The node is the stock editor's view of an object: seed what the sim would normally
    // reply with. Permission AND-masks make every stock gate treat the object as the
    // full-perm, agent-owned object it is.
    nodep->mValid = true;
    nodep->mName = record->mName;
    nodep->mDescription = record->mDesc;
    nodep->mCreationDate = (U64)record->mCreated;
    nodep->mPermissions->init(record->mCreator, gAgentID, record->mLastOwner, LLUUID::null);
    nodep->mPermissions->initMasks(PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL);
}

void SSAtmoLandscapeWorld::clearLandscapeObjects()
{
    mConvert.clear();    // <SS:Nexii> a parked conversion holds LLPointers to sim prims; they go with the objects, before gObjectList.destroy (LLWorld::resetClass)
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull())
        {
            killObject(objp.get());
        }
    }
    mObjects.clear();
}

// Kill a root and its parts: markDead cascades to the children, the root's LLPointers are dropped after.
void SSAtmoLandscapeWorld::killObject(SSAtmoLandscapeObject* rootp)
{
    if (!rootp) return;
    if (!rootp->isDead())
    {
        gObjectList.killObject(rootp);
    }
    rootp->dropChildParts();
}

// Whether the author's hand is on any scenery part (root or child).
bool SSAtmoLandscapeWorld::anySelected() const
{
    for (const LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.isNull() || objp->isDead()) continue;
        if (objp->isSelected()) return true;
        for (S32 i = 0; i < objp->childPartCount(); ++i)
        {
            const SSAtmoLandscapeObject* child = const_cast<SSAtmoLandscapeObject*>(objp.get())->childPart(i);
            if (child && !child->isDead() && child->isSelected()) return true;
        }
    }
    return false;
}

// The live root for a record id.
SSAtmoLandscapeObject* SSAtmoLandscapeWorld::rootForRecord(const LLUUID& record_id)
{
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull() && !objp->isDead() && objp->recordId() == record_id) return objp.get();
    }
    return nullptr;
}

void SSAtmoLandscapeWorld::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);
    static LLCachedControl<bool> landscape_enabled(gSavedSettings, "SSAtmoLandscape", false);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();

    // <SS:Nexii> The conversion job ticks BEFORE the active gate: it must still run the frame the feature is switched off, because that is when it has to fail out and drop its LLPointers to the source prims.
    tickConvert();

    const bool want_active = enabled && landscape_enabled && mgr->hasAsset() && applier->isActive();
    if (!want_active)
    {
        if (!mObjects.empty())
        {
            clearLandscapeObjects();
        }
        mLastSignature.clear();
        mAgentRegionHandle = 0;
        return;
    }

    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    // The author's hand is on a scenery object: defer reshapes (track/region/record-set
    // changes) until nothing is selected, so a mid-drag reconciliation cannot snap the
    // object back to the stale record. Capture continues - the drag is preserved at
    // capture cadence, and the deferred reshape then adopts the captured state.
    const bool editing = anySelected();

    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        track = 0;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;

    // Agent region change rebuilds the whole set: objects are anchored to a region origin
    // (locked records re-anchor by construction), and kill-and-recreate is the SSWaterWorld
    // idiom - the mesh repo's cache makes recreation cheap and pop-free.
    // <SS:Nexii> The objects' latest edits are flushed into their own records first (record-id paired, so a simultaneous track change cannot cross-write), otherwise the rebuild resurrects them from a record up to a capture tick stale and the scenery visibly snaps back - the "rubberband" after a move or rescale that ended right before a border crossing.
    const U64 handle = region->getHandle();
    if (!editing && handle != mAgentRegionHandle)
    {
        mAgentRegionHandle = handle;
        captureByRecordId(records);
        clearLandscapeObjects();
        mLastSignature.clear();
    }

    // The signature is the active track's record-id and part-count run PLUS the track index
    // itself: any reshape (track crossing, load, revert, floater add/delete/reorder, a linkset
    // gaining or losing parts) reshapes the live set; content edits inside a record (the
    // reconcile's own capture writes) deliberately do not.
    std::string sig;
    sig += llformat("t%d;", track);
    for (const SSAtmoEnvLandscape& r : records)
    {
        sig += r.mRecordId.asString();
        sig += llformat(":%d;", r.partCount());
    }
    if (!editing && sig != mLastSignature)
    {
        // <SS:Nexii> Same flush before a reshape: reconcile re-applies each surviving record to its object, and a record that missed the last capture tick would drag the object back to where it was before the edit.
        captureByRecordId(records);
        mLastSignature = sig;
        reconcile(asset, track, region);
    }

    applyFacesToAll();

    // Prune dead objects (killed by overflow kills or region teardown) so the live set
    // never accumulates corpses the floater's pairing would trip over.
    for (size_t oi = mObjects.size(); oi > 0; --oi)
    {
        if (mObjects[oi - 1].isNull() || mObjects[oi - 1]->isDead())
        {
            mObjects.erase(mObjects.begin() + (S32)(oi - 1));
        }
    }

    // Mesh availability feed for the floater's list: 404/purged assets surface as a
    // "(missing)" suffix (the object still renders the stock proxy box). A header that is
    // present but resolves to no LOD is a genuine 404; a header that has not arrived yet
    // is just "unknown".
    // <SS:Nexii> A linkset's marker is the aggregate over its mesh parts: unknown until every mesh part's header has answered, missing when any of them 404s; prim parts do not vote.
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.isNull() || objp->isDead())
        {
            continue;
        }
        bool all_known = true, all_available = true;
        for (S32 pi = -1; pi < objp->childPartCount(); ++pi)
        {
            SSAtmoLandscapeObject* partp = (pi < 0) ? objp.get() : objp->childPart(pi);
            if (!partp || partp->isDead()) continue;
            const LLUUID id = partp->partMeshId();
            if (id.isNull()) continue;
            const bool has_header = gMeshRepo.hasHeader(id);
            bool available = !has_header;
            if (has_header)
            {
                const LLVolumeParams params = partp->getVolume()->getParams();
                available = gMeshRepo.getActualMeshLOD(params, 0) != -1;
            }
            partp->setMeshAvailable(available, has_header);
            all_known = all_known && has_header;
            all_available = all_available && available;
        }
        objp->setMeshAvailable(all_available, all_known);
    }

    // Capture only while the live set matches the record set. While an object is selected a
    // reshape is deferred by design, and writing the OLD objects' state into the NEW track's
    // records would be cross-track contamination - so the capture skips that window and
    // resumes once the deferred reshape has adopted the new records.
    // <SS:Nexii> "Matches" is the signature test, not the selection: an edit made while the object is selected used to sit uncaptured until deselect, so the floater showed no unsaved changes and a save taken mid-edit missed the work. The reshape window is exactly the frames where sig != mLastSignature, and that is all the capture has to skip.
    const bool in_sync = (sig == mLastSignature);
    if (in_sync && mCaptureTimer.getElapsedTimeF32() > SS_LANDSCAPE_CAPTURE_INTERVAL)
    {
        mCaptureTimer.reset();
        captureAll(records);
    }
}

void SSAtmoLandscapeWorld::reconcile(const SSAtmoEnvAsset& asset, S32 track_index, LLViewerRegion* regionp)
{
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track_index)].mLandscapes;

    std::vector<LLPointer<SSAtmoLandscapeObject>> next;
    next.reserve(records.size());

    // Pairing consumes: each live object is matched at most once, in record order, so two
    // records holding the same mesh each get their own instance (and keep their own state)
    // instead of both collapsing onto the first match.
    std::vector<bool> taken(mObjects.size(), false);

    for (const SSAtmoEnvLandscape& record : records)
    {
        // Record-id adoption: a live linkset with the same id AND the same part count keeps its
        // instances and their loaded meshes, adopting the record's placement + part state - no
        // re-download, no rebuild pop. A linkset whose part count changed is rebuilt whole.
        SSAtmoLandscapeObject* match = nullptr;
        for (size_t oi = 0; oi < mObjects.size(); ++oi)
        {
            if (!taken[oi] && mObjects[oi].notNull() && !mObjects[oi]->isDead()
                && mObjects[oi]->recordId() == record.mRecordId
                && mObjects[oi]->childPartCount() + 1 == record.partCount())
            {
                match = mObjects[oi].get();
                taken[oi] = true;
                break;
            }
        }

        if (match)
        {
            match->applyRecord(record);
        }
        else
        {
            match = createObject(regionp, record);
        }
        next.push_back(match);
    }

    // Kill whatever did not survive the pairing - including a reshaped linkset's old instances.
    for (size_t oi = 0; oi < mObjects.size(); ++oi)
    {
        if (!taken[oi] && mObjects[oi].notNull())
        {
            killObject(mObjects[oi].get());
        }
    }

    mObjects = std::move(next);
}

// One root plus a real child object per further part, parented the way the sim's ObjectUpdate parents (addChild, then the drawable).
SSAtmoLandscapeObject* SSAtmoLandscapeWorld::createObject(LLViewerRegion* regionp, const SSAtmoEnvLandscape& record)
{
    LLUUID id;
    id.generate();

    SSAtmoLandscapeObject* rootp = new SSAtmoLandscapeObject(id, regionp, record, 0);
    if (!gObjectList.adoptViewerObject(rootp))
    {
        delete rootp;
        return nullptr;
    }
    gPipeline.createObject(rootp);

    for (S32 pi = 1; pi < record.partCount(); ++pi)
    {
        LLUUID cid;
        cid.generate();
        SSAtmoLandscapeObject* childp = new SSAtmoLandscapeObject(cid, regionp, record, pi);
        // <SS:Nexii> A part that cannot be built abandons the whole linkset: reconcile pairs on childPartCount() + 1 == partCount(), so a linkset short of one part would never be re-adopted and would be torn down and rebuilt on every signature change.
        if (!gObjectList.adoptViewerObject(childp))
        {
            delete childp;
            LL_WARNS("AtmoMagicLandscape") << "Landscape part " << pi << " could not be adopted; dropping the linkset" << LL_ENDL;
            killObject(rootp);
            return nullptr;
        }
        rootp->addChild(childp);
        gPipeline.createObject(childp);
        if (!childp->setDrawableParent(rootp->mDrawable))
        {
            LL_WARNS("AtmoMagicLandscape") << "Landscape part " << pi << " could not parent its drawable; dropping the linkset" << LL_ENDL;
            killObject(rootp);
            return nullptr;
        }
        childp->setPosition(record.mParts[(size_t)pi].mOffset);
        childp->setRotation(record.mParts[(size_t)pi].mRotation);
        gPipeline.markMoved(childp->mDrawable, false);
        rootp->adoptChild(childp);
    }
    return rootp;
}

void SSAtmoLandscapeWorld::applyFacesToAll()
{
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull() && !objp->isDead())
        {
            objp->applyFaces();
        }
    }
}

void SSAtmoLandscapeWorld::captureAll(std::vector<SSAtmoEnvLandscape>& records)
{
    // Reconcile builds mObjects in record order and capture only runs when the sets are
    // matched, so index pairing is exact - including for two records holding the same mesh,
    // which each keep their own instance and their own captured state. The size guard is a
    // belt-and-braces fallback to safe first-match pairing for any transient mismatch.
    if (mObjects.size() == records.size())
    {
        for (size_t i = 0; i < mObjects.size(); ++i)
        {
            if (mObjects[i].notNull())
            {
                mObjects[i]->captureToRecord(records[i]);
            }
        }
        return;
    }

    captureByRecordId(records);
}

// Each live root writes into the record carrying its id, if that record is in the list at all.
void SSAtmoLandscapeWorld::captureByRecordId(std::vector<SSAtmoEnvLandscape>& records)
{
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.isNull() || objp->isDead())
        {
            continue;
        }
        for (SSAtmoEnvLandscape& record : records)
        {
            if (record.mRecordId == objp->recordId())
            {
                objp->captureToRecord(record);
                break;
            }
        }
    }
}

// The save path's flush: capture immediately when the live set is in sync with the active track's records.
void SSAtmoLandscapeWorld::captureNow()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    if (mObjects.empty() || !mgr->hasAsset() || !applier->isActive()) return;
    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size()) return;
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    std::string sig;
    sig += llformat("t%d;", track);
    for (const SSAtmoEnvLandscape& r : records)
    {
        sig += r.mRecordId.asString();
        sig += llformat(":%d;", r.partCount());
    }
    if (sig != mLastSignature) return;    // a reshape is pending; capturing now would write the old objects into the new records
    mCaptureTimer.reset();
    captureAll(records);
}

S32 SSAtmoLandscapeWorld::recordCount() const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return 0;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return 0;
    }
    return (S32)asset.mTracks[static_cast<size_t>(track)].mLandscapes.size();
}

const SSAtmoEnvLandscape* SSAtmoLandscapeWorld::recordAt(S32 index) const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return nullptr;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return nullptr;
    }
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    if (index < 0 || index >= (S32)records.size())
    {
        return nullptr;
    }
    return &records[static_cast<size_t>(index)];
}

bool SSAtmoLandscapeWorld::toggleRecordLock(S32 index)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return true;
    }
    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return true;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    if (index < 0 || index >= (S32)records.size())
    {
        return true;
    }
    SSAtmoEnvLandscape& record = records[static_cast<size_t>(index)];

    // The paired object is resolved BEFORE the conversion because the conversion needs its
    // region. Index-paired like capture; the size-guarded fallback covers a transient mismatch.
    SSAtmoLandscapeObject* objp = nullptr;
    if (index >= 0 && index < (S32)mObjects.size())
    {
        objp = mObjects[static_cast<size_t>(index)].get();
    }
    if (!objp)
    {
        objp = rootForRecord(record.mRecordId);
    }

    // <SS:Nexii> review_loop2_fresh m3: mLockedOffset is region-local to the region the OBJECT is anchored to, which is not always the agent's - update() defers the region-change rebuild while a scenery object is selected, which is exactly when the lock button gets pressed, so converting against gAgent.getRegion() re-anchored the record 256 m off after a border crossing. Convert against the object's own region and only fall back to the agent's when there is no live object to ask.
    LLViewerRegion* anchor = objp ? objp->getRegion() : nullptr;
    if (!anchor)
    {
        anchor = gAgent.getRegion();
    }

    if (record.mLocked)
    {
        // Locked -> free: the global position is what the object is currently at.
        record.mFreeGlobal = anchor
            ? anchor->getPosGlobalFromRegion(record.mLockedOffset)
            : LLVector3d(record.mLockedOffset.mV[0], record.mLockedOffset.mV[1], record.mLockedOffset.mV[2]);
    }
    else
    {
        // Free -> locked: the region-local offset is where the object currently is.
        record.mLockedOffset = anchor
            ? anchor->getPosRegionFromGlobal(record.mFreeGlobal)
            : LLVector3((F32)record.mFreeGlobal.mdV[VX], (F32)record.mFreeGlobal.mdV[VY], (F32)record.mFreeGlobal.mdV[VZ]);
    }
    record.mLocked = !record.mLocked;

    // Re-apply so the object follows the new mode this frame - content edits do not trip the
    // reconcile signature, so this must apply directly.
    if (objp)
    {
        objp->applyRecord(record);
    }

    return record.mLocked;
}

// The feature's own gates, phrased for the user: the conversion refuses for the reasons a record would not hydrate.
bool SSAtmoLandscapeWorld::landscapeActive(std::string& out_reason) const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);
    static LLCachedControl<bool> landscape_enabled(gSavedSettings, "SSAtmoLandscape", false);
    if (!enabled)
    {
        out_reason = "Atmo Magic is switched off";
        return false;
    }
    if (!landscape_enabled)
    {
        out_reason = "Atmo Magic landscape is switched off (the SSAtmoLandscape setting)";
        return false;
    }
    if (!SSAtmoEnvManager::getInstance()->hasAsset())
    {
        out_reason = "no Atmo Magic environment is loaded";
        return false;
    }
    if (!SSAtmoEnvApplier::getInstance()->isActive())
    {
        out_reason = "the loaded environment is not driving the sky right now";
        return false;
    }
    if (!gAgent.getRegion())
    {
        out_reason = "no region";
        return false;
    }
    return true;
}

// The Convert selection entry: the synchronous gates, then the job starts and the per-frame tick takes over.
bool SSAtmoLandscapeWorld::beginConvertSelection(std::string& out_reason)
{
    out_reason.clear();
    if (convertBusy())
    {
        out_reason = "a conversion is already running";
        return false;
    }
    if (!landscapeActive(out_reason))
    {
        return false;
    }
    // <SS:Nexii> The derez at the end goes through selectDelete, which RLV can veto silently; refusing here keeps "record added, original still in world" from ever happening.
    if (rlv_handler_t::isEnabled() && !rlvCanDeleteOrReturn())
    {
        out_reason = "RLV is blocking object deletion, so the original could not be removed afterwards";
        return false;
    }

    // <SS:Nexii> The root is derived from whatever is selected, not from getRootObjectCount(): with Edit Linked Parts on, a selected child is an individual selection and the root count reads 0, yet the record is the whole build.
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* rootp = nullptr;
    if (selection.notNull())
    {
        for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
        {
            LLViewerObject* obj = (*it)->getObject();
            if (!obj || obj->isDead()) continue;
            LLViewerObject* r = obj->getRootEdit();
            if (!r) r = obj;
            if (rootp && r != rootp)
            {
                out_reason = "select exactly one object (one whole linkset) first";
                return false;
            }
            rootp = r;
        }
    }
    if (!rootp || rootp->isDead())
    {
        out_reason = "select exactly one object (one whole linkset) first";
        return false;
    }
    if (rootp->isAvatar() || rootp->getPCode() != LL_PCODE_VOLUME || !dynamic_cast<LLVOVolume*>(rootp))
    {
        out_reason = "only prim and mesh objects can become landscape";
        return false;
    }
    if (rootp->isAttachment())
    {
        out_reason = "an attachment cannot become landscape - rez it in world first";
        return false;
    }
    if (rootp->ssIsLocalContent())
    {
        out_reason = "that object is already Atmo Magic landscape";
        return false;
    }

    // <SS:Nexii> The whole LINKSET, not the selected nodes: Edit Linked Parts leaves one child selected and the record is the whole build. A seated avatar is a child of the root too, so non-volume children are skipped rather than captured as parts.
    mConvert.clear();
    ++mConvert.mGeneration;
    mConvert.mPrims.push_back(rootp);
    for (const LLPointer<LLViewerObject>& child : rootp->getChildren())
    {
        if (child.isNull() || child->isDead()) continue;
        if (child->isAvatar() || child->getPCode() != LL_PCODE_VOLUME) continue;
        mConvert.mPrims.push_back(child);
    }

    // The prim cap again up front: appendRecord enforces it too, but refusing now spares the
    // author two sim round trips for an answer already known.
    if ((S32)mConvert.mPrims.size() > SS_ATMOENV_MAX_LANDSCAPE_PARTS_PER_RECORD)
    {
        const S32 count = (S32)mConvert.mPrims.size();
        mConvert.clear();
        out_reason = llformat("that linkset has %d prims; a landscape record may have at most %d",
                              count, SS_ATMOENV_MAX_LANDSCAPE_PARTS_PER_RECORD);
        return false;
    }

    // <SS:Nexii> Only a real ObjectSelect makes the sim send ObjectProperties, and only ObjectProperties sets LLSelectNode::mValid - which is the permissions the conversion has to read. Select the whole family so every prim's reply is on its way; an already-complete family is left alone so the author's own selection is not disturbed.
    bool family_selected = true;
    for (const LLPointer<LLViewerObject>& p : mConvert.mPrims)
    {
        if (!selection->contains(p.get()))
        {
            family_selected = false;
            break;
        }
    }
    if (!family_selected)
    {
        LLSelectMgr::getInstance()->selectObjectAndFamily(rootp);
    }

    mConvert.mState = ESSConvertState::WAIT_PERMS;
    mConvert.mTimer.reset();
    return true;
}

// Any refusal after the job started: alert, drop the source LLPointers, back to idle. Nothing is ever derezzed on this path.
void SSAtmoLandscapeWorld::convertFail(const std::string& reason)
{
    mConvert.clear();
    LLNotificationsUtil::add("GenericAlert", LLSD().with(
        "MESSAGE", std::string("That selection could not become landscape: ") + reason));
}

// One state step per frame: the two sim waits, the user's answer, then the finish.
void SSAtmoLandscapeWorld::tickConvert()
{
    if (mConvert.mState == ESSConvertState::IDLE)
    {
        return;
    }

    // A prim dying under the job (derez, teleport, region teardown) would make the record half a
    // linkset - and the dialog may be up, so this runs in every state including CONFIRM.
    for (const LLPointer<LLViewerObject>& p : mConvert.mPrims)
    {
        if (p.isNull() || p->isDead())
        {
            convertFail("the object is no longer there");
            return;
        }
    }
    std::string reason;
    if (!landscapeActive(reason))
    {
        convertFail(reason);
        return;
    }

    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    const S32 prims = (S32)mConvert.mPrims.size();

    switch (mConvert.mState)
    {
    case ESSConvertState::WAIT_PERMS:
    {
        S32 valid = 0;
        for (const LLPointer<LLViewerObject>& p : mConvert.mPrims)
        {
            const LLSelectNode* node = selection.notNull() ? selection->findNode(p.get()) : nullptr;
            if (node && node->mValid) ++valid;
        }
        if (valid < prims)
        {
            if (!mConvert.mNudged && mConvert.mTimer.getElapsedTimeF32() > SS_LANDSCAPE_CONVERT_NUDGE)
            {
                mConvert.mNudged = true;
                LLSelectMgr::getInstance()->sendSelect();
            }
            if (mConvert.mTimer.getElapsedTimeF32() > SS_LANDSCAPE_CONVERT_TIMEOUT)
            {
                convertFail("could not read the object's permissions");
            }
            return;
        }

        // <SS:Nexii> Full permission on EVERY prim, from the OWNER mask: the environment notecard hands every reader a working copy of the whole linkset, so one no-transfer prim in a build would be handed out anyway. Owner verdict, doc/atmo_landscape/design_synthesis.md 15.
        for (S32 i = 0; i < prims; ++i)
        {
            LLViewerObject* p = mConvert.mPrims[(size_t)i].get();
            const LLSelectNode* node = selection.notNull() ? selection->findNode(p) : nullptr;
            const LLPermissions* perms = node ? node->mPermissions : nullptr;
            const U32 owner_mask = perms ? perms->getMaskOwner() : 0;
            const bool full = (owner_mask & PERM_ITEM_UNRESTRICTED) == PERM_ITEM_UNRESTRICTED;
            const bool mine = p->permYouOwner() && perms && perms->getOwner() == gAgentID;
            const std::string which = (node && !node->mName.empty())
                ? node->mName : llformat("prim %d", i + 1);
            if (!full || !mine)
            {
                convertFail(llformat("'%s' is not full permission and owned by you - every prim of a landscape"
                                     " linkset must be copy, modify and transfer", which.c_str()));
                return;
            }
            // <SS:Nexii> Locked prims fail permMove(), and selectDelete answers that with an asynchronous ConfirmObjectDeleteLock instead of its forceResponse path - the job would then have added the record and moved on while the dialog still pointed at the live selection, which by then is the NEW landscape. Refuse up front.
            if (!p->permMove())
            {
                convertFail(llformat("'%s' is locked - unlock every prim before converting", which.c_str()));
                return;
            }
        }

        // The root's metadata, taken now while every node is proven valid.
        if (const LLSelectNode* root_node = selection.notNull() ? selection->findNode(mConvert.mPrims[0].get()) : nullptr)
        {
            mConvert.mName = root_node->mName;
            mConvert.mDesc = root_node->mDescription;
            if (root_node->mPermissions)
            {
                mConvert.mCreator = root_node->mPermissions->getCreator();
                mConvert.mLastOwner = root_node->mPermissions->getLastOwner();
            }
            mConvert.mCreated = (F64)root_node->mCreationDate;
        }

        // Contents next: a local object has no simulator, so anything in a prim's inventory is
        // lost. Ask every prim; the reply is polled, never waited on.
        for (LLPointer<LLViewerObject>& p : mConvert.mPrims)
        {
            p->requestInventory();
        }
        mConvert.mState = ESSConvertState::WAIT_CONTENTS;
        mConvert.mTimer.reset();
        return;
    }

    case ESSConvertState::WAIT_CONTENTS:
    {
        // Arrival is a non-pending request with an inventory root: a prim with nothing in it
        // still gets the mocked-up "Contents" folder, so a null root means "not yet".
        S32 arrived = 0;
        for (LLPointer<LLViewerObject>& p : mConvert.mPrims)
        {
            if (!p->isInventoryPending() && p->getInventoryRoot() != nullptr) ++arrived;
        }
        const bool timed_out = mConvert.mTimer.getElapsedTimeF32() > SS_LANDSCAPE_CONVERT_TIMEOUT;
        if (arrived < prims && !timed_out)
        {
            return;
        }
        mConvert.mContentsUnknown = (arrived < prims);

        mConvert.mContentItems = 0;
        mConvert.mContentPrims = 0;
        for (LLPointer<LLViewerObject>& p : mConvert.mPrims)
        {
            LLInventoryObject::object_list_t items;
            p->getInventoryContents(items);
            if (!items.empty())
            {
                mConvert.mContentItems += (S32)items.size();
                ++mConvert.mContentPrims;
            }
        }

        // <SS:Nexii> Contents WARN, never block (owner verdict): the conversion is the author's own build, and the original linkset goes to the trash with its contents intact, so nothing is actually destroyed - it just stops being part of the scenery.
        if (mConvert.mContentItems > 0 || mConvert.mContentsUnknown)
        {
            mConvert.mState = ESSConvertState::CONFIRM;
            LLSD args;
            args["COUNT"] = mConvert.mContentItems;
            args["PRIMS"] = mConvert.mContentPrims;
            args["WARNING"] = mConvert.mContentsUnknown
                ? std::string("Some prims never reported their contents, so there may be more.")
                : std::string();
            // <SS:Nexii> The response functor outlives the click and can fire during logout, so it resolves the singleton instead of capturing this, and it re-checks the state: the job may have failed out from under the dialog (a prim died, the environment went away), and then the answer is not ours to act on.
            LLSD payload;
            payload["generation"] = (LLSD::Integer)mConvert.mGeneration;
            LLNotificationsUtil::add("SSAtmoLandscapeConvertContents", args, payload,
                [](const LLSD& notification, const LLSD& response)
                {
                    if (!SSAtmoLandscapeWorld::instanceExists())
                    {
                        return;
                    }
                    SSAtmoLandscapeWorld* world = SSAtmoLandscapeWorld::getInstance();
                    if (world->mConvert.mState != ESSConvertState::CONFIRM
                        || (U32)notification["payload"]["generation"].asInteger() != world->mConvert.mGeneration)
                    {
                        return;    // a stale dialog from an earlier job, or the job moved on
                    }
                    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                    {
                        world->mConvert.mState = ESSConvertState::FINISH;
                    }
                    else
                    {
                        world->mConvert.clear();
                    }
                });
            return;
        }
        mConvert.mState = ESSConvertState::FINISH;
        return;
    }

    case ESSConvertState::CONFIRM:
        // The user's answer drives the next move; the guards above still run each frame.
        return;

    case ESSConvertState::FINISH:
        convertFinish();
        return;

    case ESSConvertState::IDLE:
    default:
        return;
    }
}

// The payoff tick: capture and append, then derez the original and select what was made.
void SSAtmoLandscapeWorld::convertFinish()
{
    // tickConvert's guards already ran this frame: every prim is alive and the world is active.
    std::string reason;
    const S32 index = addFromSelection(mConvert.mPrims, reason);
    if (index < 0)
    {
        // Nothing is derezzed on a failed add - the author's build is left exactly as it was.
        convertFail(reason);
        return;
    }
    const SSAtmoEnvLandscape* record = recordAt(index);
    const LLUUID record_id = record ? record->mRecordId : LLUUID::null;

    // <SS:Nexii> The selection may have drifted while the job polled (the author clicked something else, or answered the contents dialog with a different object selected), and the derez below takes the WHOLE selection - so the source family is re-selected first and is then provably the only thing in it. The record's metadata came from the job's snapshot, taken at the end of WAIT_PERMS, so drift cannot blank it.
    LLViewerObject* sourcep = mConvert.mPrims[0].get();
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectAndFamily(sourcep);

    // <SS:Nexii> The original goes to the trash the way the build tools' Delete does. selectDelete prompts only when something is locked, no-copy or not owned, and the job already proved the selection is none of those, so it takes its own forceResponse path and derezzes without a second dialog. The non-prompting senders (sendListToRegions/packDeRezHeader) are private to LLSelectMgr.
    LLSelectMgr::getInstance()->selectDelete();

    mConvert.clear();

    // The purple silhouette answers "what did that make": select the new root's family.
    LLSelectMgr::getInstance()->deselectAll();
    if (SSAtmoLandscapeObject* rootp = rootForRecord(record_id))
    {
        LLSelectMgr::getInstance()->selectObjectAndFamily(rootp);
    }

    ss_landscape_notify_list_changed();
}

// The capture: a vetted prim list read with the same free functions the runtime object captures itself with.
S32 SSAtmoLandscapeWorld::addFromSelection(const std::vector<LLPointer<LLViewerObject>>& prims, std::string& out_reason)
{
    out_reason.clear();
    if (prims.empty() || prims[0].isNull() || prims[0]->isDead())
    {
        out_reason = "nothing to convert";
        return -1;
    }
    LLViewerObject* rootp = prims[0].get();

    SSAtmoEnvLandscape record;
    record.mRecordId.generate();
    record.mName = mConvert.mName;
    record.mDesc = mConvert.mDesc;
    record.mCreator = mConvert.mCreator;
    record.mLastOwner = mConvert.mLastOwner;
    record.mCreated = mConvert.mCreated > 0.0 ? mConvert.mCreated : (F64)time_corrected();

    // <SS:Nexii> Locked by default, and against the ROOT's own region rather than the agent's: a linkset can be selected across a border, and a record anchored to the wrong origin lands 256 m out.
    record.mLocked = true;
    record.mLockedOffset = rootp->getPositionRegion();
    record.mFreeGlobal = rootp->getPositionGlobal();
    record.mRotation = rootp->getRotationRegion();

    record.mParts.clear();
    for (S32 i = 0; i < (S32)prims.size(); ++i)
    {
        LLViewerObject* p = prims[(size_t)i].get();
        if (!p || p->isDead()) continue;

        SSAtmoEnvLandscapePart part;
        if (p->getVolume())
        {
            part.mVolume = p->getVolume()->getParams();
        }
        part.mScale = p->getScale();
        if (i > 0)
        {
            // A child's LLPrimitive position and rotation ARE parent-relative - the record's frame exactly.
            part.mOffset = p->getPosition();
            part.mRotation = p->getRotation();
        }
        ss_landscape_capture_faces(p, part.mFaces);
        ss_landscape_capture_extras(p, part.mLight, part.mFlexi);
        record.mParts.push_back(part);
    }
    record.ensureRoot();

    S32 index = -1;
    if (!appendRecord(record, out_reason, index)) return -1;
    return index;
}

// The shared tail of every add path: caps, append to the active track, hydrate now (or defer mid-edit).
bool SSAtmoLandscapeWorld::appendRecord(const SSAtmoEnvLandscape& record, std::string& out_reason, S32& out_index, bool force_hydrate)
{
    out_index = -1;
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    if (!mgr->hasAsset() || !applier->isActive())
    {
        out_reason = "no active environment";
        return false;
    }
    if (!gAgent.getRegion())
    {
        out_reason = "no region";
        return false;
    }
    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        out_reason = "no active track";
        return false;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;

    if ((S32)records.size() >= SS_ATMOENV_MAX_LANDSCAPE_PER_TRACK)
    {
        out_reason = "track landscape cap reached";
        return false;
    }
    S32 total = 0, parts_total = 0;
    for (SSAtmoEnvTrack& t : asset.mTracks)
    {
        total += (S32)t.mLandscapes.size();
        for (const SSAtmoEnvLandscape& l : t.mLandscapes) parts_total += l.partCount();
    }
    if (total >= SS_ATMOENV_MAX_LANDSCAPE_TOTAL)
    {
        out_reason = "environment landscape cap reached";
        return false;
    }
    if (record.partCount() > SS_ATMOENV_MAX_LANDSCAPE_PARTS_PER_RECORD)
    {
        out_reason = llformat("a landscape linkset may have at most %d prims", SS_ATMOENV_MAX_LANDSCAPE_PARTS_PER_RECORD);
        return false;
    }
    if (parts_total + record.partCount() > SS_ATMOENV_MAX_LANDSCAPE_PARTS_TOTAL)
    {
        out_reason = llformat("environment prim budget reached (%d of %d used)", parts_total, SS_ATMOENV_MAX_LANDSCAPE_PARTS_TOTAL);
        return false;
    }

    out_index = (S32)records.size();
    records.push_back(record);

    // Hydrate now: the floater wants the scenery visible this click, not next frame. But an
    // add lands mid-edit (another scenery object selected): defer like any reshape so an
    // in-flight drag never snaps - the add just waits out the drag.
    // <SS:Nexii> Duplicate is invoked with the original selected by definition, so it forces the hydrate: reconcile adopts the selected root by record id and part count and re-applies only its own unchanged record, so nothing snaps - the deferral guards record-SET changes, and an append is not one.
    if (!force_hydrate && anySelected())
    {
        mLastSignature.clear();
    }
    else
    {
        reconcile(asset, track, gAgent.getRegion());
    }
    return true;
}

// Build > Object > Duplicate: the record copied under a fresh id, nudged sideways by its root's width.
S32 SSAtmoLandscapeWorld::duplicateRecord(const LLUUID& record_id, std::string& out_reason)
{
    out_reason.clear();
    const SSAtmoEnvLandscape* source = ss_landscape_record_for(record_id);
    if (!source)
    {
        out_reason = "no such landscape record in the active track";
        return -1;
    }
    SSAtmoEnvLandscape copy = *source;
    copy.mRecordId.generate();
    copy.mCreated = (F64)time_corrected();
    const F32 nudge = copy.mParts.empty() ? 1.f : llmax(1.f, copy.mParts[0].mScale.mV[VX]);
    if (copy.mLocked)
    {
        copy.mLockedOffset.mV[VX] += nudge;
    }
    else
    {
        copy.mFreeGlobal.mdV[VX] += (F64)nudge;
    }
    S32 index = -1;
    if (!appendRecord(copy, out_reason, index, true)) return -1;
    return index;
}

bool SSAtmoLandscapeWorld::removeRecord(S32 index)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return false;
    }
    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        return false;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    if (index < 0 || index >= (S32)records.size())
    {
        return false;
    }

    captureByRecordId(records);    // <SS:Nexii> the survivors' latest edits, before the rebuild resurrects them from their records
    records.erase(records.begin() + index);
    clearLandscapeObjects();
    mLastSignature.clear();
    return true;
}

bool SSAtmoLandscapeWorld::removeByRecord(const LLUUID& record_id)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return false;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return false;
    }
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    for (S32 i = 0; i < (S32)records.size(); ++i)
    {
        if (records[static_cast<size_t>(i)].mRecordId == record_id)
        {
            return removeRecord(i);
        }
    }
    return false;
}