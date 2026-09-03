/**
 * @file llvowater.cpp
 * @brief LLVOWater class implementation
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"

#include "llvowater.h"

#include "llviewercontrol.h"

#include "lldrawable.h"
#include "lldrawpoolwater.h"
#include "llface.h"
#include "llsky.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"
#include "llspatialpartition.h"

///////////////////////////////////

template<class T> inline T LERP(T a, T b, F32 factor)
{
    return a + (b - a) * factor;
}

LLVOWater::LLVOWater(const LLUUID &id,
                     const LLPCode pcode,
                     LLViewerRegion *regionp) :
    LLStaticViewerObject(id, pcode, regionp),
    mRenderType(LLPipeline::RENDER_TYPE_WATER)
{
    // Terrain must draw during selection passes so it can block objects behind it.
    mbCanSelect = false;
// <FS:CR> Aurora Sim
    //setScale(LLVector3(256.f, 256.f, 0.f)); // Hack for setting scale for bounding boxes/visibility.
    setScale(LLVector3(mRegionp->getWidth(), mRegionp->getWidth(), 0.f));
// </FS:CR> Aurora Sim

    mIsEdgePatch = false;
}


void LLVOWater::markDead()
{
    LLViewerObject::markDead();
}


bool LLVOWater::isActive() const
{
    return false;
}


void LLVOWater::setPixelAreaAndAngle(LLAgent &agent)
{
    mAppAngle = 50;
    mPixelArea = 500*500;
}


// virtual
void LLVOWater::updateTextures()
{
}

// Never gets called
void  LLVOWater::idleUpdate(LLAgent &agent, const F64 &time)
{
}

LLDrawable *LLVOWater::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(mRenderType);

    LLDrawPoolWater *pool = (LLDrawPoolWater*) gPipeline.getPool(LLDrawPool::POOL_WATER);

    mDrawable->setNumFaces(1, pool, LLWorld::getInstance()->getDefaultWaterTexture());

    return mDrawable;
}

bool LLVOWater::updateGeometry(LLDrawable *drawable)
{
    LL_PROFILE_ZONE_SCOPED;
    LLFace *face;

    if (drawable->getNumFaces() < 1)
    {
        LLDrawPoolWater *poolp = (LLDrawPoolWater*) gPipeline.getPool(LLDrawPool::POOL_WATER);
        drawable->addFace(poolp, NULL);
    }
    face = drawable->getFace(0);
    if (!face)
    {
        return true;
    }

//  LLVector2 uvs[4];
//  LLVector3 vtx[4];

    LLStrider<LLVector3> verticesp, normalsp;
    LLStrider<LLVector2> texCoordsp;
    LLStrider<U16> indicesp;
    U16 index_offset;


    // A quad is 4 vertices and 6 indices (making 2 triangles)
    static const unsigned int vertices_per_quad = 4;
    static const unsigned int indices_per_quad = 6;

    S32 size_x = LLPipeline::sRenderTransparentWater ? 8 : 1;
    S32 size_y = LLPipeline::sRenderTransparentWater ? 8 : 1;

    const LLVector3& scale = getScale();
    size_x *= (S32)llmin(llround(scale.mV[0] / 256.f), 8);
    size_y *= (S32)llmin(llround(scale.mV[1] / 256.f), 8);

    // <SS:Nexii> A patch under ~128m rounds to ZERO quads above, and the zero-size buffer path dangles in the mapped-buffer flush list (access violation under movePartition). One quad is a valid mesh at any scale.
    size_x = llmax(size_x, 1);
    size_y = llmax(size_y, 1);

    const S32 num_quads = size_x * size_y;
    face->setSize(vertices_per_quad * num_quads,
                  indices_per_quad * num_quads);

    LLVertexBuffer* buff = face->getVertexBuffer();
    if (!buff ||
        buff->getNumIndices() != face->getIndicesCount() ||
        buff->getNumVerts() != face->getGeomCount() ||
        face->getIndicesStart() != 0 ||
        face->getGeomIndex() != 0)
    {
        buff = new LLVertexBuffer(LLDrawPoolWater::VERTEX_DATA_MASK);
        if (!buff->allocateBuffer(face->getGeomCount(), face->getIndicesCount()))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer on water update to "
                << face->getGeomCount() << " vertices and "
                << face->getIndicesCount() << " indices" << LL_ENDL;
        }
        face->setIndicesIndex(0);
        face->setGeomIndex(0);
        face->setVertexBuffer(buff);
    }

    index_offset = face->getGeometry(verticesp,normalsp,texCoordsp, indicesp);

    LLVector3 position_agent;
    position_agent = getPositionAgent();
    face->mCenterAgent = position_agent;
    face->mCenterLocal = position_agent;

    S32 x, y;
    F32 step_x = getScale().mV[0] / size_x;
    F32 step_y = getScale().mV[1] / size_y;

    const LLVector3 normal(0.f, 0.f, 1.f);

    F32 size_inv_x = 1.f / size_x;
    F32 size_inv_y = 1.f / size_y;

    // <SS:Nexii> Quads come off one rounded origin plus whole steps, not a rounded centre offset by half a step. Rounding each centre moved neighbours independently by up to half a metre whenever the step was fractional (step is scale/size, size capped at 64, so any patch not a multiple of 64m), tearing seams through the void water at long draw distances. Neighbours now share the exact edge coordinate.
    LLVector3 origin_agent = getPositionAgent() - getScale() * 0.5f;
    origin_agent.mV[VX] = (F32)llround(origin_agent.mV[VX]);
    origin_agent.mV[VY] = (F32)llround(origin_agent.mV[VY]);

    for (y = 0; y < size_y; y++)
    {
        const F32 y0 = origin_agent.mV[VY] + y * step_y;
        const F32 y1 = origin_agent.mV[VY] + (y + 1) * step_y;

        for (x = 0; x < size_x; x++)
        {
            S32 toffset = index_offset + 4*(y*size_x + x);
            const F32 x0 = origin_agent.mV[VX] + x * step_x;
            const F32 x1 = origin_agent.mV[VX] + (x + 1) * step_x;

            *verticesp++  = LLVector3(x0, y1, origin_agent.mV[VZ]);
            *verticesp++  = LLVector3(x0, y0, origin_agent.mV[VZ]);
            *verticesp++  = LLVector3(x1, y1, origin_agent.mV[VZ]);
            *verticesp++  = LLVector3(x1, y0, origin_agent.mV[VZ]);

            *texCoordsp++ = LLVector2(x*size_inv_x, (y+1)*size_inv_y);
            *texCoordsp++ = LLVector2(x*size_inv_x, y*size_inv_y);
            *texCoordsp++ = LLVector2((x+1)*size_inv_x, (y+1)*size_inv_y);
            *texCoordsp++ = LLVector2((x+1)*size_inv_x, y*size_inv_y);

            *normalsp++   = normal;
            *normalsp++   = normal;
            *normalsp++   = normal;
            *normalsp++   = normal;

            *indicesp++ = toffset + 0;
            *indicesp++ = toffset + 1;
            *indicesp++ = toffset + 2;

            *indicesp++ = toffset + 1;
            *indicesp++ = toffset + 3;
            *indicesp++ = toffset + 2;
        }
    }

    buff->unmapBuffer();

    mDrawable->movePartition();
    LLPipeline::sCompiles++;
    return true;
}

void LLVOWater::initClass()
{
}

void LLVOWater::cleanupClass()
{
}

void setVecZ(LLVector3& v)
{
    v.mV[VX] = 0;
    v.mV[VY] = 0;
    v.mV[VZ] = 1;
}

void LLVOWater::setIsEdgePatch(const bool edge_patch)
{
    mIsEdgePatch = edge_patch;
}

void LLVOWater::updateSpatialExtents(LLVector4a &newMin, LLVector4a& newMax)
{
    LLVector4a pos;
    pos.load3(getPositionAgent().mV);
    LLVector4a scale;
    scale.load3(getScale().mV);
    scale.mul(0.5f);

    newMin.setSub(pos, scale);
    newMax.setAdd(pos, scale);

    pos.setAdd(newMin,newMax);
    pos.mul(0.5f);

    mDrawable->setPositionGroup(pos);
}

U32 LLVOWater::getPartitionType() const
{
    if (mIsEdgePatch)
    {
        return LLViewerRegion::PARTITION_VOIDWATER;
    }

    return LLViewerRegion::PARTITION_WATER;
}

U32 LLVOVoidWater::getPartitionType() const
{
    return LLViewerRegion::PARTITION_VOIDWATER;
}

LLWaterPartition::LLWaterPartition(LLViewerRegion* regionp)
: LLSpatialPartition(0, false, regionp)
{
    mInfiniteFarClip = true;
    mDrawableType = LLPipeline::RENDER_TYPE_WATER;
    mPartitionType = LLViewerRegion::PARTITION_WATER;
}

LLVoidWaterPartition::LLVoidWaterPartition(LLViewerRegion* regionp) : LLWaterPartition(regionp)
{
    mOcclusionEnabled = false;
    mDrawableType = LLPipeline::RENDER_TYPE_VOIDWATER;
    mPartitionType = LLViewerRegion::PARTITION_VOIDWATER;
}
