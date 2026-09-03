/**
 * @file ssatmomagic.h
 * @brief Atmo Magic: synced deterministic weather manager.
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

#ifndef SS_ATMOMAGIC_H
#define SS_ATMOMAGIC_H

#include "ssprecippreset.h"

#include "llpointer.h"
#include "llrect.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"
#include "v4color.h"

#include <boost/signals2.hpp>

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

class LLViewerObject;
class LLViewerTexture;
class SSPrecipSim;
struct SSGranularParams;

namespace SSAtmoNoise
{
    inline U32 hashU32(U32 x)
    {
        x = x * 747796405u + 2891336453u;
        U32 w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
        return (w >> 22u) ^ w;
    }

    inline U32 combine(U32 a, U32 b) { return hashU32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2))); }

    inline F32 hash01(U32 x) { return (F32)(hashU32(x) & 0x00ffffffu) / (F32)0x01000000; }

    F32 value1(F32 x, U32 seed);
    F32 value2(F32 x, F32 y, U32 seed);
    F32 fbm1(F32 x, U32 seed, S32 octaves = 3);
    F32 fbm2(F32 x, F32 y, U32 seed, S32 octaves = 3);
}

class SSRandStream
{
public:
    explicit SSRandStream(U32 seed) : mState(seed ? seed : 0x2545f491u) {}

    U32 next()
    {
        mState = mState * 747796405u + 2891336453u;
        U32 w = ((mState >> ((mState >> 28u) + 4u)) ^ mState) * 277803737u;
        return (w >> 22u) ^ w;
    }
    F32 frand() { return (F32)(next() & 0x00ffffffu) / (F32)0x01000000; }
    F32 frand(F32 lo, F32 hi) { return lo + frand() * (hi - lo); }
    S32 rand(S32 n) { return n > 0 ? (S32)(next() % (U32)n) : 0; }

private:
    U32 mState;
};

struct SSAtmoAsset
{
    LLUUID mID;
    bool   mIsPBR = false;
};

class SSAtmoMagic : public LLSingleton<SSAtmoMagic>
{
    LLSINGLETON(SSAtmoMagic);
    ~SSAtmoMagic();

public:
    void idle();

    static void onObjectUpdate(LLViewerObject* objectp);

    bool isEnabled() const { return mEnabled; }

    bool isSwitchedOn() const { return mSwitchedOn; }

    F64 sharedTime() const { return mNow; }
    U32 seed() const;

    F32 precipitation() const { return mPrecipitation; }
    F32 turbulence() const { return mTurbulence; }
    F32 windSpeed() const { return mWindSpeed; }

    // <SS:Nexii> The cube's intensity-band drop character: droplet render-size scale and splash strength, 0..1 graded per band (drizzle taps, torrential hammers), or -1 with no environment. ssprecipitation modulates the preset's authored values by these where the preset opts in (mWeatherSize / mWeatherImpact).
    F32 dropletScale() const { return mDropletScale; }
    F32 impactScale() const { return mImpactScale; }

    F32 temperatureC() const { return mTemperatureC; }

    // <SS:Nexii> Storm-approach look-ahead: how imminent an approaching thunderstorm is (0..1) from the weather cube's next keyframe (SSAtmoEnvBridge::stormApproach), plus the upwind heading in degrees it comes FROM - negative when none approaches. SSLightning's bolt-from-the-blue anticipation reads these every frame.
    F32 stormApproach() const { return mStormApproach; }
    F32 stormApproachHeadingDeg() const { return mStormApproachHeading; }

    // <SS:Nexii> Granular weather: the single lift authority and the regime machine. liftAt() answers "is snow lifting here and how hard" (0-1, physical, no preset rate, no gust - the caller applies those); the regimes derived from the same env params the resolver already produces direct presentation only. doc/atmo_magic_snow.md sections 1 and 14.
    F32 liftAt(const LLVector3& pos_agent) const;
    bool granularWeather() const;
    F32 squallFactor() const { return mSquallFactor; }

    // The transport's parameter bundle for this tick - plain floats from settings, preset, regime and the gust envelope. Defined in ssgranular.h; consumers include that.
    void fillTransportParams(SSGranularParams& params) const;

    enum class ERegime : U8
    {
        CALM = 0,
        SALTATION,
        DRIFT,
        BLIZZARD,
        SQUALL,
        COUNT
    };
    ERegime regime() const { return mRegime; }
    static const char* regimeName(ERegime r);

    // Bounded by design: soundscape bed crossfade, floater stats, whiteout ramp. A second event type gets promoted to a real pump consciously, never by accretion.
    typedef boost::signals2::signal<void(ERegime, ERegime)> RegimeSignal;
    RegimeSignal& regimeSignal() { return mRegimeSignal; }

    bool lightningOn() const { return mLightning; }

    const LLColor3& lightningColor() const { return mLightningColor; }
    LLColor3 lightningCoreColor() const
    {
        const F32 w = llclamp(mLightningCoreWhite, 0.f, 1.f);
        return LLColor3(lerp(mLightningColor.mV[0], 1.f, w),
                        lerp(mLightningColor.mV[1], 1.f, w),
                        lerp(mLightningColor.mV[2], 1.f, w));
    }
    bool lightningCharge() const { return mLightningCharge; }
    bool lightningSparks() const { return mLightningSparks; }
    F32 lightningIntervalMin() const { return mLightningIntervalMin; }
    F32 lightningIntervalMax() const { return mLightningIntervalMax; }
    F32 lightningIntensity() const { return mLightningIntensity; }

    F32 gustDepth() const { return mGustDepth; }
    F32 gustLength() const { return mGustLength; }
    F32 gustVeer() const { return mGustVeer; }
    LLVector3 windXY() const { return mWindXY; }

    LLVector3 wind() const { return mWind; }

    S32 track() const { return mTrack; }

    F32 groundZero() const { return mGroundZero; }
    bool isSkyTrack() const { return mSkyTrack; }

    F32 fallThrough() const { return mFallThrough; }

    F32 trackBlend() const { return mBlend; }

    const SSPrecipPreset& preset() const { return mPreset; }
    bool hasWeather() const { return mHasWeather; }

    static F32 voidWaterHeight();

    LLVector3 rainDirection() const { return mRainDirection; }

    F32 gustEnvelopeAt(F64 time) const;

    F64 windDrift() const { return mWindDrift; }
    F32 areaFactorAt(F64 global_x, F64 global_y) const;

    LLViewerTexture* pickParticleTexture(SSRandStream& rng, LLColor4& tint, F32& glow);
    LLViewerTexture* rippleTexture();
    static LLViewerTexture* textureFromList(const std::string& csv);

    void queueImpact(F64 time, const LLVector3& pos_agent, F32 strength, bool on_water,
                     const LLVector3& normal, const LLVector3& velocity, bool shatter,
                     bool from_runoff = false);

    SSPrecipSim* sim() { return mSim.get(); }

    void shift(const LLVector3& offset);

    static void drawInfo();

    // <SS:Nexii> Click hit-test for the info overlay's orange headings: x/y in
    // scaled window coordinates (the space drawInfo lays out in). A hit toggles
    // that section's collapse and returns true so the click is consumed.
    static bool handleInfoClick(S32 x, S32 y);

    size_t pendingImpacts() const { return mImpacts.size(); }

    void renderDebug();

private:
    void refreshParams();
    void refreshAssets();
    void ensureSim();
    void processImpacts();
    void updateRegime(F32 dt);
    LLViewerTexture* textureFor(const SSAtmoAsset& asset, LLColor4& tint, F32& glow);

    struct PendingEdit
    {
        LLVector3 mPos;
        F32 mRadius = 0.f;
        F64 mSettleAt = 0.0;

        F64 mFirstSeen = 0.0;
        U32 mResets = 0;
    };

    void settleEdits();

    static LLColor4 colorForEdit(const PendingEdit& edit);

public:
    size_t pendingEdits() const { return mPendingEdits.size(); }
    U32 settledEdits() const { return mSettledEdits; }

private:

    std::map<LLUUID, PendingEdit> mPendingEdits;
    U32 mSettledEdits = 0;

    F64 mNow = 0.0;
    bool mEnabled = false;
    bool mSwitchedOn = false;

    F32 mTemperatureC = 15.f;

    // <SS:Nexii> The bolt-from-the-blue storm look-ahead, recomputed every refresh. See the getters.
    F32 mStormApproach = 0.f;
    F32 mStormApproachHeading = -1.f;

    LLColor3 mLightningColor{0.62f, 0.55f, 1.f};
    F32 mLightningCoreWhite = 0.85f;

    bool mLightning = true;
    bool mLightningCharge = true;
    bool mLightningSparks = true;
    F32 mLightningIntervalMin = -1.f;
    F32 mLightningIntervalMax = -1.f;
    F32 mLightningIntensity = -1.f;

    F32 mPrecipitation = 0.f;
    F32 mTurbulence = 0.f;
    F32 mDropletScale = -1.f;
    F32 mImpactScale = -1.f;
    F32 mWindSpeed = 0.f;
    F32 mGustDepth = 0.f;
    F32 mGustLength = 140.f;
    F32 mGustVeer = 0.f;
    F64 mWindDrift = 0.0;
    bool mWindDriftSeeded = false;
    LLVector3 mWindXY;
    LLVector3 mWind;
    LLVector3 mRainDirection;
    SSPrecipPreset mPreset;
    bool mHasWeather = false;

    // <SS:Nexii> Regime state. The dwell timer is the hysteresis in both directions; the initial regime is derived, so a viewer joining mid-storm starts right without history.
    ERegime mRegime = ERegime::CALM;
    F32 mRegimeCandidateTime = 0.f;
    bool mRegimeReady = false;
    F32 mSquallFactor = 0.f;
    RegimeSignal mRegimeSignal;

    S32 mTrack = 1;
    F32 mGroundZero = 0.f;
    bool mSkyTrack = false;
    F32 mFallThrough = 1.f;

    F32 mBlend = 0.f;
    std::string mPresetName;

    F32 mV3PrevWorldZ = 0.f;
    bool mV3PrevWorldZValid = false;
    LLUUID mV3PrevRegionID;

    std::unique_ptr<SSPrecipSim> mSim;

    struct Impact
    {
        LLVector3 mPosAgent;
        LLVector3 mNormal;
        LLVector3 mVelocity;
        F32 mStrength;
        bool mOnWater;
        bool mShatter;
        bool mRunoff;
    };
    std::multimap<F64, Impact> mImpacts;

    std::vector<SSAtmoAsset> mTextureAssets;
    LLPointer<LLViewerTexture> mRippleTexture;
    std::string mAssetsFingerprint;
    F64 mLastAssetPoll = 0.0;

    // <SS:Nexii> Info overlay: collapsed sections keyed by section name, and
    // the heading hit rects the last draw laid out.
    std::set<std::string> mInfoCollapsed;
    std::vector<std::pair<std::string, LLRect> > mInfoHeadingRects;
};

#endif
