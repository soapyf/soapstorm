/**
 * @file ssprecipitation.h
 * @brief Atmo Magic: precipitation particle simulation.
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

#ifndef SS_PRECIPITATION_H
#define SS_PRECIPITATION_H

#include "ssatmomagic.h"

#include "llpointer.h"
#include "v3math.h"
#include "v4coloru.h"

#include <cfloat>
#include <vector>

class LLViewerTexture;

enum SSPrecipFlags : U8
{
    PART_SWAY   = 0x01,
    PART_GUSTY  = 0x02,
    PART_LANDED = 0x04,
    PART_DRIP   = 0x08,
    PART_STREAM = 0x10,
    PART_CROWN  = 0x20
};

static const S32 SS_STREAM_SEGMENTS = 8;

static const F32 SS_STREAM_TEX_METRES = 1.6f;

inline F32 ssStreamScroll(F32 repeats, F32 fall_time)
{
    return repeats / llmax(fall_time, 0.05f);
}

static const U32 SS_PRECIP_MAX_TEXTURES = 64;

struct SSPrecipParticle
{
    LLVector3 mPos;
    LLVector3 mVel;
    LLVector3 mNormal = LLVector3(0.f, 0.f, 1.f);
    F32 mPlaneD = -FLT_MAX;
    F32 mFloorZ = -FLT_MAX;
    // <SS:Nexii> The z the particle started its fall from. The renderer fades a falling particle in over the top of that run (scaled to the run, capped at SS_PRECIP_TOP_FADE), so it needs the top, not just the floor. Zero for anything that doesn't fall from a top (risers, ripples, streams, drift).
    F32 mFallTop = 0.f;
    F32 mAge = 0.f;
    F32 mMaxAge = 1.f;
    F32 mSizeX = 0.05f;
    F32 mSizeY = 0.05f;
    F32 mAlpha = 1.f;
    F32 mGlow = 0.f;
    F32 mPhase = 0.f;
    LLColor4U mTint = LLColor4U(255, 255, 255, 255);
    U32 mSeed = 0;
    U8 mKind = KIND_ROUND;
    U8 mTex = 0;
    U8 mTier = TIER_DROPS;
    U8 mFlags = 0;
    U8 mMaterial = MAT_LIT;

    U8 mArt = TIER_CLUSTERS;

    F32 mRunSlope = 0.f;
};

inline F32 ssPrecipFadeOut(U8 tier) { return (tier == TIER_SHEETS) ? 0.8f : 0.25f; }

// <SS:Nexii> The cap on the fade-in band at the top of a falling particle's run: drops materialize across the top stretch of the fall instead of popping in at the spawn, scaled to about 60% of the run and never more. Long cluster and sheet curtains hit the full band; short near drops fade in over a few metres and stay dense below.
static const F32 SS_PRECIP_TOP_FADE = 48.f;

class SSPrecipSim
{
public:
    SSPrecipSim();

    void update(F32 dt);

    void spawnRipple(const LLVector3& pos_agent, F32 strength, bool on_water,
                     const LLVector3& normal, SSRandStream& rng);

    void spawnDrip(const LLVector3& lip_agent, const LLVector3& out_dir,
                   const LLVector3& land_agent, F32 volume, SSRandStream& rng);

    void refreshStream(U32 key, const LLVector3& lip_agent, const LLVector3& out_dir,
                       const LLVector3& land_agent, F32 strength, F32 width,
                       F32 run_slope, SSRandStream& rng);

    const std::vector<SSPrecipParticle>& streams() const { return mStreams; }
    S32 streamCount() const { return (S32)mStreams.size(); }

    S32 dripCount() const { return mDripCount; }

    static F32 dropRateAt(const LLVector3& pos_agent);

    void spawnShatter(const LLVector3& pos_agent, const LLVector3& normal,
                      const LLVector3& velocity, F32 strength, SSRandStream& rng);

    void shift(const LLVector3& offset);

    void clear();

    const std::vector<SSPrecipParticle>& particles() const { return mParticles; }
    const std::vector<SSPrecipParticle>& ripples() const { return mRipples; }
    const std::vector<SSPrecipParticle>& drift() const { return mDrift; }
    LLViewerTexture* texture(U8 index) const;
    bool empty() const { return mParticles.empty() && mRipples.empty() && mStreams.empty() && mDrift.empty(); }
    S32 tierCount(SSPrecipTier tier) const { return mTierCount[tier]; }
    S32 driftCount() const { return (S32)mDrift.size(); }

    // The drift pool's own cull radius - the tier bands are the falling tiers' alone, and the
    // renderer needs the same figure for its far fade.
    static F32 driftCullRadius();

    static void tierBands(SSPrecipTier tier, const SSPrecipPreset& preset,
                          F32& in_lo, F32& in_hi, F32& out_lo, F32& out_hi);

    static bool tierSprite(const SSPrecipPreset& preset, SSPrecipTier tier,
                           F32& quad_x, F32& quad_y, F32& drop_x, F32& drop_y, S32& splats);

private:
    void spawnTier(SSPrecipTier tier, U64 tick, F64 tick_time);
    void spawnTierCell(SSPrecipTier tier, U64 tick, F64 tick_time, S32 cx, S32 cy, F32 env,
                       const LLVector3& cam_agent, const LLVector3d& agent_origin_global);
    void emitParticle(SSPrecipTier tier, const LLVector3& hit_pos, F32 fall_len, F32 gust,
                      F32 size_jitter, F32 phase, F32 riser_age, F32 gust_jitter, U32 vis_seed,
                      bool has_floor);
    U8 textureIndex(LLViewerTexture* texturep);
    void resetTextureTable();
    void applyEmberFlavor(SSPrecipParticle& part, LLColor4& tint, SSRandStream& vis,
                          const SSPrecipPreset& preset);
    void pushRipple(const SSPrecipParticle& part);

    void respawnParticle(SSPrecipTier tier, U32 seed, const LLVector3& impact_pos, F32 env);

    std::vector<SSPrecipParticle> mParticles;
    std::vector<SSPrecipParticle> mRipples;
    S32 mDripCount = 0;

    // <SS:Nexii> Blowing snow: a separate pool on purpose. mTierCount/mTierTarget/tierBands stay the falling tiers' alone - a blizzard must never starve falling snow of budget, or vice versa - so drift carries its own cap and cull radius, batched by the renderer as one more source. Spawning walks the surface field's lift cells (the transport's output) plus a regime-scaled near-camera ring.
    std::vector<SSPrecipParticle> mDrift;
    void updateDrift(F32 dt);
    void spawnDriftTick(U64 tick, F64 tick_time);
    void emitDrift(const LLVector3& ground_pos, const LLVector3& flow, F32 lift,
                   SSRandStream& rng);
    U64 mLastDriftTick = 0;

    std::vector<SSPrecipParticle> mStreams;
    void updateStreams(F32 dt);
    std::vector<LLPointer<LLViewerTexture>> mTextures;
    S32 mTierCount[TIER_COUNT];
    F32 mTierTarget[TIER_COUNT] = { 0.f };
    F32 mTierSpawnAccum[TIER_COUNT] = { 0.f };
    F32 mMeanLife[TIER_COUNT] = { 0.f };
    U64 mLastTick[TIER_COUNT];
    size_t mRippleCursor = 0;
};

#endif
