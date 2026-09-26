/**
 * @file ssatmolandscapeobject.h
 * @brief Atmo Magic: the landscape runtime object - one viewer-local volume per record part.
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

#ifndef SS_ATMO_LANDSCAPE_OBJECT_H
#define SS_ATMO_LANDSCAPE_OBJECT_H

#include "lluuid.h"
#include "llvovolume.h"

#include "ssatmoenvasset.h"

#include <vector>

class LLViewerRegion;

// <SS:Nexii> The landscape LOD stretch: stock LOD switching is dialed for region viewing
// (roughly 0-256 m); landscape scenery is viewed across 0-2048 m, so the object's distance
// term is multiplied by this factor (an 8x stretch) - the ssLODDistanceScale hook in
// LLVOVolume folds it into the stock formula, Mesh-detail preference and DebugObjectLODs
// untouched. A starting curve: see doc/atmo_landscape/design_synthesis.md.
constexpr F32 SS_LANDSCAPE_LOD_STRETCH = 0.125f;

// <SS:Nexii> Capture as free functions over ANY viewer object, not just our own: the runtime object
// captures itself with them, and "Convert selection" reads a rezzed SIM linkset with the very same
// code - one implementation, so a converted record and an edited one can never drift apart.

// Sparse face blocks read off an object's texture entries (only faces differing from the TE
// default). False when the object has neither faces nor texture entries to read.
bool ss_landscape_capture_faces(const LLViewerObject* objp, std::vector<SSAtmoEnvLandscapeFace>& out_faces);

// Light and flexi parameters as LLSD; undefined LLSD when the object has neither.
void ss_landscape_capture_extras(const LLViewerObject* objp, LLSD& out_light, LLSD& out_flexi);
// </SS:Nexii>

// A landscape object is the SSWater story applied to volumes: a real LLVOVolume that the
// pcode factory cannot build (it reuses the stock LL_PCODE_VOLUME), so the landscape world
// news it directly and hands it to gObjectList.adoptViewerObject. Local-content flagged, so
// every server send touching it is gated away; full-perms flagged, so the stock editor's
// permission checks treat it as fully editable; its own volume params drive the entire stock
// fetch/LOD/face/pool pipeline, mesh or prim alike.
//
// <SS:Nexii> A record is a linkset: part 0 is the ROOT object and carries the record's placement,
// every later part is a real LLViewerObject child of the root (addChild + setDrawableParent, the
// same parenting the sim's ObjectUpdate performs), so Edit Linked Parts, root-relative dragging,
// family selection and the silhouette all work through stock code. The root owns its children
// by LLPointer and is the only object the landscape world lists. doc/atmo_landscape/design_synthesis.md 15.
//
// The record the object was last applied with is COPIED in, never pointed at: the live
// records live in the working asset's track vector, and a pointer into that vector would
// dangle the instant the floater adds or removes a record.
class SSAtmoLandscapeObject : public LLVOVolume
{
public:
    SSAtmoLandscapeObject(const LLUUID& id, LLViewerRegion* regionp, const SSAtmoEnvLandscape& record, S32 part_index);

    // Re-apply placement (root), part transform (child), volume, faces and extras from a
    // (possibly updated) record; the root forwards to its children. Volume params are only
    // re-set when they changed - identical params re-hit the repo's cached system volume,
    // but the rebuild is worth skipping.
    void applyRecord(const SSAtmoEnvLandscape& record);

    // Per-frame: write the object's transform + sparse face state + extras back into the
    // record's own part (and, on the root, the record's placement), children included.
    // Returns true when anything changed - the reconcile funnel's dirty signal.
    // Non-const: the applied snapshot mAuthored is advanced to match what was written,
    // so an unchanged object keeps reporting unchanged.
    bool captureToRecord(SSAtmoEnvLandscape& record);

    // Feeds the mesh's availability state in for floater display - set by the world from
    // the mesh repo's 404/unavailable notifications; on the root it is the linkset's aggregate.
    void setMeshAvailable(bool available, bool known) { mMeshAvailable = available; mMeshKnown = known; }

    bool meshAvailable() const { return mMeshAvailable; }
    bool meshKnown() const { return mMeshKnown; }

    // Faces materialise when mesh LOD geometry lands; the world calls this every frame
    // until the applied face count matches the volume's. The root forwards to its children.
    void applyFaces();

    // The adoption key: the record this object belongs to, and which part of it this is.
    const LLUUID& recordId() const { return mAuthored.mRecordId; }
    S32 partIndex() const { return mPartIndex; }
    bool isLandscapeRoot() const { return mPartIndex == 0; }

    // The part this object renders, from the applied snapshot; null when the record shrank.
    const SSAtmoEnvLandscapePart* part() const;

    // The mesh asset this part renders, or null for a prim part.
    LLUUID partMeshId() const;

    // Child parts (root only). The world builds them right after the root is adopted.
    void adoptChild(SSAtmoLandscapeObject* child) { mChildParts.push_back(child); }
    S32 childPartCount() const { return (S32)mChildParts.size(); }
    SSAtmoLandscapeObject* childPart(S32 i) { return (i >= 0 && i < (S32)mChildParts.size()) ? mChildParts[(size_t)i].get() : nullptr; }
    void dropChildParts() { mChildParts.clear(); }

    // Refresh the capture baseline from a record WITHOUT re-applying placement/faces -
    // the name/desc write-back path wants the diff baseline to track the record but must
    // not snap an in-flight transform to a (slightly stale) record. Forwarded to children.
    void syncAuthored(const SSAtmoEnvLandscape& record);

    // The relaxed landscape LOD range.
    F32 ssLODDistanceScale() const override { return SS_LANDSCAPE_LOD_STRETCH; }

private:
    void applyVolume(const SSAtmoEnvLandscapePart& part, bool force);
    void applyPlacement(const SSAtmoEnvLandscape& record);
    void applyPartTransform(const SSAtmoEnvLandscapePart& part);
    void applyExtras(const SSAtmoEnvLandscapePart& part);
    void applyOverrides(const SSAtmoEnvLandscapePart& part);
    bool captureFaces(SSAtmoEnvLandscapePart& part, const SSAtmoEnvLandscapePart& authored);
    bool captureExtras(SSAtmoEnvLandscapePart& part, const SSAtmoEnvLandscapePart& authored);

    // The authored snapshot this object last applied (or captured). Copy semantics on
    // purpose - see the class note.
    SSAtmoEnvLandscape mAuthored;

    S32 mPartIndex = 0;

    // The root's children, in part order (part i lives at mChildParts[i - 1]).
    std::vector<LLPointer<SSAtmoLandscapeObject>> mChildParts;

    // TE count the part's faces were last applied to; -1 until the first apply.
    S32 mAppliedFaces = -1;

    // Mesh availability for the floater's list: false = 404/purged/proxy (record still
    // renders as the stock box proxy), known = the repo reported one way or the other.
    bool mMeshAvailable = true;
    bool mMeshKnown = false;

    // <SS:Nexii> Overrides can only land once the base material has fetched (setTEGLTFMaterialOverride declines while it is fetching), so applyFaces retries them on later frames without redoing the texture entries.
    bool mOverridesPending = false;
};

#endif // SS_ATMO_LANDSCAPE_OBJECT_H
