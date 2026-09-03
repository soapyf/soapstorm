/**
 * @file sslightning.h
 * @brief Atmo Magic: lightning model - scheduling, channels, discharge phases, ground show.
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

#ifndef SS_LIGHTNING_H
#define SS_LIGHTNING_H

#include "llsingleton.h"
#include "v3math.h"
#include "v3color.h"
#include "v4color.h"
#include "v4math.h"

#include <vector>

class SSRandStream;
class LLHUDText;

// <SS:Nexii> Decay timing shared by the model and the renderer so a strike's lifetime always covers its whole show. After a return stroke each node's alpha mask turns up at its own moment in the window (LAG..LAG+SPAN, spatially coherent along the channel); from that instant its stretch of channel lives on as a widening, eroding plasma cloud for PLASMA_S (the amber foot knot PLASMA_FOOT_MULT longer), the way the recorded column cools white-blue to grey over 0.4s. EMBER_S is the old dying-spark linger kept for the plasma-off fallback. doc/atmo_magic_lightning_strike.md
namespace SSDissolve
{
    constexpr F32 LAG_S = 0.05f;
    constexpr F32 SPAN_S = 0.30f;
    constexpr F32 EMBER_S = 0.18f;
    constexpr F32 PLASMA_S = 0.36f;
    constexpr F32 PLASMA_FOOT_MULT = 1.6f;
}

// <SS:Nexii> Ground-show timing shared with the renderer: the fire's ignition ramp (the recorded fire line is at peak ~0.1s after contact, not at the stroke instant), the impact sparks' secondary life, and the visible speed the ignition spreads outward along the crawl.
namespace SSGroundShow
{
    constexpr F32 FIRE_RISE_S = 0.08f;
    constexpr F32 SECONDARY_LIFE_S = 0.6f;
    constexpr F32 CRAWL_ARC_M_S = 60.f;
    constexpr S32 AURA_PATCHES = 4;

    // <SS:Nexii> The steam burst's clock: the flash-boil itself is near-instant (a stroke deposits its energy in microseconds, and the recorded puff is already at full width within a frame or two), then the cloud rises and thins over its life. Held apart from the fire's timing because steam outlives the flare and dies well before the fire does.
    constexpr F32 STEAM_BURST_S = 0.12f;
    constexpr F32 STEAM_LIFE_S = 1.6f;
    constexpr F32 STEAM_RISE_M_S = 1.8f;
}

enum SSStrikeKind : U8
{
    STRIKE_SHEET = 0,

    STRIKE_FORK,

    STRIKE_GROUND,

    STRIKE_KIND_COUNT
};

struct SSStrikeNode
{
    LLVector3 mPos;
    S32 mParent = -1;
    F32 mWidth = 1.f;
    F32 mReachedAt = 0.f;
    bool mTrunk = false;

    // <SS:Nexii> Ground crawl node: lies on the surface past the attachment, drawn as a ground-conforming ribbon, hosts fire blobs and spark launches.
    bool mCrawl = false;

    // <SS:Nexii> Path metres from the channel root (continuous texture and noise coordinate along the bolt) and path metres to the attachment (the amber gradient's input, 0 at the foot and on the crawl, huge on anything that never reaches the ground). Both set by finishChannel.
    F32 mPathM = 0.f;
    F32 mTipDistM = 1.0e9f;

    // <SS:Nexii> This node's plasma pop threshold in the dissolve window, spatially coherent along the channel so the column breaks into stretches rather than confetti.
    F32 mThr = 0.5f;

    mutable F32 mOcc = 1.f;
};

// <SS:Nexii> One impact spark, rolled at spawn so the per-frame path is closed-form ballistics with no terrain lookups: it leaves mFrom at mT0 after a stroke, flies its heading at mVH with vertical launch mVZ, meets the surface at mHit (0 when it never lands, e.g. off a roof edge) at height mLandZ, and dies at mLife.
struct SSStrikeSpark
{
    LLVector3 mFrom;
    F32 mCos = 1.f;
    F32 mSin = 0.f;
    F32 mVH = 10.f;
    F32 mVZ = 3.f;
    F32 mT0 = 0.f;
    F32 mLife = 1.f;
    F32 mHit = 0.f;
    F32 mLandZ = 0.f;
    F32 mRadius = 0.07f;
    U32 mSeed = 0;
};

// <SS:Nexii> One ground-fire blob: a disc on the surface along the crawl, the impact fan or a spark landing, igniting mIgnite after a stroke (the ignition runs outward along the crawl) and burning its own share of the fire tail.
struct SSStrikeFire
{
    LLVector3 mPos;
    F32 mRadius = 1.5f;
    F32 mIgnite = 0.f;
    F32 mLifeMul = 1.f;
    U32 mSeed = 0;

    // A spark's landing ember ages from the first stroke (its spark flew once); crawl and fan blobs age from the latest stroke, so a restrike re-ignites them.
    bool mEmber = false;
};

// <SS:Nexii> One steam burst where the strike flash-boiled the ground. The disc positions, radii and delays are rolled at spawn like the fire blobs, but mWater cannot be: it is the water the field actually held AT CONTACT, ten seconds after the roll, so it is filled in once when the stroke lands and is zero for every blob over dry ground - which is what makes a strike on a dry road produce no steam at all. doc/atmo_magic_lightning_strike.md
struct SSStrikeSteam
{
    LLVector3 mPos;
    F32 mRadius = 1.5f;

    // Seconds after contact this blob blows, spread outward along the crawl at the same arc speed the fire ignition uses.
    F32 mDelay = 0.f;

    F32 mWater = 0.f;
    U32 mSeed = 0;
};

struct SSStrike
{
    SSStrikeKind mKind = STRIKE_SHEET;

    F64 mFireAt = 0.0;
    F64 mCreatedAt = 0.0;

    F32 mT = 0.f;

    F32 mIntensity = 1.f;

    LLVector3 mOrigin;
    LLVector3 mGround;
    F32 mDistanceM = 0.f;
    bool mAudible = false;

    std::vector<SSStrikeNode> mChannel;

    F32 mChannelBrightness = 0.f;

    // Branch exclusion for ground strikes, filled by buildChannel: a cone about the main line's
    // foot and a floor over the attachment that forked channels must stay out of.
    bool mBranchLimits = false;
    LLVector3 mBranchConeApex;
    LLVector3 mBranchConeAxis;
    F32 mBranchConeDot = 0.f;
    F32 mBranchFloorZ = 0.f;

    // <SS:Nexii> How far an in-cloud channel's runs sag below their endpoints as they travel horizontally - the dip taking an intra-cloud bolt under the deck's base and back up. Zero for a ground strike's straight-down trunk. Filled by buildChannel.
    F32 mCloudDipM = 0.f;

    // <SS:Nexii> Total channel path length from its root, derived after build. Every node's reach is its path distance from the root (see buildChannel), so the leader sweeps the whole bolt at one continuous crawl, its duration the distance divided by the visible leader speed - long bolts take time to travel.
    F32 mChannelLenM = 0.f;

    // <SS:Nexii> Debug-forced placements (Strike Now / Ground Strike buttons) keep their kind: an explicitly aimed ground strike is not re-routed by the under deck.
    bool mForced = false;

    // <SS:Nexii> Polarity. Negative bolts - the summer norm - come off the cloud's BOTTOM, close to the ground, sharp and quick. Positive bolts - the winter storm's network - launch from the cloud top (the anvil), far more powerful, firing a rapid series of quick pulses. Rolled at spawn from the temperature, deterministic per strike.
    bool mPositive = false;

    // <SS:Nexii> Bolt from the blue: a positive anvil discharge travelling huge horizontal distance before falling miles from its cloud. The origin sits at the anvil crown, far off - the storm's own position - the trunk running the whole gap to the far clip and striking ground within view.
    bool mBlue = false;

    // <SS:Nexii> The polarity's stroke timing and power, rolled at spawn: positive bolts fire more return strokes in a quicker series, hold the glow longer, and throw light further.
    S32 mStrokesMin = 1;
    S32 mStrokesMax = 4;
    F32 mRestrikeMinS = 0.03f;
    F32 mRestrikeMaxS = 0.09f;
    F32 mStrokeDecayS = 0.055f;
    F32 mPower = 1.f;

    static const S32 MAX_STROKES = 12;
    S32 mStrokeCount = 0;
    F32 mStrokeAt[MAX_STROKES] = { 0.f };
    F32 mStrokeBright[MAX_STROKES] = { 0.f };

    // <SS:Nexii> Beside the decayed brightness: each fired stroke's undecayed scale (the plasma and fire envelopes read it), and the wind-drift seconds it has earned - fast gaps count whole, a late restrike's long gap barely, because a dart leader follows the existing channel and must not land metres downwind of it.
    F32 mStrokeScale[MAX_STROKES] = { 0.f };
    F32 mStrokeDrift[MAX_STROKES] = { 0.f };

    F32 mLeaderProgress = 0.f;

    F32 mFlash = 0.f;

    F32 mCharge = 0.f;
    bool mChargeSent = false;

    // <SS:Nexii> The peak charge reached, HELD through the leader phase (mCharge itself snaps to zero the moment the leader starts, up to 1.2s before contact) so the aura survives to the strike and the amber flare can take over from it without a dark gap.
    F32 mChargeHeld = 0.f;

    // <SS:Nexii> The ground show's envelopes, computed here every frame from the stroke chain so the renderer stays stateless and sceneLights, retirement and the discs all read the same numbers. mHit is the amber impact flare (per stroke summed, fading with the bolt at 1.5x its decay); mFire the ground fire (max over strokes, 80ms ignition, the slow tail SSAtmoLightningGroundFireLife sets); mPlasmaSince the seconds since the latest stroke (the plasma column is drawn for the latest stroke only, so a restrike resets it); mLastGap the gap before that stroke (a late restrike re-lights a pinched column as distinct beads).
    F32 mHit = 0.f;
    F32 mFire = 0.f;
    F32 mPlasmaSince = -1.f;
    F32 mLastGap = 0.f;

    // <SS:Nexii> Ground crawl bookkeeping: the crawl nodes are a contiguous run of mChannel starting at mCrawlStart (-1 none), grown at spawn along the surface past the attachment - 0-30m, longer and steered over wet ground and puddles.
    S32 mCrawlStart = -1;
    S32 mCrawlCount = 0;
    F32 mCrawlLenM = 0.f;
    F32 mCrawlBearing = 0.f;

    // <SS:Nexii> The charge aura's patch discs, positions and radii fixed at spawn around the attachment (the old corona re-hashed them per frame from a contracting spread, so the haze grew then shrank). The amber flare and the fire reuse the very same discs, which is what lets the hit crossfade out of the charge without a pop.
    LLVector3 mAuraPos[SSGroundShow::AURA_PATCHES];
    F32 mAuraR[SSGroundShow::AURA_PATCHES] = { 0.f };
    F32 mAuraSurfZ[SSGroundShow::AURA_PATCHES] = { 0.f };
    F32 mAuraCentreR = 0.f;

    std::vector<SSStrikeSpark> mSparks;
    std::vector<SSStrikeFire> mFireBlobs;

    // <SS:Nexii> The steam burst: candidate discs rolled at spawn, charged with the field's live water at contact by vaporiseGround() (once - a restrike falls on ground its own first stroke already boiled dry), and the loudest blob's water kept so retirement and the renderer can skip a strike that found nothing to boil.
    std::vector<SSStrikeSteam> mSteam;
    bool mVaporised = false;
    F32 mSteamPeak = 0.f;

    // <SS:Nexii> The ground show's bounding box (attachment, crawl, spark landings, fire blobs), for the renderer's frustum test and its occlusion query.
    LLVector3 mGroundBoxMin;
    LLVector3 mGroundBoxMax;

    LLHUDText* mDebugText = nullptr;

    mutable F32 mOccAt = -1.0e9f;
    mutable LLVector3 mOccCam;

    // <SS:Nexii> The renderer's occlusion query on the ground box: a pooled GL query name (0 none), the frame it was issued, and the last answer. Released by the model on retirement and clear so the value-copied strike never double-frees.
    mutable U32 mOccQuery = 0;
    mutable U32 mOccIssuedFrame = 0;
    mutable bool mOccHidden = false;

    bool mDone = false;
    bool mThunderSent = false;
    bool mSparksSent = false;
};

class SSLightning : public LLSingleton<SSLightning>
{
    LLSINGLETON_EMPTY_CTOR(SSLightning);

public:
    void idle(F32 dt);

    const std::vector<SSStrike>& strikes() const { return mStrikes; }

    F32 flash() const { return mFlash; }

    const LLVector3& flashDirection() const { return mFlashDir; }

    S32 sceneLights(std::vector<LLVector4>& out_pos_radius,
                    std::vector<LLColor3>& out_color, S32 max_count) const;

    void triggerNow();

    void triggerGroundNow();

    void clear();

    S32 liveCount() const { return (S32)mStrikes.size(); }
    F64 nextStrikeIn() const;
    static const char* kindName(SSStrikeKind k);
    static const LLColor4& kindDebugColor(SSStrikeKind k);

    // <SS:Nexii> How likely a strike is a positive anvil discharge at a temperature - summer's network runs negative, deep winter all positive. The overlay reads it to label the mood.
    static F32 positiveSkew(F32 temperature_c);

    // <SS:Nexii> The surface height under a point for the ground show: the surface field's capture where it is live, the wind flowmap's height capture next, the terrain last, never below the water. Client-local by nature (fields and captures differ per viewer), which is why only ground-show geometry reads it.
    static F32 surfaceZ(const LLVector3& pos_agent);

    // The pending-strike debug overlay (markers, countdown label) hides this long before impact
    // so the preview does not sit on top of the strike it announced.
    static constexpr F32 MARKER_HIDE_S = 0.5f;

private:
    void spawn(F32 intensity, F64 fire_at, F32 force_bearing = -1.f, F32 force_dist = -1.f,
               SSStrikeKind force_kind = STRIKE_KIND_COUNT, const LLVector3* force_ground = nullptr,
               bool force_blue = false);
    void buildChannel(SSStrike& strike, F32 intensity);

    void growPath(SSStrike& strike, S32 parent,
                  const LLVector3& from, const LLVector3& to,
                  S32 levels, F32 width_start, F32 width_end,
                  F32 t_start, F32 t_end, bool trunk,
                  SSRandStream& rng, std::vector<S32>& out_nodes,
                  bool sag = false);

    void growBranches(SSStrike& strike, const std::vector<S32>& along,
                      S32 depth, S32 levels, F32 intensity, SSRandStream& rng,
                      F32 fecundity = 1.f);

    // <SS:Nexii> The ground crawl: from the trunk's foot, one or two arms of surface-following nodes on their own random stream (0-30m, quadratic roll, lengthened by wet ground), each step choosing among three pre-rolled headings by the wetness two steps ahead, stopping at a wall or a drop. Grown before the branches so the node cap cannot starve it.
    void growCrawl(SSStrike& strike, S32 foot, F32 intensity);

    // <SS:Nexii> Re-routes a ground strike whose channel would cross the under deck into a cloud-to-cloud crawler with branching ends inside that deck - the bolt to nowhere below a floating build becomes a fork instead. Grows the channel itself; true when re-routed.
    bool underDeckDivert(SSStrike& strike, SSRandStream& rng);

    // After all geometry is grown: every node's reach becomes its path distance from the root
    // (normalized to progress); total length stored for the leader's travel time; path metres,
    // tip distances and plasma thresholds derived alongside.
    void finishChannel(SSStrike& strike);

    // <SS:Nexii> The ground show's spawn-time tables for a ground strike: aura discs, impact spark ballistics, fire blobs and the bounding box - all hashed from the fire time so every frame and every client reads the same show.
    void buildGroundShow(SSStrike& strike);

    // Charges the strike's steam blobs from the live surface field and takes that water out of it - once, at contact.
    void vaporiseGround(SSStrike& strike);

    void advance(SSStrike& strike, F32 dt);

    std::vector<SSStrike> mStrikes;

    F64 mNextStrikeAt = -1.0;
    bool mPrepared = false;

    // <SS:Nexii> The bolt-from-the-blue scheduler: its own next-fire clock, independent of the ordinary strike interval, so lightning reaches ahead of an approaching storm before the current weather is thundery. Cleared when the storm approach dies.
    F64 mNextBlueAt = -1.0;
    bool mBluePrepared = false;

    F32 mFlash = 0.f;
    LLVector3 mFlashDir;
};

#endif
