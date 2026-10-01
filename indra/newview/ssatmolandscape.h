/**
 * @file ssatmolandscape.h
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

#ifndef SS_ATMO_LANDSCAPE_H
#define SS_ATMO_LANDSCAPE_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lldeadmantimer.h"

#include <string>
#include <vector>

#include "ssatmolandscapeobject.h"

// <SS:Nexii> Atmo Magic landscape: scenery linksets owned by the environment asset instead of the
// region. The SSWaterWorld of the landscape family - one manager, N live root objects, each
// owning its child parts.
//
// Lifecycle: the working asset's active track owns the records; the live set mirrors the
// active track's record list, keyed by asset uuid. Anything that changes the record set
// (track crossing, environment load/revert, floater convert/delete/reorder, agent region change)
// reshapes the live set; anything that changes a record's CONTENT (the author's edits) flows
// through the reconcile funnel - capture writes object state into the working asset, and the
// existing save path persists it.
//
// <SS:Nexii> Records come from CONVERSION, not from an inventory drop: the author rezzes and builds
// a linkset with the stock tools, selects it, and the floater's "Convert selection" button hands it
// to the conversion job below - it reads the live prims (volume params, root-relative transforms,
// faces, light/flexi), appends one record, and derezzes the original to the trash. The old
// drag-and-drop path could never work: an uploaded mesh is an AT_OBJECT inventory item whose asset
// uuid is the server-side prim blob, not a mesh asset id. doc/atmo_landscape/design_synthesis.md 15.
//
// Everything is opt-in: the master SSAtmoEnabled switch plus this feature's own SSAtmoLandscape
// gate, and the applier must be actively driving the sky. Nothing exists when any of those are
// off.
class SSAtmoLandscapeWorld : public LLSingleton<SSAtmoLandscapeWorld>
{
    LLSINGLETON_EMPTY_CTOR(SSAtmoLandscapeWorld);
    ~SSAtmoLandscapeWorld() = default;

public:
    // Per-frame tick, called from the Atmo block in llviewerdisplay.
    void update();

    // Kills the live set (environment unload, master toggle, hard reset).
    void clearLandscapeObjects();

    // Live set introspection for the floater's list: the ROOTS, one per record.
    S32 objectCount() const { return (S32)mObjects.size(); }
    SSAtmoLandscapeObject* objectAt(S32 index)
    {
        return (index >= 0 && index < (S32)mObjects.size()) ? mObjects[(size_t)index].get() : nullptr;
    }

    // The ACTIVE track's records - the list the floater mirrors. Index corresponds to
    // objectAt() while the live set is in sync (reconcile builds in record order).
    S32 recordCount() const;
    const SSAtmoEnvLandscape* recordAt(S32 index) const;

    // Flips a record's lock mode, converting its coordinates so the object does not jump on
    // the mode change (locked offsets become the current global, and vice versa), then
    // re-applies. Returns the new mode (true = locked).
    bool toggleRecordLock(S32 index);

    // <SS:Nexii> The conversion job's states, walked one per frame from update(). WAIT_PERMS and
    // WAIT_CONTENTS are the two sim round trips (object properties per prim, task inventory per
    // prim); CONFIRM is the user's answer to the contents warning; FINISH does the capture,
    // the append, the derez and the re-select in one tick. Nothing ever blocks.
    enum class ESSConvertState
    {
        IDLE,
        WAIT_PERMS,
        WAIT_CONTENTS,
        CONFIRM,
        FINISH
    };

    // The floater's "Convert selection" entry: validates what can be validated synchronously
    // (one volume root, not an attachment/avatar/landscape, under the prim cap) and starts the
    // job. false with out_reason for an immediate refusal; later refusals alert themselves.
    bool beginConvertSelection(std::string& out_reason);
    bool convertBusy() const { return mConvert.mState != ESSConvertState::IDLE; }

    // The conversion's capture-and-append step: a vetted prim list (root first) becomes one
    // record appended to the active track and hydrated now. Returns the record's index in the
    // active track's list, or -1 with out_reason set (the caps produce a friendly reason).
    S32 addFromSelection(const std::vector<LLPointer<LLViewerObject>>& prims, std::string& out_reason);

    // Removes the record at index in the active track and reshapes now.
    bool removeRecord(S32 index);

    // Removes the active track's record with this id (the pie-menu Delete path for
    // local-content objects; root and children share the id). Returns false when nothing matched.
    bool removeByRecord(const LLUUID& record_id);

    // <SS:Nexii> Build > Object > Duplicate for local content: a copy of the record with a fresh id,
    // offset a little so the two do not sit inside each other, appended to the active track and
    // hydrated now. Returns the new record's index, or -1 with out_reason set.
    S32 duplicateRecord(const LLUUID& record_id, std::string& out_reason);

    // The live root for a record id, or null.
    SSAtmoLandscapeObject* rootForRecord(const LLUUID& record_id);

    // Force the live set to match the working asset next tick (floater reorder etc.).
    void invalidate() { mLastSignature.clear(); }

    // <SS:Nexii> Write every live object's state into the working asset right now, ahead of a save, so the notecard never misses an edit made since the last capture tick.
    void captureNow();

private:
    void reconcile(const SSAtmoEnvAsset& asset, S32 track_index, LLViewerRegion* regionp);
    SSAtmoLandscapeObject* createObject(LLViewerRegion* regionp, const SSAtmoEnvLandscape& record);
    void killObject(SSAtmoLandscapeObject* rootp);
    bool anySelected() const;
    bool appendRecord(const SSAtmoEnvLandscape& record, std::string& out_reason, S32& out_index, bool force_hydrate = false);
    void applyFacesToAll();
    void captureAll(std::vector<SSAtmoEnvLandscape>& records);
    // Record-id paired capture only: safe against any record list, so a reshape or region rebuild can flush the objects' last edits into their own records before it tears them down.
    void captureByRecordId(std::vector<SSAtmoEnvLandscape>& records);

    // The feature's own gates as a reason string - the conversion refuses for exactly the
    // reasons a record would not hydrate.
    bool landscapeActive(std::string& out_reason) const;

    // The conversion job: one state step per frame, then idle again.
    void tickConvert();
    void convertFail(const std::string& reason);
    void convertFinish();

    // <SS:Nexii> The job holds LLPointers to the SOURCE sim prims while it polls, so a prim that
    // dies under it (derez, teleport, region teardown) is noticed instead of dereferenced; every
    // exit - success, refusal, timeout, cancel - runs clear() and drops them.
    struct SSConvertJob
    {
        ESSConvertState mState = ESSConvertState::IDLE;
        std::vector<LLPointer<LLViewerObject>> mPrims; // root first, then its volume children
        LLFrameTimer mTimer;
        bool mNudged = false;          // the one ObjectSelect re-send while properties are late
        bool mContentsUnknown = false; // a prim never answered about its inventory - warn, never block
        S32 mContentItems = 0;
        S32 mContentPrims = 0;

        // <SS:Nexii> The root's metadata, snapshotted the moment every node is proven valid (end of WAIT_PERMS): the selection can drift during the contents wait or the dialog, and the record must not come out unnamed and creator-less because of it.
        std::string mName;
        std::string mDesc;
        LLUUID mCreator;
        LLUUID mLastOwner;
        F64 mCreated = 0.0;

        // Bumped per job and carried in the contents dialog's payload, so a stale dialog cannot answer for a later job. Deliberately not reset by clear().
        U32 mGeneration = 0;

        void clear()
        {
            mState = ESSConvertState::IDLE;
            mPrims.clear();
            mNudged = false;
            mContentsUnknown = false;
            mContentItems = 0;
            mContentPrims = 0;
            mName.clear();
            mDesc.clear();
            mCreator.setNull();
            mLastOwner.setNull();
            mCreated = 0.0;
        }
    };
    SSConvertJob mConvert;

    std::vector<LLPointer<SSAtmoLandscapeObject>> mObjects;

    // The signature of the record set the live objects were shaped from - a record-id and
    // part-count run. Any change reshapes; content edits inside a record do not.
    std::string mLastSignature;

    // The region the live set is anchored to - changing it rebuilds so locked records
    // re-anchor to the new origin.
    U64 mAgentRegionHandle = 0;

    LLFrameTimer mCaptureTimer;
};

// <SS:Nexii> The conversion job's list hook: the world has no UI of its own, so a finished
// conversion pokes the environment editor's landscape list through this. Defined in
// ssfloateratmoenv.cpp; a no-op when the editor is not open.
void ss_landscape_notify_list_changed();

// <SS:Nexii> Selection-node seeding: when a local-content object is selected, the node is
// seeded from its record (name, description, creator/last-owner, perms, creation date) so
// the stock editor's General tab, Inspect and texture panels read real values instead of
// the node's default blank server-wait state. Called from LLSelectMgr's node creation.
class LLSelectNode;
void ss_seed_local_select_node(LLSelectNode* nodep);

// The record backing a live object in the ACTIVE track - used by seating and the floater
// list. Null when the record is not in the active track at all.
const SSAtmoEnvLandscape* ss_landscape_record_for(const LLUUID& record_id);

// Name/desc write-back from the stock General tab. Called from LLSelectMgr's
// selectionSetObjectName/Description when the selection is a local-content landscape
// object: the send funnel ignores local content (no sim to tell), so the record - the
// authoritative store - is updated here instead. Scans all tracks (a same-mesh record may
// sit in another track); refreshes only the LIVE object's capture baseline, never the
// placement.
void ss_landscape_persist_name(const LLUUID& record_id, const std::string& name, const std::string& desc);
// </SS:Nexii>

#endif // SS_ATMO_LANDSCAPE_H