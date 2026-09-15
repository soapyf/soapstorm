/**
 * @file ssvolcloud.cpp
 * @brief See ssvolcloud.h.
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

#include "ssvolcloud.h"

#include "ssdeckcellsoftcore.h" // the observer's cell-lattice softening - one per-cell quantity read continuously
#include "ssdeckflowcore.h" // the card's own frame, the advected octave's sign rule, its edge mask and per-puff swirl
#include "ssdecklodcore.h"
#include "ssdeckmacrocore.h"
#include "ssdecknoisecore.h"
#include "ssdeckshadecore.h"
#include "sssquallcore.h" // 8a: SSSquall::LINE_ANVIL_FRAC - every applyLineBand call site's anvil weight
#include "ssveilcore.h" // 8g: SSVeil - the base veil's own law (rim fade-IN, outward reach, tile radii)
#include "ssvirgacore.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvcloudfieldstate.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvtrackstate.h"
#include "ssatmomagic.h"
#include "ssprecipitation.h"
#include "sslightning.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "llimage.h"
#include "llrender.h"
#include "v3colorutil.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "pipeline.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

extern bool gCubeSnapshot;

namespace
{
    const F32 CELL_M = 260.f;

    // <SS:Nexii> The field's reach. Two cross-file rails are locked to FIELD_DRAW_M and must move with it: the base veil's edge fade in ssVolCloudF.glsl (ss_edge_rails, SSDeckLod::FIELD_FADE_START_M/DECK_EDGE_M uploaded from the core - not the old hardcoded 6800/9800 pair, see that uniform's own comment) and SS_DECK_EDGE_M in lldrawpoolwlsky.cpp (the dome band's horizon melt runs to the deck's dissolve line); the ss_rim uniform in render() derives from these directly. The far plane never moves - the squash folds whatever reach these ask for into the same drawn band, so raising them buys distance with builder cells, sheet tiles and depth compression, not with clip planes. 8g CORRECTION (ssveilcore.h): the first of those two rails is stale - ss_edge_rails is now the PUFFS' fade only. The base veil runs on ss_veil_rails (SSDeckLod::THIN_START_M/FIELD_DRAW_M for its rim fade-IN, SSVeil::REACH_START_M/REACH_END_M for its outward reach) and deliberately outlives the puffs, so FIELD_DRAW_M now has a THIRD consumer with the opposite sense. FIELD_RADIUS_M is likewise no longer the pass's outer limit: mEffRadius is llmax(FIELD_RADIUS_M, SSVeil::REACH_END_M) - see its own comment in update().
    const F32 FIELD_RADIUS_M = 12000.f;

    const F32 FIELD_DRAW_M = 10000.f;
    const F32 FIELD_FADE_START_M = 8000.f;

    // <SS:Nexii> The bounds the puff budget dial (SSAtmoCloudPuffBudget) is held inside. The floor is low enough to be a real emergency setting and high enough that a deck still reads as a deck rather than a scatter of stray quads; the ceiling is well past what any sky asks for, and exists so a typo in the spinner cannot hand the sort loop a number that costs a frame.
    const S32 MIN_PUFF_BUDGET = 64;
    const S32 MAX_PUFF_BUDGET = 8000;

    // <SS:Nexii> D3 [interaction: ssVolCloudF.glsl SS_NOISE_M]: metres of world per tile of the noise map, as the
    // fragment stage spells it. LOCKSTEP - the only CPU consumer is SSDeckBoil::wrapFallM, which folds the rain
    // curtain's fall on this period so the subtraction the shader does against a wrapping texture is exact; a
    // divergence here would show as a jump in the streak each time the accumulator wrapped.
    const F32 SHADER_NOISE_M = 880.f;

    const F32 PUFF_CELL_FRACTION = 0.85f;

    const F32 PUFF_WIDE = 1.7f;
    const F32 PUFF_TALL = 0.62f;

    const F32 PUFF_ROUND_LO = 0.15f;
    const F32 PUFF_ROUND_HI = 0.70f;

    // <SS:Nexii> How many puffs a 260m cell is filled with, and the bounds the viewer's field density dial (SSAtmoCloudPuffsPerCell) may move it between. This is the field's DENSITY - the cell grid stays where it is and gets more or fewer bodies in it - and it is deliberately the only count the viewer may touch: CELL_M itself is replicated verbatim by the fragment stage's gate (SS_CELL_M in ssVolCloudF.glsl), so moving the grid would desync the carving from the geometry it carves, while the sub-count is the builder's alone and nothing downstream replicates it.
    const S32 PUFFS_PER_CELL = 3;
    const S32 MIN_PUFFS_PER_CELL = 1;
    const S32 MAX_PUFFS_PER_CELL = 8;

    const F32 PUFF_THICKNESS_GAIN = 0.35f;

    // <SS:Nexii> The refinement LOD (SSAtmoCloudTessellation - hashed child puffs grown around near puffs) was removed
    // 2026-09-06: near-field detail is the anatomy tier's job, entities acting on the container (doc/atmo_magic_phase8_show.md
    // section 4.0), not a finer sampling of the same field.

    const F32 COVERAGE_FLOOR = 0.04f;

    // Deterministic cell hash - the whole field derives from position, so every client sees the same clouds.
    U32 hashCell(S32 x, S32 y, U32 salt)
    {
        U32 h = ((U32)x * 374761393u) ^ ((U32)y * 668265263u) ^ (salt * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    // Smoothstep.
    F32 ss_smoothstep(F32 lo, F32 hi, F32 v)
    {
        const F32 t = llclamp((v - lo) / llmax(hi - lo, 1.0e-5f), 0.f, 1.f);
        return cubic_step(t);
    }

    // Cell hash to [0,1).
    F32 hashUnit(S32 x, S32 y, U32 salt)
    {
        return (F32)(hashCell(x, y, salt) & 0x00ffffffu) / (F32)0x01000000;
    }

    const S32 CLUSTER_CELLS_BIG = 9;
    const S32 CLUSTER_CELLS_SMALL = 3;
    const F32 CLUSTER_OCTAVE_MIX = 0.4f;

    const F32 CLUSTER_WEIGHT = 0.85f;

    // <SS:Nexii> The convection noise map. One authored tileable greyscale map per deck, read back to the CPU and sampled per cell to give the deck's response to convection a geography. The map's values run through two ramps: the HOLE window - below its low edge the cell is cut away entirely, so where the map runs low the sky opens; this is what breaks a dry stable deck into cloud and holes, the TOWER window - the gradient ramp overlaid on the same values, deciding which columns are rising thermals. As the convection dial climbs, tower-weighted cells keep the full climb to the lid while the pockets between them are held low, and the tower columns take the anvil's flat-and-flare spread before the dial alone would allow it - which is how the anvil forms early, on the strong towers first. Moisture then lifts the whole map: the same values that broke a dry stable sky leave a moist one unbroken, the overcast nimbostratus sheet. The tile is the field-scale metre count at Noise Scale 1, and the grid is the cached readback's fixed resolution - the structure it carries is kilometres wide, so 64 across carries it with texels to spare.
    const F32 SS_NOISE_TILE_M = 2048.f;
    const S32 SS_NOISE_GRID = 64;

    // The profile curve's row count - a vertical curve needs no more resolution than this.
    const S32 SS_PROFILE_N = 64;

    // The built-in profile strip's paint size, for the picker's preview only.
    const S32 SS_PROFILE_STRIP_W = 256;
    const S32 SS_PROFILE_STRIP_H = 8;

    // <SS:Nexii> Phase 6b retune, v2 (doc/atmo_magic_far_clouds.md section 2 step 4, ssdecknoisecore.h CONTRACT; corrected 2026-09-05 per review F1): noiseFieldAt's raw_n is now
    // SSDeckNoise::mixDetile of two reads (weight 0.65), which measures 0.545x the single read's variance (SSDeckNoise::varianceRatio) - narrower around the mean. The FIRST retune pass matched
    // TAIL SHARE at each hard edge (hole 0.30/0.66, tower 0.37/0.73) and was wrong: nothing downstream reads a hard edge - the builder (and every replicated site) consumes the CONTINUOUS
    // smoothstep through gate = gate_raw + (1 - gate_raw) * (1 - presence), so a cell can be partially cut without either value ever crossing lo or hi. Matching only the tail-crossing population
    // moved the whole ramp toward the mean and, measured against the OLD windows through the SAME mixed field, deleted 18-50% of cells that used to be at least partly occupied - a materially
    // different sky, not a calibration rounding error.
    // The v2 windows below are SSDeckNoise::HOLE_LO/HI and TOWER_LO/HI: the OLD edges (0.16/0.52 hole, 0.42/0.78 tower) scaled about the map's measured mean (SSDeckNoise::MAP_MEAN = 0.5579,
    // seed 0x5EED1337) by sqrt(SSDeckNoise::varianceRatio(DETILE_WEIGHT)) = 0.7382 - see ssdecknoisecore.h's retunedEdge(). Because the mix preserves the mean and scales the spread by exactly
    // that factor, scaling the threshold the same way about the same mean keeps the STATISTICAL OCCUPANCY the continuous gate produces close to what the old, single-read field produced -
    // not just the population beyond a hard cutoff. Measured occupancy (fraction of cells with gate <= coverage, the real gate formula above, not a tail share) on the same 20km x 20km / CELL_M
    // grid the first pass used, new windows vs. old windows through the OLD single read, at hole strength (deck.mNoiseHole) 1.0 and 0.5:
    //   coverage      0.3      0.5      0.7
    //   hole 1.0     -5.5%    -2.2%    +0.9%
    //   hole 0.5     -4.5%    -0.2%    +1.4%
    // All six within the +-6% relative target. Pinned by V:\Scratch\atmo\tests\scenario_detile_occupancy.cpp, which derives both the old and new occupancy from the transliterated map and gate
    // rather than hardcoding either. LOCKSTEP with ssVolCloudF.glsl's two `smoothstep(0.264, 0.530, n_map)` hole-window literals (ss_cell_occupied's own presence fetch, and the sheet's presence
    // cut - the puff path has no presence cut of its own; its n_map read feeds the tower ramp, anvil and thick-base fill instead) - the tower window is NOT a GLSL literal, it reaches the shader
    // through the ss_tower_ramp uniform (deck.mNoiseTowerLo/Hi below), so retuning SS_TOWER_LO/HI here is enough on that side. STORM_TOWER_LO/HI (ssstormcouplecore.h, the consolidation targets
    // these lerp toward) are unchanged by this retune - severe weather's blended tower window collapses to STORM_TOWER_HI exactly (storm_eff == 1) regardless of SS_TOWER_HI.
    const F32 SS_HOLE_LO = SSDeckNoise::HOLE_LO;
    const F32 SS_HOLE_HI = SSDeckNoise::HOLE_HI;
    const F32 SS_TOWER_LO = SSDeckNoise::TOWER_LO;
    const F32 SS_TOWER_HI = SSDeckNoise::TOWER_HI;

    // The moisture band that lifts the map's floor over its holes - the dry end keeps them
    // open, the mid-high end closes every one and the deck reads as unbroken nimbostratus.
    const F32 SS_NIMBUS_LO = 0.30f;
    const F32 SS_NIMBUS_HI = 0.72f;

    // And how much of the hole-cutting convection keeps alive in the dry case: a storm sky
    // still wants its gaps between towers, a stable sky its outright holes.
    const F32 SS_STORM_GAP = 0.45f;

    // The under deck's hash salt: the same cell field, offset so its cloud masses land where the
    // main deck's do not. Zero keeps the primary deck's pattern byte-identical to the single-deck
    // field it was before two decks existed.
    constexpr U32 SS_UNDER_DECK_SALT = 61u;

    // <SS:Nexii> Distant rain shafts (ssvirgacore.h): SSVirga::keepHash's own salt argument - a different chain
    // from the gate hash (1u+salt) and the sub-puff hashes (2u..9u+sub_salt) above, so the stable keep/drop
    // decision never aliases the cell gate's own pattern. ShaftCandidate::hashKey is a SEPARATE, local hashCell
    // chain (salt + this constant) used only by the rare hard-ceiling fallback sort below - keepHash never reads
    // hashKey, it hashes ShaftCandidate::cellId itself.
    constexpr U32 SS_VIRGA_HASH_SALT = 6151u;

    // <SS:Nexii> The procedural fallback for the convection noise map, for decks with no authored texture. Square by construction (the field map is a tiling square, and a square tile is what every consumer of it assumes), tileable by wrapping lattice, and seeded off the weather - so every client sharing an environment grows the same geography without anyone uploading a map.
    const S32 SS_NOISE_PROC_SIZE = 256;

    // One octave of tileable value noise: a period-cell lattice over the unit square, every
    // corner hash wrapping at the period so the tile seamless.
    F32 tileLattice(F32 x, F32 y, S32 period, U32 salt)
    {
        const F32 fx = x * (F32)period;
        const F32 fy = y * (F32)period;

        const S32 x0 = llfloor(fx);
        const S32 y0 = llfloor(fy);

        F32 tx = fx - (F32)x0;
        F32 ty = fy - (F32)y0;
        tx = cubic_step(tx);
        ty = cubic_step(ty);

        const S32 wx = ((x0 % period) + period) % period;
        const S32 wy = ((y0 % period) + period) % period;
        const S32 wx1 = (wx + 1) % period;
        const S32 wy1 = (wy + 1) % period;

        const F32 c00 = hashUnit(wx,  wy,  salt);
        const F32 c10 = hashUnit(wx1, wy,  salt);
        const F32 c01 = hashUnit(wx,  wy1, salt);
        const F32 c11 = hashUnit(wx1, wy1, salt);

        const F32 top = c00 + (c10 - c00) * tx;
        const F32 bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    // Five octaves, periods 3..48, summed and normalised, then spread - plain FBM of value noise
    // bunches around the middle, and a map the ramps can actually split into towers and pockets
    // needs its tails.
    F32 tileFbm(F32 x, F32 y, U32 salt)
    {
        F32 v = 0.f;
        v += tileLattice(x, y,  3, salt +  1u) * 0.500f;
        v += tileLattice(x, y,  6, salt +  7u) * 0.250f;
        v += tileLattice(x, y, 12, salt + 13u) * 0.125f;
        v += tileLattice(x, y, 24, salt + 29u) * 0.0625f;
        v += tileLattice(x, y, 48, salt + 53u) * 0.0625f;
        v = llclamp(0.5f + (v - 0.5f) * 1.9f, 0.f, 1.f);
        return v;
    }

    // The square map itself: luminance in an RGB raw image, one FBM read per texel.
    LLPointer<LLImageRaw> makeProceduralNoise(U32 seed)
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw(SS_NOISE_PROC_SIZE, SS_NOISE_PROC_SIZE, 3);
        U8* data = raw->getData();
        if (!data) return nullptr;

        for (S32 y = 0; y < SS_NOISE_PROC_SIZE; ++y)
        {
            U8* row = data + (size_t)y * SS_NOISE_PROC_SIZE * 3;
            const F32 fy = (F32)y / (F32)SS_NOISE_PROC_SIZE;
            for (S32 x = 0; x < SS_NOISE_PROC_SIZE; ++x)
            {
                const F32 v = tileFbm((F32)x / (F32)SS_NOISE_PROC_SIZE, fy, seed);
                const U8 b = (U8)llclamp((S32)(v * 255.f), 0, 255);
                row[x * 3 + 0] = b;
                row[x * 3 + 1] = b;
                row[x * 3 + 2] = b;
            }
        }
        return raw;
    }

    // The built-in vertical curves painted as a strip - the picker's preview for a None profile.
    // Row 0 is v 0, the deck's base, exactly the orientation an authored strip displays in. RGB
    // only: the built-ins carry no base fill, and an RGB strip previews without its alpha
    // channel masquerading as transparency.
    LLPointer<LLImageRaw> makeProfilePreview()
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw(SS_PROFILE_STRIP_W, SS_PROFILE_STRIP_H, 3);
        U8* data = raw->getData();
        if (!data) return nullptr;

        for (S32 y = 0; y < SS_PROFILE_STRIP_H; ++y)
        {
            // Row 0 at v 0: the readback and the shader both treat the first row as the base.
            const F32 v = (F32)y / (F32)(SS_PROFILE_STRIP_H - 1);
            const F32 r = ss_smoothstep(0.70f, 1.25f, v) * 0.7f;
            const F32 g = ss_smoothstep(0.20f, 0.45f, v);
            const F32 b = ss_smoothstep(0.74f, 0.97f, v);

            U8* row = data + (size_t)y * SS_PROFILE_STRIP_W * 3;
            for (S32 x = 0; x < SS_PROFILE_STRIP_W; ++x)
            {
                row[x * 3 + 0] = (U8)llclamp((S32)(r * 255.f), 0, 255);
                row[x * 3 + 1] = (U8)llclamp((S32)(g * 255.f), 0, 255);
                row[x * 3 + 2] = (U8)llclamp((S32)(b * 255.f), 0, 255);
            }
        }
        return raw;
    }

    // One value-noise octave over cell space, for cloud clustering.
    F32 clusterOctave(S32 cx, S32 cy, S32 cells, U32 salt, F32 shift)
    {
        const F32 fx = (F32)cx / (F32)cells + shift;
        const F32 fy = (F32)cy / (F32)cells + shift;

        const S32 x0 = llfloor(fx);
        const S32 y0 = llfloor(fy);

        F32 tx = fx - (F32)x0;
        F32 ty = fy - (F32)y0;
        tx = cubic_step(tx);
        ty = cubic_step(ty);

        const F32 c00 = hashUnit(x0,     y0,     salt);
        const F32 c10 = hashUnit(x0 + 1, y0,     salt);
        const F32 c01 = hashUnit(x0,     y0 + 1, salt);
        const F32 c11 = hashUnit(x0 + 1, y0 + 1, salt);

        const F32 top = c00 + (c10 - c00) * tx;
        const F32 bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    // Two-octave cluster noise: big masses with small-scale raggedness.
    F32 clusterUnit(S32 cx, S32 cy, U32 salt)
    {
        const F32 big = clusterOctave(cx, cy, CLUSTER_CELLS_BIG, 101u + salt, 0.f);
        const F32 small = clusterOctave(cx, cy, CLUSTER_CELLS_SMALL, 137u + salt, 0.37f);
        return big * (1.f - CLUSTER_OCTAVE_MIX) + small * CLUSTER_OCTAVE_MIX;
    }

    // <SS:Nexii> review 3b NEW-3, moved into ssstormcouplecore.h (numeric shape maths belongs in a core, not the
    // shell - PLAN.md lesson 2): the per-cell height/anvil shaping buildDeck's placement loop and the V2
    // profile-outline debug view both derive from - see SSStormCouple::CellShape/cellShapeAt.

    // <SS:Nexii> Distant rain shafts (ssvirgacore.h): a qualifying cell, gathered during the main cell walk and
    // only turned into cards AFTER the whole walk finishes - the qualifying SET is a pure function of the field
    // and weather (SSVirga::qualifies), never of loop order or camera distance, so it has to be collected in full
    // before SSVirga::keepHash's per-candidate stable trim can run over it (F1: no sort needed for that trim -
    // keepHash is pure per candidate). x/y are the shaft column's world position, placed through
    // SSDeckFrame::placeWorld with a ZERO shear table (base-anchored - F11, never a respelled cell-centre + drift
    // + hero-shift sum) and the hero shift, exactly the PRODUCER placement every ordinary puff uses. cellId packs
    // the cell's (cx,cy) the way SSVolCloud::mOccGrid does (cx in the HIGH word, cy in the LOW word) - this is what keepHash
    // hashes, not a precomputed hash value. hashKey is a SEPARATE local hashCell chain (SS_VIRGA_HASH_SALT).
    // Used ONLY by the rare hard-ceiling fallback sort (keepHash's own kept share running over SSVirga::hardCap on
    // a small/quantised n) - the ordinary path never sorts candidates at all.
    struct ShaftCandidate
    {
        F32 x = 0.f;
        F32 y = 0.f;
        F32 drive = 0.f;
        U64 cellId = 0u;
        U32 hashKey = 0u;
    };
}

// Drops the field - rebuilt from scratch next update.
void SSVolCloud::clear()
{
    mPrimary.mPuffs.clear();
    mUnder.mPuffs.clear();
    mLastBuildMS = 0.f;

    // <SS:Nexii> review S9: drop the scheduler claim with the field it was feeding - nothing left holding it means
    // SSStormCells::update() early-outs again next frame instead of resolving cells nobody reads.
    mStormInterest = SSStormCells::Interest();
    mStormCellCount = 0;
    mHeroFrame = SSDeckFrame::HeroFrame();
    mLineBand = SSStormCouple::LineBand(); // 8a: strength 0 - disabled, same rule as mStormCellCount 0 above
    mVirgaDebug = SSVirgaDebug();
}

void SSVolCloud::shutdownGL()
{
    clear();
    for (Deck* deck : { &mPrimary, &mUnder })
    {
        deck->mTextureRef = nullptr;
        deck->mDetailRef = nullptr;
        deck->mTextureNextRef = nullptr;
        deck->mDetailNextRef = nullptr;
        deck->mNoiseRef = nullptr;
        deck->mNoiseProcRaw = nullptr;
        deck->mNoiseProcRef = nullptr;
        deck->mProfileRef = nullptr;
        deck->mProfileProcRef = nullptr;
    }
    mShadowRef = nullptr;
    mShadowRaw = nullptr;
    if (mDepthCopy.getWidth() > 0) mDepthCopy.release();
}

// Rebuilds the puff field for this frame from the resolved cloud state: deterministic placement, lighting, squash band, strike lights, depth sort.
void SSVolCloud::update(F32 dt)
{
    mPrimary.mPuffs.clear();
    mUnder.mPuffs.clear();
    mLastBuildMS = 0.f;
    // <SS:Nexii> V4 debug snapshot: reset every update() (not just clear()), so an early-out below (feature off,
    // no asset, no tracks) never leaves the LAST build's shaft set on display as if it were current.
    mVirgaDebug = SSVirgaDebug();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoVolumetricClouds", true);
    if (!enabled) return;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr || !mgr->hasAsset()) return;

    // <SS:Nexii> D3 (fourth build report) [interaction: ssdeckboilcore.h]: THE BOIL CLOCK, integrated. The
    // fragment stage used to build the advected octave's phase as `ss_time * rate` - an absolute clock times a
    // rate that steps once a second, because both of that rate's factors are resolved from
    // SSAtmoEnvTrack::currentDayCyclePhase(), which THEN read time(nullptr) in WHOLE SECONDS. Multiplied by a
    // clock of thousands of seconds, each one-second tread became a jump in the phase itself. Integrating the
    // rate instead makes a rate step a change of DERIVATIVE, which nothing can see. See SSDeckBoil's header.
    // <SS:Nexii> D3 follow-up (fifth build report, "still seeing the issue of clouds jumping each second")
    // [interaction: SSAtmoEnvTrack::currentDayCyclePhase]: that 1 Hz tread is GONE at its root - the phase now
    // reads SSAtmoMagic::sharedTime(), the per-frame latch of LLDate::now().secondsSinceEpoch(), so
    // deck.mDriftRate and deck.mChurn (and every other dial resolved below) ease continuously instead of
    // stepping. This integration is kept and is still the right shape: it is what makes a rate CHANGE of any
    // kind - continuous or not - invisible in the phase, and it is what keeps the uniforms in F32 range however
    // long the viewer has been open. Measured in tests/unit_phase_clock.cpp.
    // Both decks advance every frame on their own last-resolved dials, whether or not they get rebuilt below -
    // a deck that dips under the coverage floor for a few seconds must not come back on a stale phase. dt is
    // gFrameIntervalSeconds (ssatmomagic.cpp's call); SSDeckBoil::clampDt bounds a hitch or an alt-tab out.
    for (Deck* boil_deck : { &mPrimary, &mUnder })
    {
        boil_deck->mBoilLaps = SSDeckBoil::advanceLaps(boil_deck->mBoilLaps, boil_deck->mDriftRate, boil_deck->mChurn, dt);
        boil_deck->mFallM = SSDeckBoil::advanceFallM(boil_deck->mFallM, boil_deck->mDriftRate, dt);
    }

    LLTimer timer;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (asset.mTracks.empty()) return;

    const F32 world_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(asset, world_z, world_z, true);
    S32 track_index = blend.mPrimaryTrack;
    if (track_index < 0 || track_index >= (S32)asset.mTracks.size()) track_index = 0;

    const SSAtmoEnvTrack& track = asset.mTracks[(size_t)track_index];
    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();

    const F32 moisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    const F32 convection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    const F32 temperature = llclamp(track.mWeather.mTemperatureC.valueAt(phase), -60.f, 60.f);

    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 sun_alt = sky ? sky->getSunDirection().mV[VZ] : 0.f;

    const LLVector3 light_dir = LLEnvironment::instance().getLightDirection();

    // <SS:Nexii> The deck's COLOUR no longer comes from here: it lived on this class as a CPU replica of the dome band's beam extinction (the 1/(2 elevation) cosecant), and that replica is the family of failed tinting attempts - it underflowed to grey at every low sun (the same white-snap failure skyV.glsl's optics tint was rescued from), and the cloud-colour normalisation stacked on top of it (clamp(cc/0.41)) was a hack against a hack. The light is computed in the VERTEX SHADER now (ssVolCloudV.glsl), from the dome's own uniforms with skyV's capped-glow extinction, so the deck wears the same sunset the dome does by construction. What survives here is the BEAM gate alone - how much direct celestial light exists - which the same crude extinction is a perfectly good heuristic for, because all it feeds is a 0..1 flattening of the structural shading, never a colour anyone sees.
    LLColor3 sunlit(1.f, 1.f, 1.f);
    if (sky)
    {
        const F32 sun_elev = llmax(sun_alt, 0.0001f);
        const F32 off_axis = 1.f / (sun_elev * 2.f);
        const LLColor3 light_atten = (LLColor3(sky->getBlueDensity())
                                    + LLColor3(sky->getHazeDensity() * 0.25f))
                                    * (sky->getDensityMultiplier() * sky->getMaxY());
        LLColor3 sun_col(sky->getSunlightColor());
        componentMultBy(sun_col, componentExp(light_atten * -off_axis));

        // How far the sun is still "up", easing out across the horizon as it sets; below it the
        // moon's faint share keeps the beam from reading as fully dead on a bright-moon night.
        const F32 sun_up = ss_smoothstep(-0.10f, 0.05f, sun_alt);

        const LLColor3 moon_col = LLColor3(sky->getMoonlightColor()) * 0.16f;
        sunlit = sun_col + moon_col * (1.f - sun_up);
    }

    {
        const F32 sun_lum = (sunlit.mV[0] + sunlit.mV[1] + sunlit.mV[2]) / 3.f;
        const F32 t = llclamp((sun_lum - 0.08f) / 0.5f, 0.f, 1.f);
        mBeam = cubic_step(t);
    }

    mLightDir = light_dir;

    // <SS:Nexii> 8g (ssveilcore.h): the squash's virtual radius must cover the FARTHEST thing this pass draws, which is
    // no longer a puff at FIELD_DRAW_M but the base veil's outer tile ring at SSVeil::REACH_END_M (14000 m) - past
    // FIELD_RADIUS_M's 12000. Beyond mEffRadius the vertex stage clamps drawn distance to ss_squash.y * 0.999
    // (ssVolCloudV.glsl), so every veil tile from 12 to 14 km would have collapsed onto one drawn radius: a flat
    // bright ring at the fold with the fragment stage un-squashing all of it to the same wrong eye distance. Raising
    // the radius does NOT change apparent size - the squash scales each quad corner about the camera, so angular size
    // is preserved and the fragment stage inverts the mapping exactly - it only re-spreads drawn depth, so the deck
    // beyond the knee sits marginally nearer in the depth buffer than before. [interaction: doc/atmo_magic_far_clouds.md
    // section 4 "Beyond 10km", which states the squash needs no mechanical change for reach - only this radius does.]
    mEffRadius = llmax(FIELD_RADIUS_M, SSVeil::REACH_END_M);
    // Cap and knee: see SS_SQUASH_CAP_FRAC in ssvolcloud.h. The knee is a plain 0.8 of the cap, so the field from there outward folds into the last fifth of drawn depth.
    mSquashCap = MAX_FAR_CLIP * SS_SQUASH_CAP_FRAC;
    mSquashKnee = mSquashCap * 0.8f;

    const SSAtmoEnvCloudFieldState field =
        SSAtmoEnvCloudFieldResolver::resolve(track.mCloudField, track.mWeatherInfluence,
                                             moisture, convection, temperature,
                                             phase, track.mFloorZ);

    // <SS:Nexii> Which deck the weather's noise gate reads: the authored source when it names the under deck and that deck is on, the main field otherwise - which is every sky build's answer, since its under deck hangs below the platform and is nobody's weather. The same rule the environment editor's derivation follows, kept where the deck lives so precipitation and deck can never disagree about who is making weather. [interaction: precipitation]
    mWeatherDeck = (track.mWeatherSourceDeck == SS_ATMOENV_DECK_UNDER
                    && track.mUnderField.mEnabled) ? 1 : 0;

    bool primary_built = false;
    if (field.mCoverage >= COVERAGE_FLOOR && field.mThicknessM > 1.f)
    {
        buildDeck(mPrimary, field, convection, moisture, 0u, track, phase);
        primary_built = true;
    }

    // <SS:Nexii> The under deck: the same resolver and the same builder against the track's second field, hashed with its own salt so the two decks' cloud patterns are independent - a mirror copy of the main deck at a different altitude would read as exactly the artifact it is.
    bool under_built = false;
    if (track.mUnderField.mEnabled)
    {
        const SSAtmoEnvCloudFieldState under =
            SSAtmoEnvCloudFieldResolver::resolve(track.mUnderField, track.mWeatherInfluence,
                                                 moisture, convection, temperature,
                                                 phase, track.mFloorZ, false);
        if (under.mCoverage >= COVERAGE_FLOOR && under.mThicknessM > 1.f)
        {
            buildDeck(mUnder, under, convection, moisture, SS_UNDER_DECK_SALT, track, phase);
            under_built = true;
        }
    }

    // <SS:Nexii> review S9: whichever deck weatherDeck() names is the one buildDeck couples the scheduler through
    // (S4); when THAT deck falls under the coverage floor this frame (either build skipped above), release the
    // claim and zero the stale count rather than let a scheduler claim outlive the coupling it was fetched for, or
    // let precipNoiseAt/renderDebug keep reading a previous frame's cells against a deck that never rebuilt them.
    const bool weather_built = (mWeatherDeck == 1) ? under_built : primary_built;
    if (!weather_built)
    {
        mStormInterest = SSStormCells::Interest();
        mStormCellCount = 0;
        mHeroFrame = SSDeckFrame::HeroFrame();
        mLineBand = SSStormCouple::LineBand(); // 8a: same staleness rule as mStormCellCount above
    }

    // <SS:Nexii> The ground shadow bakes from the PRIMARY deck alone - it is the weather's deck at storm altitude, the one standing between the sun and the ground. The under deck hangs below a sky build's platform and shadows nothing anyone stands on. Keyed inside, so this is a no-op almost every frame.
    if (!mPrimary.mPuffs.empty())
    {
        const LLVector3 shadow_cam = LLViewerCamera::getInstance()->getOrigin();
        const LLVector2 shadow_drift = SSAtmoEnvApplier::instance().cloudDriftMetres();
        bakeGroundShadow(mPrimary, shadow_cam.mV[VX] - shadow_drift.mV[0],
                         shadow_cam.mV[VY] - shadow_drift.mV[1]);
    }
    else
    {
        mShadowValid = false;
    }

    mStrikeLights.clear();
    for (const SSStrike& strike : SSLightning::getInstance()->strikes())
    {
        const F32 b = strike.mChannelBrightness * strike.mIntensity;
        if (b <= 0.004f) continue;

        mStrikeLights.push_back(LLVector4(strike.mOrigin.mV[VX],
                                          strike.mOrigin.mV[VY],
                                          strike.mOrigin.mV[VZ], b));
        if ((S32)mStrikeLights.size() >= SS_MAX_STRIKE_LIGHTS) break;
    }

    mOccGridDirty = true;

    mLastBuildMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
}

// Builds one deck's puffs from a resolved field state: same deterministic placement and shading for both decks, hashed with the deck's salt so their patterns differ. Moisture rides along because the
// noise map's hole-cutting is what moisture moderates.
void SSVolCloud::buildDeck(Deck& deck, const SSAtmoEnvCloudFieldState& field, F32 convection, F32 moisture, U32 salt,
                           const SSAtmoEnvTrack& track, F64 phase)
{
    // <SS:Nexii> The base map and its crossfade partner both fall back to the same built-in art when their keyframe is empty, so a fade between an authored texture and None - either direction - fades between real maps instead of cutting through the fallback logic.
    const bool stormy = field.mHasAnvil || convection > 0.6f;
    const LLUUID base_fallback(stormy ? SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS
                                      : SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS);
    deck.mTexture = field.mBaseTexture.notNull() ? field.mBaseTexture : base_fallback;
    deck.mTextureNext = field.mBaseTextureNext.notNull() ? field.mBaseTextureNext : base_fallback;
    deck.mTextureBlend = field.mBaseTextureBlend;

    deck.mDetail = field.mDetailTexture;
    deck.mDetailNext = field.mDetailTextureNext;
    deck.mDetailBlend = field.mDetailTextureBlend;
    deck.mNoise = field.mNoiseTexture;
    deck.mProfile = field.mProfileTexture;

    // <SS:Nexii> No authored map, the deck still gets the feature: a square procedural tile grown from the weather seed. Generated once and folded into the same grid cache an authored map would fill, so the builder, the precipitation gate and the shader's anvil carving all read one field whether it came from a texture or from the seed. Toggleable (SSAtmoCloudProceduralNoise) so a plain sky is always one switch away.
    static LLCachedControl<bool> proc_noise_setting(gSavedSettings, "SSAtmoCloudProceduralNoise", true);
    if (deck.mNoise.isNull())
    {
        if (proc_noise_setting)
        {
            ensureProceduralNoise(deck, salt);
        }
        else if (deck.mNoiseProcRaw.notNull())
        {
            deck.mNoiseProcRaw = nullptr;
            deck.mNoiseProcRef = nullptr;
            deck.mNoiseLuma.clear();
            deck.mNoiseW = 0;
            deck.mNoiseH = 0;
            deck.mNoiseSrcW = 0;
            deck.mNoiseSrcH = 0;
        }
    }

    deck.mBaseZ = field.mBaseHeightM;
    deck.mThicknessM = llmax(1.f, field.mThicknessM);
    deck.mAnvil = field.mAnvil;
    deck.mTextureMix = field.mTextureMix;
    deck.mPuffDensity = field.mPuffDensity;
    deck.mDetailScale = field.mDetailScale;
    deck.mDriftRate = field.mDriftRate;
    deck.mChurn = llclamp(field.mChurn, 0.f, 1.f);
    deck.mCoverage = field.mCoverage;
    deck.mSalt = salt;

    // The weather the deck was built under, kept so the debug overlay can replay the
    // builder's own column shaping instead of approximating it.
    deck.mConvection = convection;
    deck.mMoisture = moisture;

    // <SS:Nexii> The field density dial. Sub-puff index 0 is always placed, so lowering this thins the field toward one body per cell rather than opening holes in it - the holes are the gate's job, and the two must not be confused. Raising it fills the SAME cells more thickly, which is what "denser cloud" means when the grid is fixed. The far field is unaffected either way: past the squash knee the loop already collapses to one puff per cell, so this is a near-field cost that pays for near-field body. [interaction: SSAtmoCloudPuffBudget, which caps the result]
    static LLCachedControl<U32> per_cell_setting(gSavedSettings, "SSAtmoCloudPuffsPerCell", (U32)PUFFS_PER_CELL);
    const S32 puffs_per_cell = llclamp((S32)per_cell_setting, MIN_PUFFS_PER_CELL, MAX_PUFFS_PER_CELL);

    // The storm gloom rides the deck for the render pass's ss_gloom uniform - it used to be multiplied into every baked vertex colour, and it is one number per deck.
    deck.mGloom = field.mGloom;

    // <SS:Nexii> The noise map's resolved shaping, baked once per build so every consumer of the field - this builder, and the precipitation gate reading the deck from outside - runs the same numbers. The tile scales off the authored Noise Scale slider; the hole weight is what survives of the map's low end once moisture has lifted the floor over it and convection has kept the storm gaps open in what is left. The procedural fallback counts as a map here the same as an authored one.
    // <SS:Nexii> The tile is quantised to a whole number of cells (2048 -> 2080 = 8 x CELL_M, a 1.5% change no eye reads) so the drift accumulator can wrap on a span that is a multiple of BOTH the cell gate and this map (SSWindProfile::wrapSpanM, doc/atmo_magic_wind_profile.md section 4) - the old fmodf(drift, 1e6) wrap was a multiple of neither and repopped the field. The shader's ss_noise_tile uniform, the shadow key and precipNoiseAt all read this one number, so the lattice holds everywhere. Floored at one cell.
    deck.mNoiseTileM = (field.mNoiseTexture.notNull() || deck.mNoiseProcRaw.notNull())
        ? CELL_M * llmax(1.f, (F32)llround(SS_NOISE_TILE_M * llmax(0.05f, field.mNoiseScale) / CELL_M))
        : 0.f;
    const F32 nimbus = ss_smoothstep(SS_NIMBUS_LO, SS_NIMBUS_HI, moisture);
    deck.mNoiseHole = (1.f - nimbus) * (1.f - SS_STORM_GAP * llclamp(convection, 0.f, 1.f));

    // <SS:Nexii> The storm consolidation: high moisture DRIVING high convection is not the regime the map's carving is for - a rain cloud busy making weather is a large solid mass, not a shredded one. As the two climb together the tower ramp's window widens until most of the map passes it, so the deck's convection variety calms from pockets-and-spikes into the 1-3km connected cells of a thunderstorm, and the pocket suppression eases off with it. The window is baked onto the deck so the shader's carving and the precipitation gate run the same numbers as this builder.
    // <SS:Nexii> review 3b NEW-3: through SSStormCouple::consolidation - the V2 profile-outline debug view used to
    // respell this same smoothstep(0.55,0.85,moisture)*smoothstep(0.45,0.75,convection) product by hand a second
    // time; now there is exactly one implementation, and the overlay reads the BAKED, storm-delegated result
    // (deck.mStormEff, below) rather than recomputing this raw figure itself.
    const F32 storm = SSStormCouple::consolidation(moisture, convection);

    // <SS:Nexii> Phase 3 (doc/atmo_magic_storm_dynamics.md section 3): storm cells couple into whichever deck IS
    // weatherDeck() this frame - mPrimary or mUnder, never both, and never the OTHER deck - fetched ONCE per build
    // (the selection does not depend on the sample point, only on the frame's resolved cell set) rather than once
    // per grid cell. mStormInterest is claimed lazily and held for the singleton's life so SSStormCells::update()
    // actually resolves cells every frame (it early-outs to empty otherwise - see its own Interest note); update()
    // releases it again once weatherDeck() stops building (review S9). Slots at or past the returned count are
    // explicitly zeroed (radius <= 0 disables a slot to sampleAt) - fillUniforms only writes [0, picked), and a
    // shrinking cell count would otherwise leave a stale, still-active cell from a previous frame sitting in the
    // tail forever.
    const bool couple_storm = (&deck == weatherDeck());
    if (couple_storm)
    {
        if (!mStormInterest)
        {
            mStormInterest = SSStormCells::getInstance()->claim();
        }
        mStormCellCount = SSStormCells::getInstance()->fillUniforms(mStormCells, SSStormCouple::MAX_CELLS);
        for (S32 i = mStormCellCount; i < SSStormCouple::MAX_CELLS; ++i)
        {
            mStormCells[i] = SSStormCouple::CellUniform();
        }

        // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): the active squall line's deck coupling,
        // fetched next to fillUniforms above from the SAME scheduler instance/frame - AGENT frame, matching
        // mStormCells so lineField's px/py agree with sampleAt's world_x/world_y at every call site below.
        // LineBand() default (strength 0) whenever no line is alive; SSStormCouple::lineField reads that as
        // "disabled" by its own invariant. [interaction: SSStormCells::fillLineBand]
        SSStormCells::getInstance()->fillLineBand(mLineBand);

        // <SS:Nexii> Phase 4 fixup (F13, doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud
        // drift"): the hero's local frame shift inputs, built ONCE for this build from ONE source - the
        // SSStormCells::hero() record's id, matched to ITS OWN SSStormCells::ActiveCell in cells() (mIsHero) -
        // rather than motion/age from one scan (the old heroMotionAgeS) and centre/radius from a second,
        // independent one (mStormCells[0], selectSlots' own separate resolution). driftVel is the applier's
        // curve-resolved rate (never the eased SSAtmoMagic::mWind), the same vector cloudDriftMetres() integrates.
        // Left at HeroFrame()'s zero default (ageS 0) whenever there is no hero, which SSDeckFrame::heroShift/
        // foldFrameKey both read as "no shift" by their own invariants.
        mHeroFrame = SSDeckFrame::HeroFrame();
        if (mStormCellCount > 0)
        {
            SSStormCells* scheduler = SSStormCells::getInstance();
            const SSStormCells::Hero* hero = scheduler->hero();
            const SSStormCells::ActiveCell* hero_cell = nullptr;
            if (hero)
            {
                for (const SSStormCells::ActiveCell& c : scheduler->cells())
                {
                    if (c.mIsHero)
                    {
                        hero_cell = &c;
                        break;
                    }
                }
            }
            if (hero_cell)
            {
                const LLVector2 centre_agent = scheduler->toAgentXY(hero_cell->mCentre);
                const LLVector2& drift_vel_ms = SSAtmoEnvApplier::instance().driftVelocityMetresPerSec();
                mHeroFrame.motion    = SSDeckFrame::Vec2{hero_cell->mMotion.x, hero_cell->mMotion.y};
                mHeroFrame.driftVel  = SSDeckFrame::Vec2{drift_vel_ms.mV[0], drift_vel_ms.mV[1]};
                mHeroFrame.ageS      = hero_cell->mAge01 * hero_cell->mCandidate.mLifetimeS;
                mHeroFrame.centre    = SSDeckFrame::Vec2{centre_agent.mV[0], centre_agent.mV[1]};
                mHeroFrame.radius    = hero_cell->mRadiusM;

                // <SS:Nexii> F13: selectSlots' own invariant (ssstormcouplecore.h) puts the hero in mStormCells[0]
                // whenever one is present - verified here rather than merely asserted in prose, so a future change
                // to selectSlots/fillUniforms that breaks it fails loudly instead of silently placing the puff
                // loop's hero_influence weight (which reads mHeroFrame.centre/radius, not mStormCells[0]) against
                // a different cell than the one occupying uniform slot 0.
                llassert(mStormCells[0].radius > 0.f);
                llassert(fabsf(mStormCells[0].x - mHeroFrame.centre.x) < 1.f);
                llassert(fabsf(mStormCells[0].y - mHeroFrame.centre.y) < 1.f);
                llassert(fabsf(mStormCells[0].radius - mHeroFrame.radius) < 1.f);
            }
        }
    }
    // <SS:Nexii> review (self, phase 4): NO else-branch reset here - mHeroFrame is class-level like mStormCells/
    // mStormCellCount above (read by precipNoiseAt/bakeGroundShadow outside this Deck), and buildDeck runs once
    // per deck per frame; the non-coupled deck's build must not wipe the coupled deck's just-set frame behind its
    // back (build order is primary then under - see update()). Cleared only where mStormCellCount already is:
    // clear() and update()'s "!weather_built" branch.

    // <SS:Nexii> Phase 4 fixup (#5/F2/F8, doc/atmo_magic_wind_profile.md section 4): this deck's own O(z) shear
    // table, baked EVERY build regardless of storm coupling - the altitude shear is a wind-profile property of the
    // deck's own column, not a storm phenomenon. z0 is this deck's base (world); z1 is floor + the track's AUTHORED
    // dome height keyframe at this build's phase (never cirrusAltitudeMetres(), which reads the seasonal setting
    // and the LIVE deck's anvil-descended band - a per-client, per-frame value the table's own bake-once-per-build
    // cadence cannot track); groundZ is the track's own floor; params is SSAtmoEnvApplier::windProfileAt(track,
    // phase) - the PURE profile path (resolves the cube at this exact phase, never the live applier's mWindProfile,
    // which may belong to a different track/phase than the one this deck is building for). bakeShearTable's own
    // invariant makes o[0] zero at z0, so a deck whose base sits above the band (z1 <= z0) still bakes a valid,
    // clamped one-metre span. Left in this function's scope past this block, unbraced (F1, 2026-09-06 review): the
    // virga shaft skew below reuses this SAME params resolution for its own windAt call rather than re-resolving
    // the cube a second time for the same track/phase.
    const SSWindProfile::Params params = SSAtmoEnvApplier::windProfileAt(track, phase);
    const F32 z1 = track.mFloorZ + track.mCloudDome.mHeightM.valueAt(phase);
    deck.mShearTable = SSDeckFrame::bakeShearTable(deck.mBaseZ, z1, track.mFloorZ, params);

    // <SS:Nexii> review S3 Delegation (doc/atmo_magic_storm_dynamics.md section 3): while any coupled cell is
    // active, the deck-wide consolidation figure hands half its authority to the cells - SSStormCouple::
    // deckConsolidation(storm, cellsActive) - so the sky outside the cells is LESS consolidated than the plain
    // `storm` figure would leave it, which is what makes the discrete cells read against a less-saturated
    // background instead of a window the deck-wide ramp already maxed out. cellsActive is now a SMOOTH figure -
    // SSStormCouple::cellsActivity(mStormCells, mStormCellCount) - rather than a 0/1 step on "any radius > 0"
    // (review 3b NEW-8: the old step snapped the whole deck's consolidation window on a cell's birth frame); it
    // fades in and out with a cell's own lifecycle radius, 0 for the uncoupled deck, which always reads storm_eff
    // == storm bit-exactly per deckConsolidation's own invariant. Both mNoiseTowerLo/Hi AND conv_gain (below) are
    // derived from storm_eff rather than raw storm, so the delegation reaches the fragment stage and the
    // precipitation gate through the one baked window uniform, with no new replication (twin gap G4: the lerp
    // targets are SSStormCouple::STORM_TOWER_LO/HI, not a second hand-spelled 0.12f/0.60f).
    F32 cellsActive = 0.f;
    if (couple_storm)
    {
        // mStormCellCount/mStormCells are THIS build's fetch (just above) whenever couple_storm is true - never a
        // stale read of the other deck's build earlier/later in the same frame, since only the coupled deck fetches.
        cellsActive = SSStormCouple::cellsActivity(mStormCells, mStormCellCount);
    }
    const F32 storm_eff = SSStormCouple::deckConsolidation(storm, cellsActive);
    deck.mStormEff = storm_eff; // <SS:Nexii> review 3b NEW-3: baked so the V2 overlay's per-cell shaping helper (cellShapeAt, below) reads the same delegated figure this build actually used, not a re-derived raw storm.
    deck.mNoiseTowerLo = lerp(SS_TOWER_LO, SSStormCouple::STORM_TOWER_LO, storm_eff);
    deck.mNoiseTowerHi = lerp(SS_TOWER_HI, SSStormCouple::STORM_TOWER_HI, storm_eff);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    const F32 air_x = cam.mV[VX] - drift.mV[0];
    const F32 air_y = cam.mV[VY] - drift.mV[1];

    // <SS:Nexii> F3 (2026-09-06 review), reworded 8e-b REVIEW F3 with measured numbers: padded by
    // SSDeckLod::WALK_PAD_M, the SAME pad cellInWalk below tests cell membership against - without this the walk's
    // own bound stopped at FIELD_DRAW_M and never reached the ring between FIELD_DRAW_M and FIELD_DRAW_M +
    // WALK_PAD_M that cellInWalk's padded disc exists to admit. The ORIGINAL wording here called the pad "inert";
    // measured instead, the old bare 39-cell-radius square (FIELD_DRAW_M / CELL_M, no pad at all) already admitted
    // 6217 of its 6241 cells through cellInWalk's own circle test - the pad was not inert, it was ANISOTROPIC: its
    // effect is realised near the square's diagonals (where the circle's slack against the square corner is
    // largest) and near-absent along the axes, so in aggregate the walk reads as "effectively a square" even
    // though the corner rejection is real and cellInWalk is genuinely doing work. HONEST COST, current numbers:
    // 8e-b's own review (N2, ssdecklodcore.h WALK_PAD_M) drops the virga skew term from the pad, so cell_radius
    // falls from 54 to 50 cells - about 26% more admitted cells than the old bare 39-cell square, not the "square
    // vs ring" cliff the first draft of this comment implied. The walk below is still a SQUARE of side
    // 2*cell_radius+1 cells, so growing this radius grows the walked cell count with its SQUARE, not with the ring
    // cellInWalk actually admits - cellInWalk (the very first thing the loop body does, ahead of any gate-hash/
    // presence work) rejects the square's four corners cheaply, but every cell inside the square still pays for
    // the coordinate math and the distance check itself. Not free; measure mLastBuildMS on a build rather than
    // assume the corner rejection makes it so.
    const S32 cell_radius = llceil((FIELD_DRAW_M + SSDeckLod::WALK_PAD_M) / CELL_M);
    const S32 cx0 = llfloor(air_x / CELL_M);
    const S32 cy0 = llfloor(air_y / CELL_M);

    const F32 base_radius = CELL_M * PUFF_CELL_FRACTION * 0.5f;
    const F32 size_gain = 1.f + PUFF_THICKNESS_GAIN * (field.mThicknessM / 500.f);

    const LLVector3 light_dir = mLightDir;
    const F32 beam = mBeam;
    const F32 shade_th = SSDeckShade::thicknessTerm(field.mThicknessM, field.mCoverage); // one layer, one optical thickness term
    static LLCachedControl<bool> shade_calib(gSavedSettings, "SSAtmoCloudShadeCalibration", true); // <SS:Nexii> The shade calibration A/B (ssdeckshadecore.h Calibration): TRUE is the retuned constant set, FALSE the pre-retune one bit-identically (V:/Scratch/atmo/tests/deckshadecore.cpp pins it). ONE read, passed to all three SSDeckShade call sites in this function, so the veil, the fine puffs and the Tier B bodies can never disagree about which calibration they are wearing. [interaction: SSAtmoCloudLightVariant, the same kind of in-build A/B]

    // <SS:Nexii> The base veil's structural shading, resolved here rather than per fragment: the shade a puff at the deck's floor would wear, run through the same formulas the puff loop below uses - the facing term at a representative low up, the exponential shade through the layer from that height, the beam gate. The sheet is a fragment of the same body of cloud as the puffs, so it wears the same form a puff in its place would (the light itself is the vertex stage's, shared by construction), and the blend at the boundary is a lighting match rather than a hope.
    {
        // <SS:Nexii> 6d review fix 3: SSDeckShade::veilForm (ssdeckshadecore.h) - the ONE shading formula site, with
        // the veil's representative depths baked in as core constants; V:/Scratch/atmo/tests/deckshadecore.cpp pins
        // it against the old inline block within 1e-6 (the old facing term's multiplication order differed by a last bit).
        const F32 sun_z = llclamp(light_dir.mV[VZ], -1.f, 1.f);
        deck.mSheetForm = SSDeckShade::veilForm(sun_z, SSDeckShade::thicknessTerm(field.mThicknessM, field.mCoverage), beam, shade_calib);
        // The inset: just off the deck's floor, deep enough to sit inside the puffs' base fade,
        // shallow enough that the sheet reads as the deck's underside and not as a second layer.
        // The flat 16 m lift sits it higher in the fade, so the veil clears the puffs' bottom dissolve instead of hugging the floor.
        deck.mSheetZ = field.mBaseHeightM + 16.f + llclamp(field.mThicknessM * 0.08f, 30.f, 90.f);
        deck.mSheetAlpha = llclamp(0.30f + 0.45f * field.mCoverage, 0.f, 0.7f);
    }

    F64 dist_sum = 0.0;

    // <SS:Nexii> Distant rain shafts (ssvirgacore.h, doc/atmo_magic_far_clouds.md section 3): gated on couple_storm
    // - shafts hang from whichever deck IS weatherDeck() this build, the same deck precipNoiseAt/precipBaseZ/
    // precipTopZ always name, so a curtain never hangs off a deck that is not the one precipitation actually falls
    // from. Driven by the resolved precipitation intensity (SSAtmoMagic::precipitation(), the same eased,
    // world-state figure ssprecipitation.cpp's own spawn gate reads via atmo->precipitation() - "ships first
    // against the global precip scalar" per the design, never a per-cell storm read yet), never recomputed per
    // cell. shaft_r2 is the particle sim's own TIER_SHEETS radius (SSPrecipSim::tierBands' out_hi for that tier,
    // after the LOD dial - the same figure ssprecipitation.cpp's tierRadii resolves to r2) so the handoff shares
    // one boundary with the particle rain instead of respelling it. Candidates are only collected here - the
    // keepHash trim and card emission happen once, after the whole cell walk (buildDeck's own
    // qualifying-cell-is-camera-free contract - see ShaftCandidate above).
    // <SS:Nexii> F4/F6: shafts_active also requires the Weather Influence row's MASTER enable (mEnabled) - the
    // per-row mDistantRainEnabled/mDistantRainStrength dials are meaningless once the whole influence system is
    // off, same as every other row in this struct. Once shaft_r2 is resolved below, shafts_active is narrowed a
    // second time to require shaft_r2 > 0 (F6) - a zero/negative handoff radius has no meaning for
    // SSVirga::handoff's skipR/bandEnd maths and would either hide every shaft (skipR 0, bandEnd == HANDOFF_BAND_M
    // only) or divide by a degenerate band, so the feature simply does not run rather than guess.
    const SSAtmoEnvWeatherInfluence& weather_influence = track.mWeatherInfluence;
    bool shafts_active = couple_storm && weather_influence.mEnabled && weather_influence.mDistantRainEnabled
                        && weather_influence.mDistantRainStrength > 0.f;
    F32 shaft_precip = 0.f;
    F32 shaft_ground_z = 0.f;
    F32 shaft_r2 = 0.f;
    F32 shaft_fall_speed = 1.f;
    F32 shaft_base_agl = 0.f;
    std::vector<ShaftCandidate> shaft_candidates;
    if (shafts_active)
    {
        shaft_precip = SSAtmoMagic::getInstance()->precipitation();
        shaft_ground_z = SSAtmoEnvApplier::instance().windProfileGroundZ();

        F32 in_lo, in_hi, out_lo;
        SSPrecipSim::tierBands(TIER_SHEETS, SSAtmoMagic::getInstance()->preset(), in_lo, in_hi, out_lo, shaft_r2);

        shafts_active = shaft_r2 > 0.f; // F6

        // <SS:Nexii> F1 (2026-09-06 review), 8e-b PROFILE SKEW (ssvirgacore.h profileSkewM, doc/atmo_magic_phase8_
        // show.md section 3b): FRAME RULE - geometry placement reads the CURVE-RESOLVED wind PROFILE
        // (SSAtmoEnvApplier::windProfileAt(track, phase), i.e. `params`), never the applier's own live
        // mWindProfile/windAt(z) overload (it reads gSavedSettings and this client's own deck top), never
        // SSAtmoMagic::mWind, and never the flowmap - the curtain skew reuses `params`, the SAME
        // windProfileAt(track, phase) resolution the shear table just above already baked from, rather than a
        // second, independent (and per-client-divergent) resolve. shaft_base_agl is deck.mBaseZ - track.mFloorZ -
        // the same baseAgl bakeShearTable derives from these two values above - since SSWindProfile::windAt takes
        // height ABOVE GROUND, not world Z; profileSkewM integrates windAt(z, params) over the fall itself (8e-b:
        // "segment the virga in parts and follow the wind profile"), so no single windAt sample is taken here any
        // more - cardGeom below calls profileSkewM per card, at that card's own altitude. This is deliberately NOT
        // the same accessor render()'s streak frame direction reads (that is still the live, per-client
        // windAt(mPrimary.mBaseZ) - geometry and that cosmetic streak are allowed to diverge; see ssvirgacore.h
        // skewOffsetM's own F10 note). fallSpeed is the ACTIVE precipitation preset's own mFallSpeed - the same
        // preset tierBands above already resolved this shaft's handoff radius from, so the fall-time rule (wind x
        // dz / v_fall) uses the same v_fall the particle rain's landing shift does.
        shaft_base_agl = deck.mBaseZ - track.mFloorZ;
        shaft_fall_speed = SSAtmoMagic::getInstance()->preset().mFallSpeed;
    }

    // <SS:Nexii> F11: shafts are base-anchored (SSVirga::BURIED, like the veil) so their placement takes NO O(z)
    // shear lean - a default-constructed ShearTable's o[] entries are all the zero vector (ssdeckframecore.h's
    // ShearTable ctor), so SSDeckFrame::shearTableAt returns zero for ANY z argument. This is the "documented
    // base-anchored overload" the contract calls for: placeWorld itself still owns the +drift+heroShift terms, so
    // no call site respells c + drift + S by hand.
    static const SSDeckFrame::ShearTable SS_VIRGA_NO_SHEAR;

    // <SS:Nexii> LOD phase: this build's own tally of SSDeckLod::subsAt(dist, dial) summed over every occupied
    // cell - the V3 debug view's "LOD-predicted" figure, read back against the actual placed/kept puff count
    // (deck.mPuffs.size()) and the budget dial for its legend row. Reset per build so a stale figure from a
    // previous frame's field never survives into this one's.
    deck.mLodSubsTally = 0;
    deck.mLodCellTally = 0;
    deck.mTierBCount = 0;

    // <SS:Nexii> F2 (2026-09-06 review): the render pass's ss_shaft_embed uniform source - reset to 0 every build
    // so a deck whose shaft path does not run this frame (feature off, not the weather deck, no qualifying cells)
    // never uploads a stale embed span left over from a build where it did; set for real only inside the
    // shafts_active card-emission block below.
    deck.mShaftEmbedTopZ = 0.f;
    deck.mShaftGroundZ = 0.f;

    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT, doc/atmo_magic_far_clouds.md section 2 step 2 Tier B):
    // the macro tier's per-build accumulator, keyed by macro cell (macroIndex(cx), macroIndex(cy)) packed into one
    // U64 - the SAME packing idiom ShaftCandidate::cellId already uses below. Filled while walking the fine cell
    // loop (never resampled - "no fourth gate": occupancy below is a straight tally of the SAME gate verdict this
    // loop already computed for the fine tier) and only for macro cells whose block centre sits beyond
    // TIER_B_M - TIER_BLEND_M/2 of the camera (macro_block_lo below) - nearer blocks never reach Tier B and are never
    // added to this map, so it stays small (far-ring cells only). Local to this one buildDeck call.
    struct MacroAccum
    {
        U8  mVerdicts[SSDeckMacro::MACRO_CELLS * SSDeckMacro::MACRO_CELLS] = {};
        F32 mUps[SSDeckMacro::MACRO_CELLS * SSDeckMacro::MACRO_CELLS] = {};
        F32 mCellHeights[SSDeckMacro::MACRO_CELLS * SSDeckMacro::MACRO_CELLS] = {};
        F32 mCoreness[SSDeckMacro::MACRO_CELLS * SSDeckMacro::MACRO_CELLS] = {};
        S32 mCount = 0;
        S32 mMcx = 0;
        S32 mMcy = 0;
        F32 mBlockDist = 0.f; // the block's horizontal, drift-only centre distance - the ONE scalar both crossfade halves read
    };
    std::unordered_map<U64, MacroAccum> macro_accum;
    const F32 macro_block_lo = SSDeckMacro::TIER_B_M - SSDeckMacro::TIER_BLEND_M * 0.5f;

    for (S32 dy = -cell_radius; dy <= cell_radius; ++dy)
    {
        for (S32 dx = -cell_radius; dx <= cell_radius; ++dx)
        {
            const S32 cx = cx0 + dx;
            const S32 cy = cy0 + dy;

            // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT), corrected 2026-09-05 (doc/atmo_magic_far_clouds.md
            // phase 6a outcome): reject the square walk's corners - outside FIELD_DRAW_M + WALK_PAD_M of the
            // AIR-frame camera cell (cx0, cy0) is defined from - before any hash, presence or hero-influence work
            // runs for this cell. SSDeckLod::cellInWalk pads the plain FIELD_DRAW_M circle by WALK_PAD_M, the
            // farthest an ORDINARY PUFF is placed from this cell's centre - the O(z) shear lean, the hero's frame
            // shift and this sub-puff's own jitter (three terms; 8e-b REVIEW N2, 2026-09-06: a virga curtain's own
            // wind-fall skew, SSVirga::SKEW_CAP_M, used to be a fourth term here but is DELETED - a skewed shaft
            // card from a candidate this walk never even collects draws nothing, so the pad owes the skew nothing;
            // see ssdecklodcore.h WALK_PAD_M's own comment) - so a cell just outside FIELD_DRAW_M whose puffs
            // still lean inward is not starved before it gets the chance to place them; this DOES change some
            // cells' fate versus the plain circle - membership is tested on the padded walk, and it is edgeFade
            // below (zero at DECK_EDGE_M, well inside this padded radius) that owns visibility, not this gate.
            // There is no per-puff FIELD_DRAW_M cull left downstream for this to mirror (removed - see the
            // puff-alpha comment near dist_sq below).
            if (!SSDeckLod::cellInWalk((F32)cx * CELL_M + CELL_M * 0.5f, (F32)cy * CELL_M + CELL_M * 0.5f, air_x, air_y))
            {
                continue;
            }

            const F32 gate_raw = clusterUnit(cx, cy, salt) * CLUSTER_WEIGHT
                               + hashUnit(cx, cy, 1u + salt) * (1.f - CLUSTER_WEIGHT);

            // <SS:Nexii> Phase 4 fixup (#1/#2, doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud
            // drift"): the cell's WORLD centre (air cell centre + drift), computed ONCE per cell through
            // SSStormCouple::samplePointM ahead of the gate below - it feeds both the hero influence weight AND the
            // storm sample. The hero's own PRODUCER rule (ssdeckframecore.h): S is evaluated at this UNSHIFTED
            // quantized centre - the cell the loop is building, never the shifted one - so the influence weight
            // matches the cell the builder is actually placing puffs for. Zero whenever mHeroFrame has no age (no
            // hero), which makes hero_shift_m the zero vector.
            F32 world_x, world_y;
            SSStormCouple::samplePointM((F32)cx * CELL_M + CELL_M * 0.5f, (F32)cy * CELL_M + CELL_M * 0.5f,
                                         CELL_M, drift.mV[0], drift.mV[1], world_x, world_y);
            const F32 hero_influence = (couple_storm && mHeroFrame.ageS > 0.f)
                ? SSStormCouple::influence(mHeroFrame.centre.x, mHeroFrame.centre.y, mHeroFrame.radius, world_x, world_y)
                : 0.f;
            const SSDeckFrame::Vec2 hero_shift_m = SSDeckFrame::heroShift(mHeroFrame, hero_influence);

            // <SS:Nexii> Phase 4 fixup (#1/#2): the PRODUCER reads the pattern (gate hash, presence, n_map) at the
            // PLAIN, unshifted cell centre - ssdeckframecore.h's producer/observer rule. gateAir is an OBSERVER
            // transform (air - S) for a point the fragment/bake/precip side is classifying from outside; the
            // builder IS the producer for this cell and must not shift its own read, or the cell it hashes and the
            // cell whose pattern it reads would disagree the moment a hero is near. The gate (cx,cy) and gate_raw's
            // hash stay on base drift exactly as the frame contract requires (nothing repops); the hero's S only
            // ever reaches this cell's puffs through placeWorld below.
            F32 presence = 1.f;
            F32 tower = 0.f;
            F32 raw_n = 0.f;
            noiseFieldAt(deck, (F32)(cx + 0.5) * CELL_M, (F32)(cy + 0.5) * CELL_M, presence, tower, raw_n);

            const F32 gate = gate_raw + (1.f - gate_raw) * (1.f - presence);

            // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT, doc/atmo_magic_far_clouds.md section 2 step 2
            // Tier B): the macro block this fine cell belongs to, and that BLOCK's own centre distance from the
            // true camera - used only to decide (a) whether this cell's gate verdict is worth accumulating for
            // Tier B (macro_block_lo, the TIER_B_M - TIER_BLEND_M/2 rail) and (b) the Tier A/B crossfade weight
            // every fine cell in the SAME block shares, so a block never straddles two different wA values between
            // its own sibling fine cells - a per-fine-cell distance (lod_dist, computed further below for the
            // established fine-tier ramps and left untouched) would let siblings 260m apart pick different weights
            // right at the boundary. macro_block_world is the block's plain air centre + drift only (no hero
            // shift - the coarse crossfade/eligibility test does not need the small hero displacement the body's
            // own final placement applies after the fine loop, below).
            const S32 macro_cx = SSDeckMacro::macroIndex(cx);
            const S32 macro_cy = SSDeckMacro::macroIndex(cy);
            const F32 macro_block_air_x = ((F32)macro_cx + 0.5f) * SSDeckMacro::MACRO_M;
            const F32 macro_block_air_y = ((F32)macro_cy + 0.5f) * SSDeckMacro::MACRO_M;
            F32 macro_block_world_x, macro_block_world_y;
            SSStormCouple::samplePointM(macro_block_air_x, macro_block_air_y, SSDeckMacro::MACRO_M,
                                         drift.mV[0], drift.mV[1], macro_block_world_x, macro_block_world_y);
            const F32 macro_bdx = macro_block_world_x - cam.mV[VX];
            const F32 macro_bdy = macro_block_world_y - cam.mV[VY];
            const F32 macro_block_dist = sqrtf(macro_bdx * macro_bdx + macro_bdy * macro_bdy);
            const bool macro_in_scope = macro_block_dist > macro_block_lo;
            F32 tier_wA = 1.f, tier_wB = 0.f;
            if (macro_in_scope)
            {
                SSDeckMacro::tierWeights(macro_block_dist, tier_wA, tier_wB);
            }

            MacroAccum* macro_slot = nullptr;
            if (macro_in_scope)
            {
                const U64 macro_key = ((U64)(U32)macro_cx << 32) | (U64)(U32)macro_cy;
                macro_slot = &macro_accum[macro_key];
                macro_slot->mMcx = macro_cx;
                macro_slot->mMcy = macro_cy;
                macro_slot->mBlockDist = macro_block_dist;
            }
            const S32 macro_slot_cap = (S32)(sizeof(MacroAccum::mVerdicts) / sizeof(MacroAccum::mVerdicts[0]));

            if (gate > field.mCoverage)
            {
                // <SS:Nexii> LOD phase 6d: the fine gate's own MISS verdict, tallied into the macro accumulator so
                // SSDeckMacro::occupancy sees the SAME denominator the fine tier's gate produced right here - no
                // fourth gate, no resampling.
                if (macro_slot && macro_slot->mCount < macro_slot_cap)
                {
                    macro_slot->mVerdicts[macro_slot->mCount] = 0;
                    ++macro_slot->mCount;
                }
                continue;
            }

            const F32 coreness = llclamp(
                (field.mCoverage - gate) / llmax(field.mCoverage, 0.01f), 0.f, 1.f);

            // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): the cell's own camera distance, computed ONCE here
            // and reused by every sub-puff's subsAt/subAlpha/keepPuff/thinAlphaComp call below - a per-cell
            // granularity by design (the contract's own words: "LOD here is a function of CAMERA DISTANCE ONLY").
            // Horizontal only (world_x/world_y have no z - height is a per-sub quantity computed later in this
            // loop), from the TRUE camera `cam`, not the air-frame origin the walk is centred on - the same
            // reference frame the per-puff FIELD_DRAW_M cull below already measures against. subs is not used to
            // truncate the sub loop (subAlpha's own blend band is the only thing allowed to fade a sub out, so the
            // count itself never pops per the contract) - it is only tallied for the V3 debug legend's live count.
            const F32 cell_dx = world_x - cam.mV[VX];
            const F32 cell_dy = world_y - cam.mV[VY];
            const F32 lod_dist = sqrtf(cell_dx * cell_dx + cell_dy * cell_dy);
            const S32 lod_subs = SSDeckLod::subsAt(lod_dist, puffs_per_cell);
            deck.mLodSubsTally += lod_subs;
            deck.mLodCellTally += 1;

            // <SS:Nexii> Phase 3 coupling (doc/atmo_magic_storm_dynamics.md section 3): the storm field at this
            // cell's WORLD centre, sampled once per cell, never per sub-puff. review 3b NEW-6 (doc S3 "Sample point
            // lockstep"): agreement with the fragment stage's own ss_storm_sampleAt is PER-CELL, not per-fragment -
            // a sub-puff's jitter (up to CELL_M*0.4 off this cell's centre) can put its fragments over a
            // NEIGHBOURING cell's storm sample at the shader's own, continuous world position, while this builder
            // shapes every sub-puff in the cell from the one sample taken here. Stated, not hidden: carrying the
            // owning cell index on a vertex channel so the fragment stage could re-read this exact sample instead
            // of its own would close the residual, and is deferred.
            SSStormCouple::Sample storm_sample;
            if (couple_storm)
            {
                if (mStormCellCount > 0)
                {
                    storm_sample = SSStormCouple::sampleAt(mStormCells, mStormCellCount, world_x, world_y);
                }
                // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): the squall line's wall/shelf
                // folded in right after the discrete-cell sample, at the SAME world point - BUILDER call site.
                // Independent of mStormCellCount (a line's members may outnumber the MAX_CELLS uniform slots).
                SSStormCouple::applyLineBand(storm_sample, SSStormCouple::lineField(mLineBand, world_x, world_y), SSSquall::LINE_ANVIL_FRAC);
            }

            // <SS:Nexii> review S12: tower is ALWAYS derived through SSStormCouple::towerFromMap - never left at
            // noiseFieldAt's own internal ss_smoothstep result - so there is exactly one tower implementation for
            // the builder to swap between, not two that happen to agree when storm_sample.boost is 0.
            // storm_sample.boost defaults to 0 off-cell/uncoupled, and towerFromMap(n, lo, hi, 0) == smoothstep(lo,
            // hi, n) by the core's own invariant, so this is not claimed bit-identical to the old direct read
            // without a harness test pinning it - only that the two expressions are the same maths. `n < 0` reads
            // raw_n 0, and towerFromMap(0, lo, hi, boost) with 0 <= lo is 0 same as the un-widened ramp would give,
            // so an unready map is unaffected.
            tower = SSStormCouple::towerFromMap(raw_n, deck.mNoiseTowerLo, deck.mNoiseTowerHi, storm_sample.boost);

            // <SS:Nexii> Distant rain shafts (ssvirgacore.h): qualification off the SAME in-loop presence/tower
            // this cell's puffs are about to be shaped from - drive is precip_intensity x presence x tower
            // (SSVirga::drive), and qualifies scales the threshold down as the Weather Influence "Distant rain"
            // strength rises. This is the pure, camera-free half of the contract: cellInWalk above already bounded
            // which cells reach here (camera-only pad, same as puffs), but nothing past that gate depends on
            // distance - the candidate is only COLLECTED here; SSVirga::keepHash's stable trim and the actual card
            // emission happen once, after the whole cell walk (see shaft_candidates' own comment).
            if (shafts_active)
            {
                const F32 shaft_drive = SSVirga::drive(shaft_precip, presence, tower);
                if (SSVirga::qualifies(shaft_drive, weather_influence.mDistantRainStrength))
                {
                    // <SS:Nexii> F11: PRODUCER placement through SSDeckFrame::placeWorld itself (the PLAIN,
                    // unshifted cell centre + drift + hero shift + a ZERO O(z) lean - see SS_VIRGA_NO_SHEAR
                    // above), never a respelled c + drift + S sum - the same call every ordinary puff below
                    // makes, just with the base-anchored zero table instead of deck.mShearTable.
                    const SSDeckFrame::Vec2 shaft_placed = SSDeckFrame::placeWorld(
                        SSDeckFrame::Vec2{(F32)cx * CELL_M + CELL_M * 0.5f, (F32)cy * CELL_M + CELL_M * 0.5f},
                        SSDeckFrame::Vec2{drift.mV[0], drift.mV[1]}, hero_shift_m, deck.mBaseZ, SS_VIRGA_NO_SHEAR);

                    // <SS:Nexii> 8e-b REVIEW N2 (decided, 2026-09-06): a candidate at or beyond DECK_EDGE_M can
                    // never emit a card - every emitted card's alpha chain multiplies edgeFade of THIS SAME
                    // candidate distance (F6, see the alpha line below) - yet before this filter it was still
                    // counted in keepHash's n, diluting the MAX_SHAFTS budget across cells that could never be
                    // seen (measured: visible share of the kept set 72% -> 50% after the padded walk widened the
                    // candidate ring). Pre-filtered here, before push_back, the same class of camera-only LOD
                    // bound cellInWalk already applies to cell membership - never a world-state gate.
                    const F32 cand_dx = shaft_placed.x - cam.mV[VX];
                    const F32 cand_dy = shaft_placed.y - cam.mV[VY];
                    const F32 cand_horiz = sqrtf(cand_dx * cand_dx + cand_dy * cand_dy);
                    if (SSDeckLod::edgeFade(cand_horiz) > 0.f)
                    {
                        ShaftCandidate cand;
                        cand.x = shaft_placed.x;
                        cand.y = shaft_placed.y;
                        cand.drive = shaft_drive;
                        cand.cellId = ((U64)(U32)cx << 32) | (U64)(U32)cy;
                        cand.hashKey = hashCell(cx, cy, salt + SS_VIRGA_HASH_SALT);
                        shaft_candidates.push_back(cand);
                    }
                }
            }

            // <SS:Nexii> The tower shaping. Convection decides how much say the map gets over heights at all - a stable sky keeps every column at the cluster's own height and only the holes differ - and past that the map decides which columns RISE: tower-weighted cells keep the full climb to the lid while the pockets between them are held low, which is what stands a cumulonimbus tower up in the gaps of its own field. The stretch to the towers comes free with the same stroke: a column the map marks high spans the layer's whole convective thickness, base to lid, because nothing pulls it back down. conv_gain reads storm_eff (review S3 Delegation), not raw storm, so the cells' authority over height is delegated the same window the tower ramp was.
            // <SS:Nexii> review 3b NEW-3: through cellShapeAt, the one helper the V2 profile-outline debug view
            // also calls (below), so conv_gain/cell_height/cell_anvil can never drift between what is actually
            // built and what the overlay draws as its explanation of it. The anvil's own storm term
            // (storm_sample.anvil, 0 off-cell) still joins the max via SSStormCouple::anvilWeight inside the
            // helper rather than a hand-respelled llmax.
            const SSStormCouple::CellShape shape = SSStormCouple::cellShapeAt(convection, storm_eff, tower, coreness, field.mAnvil, storm_sample.anvil);
            const F32 cell_height = shape.cell_height;
            const F32 cell_anvil = shape.cell_anvil;

            // <SS:Nexii> Phase 4 fixup (#7/F9, doc/atmo_magic_storm_dynamics.md section 3 "Overshooting top vs the
            // lid cut"): the overshoot bonus goes to sub-puff 0 ALWAYS, never a camera-dependent "tallest survivor"
            // choice - the phase-3c tallest-sub probe (which read the camera to pick a survivor, and needed its
            // own squash guard) is deleted entirely. Sub 0 is the one sub-puff every cell always keeps regardless
            // of the squash knee (`squashed && sub > 0` below only culls sub > 0) and of camera distance (LOD phase,
            // 2026-09-05: SSDeckLod::keepPuff never thins sub 0, and the builder's pre-gate above and the puff loop
            // below carry no distance cull that could drop it either - the per-puff FIELD_DRAW_M cull that used to
            // sit near dist_sq below is gone, edgeFade owns the edge instead). So sub 0 is always placed for every
            // gated cell (design rule, not a claim any single test pins end to end - decklodcore.cpp's keepPuff_
            // sub_zero_always_kept test pins the thinning half of it), which is exactly why it is the only choice
            // that can carry the overshoot lift, and it makes which puff overshoots a pure function of the cell
            // alone rather than of where the camera happens to be standing.
            // <SS:Nexii> NEW-4 fix (2026-09-05, doc/atmo_magic_storm_dynamics.md section 3): sub 0 also needs to
            // BE the cell's tallest sub-puff, not merely the one that keeps the overshoot bonus - otherwise the
            // lid cut (SSStormCouple::lidTopM, which rises to meet whatever height the overshoot landed on) could
            // sit above empty air while the cell's actual tallest puff, some other sub, was culled at distance.
            // Fixed by a DETERMINISTIC SWAP, still world-state and camera-free: up_cell_arr below is the same
            // hashUnit(cx, cy, 4u + sub_salt) every sub already computes, read once here (before the loop, so
            // nothing inside it changes) to find its own argmax; only the up_cell VALUE trades places between
            // slot 0 and the argmax slot - jitter (jx/jy) and every other per-sub hash stay on their own
            // sub_salt - so the cell's multiset of heights is unchanged and only WHICH physical puff is "sub 0"
            // (and therefore keeps the overshoot lift and survives the squash cull) changes.
            // NEW-E, stated (pre-existing, not a bug this pass fixes): puffs_per_cell is SSAtmoCloudPuffsPerCell,
            // a gSavedSettings dial (see its read above), so the probe loop below runs a different length on
            // different clients - the sub SET a cell hashes, and therefore which sub_salt's hashUnit wins the
            // argmax and becomes "sub 0", is per-client. Two viewers looking at the same cell can disagree on
            // which physical puff carries the overshoot lift; this was already true of the un-swapped hash before
            // NEW-4 and is orthogonal to the swap itself, which only changes WHICH already-computed sub is 0.
            S32 tallest_sub = 0;
            F32 tallest_up = hashUnit(cx, cy, 4u + salt);
            for (S32 probe = 1; probe < puffs_per_cell; ++probe)
            {
                const F32 candidate = hashUnit(cx, cy, 4u + salt + (U32)probe * 8u);
                if (candidate > tallest_up)
                {
                    tallest_up = candidate;
                    tallest_sub = probe;
                }
            }

            // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): the fine gate's own HIT verdict, plus this
            // cell's sub-0 up_cell hash (tallest_up - the SAME value sub 0's own placement below uses, after the
            // NEW-4 swap above) and its cellShapeAt outputs (cell_height, coreness) - tallied into the SAME macro
            // slot the miss branch above would have used, so SSDeckMacro::occupancy/bodyUp average over exactly
            // the (cx,cy) set the fine gate actually walked, never a resampled one.
            if (macro_slot && macro_slot->mCount < macro_slot_cap)
            {
                const S32 slot_i = macro_slot->mCount;
                macro_slot->mVerdicts[slot_i] = 1;
                macro_slot->mUps[slot_i] = tallest_up;
                macro_slot->mCellHeights[slot_i] = cell_height;
                macro_slot->mCoreness[slot_i] = coreness;
                ++macro_slot->mCount;
            }

            if (tier_wA <= 0.f)
            {
                // <SS:Nexii> LOD phase 6d: beyond TIER_B_M + TIER_BLEND_M/2 (wA == 0) no fine puff is placed for
                // this cell at all - Tier B's merged body (emitted once per macro cell after the whole fine loop,
                // below) carries the block from here - but the gate was still evaluated and just tallied above,
                // exactly as the contract requires ("STILL evaluate the fine gate for the accumulator").
                continue;
            }

            for (S32 sub = 0; sub < puffs_per_cell; ++sub)
            {
                // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): deterministic far thinning of sub > 0 puffs -
                // sub 0 is unconditionally kept (SSDeckLod::keepPuff's own invariant; the constraint that every
                // gate-occupied cell always draws at least one body). Ahead of the jitter/height/placement work
                // below so a thinned-out sub costs nothing beyond the hash. Uses lod_dist, the cell-level distance
                // computed once above, and buildDeck's own deck salt - the SAME salt the gate hash above reads,
                // through a different hash chain (SSAtmoNoise::combine), so this thinning decision cannot alias
                // the gate's own pattern.
                if (sub > 0 && !SSDeckLod::keepPuff(cx, cy, sub, salt, lod_dist))
                {
                    continue;
                }

                const U32 sub_salt = salt + (U32)sub * 8u;

                const F32 jx = (hashUnit(cx, cy, 2u + sub_salt) - 0.5f) * CELL_M * 0.8f;
                const F32 jy = (hashUnit(cx, cy, 3u + sub_salt) - 0.5f) * CELL_M * 0.8f;

                // NEW-4: the swap - sub 0 reads the argmax's hashed height, the argmax sub (if not already 0)
                // reads sub 0's own hashed height back; every other sub is untouched.
                F32 up_cell = hashUnit(cx, cy, 4u + sub_salt);
                if (sub == 0) up_cell = tallest_up;
                else if (sub == tallest_sub) up_cell = hashUnit(cx, cy, 4u + salt);
                const F32 up = up_cell * cell_height;

                LLVector3 pos;
                pos.mV[VZ] = field.mBaseHeightM + up * field.mThicknessM;

                // <SS:Nexii> Phase 4 fixup (#7/F9): the overshooting top, added ONLY to the cell's sub-puff 0 -
                // ALWAYS, never a "tallest survivor" argmax (see the deleted probe above) - as a straight height
                // bonus past the column's own ceiling - the real thing punches through the tropopause above
                // whatever height the layer's own shaping already gave it, so this rides on top of `up`/
                // `cell_height` rather than folding into them, and BEFORE placeWorld below so the O(z) lean it
                // folds in is the overshot puff's OWN altitude, not its column's un-lifted one. review 3b NEW-4: no
                // CPU-side lid uniform is needed for this to survive the fragment stage's lid cut - the shader
                // derives its own lid altitude from ss_layer_thick and its own storm sample via
                // SSStormCouple::lidTopM's GLSL twin (topZ + overshootBonusM), the SAME formula this line calls, so
                // the lid rises to meet exactly the puff this lifts rather than cutting it off at the un-lifted ceiling.
                if (sub == 0 && storm_sample.overshoot > 0.f)
                {
                    // <SS:Nexii> phase-3c fix F4: deck.mThicknessM (llmax(1, field.mThicknessM)), matching the
                    // shader's ss_layer_thick uniform and the V2 overlay's own overshootBonusM call - not the raw,
                    // unclamped field.mThicknessM this line used to read.
                    pos.mV[VZ] += SSStormCouple::overshootBonusM(deck.mThicknessM, storm_sample.overshoot);
                }

                // <SS:Nexii> Phase 4 fixup (#1/#2, doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs
                // cloud drift" / doc/atmo_magic_wind_profile.md section 4): PRODUCER placement - pattern-cell
                // centre (+ this sub's jitter) + drift + the hero's rigid local shift S + the O(z) shear lean at
                // this puff's OWN final altitude, all folded by SSDeckFrame::placeWorld itself (no hand-added
                // shearTableAt call at the placement - placeWorld owns the O(z) term so no call site adds it by
                // hand, per ssdeckframecore.h). Identity (+jitter+drift only) whenever hero_shift_m and the table
                // are both zero.
                const SSDeckFrame::Vec2 placed = SSDeckFrame::placeWorld(
                    SSDeckFrame::Vec2{(F32)cx * CELL_M + CELL_M * 0.5f + jx, (F32)cy * CELL_M + CELL_M * 0.5f + jy},
                    SSDeckFrame::Vec2{drift.mV[0], drift.mV[1]}, hero_shift_m, pos.mV[VZ], deck.mShearTable);
                pos.mV[VX] = placed.x;
                pos.mV[VY] = placed.y;

                const F32 waist = 1.f - 0.35f * ss_smoothstep(0.2f, 0.65f, up_cell);
                const F32 flare = 1.1f * ss_smoothstep(0.74f, 1.f, up_cell);

                // <SS:Nexii> The anvil is a TOP feature and must behave like one. The height weight comes from the authored profile ramp's RED channel when there is one - the same curve the shader's carving samples - and from the built-in window otherwise. Either way it is gated by convection: a stable sky keeps rounded tops whatever the profile says, and the ramp can only bring the anvil forward, never hold it back once the deck-wide figure takes over.
                const F32 ramp_v = (deck.mProfileN > 0)
                    ? profileSample(deck, up, 0)
                    : ss_smoothstep(0.55f, 0.85f, up);
                const F32 anvil_h = ramp_v * ss_smoothstep(0.40f, 0.60f, convection);
                const F32 puff_anvil = llmax(cell_anvil, anvil_h);

                // <SS:Nexii> Phase 3: mammatus flares the anvil's flattening on puffs already in its top third
                // (up_cell >= SSStormCouple::MAMMATUS_TOP_FRAC) - a CPU-only shape modifier (no shader change),
                // scaling how far the waist/flare term pulls the puff from round (waist+flare-1) rather than the
                // puffs themselves, so a puff below the top third is untouched regardless of storm_sample.mammatus.
                F32 flatten_term = waist + flare - 1.f;
                if (storm_sample.mammatus > 0.f && up_cell >= SSStormCouple::MAMMATUS_TOP_FRAC)
                {
                    flatten_term *= SSStormCouple::mammatusScale(storm_sample.mammatus);
                }
                // <SS:Nexii> 8a item 4 (doc/atmo_magic_phase8_show.md section 3): the gust-front shelf ahead of a
                // squall line's wall floors this same flatten term - the shelf's ONLY visual for now (it does not
                // yet widen/lower the puff independent of puff_anvil*coreness the way the wall itself does through
                // boost/anvil above), so a shelf under a puff whose height/convection would otherwise round it off
                // still reads laminar rather than puffy.
                flatten_term = llmax(flatten_term, storm_sample.lineShelf);

                const F32 flat = 1.f + puff_anvil * coreness * flatten_term;

                const LLVector3 to_cam = pos - cam;
                const F32 dist_sq = to_cam.magVecSquared();
                // <SS:Nexii> LOD phase, removed 2026-09-05 (doc/atmo_magic_far_clouds.md phase 6a outcome): this
                // used to cull any puff past FIELD_DRAW_M. It is gone - SSDeckLod::edgeFade (zero at DECK_EDGE_M,
                // 9800 < FIELD_DRAW_M's 10000) already owns the edge below, and a hard FIELD_DRAW_M cull on top of
                // it was exactly the thing that could drop a leaned sub-0 puff whose cell centre sat inside the
                // walk but whose placed position, past the shear/hero/jitter displacement, crossed FIELD_DRAW_M -
                // silently breaking "sub 0 is always placed for every gated cell". Removing it restores that rule.

                // <SS:Nexii> phase-3c fix F1, still in force post phase-4 fixup #7/F9: routed through
                // SSStormCouple::squashedAt, the one shared predicate for this cull - sub 0 never squashes here,
                // which is exactly why the overshoot bonus above always goes to sub 0 rather than an argmax that
                // could land on a sub this line is about to drop.
                const bool squashed = SSStormCouple::squashedAt(to_cam.mV[VX], to_cam.mV[VY], to_cam.mV[VZ], mSquashKnee);
                if (squashed && sub > 0) continue;

                Puff puff;
                puff.mPosAgent = pos;
                puff.mRadius = base_radius * size_gain * flat
                    * (0.7f + 0.6f * hashUnit(cx, cy, 5u + sub_salt))
                    * (squashed ? 1.6f : 1.f);
                puff.mCamDistSq = dist_sq;

                const F32 dist = sqrtf(dist_sq);
                const F32 edge_t = llclamp((dist - FIELD_FADE_START_M)
                                           / (FIELD_DRAW_M - FIELD_FADE_START_M), 0.f, 1.f);

                // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): the puff's own alpha, in three factors - the
                // sub-count crossfade (subAlpha, 1 for sub 0 always, so this never dims the one body every
                // gate-occupied cell keeps), the far-thinning compensation (thinAlphaComp, >= 1, so a surviving
                // sub > 0 puff brightens to cover for the peers keepPuff above just skipped - the product with
                // subAlpha is clamped to 1 rather than letting compensation push a puff past fully opaque), and
                // edgeFade on the puff's own TRUE horizontal distance (to_cam.xy, not the 3-D `dist` above -
                // matches the veil's ss_edge_rails fade, which reads horizontal distance too) - replacing the old
                // 3-D edge_t ramp, which used different rails (0.85x FIELD_FADE_START_M) than the veil's did.
                // edge_t/cubic_step(edge_t) below (the `rim` structural-shading term) is untouched by this change.
                const F32 horiz = sqrtf(to_cam.mV[VX] * to_cam.mV[VX] + to_cam.mV[VY] * to_cam.mV[VY]);
                const F32 lod_alpha = llmin(1.f, SSDeckLod::subAlpha(lod_dist, sub, puffs_per_cell)
                                                * SSDeckLod::thinAlphaComp(lod_dist));
                // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): tier_wA - this cell's own macro-block
                // crossfade weight, computed once above from the BLOCK's centre distance (not lod_dist) so every
                // fine cell in the same macro block fades together - multiplies in as a fourth factor, 1 below
                // TIER_B_M - TIER_BLEND_M/2 (Tier B not yet in scope) and falling to 0 by TIER_B_M + TIER_BLEND_M/2
                // (where the cell never reaches this line at all - see the tier_wA <= 0 skip above), so Tier A's
                // puffs fade out exactly as Tier B's merged body (below, weighted by wB = 1 - wA) fades in.
                puff.mAlpha = lod_alpha * SSDeckLod::edgeFade(horiz) * llclamp(0.35f + 0.65f * field.mCoverage, 0.f, 1.f) * tier_wA;

                // <SS:Nexii> 6d review fix 3: the structural shading (facing term, exponential shade through the layer,
                // beam gate, rim ease) and the buried depth (see Puff::mBuried - NOT beam-gated, a storm deck is dark
                // underneath at midnight too) come from SSDeckShade (ssdeckshadecore.h), the one formula site the veil
                // above and the Tier B body below share; deckshadecore.cpp pins this call bit-identical to the old block.
                const F32 rim = cubic_step(edge_t);
                puff.mForm = SSDeckShade::puffForm(light_dir.mV[VZ], shade_th, up, cell_height, coreness, rim, beam, shade_calib);
                puff.mBuried = SSDeckShade::buried(cell_height, up, rim);
                // <SS:Nexii> S2 (doc/atmo_magic_flow_field.md section 2, ssvolcloud.h Puff::mPhase): the per-puff
                // advected-detail phase - same hashUnit/sub_salt idiom as jx/jy/up_cell/mRadius above, its own
                // untaken slot (977) so it does not alias any of them, keyed on the cell and sub only, never camera
                // or time (I3/I5).
                puff.mPhase = hashUnit(cx, cy, sub_salt + 977u);

                // <SS:Nexii> [interaction: ssdeckflowcore.h] The per-puff flow SWIRL - a FIELD read, not a hash, so
                // neighbouring puffs lean the same way (see Puff::mFlowSwirl). Read at the cell's own AIR-frame
                // centre, exactly like every other per-cell quantity here: no camera, no clock, no drift, so two
                // clients agree and a wrap realigns it with the lattice. Per CELL rather than per SUB on purpose -
                // the sub-puffs of one cell are one cloud lump and should boil together; the phase above is what
                // keeps them out of step in TIME.
                puff.mFlowSwirl = SSDeckFlow::swirlUnit(((F32)cx + 0.5f) * CELL_M, ((F32)cy + 0.5f) * CELL_M,
                                                        SSDeckFlow::SWIRL_SALT);

                dist_sum += dist_sq;
                deck.mPuffs.push_back(puff);
            }
        }
    }

    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT, doc/atmo_magic_far_clouds.md section 2 step 2 Tier B):
    // one merged macro-puff body per accumulated macro cell with occupancy > 0 and macroEligible, emitted as an
    // ORDINARY Puff - same sort, same budget, same shader path as every fine puff above - so nothing downstream
    // (the depth sort, the budget trim, the render pass) needs to know Tier A from Tier B. Placed by the PRODUCER
    // rule: the block's own quantized air-frame centre + drift + the hero shift evaluated AT that unshifted
    // centre (never the fine cells' own, possibly-jittered placements) + O(z) at the body's own altitude, all
    // through the SAME SSDeckFrame::placeWorld the fine loop calls. mForm/mBuried reuse the fine puff loop's own
    // shade formula (above) at the body's own `up`, so a Tier B body shades exactly as a fine puff standing in
    // its place would - the two tiers must never visibly disagree about which side of a body is lit.
    for (auto& kv : macro_accum)
    {
        MacroAccum& acc = kv.second;
        const F32 occ = SSDeckMacro::occupancy(acc.mVerdicts, acc.mCount);
        if (occ <= 0.f) continue;

        const F32 block_air_x = ((F32)acc.mMcx + 0.5f) * SSDeckMacro::MACRO_M;
        const F32 block_air_y = ((F32)acc.mMcy + 0.5f) * SSDeckMacro::MACRO_M;

        if (!SSDeckMacro::macroEligible(block_air_x, block_air_y, air_x, air_y, occ)) continue;

        // <SS:Nexii> Producer rule: the hero's influence weight is evaluated at the block's own UNSHIFTED,
        // quantized world point (SSStormCouple::samplePointM on the MACRO lattice - the same helper buildDeck's
        // own per-cell hero_influence above calls on the fine lattice, one level up) - never the hero-shifted
        // result placeWorld below produces.
        F32 block_world_x, block_world_y;
        SSStormCouple::samplePointM(block_air_x, block_air_y, SSDeckMacro::MACRO_M,
                                     drift.mV[0], drift.mV[1], block_world_x, block_world_y);
        const F32 body_hero_influence = (couple_storm && mHeroFrame.ageS > 0.f)
            ? SSStormCouple::influence(mHeroFrame.centre.x, mHeroFrame.centre.y, mHeroFrame.radius,
                                        block_world_x, block_world_y)
            : 0.f;
        const SSDeckFrame::Vec2 body_hero_shift_m = SSDeckFrame::heroShift(mHeroFrame, body_hero_influence);

        // <SS:Nexii> bodyUp (ssdeckmacrocore.h) is a masked mean over the occupied fine cells' own array - its
        // contract names `ups` (the up_cell hash) but the implementation is a generic masked mean, so it is
        // reused here for cell_height and coreness too rather than hand-rolling the identical sum/count loop a
        // second and third time in shell code; occ > 0 above guarantees at least one occupied slot, so the
        // default-when-empty branch (0.5) is never actually taken for these two reused calls. Reported as a
        // formula this pass was tempted to write directly in shell code (see the task's own return-data request).
        const F32 up_cell_mean = SSDeckMacro::bodyUp(acc.mUps, acc.mVerdicts, acc.mCount);
        const F32 cell_height_mean = SSDeckMacro::bodyUp(acc.mCellHeights, acc.mVerdicts, acc.mCount);
        const F32 coreness_mean = SSDeckMacro::bodyUp(acc.mCoreness, acc.mVerdicts, acc.mCount);

        const F32 up = up_cell_mean * cell_height_mean;
        const F32 z = field.mBaseHeightM + up * field.mThicknessM;

        // <SS:Nexii> 8g rim de-lattice (ssdeckmacrocore.h, doc/atmo_magic_phase8_show.md section 5): the body's own
        // hashed offset from the block centre, added HERE - inside placeWorld's position argument, exactly where the
        // fine loop adds its sub-puff's jx/jy - and nowhere else. Everything that classifies the block still reads the
        // plain quantized centre: macroEligible above, the hero influence sample (producer rule) and acc.mBlockDist,
        // the one scalar both halves of the A/B crossfade read. So the jitter moves the drawn body and cannot move the
        // gate, the walk membership or the tier weight. Bound +-156 m (0.30 * MACRO_M), under the block half-width and
        // under the smallest body radius - see the constant's own comment for why both matter.
        F32 body_jx, body_jy;
        SSDeckMacro::bodyJitterM(acc.mMcx, acc.mMcy, salt, body_jx, body_jy);

        const SSDeckFrame::Vec2 placed = SSDeckFrame::placeWorld(
            SSDeckFrame::Vec2{block_air_x + body_jx, block_air_y + body_jy},
            SSDeckFrame::Vec2{drift.mV[0], drift.mV[1]}, body_hero_shift_m, z, deck.mShearTable);

        Puff body;
        body.mPosAgent = LLVector3(placed.x, placed.y, z);
        // body.mRadius is set below, from the SAME SSDeckMacro::bodyShape call that produces the alpha - see there.

        const LLVector3 to_cam = body.mPosAgent - cam;
        body.mCamDistSq = to_cam.magVecSquared();
        const F32 dist = sqrtf(body.mCamDistSq);
        const F32 horiz = sqrtf(to_cam.mV[VX] * to_cam.mV[VX] + to_cam.mV[VY] * to_cam.mV[VY]);

        // <SS:Nexii> 6d review findings 1+2: BOTH halves of the A/B crossfade read ONE scalar - acc.mBlockDist, the
        // block's horizontal, drift-only centre distance the fine loop computed for this block's cells' tier_wA -
        // never this body's own 3-D / hero-shifted / O(z)-leaned distance. The body sits at deck altitude, so its 3-D
        // distance ran hundreds of metres ahead of its fine cells' horizontal one and read wB ~ 1 while they still read
        // wA ~ 1: double coverage at the band's inner edge, not a crossfade. wA is the fine side's and unused here.
        F32 body_wA, body_wB;
        SSDeckMacro::tierWeights(acc.mBlockDist, body_wA, body_wB);
        (void)body_wA;

        // <SS:Nexii> "fine alpha at that distance": the fine puff loop's own sub-0 alpha factors (subAlpha is
        // always 1 for sub 0; thinAlphaComp applies to sub 0 too, at the fine loop's horizontal cell distance) -
        // re-run at the block's SAME horizontal distance rather than a hand respelled constant, so a Tier B body's
        // brightness tracks what the fine puffs it replaces would have carried.
        const F32 fine_alpha_at_dist = llmin(1.f, SSDeckLod::subAlpha(acc.mBlockDist, 0, puffs_per_cell)
                                                  * SSDeckLod::thinAlphaComp(acc.mBlockDist))
                                      * llclamp(0.35f + 0.65f * field.mCoverage, 0.f, 1.f);
        // <SS:Nexii> 8g rim de-lattice: radius and alpha come from ONE core call - the hashed size roll (0.80..1.25 of
        // bodyRadiusM()) and the BODY_AREA_NORM factor that keeps the population's mean covered area equal to the
        // unvaried constant are two halves of one identity, so the core hands them out together and no call site can
        // take a varied radius with an unvaried alpha (that would inflate far coverage by E[s^2], 6.75%). The tier
        // weight, edgeFade and the emptiness cut below are unchanged.
        F32 body_radius_m, body_shape_alpha;
        SSDeckMacro::bodyShape(acc.mMcx, acc.mMcy, salt, fine_alpha_at_dist, occ, body_radius_m, body_shape_alpha);
        body.mRadius = body_radius_m;
        body.mAlpha = body_shape_alpha * body_wB * SSDeckLod::edgeFade(horiz);
        if (body.mAlpha <= 0.001f) continue;

        // <SS:Nexii> 6d review fix 3: the SAME SSDeckShade call the fine loop makes (one formula site, ssdeckshadecore.h),
        // evaluated at this body's own up / cell_height_mean / coreness_mean; edge_t from the body's own 3-D distance
        // exactly as a fine puff's rim is.
        const F32 edge_t = llclamp((dist - FIELD_FADE_START_M) / (FIELD_DRAW_M - FIELD_FADE_START_M), 0.f, 1.f);
        const F32 rim = cubic_step(edge_t);
        body.mForm = SSDeckShade::puffForm(light_dir.mV[VZ], shade_th, up, cell_height_mean, coreness_mean, rim, beam, shade_calib);
        body.mBuried = SSDeckShade::buried(cell_height_mean, up, rim);
        // <SS:Nexii> S2 review fix (review_s0s2_sonnet.md #1): a Tier B body gets its own hashed phase off its
        // macro-cell coords - the whole far field otherwise pulsed in lockstep at the default 0. Same 977 slot
        // idiom as the fine loop's sub_salt + 977; a macro cell (mcx,mcy) can numerically collide with fine cell
        // (cx,cy) of the same integers and share a phase VALUE, but the two are unrelated puffs on different
        // grids hundreds of metres apart, so a shared phase between them is invisible - not "no aliasing", just
        // aliasing that cannot matter.
        body.mPhase = hashUnit(acc.mMcx, acc.mMcy, salt + 977u);

        // <SS:Nexii> [interaction: ssdeckflowcore.h] The same swirl FIELD the fine loop reads, at this body's own
        // block centre - the same air-frame position, so a Tier B body agrees with the Tier A puffs it stands in
        // for across the crossfade instead of picking an unrelated lean (the field is continuous, so the block
        // centre and its cells are all within one lattice cell of each other).
        body.mFlowSwirl = SSDeckFlow::swirlUnit(block_air_x, block_air_y, SSDeckFlow::SWIRL_SALT);

        // <SS:Nexii> A Tier B body counts toward the deck's own (non-shaft) mean distance below, same as a fine
        // puff - it is an ordinary cloud body, unlike the shaft cards F5 excludes for being a different geometry.
        dist_sum += body.mCamDistSq;
        deck.mPuffs.push_back(body);
        ++deck.mTierBCount;
    }

    // <SS:Nexii> F5: the deck's own (non-shaft) puff count, captured HERE - before any shaft cards are added to
    // the SAME mPuffs vector below - so mMeanDistSq (the primary/under draw-order hysteresis input, see
    // mUnderOnTop) stays a property of the ordinary cloud body. A wide storm's curtain of far shaft cards must
    // not drag the mean outward and flip which deck draws on top; dist_sum below is likewise never added to by
    // the shaft loop, only divided by this count.
    const size_t nonshaft_puff_count = deck.mPuffs.size();

    // <SS:Nexii> Distant rain shafts (ssvirgacore.h, doc/atmo_magic_far_clouds.md section 3): the qualifying cell
    // set collected above, trimmed by SSVirga::keepHash - a per-candidate STABLE hash trim (F1), so a camera walk
    // crossing or an easing precip that moves the candidate count n by one changes only the cells whose hash sits
    // between the old and new keep threshold, never the whole kept set (the index-based trim this replaced
    // re-picked all MAX_SHAFTS curtains on every count change). No sort is needed for this - keepHash is a pure
    // per-candidate test. keepHash's own p targets a SHARE of n, not a literal count, so a run of bad luck on a
    // small/quantised n can occasionally keep more than MAX_SHAFTS; SSVirga::hardCap (HARD_CAP_FRAC x MAX_SHAFTS,
    // the stated exception in ssvirgacore.h's header) backstops that - ONLY when the kept set actually exceeds it does
    // a second pass sort the kept indices by ShaftCandidate::hashKey (a separate, camera-free hash chain) and cut
    // to the ceiling, so the ordinary path never pays for a sort it does not need. Emitted straight into
    // deck.mPuffs, ahead of the depth sort and puff-budget trim just below, so shafts get the SAME farthest-first
    // draw order and the SAME last-resort budget backstop every ordinary puff does - no separate pass, no
    // separate cap accounting.
    if (shafts_active)
    {
        // <SS:Nexii> V4 debug snapshot (ssatmoinfoviewcore.h MODE_PRECIP_VIRGA): recorded here, once per build,
        // off the SAME candidate list and the SAME keepHash verdict the card-emission loop below uses (updated a
        // second time below for any candidate the hard-ceiling backstop later drops) - the view never re-derives
        // qualification or the trim. Recorded even when shaft_candidates is empty (mCells then empty, mActive
        // still true), so the legend can say "0 qualifying cells" rather than "off". [interaction: SSAtmoInfoView
        // virgaDebug]
        mVirgaDebug.mActive = true;
        mVirgaDebug.mR2 = shaft_r2;
        mVirgaDebug.mGroundZ = shaft_ground_z;
        mVirgaDebug.mBaseZ = deck.mBaseZ;
        // <SS:Nexii> F9 (2026-09-06 review), 8e-b PROFILE SKEW: the SAME params/baseAgl/fall-speed/embed-top
        // values the card-emission loop below actually skews its cards by, snapshotted here rather than left for
        // the view to re-derive.
        mVirgaDebug.mWindParams = params;
        mVirgaDebug.mBaseAglM = shaft_base_agl;
        mVirgaDebug.mFallSpeed = shaft_fall_speed;
        mVirgaDebug.mEmbedTopZ = SSVirga::embedTopZ(deck.mBaseZ, deck.mThicknessM);
        mVirgaDebug.mCells.clear();
        mVirgaDebug.mCells.reserve(shaft_candidates.size());

        const S32 n = (S32)shaft_candidates.size();

        std::vector<S32> kept_idx;
        kept_idx.reserve(n);
        for (S32 i = 0; i < n; ++i)
        {
            const ShaftCandidate& cand = shaft_candidates[i];
            const bool hash_kept = SSVirga::keepHash(cand.cellId, SS_VIRGA_HASH_SALT, n, SSVirga::MAX_SHAFTS);
            mVirgaDebug.mCells.push_back({ cand.x, cand.y, cand.drive, hash_kept });
            if (hash_kept) kept_idx.push_back(i);
        }

        // The core's hard ceiling (SSVirga::hardCap): rank cut on the fixed per-cell hashKey, only if exceeded.
        const S32 hard_cap = SSVirga::hardCap(SSVirga::MAX_SHAFTS);
        if ((S32)kept_idx.size() > hard_cap)
        {
            std::sort(kept_idx.begin(), kept_idx.end(), [&shaft_candidates](S32 a, S32 b)
                      { return shaft_candidates[a].hashKey < shaft_candidates[b].hashKey; });
            for (S32 j = hard_cap; j < (S32)kept_idx.size(); ++j)
            {
                mVirgaDebug.mCells[kept_idx[j]].mKept = false; // overridden by the hard-ceiling backstop
            }
            kept_idx.resize(hard_cap);
        }

        // <SS:Nexii> Phase 8e (ssvirgacore.h embedTopZ, doc/atmo_magic_phase8_show.md section 3b): the stack's
        // TOP is no longer the deck base itself but a fixed span of the puff thickness above it, so the curtain
        // starts inside the cloud rather than hanging off a hard line - see embedTopZ's own comment. Shared by
        // every candidate this build (deck.mBaseZ/mThicknessM do not vary per candidate).
        const F32 shaft_top_z = SSVirga::embedTopZ(deck.mBaseZ, deck.mThicknessM);

        // <SS:Nexii> F2 (2026-09-06 review): carried to render()'s ss_shaft_embed uniform (deck.mBaseZ,
        // shaft_top_z) so the fragment stage's ss_virga_embedAlpha reads the SAME embed span this build actually
        // used, rather than a second independent embedTopZ() call in the render pass.
        deck.mShaftEmbedTopZ = shaft_top_z;

        // <SS:Nexii> Phase 8e item 2: carried to render()'s ss_shaft_embed uniform's z component alongside
        // deck.mBaseZ/mShaftEmbedTopZ so the fragment stage's curtain-height fraction h reads the SAME
        // [ground, base] span this build's card loop actually used.
        deck.mShaftGroundZ = shaft_ground_z;

        for (S32 idx : kept_idx)
        {
            const ShaftCandidate& cand = shaft_candidates[idx];

            // <SS:Nexii> F3: the far ground lift (SSVirga::groundLiftZ) - a per-CANDIDATE quantity (the column's
            // own camera distance), computed once and shared by every card in its stack. Cards whose whole slab
            // lies below the lifted ground are skipped entirely; the lowest emitted card's bottom clamps UP to
            // the lift, never down past it - virga: precipitation evaporating before it lands, which also keeps
            // this column's ground-level geometry out of the far-squash depth fold that would otherwise draw it
            // over terrain standing in front of it. Stated residual (ssvirgacore.h's own comment): the fold still
            // affects a curtain's mid-air cards against tall terrain, as it does far puffs. h01/alpha/width below
            // deliberately use each card's TRUE (unlifted) height fraction - the lift truncates what is drawn, it
            // does not change the physical curtain's own vertical profile. groundLiftZ itself still spans the REAL
            // deck base (not the embedded top) - the knee/reach retune is about how far virga reaches down, not
            // about where the stack starts.
            const F32 cand_dx = cand.x - cam.mV[VX];
            const F32 cand_dy = cand.y - cam.mV[VY];
            const F32 cand_horiz = sqrtf(cand_dx * cand_dx + cand_dy * cand_dy);
            const F32 lift_z = SSVirga::groundLiftZ(shaft_ground_z, deck.mBaseZ, cand_horiz, mSquashKnee);

            // F2/F7: overlapping card slabs (SSVirga::cardCountOverlapped/cardSpanLifted), not the old non-overlapping
            // ceil(span / CARD_MAX_M) - the shader's two-sided soft ends (SS_SHAFT_V_SOFT) crossfade in the
            // OVERLAP_FRAC overlap instead of punching a transparent band at each stack seam. The lift truncation
            // (skip a slab wholly below lift_z, raise the lowest emitted bottom to it) is the core's, so the
            // twin test calls the same function rather than copying this loop. The stack spans [groundZ,
            // shaft_top_z] (phase 8e's embedded top), not [groundZ, deck.mBaseZ].
            const S32 cards = SSVirga::cardCountOverlapped(shaft_top_z, shaft_ground_z);
            for (S32 card = 0; card < cards; ++card)
            {
                // <SS:Nexii> Phase 8e (ssvirgacore.h cardGeom), 8e-b PROFILE SKEW: builds this card's wind-skewed
                // slab in one call - centreXY/shearXY are BOTH profileSkewM(params, baseAglM, ...) evaluated at
                // this card's own z (never a value copied from a neighbour, which is what keeps consecutive
                // cards' shear chains continuous - and what makes the card a CHORD of the wind-profile-integrated
                // trajectory), h01Mid is the DECK BASE profile fraction (not the embedded stack's own
                // topZ-relative one) that alphaFor/halfWidthM below expect. F2 (2026-09-06 review): no alphaMul
                // any more - the embed fade is read by the fragment shader itself, per pixel, off ss_shaft_embed
                // (set just above) and world_true.z, not baked here at the card's mid-height. shaft_base_agl is
                // the SAME baseAgl bakeShearTable/params resolved this build with - see its own comment above.
                bool card_emitted = false;
                const SSVirga::CardGeom cg = SSVirga::cardGeom(
                    card, deck.mBaseZ, shaft_top_z, shaft_ground_z, lift_z,
                    params, shaft_base_agl, shaft_fall_speed, card_emitted);
                if (!card_emitted) continue; // F3

                // <SS:Nexii> ONE PHENOMENON, EVAPORATION FROM BELOW (user, 2026-09-06): a card wholly inside the
                // drive-dependent evaporated zone is skipped entirely (geometry saved; the fragment's own ragged
                // mask does the eroded edge) - SSVirga::cardEmittedFor tests the card's OWN TOP fraction against
                // the deck base profile (SSVirga::baseProfileH01, the SAME normalization cardGeom's own h01Mid
                // uses), never cg.h01Mid itself (that is the card's MID fraction, not its top).
                const F32 card_top_h01 = SSVirga::baseProfileH01(cg.zTop, deck.mBaseZ, shaft_ground_z);
                if (!SSVirga::cardEmittedFor(card_top_h01, cand.drive)) continue;

                const F32 card_h = cg.zTop - cg.zBot;
                const F32 z_mid = 0.5f * (cg.zTop + cg.zBot);

                Puff shaft;
                shaft.mPosAgent = LLVector3(cand.x + cg.centreXY.x, cand.y + cg.centreXY.y, z_mid);
                shaft.mShearXY = LLVector2(cg.shearXY.x, cg.shearXY.y);
                // <SS:Nexii> WIDTH FOLLOWS INTENSITY (user, 2026-09-06): halfWidthM now also takes the cell's own
                // drive, so a heavy-rain curtain reads as a wide wall and a wispy one as a narrow filament.
                shaft.mRadius = SSVirga::halfWidthM(cg.h01Mid, cand.drive);
                shaft.mHalfHeightM = card_h * 0.5f;
                // <SS:Nexii> Phase 8e item 3: the shaft's form/buried are the SAME veil terms the deck's own
                // base floor wears (deck.mSheetForm; the BURIED depths are no longer equal - see D2 at Deck::SHEET_BURIED) - the curtain is the
                // deck's underside pulled down, so it is lit exactly like the veil's own lower rim, which is why
                // the shaft branch's shading below (same frame, same tail, same mSheetForm/BURIED inputs) matches
                // the veil with only the droop differing.
                shaft.mForm = deck.mSheetForm;
                shaft.mBuried = SSVirga::BURIED;
                shaft.mShaft = true;
                // <SS:Nexii> Phase 8e item 4, DRIVE REACHES THE FRAGMENT: the SAME SSVirga::drive this cell
                // qualified with (cand.drive), carried to render()'s vertex colour b channel.
                shaft.mDrive = cand.drive;

                const LLVector3 to_cam = shaft.mPosAgent - cam;
                shaft.mCamDistSq = to_cam.magVecSquared(); // true per-card distance: this is the depth-sort key, and every card's own position is what it must sort by

                // <SS:Nexii> F6 (2026-09-06 review): handoff and edgeFade read cand_horiz - the CANDIDATE's own
                // horizontal distance, computed once above and already shared by groundLiftZ - rather than a fresh
                // per-card horizontal distance off this card's own skewed position. A curtain is ONE object: its
                // handoff/edge fade must not vary card-to-card up a skewed stack, or a strong wind's lean would
                // wipe alpha vertically along the curtain (nearer cards fading in/out independently of farther
                // ones at the same candidate) instead of the whole curtain fading as a unit with camera distance.
                //
                // <SS:Nexii> ONE PHENOMENON, EVAPORATION FROM BELOW (user, 2026-09-06): alphaFor(drive) replaces
                // alphaAt(h01, drive) - alpha is intensity x density across the whole volume, not a function of
                // height any more (the OLD vertical alpha profile is deleted); the "denser near the base, wispy at
                // the ground" look now comes entirely from cardEmittedFor's evaporation gate above, not a per-h01
                // alpha term. F2 (2026-09-06 review): no cg.alphaMul factor here either - the embed fade is the
                // fragment's own, per pixel (ss_virga_embedAlpha in ssVolCloudF.glsl), not a value baked once per
                // card at its mid-height that could never ramp WITHIN a card's own span.
                shaft.mAlpha = SSVirga::alphaFor(cand.drive)
                             * SSVirga::handoff(cand_horiz, shaft_r2)
                             * SSDeckLod::edgeFade(cand_horiz);
                if (shaft.mAlpha <= 0.001f) continue;

                // F5: shaft cards are EXCLUDED from dist_sum - see nonshaft_puff_count's own comment above.
                deck.mPuffs.push_back(shaft);
            }
        }
    }

    if (!deck.mPuffs.empty())
    {
        deck.mMeanDistSq = (nonshaft_puff_count > 0) ? (F32)(dist_sum / (F64)nonshaft_puff_count) : 0.f;

        std::sort(deck.mPuffs.begin(), deck.mPuffs.end(),
                  [](const Puff& a, const Puff& b) { return a.mCamDistSq > b.mCamDistSq; });

        // <SS:Nexii> The puff ceiling is a viewer dial rather than a build constant, because it is this field's whole LOD axis and the machine drawing the sky is the only thing that knows what it can afford. Applied per deck, after the depth sort: the farthest puffs are at the front of the vector, so the erase takes the field's far edge and leaves the sky directly overhead whole - the same way the tier distances trim precipitation.
        static LLCachedControl<U32> budget_setting(gSavedSettings, "SSAtmoCloudPuffBudget", 2520);
        const S32 max_puffs = llclamp((S32)budget_setting, MIN_PUFF_BUDGET, MAX_PUFF_BUDGET);
        if ((S32)deck.mPuffs.size() > max_puffs)
        {
            // <SS:Nexii> LOD phase: this erase is now the LAST-RESORT safety, not the field's primary far-distance
            // control - SSDeckLod::keepPuff/subAlpha above are meant to hold the built count under the budget by
            // thinning, so a trim here means the dial's own density plus the near field alone already outgrew it.
            // Logged (not warned) so a design pass can see how often/how hard this backstop still fires without
            // spamming a normal session's log.
            LL_DEBUGS("AtmoMagic") << "buildDeck: puff budget trimmed " << (deck.mPuffs.size() - (size_t)max_puffs)
                                   << " of " << deck.mPuffs.size() << " built puffs (budget " << max_puffs
                                   << ", salt " << salt << ")" << LL_ENDL;
            deck.mPuffs.erase(deck.mPuffs.begin(), deck.mPuffs.end() - max_puffs);
        }
    }
}

// Drawn/true distance ratio of the shared squash band, for anything that must land at the field's drawn depth.
F32 SSVolCloud::squashScale(F32 true_dist) const
{
    if (true_dist <= mSquashKnee || true_dist <= 0.f) return 1.f;
    const F32 span = llmax(mEffRadius - mSquashKnee, 1.f);
    const F32 drawn = llmin(mSquashKnee + (true_dist - mSquashKnee) * (mSquashCap - mSquashKnee) / span,
                            mSquashCap * 0.999f);
    return drawn / true_dist;
}

// <SS:Nexii> The ground shadow bake: one small transmittance map over the field's whole extent - how much direct sun survives a straight fall through the deck at each point of the air frame. Built from the builder's own answers (the cell gate that decides which cells hold puffs, the noise map's presence cut, and the map's mottle for texture inside occupied regions), so the shadow on the ground is the deck in the sky, never a second opinion of it. The vertical-column approximation is deliberate: the deck is thin against the map's footprint, and the SUN'S ANGLE enters at sample time in the soften shader, which projects each ground point up along the live sun direction to the casting plane - so the shadows lie, stretch and crawl correctly while the bake stays angle-free and therefore reusable across the whole day. Keyed on everything it reads; update() calls this every frame and the key makes that free.
void SSVolCloud::bakeGroundShadow(const Deck& deck, F32 air_x, F32 air_y)
{
    // N rides FIELD_DRAW_M: the map spans the whole field, so the texel count doubles with the reach to hold a ground texel at ~78m - the scale the shadow's edges were tuned at.
    const S32 N = 256;
    const F32 span = FIELD_DRAW_M * 2.f;

    // The key: the camera's air CELL (which also fixes the grid origin, so equal key means equal frame), and every dial the texels read. Quantised where the source eases continuously, so a slowly-consolidating storm rebakes every few steps of the dial rather than every frame of the easing.
    U64 key = 1469598103934665603ULL;
    const auto fold = [&key](U64 v) { key ^= v + 0x9e3779b97f4a7c15ULL; key *= 1099511628211ULL; };
    const S32 cam_cx = llfloor(air_x / CELL_M);
    const S32 cam_cy = llfloor(air_y / CELL_M);
    fold((U64)(U32)cam_cx);
    fold((U64)(U32)cam_cy);
    fold((U64)llround(deck.mCoverage * 64.f));
    fold((U64)llround(deck.mNoiseHole * 64.f));
    fold((U64)llround(deck.mNoiseTowerLo * 64.f));
    fold((U64)llround(deck.mNoiseTowerHi * 64.f));
    fold((U64)llround(deck.mThicknessM / 25.f));
    fold((U64)llround(llclamp(deck.mPuffDensity, 0.f, 1.f) * 32.f));
    fold((U64)llround(deck.mNoiseTileM));
    fold((U64)deck.mSalt);
    fold((U64)(U32)deck.mNoiseW);   // the readback generation: the presence cut sharpens when the map lands

    // <SS:Nexii> Phase 4 fixup (#4/F6, doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud drift"):
    // mHeroFrame belongs to whichever deck IS weatherDeck() this build (buildDeck only ever sets it for that
    // deck's own coupled pass - see buildDeck's own comment); this bake is called for `deck`, which may be a
    // DIFFERENT deck than the one mHeroFrame was built for (bakeGroundShadow always bakes mPrimary, and
    // weatherDeck() may resolve to mUnder). So the hero frame this bake reads is zeroed out unless `deck` IS
    // weatherDeck() - ssdeckframecore.h's OWNERSHIP rule ("hero_influence is zero unless couple_storm"). The bake
    // key folds it with the NEW foldFrameKey signature (no table - the bake applies no O(z), so the table was
    // never folded here to begin with). Identity fold (key returned unchanged) whenever there is no hero, by the
    // core's own invariant, so a plain sky's key is untouched.
    const SSDeckFrame::HeroFrame& hero = (&deck == weatherDeck()) ? mHeroFrame : SSDeckFrame::HeroFrame();
    key = SSDeckFrame::foldFrameKey(key, hero);
    if (key == mShadowKey && mShadowRef.notNull())
    {
        return;
    }

    const F32 ox = ((F32)cam_cx + 0.5f) * CELL_M - span * 0.5f;
    const F32 oy = ((F32)cam_cy + 0.5f) * CELL_M - span * 0.5f;

    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();
    // <SS:Nexii> [interaction: ssdeckcellsoftcore.h] Takes an AIR POINT, not a cell index, and reads the hero's influence through SSDeckCellSoft::heroInfluenceSoft - the same fix ssVolCloudF.glsl's ss_hero_influenceSoft carries, for the same reason and on the same lattice. What this replaced hashed the texel's own cell (llfloor(ax / CELL_M)) and took ONE influence at that cell's centre, so the bake's gate point was a 260 m staircase across the hero's falloff ring exactly as the fragment stage's was - the comment at the call site below already recorded the symptom ("in the falloff ring it put the bake up to half a cell from the fragment's gate_air") without naming it a discontinuity. The blend is bit-identical to the old read at every cell CENTRE (unit_deck_cellsoft.cpp), so the producer-side lockstep this bake mirrors is unchanged; only the points between centres move, and they move continuously. Zero whenever `hero` has no age (no hero, or this deck is not weatherDeck()), making every gateAir call below an identity read.
    const auto heroShiftAt = [&](F32 ax, F32 ay) -> SSDeckFrame::Vec2
    {
        if (!(hero.ageS > 0.f))
        {
            return SSDeckFrame::Vec2();
        }
        const F32 inf = SSDeckCellSoft::heroInfluenceSoft(ax, ay, CELL_M, drift.mV[0], drift.mV[1],
                                                          hero.centre.x, hero.centre.y, hero.radius);
        return SSDeckFrame::heroShift(hero, inf);
    };

    // <SS:Nexii> NEW-1 fix (2026-09-05, ssdeckframecore.h's producer/observer rule): this CELL loop is a PRODUCER
    // MIRROR, not an observer - buildDeck's gate hash and presence read run at the lattice cell's PLAIN centre c
    // (see ssdeckframecore.h line 34), so this loop's presence read (below) is likewise at the plain centre, no
    // hero shift. The three-way replication is therefore: buildDeck's own cell gate (producer, plain centre),
    // this CELL loop (producer mirror, plain centre) and ssVolCloudF.glsl's ss_cell_occupied (OBSERVER, gateAir =
    // plain centre - S) - three implementations of one field, but only two of the three agree on which centre to
    // hash; the TEXEL loop below is the bake's own observer and carries the S subtraction on EVERY read it makes,
    // not only presence/tower/mottle - the cell-occupancy bilinear lookup is read at the texel's gateAir too (NEW-A
    // fix, 2026-09-05: it used to index from the raw texel, disagreeing with presence/tower/mottle in the same
    // loop), which is why the grid below is padded wide enough for a gateAir-shifted lookup to stay in bounds.
    // [interaction: buildDeck's gate, ssVolCloudF.glsl's ss_cell_occupied]
    // <SS:Nexii> Phase 6b (ssdecknoisecore.h CONTRACT): the presence half of this same gate lockstep is the
    // de-tiled read (DETILE_SCALE, DETILE_ROT_RAD, DETILE_WEIGHT, all in ssdecknoisecore.h) on BOTH sides -
    // noiseFieldAt does the mix internally on the CPU, so buildDeck's cell gate and this CELL loop's
    // producer-mirror gate read the identical de-tiled field; ss_cell_occupied's GLSL side calls
    // ss_noise_mapDetiled at its own cell centre (ssVolCloudF.glsl ~415), the GLSL twin of the same
    // detileCoord/mixDetile formula, so the three-way lockstep this comment describes holds across the CPU/GLSL
    // boundary too - pinned by V:\Scratch\atmo\tests\twin_detile.cpp's Part 3 call-site tests. Nothing new folds
    // into the bake key above: the mix is a pure function of the read position, no camera/time/client state,
    // exactly like the single read it replaces.
    // <SS:Nexii> NEW-A fix: padded by heroShiftPadCells cells each side beyond the base +3 slop, so a texel whose
    // gateAir has been pushed by up to HERO_SHIFT_CAP_M still resolves to an in-bounds bx/by (ceil(390 / 260) == 2).
    const S32 heroShiftPadCells = (S32)std::ceil(SSDeckFrame::HERO_SHIFT_CAP_M / CELL_M);
    const S32 c0x = llfloor(ox / CELL_M) - 1 - heroShiftPadCells;
    const S32 c0y = llfloor(oy / CELL_M) - 1 - heroShiftPadCells;
    const S32 cn = (S32)(span / CELL_M) + 3 + 2 * heroShiftPadCells;
    std::vector<F32> cell_occ((size_t)cn * (size_t)cn);
    for (S32 gy = 0; gy < cn; ++gy)
    {
        for (S32 gx = 0; gx < cn; ++gx)
        {
            const S32 cx = c0x + gx;
            const S32 cy = c0y + gy;
            const F32 gate_raw = clusterUnit(cx, cy, deck.mSalt) * CLUSTER_WEIGHT
                               + hashUnit(cx, cy, 1u + deck.mSalt) * (1.f - CLUSTER_WEIGHT);
            F32 presence = 1.f;
            F32 tower = 0.f;
            // <SS:Nexii> NEW-1 fix: PRODUCER MIRROR - plain centre, no hero shift (see the CELL-loop comment
            // above). gate_raw just above already hashes the plain (cx, cy); this presence read must classify
            // the SAME plain cell buildDeck's own gate does, or the gate the shader observes at gateAir and the
            // gate this bake's cell_occ array records would disagree on which cell counts as occupied.
            noiseFieldAt(deck, ((F32)cx + 0.5f) * CELL_M, ((F32)cy + 0.5f) * CELL_M, presence, tower);
            const F32 gate = gate_raw + (1.f - gate_raw) * (1.f - presence);
            cell_occ[(size_t)gy * cn + gx] = (gate <= deck.mCoverage) ? 1.f : 0.f;
        }
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw(N, N, 3);
    if (raw.isNull() || !raw->getData())
    {
        return;
    }
    U8* data = raw->getData();

    // <SS:Nexii> The optical depth a fully-occupied column costs, and THICKNESS is its primary driver - linear in the deck's depth, which is what an extinction through a slab actually is. A 100m fair-weather sheet barely shades (about half a depth, ~60% of the beam through), a 500m deck reads as honest cloud shade (~8%), and a 1200m storm column goes functionally black (~0.1%) - so the difference between skies is carried by the weather's own thickness rather than by the user's strength dial, which now defaults to full and only exists to take the effect away. The puff-density dial still modulates: a deck drawn wispy casts wispy.
    const F32 tau_scale = 5.0f * llclamp(deck.mThicknessM / 1000.f, 0.08f, 1.4f)
                        * (0.4f + 0.6f * llclamp(deck.mPuffDensity, 0.f, 1.f));

    // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): the bake integrates every texel's column as if the full
    // puffs_per_cell dial stood there, but buildDeck's own SSDeckLod::subAlpha/keepPuff thin the sub > 0 bodies
    // with distance - so past THIN_START_M the sky the bake shades is darker than the sky actually drawn. Scaled
    // per texel by SSDeckLod::shadowTauScale(dist, dial), read from the SAME SSAtmoCloudPuffsPerCell dial buildDeck
    // reads (bakeGroundShadow has no `deck`-carried dial of its own to reuse). "dist" is from the bake's own
    // camera CELL centre (cam_cx, cam_cy, already quantised above for the bake's key) to the texel's AIR position -
    // the world-frame distance from the true camera is identical (both shift by the same live drift, which
    // cancels), so no drift add/subtract is needed here.
    static LLCachedControl<U32> shadow_dial_setting(gSavedSettings, "SSAtmoCloudPuffsPerCell", (U32)PUFFS_PER_CELL);
    const S32 shadow_dial = llclamp((S32)shadow_dial_setting, MIN_PUFFS_PER_CELL, MAX_PUFFS_PER_CELL);
    const F32 cam_air_wx = ((F32)cam_cx + 0.5f) * CELL_M;
    const F32 cam_air_wy = ((F32)cam_cy + 0.5f) * CELL_M;

    const F32 texel = span / (F32)N;
    for (S32 ty = 0; ty < N; ++ty)
    {
        for (S32 tx = 0; tx < N; ++tx)
        {
            const F32 ax = ox + ((F32)tx + 0.5f) * texel;
            const F32 ay = oy + ((F32)ty + 0.5f) * texel;

            // <SS:Nexii> NEW-1/NEW-2/NEW-A fix (2026-09-05, ssdeckframecore.h's producer/observer rule): this TEXEL
            // loop is the bake's OBSERVER, so the read position itself moves for EVERY read it makes below -
            // texel -> cell via floor((air - S) / CELL_M), then the occupancy lookup (NEW-A), presence AND mottle
            // (NEW-2) all read at gateAir, not the raw texel. S is derived in ONE step from the texel's own air
            // position - exactly as the fragment stage does - and no observer iterates a fixed point
            // (ssdeckframecore.h, foldFrameKey's note). A two-step re-evaluation at the OWNING cell was tried and
            // withdrawn (phase-4d review): inside the plateau it changed nothing, in the falloff ring it put the
            // bake up to half a cell from the fragment's gate_air.
            // <SS:Nexii> 2026-09-06 (ssdeckcellsoftcore.h): that ONE step no longer floors to the texel's containing
            // cell - "the bake, the fragment and precip agree on S wherever their quantized cells agree" was true and
            // was also the defect, because it means S is piecewise constant and steps at every cell wall. Both the
            // bake and the fragment read SSDeckCellSoft::heroInfluenceSoft now, which is the same number at every
            // cell CENTRE and continuous between them, so they agree everywhere rather than only per cell.
            // Hoisted above the occupancy block so every read in this loop shares the one texel_gate_pt.
            const SSDeckFrame::Vec2 texel_shift = heroShiftAt(ax, ay);
            const SSDeckFrame::Vec2 texel_gate_pt = SSDeckFrame::gateAir(SSDeckFrame::Vec2{ax, ay}, texel_shift);

            // The four nearest cells' verdicts, cubic-eased over one cell - the veil's own softening, so the shadow's edges fall where the outermost puffs of an occupied cell reach. NEW-A fix: indexed from texel_gate_pt (the same shifted point presence/tower/mottle read below), not the raw texel - the cell_occ grid above is padded by heroShiftPadCells so this lookup stays in bounds even at the displacement cap.
            const F32 qx = texel_gate_pt.x / CELL_M - 0.5f;
            const F32 qy = texel_gate_pt.y / CELL_M - 0.5f;
            const S32 bx = llfloor(qx);
            const S32 by = llfloor(qy);
            const F32 sx = cubic_step(qx - (F32)bx);
            const F32 sy = cubic_step(qy - (F32)by);
            const S32 ix = llclamp(bx - c0x, 0, cn - 2);
            const S32 iy = llclamp(by - c0y, 0, cn - 2);
            const F32 o00 = cell_occ[(size_t)iy * cn + ix];
            const F32 o10 = cell_occ[(size_t)iy * cn + ix + 1];
            const F32 o01 = cell_occ[(size_t)(iy + 1) * cn + ix];
            const F32 o11 = cell_occ[(size_t)(iy + 1) * cn + ix + 1];
            const F32 occ = lerp(lerp(o00, o10, sx), lerp(o01, o11, sx), sy);

            // <SS:Nexii> Phase 6b (ssdecknoisecore.h CONTRACT): the mottle read below used to be a second, direct
            // noiseSample call at the same point - now it takes the 5-arg overload's raw_n out-param instead, so
            // the mottle is the SAME de-tiled field presence/tower were just derived from, not a plain single read
            // that would show the map's un-broken tiling pattern under a de-tiled hole/tower carve.
            F32 presence = 1.f;
            F32 tower = 0.f;
            F32 n = 0.f;
            noiseFieldAt(deck, texel_gate_pt.x, texel_gate_pt.y, presence, tower, n);
            // noiseFieldAt's raw_n out-param stays at its 0.f default (not a negative sentinel) whenever
            // noiseSample's own "not ready" gate trips - mirror that exact gate here, the same one noiseSample
            // checks internally, rather than trusting n's value to tell ready from a genuine zero-luma sample.
            if (deck.mNoiseW <= 0 || deck.mNoiseH <= 0 || deck.mNoiseLuma.empty() || deck.mNoiseTileM <= 0.f) n = 0.55f;

            const F32 dens = occ * presence * llclamp(0.25f + 1.1f * n, 0.f, 1.f);

            const F32 tex_dx = ax - cam_air_wx;
            const F32 tex_dy = ay - cam_air_wy;
            const F32 tex_dist = sqrtf(tex_dx * tex_dx + tex_dy * tex_dy);
            const F32 tau_here = tau_scale * SSDeckLod::shadowTauScale(tex_dist, shadow_dial);

            const U8 b = (U8)llround(llclamp(expf(-dens * tau_here), 0.f, 1.f) * 255.f);

            const size_t idx = ((size_t)ty * N + tx) * 3;
            data[idx + 0] = b;
            data[idx + 1] = b;
            data[idx + 2] = b;
        }
    }

    mShadowRef = LLViewerTextureManager::getLocalTexture(raw.get(), true);
    if (mShadowRef.notNull() && mShadowRef->getGLTexture())
    {
        // Clamped, not wrapped: beyond the grid there is no verdict, and the soften shader's edge fade must meet a stable border texel, never the far side of the map.
        mShadowRef->getGLTexture()->setAddressMode(LLTexUnit::TAM_CLAMP);
    }
    mShadowRaw = raw;
    mShadowOriginX = ox;
    mShadowOriginY = oy;
    mShadowSpanM = span;
    mShadowKey = key;
    mShadowValid = mShadowRef.notNull();
}

// <SS:Nexii> The ground shadow's per-frame bind - see the header note. The grid uniform carries the WORLD frame (air origin plus live drift, folded here so the shader never knows the air frame exists), and the gate multiplies together every reason not to draw: the dial, the beam (no direct light, no shadow to cast), and the graze fade - below ~10 degrees the projection stretches toward infinity and real cloud shadow dissolves into the general dusk anyway, so it eases out rather than smearing one texel across a region.
void SSVolCloud::bindGroundShadow(LLGLSLShader& shader)
{
    static LLStaticHashedString s_cshadow_grid("ss_cshadow_grid");
    static LLStaticHashedString s_cshadow_sun("ss_cshadow_sun");
    static LLCachedControl<F32> strength_setting(gSavedSettings, "SSAtmoCloudGroundShadow", 0.7f);

    // The light's true WORLD direction: the sun through the whole rise band (lightnorm hands the direction to the moon at centre-set - see skyV.glsl's ss_sun_dir note), the shared light otherwise - so a moon-lit night projects along the moon. The beam gate below keeps night shadows faint in practice: mBeam tracks the direct light's strength, and a shadow is only as strong as the light it interrupts.
    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    LLVector3 sun_w = mLightDir;
    if (applier.isActive() && applier.sunRiseFraction() > 0.001f)
    {
        sun_w = applier.sunSlotDirection();
    }

    F32 gate = 0.f;
    const F32 strength = llclamp((F32)strength_setting, 0.f, 1.f);
    if (mShadowValid && mShadowRef.notNull() && strength > 0.f && !mPrimary.mPuffs.empty()
        && mShadowSpanM > 1.f)
    {
        const F32 graze = ss_smoothstep(0.06f, 0.18f, sun_w.mV[VZ]);
        gate = strength * graze * mBeam;
    }

    if (gate > 0.001f)
    {
        if (shader.bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, mShadowRef, LLTexUnit::TT_TEXTURE) < 0)
        {
            gate = 0.f;
        }
    }

    const LLVector2 drift = applier.cloudDriftMetres();
    shader.uniform4f(s_cshadow_grid,
                     mShadowOriginX + drift.mV[0],
                     mShadowOriginY + drift.mV[1],
                     (mShadowSpanM > 1.f) ? 1.f / mShadowSpanM : 0.f,
                     gate);
    shader.uniform4f(s_cshadow_sun,
                     sun_w.mV[VX], sun_w.mV[VY], llmax(sun_w.mV[VZ], 0.05f),
                     mPrimary.mBaseZ + mPrimary.mThicknessM * 0.35f);

    // The camera's world frame for the shader's view-to-world step, under custom names because the reserved inv_modelview is re-synced from the LIVE matrix stack at draw time - which a full-screen pass may have loaded with identity. Axes in xyz, origin spread across the w channels.
    static LLStaticHashedString s_cshadow_r("ss_cshadow_r");
    static LLStaticHashedString s_cshadow_u("ss_cshadow_u");
    static LLStaticHashedString s_cshadow_f("ss_cshadow_f");
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam_origin = camera->getOrigin();
    const LLVector3 cam_right = camera->getLeftAxis() * -1.f;
    const LLVector3 cam_up = camera->getUpAxis();
    const LLVector3 cam_fwd = camera->getAtAxis();
    shader.uniform4f(s_cshadow_r, cam_right.mV[VX], cam_right.mV[VY], cam_right.mV[VZ], cam_origin.mV[VX]);
    shader.uniform4f(s_cshadow_u, cam_up.mV[VX], cam_up.mV[VY], cam_up.mV[VZ], cam_origin.mV[VY]);
    shader.uniform4f(s_cshadow_f, cam_fwd.mV[VX], cam_fwd.mV[VY], cam_fwd.mV[VZ], cam_origin.mV[VZ]);
}

// How much light survives from A to B through the primary deck's puffs - grid-accelerated, drives lightning occlusion. The under deck sits below the weather and is nobody's occluder.
F32 SSVolCloud::transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength)
{
    if (mPrimary.mPuffs.empty() || strength <= 0.f) return 1.f;

    if (mOccGridDirty)
    {
        mOccGrid.clear();
        mMaxPuffR = 0.f;
        for (S32 i = 0; i < (S32)mPrimary.mPuffs.size(); ++i)
        {
            const Puff& p = mPrimary.mPuffs[(size_t)i];
            const F32 r = p.mRadius * PUFF_WIDE;
            mMaxPuffR = llmax(mMaxPuffR, r);
            const S32 x0 = llfloor((p.mPosAgent.mV[VX] - r) / CELL_M);
            const S32 x1 = llfloor((p.mPosAgent.mV[VX] + r) / CELL_M);
            const S32 y0 = llfloor((p.mPosAgent.mV[VY] - r) / CELL_M);
            const S32 y1 = llfloor((p.mPosAgent.mV[VY] + r) / CELL_M);
            for (S32 gy = y0; gy <= y1; ++gy)
            {
                for (S32 gx = x0; gx <= x1; ++gx)
                {
                    mOccGrid[((U64)(U32)gx << 32) | (U64)(U32)gy].push_back(i);
                }
            }
        }
        mOccStamp.assign(mPrimary.mPuffs.size(), 0u);
        mOccQuery = 0;
        mOccGridDirty = false;
    }

    const LLVector3 d = to_agent - from_agent;
    const F32 z_lo = mPrimary.mBaseZ - mMaxPuffR;
    const F32 z_hi = mPrimary.mBaseZ + mPrimary.mThicknessM + mMaxPuffR;
    F32 t0 = 0.f, t1 = 1.f;
    if (llabs(d.mV[VZ]) > 0.001f)
    {
        F32 ta = (z_lo - from_agent.mV[VZ]) / d.mV[VZ];
        F32 tb = (z_hi - from_agent.mV[VZ]) / d.mV[VZ];
        if (ta > tb) { const F32 tmp = ta; ta = tb; tb = tmp; }
        t0 = llmax(0.f, ta);
        t1 = llmin(1.f, tb);
        if (t0 >= t1) return 1.f;
    }
    else if (from_agent.mV[VZ] < z_lo || from_agent.mV[VZ] > z_hi)
    {
        return 1.f;
    }

    const LLVector3 a = from_agent + d * t0;
    const LLVector3 b = from_agent + d * t1;
    const F32 d_sq = llmax(d.magVecSquared(), 0.0001f);

    ++mOccQuery;
    F32 trans = 1.f;

    const F32 len_xy = sqrtf((b.mV[VX] - a.mV[VX]) * (b.mV[VX] - a.mV[VX])
                             + (b.mV[VY] - a.mV[VY]) * (b.mV[VY] - a.mV[VY]));
    const S32 steps = llmin((S32)(len_xy / CELL_M) + 1, 64);
    for (S32 s = 0; s <= steps; ++s)
    {
        const LLVector3 px = a + (b - a) * ((F32)s / (F32)steps);
        const S32 gx = llfloor(px.mV[VX] / CELL_M);
        const S32 gy = llfloor(px.mV[VY] / CELL_M);
        auto it = mOccGrid.find(((U64)(U32)gx << 32) | (U64)(U32)gy);
        if (it == mOccGrid.end()) continue;

        for (S32 idx : it->second)
        {
            if (mOccStamp[(size_t)idx] == mOccQuery) continue;
            mOccStamp[(size_t)idx] = mOccQuery;

            const Puff& p = mPrimary.mPuffs[(size_t)idx];

            const F32 t = llclamp(((p.mPosAgent - from_agent) * d) / d_sq, 0.f, 1.f);
            const LLVector3 closest = from_agent + d * t;
            const F32 r_eff = p.mRadius * 1.15f;
            const F32 off_sq = (closest - p.mPosAgent).magVecSquared();
            if (off_sq >= r_eff * r_eff) continue;

            const F32 prof = 1.f - off_sq / (r_eff * r_eff);
            trans *= 1.f - llclamp(p.mAlpha * prof * strength, 0.f, 1.f);
            if (trans < 0.004f) return 0.f;
        }
    }
    return trans;
}

// The scene depth copy for the soft fades, once per frame for every weather pass that asks.
LLRenderTarget* SSVolCloud::ensureSceneDepthCopy()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    const S32 view_w = (S32)gGLViewport[2];
    const S32 view_h = (S32)gGLViewport[3];
    if (view_w <= 0 || view_h <= 0 || !gCopyDepthProgram.isComplete()) return nullptr;

    if (mDepthCopyFrame == frame && (S32)mDepthCopy.getWidth() == view_w && (S32)mDepthCopy.getHeight() == view_h)
    {
        return &mDepthCopy;
    }

    if ((S32)mDepthCopy.getWidth() != view_w || (S32)mDepthCopy.getHeight() != view_h)
    {
        mDepthCopy.release();
        if (!mDepthCopy.allocate(view_w, view_h, GL_RGBA, true)) return nullptr;
    }

    {
        LL_PROFILE_GPU_ZONE("atmo cloud depth copy");

        LLGLDepthTest copy_depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

        gPipeline.mRT->screen.flush();
        mDepthCopy.bindTarget();

        gCopyDepthProgram.bind();

        S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
        S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
        gGL.getTexUnit(diff_map)->bind(&gPipeline.mRT->screen);
        gGL.getTexUnit(depth_map)->bind(&gPipeline.mRT->deferredScreen, true);

        gGL.setColorMask(false, false);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.setColorMask(true, true);

        gCopyDepthProgram.unbind();

        mDepthCopy.flush();
        gPipeline.mRT->screen.bindTarget();
    }
    mDepthCopyFrame = frame;
    return &mDepthCopy;
}

// Draws the sorted puffs as camera-faced billboards with soft depth, storm lighting and strike flashes.
void SSVolCloud::render()
{
    if ((mPrimary.mPuffs.empty() && mUnder.mPuffs.empty())) return;
    if (!gSSVolCloudProgram.isComplete()) return;

    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }

    // <SS:Nexii> Depth copy: taken once before either deck draws - the primary deck is the occluder the soft edges belong to, and the under deck at the bottom of a build blends against world geometry plus the primary deck above it in the one copy.
    LL_PROFILE_GPU_ZONE("atmo volumetric clouds");

    const bool have_depth_copy = (ensureSceneDepthCopy() != nullptr);

    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    gGL.setColorMask(true, false);

    gSSVolCloudProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    static LLStaticHashedString s_drift("ss_drift");
    // <SS:Nexii> D3: ss_time is retired - see the boil-clock block in update() and ssdeckboilcore.h. These two
    // are the folded accumulators that replace it, uploaded per deck below because both rates are per deck.
    static LLStaticHashedString s_boil_laps("ss_boil_laps");
    static LLStaticHashedString s_fall_m("ss_fall_m");
    static LLStaticHashedString s_churn("ss_churn");
    static LLStaticHashedString s_anvil("ss_anvil");
    static LLStaticHashedString s_base_z("ss_base_z");
    static LLStaticHashedString s_thick("ss_layer_thick");
    static LLStaticHashedString s_tex_mix("ss_tex_mix");
    static LLStaticHashedString s_base_blend("ss_base_blend");
    static LLStaticHashedString s_detail_blend("ss_detail_blend");
    static LLStaticHashedString s_puff_density("ss_puff_density");
    static LLStaticHashedString s_detail_scale("ss_detail_scale");
    static LLStaticHashedString s_drift_rate("ss_drift_rate");
    static LLStaticHashedString s_noise_tile("ss_noise_tile");
    static LLStaticHashedString s_noise_hole("ss_noise_hole");
    static LLStaticHashedString s_coverage("ss_coverage");
    static LLStaticHashedString s_cell_salt("ss_cell_salt");
    static LLStaticHashedString s_tower_ramp("ss_tower_ramp");
    static LLStaticHashedString s_profile("ss_profile");
    static LLStaticHashedString s_sheet("ss_sheet");
    static LLStaticHashedString s_wind("ss_wind");
    static LLStaticHashedString s_strike("ss_strike");
    static LLStaticHashedString s_strike_count("ss_strike_count");
    static LLStaticHashedString s_strike_color("ss_strike_color");
    static LLStaticHashedString s_strike_occ("ss_strike_occ");
    static LLStaticHashedString s_light_dir("ss_light_dir");
    static LLStaticHashedString s_gloom("ss_gloom");
    static LLStaticHashedString s_light_variant("ss_light_variant");
    static LLStaticHashedString s_cam_pos("ss_cam_pos");
    static LLStaticHashedString s_beam("ss_beam");
    static LLStaticHashedString s_rim("ss_rim");
    static LLStaticHashedString s_edge_rails("ss_edge_rails");
    static LLStaticHashedString s_veil_rails("ss_veil_rails");
    static LLStaticHashedString s_squash("ss_squash");
    static LLStaticHashedString s_clip("ss_clip");
    static LLStaticHashedString s_soft("ss_soft_m");
    static LLStaticHashedString s_storm_n("ss_storm_n");
    static LLStaticHashedString s_storm_a("ss_storm_a");
    static LLStaticHashedString s_storm_b("ss_storm_b");
    static LLStaticHashedString s_storm_c("ss_storm_c");
    static LLStaticHashedString s_line_a("ss_line_a");
    static LLStaticHashedString s_line_b("ss_line_b");
    static LLStaticHashedString s_line_c("ss_line_c");
    // <SS:Nexii> Phase 4, revised 2026-09-05 (doc/atmo_magic_wind_profile.md section 4, doc/atmo_magic_storm_dynamics.md
    // section 3): the deck's frame transforms - see SSDeckFrameCore. Stale claim corrected: ss_shear_z/ss_shear_table are
    // NOT weather-pass-only - EVERY deck's pass uploads its own baked table (never gated on weather_pass; see the upload
    // site below - the shear lean is a wind-profile property of this deck's own column, not a storm phenomenon), and they
    // are NOT unread - ssVolCloudF.glsl's ss_frame_shearTableAt / ss_frame_frameAir are called ONCE, UNCONDITIONALLY, on
    // every fragment of both the puff and sheet paths (main()'s shared frame block, before the ss_sheet branch), so the
    // table is read there too even though the sheet's OWN pattern reads then take gate_air, not shape_air. NEW-5 fix
    // (2026-09-05), residue corrected same day: the sheet is NOT at the deck floor - it is drawn 46-106 m above z0 - so
    // this is not "shape_air == gate_air because O(z) happens to be zero there"; per ssdeckframecore.h's corrected
    // contract the veil sheet is base-anchored BY DEFINITION and every sheet read (presence, gate, mottle) uses gateAir
    // unconditionally, never routing through shape_air/frameAir at all, at whatever height it is drawn. shape_air is
    // still computed once per fragment in the shared frame block above (cheap, and the puff path needs it), the sheet
    // path simply never reads it - one shared call site upstream, but the sheet's own reads are gate_air by definition,
    // not by a coincidence of z0.
    // ss_hero/ss_hero_c/ss_drift_vel below ARE gated to the weather pass, zero-filled for the under deck's pass, same
    // rule as ss_storm_n - see their own upload comment.
    static LLStaticHashedString s_shear_z("ss_shear_z");
    static LLStaticHashedString s_shear_table("ss_shear_table");
    static LLStaticHashedString s_hero("ss_hero");
    static LLStaticHashedString s_hero_c("ss_hero_c");
    static LLStaticHashedString s_drift_vel("ss_drift_vel");
    // <SS:Nexii> F2 (2026-09-06 review): the virga embed span, gated to the weather pass exactly like ss_storm_n/
    // ss_hero above (mShaftEmbedTopZ is only ever non-zero for weatherDeck()'s own build) - see the upload site.
    static LLStaticHashedString s_shaft_embed("ss_shaft_embed");

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    // <SS:Nexii> The streak frame's direction is the CURVE-RESOLVED wind at the deck base (SSAtmoEnvApplier::windAt), not the normalised drift accumulator: the accumulator now wraps into [0, span), so its direction is meaningless from the first frame and snaps at every wrap - and which way the wind blows was never a question an accumulated distance could answer. [interaction: SSAtmoEnvApplier wind profile]
    LLVector2 wind = SSAtmoEnvApplier::instance().windAt(mPrimary.mBaseZ);
    if (wind.length() < 0.001f)
    {
        wind.setVec(1.f, 0.f);
    }
    else
    {
        wind.normalize();
    }

    {
        const LLColor3 lit = SSAtmoMagic::getInstance()->lightningColor();
        gSSVolCloudProgram.uniform3fv(s_strike_color, 1, lit.mV);

        const S32 count = llmin((S32)mStrikeLights.size(), SS_MAX_STRIKE_LIGHTS);
        gSSVolCloudProgram.uniform1i(s_strike_count, count);
        if (count > 0)
        {
            gSSVolCloudProgram.uniform4fv(s_strike, count,
                                          (F32*)mStrikeLights.data());
        }

        static LLCachedControl<F32> occl_setting(gSavedSettings, "SSAtmoLightningOcclusion", 0.85f);
        gSSVolCloudProgram.uniform1f(s_strike_occ, llclamp((F32)occl_setting, 0.f, 1.f));
    }

    LLVector3 light = mLightDir;
    if (light.normalize() < 0.001f) light = LLVector3::z_axis;

    gSSVolCloudProgram.uniform3fv(s_light_dir, 1, light.mV);
    gSSVolCloudProgram.uniform3fv(s_cam_pos, 1, camera->getOrigin().mV);

    gSSVolCloudProgram.uniform1f(s_beam, mBeam);

    // <SS:Nexii> S4 (doc/atmo_magic_flow_field.md section 2 S4): SSAtmoCloudLightVariant, a runtime A/B dial over the scene-proven lighting variants (ssVolCloudF.glsl's ss_light_variant note) -
    // 0 keeps today's tail bit-identical, 1/2/3 pick POWDER/HG2/FULL. Clamped so a stray Debug Settings value never indexes past the shader's own branch set.
    static LLCachedControl<S32> light_variant_setting(gSavedSettings, "SSAtmoCloudLightVariant", 0);
    gSSVolCloudProgram.uniform1i(s_light_variant, llclamp((S32)light_variant_setting, 0, 3));

    gSSVolCloudProgram.uniform2f(s_rim, FIELD_FADE_START_M, FIELD_DRAW_M * 0.98f);

    // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT), doc/atmo_magic_far_clouds.md section 2 step 3 and phase 6a
    // outcome: an INTENDED VALUE CHANGE, not a same-math relocation - the veil used to fade on its own hardcoded
    // 6800/9800 rails (ssVolCloudF.glsl, pre-phase-6a), fully independent of the puffs' own edge ramp. Those
    // numbers are gone; ss_edge_rails now carries SSDeckLod::FIELD_FADE_START_M/DECK_EDGE_M (8000/9800), so the
    // fade start moved 6800 -> 8000 and the veil's dissolve agreed with buildDeck's own SSDeckLod::edgeFade() call
    // on the puffs - both dissolving on FIELD_FADE_START_M -> DECK_EDGE_M (ss_rim above is the separate dome-band
    // rim convergence, still FIELD_DRAW_M*0.98, and is untouched). 8g CORRECTION (ssveilcore.h): the sheet reads
    // this pair INVERTED now - it is the edge_keep factor inside SSVeil::rimIn, so the veil rises to full where
    // these rails take the puffs to zero instead of dissolving with them. The pair itself, and the puffs' CPU-side
    // edgeFade it mirrors, are unchanged.
    gSSVolCloudProgram.uniform2f(s_edge_rails, SSDeckLod::FIELD_FADE_START_M, SSDeckLod::DECK_EDGE_M);

    // <SS:Nexii> 8g veil law (ssveilcore.h), doc/atmo_magic_far_clouds.md section 2 steps 2-3, doc/atmo_magic_phase8_show.md
    // section 5: the BASE VEIL's own four rails. xy is the far-thinning pair the veil's rim FADE-IN is the complement of
    // (SSDeckLod::THIN_START_M/FIELD_DRAW_M, 5000/10000), zw its outward reach past the puffs' DECK_EDGE_M
    // (SSVeil::REACH_START_M/REACH_END_M, 10000/14000). Deliberately a SECOND uniform rather than a widening of
    // ss_edge_rails: that pair is the deck's dissolve line and the puffs' CPU-side edgeFade still owns it outright -
    // the whole point of this phase is that the veil is the one surface that fades IN where the puffs fade out, so it
    // cannot share their rail, only complement it.
    gSSVolCloudProgram.uniform4f(s_veil_rails, SSDeckLod::THIN_START_M, SSDeckLod::FIELD_DRAW_M,
                                 SSVeil::REACH_START_M, SSVeil::REACH_END_M);

    gSSVolCloudProgram.uniform3f(s_squash, mSquashKnee, mSquashCap, mEffRadius);

    static const F32 SOFT_M = 112.5f;

    bool soft = have_depth_copy &&
        gSSVolCloudProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mDepthCopy, true) >= 0;

    gSSVolCloudProgram.uniform1f(s_soft, soft ? SOFT_M : 0.f);
    if (soft)
    {
        gSSVolCloudProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                     (F32)gGLViewport[2], (F32)gGLViewport[3]);
        gSSVolCloudProgram.uniform2f(s_clip, camera->getNear(), camera->getFar());
    }

    gSSVolCloudProgram.uniform2f(s_wind, wind.mV[0], wind.mV[1]);

    // <SS:Nexii> Far deck first: the primary deck lives at storm altitude and the under deck at the build's floor, so the deck whose mean puff is farther from the eye draws first and the nearer one blends over it. Each deck sets its own per-deck uniforms and textures; blending state and the shared uniforms above survive across both.
    // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): routed through SSDeckLod::underOnTop's hysteresis rather
    // than a bare mMeanDistSq compare - the two decks' mean puff distance can cross back and forth across a frame
    // or two right at the boundary (both are rebuilt every frame, and thinning moves each mean a
    // little), which used to flicker the draw order every such frame. mUnderOnTop is this call's own `prev`,
    // carried across frames on the class; the function's own ORDER_HYST_M2 band absorbs the jitter.
    mUnderOnTop = SSDeckLod::underOnTop(mUnderOnTop, mUnder.mMeanDistSq, mPrimary.mMeanDistSq);
    const bool under_on_top = mUnderOnTop;
    Deck* order[2] = { under_on_top ? &mPrimary : &mUnder,
                       under_on_top ? &mUnder    : &mPrimary };

    const LLVector3 cam_pos = camera->getOrigin();
    const LLVector3 cam_right_fallback = camera->getLeftAxis() * -1.f;

    for (Deck* deckp : order)
    {
        Deck& deck = *deckp;
        if (deck.mPuffs.empty() || deck.mTexture.isNull()) continue;

        if (!fetchDeckTextures(deck)) continue;

        gSSVolCloudProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, deck.mTextureRef, LLTexUnit::TT_TEXTURE);
        gSSVolCloudProgram.bindTexture(LLShaderMgr::CLOUD_NOISE_MAP,
                                       deck.mDetailRef.notNull() ? deck.mDetailRef.get() : deck.mTextureRef.get(),
                                       LLTexUnit::TT_TEXTURE);

        // <SS:Nexii> The crossfade partners on the spare reserved channels (bumpMap, specularMap - same reserved-name rule as altDiffuseMap above), pinned on the current maps when no fade runs so the shader's partner samples never read an unbound unit. The weights mix the pairs per sample in the fragment stage.
        LLViewerFetchedTexture* tex_next = deck.mTextureNextRef.notNull()
            ? deck.mTextureNextRef.get()
            : deck.mTextureRef.get();
        gSSVolCloudProgram.bindTexture(LLShaderMgr::BUMP_MAP, tex_next, LLTexUnit::TT_TEXTURE);
        gSSVolCloudProgram.uniform1f(s_base_blend, deck.mTextureBlend);

        LLViewerFetchedTexture* det_cur = deck.mDetailRef.notNull() ? deck.mDetailRef.get() : deck.mTextureRef.get();
        LLViewerFetchedTexture* det_next = deck.mDetailNextRef.notNull() ? deck.mDetailNextRef.get() : det_cur;
        gSSVolCloudProgram.bindTexture(LLShaderMgr::SPECULAR_MAP, det_next, LLTexUnit::TT_TEXTURE);
        gSSVolCloudProgram.uniform1f(s_detail_blend, deck.mDetailBlend);

        // <SS:Nexii> The convection noise map, bound for the fragment stage's anvil carving - the same map the field was shaped with, authored or procedural, so the shader cuts the puffs by the very geography the towers were grown from. A reserved channel (altDiffuseMap): only reserved names can be bound as textures, see the depthMap note in ssVolCloudF.glsl. Tile metres of zero tells the shader there is nothing to read.
        LLTexture* noise_map = deck.mNoiseRef.notNull()
            ? (LLTexture*)deck.mNoiseRef.get()
            : (LLTexture*)deck.mNoiseProcRef.get();
        if (noise_map)
        {
            gSSVolCloudProgram.bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, noise_map);
        }
        gSSVolCloudProgram.uniform1f(s_noise_tile, noise_map ? deck.mNoiseTileM : 0.f);
        gSSVolCloudProgram.uniform1f(s_noise_hole, deck.mNoiseHole);
        gSSVolCloudProgram.uniform2f(s_tower_ramp, deck.mNoiseTowerLo, deck.mNoiseTowerHi);

        // <SS:Nexii> Phase 3 (doc/atmo_magic_storm_dynamics.md section 3): the coupled storm cells, uploaded for
        // the WEATHER deck pass only (whichever of mPrimary/mUnder weatherDeck() resolves to this frame -
        // buildDeck's own skip keys mStormCells/mStormCellCount to that same deck's build, review S4). ss_storm_n
        // zero for the other deck's pass reads as "no cells" to the shader without this function needing to know
        // that reason. review S7: the shader DOES read these now (ssVolCloudF.glsl's ss_storm_sampleAt /
        // ss_storm_towerFromMap / ss_storm_anvilWeight, wired since phase 3) - this is not a forward-looking upload
        // ahead of an unwired consumer. Layout is ss_storm_a.xy/z/w, ss_storm_b.xyzw, ss_storm_c.xy/z, matching
        // SSStormCouple::CellUniform's own field comments field-for-field.
        {
            const bool weather_pass = (&deck == weatherDeck());
            const S32 storm_n = weather_pass ? mStormCellCount : 0;
            gSSVolCloudProgram.uniform1i(s_storm_n, storm_n);

            LLVector4 storm_a[SSStormCouple::MAX_CELLS];
            LLVector4 storm_b[SSStormCouple::MAX_CELLS];
            LLVector4 storm_c[SSStormCouple::MAX_CELLS];
            for (S32 i = 0; i < SSStormCouple::MAX_CELLS; ++i)
            {
                const SSStormCouple::CellUniform& c = weather_pass ? mStormCells[i] : SSStormCouple::CellUniform();
                storm_a[i] = LLVector4(c.x, c.y, c.radius, c.boost);
                storm_b[i] = LLVector4(c.anvil, c.meso, c.overshoot, c.mammatus);
                storm_c[i] = LLVector4(c.dirX, c.dirY, c.rotSign, 0.f);
            }
            gSSVolCloudProgram.uniform4fv(s_storm_a, SSStormCouple::MAX_CELLS, (F32*)storm_a);
            gSSVolCloudProgram.uniform4fv(s_storm_b, SSStormCouple::MAX_CELLS, (F32*)storm_b);
            gSSVolCloudProgram.uniform4fv(s_storm_c, SSStormCouple::MAX_CELLS, (F32*)storm_c);

            // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): the active squall line's band,
            // uploaded next to ss_storm_a/b/c above, same weather-pass gate (mLineBand is only ever fetched for
            // that same deck's build - see buildDeck's own fillLineBand call). Zero-filled LineBand() for the
            // other deck's pass reads as "no line" (strength <= 0) to ss_line_field, same rule as ss_storm_n 0.
            // Layout LOCKSTEP with SSStormCouple::LineBand's own field comment: ss_line_a.xy/zw, ss_line_b.xyzw,
            // ss_line_c.xy.
            {
                const SSStormCouple::LineBand& lb = weather_pass ? mLineBand : SSStormCouple::LineBand();
                gSSVolCloudProgram.uniform4f(s_line_a, lb.ox, lb.oy, lb.dirX, lb.dirY);
                gSSVolCloudProgram.uniform4f(s_line_b, lb.motX, lb.motY, lb.halfLen, lb.bandM);
                gSSVolCloudProgram.uniform4f(s_line_c, lb.shelfM, lb.strength, 0.f, 0.f);
            }

            // <SS:Nexii> Phase 4: the deck's own baked O(z) table - EVERY deck's pass uploads its own (never
            // gated on weather_pass; the shear lean is a wind-profile property of this deck's own column, not a
            // storm phenomenon - see buildDeck's unconditional bake). ss_shear_z is (z0, z1); ss_shear_table[i] is
            // o[i] as SSDeckFrame::ShearTable lays it out, so shearTableAt's GLSL twin reads the identical layout.
            gSSVolCloudProgram.uniform2f(s_shear_z, deck.mShearTable.z0, deck.mShearTable.z1);
            LLVector2 shear_table[SSDeckFrame::SHEAR_TABLE_N];
            for (S32 i = 0; i < SSDeckFrame::SHEAR_TABLE_N; ++i)
            {
                shear_table[i] = LLVector2(deck.mShearTable.o[i].x, deck.mShearTable.o[i].y);
            }
            gSSVolCloudProgram.uniform2fv(s_shear_table, SSDeckFrame::SHEAR_TABLE_N, (F32*)shear_table);

            // <SS:Nexii> Phase 4: the hero's frame-shift inputs, gated to the weather pass exactly like ss_storm_n
            // above (mHeroFrame is only ever non-zero for weatherDeck()'s own build) - ss_hero is (motion.xy,
            // ageS, radius), ss_hero_c the centre, ss_drift_vel the applier's curve-resolved rate, matching
            // SSDeckFrame::HeroFrame's own fields. Zero-filled for the under deck's pass, which reads as "no
            // hero" to heroShift's own invariant (ageS <= 0).
            const SSDeckFrame::HeroFrame& hf = weather_pass ? mHeroFrame : SSDeckFrame::HeroFrame();
            gSSVolCloudProgram.uniform4f(s_hero, hf.motion.x, hf.motion.y, hf.ageS, hf.radius);
            gSSVolCloudProgram.uniform2f(s_hero_c, hf.centre.x, hf.centre.y);
            gSSVolCloudProgram.uniform2f(s_drift_vel, hf.driftVel.x, hf.driftVel.y);

            // <SS:Nexii> F2 (2026-09-06 review), extended phase 8e item 2: the virga embed span PLUS the ground
            // reference (deck base Z, embedded stack top Z, ground reference Z, 0), gated to the weather pass
            // exactly like ss_hero/ss_storm_n above (deck.mShaftEmbedTopZ/mShaftGroundZ are only ever non-zero
            // for weatherDeck()'s own build, and only when the shaft path actually ran this build - see their own
            // reset comments). Zero-filled for the under deck's pass; that deck never draws a shaft fragment
            // (shafts only ever hang from weatherDeck()), so the degenerate (0,0,0,0) span is harmless. The z
            // component (ground reference) feeds the fragment stage's curtain-height fraction h (item 2's droop
            // and item 5's evaporation mask both read h); w is spare.
            const F32 shaft_embed_top = weather_pass ? deck.mShaftEmbedTopZ : 0.f;
            const F32 shaft_embed_ground = weather_pass ? deck.mShaftGroundZ : 0.f;
            gSSVolCloudProgram.uniform4f(s_shaft_embed, weather_pass ? deck.mBaseZ : 0.f, shaft_embed_top,
                                         shaft_embed_ground, 0.f);
        }

        // <SS:Nexii> The cell gate's inputs, for the base veil: the builder's coverage threshold and this deck's hash salt, so the veil's fragment stage can re-run the exact cell gate the puff loop above ran (cluster noise, cell hash, the presence push, gate vs coverage) and open its gaps precisely under the sky the builder left empty of puffs. [interaction: buildDeck's gate at the coverage check - the two must run the same numbers or veil and field disagree about where the deck is]
        gSSVolCloudProgram.uniform1f(s_coverage, deck.mCoverage);
        gSSVolCloudProgram.uniform1f(s_cell_salt, (F32)deck.mSalt);

        // <SS:Nexii> The vertical profile ramp, bound for the fragment stage's four vertical curves (tower weight, carve guard, cap band, base fill) on the bumpMap2 reserved channel - same rule as altDiffuseMap above: only reserved names can be bound as textures. Sampled clamped at the deck's base and lid, so the strip must address CLAMP, not the fetched default's wrap - or v 0 would blend with v 1 at both rails.
        LLTexture* profile_map = deck.mProfileRef.notNull() ? (LLTexture*)deck.mProfileRef.get() : nullptr;
        if (profile_map)
        {
            gSSVolCloudProgram.bindTexture(LLShaderMgr::BUMP_MAP2, profile_map);
        }
        gSSVolCloudProgram.uniform1f(s_profile, profile_map ? 1.f : 0.f);

        gSSVolCloudProgram.uniform2f(s_drift, drift.mV[0], drift.mV[1]);
        // <SS:Nexii> D3 [interaction: ssdeckboilcore.h]: the two folded accumulators in place of the old
        // `(F32)LLFrameTimer::getElapsedSeconds()`. The folds are EXACT, not approximations - the shader takes
        // fract() of the phase and subtracts the fall from a coordinate divided by a wrapping map's tile - so
        // the uniforms keep full F32 precision however long the viewer has been open, which the old raw
        // elapsed-seconds product did not (at ten hours of uptime an F32 second count has ~4 ms of resolution).
        gSSVolCloudProgram.uniform1f(s_boil_laps, SSDeckBoil::wrapLaps(deck.mBoilLaps));
        gSSVolCloudProgram.uniform1f(s_fall_m, SSDeckBoil::wrapFallM(deck.mFallM, SHADER_NOISE_M));
        gSSVolCloudProgram.uniform1f(s_churn, deck.mChurn);

        gSSVolCloudProgram.uniform1f(s_anvil, deck.mAnvil);

        gSSVolCloudProgram.uniform1f(s_base_z, deck.mBaseZ);
        gSSVolCloudProgram.uniform1f(s_thick, deck.mThicknessM);
        gSSVolCloudProgram.uniform1f(s_tex_mix, deck.mTextureMix);
        gSSVolCloudProgram.uniform1f(s_puff_density, deck.mPuffDensity);
        gSSVolCloudProgram.uniform1f(s_detail_scale, deck.mDetailScale);
        gSSVolCloudProgram.uniform1f(s_drift_rate, deck.mDriftRate);

        // The deck's storm gloom, once baked into every vertex colour - one number per deck, so a uniform.
        gSSVolCloudProgram.uniform1f(s_gloom, deck.mGloom);

        // <SS:Nexii> The base veil: one horizontal sheet inset into the deck's floor, camera- centred and drawn BEFORE the deck's puffs, so the field's gaps read filled - the puffs pile up over their own floor and the spaces between them show it. The sheet is the deck's underside, so it is wound to face DOWN (the camera sees its front from below); from above the deck it is a backface and rightly culls - the gaps over a deck open on what is behind the deck, not on its floor. It rides the same shader as the puffs with ss_sheet switched on: same texture, aperiodically read, same lighting vocabulary, the same fog and dome handoff - which is the whole point of it blending rather than sitting under the deck as a second material.
        gSSVolCloudProgram.uniform1f(s_sheet, 1.f);
        {
            // <SS:Nexii> Culling off for the sheet: it is wound to face down - the deck's underside - but the pass's cull state is not this function's to reason about, and a wrongly-fallen winding would silent-drop the whole layer. A two-triangle quad drawn double-sided costs nothing; the veil is soft enough that its back face reading through the deck's gaps from above reads as the floor it is.
            LLGLDisable no_cull(GL_CULL_FACE);

            const F32 z = deck.mSheetZ;

            // <SS:Nexii> The sheet is TILED, on the same air-frame cell grid the puffs are placed on (CELL_M steps about the camera's cell, corners slid back by the drift into world space), not drawn as the one camera-centred rect it used to be. The far-field squash is exact per VERTEX, and the fragment stage un-squashes per fragment along the view ray - but a fragment inside a triangle gets its drawn position by interpolation, and the squash bends the sheet's plane, so a triangle as wide as the old 10 km rect reconstructed a world position tens to hundreds of metres off its true plane point, by an amount that changes with the camera's relation to the sheet: the veil swam across the field with every camera move and its texture would not sit under the puffs. At puff-quad scale the interpolation error collapses to nothing, sheet and field read from one anchored frame, and the per-tile cull keeps the pass inside the same draw radius the puffs run.
            // <SS:Nexii> 8g (ssveilcore.h), doc/atmo_magic_phase8_show.md section 5: the sheet's reach is the VEIL's own
            // SSVeil::REACH_END_M (14000 m), no longer max(FIELD_DRAW_M, DECK_EDGE_M) (10000 m). The old reach was
            // correct for the old law - the veil died on the puffs' own edge rail at 9800, so a tile past 10 km drew
            // nothing - and is wrong for this one: reachOut holds the veil at full amplitude to 10 km and dissolves it
            // over 10-14 km, so the tiles have to be there to carry it. Two tile sizes, because CELL_M (260 m) all the
            // way to 14 km nearly doubles a draw the far-clouds doc already flags as one of the two dominant per-frame
            // costs, while a 520 m quad at 10-14 km costs under a third of a degree of extra interpolation error.
            // <SS:Nexii> 8g F1 (2026-09-06 review), THE PARTITION - the two sizes are ONE grid subdivided, not two
            // rings: walk SSVeil::COARSE_TILE_M (520 m) blocks once over coarseTileRadius(), drop a block whose own
            // CENTRE is past REACH_END_M, and then decide the size with ONE rule on that same block - centre inside
            // SSVeil::FINE_REACH_M (10000 m) emits its SUB_PER_SIDE^2 CELL_M children with no further cull, else the
            // one 520 m quad. Every surviving block is therefore tiled exactly once, so the emission is gap-free and
            // overlap-free BY CONSTRUCTION rather than by the two culls happening to agree.
            // WHAT THIS REPLACES, and why it was a defect and not a seam: the old code ran two loops, a CELL_M ring
            // culled on `centre d2 > fine_sq` and a COARSE_TILE_M ring culled on `d2 > reach_sq || d2 <= fine_sq`.
            // Both tested CENTRES while the tiles are squares, so the union was neither a partition nor a cover. Along
            // +x from a corner-aligned camera the 520 m block centred at 9880 was dropped, its outer 260 m child at
            // 10010 was dropped too, and the next coarse block only began at 10140 - so [9880, 10140] was drawn by
            // nothing; a bearing away the phase reverses and a fine tile and a coarse tile blend the veil twice.
            // Measured by unit_veil_sheet_partition.cpp over 120 rays x 10 m steps across 9000-11500 m, camera (0,0):
            // 668 of 29225 samples uncovered (radii 9910-10290) and 464 double-covered (9740-10090), against 0 and 0
            // for this loop. That is exactly the radius where the veil runs at maximum amplitude (rimIn 1, reachOut 1,
            // SSVeil::RIM_FULL 0.90), so it read as a ragged ring of transparent notches beside a brighter
            // double-blended band. The comment that stood here claimed the opposite - "a ragged half-tile seam rather
            // than an overlap ... the seam has nothing to reveal" - with no test behind it.
            // The children stay on the PUFFS' own lattice, which is what lets the fine tier be reached by subdivision
            // instead of by its own anchored loop: a block origin is (ccx0 + tx) * coarse_m + drift and coarse_m is
            // SUB_PER_SIDE * CELL_M, so every child corner is k * CELL_M + drift for an integer k - the same
            // drift-slid air frame the puff loop walks. [interaction: buildDeck's cell walk, which anchors on that
            // same floor(air/CELL_M) lattice; unit_veil_sheet_partition.cpp asserts the corners land on it]
            // Counts, camera at a tile corner: 3025 block candidates at coarse radius 27, of which 2284 survive the
            // reach cull and 1160 subdivide -> 4640 fine quads + 1124 coarse quads = 5764 (the two-ring code emitted
            // 5768, gaps and overlaps included); subdividing every surviving block would be 9136. Both counts and the
            // zero-gap/zero-overlap claim are pinned by unit_veil_sheet_partition.cpp, the source text by
            // twin_veil_law.cpp.
            const F32 fine_reach = SSVeil::FINE_REACH_M;
            const F32 fine_sq = fine_reach * fine_reach;

            const F32 coarse_m = SSVeil::COARSE_TILE_M;
            const F32 reach_sq = SSVeil::REACH_END_M * SSVeil::REACH_END_M;
            const S32 coarse_radius = SSVeil::coarseTileRadius();
            const S32 ccx0 = llfloor((cam_pos.mV[VX] - drift.mV[0]) / coarse_m);
            const S32 ccy0 = llfloor((cam_pos.mV[VY] - drift.mV[1]) / coarse_m);

            gGL.begin(LLRender::TRIANGLES);
            // r the structural form, g the buried depth the gloom grades over (the veil is the floor, so it takes the dark end whole), a the alpha ceiling - the shader multiplies its own sky light in (see the vary_color note in ssVolCloudV.glsl); b spare.
            gGL.color4f(deck.mSheetForm, Deck::SHEET_BURIED, 0.f, deck.mSheetAlpha);
            for (S32 ty = -coarse_radius; ty <= coarse_radius; ++ty)
            {
                for (S32 tx = -coarse_radius; tx <= coarse_radius; ++tx)
                {
                    const F32 bx0 = (F32)(ccx0 + tx) * coarse_m + drift.mV[0];
                    const F32 by0 = (F32)(ccy0 + ty) * coarse_m + drift.mV[1];

                    const F32 mx = bx0 + 0.5f * coarse_m - cam_pos.mV[VX];
                    const F32 my = by0 + 0.5f * coarse_m - cam_pos.mV[VY];
                    const F32 d2 = mx * mx + my * my;
                    if (d2 > reach_sq) continue;

                    const bool subdivide = (d2 <= fine_sq);
                    const S32 sub = subdivide ? SSVeil::SUB_PER_SIDE : 1;
                    const F32 step = subdivide ? CELL_M : coarse_m;

                    for (S32 sy = 0; sy < sub; ++sy)
                    {
                        for (S32 sx = 0; sx < sub; ++sx)
                        {
                            const F32 x0 = bx0 + (F32)sx * step;
                            const F32 y0 = by0 + (F32)sy * step;
                            const F32 x1 = x0 + step;
                            const F32 y1 = y0 + step;

                            // A=(x0,y0) B=(x1,y0) C=(x1,y1) D=(x0,y1); (A,D,C) and (A,C,B) run
                            // front-facing seen from underneath - the old sheet's winding, per tile.
                            // Texcoords are unused on this path.
                            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x0, y0, z).mV);
                            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x0, y1, z).mV);
                            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x1, y1, z).mV);

                            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x0, y0, z).mV);
                            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x1, y1, z).mV);
                            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x1, y0, z).mV);
                        }
                    }
                }
            }
            gGL.end();
        }
        gSSVolCloudProgram.uniform1f(s_sheet, 0.f);

        gGL.begin(LLRender::TRIANGLES);
        for (const Puff& puff : deck.mPuffs)
        {
            LLVector3 right;
            LLVector3 up;

            if (puff.mShaft)
            {
                // <SS:Nexii> Distant rain shaft (ssvirgacore.h, doc/atmo_magic_far_clouds.md section 3): Z-axis
                // billboarded, not camera-facing like an ordinary puff's disc - vertical, rotating in yaw only, so
                // the curtain never goes edge-on and vanishes the way a world-fixed quad would. right is the
                // horizontal perpendicular to the puff-to-camera direction (the same ref % normal shape the puff
                // branch below uses, with ref pinned to z_axis and normal flattened into the horizontal plane
                // rather than the puff's own facing/flatten blend). F7 (2026-09-06 review), stale claim corrected:
                // `up` is NOT purely the fixed world Z axis any more - phase 8e's wind skew (below) displaces it
                // sideways by half the card's own top-relative-to-bottom shear, so the card's height leans with
                // the wind rather than staying strictly vertical; `right` is still the plain horizontal billboard
                // axis (unaffected by the skew), and it is that leaned `up`, not a cross product, that carries the
                // slant into the quad's corners.
                LLVector3 normal = cam_pos - puff.mPosAgent;
                normal.mV[VZ] = 0.f;
                if (normal.normalize() < 0.001f)
                {
                    normal = cam_right_fallback;
                    normal.mV[VZ] = 0.f;
                    if (normal.normalize() < 0.001f) normal = LLVector3::x_axis;
                }

                LLVector3 base_right = LLVector3::z_axis % normal;
                if (base_right.normalize() < 0.001f)
                {
                    base_right = cam_right_fallback;
                }

                right = base_right * puff.mRadius;

                // <SS:Nexii> Phase 8e (ssvirgacore.h CardGeom::shearXY, doc/atmo_magic_phase8_show.md section 3b):
                // `up` no longer runs straight along world Z - it is displaced by half the card's own top-relative-
                // to-bottom skew, so the SAME vertex offset that pushes the top corners up also leans them sideways
                // by +shearXY*0.5 and the bottom corners by -shearXY*0.5 (the vertex loop below adds/subtracts
                // `up` unchanged), turning the quad into a parallelogram. `right` stays the horizontal billboard
                // axis - only the vertical edge leans. F4/F5 (2026-09-06 review): with SSVirga::SKEW_RATE_MAX
                // capping the slope at 1 (45 degrees), shearXY's own length is at most this card's own height, so
                // the slanted edge |up|*2 is at most sqrt(2) times the card's height - bounded, not unbounded
                // shear. This is NOT a claim that the whole stack reads as one continuous slanted sheet: the one
                // card whose span straddles SSVirga::skewOffsetM's magnitude-cap knee is a chord of a piecewise
                // (linear-then-clamped) path rather than two points on a single line, and its overlap partner -
                // evaluated at ITS OWN zTop/zBot, per cardGeom's own contract - generally disagrees with it across
                // their shared overlap band. A stated residual at that one boundary (see ssvirgacore.h's own
                // comment), not a rendering bug this pass tries to hide.
                up = LLVector3::z_axis * puff.mHalfHeightM + LLVector3(puff.mShearXY.mV[VX] * 0.5f, puff.mShearXY.mV[VY] * 0.5f, 0.f);
            }
            else
            {
                // <SS:Nexii> [interaction: ssdeckflowcore.h] ONE FORMULA SITE for the card's frame. This block used
                // to spell the flatten blend, the ref blend and the two cross products inline; it is
                // SSDeckFlow::cardFrame now, character for character in the same order (unit_deckflow.cpp pins its
                // invariants), because ssVolCloudF.glsl's flow block has to build the SAME frame per fragment and a
                // second, independently spelled copy is exactly how the fifth build report's reversed flow
                // happened: the shader spelled `ref = (abs(nrm.z) < 0.95) ? +Z : +X` against this blend, and the
                // two disagree by a SIGN over a third of the sky. ONE behaviour change, stated: the near-degenerate
                // fallback was cam_right_fallback (a camera vector the fragment stage cannot reproduce) and is
                // SSDeckFlow::FALLBACK_RIGHT now - reachable only within 0.0098 degrees of one direction
                // (elevation 46.3355, bearing +X), a solid-angle share of 6.4e-7.
                const LLVector3 to_cam = cam_pos - puff.mPosAgent;
                const SSDeckFlow::CardFrame card = SSDeckFlow::cardFrame(
                    SSDeckFlow::Vec3{ to_cam.mV[VX], to_cam.mV[VY], to_cam.mV[VZ] });
                const LLVector3 base_right(card.right.x, card.right.y, card.right.z);
                const LLVector3 base_up(card.up.x, card.up.y, card.up.z);

                const F32 layer_h = llclamp(
                    (puff.mPosAgent.mV[VZ] - deck.mBaseZ) / deck.mThicknessM, 0.f, 1.f);
                const F32 round = llclamp(
                    (layer_h - PUFF_ROUND_LO) / (PUFF_ROUND_HI - PUFF_ROUND_LO), 0.f, 1.f);

                const F32 wide = PUFF_WIDE + (1.f - PUFF_WIDE) * round;
                const F32 tall = PUFF_TALL + (1.f - PUFF_TALL) * round;

                right = base_right * (puff.mRadius * wide);
                up = base_up * (puff.mRadius * tall);
            }

            // <SS:Nexii> The card, exactly as it has always been: four corners, one quad, no
            // vertex displacement of any kind. Near-field detail is built geometry placed by the
            // builder (the anatomy tier's entities from 8b on; the old refinement LOD was removed
            // 2026-09-06), so this pass has nothing per-puff to decide and nothing to re-roll: what
            // moves with the wind is the whole card, and what the fragment carve reads stays put with it. Displacing rim
            // vertices here was tried and cut twice over - the seed keyed on the world position
            // re-rolled against the drift every frame, and even anchored it reshaped the one puff
            // the LOD was meant to leave alone.
            // r the structural form, g the buried depth (Puff::mBuried) the storm gloom grades
            // over, a the edge fade - the shader multiplies its own sky light in (see the
            // vary_color note in ssVolCloudV.glsl); b encodes the shaft flag AND (phase 8e item 4,
            // DRIVE REACHES THE FRAGMENT) the card's own drive: 0.5 + 0.5 * puff.mDrive for a virga
            // card (always > 0.5, since a qualifying cell's drive is always > 0); an ordinary puff
            // (S2, Puff::mPhase) carries mPhase * 0.49 instead of the old flat 0, which stays well
            // under the shaft flag's 0.5 threshold (0.49 * 255 = 124.95, rounds to 125/255 = 0.4902 -
            // margin holds under U8 quantization) - the fragment stage's branch test
            // (vary_color.b > 0.5) is unchanged, and the shaft path recovers drive as
            // clamp((vary_color.b - 0.5) * 2.0, 0.0, 1.0) to shape its body floor, streak
            // amplitude, and evaporation mask by intensity. The sheet keeps its own flat 0.
            // [interaction: ssVolCloudF.glsl's shaft decode and puff phase decode]
            gGL.color4f(puff.mForm, puff.mBuried, puff.mShaft ? (0.5f + 0.5f * puff.mDrive) : (puff.mPhase * 0.49f), puff.mAlpha);

            if (puff.mShaft)
            {
                // <SS:Nexii> S1 vertex-decode channel fix (doc/atmo_magic_flow_field.md section 2): shafts carry
                // NO texcoord payload - these four corners stay the exact 0/1 markers they have always been, read
                // by the shader's shaft branch as real card u/v (vbox/hedge/p_shaft), never as a corner-plus-payload
                // pair. Untouched by this stage; see the else branch below for the puff path that DOES ride the
                // payload encode.
                gGL.texCoord2f(0.f, 1.f); gGL.vertex3fv((puff.mPosAgent - right + up).mV);
                gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv((puff.mPosAgent - right - up).mV);
                gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv((puff.mPosAgent + right + up).mV);

                gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv((puff.mPosAgent + right + up).mV);
                gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv((puff.mPosAgent - right - up).mV);
                gGL.texCoord2f(1.f, 0.f); gGL.vertex3fv((puff.mPosAgent + right - up).mV);
            }
            else
            {
                // <SS:Nexii> S1 vertex-decode channel fix (doc/atmo_magic_flow_field.md section 2): ordinary puffs
                // now write texcoord0 = cornerMarker + payload * 0.45, decoded back in ssVolCloudV.glsl by
                // round()/0.45 before interpolation ever sees it (opus review Q2b: recovering payload AFTER
                // interpolation is unsound, so the round() must happen here, pre-rasterizer). The encode is exact
                // over the whole payload domain [-1,1]^2 (V:\Scratch\flow\tests\twin_flowchannel.cpp's grid and
                // extremes tests: |payload * 0.45| <= 0.45 < 0.5, so round() can never cross into the neighbouring
                // corner), and at payload 0 it is bit-identical to the old 0.f/1.f literal. STALE CLAIM REMOVED:
                // this comment used to say payload is (0,0) for every puff "at this stage" and therefore that the
                // output is bit-identical - that stopped being true below.
                // <SS:Nexii> [interaction: ssdeckflowcore.h] THE PAYLOAD IS NO LONGER ZERO: px carries this puff's
                // flow swirl (Puff::mFlowSwirl, in [-1, 1] by SSDeckFlow::swirlUnit's own range), the same value on
                // all four corners so the varying is constant across the quad and no seam or gradient can appear
                // inside one card. Clamped to the encode's own safe range rather than trusted: round() recovers the
                // corner only while |payload * 0.45| < 0.5, i.e. |payload| < 1.111, and twin_flowchannel.cpp pins
                // the decode at the extremes. py stays 0 - the second payload slot is still unspent.
                const F32 px = llclamp(puff.mFlowSwirl, -1.f, 1.f);
                const F32 py = 0.f;
                gGL.texCoord2f(0.f + px * 0.45f, 1.f + py * 0.45f); gGL.vertex3fv((puff.mPosAgent - right + up).mV);
                gGL.texCoord2f(0.f + px * 0.45f, 0.f + py * 0.45f); gGL.vertex3fv((puff.mPosAgent - right - up).mV);
                gGL.texCoord2f(1.f + px * 0.45f, 1.f + py * 0.45f); gGL.vertex3fv((puff.mPosAgent + right + up).mV);

                gGL.texCoord2f(1.f + px * 0.45f, 1.f + py * 0.45f); gGL.vertex3fv((puff.mPosAgent + right + up).mV);
                gGL.texCoord2f(0.f + px * 0.45f, 0.f + py * 0.45f); gGL.vertex3fv((puff.mPosAgent - right - up).mV);
                gGL.texCoord2f(1.f + px * 0.45f, 0.f + py * 0.45f); gGL.vertex3fv((puff.mPosAgent + right - up).mV);
            }
        }
        gGL.end();
    }

    gGL.flush();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSVolCloudProgram.unbind();

    gGL.setColorMask(true, true);
}

// Binds a deck's authored textures, falling back to the sky dome's noise for an empty detail slot - per deck, since the two may carry different maps.
bool SSVolCloud::fetchDeckTextures(Deck& deck)
{
    // <SS:Nexii> The convection noise map: fetched like the other maps, then read back out of VRAM once it has one - the way sculpties read theirs - into the small wrapped grid the builder and the precipitation gate sample on the CPU. The GPU reads the same map through its own binding in render(), for the anvil's carving; the CPU grid is the same geography at field scale. readbackRawImage keeps its raw copy current as better mips stream in, so re-caching whenever that copy's size changes keeps both sides honest through the load.
    if (deck.mNoise.notNull())
    {
        if (deck.mNoiseRef.isNull() || deck.mNoiseRef->getID() != deck.mNoise)
        {
            deck.mNoiseRef = LLViewerTextureManager::getFetchedTexture(
                deck.mNoise, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (deck.mNoiseRef.notNull())
            {
                deck.mNoiseRef->setNoDelete();
            }
            deck.mNoiseW = 0;
            deck.mNoiseH = 0;
            deck.mNoiseSrcW = 0;
            deck.mNoiseSrcH = 0;
            deck.mNoiseLuma.clear();
        }

        if (deck.mNoiseRef.notNull())
        {
            deck.mNoiseRef->addTextureStats((F32)MAX_IMAGE_AREA);
            deck.mNoiseProcRaw = nullptr;
            deck.mNoiseProcRef = nullptr;

            LLImageRaw* raw = deck.mNoiseRef->getRawImage();
            if (!raw || raw->getWidth() < deck.mNoiseRef->getWidth()
                     || raw->getHeight() < deck.mNoiseRef->getHeight())
            {
                deck.mNoiseRef->readbackRawImage();
                raw = deck.mNoiseRef->getRawImage();
            }

            if (raw && (raw->getWidth() != deck.mNoiseSrcW
                     || raw->getHeight() != deck.mNoiseSrcH))
            {
                cacheNoiseGrid(deck, raw);
            }
        }
    }
    else
    {
        // <SS:Nexii> Nothing authored: the procedural map is the noise map. Its CPU grid was folded by the builder; this is where its GPU copy uploads - once per generation - wrapped, mipmapped, and ready for the fragment stage's carving.
        deck.mNoiseRef = nullptr;
        if (deck.mNoiseProcRaw.notNull())
        {
            if (deck.mNoiseProcRef.isNull())
            {
                deck.mNoiseProcRef = LLViewerTextureManager::getLocalTexture(deck.mNoiseProcRaw.get(), true);
                if (deck.mNoiseProcRef.notNull() && deck.mNoiseProcRef->getGLTexture())
                {
                    // The map tiles, so the GL copy has to as well.
                    deck.mNoiseProcRef->getGLTexture()->setAddressMode(LLTexUnit::TAM_WRAP);
                }
            }
        }
        else if (deck.mNoiseW > 0)
        {
            deck.mNoiseLuma.clear();
            deck.mNoiseW = 0;
            deck.mNoiseH = 0;
            deck.mNoiseSrcW = 0;
            deck.mNoiseSrcH = 0;
        }
    }

    // <SS:Nexii> The vertical profile ramp: authored only (none runs the built-in curves), read back through the same ladder as the noise map and folded into one averaged curve per channel. The readback's rows arrive in GL order - row 0 is v 0, the deck's base - which is exactly the orientation the shader's own texture read samples, so CPU and GPU run one profile however the author painted it.
    if (deck.mProfile.notNull())
    {
        if (deck.mProfileRef.isNull() || deck.mProfileRef->getID() != deck.mProfile)
        {
            deck.mProfileRef = LLViewerTextureManager::getFetchedTexture(
                deck.mProfile, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (deck.mProfileRef.notNull())
            {
                deck.mProfileRef->setNoDelete();
                if (deck.mProfileRef->getGLTexture())
                {
                    // The ramp runs base to lid and clamps at both rails; a wrapped strip
                    // would blend its ends together at v 0 and v 1.
                    deck.mProfileRef->getGLTexture()->setAddressMode(LLTexUnit::TAM_CLAMP);
                }
            }
            deck.mProfileN = 0;
            deck.mProfileCurve.clear();
        }

        if (deck.mProfileRef.notNull())
        {
            deck.mProfileRef->addTextureStats((F32)MAX_IMAGE_AREA);

            LLImageRaw* raw = deck.mProfileRef->getRawImage();
            if (!raw || raw->getWidth() < deck.mProfileRef->getWidth()
                     || raw->getHeight() < deck.mProfileRef->getHeight())
            {
                deck.mProfileRef->readbackRawImage();
                raw = deck.mProfileRef->getRawImage();
            }

            if (raw && raw->getHeight() != deck.mProfileN)
            {
                cacheProfileCurve(deck, raw);
            }
        }
    }
    else if (deck.mProfileRef.notNull() || deck.mProfileN > 0)
    {
        deck.mProfileRef = nullptr;
        deck.mProfileCurve.clear();
        deck.mProfileN = 0;
    }
    else if (deck.mProfileProcRef.isNull())
    {
        // <SS:Nexii> Nothing authored: paint the built-in curves once so the picker's placeholder preview has something honest to show for the None state. Display only - the shader runs these curves as maths, never as a texture.
        LLPointer<LLImageRaw> strip = makeProfilePreview();
        if (strip.notNull())
        {
            deck.mProfileProcRef = LLViewerTextureManager::getLocalTexture(strip.get(), true);
        }
    }

    if (deck.mTextureRef.isNull() || deck.mTextureRef->getID() != deck.mTexture)
    {
        deck.mTextureRef = LLViewerTextureManager::getFetchedTexture(
            deck.mTexture, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (deck.mTextureRef.notNull())
        {
            deck.mTextureRef->setNoDelete();
        }
    }
    if (deck.mTextureRef.isNull()) return false;
    deck.mTextureRef->addTextureStats((F32)MAX_IMAGE_AREA);

    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const LLUUID detail_id = deck.mDetail.notNull()
        ? deck.mDetail
        : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
    if (detail_id.notNull() && (deck.mDetailRef.isNull() || deck.mDetailRef->getID() != detail_id))
    {
        deck.mDetailRef = LLViewerTextureManager::getFetchedTexture(
            detail_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (deck.mDetailRef.notNull())
        {
            deck.mDetailRef->setNoDelete();
        }
    }
    if (deck.mDetailRef.notNull())
    {
        deck.mDetailRef->addTextureStats((F32)MAX_IMAGE_AREA);
    }

    // <SS:Nexii> The crossfade partners, fetched only while a fade is live - the same ladder as the primaries, keyed by id so a fade holds one fetch. The detail partner falls back to the dome's cloud noise exactly as the primary does; a partner that lands on the current map is skipped and the renderer pins that pair on the primary, so a fade to the same texture costs nothing. Dropped the moment the weight reaches the rail.
    deck.mTextureNextRef = nullptr;
    deck.mDetailNextRef = nullptr;
    if (deck.mTextureBlend > 0.f && deck.mTextureNext.notNull() && deck.mTextureNext != deck.mTexture)
    {
        if (deck.mTextureNextRef.isNull() || deck.mTextureNextRef->getID() != deck.mTextureNext)
        {
            deck.mTextureNextRef = LLViewerTextureManager::getFetchedTexture(
                deck.mTextureNext, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (deck.mTextureNextRef.notNull())
            {
                deck.mTextureNextRef->setNoDelete();
            }
        }
        if (deck.mTextureNextRef.notNull())
        {
            deck.mTextureNextRef->addTextureStats((F32)MAX_IMAGE_AREA);
        }
    }

    if (deck.mDetailBlend > 0.f)
    {
        const LLUUID detail_cur_id = deck.mDetail.notNull()
            ? deck.mDetail
            : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
        const LLUUID detail_next_id = deck.mDetailNext.notNull()
            ? deck.mDetailNext
            : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
        if (detail_next_id.notNull() && detail_next_id != detail_cur_id)
        {
            if (deck.mDetailNextRef.isNull() || deck.mDetailNextRef->getID() != detail_next_id)
            {
                deck.mDetailNextRef = LLViewerTextureManager::getFetchedTexture(
                    detail_next_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
                if (deck.mDetailNextRef.notNull())
                {
                    deck.mDetailNextRef->setNoDelete();
                }
            }
            if (deck.mDetailNextRef.notNull())
            {
                deck.mDetailNextRef->addTextureStats((F32)MAX_IMAGE_AREA);
            }
        }
    }

    return true;
}

// Box-averages the noise map's raw image into the deck's fixed wrapped grid, luminance only: each destination texel the mean of the source block it covers. The field's structure is kilometres
// wide, so a 64-across grid carries it whole; the point of shrinking it is that the builder and the precipitation gate sample it thousands of times a frame.
void SSVolCloud::cacheNoiseGrid(Deck& deck, LLImageRaw* raw)
{
    const S32 sw = raw->getWidth();
    const S32 sh = raw->getHeight();
    const S32 comps = raw->getComponents();
    if (sw <= 0 || sh <= 0 || comps < 1) return;

    const U8* data = raw->getData();
    deck.mNoiseLuma.assign((size_t)SS_NOISE_GRID * SS_NOISE_GRID, 0.f);

    for (S32 gy = 0; gy < SS_NOISE_GRID; ++gy)
    {
        const S32 sy0 = llclamp((S32)((F64)gy * sh / SS_NOISE_GRID), 0, sh - 1);
        const S32 sy1 = llclamp(llmax(sy0 + 1, (S32)((F64)(gy + 1) * sh / SS_NOISE_GRID)), 1, sh);
        for (S32 gx = 0; gx < SS_NOISE_GRID; ++gx)
        {
            const S32 sx0 = llclamp((S32)((F64)gx * sw / SS_NOISE_GRID), 0, sw - 1);
            const S32 sx1 = llclamp(llmax(sx0 + 1, (S32)((F64)(gx + 1) * sw / SS_NOISE_GRID)), 1, sw);

            F64 sum = 0.0;
            for (S32 y = sy0; y < sy1; ++y)
            {
                const U8* row = data + (size_t)y * sw * comps;
                for (S32 x = sx0; x < sx1; ++x)
                {
                    const U8* px = row + (size_t)x * comps;
                    // Luminance the same way the shaders read their maps - the mean of RGB
                    // when there are three channels to mean, the one channel otherwise.
                    const F32 lum = (comps >= 3)
                        ? (F32)(px[0] + px[1] + px[2]) / 765.f
                        : (F32)px[0] / 255.f;
                    sum += lum;
                }
            }
            deck.mNoiseLuma[(size_t)gy * SS_NOISE_GRID + gx] = (F32)(sum / (F64)((sy1 - sy0) * (sx1 - sx0)));
        }
    }

    deck.mNoiseW = SS_NOISE_GRID;
    deck.mNoiseH = SS_NOISE_GRID;
    deck.mNoiseSrcW = sw;
    deck.mNoiseSrcH = sh;
}

// The procedural fallback map: one square tileable FBM per deck salt, regrown only when the weather seed changes. Both decks of a build derive different patterns from the same seed by the
// salt, and every client sharing the environment derives the same patterns, period - the
// geography is as syncable as the weather that seeds it.
void SSVolCloud::ensureProceduralNoise(Deck& deck, U32 salt)
{
    const U32 seed = SSAtmoMagic::getInstance()->seed() ^ (salt * 0x9E3779B9u);
    if (deck.mNoiseProcRaw.notNull() && deck.mNoiseProcSeed == seed && deck.mNoiseW > 0) return;

    deck.mNoiseProcRaw = makeProceduralNoise(seed);
    deck.mNoiseProcSeed = seed;
    deck.mNoiseProcRef = nullptr;   // the GPU copy re-uploads at fetch time

    if (deck.mNoiseProcRaw.notNull())
    {
        cacheNoiseGrid(deck, deck.mNoiseProcRaw);
    }
    else if (deck.mNoiseW > 0)
    {
        deck.mNoiseLuma.clear();
        deck.mNoiseW = 0;
        deck.mNoiseH = 0;
        deck.mNoiseSrcW = 0;
        deck.mNoiseSrcH = 0;
    }
}

// Folds the profile ramp's raw readback into one averaged curve per channel: each destination row the mean of a source row band across the full width, row 0 the deck's BASE (the readback
// arrives in GL order, the same v the shader's texture read samples). Row count fixed at
// SS_PROFILE_N - a vertical curve needs no more - and the four channels ride along so the
// tower weight, carve guard, cap band and base fill stay separate curves.
void SSVolCloud::cacheProfileCurve(Deck& deck, LLImageRaw* raw)
{
    const S32 sw = raw->getWidth();
    const S32 sh = raw->getHeight();
    const S32 comps = raw->getComponents();
    if (sw <= 0 || sh <= 0 || comps < 1) return;

    const S32 ch = llmin(comps, 4);
    const U8* data = raw->getData();

    deck.mProfileCurve.assign((size_t)SS_PROFILE_N * 4, 0.f);

    for (S32 gy = 0; gy < SS_PROFILE_N; ++gy)
    {
        const S32 sy0 = llclamp((S32)((F64)gy * sh / SS_PROFILE_N), 0, sh - 1);
        const S32 sy1 = llclamp(llmax(sy0 + 1, (S32)((F64)(gy + 1) * sh / SS_PROFILE_N)), 1, sh);

        F64 sum[4] = { 0.0, 0.0, 0.0, 0.0 };
        S32 count = 0;
        for (S32 y = sy0; y < sy1; ++y)
        {
            const U8* row = data + (size_t)y * sw * comps;
            for (S32 x = 0; x < sw; ++x)
            {
                const U8* px = row + (size_t)x * comps;
                for (S32 c = 0; c < ch; ++c)
                {
                    sum[c] += (F32)px[c] / 255.f;
                }
            }
            ++count;
        }

        if (count <= 0) continue;
        for (S32 c = 0; c < 4; ++c)
        {
            // Channels the image does not carry read as white for the tower ramp and black for
            // the rest - a grey-scale strip authors the ramp and nothing else.
            deck.mProfileCurve[(size_t)gy * 4 + c] =
                (c < ch) ? (F32)(sum[c] / (F64)count) : ((c == 0 && comps < 3) ? 1.f : 0.f);
        }
    }

    deck.mProfileN = SS_PROFILE_N;
}

// One linear read of the profile curve: v in deck-height fractions (0 base, 1 lid), channel 0..3.
F32 SSVolCloud::profileSample(const Deck& deck, F32 v, S32 channel) const
{
    const S32 n = deck.mProfileN;
    if (n <= 0 || deck.mProfileCurve.empty()) return 0.f;

    const F32 fv = llclamp(v, 0.f, 1.f) * (F32)(n - 1);
    const S32 i0 = llfloor(fv);
    const S32 i1 = llmin(i0 + 1, n - 1);
    // The fraction WITHIN the cell, off the scaled coordinate - not off v, which is the whole
    // curve's 0-1 and only happens to be the right answer in the first cell.
    const F32 t = fv - (F32)i0;

    const F32 a = deck.mProfileCurve[(size_t)i0 * 4 + channel];
    const F32 b = deck.mProfileCurve[(size_t)i1 * 4 + channel];
    return a + (b - a) * t;
}

// One wrapped bilinear read of the cached grid at an air-frame position, in map values. Anything that stops the read - no map, no readback yet - answers a negative, which every
F32 SSVolCloud::noiseSample(const Deck& deck, F32 air_x, F32 air_y) const
{
    const S32 w = deck.mNoiseW;
    const S32 h = deck.mNoiseH;
    if (w <= 0 || h <= 0 || deck.mNoiseLuma.empty() || deck.mNoiseTileM <= 0.f) return -1.f;

    const F32 fx = air_x / deck.mNoiseTileM * (F32)w - 0.5f;
    const F32 fy = air_y / deck.mNoiseTileM * (F32)h - 0.5f;

    const S32 ix = llfloor(fx);
    const S32 iy = llfloor(fy);
    const F32 tx = fx - (F32)ix;
    const F32 ty = fy - (F32)iy;

    // The map tiles, exactly as it draws: wrap both axes, so a field kilometres across never
    // falls off the edge of its own pattern.
    const S32 x0 = ((ix % w) + w) % w;
    const S32 y0 = ((iy % h) + h) % h;
    const S32 x1 = (x0 + 1) % w;
    const S32 y1 = (y0 + 1) % h;

    const F32 c00 = deck.mNoiseLuma[(size_t)y0 * w + x0];
    const F32 c10 = deck.mNoiseLuma[(size_t)y0 * w + x1];
    const F32 c01 = deck.mNoiseLuma[(size_t)y1 * w + x0];
    const F32 c11 = deck.mNoiseLuma[(size_t)y1 * w + x1];

    const F32 top = c00 + (c10 - c00) * tx;
    const F32 bot = c01 + (c11 - c01) * tx;
    return top + (bot - top) * ty;
}

// The noise map's two ramps at one point of a deck's field: presence (1 = cloud whole, 0 = a hole the map cut) and tower (0 = pocket, 1 = a rising thermal's column). The hole window's
// strength was baked at build time - moisture's floor lift and convection's storm-gap keep are
// already inside it, so every reader of the field gets the same sky.
void SSVolCloud::noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower) const
{
    F32 raw_n;
    noiseFieldAt(deck, air_x, air_y, presence, tower, raw_n);
}

// <SS:Nexii> Phase 3 overload - see the header note. presence and tower are computed exactly as the 4-arg overload
// always has (unchanged by the storm coupling); raw_n is the map sample the hole/tower ramps were run on, 0 when
// there is no map cached (matching noiseSample's own "not ready" sentinel before this function used to discard it).
// <SS:Nexii> Phase 6b (doc/atmo_magic_far_clouds.md section 2 step 4, ssdecknoisecore.h CONTRACT): this is the ONE
// place every CPU consumer of presence/n_map shares (the builder, precipNoiseAt, the shadow bake's cell loop and
// texel loop all route through here now - none of them calls noiseSample directly for presence or n_map any
// more), so the de-tile mix lives here and nowhere else on the CPU side. n = mixDetile(noiseSample(air),
// noiseSample(detileCoord(air))) - a second, incommensurate-scale-and-rotation read of the SAME map, mixed with
// the first, so the map's own tiling period stops being a visible repeat at the far edge of the field. Pure
// function of (deck, air_x, air_y) - nothing camera- or time-dependent enters this read, so the shadow bake's key
// folds nothing new for it (deck.mNoiseTileM/mSalt/mNoiseW are already folded, and DETILE_SCALE/ROT/WEIGHT are
// compile-time constants, not per-client state). raw_n out-param is the de-tiled value too, not the single-read
// raw n - every reader of raw_n (tower's map sample, precipNoiseAt's shifted read, the bake's mottle read) gets
// the same de-tiled field the hole/tower ramps below were run on. ssVolCloudF.glsl's ss_cell_occupied presence
// fetch, sheet presence read and puff-path n_map read carry the matching mix too (ss_noise_mapDetiled, the
// GLSL twin of detileCoord/mixDetile) - this comment previously read "NOT yet changed... pending" while the
// shader file already had it; that was stale, not a statement of the CPU-only state at the time it was written.
// [interaction: ssVolCloudF.glsl's ss_cell_occupied, sheet presence read, puff-path n_map read - ss_noise_mapDetiled]
void SSVolCloud::noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower, F32& raw_n) const
{
    presence = 1.f;
    tower = 0.f;
    raw_n = 0.f;

    const F32 n1 = noiseSample(deck, air_x, air_y);
    if (n1 < 0.f) return;

    // n2's own "not ready" gate reads the same deck fields (w/h/tileM) as n1's just did, so it cannot fail here
    // when n1 did not - both reads always land on the mix, exactly as the CONTRACT's formula states.
    const SSDeckNoise::Vec2 detiled = SSDeckNoise::detileCoord(SSDeckNoise::Vec2{air_x, air_y});
    const F32 n2 = noiseSample(deck, detiled.x, detiled.y);
    const F32 n = SSDeckNoise::mixDetile(n1, n2);

    raw_n = n;
    const F32 cut = ss_smoothstep(SS_HOLE_LO, SS_HOLE_HI, n);
    presence = 1.f - (1.f - cut) * deck.mNoiseHole;
    tower = ss_smoothstep(deck.mNoiseTowerLo, deck.mNoiseTowerHi, n);
}

// The deck the weather reads, resolved at build time - see update().
const SSVolCloud::Deck* SSVolCloud::weatherDeck() const
{
    return (mWeatherDeck == 1) ? &mUnder : &mPrimary;
}

// Whether the precipitation gate has anything to say: a built weather deck with a noise map read back. Checked before any rain-shadow work so a plain sky pays nothing.
bool SSVolCloud::precipNoiseReady() const
{
    const Deck* deck = weatherDeck();
    return deck && !deck->mPuffs.empty() && deck->mNoiseW > 0 && deck->mNoiseTileM > 0.f;
}

LLVector3 SSVolCloud::precipNoiseAt(const LLVector3& pos_agent) const
{
    const Deck* deck = weatherDeck();
    if (!deck || deck->mPuffs.empty() || deck->mNoiseW <= 0 || deck->mNoiseTileM <= 0.f)
    {
        return LLVector3(1.f, 0.f, 0.f);
    }

    // The air frame, the same one the cells are placed in, so the gate moves with the deck.
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    // <SS:Nexii> Phase 3 (doc/atmo_magic_storm_dynamics.md section 3): the storm's downwind precipitation shift -
    // SSStormCouple::precipShift - through this deck's coupled cells ONLY when this deck IS weatherDeck() (`deck`
    // above already is weatherDeck(), whichever of mPrimary/mUnder that resolves to this frame - buildDeck fetches
    // mStormCells/mStormCellCount for that same deck and only that deck, so a plain mStormCellCount > 0 check is
    // enough here; no separate deck-identity test is needed since `deck` cannot be anything else). review 3b NEW-7
    // (doc S3 "Sample point lockstep"): the storm sample is taken at the QUANTIZED cell centre of the landing
    // point - SSStormCouple::samplePointM - the same point buildDeck's per-cell sample and the shader's own
    // literal twin (ss_storm_samplePoint) use, not the raw sub-cell pos_agent; otherwise this reads a finer-grained
    // storm figure than the deck was actually built with, and geometry/carve/rain could disagree at a storm's
    // edge. Its boost widens the tower window exactly as the builder's does; only the noise MAP lookup feeding
    // towerFromMap moves to the shifted, upwind point, so the rain reads as fed from the updraft's actual column
    // while the updraft itself stays rain-free.
    // <SS:Nexii> Phase 4 (doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud drift"): the landing
    // point's quantized cell centre, WORLD frame - the same samplePointM call phase 3 already made for the storm
    // sample, now shared with the hero influence weight below (it no longer waits on coupled_deck: the hero's
    // shift must be known before the presence read runs, and mHeroFrame.ageS is only ever > 0 when coupled_deck
    // is also true, so this costs nothing extra on an uncoupled deck).
    F32 sample_x = 0.f;
    F32 sample_y = 0.f;
    SSStormCouple::samplePointM(pos_agent.mV[VX] - drift.mV[0], pos_agent.mV[VY] - drift.mV[1],
                                 CELL_M, drift.mV[0], drift.mV[1], sample_x, sample_y);

    F32 shift_x = 0.f;
    F32 shift_y = 0.f;
    SSStormCouple::Sample sample;
    const bool coupled_deck = mStormCellCount > 0;
    if (coupled_deck)
    {
        sample = SSStormCouple::sampleAt(mStormCells, mStormCellCount, sample_x, sample_y);
        SSStormCouple::precipShift(sample, shift_x, shift_y);
    }
    // <SS:Nexii> 8a item 2/4 (doc/atmo_magic_phase8_show.md section 3): PRECIPNOISEAT call site - folded in at the
    // same landing point regardless of coupled_deck (a line's own members can leave mStormCellCount at 0 only when
    // mCells itself is empty, which also empties the line, but this stays independent of that coincidence). sample
    // (unused by precipShift beyond meso/dir) now also carries lineWall/lineShelf, returned to the caller below.
    SSStormCouple::applyLineBand(sample, SSStormCouple::lineField(mLineBand, sample_x, sample_y), SSSquall::LINE_ANVIL_FRAC);

    // <SS:Nexii> Phase 4: the hero's rigid local shift, evaluated at the UNSHIFTED quantized cell centre
    // (sample_x, sample_y) exactly as buildDeck's own per-cell hero_shift_m is - per-cell lockstep with the
    // builder that placed this deck. Zero whenever there is no hero, making gateAir below an identity read.
    const F32 hero_influence = (mHeroFrame.ageS > 0.f)
        ? SSStormCouple::influence(mHeroFrame.centre.x, mHeroFrame.centre.y, mHeroFrame.radius, sample_x, sample_y)
        : 0.f;
    const SSDeckFrame::Vec2 hero_shift_m = SSDeckFrame::heroShift(mHeroFrame, hero_influence);

    // <SS:Nexii> review S1: presence is read at the UNDISPLACED point - pos_agent - drift, the same point the
    // builder's own cell gate ran at - or rain would fall out of holes that were never shifted. Only the raw map
    // sample feeding towerFromMap (the second lookup below) moves to the shifted, upwind point; presence and tower
    // therefore come from two separate noiseFieldAt calls whenever the deck is coupled. Phase 4: both reads now
    // additionally go through gateAir, subtracting the hero's own shift - the same GATE transform buildDeck's
    // cell loop applies, so presence/tower agree with the deck the rain is falling from at a hero's edge too.
    const SSDeckFrame::Vec2 gate_pt = SSDeckFrame::gateAir(
        SSDeckFrame::Vec2{pos_agent.mV[VX] - drift.mV[0], pos_agent.mV[VY] - drift.mV[1]}, hero_shift_m);
    F32 presence = 1.f;
    F32 tower = 0.f;
    F32 raw_n = 0.f;
    noiseFieldAt(*deck, gate_pt.x, gate_pt.y, presence, tower, raw_n);

    if (coupled_deck)
    {
        // <SS:Nexii> phase-3c fix F4: only the raw map sample at the shifted point is used here (tower is
        // re-derived from it below), so this reads raw_n through the 5-arg noiseFieldAt's out-param - the overload
        // that exposes it without discarding a presence/tower pair - instead of two now-dropped, always-discarded
        // shifted_presence/shifted_tower out-params from the 6-arg noiseFieldAt. Phase 4: gateAir applies here too,
        // ON TOP of the existing precip shift (shift_x/shift_y) - the hero shift stays on the raw_n read only,
        // exactly as the precip shift already did; presence above never sees either.
        // <SS:Nexii> Phase 6b (ssdecknoisecore.h CONTRACT): raw_n is now the de-tiled read (noiseFieldAt does the
        // mix internally), and its "not ready" case already floors to 0.f inside noiseFieldAt - the llmax(...,
        // 0.f) this used to wrap a direct noiseSample call with is no longer needed (a plain noiseSample call
        // here would also have skipped the de-tile mix entirely, which the contract forbids for any n_map read).
        const SSDeckFrame::Vec2 shifted_gate_pt = SSDeckFrame::gateAir(
            SSDeckFrame::Vec2{pos_agent.mV[VX] - shift_x - drift.mV[0], pos_agent.mV[VY] - shift_y - drift.mV[1]},
            hero_shift_m);
        F32 shifted_presence = 1.f;
        F32 shifted_tower = 0.f;
        F32 shifted_raw_n = 0.f;
        noiseFieldAt(*deck, shifted_gate_pt.x, shifted_gate_pt.y, shifted_presence, shifted_tower, shifted_raw_n);
        tower = SSStormCouple::towerFromMap(shifted_raw_n, deck->mNoiseTowerLo, deck->mNoiseTowerHi, sample.boost);
    }
    else
    {
        // <SS:Nexii> review 3b NEW-10 / review S12: tower is derived through SSStormCouple::towerFromMap here too,
        // boost 0, rather than left at noiseFieldAt's own internal ss_smoothstep result above (the value `tower`
        // already holds) - one implementation for both of precipNoiseAt's paths, matching the builder's own S12
        // discipline, even though towerFromMap(raw_n, lo, hi, 0) == smoothstep(lo, hi, n) makes this a no-op today.
        tower = SSStormCouple::towerFromMap(raw_n, deck->mNoiseTowerLo, deck->mNoiseTowerHi, 0.f);
    }

    // <SS:Nexii> 8a item 4 (doc/atmo_magic_phase8_show.md section 3): z carries the line-band wall term
    // (sample.lineWall, folded in above) - dropRateAt and the spawner's per-cell rate both take max(intensity,
    // z * x) so the rain starts exactly where the wall is, even when the authored precip curve has not yet ramped.
    // 8a audit F4: z is returned RAW here, not pre-multiplied by presence (x) - the wall is a purely geometric
    // distance-to-segment field (SSStormCouple::lineField) with no notion of the deck's own noise-map holes, so
    // BOTH callers gate it by x themselves before using it as a floor; do not use z alone as a rate.
    return LLVector3(presence, tower, sample.lineWall);
}

F32 SSVolCloud::precipBaseZ() const
{
    const Deck* deck = weatherDeck();
    return (deck && !deck->mPuffs.empty()) ? deck->mBaseZ : cloudBaseZ();
}

// The weather deck's ceiling - the top of the band precipitation falls out of. Same deck choice
// as precipBaseZ, so both ends of the weather span stay one deck's band.
F32 SSVolCloud::precipTopZ() const
{
    const Deck* deck = weatherDeck();
    return (deck && !deck->mPuffs.empty()) ? deck->mBaseZ + deck->mThicknessM : -FLT_MAX;
}

// How solid the under deck is over one point of the sky: the same noise-map presence gate the
// builder ran for the column, read in the air frame so it drifts with the deck. No map read
// back yet answers neutral - the deck is there until proven a hole.
F32 SSVolCloud::underPresenceAt(const LLVector3& pos_agent) const
{
    if (mUnder.mPuffs.empty() || mUnder.mNoiseW <= 0 || mUnder.mNoiseTileM <= 0.f) return 1.f;

    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    F32 presence = 1.f;
    F32 tower = 0.f;
    noiseFieldAt(mUnder,
                 pos_agent.mV[VX] - drift.mV[0],
                 pos_agent.mV[VY] - drift.mV[1],
                 presence, tower);
    return presence;
}

// The picker previews' stand-ins: what the deck is actually running while its authored field is
// None. An authored texture hands back null - the picker then previews the real asset by its
// own uuid, as texture pickers always have.
LLViewerTexture* SSVolCloud::noisePreviewTexture(bool under_deck) const
{
    const Deck& deck = under_deck ? mUnder : mPrimary;
    if (deck.mNoise.notNull()) return nullptr;
    return deck.mNoiseProcRef;
}

LLViewerTexture* SSVolCloud::profilePreviewTexture(bool under_deck) const
{
    const Deck& deck = under_deck ? mUnder : mPrimary;
    if (deck.mProfile.notNull()) return nullptr;
    return deck.mProfileProcRef;
}

// <SS:Nexii> The field's own overlay: four views over one build, picked by SSAtmoCloudDebugView, each answering a different question about a sky that is otherwise only inspectable by looking at it. Every mark goes through the SAME far-field squash the vertex stage applies - squashScale, pulled radially toward the eye - because without it the marks land kilometres behind the cloud they describe and the far half of a 10km field never survives a 2km far plane to be drawn at all. Lines are subdivided BEFORE they are squashed: the squash is not linear along a segment, and the band rings span kilometres.
void SSVolCloud::renderDebug()
{
    static LLCachedControl<U32> view_setting(gSavedSettings, "SSAtmoCloudDebugView", 1);
    const S32 which = llclamp((S32)view_setting, 1, 3);

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;

    if (mPrimary.mPuffs.empty() && mUnder.mPuffs.empty()) return;

    const LLVector3 cam = camera->getOrigin();
    const LLVector3 cam_right = camera->getLeftAxis() * -1.f;

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        const LLVector3 rel = p - cam;
        const F32 d = rel.magVec();
        if (d <= 1.0e-4f) return p;
        return cam + rel * squashScale(d);
    };

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);

    // The two decks' bands - the floor and lid each field was built between, as a ring at the field's fade radius with posts at its corners. Drawn under every view, because none of the other marks mean much except against the band they belong to.
    auto band = [&](const Deck& deck, const LLColor4& c)
    {
        if (deck.mPuffs.empty()) return;

        const F32 r = FIELD_FADE_START_M;
        const F32 z0 = deck.mBaseZ;
        const F32 z1 = deck.mBaseZ + deck.mThicknessM;
        const F32 bx[4] = { cam.mV[VX] - r, cam.mV[VX] + r, cam.mV[VX] + r, cam.mV[VX] - r };
        const F32 by[4] = { cam.mV[VY] - r, cam.mV[VY] - r, cam.mV[VY] + r, cam.mV[VY] + r };

        gGL.color4fv(c.mV);
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 j = (i + 1) % 4;
            line(LLVector3(bx[i], by[i], z0), LLVector3(bx[j], by[j], z0));
            line(LLVector3(bx[i], by[i], z1), LLVector3(bx[j], by[j], z1));
            line(LLVector3(bx[i], by[i], z0), LLVector3(bx[i], by[i], z1));
        }
    };
    band(mPrimary, LLColor4(0.45f, 0.70f, 1.00f, 0.30f));
    band(mUnder,   LLColor4(1.00f, 0.72f, 0.35f, 0.30f));

    // <SS:Nexii> The builder's cell gate, as one call both views can share: the cluster lattice and the per-cell hash mixed by CLUSTER_WEIGHT, then pushed toward a certain skip wherever the noise map's presence runs low. A cell holds cloud when this comes back at or under the deck's coverage. Kept in step with buildDeck by hand - the fragment stage replicates the same lines a third time (ssVolCloudF.glsl), and all three must move together.
    // <SS:Nexii> review S6/S12: tower is derived through SSStormCouple::towerFromMap exactly as buildDeck's does,
    // never a bare ss_smoothstep re-spelling - and, when this deck IS weatherDeck() and cells are coupled, sampled
    // through SSStormCouple::sampleAt at this cell's WORLD centre (air cell centre + drift, the caller's own
    // drift) so the two views this helper feeds see the same storm-widened window buildDeck used, not a
    // storm-blind replay of it. storm_sample is returned so a caller (the V2 column outline) can also fold
    // storm_sample.anvil into SSStormCouple::anvilWeight the same way buildDeck's cell_anvil does.
    auto cellGate = [&](const Deck& deck, S32 cx, S32 cy, F32 drift_x, F32 drift_y,
                         F32& presence, F32& tower, SSStormCouple::Sample& storm_sample) -> F32
    {
        const F32 gate_raw = clusterUnit(cx, cy, deck.mSalt) * CLUSTER_WEIGHT
                           + hashUnit(cx, cy, 1u + deck.mSalt) * (1.f - CLUSTER_WEIGHT);

        presence = 1.f;
        tower = 0.f;
        F32 raw_n = 0.f;
        noiseFieldAt(deck, (F32)(cx + 0.5) * CELL_M, (F32)(cy + 0.5) * CELL_M, presence, tower, raw_n);

        storm_sample = SSStormCouple::Sample();
        if (&deck == weatherDeck() && mStormCellCount > 0)
        {
            // <SS:Nexii> phase-3c fix F2: routed through SSStormCouple::samplePointM, the same call buildDeck's
            // per-cell sample and precipNoiseAt's landing-point sample now both make - one point, one call site
            // form, per doc S3 "Sample point lockstep" - rather than a third hand-derived copy of the same lines.
            F32 world_x, world_y;
            SSStormCouple::samplePointM((F32)cx * CELL_M + CELL_M * 0.5f, (F32)cy * CELL_M + CELL_M * 0.5f,
                                         CELL_M, drift_x, drift_y, world_x, world_y);
            storm_sample = SSStormCouple::sampleAt(mStormCells, mStormCellCount, world_x, world_y);
            // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): DEBUG-BAKE call site (cellGate, the
            // V2/V7 debug views' cell-gate replay) - folded in at the same world point, same gate as the other two
            // CPU sites, so the debug overlay's tower/anvil readout agrees with what buildDeck actually shaped.
            SSStormCouple::applyLineBand(storm_sample, SSStormCouple::lineField(mLineBand, world_x, world_y), SSSquall::LINE_ANVIL_FRAC);
        }
        tower = SSStormCouple::towerFromMap(raw_n, deck.mNoiseTowerLo, deck.mNoiseTowerHi, storm_sample.boost);

        return gate_raw + (1.f - gate_raw) * (1.f - presence);
    };

    if (which == 1)
    {
        // <SS:Nexii> The cell gate, replayed on the same 260m air-frame grid the builder walks: green where the gate passed and cloud was placed, red where it did not and the sky is open. A kept cell also grows a stalk as tall as the noise map's TOWER weight wants that column to climb, so the map's convection geography - the thing that decides which cells stand up as cumulonimbus and which stay pockets - is visible as terrain rather than inferred from the cloud it produced. Drift is added back on the way out, so the grid rides with the field instead of standing still under it.
        const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

        const S32 span = 14;
        for (S32 d = 0; d < 2; ++d)
        {
            const Deck& deck = d ? mUnder : mPrimary;
            if (deck.mPuffs.empty()) continue;

            const S32 cx0 = llfloor((cam.mV[VX] - drift.mV[0]) / CELL_M);
            const S32 cy0 = llfloor((cam.mV[VY] - drift.mV[1]) / CELL_M);

            for (S32 dy = -span; dy <= span; ++dy)
            {
                for (S32 dx = -span; dx <= span; ++dx)
                {
                    const S32 cx = cx0 + dx;
                    const S32 cy = cy0 + dy;

                    F32 presence = 1.f;
                    F32 tower = 0.f;
                    SSStormCouple::Sample storm_sample;
                    const F32 gate = cellGate(deck, cx, cy, drift.mV[0], drift.mV[1], presence, tower, storm_sample);
                    const bool kept = gate <= deck.mCoverage;

                    const LLVector3 at((F32)cx * CELL_M + CELL_M * 0.5f + drift.mV[0],
                                       (F32)cy * CELL_M + CELL_M * 0.5f + drift.mV[1],
                                       deck.mBaseZ);

                    const F32 half = CELL_M * 0.42f;
                    gGL.color4f(kept ? 0.30f : 0.95f, kept ? 0.90f : 0.25f, 0.35f,
                                kept ? 0.55f : 0.30f);
                    line(at - cam_right * half, at + cam_right * half);
                    line(LLVector3(at.mV[VX], at.mV[VY] - half, at.mV[VZ]),
                         LLVector3(at.mV[VX], at.mV[VY] + half, at.mV[VZ]));

                    if (kept && tower > 0.01f)
                    {
                        const F32 t = llclamp(tower, 0.f, 1.f);
                        gGL.color4f(0.40f + 0.60f * t, 0.55f + 0.45f * t, 1.00f, 0.20f + 0.45f * t);
                        line(at, at + LLVector3(0.f, 0.f, t * deck.mThicknessM));
                    }
                }
            }
        }
    }
    else if (which == 2)
    {
        // <SS:Nexii> The profile ramp, drawn as the thing it actually produces. A chart of the curve tells you what the numbers are and nothing about what they do to a sky, so this stands the curve up IN THE WORLD instead: for every cell near the camera that the gate kept, the column that cell would grow is outlined at its true position and altitude, its half-width at each height being exactly the width the builder gives a puff there - waist, flare, the profile ramp's own anvil term and the deck-wide one, all replayed off the deck rather than approximated. Where a column flares near its lid, that IS the ramp's upper end; where a stack of them stays a cylinder, the ramp is doing nothing up there. The outlines hang beside the cloud they shaped, so the two can be read against each other.
        const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

        // Deliberately a small neighbourhood. A column outline is a legible thing and a
        // thousand of them are not: this is the near field, where the shapes can be told
        // apart, and the tower view above is where whole-field geography belongs.
        const S32 span = 5;
        const S32 steps = 14;

        for (S32 d = 0; d < 2; ++d)
        {
            const Deck& deck = d ? mUnder : mPrimary;
            if (deck.mPuffs.empty()) continue;

            const F32 base_radius = CELL_M * PUFF_CELL_FRACTION * 0.5f;
            const F32 size_gain = 1.f + PUFF_THICKNESS_GAIN * (deck.mThicknessM / 500.f);

            const F32 anvil_gate = ss_smoothstep(0.40f, 0.60f, deck.mConvection);

            const S32 cx0 = llfloor((cam.mV[VX] - drift.mV[0]) / CELL_M);
            const S32 cy0 = llfloor((cam.mV[VY] - drift.mV[1]) / CELL_M);

            for (S32 dy = -span; dy <= span; ++dy)
            {
                for (S32 dx = -span; dx <= span; ++dx)
                {
                    const S32 cx = cx0 + dx;
                    const S32 cy = cy0 + dy;

                    F32 presence = 1.f;
                    F32 tower = 0.f;
                    SSStormCouple::Sample storm_sample;
                    const F32 gate = cellGate(deck, cx, cy, drift.mV[0], drift.mV[1], presence, tower, storm_sample);
                    if (gate > deck.mCoverage) continue;

                    // <SS:Nexii> Phase 4 fixup (F10): the hero's local frame shift for this cell, PRODUCER rule -
                    // S is evaluated at the UNSHIFTED cell centre, the same pattern buildDeck's placement loop
                    // follows - so the outline leans with the same puffs it explains instead of drawing a straight
                    // column through a hero's shift. Zero whenever this deck is not weatherDeck(), there are no
                    // coupled cells, or there is no hero, exactly as buildDeck's own hero_influence gate reads.
                    F32 world_x, world_y;
                    SSStormCouple::samplePointM((F32)cx * CELL_M + CELL_M * 0.5f, (F32)cy * CELL_M + CELL_M * 0.5f,
                                                 CELL_M, drift.mV[0], drift.mV[1], world_x, world_y);
                    const bool couple_storm_view = (&deck == weatherDeck()) && mStormCellCount > 0;
                    const F32 hero_influence = (couple_storm_view && mHeroFrame.ageS > 0.f)
                        ? SSStormCouple::influence(mHeroFrame.centre.x, mHeroFrame.centre.y, mHeroFrame.radius, world_x, world_y)
                        : 0.f;
                    const SSDeckFrame::Vec2 hero_shift_m = SSDeckFrame::heroShift(mHeroFrame, hero_influence);

                    const F32 coreness = llclamp(
                        (deck.mCoverage - gate) / llmax(deck.mCoverage, 0.01f), 0.f, 1.f);
                    // <SS:Nexii> review S6 / review 3b NEW-3: cellShapeAt is the SAME helper buildDeck's placement
                    // loop calls, so cell_height/cell_anvil (and the storm's own anvil term, storm_sample.anvil, 0
                    // off-cell/uncoupled, folded in via SSStormCouple::anvilWeight inside it) can never drift
                    // between what is actually built and what this overlay draws as its explanation of it.
                    // <SS:Nexii> review 3b NEW-3 (phase-3c F4: moved to this read site): deck.mStormEff, not a
                    // second hand-spelled SSStormCouple::consolidation(deck.mMoisture, deck.mConvection) - buildDeck
                    // already bakes the storm-delegated figure it actually used (cellsActive included) onto the
                    // deck, and reading anything else here would show a different consolidation than the one that
                    // shaped the cloud on screen.
                    const SSStormCouple::CellShape shape = SSStormCouple::cellShapeAt(deck.mConvection, deck.mStormEff, tower, coreness, deck.mAnvil, storm_sample.anvil);
                    const F32 cell_height = shape.cell_height;
                    const F32 cell_anvil = shape.cell_anvil;

                    // <SS:Nexii> review 3b NEW-3, updated by NEW-4 (2026-09-05): the overshoot lift buildDeck adds
                    // to sub-puff 0 (SSStormCouple::overshootBonusM), mirrored onto the outline's own top
                    // (i == steps below) so the drawn ceiling matches where that puff actually reaches instead of
                    // stopping short of it. Sub 0 IS the cell's tallest sub-puff as of NEW-4's deterministic swap
                    // (buildDeck's own comment) - a world-state, camera-free swap of the hashed height with
                    // whichever sub actually hashed the cell's maximum - so "sub 0" and "the tallest sub-puff" are
                    // the same puff by construction, not merely both lifted by the same formula.
                    const F32 overshoot_topM = (storm_sample.overshoot > 0.f)
                        ? SSStormCouple::overshootBonusM(deck.mThicknessM, storm_sample.overshoot)
                        : 0.f;

                    // The two sides of the silhouette, walked together so the outline closes
                    // at both ends and the flare reads as one shape rather than two curves.
                    LLVector3 prev_l;
                    LLVector3 prev_r;
                    for (S32 i = 0; i <= steps; ++i)
                    {
                        const F32 up_cell = (F32)i / (F32)steps;
                        const F32 up = up_cell * cell_height;

                        const F32 waist = 1.f - 0.35f * ss_smoothstep(0.2f, 0.65f, up_cell);
                        const F32 flare = 1.1f * ss_smoothstep(0.74f, 1.f, up_cell);

                        const F32 ramp_v = (deck.mProfileN > 0)
                            ? profileSample(deck, up, 0)
                            : ss_smoothstep(0.55f, 0.85f, up);
                        const F32 puff_anvil = llmax(cell_anvil, ramp_v * anvil_gate);

                        // <SS:Nexii> review 3b NEW-3: mammatus flares this same flatten term on the outline's top
                        // third (up_cell >= SSStormCouple::MAMMATUS_TOP_FRAC) exactly as buildDeck's per-sub-puff
                        // flatten_term does - the overlay was drawing every mammatus-flared cell as if it were
                        // unflared.
                        F32 flatten_term = waist + flare - 1.f;
                        if (storm_sample.mammatus > 0.f && up_cell >= SSStormCouple::MAMMATUS_TOP_FRAC)
                        {
                            flatten_term *= SSStormCouple::mammatusScale(storm_sample.mammatus);
                        }
                        // <SS:Nexii> 8a item 4: the shelf's flatten floor, exactly as buildDeck's own flatten_term
                        // above - kept in step by hand, same discipline as the mammatus fix this replica already
                        // carries (review 3b NEW-3).
                        flatten_term = llmax(flatten_term, storm_sample.lineShelf);
                        const F32 flat = 1.f + puff_anvil * coreness * flatten_term;

                        const F32 round = llclamp(
                            (up - PUFF_ROUND_LO) / (PUFF_ROUND_HI - PUFF_ROUND_LO), 0.f, 1.f);
                        const F32 wide = PUFF_WIDE + (1.f - PUFF_WIDE) * round;

                        const F32 half_w = base_radius * size_gain * flat * wide;
                        F32 z = deck.mBaseZ + up * deck.mThicknessM;
                        if (i == steps && overshoot_topM > 0.f)
                        {
                            z += overshoot_topM;
                        }
                        // <SS:Nexii> Phase 4 fixup (F10): PRODUCER placement, no respelled transform - the SAME
                        // placeWorld buildDeck's own puffs go through (cell centre, no jitter for the outline +
                        // drift + S + O(z) at THIS row's own z), so the outline leans exactly as the cloud it is
                        // explaining does, row by row, rather than standing as a rigid drift-only column.
                        const SSDeckFrame::Vec2 placed = SSDeckFrame::placeWorld(
                            SSDeckFrame::Vec2{(F32)cx * CELL_M + CELL_M * 0.5f, (F32)cy * CELL_M + CELL_M * 0.5f},
                            SSDeckFrame::Vec2{drift.mV[0], drift.mV[1]}, hero_shift_m, z, deck.mShearTable);
                        const LLVector3 at(placed.x, placed.y, z);
                        const LLVector3 l = at - cam_right * half_w;
                        const LLVector3 r = at + cam_right * half_w;

                        // Hue is the anvil figure in force at THIS height - blue where the
                        // column is still a rounded body, orange where the ramp has opened it
                        // out - so the height the profile takes over at is visible as the
                        // colour changing partway up the outline.
                        const F32 a = llclamp(puff_anvil, 0.f, 1.f);
                        gGL.color4f(0.25f + 0.75f * a, 0.50f + 0.20f * a, 1.00f - 0.70f * a,
                                    0.30f + 0.45f * coreness);

                        if (i == 0 || i == steps)
                        {
                            line(l, r);
                        }
                        if (i > 0)
                        {
                            line(prev_l, l);
                            line(prev_r, r);
                        }
                        prev_l = l;
                        prev_r = r;
                    }
                }
            }
        }
    }

    gGL.end();

    // <SS:Nexii> The GROUND SHADOW view: the baked transmittance map's own texels, drawn as dark blots ON THE GROUND at the exact spot the soften shader lands each one - the same casting plane, the same live sun direction, the same drift - so the overlay and the rendered shadow must lie on top of each other, and any daylight between them is a projection bug wearing its address. Darkness is the texel's occlusion, so the thickness driver can be read directly: a deep storm deck's blots go near-black, a thin sheet's barely register. Filled triangles rather than lines, hence its own pass after the shared line batch.
    if (which == 3 && mShadowValid && mShadowRaw.notNull() && mShadowRaw->getData()
        && mShadowSpanM > 1.f && !mPrimary.mPuffs.empty())
    {
        const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
        LLVector3 sun_w = mLightDir;
        if (applier.isActive() && applier.sunRiseFraction() > 0.001f)
        {
            sun_w = applier.sunSlotDirection();
        }
        const F32 sz = llmax(sun_w.mV[VZ], 0.05f);

        // The camera's own feet stand in for the terrain: the map is being judged for placement and darkness, not draped over every hill.
        const F32 gz = cam.mV[VZ] - 1.2f;
        const F32 plane_z = mPrimary.mBaseZ + mPrimary.mThicknessM * 0.35f;
        const F32 fall = llmax(plane_z - gz, 0.f) / sz;

        const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();
        const S32 n = mShadowRaw->getWidth();
        const U8* data = mShadowRaw->getData();
        const F32 texel = mShadowSpanM / (F32)n;
        const F32 half = texel * 0.5f;
        const F32 draw_r_sq = 2600.f * 2600.f;

        gGL.begin(LLRender::TRIANGLES);
        for (S32 ty = 0; ty < n; ++ty)
        {
            for (S32 tx = 0; tx < n; ++tx)
            {
                const F32 occl = 1.f - (F32)data[((size_t)ty * n + tx) * 3] / 255.f;
                if (occl < 0.04f) continue;

                // Where this texel's shadow lands: down the sun ray from the casting plane.
                const F32 gx = mShadowOriginX + drift.mV[0] + ((F32)tx + 0.5f) * texel - sun_w.mV[VX] * fall;
                const F32 gy = mShadowOriginY + drift.mV[1] + ((F32)ty + 0.5f) * texel - sun_w.mV[VY] * fall;

                const F32 ddx = gx - cam.mV[VX];
                const F32 ddy = gy - cam.mV[VY];
                if (ddx * ddx + ddy * ddy > draw_r_sq) continue;

                gGL.color4f(0.05f, 0.02f, 0.10f, occl * 0.55f);
                const LLVector3 p00 = drawn(LLVector3(gx - half, gy - half, gz));
                const LLVector3 p10 = drawn(LLVector3(gx + half, gy - half, gz));
                const LLVector3 p11 = drawn(LLVector3(gx + half, gy + half, gz));
                const LLVector3 p01 = drawn(LLVector3(gx - half, gy + half, gz));
                gGL.vertex3fv(p00.mV); gGL.vertex3fv(p10.mV); gGL.vertex3fv(p11.mV);
                gGL.vertex3fv(p00.mV); gGL.vertex3fv(p11.mV); gGL.vertex3fv(p01.mV);
            }
        }
        gGL.end();
    }

    gGL.flush();
}
