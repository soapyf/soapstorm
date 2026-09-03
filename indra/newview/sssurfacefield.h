/**
 * @file sssurfacefield.h
 * @brief Atmo Magic: surface wetness/snow/standing water field.
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

#ifndef SS_SURFACEFIELD_H
#define SS_SURFACEFIELD_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "ssrainshadow.h"
#include "v3math.h"
#include "v4math.h"

#include <functional>
#include <map>
#include <vector>

struct SSPrecipPreset;
class LLGLSLShader;
struct SSGranularParams;

class SSSurfaceField : public LLSingleton<SSSurfaceField>
{
    LLSINGLETON_EMPTY_CTOR(SSSurfaceField);

public:
    void idle(F32 dt);

    void clear();

    void renderDebug();

    void renderRunoffDebug();

    struct Sample
    {
        F32 mWet = 0.f;
        F32 mSnow = 0.f;
        F32 mPuddle = 0.f;
        F32 mLift = 0.f;
        F32 mSurfaceZ = 0.f;
        bool mValid = false;
    };
    Sample sample(const LLVector3& pos_agent) const;

    // Landing credit for a granular runoff clump - the one write path anything outside the field
    // has into mSnow. Forwards to the transport's repose logic.
    void depositAt(const LLVector3& pos_agent, F32 depth);

    // <SS:Nexii> Flash-boils the surface water in a disc: takes the standing puddle outright, most of the film of wet, and the top of any snow, and holds those cells against re-accumulation for hold_s so the patch stays gone long enough to read instead of refilling from the next rain tick. Returns the water it actually took, 0..1 against a full puddle, which is what the steam burst scales itself by - a strike on dry ground removes nothing and steams not at all. doc/atmo_magic_lightning_strike.md
    F32 vaporise(const LLVector3& center_agent, F32 radius_m, F32 hold_s);

    // Walks every cell holding settled snow and lift around a point - the drift tier's spawn walk.
    void forEachLiftCell(const LLVector3& center_agent, F32 radius_m,
                         const std::function<void(const LLVector3& pos_agent, F32 depth, F32 lift)>& fn) const;

    bool bindForShader(LLGLSLShader& shader, S32 channel);
    bool hasWindow() const { return mWindowTex != 0 && mWindowValid; }

    bool bindFlowForShader(LLGLSLShader& shader, S32 channel);
    bool hasFlowWindow() const { return mWindowFlowTex != 0 && mWindowValid; }

    void releaseGL();

    void renderWetPass();

    void renderSnowPass();

    S32 fieldCount() const { return (S32)mFields.size(); }
    F32 lastTickMS() const { return mLastTickMS; }
    F32 peakWet() const { return mPeakWet; }
    F32 peakSnow() const { return mPeakSnow; }
    F32 peakPuddle() const { return mPeakPuddle; }

private:
    // Field and Geometry are the transport's working data - SSGranular::step() integrates over
    // them directly, so they live in the public eye with the storage-only caveat that implies:
    // anything outside the field writes mSnow through depositAt() and nothing else.
public:
    struct Geometry
    {
        S32 mN = 0;
        F32 mCell = 0.f;
        U32 mGeomSerial = 0xFFFFFFFFu;

        std::vector<F32> mZ;
        std::vector<U8> mFlags;

        // Metres the surface stands above the terrain/water reference under it - carried through
        // from the grid so the tick can tell a street from a tower roof: puddles and deep snow
        // piles belong at grade, and a terrain ledge is not a roof edge however sharply it drops.
        std::vector<F32> mAbove;

        std::vector<F32> mSlopeX;
        std::vector<F32> mSlopeY;
        std::vector<F32> mSlope;

        std::vector<U8> mEdge;
        std::vector<F32> mEdgeX;
        std::vector<F32> mEdgeY;

        std::vector<S32> mEdgeCells;

        std::vector<U8> mPool;

        bool valid() const { return mN > 0 && !mZ.empty(); }
        bool solid(size_t i) const { return mFlags[i] != 0; }
        bool water(size_t i) const { return (mFlags[i] & SSRainShadowMap::SURF_WATER) != 0; }
        F32 above(size_t i) const { return mAbove.empty() ? 0.f : mAbove[i]; }
    };

    struct Field
    {
        U64 mRegionHandle = 0;
        S32 mN = 0;
        F32 mCell = 0.f;

        std::vector<F32> mZ;

        std::vector<F32> mWet;
        std::vector<F32> mSnow;
        std::vector<F32> mPuddle;

        // The granular transport's state: the per-cell lift figure the drift tier's spawn walk
        // reads, and the creep pass's inflow accumulator (one step's arrivals, applied after the
        // outflows so the exchange is order-independent).
        // <SS:Nexii> Seconds of accumulation hold left on each cell after a strike flash-boiled it. Counts down in the tick; while it is up the cell takes no wet, no pool and no snow, but its ordinary loss paths still run, so a scorched patch dries rather than freezing in place. One F32 per cell, zero for every cell that has never been struck.
        std::vector<F32> mScorch;

        std::vector<F32> mLift;
        std::vector<F32> mInflow;

        std::vector<F32> mStore;
        std::vector<F32> mAccum;

        F64 mLastTouched = 0.0;
    };

private:
    void refreshGeometry();
    static void buildGeometry(const SSRainShadowMap::SurfaceGrid& grid, Geometry& out);

    std::map<U64, Geometry> mGeometry;

    void shedEdges(F32 dt);

    void renderRunoffLips(U32 view, const LLVector3& cam,
                          F32 radius_sq, F32 budget, bool context_only) const;

    void shedRegion(U64 region_handle, const Geometry& geom, Field& fld,
                    F32 dt, F32 rate_m2, const LLVector3& camera_agent);

    Field* fieldFor(U64 region_handle, const Geometry& geom, F64 now);
    void updateWindow();
    void tick(Field& fld, const Geometry& geom, F32 dt,
              const SSPrecipPreset& preset, F32 intensity, F32 melt_scale,
              const SSGranularParams& granular, const LLVector4* flow);
    void evict(F64 now);

    std::map<U64, Field> mFields;

    // The fixed-step transport clock: steps land on exact quanta of shared time, so creep,
    // erosion and regime transitions never vary with frame rate (or with the viewer).
    F64 mLastStep = -1.0;

    LLRenderTarget mScratch;

    LLRenderTarget mScratchNormal;

    U32 mWindowTex = 0;
    S32 mWindowRes = 0;
    F32 mWindowCell = 0.f;
    LLVector3 mWindowOrigin;
    std::vector<F32> mWindowData;
    bool mWindowValid = false;

    U32 mWindowFlowTex = 0;
    std::vector<F32> mWindowFlowData;

    std::map<U64, S32> mShedCursor;

    F32 mLastTickMS = 0.f;
    F32 mPeakWet = 0.f;
    F32 mPeakSnow = 0.f;
    F32 mPeakPuddle = 0.f;
};

#endif
