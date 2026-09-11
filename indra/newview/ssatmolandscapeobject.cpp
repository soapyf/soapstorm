/**
 * @file ssatmolandscapeobject.cpp
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

#include "llviewerprecompiledheaders.h"

#include "ssatmolandscapeobject.h"

#include "llviewerregion.h"

#include "llgltfmaterial.h"
#include "llmath.h"
#include "llmaterial.h"
#include "llprimitive.h"
#include "llsdutil.h"
#include "lltextureentry.h"
#include "llvolume.h"

namespace
{
    bool near_v3(const LLVector3& a, const LLVector3& b, F32 eps = 1e-4f)
    {
        return llabs(a.mV[VX] - b.mV[VX]) < eps
            && llabs(a.mV[VY] - b.mV[VY]) < eps
            && llabs(a.mV[VZ] - b.mV[VZ]) < eps;
    }

    bool near_v3d(const LLVector3d& a, const LLVector3d& b, F64 eps = 1e-3)
    {
        return llabs(a.mdV[VX] - b.mdV[VX]) < eps
            && llabs(a.mdV[VY] - b.mdV[VY]) < eps
            && llabs(a.mdV[VZ] - b.mdV[VZ]) < eps;
    }

    bool near_quat(const LLQuaternion& a, const LLQuaternion& b, F32 eps = 1e-4f)
    {
        return llabs(a.mQ[VX] - b.mQ[VX]) < eps
            && llabs(a.mQ[VY] - b.mQ[VY]) < eps
            && llabs(a.mQ[VZ] - b.mQ[VZ]) < eps
            && llabs(a.mQ[VW] - b.mQ[VW]) < eps;
    }

    bool face_equiv(const SSAtmoEnvLandscapeFace& a, const SSAtmoEnvLandscapeFace& b)
    {
        return a.mIndex == b.mIndex
            && a.mTexture == b.mTexture
            && a.mMaterial == b.mMaterial
            && a.mAlphaMode == b.mAlphaMode
            && near_v3(LLVector3(a.mRepeats.mV[VX], a.mRepeats.mV[VY], a.mRepeats.mV[VZ]), LLVector3(b.mRepeats.mV[VX], b.mRepeats.mV[VY], b.mRepeats.mV[VZ]))
            && llabs(a.mRepeats.mV[VW] - b.mRepeats.mV[VW]) < 1e-4f
            && llabs(a.mRotation - b.mRotation) < 1e-4f
            && near_v3(LLVector3(a.mColor.mV[VRED], a.mColor.mV[VGREEN], a.mColor.mV[VBLUE]), LLVector3(b.mColor.mV[VRED], b.mColor.mV[VGREEN], b.mColor.mV[VBLUE]))
            && llabs(a.mColor.mV[VALPHA] - b.mColor.mV[VALPHA]) < 1e-4f
            && llsd_equals(a.mOverride, b.mOverride);
    }
}

// Sparse faces read off any object's texture entries - one block per face that differs from the TE default.
bool ss_landscape_capture_faces(const LLViewerObject* objp, std::vector<SSAtmoEnvLandscapeFace>& out_faces)
{
    out_faces.clear();
    if (!objp)
    {
        return false;
    }

    // <SS:Nexii> A mesh part's faces only exist once its geometry has loaded; the texture entries arrive with the object update and are the fallback bound, which is what the conversion path reads off a rezzed, visible prim whose face count has not settled yet.
    S32 num = objp->getNumFaces();
    if (num <= 0)
    {
        num = (S32)objp->getNumTEs();
    }
    if (num <= 0)
    {
        return false;
    }

    for (S32 i = 0; i < num; ++i)
    {
        LLTextureEntry def;
        const LLTextureEntry* tep = (i < (S32)objp->getNumTEs()) ? objp->getTE((U8)i) : nullptr;
        LLTextureEntry te = tep ? *tep : def;
        const LLUUID mat = objp->getRenderMaterialID((U8)i);
        // TE operator== ignores material params - an alpha-mode face reads as "default"
        // and would be dropped, so fold the material's diffuse alpha mode in explicitly.
        const LLMaterialPtr matp = te.getMaterialParams();
        const S32 alpha = matp.notNull() ? (S32)matp->getDiffuseAlphaMode() : 0;
        if (te == def && alpha == 0 && mat.isNull())
        {
            continue;
        }
        SSAtmoEnvLandscapeFace f;
        f.mIndex = i;
        f.mTexture = te.getID();
        f.mRepeats = LLVector4(te.getScaleS(), te.getScaleT(), te.getOffsetS(), te.getOffsetT());
        f.mRotation = te.getRotation();
        f.mColor = te.getColor();
        f.mAlphaMode = alpha;
        f.mMaterial = mat;
        // <SS:Nexii> The override is read off the live entry, not the copy: it is the per-face edit on top of the base material, and the sim's own objects carry it in the region cache rather than the TE proper.
        if (tep && mat.notNull())
        {
            if (const LLGLTFMaterial* ov = tep->getGLTFMaterialOverride())
            {
                LLSD od;
                LLGLTFMaterial::sDefault.getOverrideLLSD(*ov, od);
                if (od.isMap() && od.size() > 0) f.mOverride = od;
            }
        }
        out_faces.push_back(f);
    }
    return true;
}

// Light and flexi read off any object as LLSD; undefined when the object has neither.
void ss_landscape_capture_extras(const LLViewerObject* objp, LLSD& out_light, LLSD& out_flexi)
{
    out_light = LLSD();
    out_flexi = LLSD();
    if (!objp)
    {
        return;
    }
    if (const LLLightParams* lp = objp->getLightParams())
    {
        out_light = lp->asLLSD();
    }
    if (const LLFlexibleObjectData* fd = objp->getFlexibleObjectData())
    {
        out_flexi = fd->asLLSD();
    }
}

// The pcode factory cannot build this class, so the world news it and adopts it; part 0 is the root, later parts are its children.
SSAtmoLandscapeObject::SSAtmoLandscapeObject(const LLUUID& id, LLViewerRegion* regionp, const SSAtmoEnvLandscape& record, S32 part_index)
    : LLVOVolume(id, LL_PCODE_VOLUME, regionp)
    , mPartIndex(part_index)
{
    // Local content: every server send touching this object is gated on ssIsLocalContent().
    ssSetLocalContent(true);

    // The stock editor's manipulator and menu permission checks read these VO flags
    // (permMove/permModify/permCopy/permYouOwner). A local object's permissions come from
    // the author's captured item metadata rather than a sim, and authoring the scenery the
    // author dropped is always allowed - so the object carries full perms in its flags.
    // WithoutUpdate: the flags exist only for local permission checks and there is no sim
    // to tell.
    setFlagsWithoutUpdate(FLAGS_OBJECT_YOU_OWNER
        | FLAGS_OBJECT_MODIFY
        | FLAGS_OBJECT_COPY
        | FLAGS_OBJECT_MOVE
        | FLAGS_OBJECT_TRANSFER
        | FLAGS_OBJECT_ANY_OWNER
        | FLAGS_OBJECT_OWNER_MODIFY, true);

    // The first apply forces the volume through: the ctor's default box is never what the part wants.
    mAuthored = record;
    if (const SSAtmoEnvLandscapePart* p = part())
    {
        applyVolume(*p, true);
        applyExtras(*p);
    }
    if (isLandscapeRoot())
    {
        applyPlacement(record);
    }
    else if (const SSAtmoEnvLandscapePart* p2 = part())
    {
        applyPartTransform(*p2);
    }
}

// The part this object renders, from the applied snapshot.
const SSAtmoEnvLandscapePart* SSAtmoLandscapeObject::part() const
{
    if (mPartIndex < 0 || mPartIndex >= (S32)mAuthored.mParts.size()) return nullptr;
    return &mAuthored.mParts[(size_t)mPartIndex];
}

// The mesh asset behind this part, if its sculpt entry is a mesh.
LLUUID SSAtmoLandscapeObject::partMeshId() const
{
    const SSAtmoEnvLandscapePart* p = part();
    return p ? p->meshId() : LLUUID::null;
}

// Re-apply from a record; the volume only when its params actually changed.
void SSAtmoLandscapeObject::applyRecord(const SSAtmoEnvLandscape& record)
{
    const SSAtmoEnvLandscapePart* before = part();
    const LLVolumeParams old_volume = before ? before->mVolume : LLVolumeParams();
    mAuthored = record;
    const SSAtmoEnvLandscapePart* p = part();
    if (p)
    {
        applyVolume(*p, !(old_volume == p->mVolume));
        applyExtras(*p);
    }
    if (isLandscapeRoot())
    {
        applyPlacement(record);
        for (LLPointer<SSAtmoLandscapeObject>& child : mChildParts)
        {
            if (child.notNull() && !child->isDead()) child->applyRecord(record);
        }
    }
    else if (p)
    {
        applyPartTransform(*p);
    }
    mAppliedFaces = -1;
}

// The part's volume params: a prim's path/profile or a mesh's sculpt entry, through the stock setVolume fetch path.
void SSAtmoLandscapeObject::applyVolume(const SSAtmoEnvLandscapePart& part, bool force)
{
    if (!force) return;
    // <SS:Nexii> LLVOVolume::isSculpted()/isMesh() read the PARAMS_SCULPT extra-parameter block, NOT the volume params, and setVolume only asks the mesh repo to fetch when isSculpted() is true. Without this block a mesh part rendered only while the source's loaded volume still sat in the shared LOD group; once that was evicted (relog, cache churn) the same record came back as the sculpt placeholder sphere. The block mirrors the sculpt entry the sim would have sent.
    if (part.mVolume.isSculpt())
    {
        LLSculptParams sculpt;
        sculpt.setSculptTexture(part.mVolume.getSculptID(), part.mVolume.getSculptType());
        setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt, false);
        setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, true, false);
    }
    else if (getSculptParams())
    {
        setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, false, false);
    }
    // <SS:Nexii> setVolume drives the whole stock pipeline - for a mesh part the sculpt entry's asset is the geometry (gMeshRepo.loadMesh through the volume-coupled delivery), for a prim part the path and profile are. No sim, so no ObjectShape send: the record is the store.
    setVolume(part.mVolume, 0);
}

// The root's placement: region-locked offset or free global, plus rotation and the root part's scale.
void SSAtmoLandscapeObject::applyPlacement(const SSAtmoEnvLandscape& record)
{
    if (record.mLocked)
    {
        setPositionRegion(record.mLockedOffset);
    }
    else
    {
        setPositionGlobal(record.mFreeGlobal);
    }
    setRotation(record.mRotation);
    if (const SSAtmoEnvLandscapePart* p = part())
    {
        setScale(p->mScale, false);
    }
}

// A child's root-relative transform: LLPrimitive's position and rotation ARE parent-relative for a child.
void SSAtmoLandscapeObject::applyPartTransform(const SSAtmoEnvLandscapePart& part)
{
    setPosition(part.mOffset);
    setRotation(part.mRotation);
    setScale(part.mScale, false);
}

// Light and flexi extras: applied as local (never sent) parameter entries, cleared when the part has none.
void SSAtmoLandscapeObject::applyExtras(const SSAtmoEnvLandscapePart& part)
{
    if (part.mLight.isMap())
    {
        LLLightParams lp;
        LLSD copy = part.mLight;
        lp.fromLLSD(copy);
        setParameterEntry(LLNetworkData::PARAMS_LIGHT, lp, false);
        setParameterEntryInUse(LLNetworkData::PARAMS_LIGHT, true, false);
    }
    else if (getLightParams())
    {
        setParameterEntryInUse(LLNetworkData::PARAMS_LIGHT, false, false);
    }
    if (part.mFlexi.isMap())
    {
        LLFlexibleObjectData fd;
        LLSD copy = part.mFlexi;
        fd.fromLLSD(copy);
        setParameterEntry(LLNetworkData::PARAMS_FLEXIBLE, fd, false);
        setParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE, true, false);
    }
    else if (getFlexibleObjectData())
    {
        setParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE, false, false);
    }
}

// Sparse faces onto the TEs once geometry exists; the root forwards to its children.
void SSAtmoLandscapeObject::applyFaces()
{
    if (isLandscapeRoot())
    {
        for (LLPointer<SSAtmoLandscapeObject>& child : mChildParts)
        {
            if (child.notNull() && !child->isDead()) child->applyFaces();
        }
    }

    const S32 num = getNumFaces();
    if (num <= 0)
    {
        return;
    }
    const SSAtmoEnvLandscapePart* p = part();
    if (num == mAppliedFaces)
    {
        if (mOverridesPending && p) applyOverrides(*p);
        return;
    }
    if (!p)
    {
        mAppliedFaces = num;
        return;
    }

    const S32 max_face = (S32)p->mFaces.size();

    // The sparse list is indexed by face; a hand-edited block may omit the index (then it
    // applies to its array position). Build the mapping once per apply.
    std::vector<S32> by_face(static_cast<size_t>(num), -1);
    for (S32 i = 0; i < max_face; ++i)
    {
        const SSAtmoEnvLandscapeFace& f = p->mFaces[static_cast<size_t>(i)];
        S32 face_index = f.mIndex >= 0 ? f.mIndex : i;
        if (face_index >= 0 && face_index < num)
        {
            by_face[static_cast<size_t>(face_index)] = i;
        }
    }

    for (S32 i = 0; i < num; ++i)
    {
        const S32 src = by_face[static_cast<size_t>(i)];
        if (src < 0)
        {
            continue;
        }
        const SSAtmoEnvLandscapeFace& f = p->mFaces[static_cast<size_t>(src)];

        // A fully-default block (nothing authored) is skippable - capture only writes blocks
        // that differ from the default texture entry. A tint-only or alpha-only block
        // (texture and material both empty but colour/alpha authored) is NOT default, so
        // the test mirrors the TE defaults rather than just the two ids.
        const bool empty = f.mTexture.isNull() && f.mMaterial.isNull() && f.mAlphaMode == 0
            && near_v3(LLVector3(f.mRepeats.mV[VX], f.mRepeats.mV[VY], f.mRepeats.mV[VZ]), LLVector3(1.f, 1.f, 0.f))
            && llabs(f.mRepeats.mV[VW]) < 1e-4f
            && llabs(f.mRotation) < 1e-4f
            && f.mColor == LLColor4::white;
        if (empty)
        {
            continue;
        }

        LLTextureEntry te;
        if (i < (S32)getNumTEs())
        {
            te = *getTE((U8)i);
        }

        if (!f.mTexture.isNull())
        {
            te.setID(f.mTexture);
            te.setScaleS(f.mRepeats.mV[VX]);
            te.setScaleT(f.mRepeats.mV[VY]);
            te.setOffsetS(f.mRepeats.mV[VZ]);
            te.setOffsetT(f.mRepeats.mV[VW]);
            te.setRotation(f.mRotation);
        }
        te.setColor(f.mColor);
        if (f.mAlphaMode != 0)
        {
            // Alpha mode lives on the face's material params, not the TE - carry any
            // existing material through and stamp the mode onto it.
            LLMaterialPtr mat = te.getMaterialParams();
            if (mat.isNull())
            {
                mat = new LLMaterial();
            }
            mat->setDiffuseAlphaMode(f.mAlphaMode);
            te.setMaterialParams(mat);
        }

        setTE((U8)i, te);

        if (!f.mMaterial.isNull())
        {
            setRenderMaterialID(i, f.mMaterial, false, true);
        }
    }

    mAppliedFaces = num;
    applyOverrides(*p);
}

// The faces' material overrides onto the texture entries; a base material still fetching declines, so the pass is retried by applyFaces until every override has landed.
void SSAtmoLandscapeObject::applyOverrides(const SSAtmoEnvLandscapePart& p)
{
    mOverridesPending = false;
    const S32 num = getNumFaces();
    for (S32 i = 0; i < (S32)p.mFaces.size(); ++i)
    {
        const SSAtmoEnvLandscapeFace& f = p.mFaces[(size_t)i];
        const S32 face_index = f.mIndex >= 0 ? f.mIndex : i;
        if (face_index < 0 || face_index >= num || face_index >= (S32)getNumTEs()) continue;
        if (f.mMaterial.isNull() || !f.mOverride.isMap()) continue;
        LLPointer<LLGLTFMaterial> mat = new LLGLTFMaterial();
        mat->applyOverrideLLSD(f.mOverride);
        if (setTEGLTFMaterialOverride((U8)face_index, mat) == TEM_CHANGE_NONE)
        {
            mOverridesPending = true;
        }
    }
}

// Refresh the diff baseline without touching placement; forwarded to the children.
void SSAtmoLandscapeObject::syncAuthored(const SSAtmoEnvLandscape& record)
{
    mAuthored = record;
    for (LLPointer<SSAtmoLandscapeObject>& child : mChildParts)
    {
        if (child.notNull() && !child->isDead()) child->syncAuthored(record);
    }
}

// Sparse faces read back from the TEs; written only when they differ from what was last applied.
bool SSAtmoLandscapeObject::captureFaces(SSAtmoEnvLandscapePart& part, const SSAtmoEnvLandscapePart& authored)
{
    // Faces are only captured once mesh geometry actually exists - until then the record's
    // authored set is authoritative and must not be replaced by the object's (still empty)
    // face state.
    if (getNumFaces() <= 0 || mAppliedFaces < 0) return false;

    std::vector<SSAtmoEnvLandscapeFace> faces;
    if (!ss_landscape_capture_faces(this, faces)) return false;

    bool changed = faces.size() != authored.mFaces.size();
    for (size_t i = 0; !changed && i < faces.size(); ++i)
    {
        if (!face_equiv(faces[i], authored.mFaces[i])) changed = true;
    }
    if (changed)
    {
        part.mFaces = std::move(faces);
    }
    return changed;
}

// Light and flexi read back as LLSD; undefined when the part has none.
bool SSAtmoLandscapeObject::captureExtras(SSAtmoEnvLandscapePart& part, const SSAtmoEnvLandscapePart& authored)
{
    bool changed = false;
    LLSD light, flexi;
    ss_landscape_capture_extras(this, light, flexi);
    if (!llsd_equals(light, authored.mLight))
    {
        part.mLight = light;
        changed = true;
    }
    if (!llsd_equals(flexi, authored.mFlexi))
    {
        part.mFlexi = flexi;
        changed = true;
    }
    return changed;
}

// Write the object's state into its part (and, on the root, the placement), children included.
bool SSAtmoLandscapeObject::captureToRecord(SSAtmoEnvLandscape& record)
{
    bool changed = false;

    if (mPartIndex < 0 || mPartIndex >= (S32)record.mParts.size() || !part())
    {
        mAuthored = record;
        return false;
    }
    SSAtmoEnvLandscapePart& out = record.mParts[(size_t)mPartIndex];
    const SSAtmoEnvLandscapePart authored = *part();

    if (isLandscapeRoot())
    {
        if (record.mLocked)
        {
            const LLVector3 offset = getPositionRegion();
            if (!near_v3(offset, mAuthored.mLockedOffset))
            {
                record.mLockedOffset = offset;
                changed = true;
            }
        }
        else
        {
            const LLVector3d global = getPositionGlobal();
            if (!near_v3d(global, mAuthored.mFreeGlobal))
            {
                record.mFreeGlobal = global;
                changed = true;
            }
        }

        const LLQuaternion rot = getRotation();
        if (!near_quat(rot, mAuthored.mRotation))
        {
            record.mRotation = rot;
            changed = true;
        }
    }
    else
    {
        const LLVector3 offset = getPosition();
        if (!near_v3(offset, authored.mOffset))
        {
            out.mOffset = offset;
            changed = true;
        }
        const LLQuaternion rot = getRotation();
        if (!near_quat(rot, authored.mRotation))
        {
            out.mRotation = rot;
            changed = true;
        }
    }

    const LLVector3 scale = getScale();
    if (!near_v3(scale, authored.mScale))
    {
        out.mScale = scale;
        changed = true;
    }

    // <SS:Nexii> Shape edits (the Object tab's path/profile/sculpt controls call setVolume on the object, and the send funnel drops the ObjectShape) come back through the live volume params, so a prim reshaped in the editor persists.
    LLVolumeParams live = getVolume()->getParams();
    // <SS:Nexii> LLVOVolume::setVolume strips the sculpt entry when the mesh repo reports no LOD (404, purged) and draws the proxy box; capturing that would write box params into the record and lose the mesh id for good on a transient asset failure. The authored sculpt entry is kept until a real reshape replaces it.
    if ((authored.mVolume.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_MESH && live.getSculptID().isNull())
    {
        live.setSculptID(authored.mVolume.getSculptID(), authored.mVolume.getSculptType());
    }
    if (!(live == authored.mVolume))
    {
        out.mVolume = live;
        changed = true;
    }

    if (captureFaces(out, authored)) changed = true;
    if (captureExtras(out, authored)) changed = true;

    if (isLandscapeRoot())
    {
        for (LLPointer<SSAtmoLandscapeObject>& child : mChildParts)
        {
            if (child.notNull() && !child->isDead() && child->captureToRecord(record)) changed = true;
        }
    }

    // Advance the applied snapshot so the next capture diffs against what was just
    // written - an untouched object stays untouched forever after.
    mAuthored = record;

    return changed;
}
