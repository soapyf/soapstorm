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
// <SS:Nexii> Header-only core (doc/atmo_magic_surface_weather.md): the ice/frost/stain/age step, the liquid/deposit looks and their crossfade, and the impact ring buffer all live here so the field owns the state without owning the physics.
#include "sssurfacestatecore.h"
#include "v3math.h"
#include "v4math.h"

#include <functional>
#include <map>
#include <vector>

struct SSPrecipPreset;
class LLGLSLShader;
struct SSGranularParams;
class SSWorldField;

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
        F32 mIce = 0.f;
        F32 mFrost = 0.f;
        F32 mStain = 0.f;
        F32 mAge = 0.f;
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

    // <SS:Nexii> The state window (ice, frost, stain, age) - same lattice and origin uniform as ssFieldMap, a separate texture because a fourth channel would not fit beside puddle in ssFieldMap.
    bool bindStateForShader(LLGLSLShader& shader, S32 channel);
    bool hasStateWindow() const { return mWindowStateTex != 0 && mWindowValid; }

    // <SS:Nexii> The cover window - the world field's enclosure spectrum per
    // cell (0 outdoors, 1 sealed interior, -1 where neither the field nor the
    // world field has an answer), sampled just above each cell's stored
    // surface: the air a fog sample sits in when the covered test gates it.
    // Same lattice/origin as ssFieldMap, a separate texture the same way; the
    // height fog is its only consumer, and like the flow window only the
    // passes that read it bind it.
    bool bindCoverForShader(LLGLSLShader& shader, S32 channel);
    bool hasCoverWindow() const { return mWindowCoverTex != 0 && mWindowValid; }

    // Uploads the resolved liquid/deposit looks and the plain weather scalars every surface pass shares.
    void bindLooksForShader(LLGLSLShader& shader) const;

    // Uploads the live impact ring buffer (expired already this frame in idle()).
    void bindRingsForShader(LLGLSLShader& shader) const;

    // Records an impact for the ring shader if it fell within SSSurfaceState::RING_NEAR_M of the camera; farther ones stay ripple quads.
    void noteImpact(const LLVector3& pos_agent, F32 strength);

    // How fast the analytic ring's clock runs, from the same preset and taste controls the landing ripple quad expands off.
    F32 ringRate() const;

    void releaseGL();

    void renderWetPass();

    // <SS:Nexii> Was renderSnowPass(): now the albedo pass (doc sec 3), gated on any of wet/snow/ice/frost/stain having accumulated rather than snow depth alone.
    void renderAlbedoPass();

    S32 fieldCount() const { return (S32)mFields.size(); }
    F32 lastTickMS() const { return mLastTickMS; }
    F32 peakWet() const { return mPeakWet; }
    F32 peakSnow() const { return mPeakSnow; }
    F32 peakPuddle() const { return mPeakPuddle; }
    F32 peakIce() const { return mPeakIce; }
    F32 peakFrost() const { return mPeakFrost; }
    F32 peakStain() const { return mPeakStain; }

    // The resolved looks the shaders wear - the crossfade collapsed to one value each.
    SSSurfaceState::LiquidLook liquidLook() const { return SSSurfaceState::resolve(mLiquidMix); }
    SSSurfaceState::DepositLook depositLook() const { return SSSurfaceState::resolve(mDepositMix); }

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

        // <SS:Nexii> The runoff network: the DRAINAGE_NETWORK channel
        // materialised over a finer grid than the wet field's lattice (the
        // capture serves it, the wet field never sees it). SSWorldField::
        // buildDrainage fills depressions, directs D8 flow down the filled
        // surface and accumulates catchment; the eave rule terminates that
        // flow at the capture's discontinuities, and the edge cells holding
        // catchment flood-fill into RUNS - a connected line of lip cells that
        // shed the same way, the grouping layer the liquid shed hangs its
        // curtain streams and catchment-weighted drips on. The per-cell edge
        // arrays above stay the granular transport's spilling surface at the
        // wet field's own resolution; this is the liquid's.
        struct Run
        {
            std::vector<S32> mCells;        // member cells, ordered along the run
            std::vector<LLVector3> mLips;   // refined lip point per member, region-local XY, absolute Z
            F32 mCatch = 0.f;               // summed catchment, m2
            LLVector3 mOut;                 // mean outward direction, horizontal, normalised
            LLVector3 mCentroid;            // mean member lip, region-local XY, absolute Z
            U32 mKey = 0;                   // stable across retraces that keep the biggest feeder
        };
        struct Runoff
        {
            U32 mGeomSerial = 0xFFFFFFFFu;
            S32 mN = 0;
            F32 mCell = 0.f;
            std::vector<S32> mEdgeCells;
            std::vector<F32> mCatch;        // contributing area per cell, m2 (0 where none)
            std::vector<Run> mRuns;
            bool valid() const { return mN > 0 && mCell > 0.f; }
        };
        Runoff mRunoff;

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

        // <SS:Nexii> Surface weather state (doc/atmo_magic_surface_weather.md sec 2), stepped by SSSurfaceState::stepCell in tick() right after the settle pass above. All 0..1, zeroed wherever wet/snow/puddle are (non-solid, water, rebuilt).
        std::vector<F32> mIce;
        std::vector<F32> mFrost;
        std::vector<F32> mStain;
        std::vector<F32> mAge;

        // <SS:Nexii> Wind carry: the ground flow speed over SSAtmoSnowLiftHi, clamped 0..1, written whole in idle() from the flow grid already sampled for the transport (not integrated, not decayed - a static figure per solve). Feeds the flow window's spare channel so blown snow can settle in a lee the sky-view exposure term alone would leave bare.
        std::vector<F32> mGroundSpeed01;

        // The granular transport's state: the per-cell lift figure the drift tier's spawn walk
        // reads, and the creep pass's inflow accumulator (one step's arrivals, applied after the
        // outflows so the exchange is order-independent).
        // <SS:Nexii> Seconds of accumulation hold left on each cell after a strike flash-boiled it. Counts down in the tick; while it is up the cell takes no wet, no pool and no snow, but its ordinary loss paths still run, so a scorched patch dries rather than freezing in place. One F32 per cell, zero for every cell that has never been struck.
        std::vector<F32> mScorch;

        std::vector<F32> mLift;
        std::vector<F32> mInflow;

        std::vector<F32> mStore;
        std::vector<F32> mAccum;

        // <SS:Nexii> The liquid shed's per-run state, keyed by Run::mKey so a retrace that
        // keeps a run's biggest feeder keeps its water: mRunStore is the roof reservoir the
        // rain fills and the eave drains (drops' worth, as mStore is for granular creep),
        // mRunAccum the fractional drip the run is owed. Runs are the liquid shed's only
        // sites - granular creep keeps debiting the per-cell mStore above and the per-lip
        // cursor drains it, one ledger per weather.
        std::map<U32, F32> mRunStore;
        std::map<U32, F32> mRunAccum;

        F64 mLastTouched = 0.0;
    };

