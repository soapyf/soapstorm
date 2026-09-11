/**
 * @file sssoundscape.cpp
 * @brief See sssoundscape.h.
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

#include "sssoundscape.h"
#include "ssatmomagic.h"
#include "ssatmostore.h"
#include "sswindflow.h"
#include "ssprecippreset.h"
#include "sssurfacefield.h"
#include "sssoundmeta.h"
#include "ssworldfieldshapes.h"

#include "llrand.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llaudioengine.h"
#include "llmutelist.h"
#include "llviewerparcelmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "lldrawable.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "pipeline.h"

static const F64 PROBE_INTERVAL    = 0.05;
static const F32 MOVE_TRIGGER     = 0.2f;
static const F64 STALE_TRIGGER    = 1.0;
static const F32 UP_RAY_LENGTH    = 100.f;
static const S32 UP_RAY_COUNT     = 3;
static const F32 UP_RAY_TILT      = 12.f;
static const F32 SIDE_RAY_LENGTH  = 50.f;
static const F32 SMALL_SPACE_AVG  = 10.f;
static const F32 MEDIUM_SPACE_AVG = 30.f;
static const F32 IMPACT_RATE_FULL = 22.f;
static const F32 IMPACT_RATE_TAU  = 1.5f;
static const F32 COVER_BLEND_RATE = 8.f;

static const F32 BURIAL_FULL       = 12.f;
static const F32 BURIAL_MAX_DUCK   = 0.85f;
static const F32 BURIAL_BLEND_RATE = 2.5f;
// The burial figure the air flood's sealed-room verdict wants to stand at: a
// point the connectivity walk cannot reach from sky or border is as enclosed
// as the muffle can express, whether or not the band stack shows a roof only
// a few metres up.
static const F32 BURIAL_INTERIOR_DEPTH = 18.f;

// <SS:Nexii> The through-solid transmission's half-loss thickness (the doc's Part 3):
// a direct line crossing this many solid metres loses half its energy. One material
// until span flags grow absorption classes - the doc's own stated guess - so the dial
// sits between "a 0.5 m wall barely matters" and "a 10 m berm is silence".
static const F32 OCCLUSION_HALF_LOSS_M = 2.f;

static LLTrace::BlockTimerStatHandle FTM_SS_AUDIO("Atmo Magic Audio");
static LLTrace::BlockTimerStatHandle FTM_SS_AUDIO_PROBE("Cover Probes");

// Triangle envelope: 0 at lo, 1 at peak, 0 at hi.
static F32 tri(F32 x, F32 lo, F32 peak, F32 hi)
{
    if (x <= lo || x >= hi) return 0.f;
    return x < peak ? (x - lo) / llmax(0.01f, peak - lo)
                    : (hi - x) / llmax(0.01f, hi - peak);
}

// CSV to UUID list.
static void parseSoundList(const std::string& value, std::vector<LLUUID>& out)
{
    out.clear();
    std::string::size_type pos = 0;
    while (pos < value.size())
    {
        std::string::size_type end = value.find(',', pos);
        if (end == std::string::npos) end = value.size();
        std::string token = value.substr(pos, end - pos);
        pos = end + 1;

        LLStringUtil::trim(token);
        if (LLUUID::validate(token))
        {
            out.push_back(LLUUID(token));
        }
    }
}

// Stops and forgets one ambient loop.
void SSSoundscape::releaseLoop(Loop& loop)
{
    if (gAudiop && loop.mSourceID.notNull())
    {
        LLAudioSource* source = gAudiop->findAudioSource(loop.mSourceID);
        if (source)
        {
            gAudiop->cleanupAudioSource(source);
        }
    }
    loop.mSourceID.setNull();
    loop.mGain = 0.f;
    loop.mTarget = 0.f;
}

// Silence everything - the off switch.
void SSSoundscape::stopAll()
{
    for (Loop& loop : mLoops)
    {
        releaseLoop(loop);
    }
    // <SS:Nexii> The auto-sort ladder's voices live outside mLoops: without this drain an ambient voice active when the gate hit keeps its looping source alive on a parcel with no environment.
    if (gAudiop)
    {
        for (auto& pair : mAmbientVoices)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(pair.second.mSourceID))
            {
                gAudiop->cleanupAudioSource(source);
            }
        }
    }
    mAmbientVoices.clear();
    mImpactRate = 0.f;
}

// Feeds an impact into the rate estimate that drives the ambient mix.
void SSSoundscape::notifyImpact(F32 strength)
{
    mImpactRate += strength;
}

// One upward roof raycast slot; distance out when it hits.
bool SSSoundscape::castUpProbe(S32 index, F32& hit_dist)
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 azimuth = (F32)index * (F_TWO_PI / (F32)UP_RAY_COUNT) + 0.7f;
    const F32 tilt = UP_RAY_TILT * DEG_TO_RAD;
    const LLVector3 dir(cosf(azimuth) * sinf(tilt), sinf(azimuth) * sinf(tilt), cosf(tilt));

    // <SS:Nexii> Exact declared-shape cast first (doc/atmo_magic_worldfield_competition.md 10):
    // wall-exact ceiling answers without the octree walk; the census includes
    // phantom decks the stock ray's skip_phantom would ignore, so a phantom
    // roof finally shelters. Unanswered census falls through to the stock ray.
    static LLCachedControl<bool> shapes_cast(gSavedSettings, "SSWorldFieldShapes", false);
    if (shapes_cast)
    {
        SSWorldFieldShapes::SegmentHit probe_hit;
        if (SSWorldFieldShapes::getInstance()->segmentCast(cam, cam + dir * UP_RAY_LENGTH, probe_hit))
        {
            hit_dist = probe_hit.mHit ? probe_hit.mDistance : UP_RAY_LENGTH;
            return probe_hit.mHit;
        }
    }

    LLVector4a start4, end4, intersect;
    start4.load3(cam.mV);
    const LLVector3 end = cam + dir * UP_RAY_LENGTH;
    end4.load3(end.mV);

    if (gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true))
    {
        LLVector4a delta = intersect;
        delta.sub(start4);
        hit_dist = delta.getLength3().getF32();
        return true;
    }
    hit_dist = UP_RAY_LENGTH;
    return false;
}

// One horizontal wall raycast slot; how far the room extends that way.
F32 SSSoundscape::castSideProbe(S32 index)
{
    static const LLVector3 cardinals[4] = {
        LLVector3(1.f, 0.f, 0.f), LLVector3(-1.f, 0.f, 0.f),
        LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, -1.f, 0.f)
    };

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    // <SS:Nexii> Exact declared-shape cast first - the wall profile becomes
    // wall-exact instead of cell-quantized; unanswered census falls through.
    static LLCachedControl<bool> shapes_cast(gSavedSettings, "SSWorldFieldShapes", false);
    if (shapes_cast)
    {
        SSWorldFieldShapes::SegmentHit probe_hit;
        if (SSWorldFieldShapes::getInstance()->segmentCast(cam, cam + cardinals[index] * SIDE_RAY_LENGTH, probe_hit))
        {
            return probe_hit.mHit ? probe_hit.mDistance : SIDE_RAY_LENGTH;
        }
    }

    LLVector4a start4, end4, intersect;
    start4.load3(cam.mV);
    const LLVector3 end = cam + cardinals[index] * SIDE_RAY_LENGTH;
    end4.load3(end.mV);

    if (gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true))
    {
        LLVector4a delta = intersect;
        delta.sub(start4);
        return delta.getLength3().getF32();
    }
    return SIDE_RAY_LENGTH;
}

// Rotates through the roof and wall probes so cover and room size stay current without a raycast storm.
void SSSoundscape::updateProbes(F64 now)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_AUDIO_PROBE);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    // The sealed-room verdict only survives a cycle that re-answers it: reset
    // here, above the movement/staleness gates, so turning the field source
    // off or losing its tile clears the flag on the very next frame instead of
    // carrying a stale interior boost into an open area.
    mInterior = false;

    const bool moved = (cam - mProbeAnchor).magVec() > MOVE_TRIGGER;
    const bool stale = now - mLastCycleDone > STALE_TRIGGER;
    if (!moved && !stale) return;
    if (now - mLastCycleDone < PROBE_INTERVAL) return;

    // The enclosure spectrum's validity resets only with a cycle that
    // re-answers it - unlike the interior flag it is read every frame by the
    // ambient blend, so clearing it above the gates would flip the mix to the
    // fallback and back between a standing listener's cycles. Between cycles
    // the last verdict holds, eased.
    mEnclosureValid = false;

    mProbeAnchor = cam;
    mProbeOrigin = cam;
    mLastCycleDone = now;

    // <SS:Nexii> Cover and burial from the shared world field where it has an answer - a handful of band-stack reads standing in for the three tilted raycasts, plus a burial figure that walks the actual span stack rather than one column-top subtraction. The claim handle is what makes the field build here; the raycasts below stay as the fallback for the stretch before a tile exists and for when the switch is off.
    static LLCachedControl<bool> field_coverage(gSavedSettings, "SSWorldFieldCoverage", false);
    bool field_answered = false;
    if (field_coverage)
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(cam);
        if (regionp)
        {
            if (!mCoverageClaim || mCoverageRegion != regionp->getHandle())
            {
                mCoverageRegion = regionp->getHandle();
                mCoverageClaim = SSWorldField::getInstance()->claim(
                    mCoverageRegion, SSWorldField::EChannel::COVERAGE);
            }
        }

        // Five taps - the camera and a short cross around it - so coverage is
        // a fraction rather than one cell's yes/no, the same reason the ray
        // version wiggles its three rays. All five must be covered for
        // mCovered, matching the rays' all-must-hit rule.
        SSWorldField* field = SSWorldField::getInstance();
        static const F32 TAP_REACH = 2.5f;
        static const LLVector3 taps[5] = {
            LLVector3(0.f, 0.f, 0.f),
            LLVector3(TAP_REACH, 0.f, 0.f), LLVector3(-TAP_REACH, 0.f, 0.f),
            LLVector3(0.f, TAP_REACH, 0.f), LLVector3(0.f, -TAP_REACH, 0.f)
        };

        bool centre_covered = false;
        F32 centre_ceiling = 0.f, centre_top = 0.f;
        S32 answered = 0, covered_count = 0;
        for (S32 i = 0; i < 5; ++i)
        {
            bool covered = false;
            F32 ceiling_z = 0.f, top_z = 0.f;
            if (!field->coverageDetail(cam + taps[i], covered, ceiling_z, top_z)) continue;

            ++answered;
            if (covered) ++covered_count;
            if (i == 0)
            {
                centre_covered = covered;
                centre_ceiling = ceiling_z;
                centre_top = top_z;
            }
        }

        if (answered == 5)
        {
            field_answered = true;
            mCoverage = (F32)covered_count / 5.f;
            mCovered = (covered_count == 5) && centre_covered;
            mRoofDist = mCovered ? llclamp(centre_ceiling - cam.mV[VZ], 0.f, UP_RAY_LENGTH) : 0.f;
            mBuriedDepth = mCovered ? llmax(0.f, centre_top - centre_ceiling) : 0.f;

            // Sealed-room verdict from the air flood: a camera cell the
            // connectivity walk could not reach from sky or tile border is
            // inside a volume the store considers sealed, and that matters to
            // the mix even where the coverage taps found no roof to measure -
            // a mezzanine air band under a deck roof draws no ceiling the band
            // stack can name, but the flood still proves it cannot leave.
            mInterior = (field->airLabelAt(cam) == SSWorldField::AIR_INTERIOR);
            if (mInterior)
            {
                mBuriedDepth = llmax(mBuriedDepth, BURIAL_INTERIOR_DEPTH);
            }

            // The enclosure spectrum: 0 outdoors, 1 sealed interior, ramped
            // on the flood's depth back to open sky. -1 while the labels are
            // still flooding (or the point sits inside a band's implied
            // solid) keeps this cycle on the raycast mix.
            const F32 enc = field->enclosureAt(cam);
            if (enc >= 0.f)
            {
                mEnclosure = enc;
                mEnclosureValid = true;
            }
        }
    }
    else if (mCoverageClaim)
    {
        mCoverageClaim = SSWorldField::Interest();
        mCoverageRegion = 0;
    }

    // <SS:Nexii> The ACOUSTIC stake: the channel that bakes the gap-anchored probes
    // and the propagation graph. Held for the camera's region while its switch is
    // on, independently of the coverage switch - the wall profile and the
    // classification want the bake even where cover is still answered by rays.
    static LLCachedControl<bool> field_acoustics(gSavedSettings, "SSWorldFieldAcoustics", true);
    if ((bool)field_acoustics)
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(cam);
        if (regionp)
        {
            if (!mAcousticClaim || mAcousticRegion != regionp->getHandle())
            {
                mAcousticRegion = regionp->getHandle();
                mAcousticClaim = SSWorldField::getInstance()->claim(
                    mAcousticRegion, SSWorldField::EChannel::ACOUSTIC);
            }
        }
    }
    else if (mAcousticClaim)
    {
        mAcousticClaim = SSWorldField::Interest();
        mAcousticRegion = 0;
    }

    if (!field_answered)
    {
    S32 up_hits = 0;
    F32 roof_dist = UP_RAY_LENGTH;
    for (S32 i = 0; i < UP_RAY_COUNT; ++i)
    {
        F32 hit_dist = UP_RAY_LENGTH;
        if (castUpProbe(i, hit_dist))
        {
            ++up_hits;
            roof_dist = llmin(roof_dist, hit_dist);
        }
    }

    mCoverage = (F32)up_hits / (F32)UP_RAY_COUNT;
    mCovered = (up_hits == UP_RAY_COUNT);
    mRoofDist = mCovered ? roof_dist : 0.f;

    mBuriedDepth = 0.f;
    if (mCovered)
    {
        F32 column_top = 0.f;
        if (SSWindFlowMap::getInstance()->surfaceAt(cam, column_top))
        {
            const F32 ceiling = cam.mV[VZ] + roof_dist;
            mBuriedDepth = llmax(0.f, column_top - ceiling);
        }
    }
    }

    // <SS:Nexii> The listener blend from the probe bake where it stands (the doc's
    // Part 3): the connected probes' blended wall profile, room class and size,
    // inverse-distance weighted over the listener's own gap plus graph-adjacent
    // probes - never a raw trilinear tap over the lattice, because the nearest probe
    // through a wall is exactly the one that must not contribute. This replaces the
    // classification the cycle used to run its own math for; the lattice read and
    // the four raycasts below are the fallbacks, in that order.
    bool probe_answered = false;
    if ((bool)field_acoustics && SSWorldField::instanceExists())
    {
        SSWorldField::ProbeSample samples[4];
        S32 ns = 0;
        if (SSWorldField::getInstance()->probesAt(cam, samples, ns) && ns > 0)
        {
            F32 wsum = 0.f;
            F32 wall[4] = { 0.f, 0.f, 0.f, 0.f };
            F32 space_w[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
            F32 size_w[3] = { 0.f, 0.f, 0.f };
            for (S32 i = 0; i < ns; ++i)
            {
                const F32 w = llmax(samples[i].mWeight, 0.f);
                wsum += w;
                wall[0] += w * samples[i].mWall[0];
                wall[1] += w * samples[i].mWall[2];
                wall[2] += w * samples[i].mWall[4];
                wall[3] += w * samples[i].mWall[6];
                space_w[llclamp(samples[i].mSpaceClass, 0, 4)] += w;
                size_w[llclamp(samples[i].mSizeClass, 0, 2)] += w;
            }
            if (wsum > 0.f)
            {
                probe_answered = true;
                const F32 inv = 1.f / wsum;
                for (S32 i = 0; i < 4; ++i) mSideDist[i] = wall[i] * inv;

                S32 walls = 0;
                F32 sum = 0.f;
                for (S32 i = 0; i < 4; ++i)
                {
                    if (mSideDist[i] < SIDE_RAY_LENGTH - 0.5f) ++walls;
                    sum += mSideDist[i];
                }
                mWallCount = walls;
                mWallAvg = sum * 0.25f;

                S32 space = 0;
                for (S32 i = 1; i < 5; ++i) if (space_w[i] > space_w[space]) space = i;
                S32 size = 0;
                for (S32 i = 1; i < 3; ++i) if (size_w[i] > size_w[size]) size = i;
                mSpace = (ESpace)space;
                mOutdoorSize = (ESize)size;
            }
        }
    }

    if (!probe_answered)
    {
    // The wall profile: the probe bake's nearest probe where the channel stands -
    // one read standing in for four live raycasts per cycle, walking the flood's
    // solid map on the worker instead of the render pipeline - else the four
    // raycasts. The classification math below is shared; a stale bake (edited tile,
    // flood pending) falls back automatically.
    F32 lattice_walls[4];
    if (field_answered && SSWorldField::getInstance()->acousticAt(cam, lattice_walls))
    {
        for (S32 i = 0; i < 4; ++i) mSideDist[i] = lattice_walls[i];
    }
    else
    {
        for (S32 i = 0; i < 4; ++i)
        {
            mSideDist[i] = castSideProbe(i);
        }
    }

    S32 walls = 0;
    F32 sum = 0.f;
    for (S32 i = 0; i < 4; ++i)
    {
        if (mSideDist[i] < SIDE_RAY_LENGTH - 0.5f) ++walls;
        sum += mSideDist[i];
    }
    const F32 avg = sum * 0.25f;
    mWallCount = walls;
    mWallAvg = avg;

    mOutdoorSize = (avg < SMALL_SPACE_AVG)  ? SIZE_SMALL
                 : (avg < MEDIUM_SPACE_AVG) ? SIZE_MEDIUM : SIZE_LARGE;

    if (!mCovered)
    {
        mSpace = SPACE_OUTDOOR;
    }
    else if (walls <= 1)
    {
        mSpace = SPACE_SHELTERED;
    }
    else if (walls >= 3 && avg < SMALL_SPACE_AVG)
    {
        mSpace = SPACE_SMALL;
    }
    else if (avg < MEDIUM_SPACE_AVG)
    {
        mSpace = SPACE_MEDIUM;
    }
    else
    {
        mSpace = SPACE_BIG;
    }
    }
}

// How buried under geometry the listener is, 0..1.
F32 SSSoundscape::burialOcclusion() const
{
    const F32 t = llclamp(mBuriedSmooth / BURIAL_FULL, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// The eased enclosure spectrum, or -1 when the field has no current verdict.
F32 SSSoundscape::enclosure() const
{
    return mEnclosureValid ? mEnclosureSmooth : -1.f;
}

// Debug label for a cover space.
const char* SSSoundscape::spaceName(ESpace space)
{
    switch (space)
    {
        case SPACE_SHELTERED: return "sheltered (roof, open sides)";
        case SPACE_SMALL:     return "small room";
        case SPACE_MEDIUM:    return "medium room";
        case SPACE_BIG:       return "big hall";
        default:              return "outdoors";
    }
}

// Debug label for a room size.
const char* SSSoundscape::sizeName(ESize size)
{
    switch (size)
    {
        case SIZE_SMALL:  return "tight";
        case SIZE_MEDIUM: return "medium";
        default:          return "open";
    }
}

// Interpolated wall distance in a horizontal direction, from the side probes.
F32 SSSoundscape::wallDistanceToward(const LLVector3& dir_horizontal) const
{
    static const LLVector3 cardinals[4] = {
        LLVector3(1.f, 0.f, 0.f), LLVector3(-1.f, 0.f, 0.f),
        LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, -1.f, 0.f)
    };

    F32 weighted = 0.f;
    F32 total = 0.f;
    for (S32 i = 0; i < 4; ++i)
    {
        const F32 w = llmax(0.f, dir_horizontal * cardinals[i]);
        weighted += w * mSideDist[i];
        total += w;
    }
    return (total > 0.f) ? (weighted / total) : SIDE_RAY_LENGTH;
}

// Gain multiplier for an outdoor source heard from the listener's cover.
F32 SSSoundscape::occlusionGain(const LLVector3& source_pos) const
{
    LLVector3 to_source = source_pos - mProbeOrigin;
    to_source.mV[VZ] = 0.f;
    const F32 dist = to_source.normVec();
    if (dist < 1.f) return 1.f;

    // <SS:Nexii> The real trace where the store answers (the doc's Part 3 replaces
    // the heuristic): transmission through every solid metre the direct line
    // crosses - the 2D DDA over the columns, exact against the span store, no scene
    // raycast - floored by the diffracted path's energy when the probe graph
    // answers. Sound through a wall or around it, whichever survives better.
    if (SSWorldField::instanceExists())
    {
        SSWorldField* field = SSWorldField::getInstance();
        F32 solid_m = 0.f;
        S32 crossings = 0;
        if (field->traceSolid(mProbeOrigin, source_pos, solid_m, crossings))
        {
            F32 gain = exp2f(-solid_m / OCCLUSION_HALF_LOSS_M);
            SSWorldField::Propagation prop;
            if (field->propagationQuery(source_pos, mProbeOrigin, prop)
                && prop.mDirectM > 1.f && prop.mPathM > 1.f)
            {
                // Diffracted amplitude scales ~ direct/path; energy squares it.
                const F32 diffracted = prop.mDirectM / prop.mPathM;
                gain = llmax(gain, diffracted * diffracted);
            }
            return llclamp(gain, 0.02f, 1.f);
        }
    }

    // No tile, or the segment left it: the old cover heuristic, unchanged - the
    // migration rule's "every intermediate state degrades to today's behaviour".
    const F32 wall = wallDistanceToward(to_source);
    if (dist <= wall + 0.5f) return 1.f;

    // The spectrum deepens the attenuation beyond what cover alone says: a
    // source beyond the walls of a space the flood ranks deeply enclosed
    // loses more than one under an eave, at whatever depth the walk measured.
    const F32 close = llmax(mCoverSmooth, mEnclosureValid ? mEnclosureSmooth : 0.f);
    return lerp(0.6f, 0.22f, close);
}

// Smoothed impacts per second around the camera.
F32 SSSoundscape::impactRate() const
{
    return mImpactRate / IMPACT_RATE_TAU;
}

// How many loops are audible, for status UI.
S32 SSSoundscape::activeLoops() const
{
    S32 count = 0;
    for (const Loop& loop : mLoops)
    {
        if (loop.mSourceID.notNull() && loop.mGain > 0.005f) ++count;
    }
    return count;
}

// Seconds since a probe ran, for status UI.
F64 SSSoundscape::lastProbeAge() const
{
    return SSAtmoMagic::getInstance()->sharedTime() - mLastCycleDone;
}

// Drives one ambient loop at a target gain: picks the sound, starts, crossfades, retires.
void SSSoundscape::applyLoop(Loop& loop, const std::string& configured, F32 master, F32 dt)
{
    if (configured != loop.mConfigured)
    {
        releaseLoop(loop);
        loop.mConfigured = configured;
        parseSoundList(configured, loop.mSounds);
        loop.mIndex = 0;
    }

    if (loop.mSounds.empty() || !gAudiop)
    {
        loop.mTarget = 0.f;
        return;
    }

    static LLCachedControl<F32> fade_rate(gSavedSettings, "SSAtmoSoundResponse", 3.f);
    loop.mGain = lerp(loop.mGain, loop.mTarget, llclamp(llmax(0.1f, (F32)fade_rate) * dt, 0.f, 1.f));
    const F32 gain = llclamp(loop.mGain * master, 0.f, 1.f);

    LLAudioSource* source = loop.mSourceID.notNull() ? gAudiop->findAudioSource(loop.mSourceID) : nullptr;

    if (gain < 0.005f)
    {
        if (loop.mTarget < 0.005f && loop.mSourceID.notNull())
        {
            if (source)
            {
                gAudiop->cleanupAudioSource(source);
            }
            loop.mSourceID.setNull();
        }
        else if (source)
        {
            source->setGain(0.f);
        }
        return;
    }

    if (!source)
    {
        const bool sequence = loop.mSounds.size() > 1;
        if (loop.mSourceID.notNull() && sequence)
        {
            loop.mIndex = (loop.mIndex + 1) % (U32)loop.mSounds.size();
        }
        loop.mIndex %= (U32)loop.mSounds.size();

        loop.mSourceID.generate();
        source = new LLAudioSource(loop.mSourceID, gAgent.getID(), gain,
                                   LLAudioEngine::AUDIO_TYPE_AMBIENT);
        source->setLoop(!sequence);
        source->setForcedPriority(true);
        gAudiop->addAudioSource(source);
        source->play(loop.mSounds[loop.mIndex]);

        if (sequence)
        {
            SSSoundMeta::getInstance()->fetch(loop.mSounds[(loop.mIndex + 1) % (U32)loop.mSounds.size()]);
        }
    }

    source->setGain(gain);
    source->setPositionGlobal(gAgent.getPosGlobalFromAgent(
        LLViewerCamera::getInstance()->getOrigin() + loop.mOffset));
}

// Mixes the whole ambient set - rain ambiences by impact rate, wind by speed, roof ambiences by cover - and applies each loop.
void SSSoundscape::updateLoops(F64 now, F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    static LLCachedControl<F32> master_setting(gSavedSettings, "SSAtmoVolumeMaster", 0.8f);
    static LLCachedControl<F32> ambient_setting(gSavedSettings, "SSAtmoVolumeAmbient", 1.f);
    static LLCachedControl<F32> wind_setting(gSavedSettings, "SSAtmoVolumeWind", 1.f);
    const F32 master = llclamp((F32)master_setting, 0.f, 1.f);
    const F32 ambient_vol = llclamp((F32)ambient_setting, 0.f, 1.f);
    const F32 wind_vol = llclamp((F32)wind_setting, 0.f, 1.f);

    const F32 category[LOOP_COUNT] = {
        ambient_vol, ambient_vol, ambient_vol,
        ambient_vol, ambient_vol, ambient_vol, ambient_vol,
        wind_vol, wind_vol,
    };

    mCoverSmooth = lerp(mCoverSmooth, mCovered ? 1.f : 0.f, llclamp(COVER_BLEND_RATE * dt, 0.f, 1.f));
    mEnclosureSmooth = lerp(mEnclosureSmooth, mEnclosure, llclamp(COVER_BLEND_RATE * dt, 0.f, 1.f));
    mBuriedSmooth = lerp(mBuriedSmooth, mBuriedDepth, llclamp(BURIAL_BLEND_RATE * dt, 0.f, 1.f));

    // <SS:Nexii> The environment is the soundscape's source: with none resolved, stand down on the loops' own crossfade. Gating on mEnabled alone rode the weather blend's ~10 s fade-out, where no-env precipitation/turbulence defaults and the stale preset kept wet and wind targets sounding on a parcel with no environment; isSwitchedOn is the same immediate gate footsteps use.
    if (!atmo->isSwitchedOn())
    {
        mLadderTargets.clear();
        for (S32 i = 0; i < LOOP_COUNT; ++i)
        {
            mLoops[i].mTarget = 0.f;
            applyLoop(mLoops[i], mLoops[i].mConfigured, master * category[i], dt);
        }
        updateAmbientVoices(now, dt, master * ambient_vol);
        return;
    }

    const F32 env = llclamp(atmo->gustEnvelopeAt(now), 0.f, 2.5f);
    const SSPrecipPreset& preset = atmo->preset();
    const F32 param_wet = atmo->hasWeather() ? atmo->precipitation() * (0.4f + 0.3f * env) : 0.f;
    const F32 impact_wet = llclamp(mImpactRate / IMPACT_RATE_FULL, 0.f, 1.f);
    const F32 wet = llclamp(0.55f * param_wet + 0.45f * impact_wet, 0.f, 1.f);

    mWetSlow = lerp(mWetSlow, wet, llclamp(dt / 10.f, 0.f, 1.f));

    const LLVector3 cam_pos = LLViewerCamera::getInstance()->getOrigin();
    SSWindFlowMap* flow = SSWindFlowMap::getInstance();
    const F32 local_speed = flow->isValid() ? flow->sample(cam_pos).magVec()
                                            : atmo->windSpeed();

    const F32 wind = llclamp(local_speed / 14.f, 0.f, 1.f)
                   * (0.55f + 0.45f * llclamp(env, 0.f, 2.f) * 0.5f)
                   * (0.6f + 0.4f * atmo->turbulence());

    const bool sheltered = (mSpace == SPACE_SHELTERED);

    // The enclosure spectrum drives the ambient blend where the field answered:
    // the old sheltered/room duck pair became the two ends of a continuous
    // ramp, so a cave mouth, an arcade and a warehouse interior each sit
    // between them at their own measured depth. Without the field, the probe
    // verdict's own rungs stand in - the same two values the discrete mix
    // used, so the raycast fallback is unchanged.
    const F32 enc = mEnclosureValid ? mEnclosureSmooth : (sheltered ? 0.f : 1.f);

    const F32 buried = burialOcclusion();
    const F32 outdoor = (1.f - lerp(0.4f, 0.85f, enc) * mCoverSmooth)
                      * (1.f - BURIAL_MAX_DUCK * buried);

    F32 w_light = tri(wet, 0.01f, 0.18f, 0.55f);
    F32 w_med   = tri(wet, 0.15f, 0.5f, 0.9f);
    F32 w_heavy = llclamp((wet - 0.55f) / 0.3f, 0.f, 1.f);
    if (preset.mSounds.mAmbientLight.empty()) { w_med += w_light; w_light = 0.f; }
    if (preset.mSounds.mAmbientHeavy.empty()) { w_med += w_heavy; w_heavy = 0.f; }
    w_med = llmin(w_med, 1.f);

    F32 targets[LOOP_COUNT] = { 0.f };
    targets[LOOP_AMBIENT_LIGHT]  = w_light * outdoor;
    targets[LOOP_AMBIENT_MEDIUM] = w_med * outdoor;
    targets[LOOP_AMBIENT_HEAVY]  = w_heavy * outdoor;

    mLadderTargets.clear();
    {
        static LLCachedControl<bool> auto_sort(gSavedSettings, "SSAtmoAmbientAutoSort", true);
        if (auto_sort)
        {
            std::vector<std::pair<F32, LLUUID>> rungs;
            for (const std::string* csv : { &preset.mSounds.mAmbientLight, &preset.mSounds.mAmbientMedium, &preset.mSounds.mAmbientHeavy })
            {
                std::vector<std::string> tokens;
                LLStringUtil::getTokens(*csv, tokens, ",");
                for (const std::string& tok : tokens)
                {
                    LLUUID id(tok);
                    if (id.isNull()) continue;
                    if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id))
                    {
                        rungs.emplace_back(meta->mDensity, id);
                    }
                }
            }
            if (rungs.size() >= 3)
            {
                std::sort(rungs.begin(), rungs.end());
                if (rungs.back().first - rungs.front().first >= 0.08f)
                {
                    const F32 pos = llclamp(wet, 0.f, 1.f) * (F32)(rungs.size() - 1);
                    const size_t lo = (size_t)llmin((F32)(rungs.size() - 1), pos);
                    const size_t hi = llmin(lo + 1, rungs.size() - 1);
                    const F32 frac = llclamp(pos - (F32)lo, 0.f, 1.f);

                    const F32 amb = llclamp(wet / 0.3f, 0.f, 1.f) * outdoor;
                    mLadderTargets.emplace_back(rungs[lo].second, amb * cosf(frac * F_PI_BY_TWO));
                    if (hi != lo) mLadderTargets.emplace_back(rungs[hi].second, amb * sinf(frac * F_PI_BY_TWO));

                    targets[LOOP_AMBIENT_LIGHT]  = 0.f;
                    targets[LOOP_AMBIENT_MEDIUM] = 0.f;
                    targets[LOOP_AMBIENT_HEAVY]  = 0.f;
                }
            }
        }
    }

    const F32 roof = mCoverSmooth * wet * (1.f - BURIAL_MAX_DUCK * buried);
    // The roof ambience's open-cover share: an eave or canopy sits at the outdoors
    // end of the spectrum and takes the open recording, the room-character
    // ambiences take what it leaves. Without the field, the probe verdict's own
    // sheltered rung stands in - the exact discrete split this generalises.
    const F32 open_w = mEnclosureValid ? llclamp(1.f - enc / 0.4f, 0.f, 1.f)
                                       : (sheltered ? 1.f : 0.f);
    targets[LOOP_ROOF_OPEN] = roof * open_w;
    // A sheltered space reads its own wall-distance size for the room ambiences,
    // so a deep tunnel lands on the big-hall ambience instead of dropping out.
    const bool small_room  = (mSpace == SPACE_SMALL)
                          || (mSpace == SPACE_SHELTERED && mOutdoorSize == SIZE_SMALL);
    const bool medium_room = (mSpace == SPACE_MEDIUM)
                          || (mSpace == SPACE_SHELTERED && mOutdoorSize == SIZE_MEDIUM);
    const bool big_room    = (mSpace == SPACE_BIG && mCovered)
                          || (mSpace == SPACE_SHELTERED && mOutdoorSize == SIZE_LARGE);
    targets[LOOP_ROOF_SMALL]  = small_room  ? roof * (1.f - open_w) : 0.f;
    targets[LOOP_ROOF_MEDIUM] = medium_room ? roof * (1.f - open_w) : 0.f;
    targets[LOOP_ROOF_BIG]    = big_room    ? roof * (1.f - open_w) : 0.f;

    const F32 probe_openness = (mOutdoorSize == SIZE_SMALL)  ? 0.55f
                             : (mOutdoorSize == SIZE_MEDIUM) ? 0.8f : 1.f;
    const F32 outdoor_openness = flow->isValid()
        ? llclamp(flow->exposure(cam_pos), 0.f, 1.5f)
        : probe_openness;
    const F32 wind_indoor = (1.f - lerp(0.3f, 0.75f, enc) * mCoverSmooth)
                          * lerp(outdoor_openness, 1.f, mCoverSmooth);
    targets[LOOP_WIND_LIGHT]  = tri(wind, 0.02f, 0.3f, 0.75f) * wind_indoor;
    targets[LOOP_WIND_STRONG] = llclamp((wind - 0.45f) / 0.4f, 0.f, 1.f) * wind_indoor;

    if (mLastCycleDone <= 0.0)
    {
        for (S32 i = 0; i < LOOP_COUNT; ++i) targets[i] = 0.f;
        mLadderTargets.clear();
    }

    const std::string sources[LOOP_COUNT] = {
        preset.mSounds.mAmbientLight,
        preset.mSounds.mAmbientMedium,
        preset.mSounds.mAmbientHeavy,
        preset.mSounds.mRoofOpen,
        preset.mSounds.mRoofSmall,
        preset.mSounds.mRoofMedium,
        preset.mSounds.mRoofBig,
        SSAtmoStore::getString(SSAtmoStoreKey::WIND_LIGHT),
        SSAtmoStore::getString(SSAtmoStoreKey::WIND_STRONG),
    };

    {
        LLVector3 local = flow->isValid() ? flow->sample(cam_pos)
                                          : atmo->windXY();
        const F32 speed = local.normalize();
        const LLVector3 upwind = (speed > 0.4f) ? local * -6.f : LLVector3::zero;
        mLoops[LOOP_WIND_LIGHT].mOffset = upwind;
        mLoops[LOOP_WIND_STRONG].mOffset = upwind;
    }

    for (S32 i = 0; i < LOOP_COUNT; ++i)
    {
        mLoops[i].mTarget = llclamp(targets[i], 0.f, 1.f);
        applyLoop(mLoops[i], sources[i], master * category[i], dt);
    }

    updateAmbientVoices(now, dt, master * ambient_vol);
}

// Per-ambience voice management: which recording each ambience plays and at what level, levelled by the analysed metadata.
void SSSoundscape::updateAmbientVoices(F64 now, F32 dt, F32 master_mul)
{
    if (!gAudiop) return;

    for (auto& pair : mAmbientVoices) pair.second.mTarget = 0.f;
    for (const auto& want : mLadderTargets)
    {
        mAmbientVoices[want.first].mTarget = llclamp(want.second, 0.f, 1.f);
    }

    const F32 fade = llclamp(dt * 1.5f, 0.f, 1.f);

    for (auto it = mAmbientVoices.begin(); it != mAmbientVoices.end(); )
    {
        AmbientVoice& voice = it->second;
        voice.mGain = lerp(voice.mGain, voice.mTarget, fade);

        LLAudioSource* source = voice.mSourceID.notNull() ? gAudiop->findAudioSource(voice.mSourceID) : nullptr;

        if (voice.mTarget <= 0.001f && voice.mGain <= 0.005f)
        {
            if (source)
            {
                U32 len = 0;
                if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(it->first)) len = meta->mLengthMS;
                if (len > 0)
                {
                    mAmbientResume[it->first] = (U32)((voice.mOffsetMS + (U64)((now - voice.mStartedAt) * 1000.0)) % len);
                }
                gAudiop->cleanupAudioSource(source);
            }
            it = mAmbientVoices.erase(it);
            continue;
        }

        if (!source && voice.mTarget > 0.001f)
        {
            voice.mSourceID.generate();
            voice.mStartedAt = now;
            auto resume = mAmbientResume.find(it->first);
            voice.mOffsetMS = (resume != mAmbientResume.end()) ? resume->second : 0;

            source = new LLAudioSource(voice.mSourceID, gAgent.getID(), 0.f, LLAudioEngine::AUDIO_TYPE_AMBIENT);
            source->setStartOffsetMS(voice.mOffsetMS);
            source->setLoop(true);
            source->setForcedPriority(true);
            source->setPositionGlobal(gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()));
            gAudiop->addAudioSource(source);
            source->play(it->first);
        }

        if (source)
        {
            source->setGain(llclamp(voice.mGain * master_mul, 0.f, 1.f));
            source->setPositionGlobal(gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()));
        }
        ++it;
    }
}

namespace
{
    // The configurable speed of sound - thunder delay's one physical constant.
    F32 speed_of_sound_ms()
    {
        return 331.3f + 0.606f * SSAtmoMagic::getInstance()->temperatureC();
    }

    const F32 THUNDER_CRACK_M = 1500.f;
    const F32 THUNDER_RUMBLE_M = 6000.f;

    // Rolls a sound from a comma-separated UUID list.
    LLUUID pick_from_csv(const std::string& csv, SSRandStream& rng)
    {
        if (csv.empty()) return LLUUID::null;

        std::vector<std::string> tokens;
        LLStringUtil::getTokens(csv, tokens, ",");

        std::vector<LLUUID> ids;
        for (const std::string& tok : tokens)
        {
            LLUUID id(tok);
            if (id.notNull()) ids.push_back(id);
        }
        if (ids.empty()) return LLUUID::null;
        return ids[rng.rand((S32)ids.size())];
    }

    // A recording's analysed onset, 0 when unanalysed.
    U32 sound_onset_ms(const LLUUID& id)
    {
        if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id))
        {
            return meta->mOnsetMS;
        }

        if (id.isNull() || !gAudiop) return 0;

        LLAudioData* data = gAudiop->getAudioData(id);
        if (!data || !data->hasDecodedData()) return 0;

        LLAudioBuffer* buffer = data->getBuffer();
        if (!buffer)
        {
            gAudiop->updateBufferForData(data, id);
            buffer = data->getBuffer();
        }
        return buffer ? buffer->getOnsetMS() : 0;
    }
}

// How much sky the listener has, from cover.
F32 SSSoundscape::skyOcclusion() const
{
    return llclamp(mCoverSmooth * 0.55f + burialOcclusion() * 0.45f, 0.f, 1.f);
}

// Fire-and-forget positioned sound; returns the source id.
static LLUUID ss_play_oneshot(const LLUUID& sound, const LLVector3d& pos_global, F32 gain, F32 occlusion)
{
    if (!gAudiop || sound.isNull()) return LLUUID::null;

    const LLUUID id = LLUUID::generateNewID();
    LLAudioSource* source = new LLAudioSource(id, gAgent.getID(),
                                              llclamp(gain, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_AMBIENT);
    source->setPositionGlobal(pos_global);
    source->setOcclusion(occlusion);
    gAudiop->addAudioSource(source);
    source->play(sound);
    return id;
}

// Registers a playing source to keep a fixed bearing from the moving listener.
void SSSoundscape::registerFollower(const LLUUID& source_id, const LLVector3& dir_world, F64 now)
{
    if (source_id.isNull()) return;
    OneShotFollower f;
    f.mSourceID = source_id;
    f.mDir = dir_world;
    f.mLastPos = LLViewerCamera::getInstance()->getOrigin() + dir_world * 12.f;
    f.mExpires = now + 120.0;
    mFollowers.push_back(f);
}

// Re-aims follower sources as the listener moves.
void SSSoundscape::updateFollowers(F64 now, F32 dt)
{
    if (!gAudiop) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    for (size_t i = 0; i < mFollowers.size(); )
    {
        OneShotFollower& f = mFollowers[i];
        LLAudioSource* source = gAudiop->findAudioSource(f.mSourceID);
        if (!source || now > f.mExpires)
        {
            mFollowers.erase(mFollowers.begin() + i);
            continue;
        }

        const LLVector3 pos = cam + f.mDir * 12.f;
        source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos));
        source->setVelocity((dt > 0.001f) ? (pos - f.mLastPos) / dt : LLVector3::zero);
        f.mLastPos = pos;
        ++i;
    }
}

// The pre-strike charge crackle at the strike point.
void SSSoundscape::playCharge(const LLVector3& pos_agent, F32 intensity)
{
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!sounds || !gAudiop) return;

    SSRandStream rng((U32)(SSAtmoMagic::getInstance()->sharedTime() * 8171.0));
    const LLUUID sound = pick_from_csv(gSavedSettings.getString("SSAtmoLightningCharge"), rng);
    if (sound.isNull()) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 dir = pos_agent - cam;
    const F32 d = dir.normalize();
    const LLVector3 near_pos = cam + dir * llmin(d, 12.f);
    registerFollower(ss_play_oneshot(sound, gAgent.getPosGlobalFromAgent(near_pos),
                                     llclamp(intensity, 0.f, 1.f), skyOcclusion()),
                     dir, SSAtmoMagic::getInstance()->sharedTime());
}

// Upwind sources carry, downwind ones lose - gain from the wind ray.
F32 SSSoundscape::windCarryGain(const LLVector3& source_pos_agent) const
{
    const LLVector3 listener = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 to_listener = listener - source_pos_agent;
    const F32 dist = to_listener.normalize();
    if (dist < 50.f) return 1.f;

    LLVector3 wind = SSAtmoMagic::getInstance()->wind();
    const F32 speed = wind.normalize();
    if (speed < 0.5f) return 1.f;

    const F32 along = wind * to_listener;

    const F32 range = llmin(dist / 3000.f, 1.f);
    const F32 strength = llmin(speed / 12.f, 1.f);
    return llclamp(1.f + along * range * strength * 0.8f, 0.25f, 1.6f);
}

// Rolls a recording from the crack or rumble list.
static LLUUID pick_thunder(bool want_rumble, SSRandStream& rng)
{
    static LLCachedControl<bool> auto_sort(gSavedSettings, "SSAtmoThunderAutoSort", true);
    const std::string& home = want_rumble ? SSAtmoStoreKey::THUNDER_RUMBLE
                                          : SSAtmoStoreKey::THUNDER_CRACK;
    if (!auto_sort) return pick_from_csv(SSAtmoStore::getString(home), rng);

    std::vector<std::pair<F32, LLUUID>> rated;
    for (const std::string& key : { SSAtmoStoreKey::THUNDER_CRACK, SSAtmoStoreKey::THUNDER_RUMBLE })
    {
        std::vector<std::string> tokens;
        LLStringUtil::getTokens(SSAtmoStore::getString(key), tokens, ",");
        for (const std::string& tok : tokens)
        {
            LLUUID id(tok);
            if (id.isNull()) continue;
            if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id))
            {
                rated.emplace_back(meta->mCrackiness, id);
            }
        }
    }

    if (rated.size() < 4) return pick_from_csv(SSAtmoStore::getString(home), rng);

    std::sort(rated.begin(), rated.end());
    if (rated.back().first - rated.front().first < 0.08f) return pick_from_csv(SSAtmoStore::getString(home), rng);

    const size_t half = rated.size() / 2;
    const size_t lo = want_rumble ? 0 : rated.size() - half;
    return rated[lo + (size_t)rng.rand((S32)half)].second;
}

// Books thunder for a future strike: travel delay, crack/rumble choice, muffle - aligned so the recording's ONSET lands on time.
void SSSoundscape::scheduleThunder(const LLVector3& pos_agent, F32 distance_m,
                                   F32 intensity, F64 fire_at, F32 muffle)
{
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!sounds || !gAudiop) return;
    muffle = llclamp(muffle, 0.f, 1.f);

    // <SS:Nexii> The ACOUSTIC channel's propagation query turns the storm system's
    // muffle guess into three outputs (the doc's Part 3): the travel time walks the
    // graph's PATH metres - around buildings, through alleys, into courtyards -
    // instead of the euclidean distance, the portal count and the path-vs-direct
    // ratio become a muffle share of their own, and the arrival direction renders
    // the sound from the doorway it actually came through. The cloud-burial guess
    // stays in the mix: intra-cloud lightning is a rumble however close it is.
    ThunderPath& dbg = mThunderPath;
    dbg = ThunderPath();
    dbg.mValid = true;
    dbg.mSource = pos_agent;
    dbg.mDirectM = distance_m;
    dbg.mMuffleCloud = muffle;
    dbg.mWhen = SSAtmoMagic::getInstance()->sharedTime();

    F32 travel_m = distance_m;
    F32 field_muffle = 0.f;
    bool has_arrival = false;
    LLVector3 arrival_dir(0.f, 0.f, 1.f);
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (SSWorldField::instanceExists())
    {
        SSWorldField* field = SSWorldField::getInstance();
        SSWorldField::Propagation prop;
        if (field->propagationQuery(pos_agent, cam, prop))
        {
            dbg.mPropagated = true;
            dbg.mPathM = prop.mPathM;
            dbg.mPortals = prop.mPortals;
            dbg.mMuffleField = prop.mMuffle;
            dbg.mPath = prop.mPath;
            travel_m = llmax(prop.mPathM, distance_m);
            field_muffle = prop.mMuffle;
            has_arrival = prop.mHaveArrival;
            arrival_dir = prop.mArrivalDir;
        }

        F32 solid_m = -1.f;
        S32 crossings = 0;
        if (field->traceSolid(pos_agent, cam, solid_m, crossings))
        {
            dbg.mSolidM = solid_m;
            dbg.mCrossings = crossings;
        }
    }
    dbg.mDelayS = (F32)llmax(0.0, (F64)(travel_m - distance_m) / (F64)speed_of_sound_ms());

    const F32 total_muffle = llclamp(1.f - (1.f - muffle) * (1.f - field_muffle), 0.f, 1.f);

    SSRandStream rng((U32)(fire_at * 6151.0) ^ (U32)distance_m);

    const F64 travel = (F64)(travel_m / speed_of_sound_ms());
    const F64 heard_at = fire_at + travel;

    const F32 crack_gain = (1.f - llclamp(
        (distance_m - THUNDER_CRACK_M) / (THUNDER_RUMBLE_M - THUNDER_CRACK_M), 0.f, 1.f))
        * (1.f - total_muffle);

    const F32 fade = 1.f / (1.f + (travel_m / 3000.f));
    const F32 gain = llclamp(intensity * fade * windCarryGain(pos_agent), 0.f, 1.f)
                   * (1.f - 0.45f * total_muffle);

    if (crack_gain > 0.02f)
    {
        queueThunder(pick_thunder(false, rng),
                     pos_agent, distance_m, gain * crack_gain, heard_at, total_muffle,
                     has_arrival, arrival_dir);
    }

    const F64 spread = (F64)(rng.frand(2000.f, 5000.f) * (0.6f + intensity * 0.7f)
                             / speed_of_sound_ms()) * (F64)(1.f - 0.45f * total_muffle);

    queueThunder(pick_thunder(true, rng),
                 pos_agent, distance_m, gain * (0.5f + 0.5f * (1.f - crack_gain)),
                 heard_at + spread * (F64)(1.f - crack_gain), total_muffle,
                 has_arrival, arrival_dir);
}

// Queues one thunder playback at an absolute time.
void SSSoundscape::queueThunder(const LLUUID& sound, const LLVector3& pos_agent,
                                F32 distance_m, F32 gain, F64 heard_at, F32 muffle,
                                bool has_arrival, const LLVector3& arrival_dir)
{
    if (sound.isNull() || gain <= 0.f) return;

    SSSoundMeta::getInstance()->fetch(sound);

    PendingThunder pending;
    pending.mPos = pos_agent;
    pending.mDistanceM = distance_m;
    pending.mMuffle = muffle;
    pending.mSound = sound;
    pending.mGain = gain;
    pending.mHeardAt = heard_at;
    pending.mPlayAt = heard_at;
    pending.mHasArrival = has_arrival;
    pending.mArrivalDir = arrival_dir;

    mThunder.push_back(pending);
}

// Fires due thunder with distance shaping and cover occlusion.
void SSSoundscape::updateThunder(F64 now)
{
    for (size_t i = 0; i < mThunder.size(); )
    {
        PendingThunder& p = mThunder[i];

        if (!p.mAligned)
        {
            const U32 onset = sound_onset_ms(p.mSound);
            if (onset > 0)
            {
                p.mPlayAt = p.mHeardAt - (F64)onset / 1000.0;
                p.mAligned = true;
            }
            else if (now >= p.mHeardAt - 0.05)
            {
                p.mAligned = true;
            }
        }

        if (now < p.mPlayAt) { ++i; continue; }

        F32 gain = p.mGain;
        {
            const F32 REF_LEVEL = 0.22f;
            F32 level = 0.f;
            if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(p.mSound))
            {
                level = meta->mPeakLevel;
            }
            else if (gAudiop)
            {
                if (LLAudioData* data = gAudiop->getAudioData(p.mSound))
                {
                    if (LLAudioBuffer* buffer = data->getBuffer()) level = buffer->getPeakLevel();
                }
            }
            if (level > 0.001f) gain *= llclamp(REF_LEVEL / level, 0.5f, 2.f);
        }

        static LLCachedControl<F32> thunder_vol(gSavedSettings, "SSAtmoVolumeThunder", 2.5f);
        gain *= llclamp((F32)thunder_vol, 0.f, 4.f);

        {
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
            // <SS:Nexii> The propagation solve's arrival direction wins when it has
            // one: sound entering through a doorway is rendered FROM the doorway.
            LLVector3 dir;
            if (p.mHasArrival)
            {
                dir = p.mArrivalDir;
                dir.normVec();
            }
            else
            {
                dir = p.mPos - cam;
                dir.normVec();
            }
            const LLVector3 near_pos = cam + dir * 12.f;

            const F32 occ = llclamp(skyOcclusion() + p.mMuffle * 0.55f, 0.f, 1.f);
            registerFollower(ss_play_oneshot(p.mSound, gAgent.getPosGlobalFromAgent(near_pos), gain, occ),
                             dir, now);
        }

        mThunder.erase(mThunder.begin() + i);
    }
}

// Cheap cached roof test over an avatar.
bool SSSoundscape::roofOver(const LLUUID& avatar_id, const LLVector3& pos_agent, bool is_self)
{
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    AvatarCover& cover = mAvatarCover[avatar_id];

    const F32 moved = (pos_agent - cover.mPos).magVec();
    const F32 cam_dist = (pos_agent - LLViewerCamera::getInstance()->getOrigin()).magVec();
    const F64 interval = is_self ? 1.0 : 1.0 + (F64)llclamp(cam_dist / 24.f, 0.f, 4.f);

    if (cover.mWhen < 0.0 || (moved > 1.5f && now - cover.mWhen > interval))
    {
        cover.mPos = pos_agent;
        cover.mWhen = now;

        const LLVector3 start = pos_agent + LLVector3(0.f, 0.f, 2.2f);
        const LLVector3 dir(0.12f, 0.09f, 0.99f);
        LLVector4a start4, end4, intersect;
        start4.load3(start.mV);
        const LLVector3 end = start + dir * 50.f;
        end4.load3(end.mV);
        cover.mIndoors = gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true);

        if (mAvatarCover.size() > 64)
        {
            for (auto it = mAvatarCover.begin(); it != mAvatarCover.end(); )
            {
                it = (now - it->second.mWhen > 60.0) ? mAvatarCover.erase(it) : ++it;
            }
        }
    }

    return cover.mIndoors;
}

// Whether the avatar stands on an object rather than terrain, for the footstep surface pick.
bool SSSoundscape::onObject(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self)
{
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    AvatarCover& cover = mAvatarCover[avatar_id];

    const F32 moved = (foot_pos_agent - cover.mFootPos).magVec();
    const F32 cam_dist = (foot_pos_agent - LLViewerCamera::getInstance()->getOrigin()).magVec();
    const F64 interval = is_self ? 0.3 : 0.3 + (F64)llclamp(cam_dist / 24.f, 0.f, 3.f);

    if (cover.mFootWhen < 0.0 || (moved > 0.75f && now - cover.mFootWhen > interval))
    {
        cover.mFootPos = foot_pos_agent;
        cover.mFootWhen = now;

        const LLVector3 start = foot_pos_agent + LLVector3(0.f, 0.f, 0.5f);
        const LLVector3 end = foot_pos_agent - LLVector3(0.f, 0.f, 1.2f);
        LLVector4a start4, end4, intersect;
        start4.load3(start.mV);
        end4.load3(end.mV);

        LLDrawable* hit = gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true);
        LLViewerObject* vo = hit ? hit->getVObj().get() : nullptr;
        cover.mOnObject = vo && vo->getPCode() != LLViewerObject::LL_VO_SURFACE_PATCH;
    }

    return cover.mOnObject;
}

// Fades a source out instead of cutting it.
void SSSoundscape::fadeKill(const LLUUID& source_id)
{
    if (!gAudiop || source_id.isNull()) return;
    if (LLAudioSource* source = gAudiop->findAudioSource(source_id))
    {
        source->setGain(0.f);
        mDying.emplace_back(source_id, SSAtmoMagic::getInstance()->sharedTime() + 0.06);
    }
}

// Advances fading sources and kills the finished.
void SSSoundscape::updateDying(F64 now)
{
    if (!gAudiop) return;
    for (size_t i = 0; i < mDying.size(); )
    {
        if (now >= mDying[i].second)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(mDying[i].first))
            {
                gAudiop->cleanupAudioSource(source);
            }
            mDying.erase(mDying.begin() + i);
        }
        else ++i;
    }
}

// Stops one avatar's footstep loop.
void SSSoundscape::releaseStepLoop(StepLoop& loop)
{
    if (gAudiop && loop.mSourceID.notNull())
    {
        fadeKill(loop.mSourceID);
    }
    loop.mSourceID.setNull();
    loop.mSound.setNull();
    loop.mSurface = -1;
    loop.mAction = -1;
}

// Drops footstep loops for avatars gone quiet or away.
void SSSoundscape::reapStepLoops(F64 now)
{
    for (auto it = mStepLoops.begin(); it != mStepLoops.end(); )
    {
        if (now - it->second.mLastSeen > 0.5)
        {
            releaseStepLoop(it->second);
            it = mStepLoops.erase(it);
        }
        else ++it;
    }
}

// Whether this avatar's steps may sound at all: parcel sound setting, then the per-avatar object-sound mute.
bool SSSoundscape::stepsAudible(const LLUUID& avatar_id, const LLVector3& pos_agent)
{
    // <SS:Nexii> The two gates stock applied to its own trigger-style footsteps, kept for the Atmo paths after the trigger function they lived in was left callerless by the loop rework - muting an avatar silenced its chat and its attachments but not its boots, and a no-sound parcel was ignored outright. The mute is keyed on the WALKER, not the listener, and the parcel is the one under the walker's feet, so a muted neighbour two parcels over stays silent while your own steps follow your own ground. [interaction] called from footstepEvent, updateFootstepLoop and footstepImpact - every live step path there is.
    return LLViewerParcelMgr::getInstance()->canHearSound(gAgent.getPosGlobalFromAgent(pos_agent))
        && !LLMuteList::getInstance()->isMuted(avatar_id, LLMute::flagObjectSounds);
}

// The per-avatar walking loop: surface pick, cadence from the analysed onsets, start and stop with movement.
void SSSoundscape::updateFootstepLoop(const LLUUID& avatar_id, const LLVector3& pos_agent,
                                      bool on_land, S32 locomotion, F32 speed_gain, bool is_self)
{
    if (!gAudiop) return;

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();

    // <SS:Nexii> Going inaudible mid-stride has to STOP the loop, not merely refuse to start it - a mute or a parcel crossing while walking would otherwise leave the running source playing forever. Reporting it as airborne reuses the existing teardown and skips the cadence-gap wait, which is the right cut here: this is not a halt to round off, it is a sound that should not be playing.
    if (!stepsAudible(avatar_id, pos_agent)) locomotion = STEP_JUMP;

    if (locomotion != STEP_WALK && STEP_RUN != locomotion)
    {
        auto it = mStepLoops.find(avatar_id);
        if (it == mStepLoops.end()) return;
        StepLoop& loop = it->second;
        loop.mLastSeen = now;

        // <SS:Nexii> STEP_JUMP is the avatar saying airborne: no cut-point wait, the loop was audibly running on into the jump.
        if (locomotion == STEP_JUMP)
        {
            loop.mStopAt = now;
        }
        else if (loop.mStopAt <= 0.0)
        {
            F64 wait = 0.0;
            const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(loop.mSound);
            if (meta && meta->mOnsets.size() >= 2 && meta->mLengthMS > 0)
            {
                const F64 pos_ms = fmod((F64)loop.mOffsetMS + (now - loop.mStartedAt) * 1000.0, (F64)meta->mLengthMS);
                F64 cut_ms = -1.0;
                for (size_t k = 0; k + 1 < meta->mOnsets.size(); ++k)
                {
                    const F64 gap = (F64)meta->mOnsets[k] + ((F64)meta->mOnsets[k + 1] - (F64)meta->mOnsets[k]) * 0.66;
                    if (gap > pos_ms) { cut_ms = gap; break; }
                }
                if (cut_ms < 0.0) cut_ms = (F64)meta->mTailMS;
                wait = llclamp((cut_ms - pos_ms) / 1000.0, 0.0, 0.4);
            }
            loop.mStopAt = now + wait;
        }

        if (now >= loop.mStopAt)
        {
            releaseStepLoop(loop);
            mStepLoops.erase(it);
        }
        else if (gAudiop)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(loop.mSourceID))
            {
                source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
            }
        }
        return;
    }

    const bool fresh = mStepLoops.find(avatar_id) == mStepLoops.end();
    StepLoop& loop = mStepLoops[avatar_id];
    loop.mLastSeen = now;
    loop.mStopAt = 0.0;
    loop.mSpeedGain = llclamp(speed_gain, 0.f, 1.f);
    if (fresh)
    {
        // Per-walk, so the debug readout counts drops for the walk you are listening to rather than every walk this session.
        StepDebug& dbg = is_self ? mStepSelf : mStepOther;
        dbg.mStepDropped = 0;
        dbg.mStepGap = 0.f;
    }

    LLUUID rolled;
    S32 surface = -1;
    {
        rolled = footstepSound(avatar_id, pos_agent, on_land, locomotion, is_self);
        const StepDebug& dbg = is_self ? mStepSelf : mStepOther;
        surface = dbg.mSurface;
    }

    const bool state_changed = (surface != loop.mSurface) || (locomotion != loop.mAction);
    const LLUUID sound = (state_changed || loop.mSound.isNull()) ? rolled : loop.mSound;

    if (!sound.isNull())
    {
        if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(sound))
        {
            const bool segmentable = meta->mOnsets.size() >= 4
                && meta->mGapFloor < 0.12f
                && meta->mCadenceCV < 0.4f
                && meta->mImpactRate > 0.8f && meta->mImpactRate < 4.5f;
            if (segmentable)
            {
                if (LLAudioSource* old_src = loop.mSourceID.notNull() ? gAudiop->findAudioSource(loop.mSourceID) : nullptr)
                {
                    fadeKill(loop.mSourceID);
                    loop.mSourceID.setNull();
                }
                loop.mSurface = surface;
                loop.mAction = locomotion;
                loop.mSound = sound;
                loop.mSegmented = true;
                loop.mStopAt = 0.0;
                (is_self ? mStepSelf : mStepOther).mMode = 'S';
                return;
            }
        }
    }
    loop.mSegmented = false;
    (is_self ? mStepSelf : mStepOther).mMode = 'L';

    LLAudioSource* source = loop.mSourceID.notNull() ? gAudiop->findAudioSource(loop.mSourceID) : nullptr;

    if (!state_changed && !source && now - loop.mStartedAt < 1.0) return;

    if (state_changed || !source)
    {
        releaseStepLoop(loop);
        loop.mSurface = surface;
        loop.mAction = locomotion;
        loop.mLastSeen = now;

        if (sound.isNull()) return;

        static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);
        loop.mSourceID.generate();
        loop.mSound = sound;
        loop.mStartedAt = now;

        loop.mOffsetMS = 0;
        if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(sound))
        {
            if (meta->mOnsets.size() >= 2)
            {
                const size_t k = (size_t)ll_rand((S32)meta->mOnsets.size() - 1);
                loop.mOffsetMS = meta->mOnsets[k] + (U32)((meta->mOnsets[k + 1] - meta->mOnsets[k]) * 2 / 3);
            }
        }

        source = new LLAudioSource(loop.mSourceID, avatar_id,
                                   llclamp((F32)vol, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_AMBIENT);
        source->setStartOffsetMS(loop.mOffsetMS);
        source->setLoop(true);
        source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
        source->setOcclusion(0.f);
        gAudiop->addAudioSource(source);
        source->play(sound);
        SSSoundMeta::getInstance()->fetch(sound);
        markStepSource(loop.mSourceID);
    }

    if (source)
    {
        static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);
        source->setGain(llclamp((F32)vol, 0.f, 1.f) * loop.mSpeedGain);
        source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
    }
}

// One discrete footstep from a live segmented loop: gate, then cut at the foot.
void SSSoundscape::footstepImpact(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self)
{
    // <SS:Nexii> Gated on its own rather than trusting the loop's gate: the cut fires at the FOOT, which can be across a parcel line from the body the loop was positioned at, and the call comes straight from the avatar's detector.
    if (!stepsAudible(avatar_id, foot_pos_agent)) return;

    auto it = mStepLoops.find(avatar_id);
    if (it == mStepLoops.end() || !it->second.mSegmented || it->second.mSound.isNull() || !gAudiop) return;

    const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(it->second.mSound);
    if (!meta || meta->mOnsets.size() < 4 || meta->mLengthMS == 0) return;

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    StepDebug& dbg = (avatar_id == gAgentID) ? mStepSelf : mStepOther;

    // Pure anti-spam floor, well under any real cadence (SL's run is about a step every 0.3s). It used to sit at 0.24/0.34, close enough to the gait that a slightly early second footfall was
    // read as a duplicate and dropped - which is how a per-step detector ends up sounding like one step per cycle. Rejecting the footfall is now the detector's job, not this gate's.
    const F64 min_gap = 0.10;
    const F64 gap = now - it->second.mLastImpactAt;
    if (gap < min_gap)
    {
        ++dbg.mStepDropped;
        return;
    }
    if (gap < 5.0) dbg.mStepGap = (F32)gap;   // the first footfall of a walk has no predecessor to measure against - mLastImpactAt is still the epoch
    it->second.mLastImpactAt = now;

    static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);

    fadeKill(it->second.mSegSourceID);
    it->second.mSegSourceID = playStepCut(it->second.mSound, foot_pos_agent, llclamp((F32)vol, 0.f, 1.f) * it->second.mSpeedGain);
}

// One discrete footfall window cut from a recording, at the foot; no step loop required.
LLUUID SSSoundscape::playStepCut(const LLUUID& sound, const LLVector3& pos_agent, F32 gain)
{
    if (!gAudiop || sound.isNull()) return LLUUID::null;

    const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(sound);
    if (!meta || meta->mOnsets.size() < 2 || meta->mLengthMS == 0) return LLUUID::null;

    const size_t k = (size_t)ll_rand((S32)meta->mOnsets.size() - 1);

    const U32 start = (meta->mOnsets[k] > 60) ? meta->mOnsets[k] - 60 : 0;
    const U32 cut = meta->mOnsets[k] + (meta->mOnsets[k + 1] - meta->mOnsets[k]) * 2 / 3;
    const F64 window_s = llclamp((F64)(cut - start) / 1000.0, 0.1, 0.9);

    const LLUUID id = LLUUID::generateNewID();
    LLAudioSource* source = new LLAudioSource(id, gAgent.getID(),
                                              llclamp(gain, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_AMBIENT);
    source->setStartOffsetMS(start);
    source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
    gAudiop->addAudioSource(source);
    source->play(sound);
    markStepSource(id);

    mSegmentStops.emplace_back(id, SSAtmoMagic::getInstance()->sharedTime() + window_s);
    return id;
}

// Stops segment-scheduled sources at their planned end.
void SSSoundscape::updateSegmentStops(F64 now)
{
    if (!gAudiop) return;
    for (size_t i = 0; i < mSegmentStops.size(); )
    {
        if (now >= mSegmentStops[i].second)
        {
            fadeKill(mSegmentStops[i].first);
            mSegmentStops.erase(mSegmentStops.begin() + i);
        }
        else ++i;
    }
}

// Entry point from the avatar: routes a step to loop or impact handling.
void SSSoundscape::footstepEvent(const LLUUID& avatar_id, const LLVector3& pos_agent,
                                 bool on_land, S32 action, bool is_self)
{
    // <SS:Nexii> The jump and land one-shots gate here, before the surface roll: nothing below this line makes a sound a muted avatar or a no-sound parcel is allowed to make.
    if (!stepsAudible(avatar_id, pos_agent)) return;

    const LLUUID sound = footstepSound(avatar_id, pos_agent, on_land, action, is_self);
    if (sound.isNull())
    {
        // <SS:Nexii> A touchdown is a footstep even with no Land recording configured:
        // cut one step from the walk recording instead of going silent. The touchdown
        // edge fires once per landing, so a two-foot soft landing sounds once; a live
        // loop mode no-ops the impact because the restarted loop already sounds.
        if (action == STEP_LAND)
        {
            footstepImpact(avatar_id, pos_agent, is_self);
            if (mStepLoops.find(avatar_id) == mStepLoops.end())
            {
                static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);
                // <SS:Nexii> footstepSound wipes the debug record on entry, so asking it for the walk sound here overwrote the landing it was just asked about and the readout showed every touchdown as a walk step. The land record is the one worth keeping - the walk lookup is a fallback for a UUID, not an event - so it is saved across the query and put back.
                StepDebug& dbg = is_self ? mStepSelf : mStepOther;
                const StepDebug land_dbg = dbg;
                const LLUUID fallback = footstepSound(avatar_id, pos_agent, on_land, STEP_WALK, is_self);
                dbg = land_dbg;
                playStepCut(fallback, pos_agent, llclamp((F32)vol, 0.f, 1.f));
            }
        }
        return;
    }

    static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);

    markStepSource(ss_play_oneshot(sound, gAgent.getPosGlobalFromAgent(pos_agent),
                                   llclamp((F32)vol, 0.f, 1.f), 0.f));
}

// Mirrors the avatar-side footfall detector into the debug readout: foot offsets along travel, swing/stance phase and the speed gain.
void SSSoundscape::noteFootGait(bool is_self, S32 loco, const F32 s[2], const bool swing[2], F32 speed_gain)
{
    StepDebug& dbg = is_self ? mStepSelf : mStepOther;
    dbg.mLoco = loco;
    dbg.mSpeedGain = speed_gain;
    for (S32 f = 0; f < 2; ++f)
    {
        dbg.mFootS[f] = s[f];
        dbg.mFootSwing[f] = swing[f];
    }
}

// Tags a source as a step sound for the reaper.
void SSSoundscape::markStepSource(const LLUUID& source_id)
{
    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugFootstepMarkers", false);
    if (!markers || source_id.isNull()) return;

    StepMark mark;
    mark.mSourceID = source_id;
    mark.mStart = SSAtmoMagic::getInstance()->sharedTime();
    mark.mText = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
    if (mark.mText)
    {
        mark.mText->setDoFade(false);
        mark.mText->setZCompare(false);
    }
    mStepMarks.push_back(mark);
}

// Ages step-source marks.
void SSSoundscape::updateStepMarks(F64 now)
{
    for (size_t i = 0; i < mStepMarks.size(); )
    {
        StepMark& mark = mStepMarks[i];
        LLAudioSource* source = gAudiop ? gAudiop->findAudioSource(mark.mSourceID) : nullptr;
        if (!source)
        {
            if (mark.mText) mark.mText->markDead();
            mStepMarks.erase(mStepMarks.begin() + i);
            continue;
        }
        if (mark.mText)
        {
            const LLVector3 pos = gAgent.getPosAgentFromGlobal(source->getPositionGlobal());
            const LLVector3 mark_pos = pos + LLVector3(0.f, 0.f, 0.3f);
            mark.mText->setPositionAgent(mark_pos);

            // <SS:Nexii> Only marks the camera could actually see: within 24m and with clear line of sight. The ray runs from the mark towards the camera through world geometry only -
            // avatars are not in that partition set and alpha texels are not picked - so a crowd or a foliage wall never hides a step, but a floor or a solid wall does. Cheap enough per
            // frame: there is one mark per live step source.
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
            const F32 MARK_RANGE = 24.f;
            bool visible = (cam - mark_pos).magVecSquared() < MARK_RANGE * MARK_RANGE;
            if (visible)
            {
                LLVector4a start4, end4, hit;
                start4.load3(mark_pos.mV);
                end4.load3(cam.mV);
                visible = !gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &hit, false, false);
            }
            mark.mText->setHidden(!visible);

            const bool fresh = now - mark.mStart < 0.25;
            mark.mText->setString(fresh ? std::string(">> STEP <<") : std::string("."));
            mark.mText->setColor(fresh ? LLColor4(1.f, 0.95f, 0.1f, 1.f)
                                       : LLColor4(0.45f, 0.95f, 0.55f, 0.5f));
        }
        ++i;
    }
}

// Chooses which recording a step plays: surface, action, wetness, preset overrides then the globals.
LLUUID SSSoundscape::footstepSound(const LLUUID& avatar_id, const LLVector3& foot_pos_agent,
                                   bool on_land, S32 action, bool is_self)
{
    StepDebug& dbg = is_self ? mStepSelf : mStepOther;
    dbg = StepDebug();
    dbg.mWhen = SSAtmoMagic::getInstance()->sharedTime();
    dbg.mAction = action;
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!SSAtmoMagic::getInstance()->isSwitchedOn() || !sounds)
    {
        dbg.mWhyNot = SSAtmoMagic::getInstance()->isSwitchedOn()
            ? "SSAtmoSounds off" : "Atmo Magic off";
        return LLUUID::null;
    }

    F32 column_top = 0.f;
    const bool flow_knows = SSWindFlowMap::getInstance()->surfaceAt(foot_pos_agent, column_top);
    bool indoors = flow_knows && (column_top - foot_pos_agent.mV[VZ] > 0.75f);
    dbg.mIndoorsFrom = flow_knows ? 'f' : '-';

    if (!flow_knows)
    {
        if (roofOver(avatar_id, foot_pos_agent, is_self))
        {
            indoors = true;
            dbg.mIndoorsFrom = 'r';
        }
    }

    dbg.mIndoors = indoors;

    SSStepSurface surface;
    if (indoors)
    {
        surface = STEP_INSIDE_DRY;
    }
    else
    {
        const SSSurfaceField::Sample wet = SSSurfaceField::instance().sample(foot_pos_agent);
        const bool puddle = wet.mValid && wet.mPuddle > 0.005f;
        const bool damp = wet.mValid && wet.mWet > 0.3f;

        dbg.mFieldValid = wet.mValid;
        dbg.mWet = wet.mWet;
        dbg.mPuddle = wet.mPuddle;

        if (on_land && !onObject(avatar_id, foot_pos_agent, is_self))
        {
            surface = puddle ? STEP_TERRAIN_PUDDLE : (damp ? STEP_TERRAIN_WET : STEP_TERRAIN_DRY);
        }
        else
        {
            surface = puddle ? STEP_OUTSIDE_PUDDLE : (damp ? STEP_OUTSIDE_WET : STEP_OUTSIDE_DRY);
        }
    }

    dbg.mSurface = surface;
    dbg.mGlobal = SSFootstepSounds::surfaceIsGlobal(surface);

    std::string csv;
    if (dbg.mGlobal)
    {
        dbg.mSource = SSFootstepSounds::globalSettingName(surface, (SSStepAction)action);
        csv = SSAtmoStore::getString(dbg.mSource);
    }
    else
    {
        dbg.mSource = std::string(SSPrecipPresetManager::instance().active().mName)
            + "/" + SSFootstepSounds::surfaceKey(surface)
            + "_" + SSFootstepSounds::actionKey((SSStepAction)action);
        csv = SSPrecipPresetManager::instance().active().mFootsteps.at(surface, (SSStepAction)action);
    }

    if (csv.empty())
    {
        dbg.mWhyNot = "slot empty";
        return LLUUID::null;
    }

    std::vector<std::string> tokens;
    LLStringUtil::getTokens(csv, tokens, ",");
    std::vector<LLUUID> ids;
    ids.reserve(tokens.size());
    for (const std::string& tok : tokens)
    {
        LLUUID id(tok);
        if (id.notNull()) ids.push_back(id);
    }
    dbg.mListSize = (S32)ids.size();
    if (ids.empty())
    {
        dbg.mWhyNot = "no valid UUIDs";
        return LLUUID::null;
    }

    dbg.mPicked = ids[ll_rand((S32)ids.size())];
    return dbg.mPicked;
}

// Per-frame drive: probes, loops, thunder, followers, steps, reaping.
void SSSoundscape::idle()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_AUDIO);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const F64 now = atmo->sharedTime();
    F32 dt = (mLastIdle > 0.0) ? (F32)(now - mLastIdle) : 0.f;
    dt = llclamp(dt, 0.f, 0.25f);
    mLastIdle = now;

    SSSoundMeta::getInstance()->idle();

    reapStepLoops(now);
    updateFollowers(now, dt);
    updateSegmentStops(now);
    updateStepMarks(now);
    updateDying(now);

    updateThunder(now);

    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!atmo->isEnabled() || !sounds || !gAudiop)
    {
        stopAll();
        return;
    }

    if (dt > 0.f)
    {
        mImpactRate *= expf(-dt / IMPACT_RATE_TAU);
    }

    updateProbes(now);
    updateLoops(now, dt);
}