private:
    void refreshGeometry();
    static void buildGeometry(const SSRainShadowMap::SurfaceGrid& grid, Geometry& out);
    static void buildRunoff(U64 region_handle, Geometry& out, SSWorldField* field);

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

    // <SS:Nexii> The state window (ice, frost, stain, age), same lattice/origin as mWindowTex - filled in updateWindow() beside the other two.
    U32 mWindowStateTex = 0;
    std::vector<F32> mWindowStateData;

    // <SS:Nexii> The cover window (the world field's enclosure spectrum), same lattice/origin - filled in updateWindow() beside the other three.
    U32 mWindowCoverTex = 0;
    std::vector<F32> mWindowCoverData;

    std::map<U64, S32> mShedCursor;

    F32 mLastTickMS = 0.f;
    F32 mPeakWet = 0.f;
    F32 mPeakSnow = 0.f;
    F32 mPeakPuddle = 0.f;
    F32 mPeakIce = 0.f;
    F32 mPeakFrost = 0.f;
    F32 mPeakStain = 0.f;

    // <SS:Nexii> Scratch peaks for the look crossfade only (not exposed - the mix is the field's own business): the largest per-cell wet/deposit GAIN any tick landed this idle(), read by the advanceMix call at the end of idle() and reset alongside the others.
    F32 mPeakWetGain = 0.f;
    F32 mPeakDepositGain = 0.f;

    // <SS:Nexii> The "present" normaliser advanceMix divides by must be scoped to the SAME tick as the gain above it, or two clients replaying the same shared-time quanta at different frame rates (ran > 1 vs ran == 1) see a different mT out of an identical world state - mPeakWet/mPeakSnow above stay idle()-scoped for peakWet()/peakSnow()'s external readers.
    F32 mPeakWetPresent = 0.f;
    F32 mPeakDepositPresent = 0.f;

    // <SS:Nexii> The field holds ONE liquid and ONE deposit look at a time (doc sec 2); the mix crossfades from mA to whatever the active preset currently prescribes as it accumulates, promoting at t>=1. Defaults (water, snow) so an untouched field starts as the old code always looked.
    SSSurfaceState::Mix<SSSurfaceState::LiquidLook> mLiquidMix;
    SSSurfaceState::Mix<SSSurfaceState::DepositLook> mDepositMix;

    // <SS:Nexii> Impact rings within RING_NEAR_M of the camera (doc sec 6) - one buffer, camera-relative, not per-region: an impact is either near enough to ripple analytically or it is not.
    SSSurfaceState::RingBuffer mRings;
};

#endif
