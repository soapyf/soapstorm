/**
 * @file ssvolcloud.h
 * @brief Atmo Magic: volumetric cloud puff field.
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

#ifndef SS_VOLCLOUD_H
#define SS_VOLCLOUD_H

#include "llsingleton.h"
#include "llpointer.h"
#include "lluuid.h"
#include "llimage.h"
#include "llrendertarget.h"
#include "llviewertexture.h"
#include "ssdeckboilcore.h"
#include "ssdeckframecore.h"
#include "ssdeckshadecore.h"
#include "ssstormcells.h"
#include "ssstormcouplecore.h"
#include "ssvirgacore.h"
#include "v2math.h"
#include "v3color.h"
#include "v3math.h"
#include "v4math.h"

#include <vector>
#include <unordered_map>

struct SSAtmoEnvCloudFieldState;
struct SSAtmoEnvTrack;
class LLGLSLShader;

static const S32 SS_MAX_STRIKE_LIGHTS = 4;

// <SS:Nexii> The far-field squash cap as a fraction of MAX_FAR_CLIP: where the cloud field's drawn depth tops out, just short of the projection far plane so nothing rasterises against it. The knee (0.8 of the cap, set in update()) is where the compression starts; between them the whole remaining field is folded into the last fifth of drawn depth. Shared with lightning so bolt and cloud agree about drawn depth. [interaction: SSLightning]
constexpr F32 SS_SQUASH_CAP_FRAC = 0.98f;

class SSVolCloud : public LLSingleton<SSVolCloud>
{
    LLSINGLETON_EMPTY_CTOR(SSVolCloud);

public:
    void update(F32 dt);

    void render();

    // <SS:Nexii> The scene depth copy the soft fades read, taken once per frame by whichever post-deferred weather pass asks first (the puffs, else the lightning pass on a clear sky) - exact for both, since no pass after the deferred lighting writes depth. Null when the copy program or viewport is unavailable. Must be called before the caller binds its own program: it binds the copy program and flushes/rebinds the screen target. [interaction: SSLightningRender discs]
    LLRenderTarget* ensureSceneDepthCopy();

    // <SS:Nexii> The field's debug overlay, driven by RENDER_DEBUG_CLOUD_FIELD with the view picked by SSAtmoCloudDebugView: the cell gate and tower map replayed on the builder's own 260m grid, or the columns those cells grow outlined at their true altitude so the profile ramp can be read as the shape it makes. Both walk CELLS, not puffs - an outline per puff buries the sky it is describing. Everything it draws is squash-corrected, so it lands on the cloud rather than behind it. [interaction: pipeline debug masks]
    void renderDebug();

    void clear();

    // GL teardown: clear(), then every deck texture reference, the shadow texture and the depth copy.
    void shutdownGL();

    // <SS:Nexii> The primary deck's geometry and coverage: the auto dome altitude derivation and every consumer that asks "how much cloud is overhead" mean the main field, not the under deck bolted on below a sky build.
    F32 cloudBaseZ() const { return mPrimary.mBaseZ; }
    F32 cloudTopZ() const { return mPrimary.mBaseZ + mPrimary.mThicknessM; }
    bool empty() const { return mPrimary.mPuffs.empty(); }

    // <SS:Nexii> The coupled weather deck's coverage, 0-1 - SSVortices::dustGate's "the deck's coverage" input.
    // weatherDeck() is whichever of mPrimary/mUnder is coupled to the storm scheduler this build (private; this is
    // a public read-only forward). Zero when no weather deck is built yet.
    F32 weatherCoverage() const { const Deck* d = weatherDeck(); return d ? d->mCoverage : 0.f; }

    // <SS:Nexii> The coupled weather deck's own storm gloom and light direction - SSVortexRender reads both so a
    // funnel darkens under the same storm moisture and lights its rim from the same sun/moon the deck it hangs
    // under is shaded by, rather than carrying a second, disagreeing light. Neutral (no darkening, +Z) when no
    // weather deck is built yet.
    F32 weatherGloom() const { const Deck* d = weatherDeck(); return d ? d->mGloom : 1.f; }
    LLVector3 lightDir() const { return mLightDir; }

    // <SS:Nexii> The primary deck's noise tile, metres (0 when it has no map) - already quantised to the cell lattice by the builder, so the drift accumulator's wrap span can be a multiple of both (SSWindProfile::wrapSpanM) and the wrap leaves the cell gate and the noise map exactly where they were. [interaction: SSAtmoEnvApplier drift]
    F32 noiseTileMetres() const { return mPrimary.mNoiseTileM; }
    F32 underNoiseTileMetres() const { return mUnder.mNoiseTileM; }

    // <SS:Nexii> The under deck's live world-frame band and whether it has a built field at all. Lightning reads these to decide whether a ground strike would cross the deck (and so be re-routed to cloud-to-cloud) and how deep its in-cloud crawl may dive. The band is only meaningful while underPresent() holds.
    F32 underBaseZ() const { return mUnder.mBaseZ; }
    F32 underTopZ() const { return mUnder.mBaseZ + mUnder.mThicknessM; }
    bool underPresent() const { return !mUnder.mPuffs.empty(); }

    // <SS:Nexii> How solid the under deck is over one point of the sky, 0-1 - the noise map's presence gate for the deck's own column, in the same air frame the clouds drift in. 1 is solid cloud at that spot, 0 a hole or gap. Neutral 1 when the deck has no map cached yet.
    F32 underPresenceAt(const LLVector3& pos_agent) const;

    F32 transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength);

    S32 puffCount() const { return (S32)(mPrimary.mPuffs.size() + mUnder.mPuffs.size()); }
    F32 lastBuildMS() const { return mLastBuildMS; }

    // <SS:Nexii> V3 (ssatmoinfoviewcore.h MODE_DECK_LOD): the primary deck's own last-build LOD tally
    // (SSDeckLod::subsAt(dist, dial) summed over every cell that passed the gate) and the cell count it was
    // summed over - the "if nothing were thinned or budget-clamped" figure the debug legend reads against
    // puffCount()/the SSAtmoCloudPuffBudget dial. Zero before any build.
    S64 primaryLodPredictedSubs() const { return mPrimary.mLodSubsTally; }
    S32 primaryLodCellsWalked() const { return mPrimary.mLodCellTally; }

    // <SS:Nexii> V3 (ssatmoinfoviewcore.h MODE_DECK_LOD, ssdeckmacrocore.h CONTRACT): the primary deck's own
    // last-build Tier B macro-puff body count - see Deck::mTierBCount.
    S32 primaryTierBCount() const { return mPrimary.mTierBCount; }

    // <SS:Nexii> V4 (ssatmoinfoviewcore.h MODE_PRECIP_VIRGA, doc/atmo_magic_debug_views.md V4): one qualifying
    // virga cell from the LAST build, exactly as buildDeck's shaft-candidate walk resolved it - the view reads
    // this snapshot rather than re-deriving qualification, so it can never show a cell set the builder did not
    // actually decide on. mX/mY are the shaft column's world (agent) position - the SAME point the card stack
    // itself is drawn at (SSDeckFrame::placeWorld through a zero shear table, hero shift included -
    // ShaftCandidate's own placement). mKept is SSVirga::keepHash's own verdict for this cell (the per-candidate
    // stable hash trim, plus the rare hard-ceiling backstop when keepHash's own kept share runs over 1.3x
    // MAX_SHAFTS), recorded once at build time - not a distance cull (the trim is camera-free by contract).
    struct SSVirgaDebugCell
    {
        F32 mX = 0.f;
        F32 mY = 0.f;
        F32 mDrive = 0.f;
        bool mKept = false;
    };

    // <SS:Nexii> V4: the last build's full virga snapshot. mActive is false whenever the shaft path did not run
    // this build (Distant Rain off/zero-strength in the Weather Influence row, or this deck is not the one
    // storm-coupled - couple_storm false) - the view then says so rather than drawing a stale set left over from
    // a build where it did. mR2 is the particle rain's own TIER_SHEETS radius (SSVirga::handoff's r2 input);
    // mGroundZ/mBaseZ are the span the card stacks and the fall-tilt comparison line both span. F9 (2026-09-06
    // review): mEmbedTopZ is SSVirga::embedTopZ(mBaseZ, deck thickness) as this build's card loop actually used it
    // (the SAME value Deck::mShaftEmbedTopZ carries to the render pass - the view outlines cells here, not at
    // mBaseZ, since embedTopZ is where the stack itself now starts). 8e-b PROFILE SKEW (2026-09-06): mWindParams/
    // mBaseAglM replace the old single mWindBase vector - the emitter's cardGeom calls now integrate the wind
    // PROFILE (SSVirga::profileSkewM) rather than sample one altitude, so the view needs the same (params,
    // baseAgl) pair to reconstruct a card's own chord, not a single vector; mFallSpeed is unchanged, the curve-
    // resolved active preset fall speed the emitter's own profileSkewM calls used this build. A snapshot, not a
    // value the view re-derives, so V4's drawn skew polyline can never disagree with the geometry it is
    // describing. Reset to default (mActive false, empty mCells) at the top of every update() and by clear(), so
    // an early-out frame (feature off, no asset) never leaves a stale snapshot behind either.
    struct SSVirgaDebug
    {
        bool mActive = false;
        F32 mR2 = 0.f;
        F32 mGroundZ = 0.f;
        F32 mBaseZ = 0.f;
        F32 mEmbedTopZ = 0.f;
        SSWindProfile::Params mWindParams;
        F32 mBaseAglM = 0.f;
        F32 mFallSpeed = 0.f;
        std::vector<SSVirgaDebugCell> mCells;
    };
    const SSVirgaDebug& virgaDebug() const { return mVirgaDebug; }

    // <SS:Nexii> The convection noise map's gate for the weather. Precipitation asks the deck it falls from two questions about a point of the sky: how much cloud is over it (x, a hole in the map reads zero and takes the rain with it) and how tower-like the column is (y, which tweaks the intensity toward the dense parts). The point handed in must already be the WIND-TILTED one - where a drop falling at the weather's angle entered the deck, not where it lands - because only the caller knows the fall; this side supplies everything else, drift included. A deck with no map, or whose map has not read back yet, answers neutral: everything present, nothing tower-like. [interaction: precipitation]
    // <SS:Nexii> 8a item 4 (doc/atmo_magic_phase8_show.md section 3): x presence, y tower (unchanged), z the
    // squall line-band's wall term at this point (SSStormCouple::Sample::lineWall) - dropRateAt and the spawner
    // both fold z into their intensity with max() so the wall's own precipitation is never held back by the
    // authored curve's own ramp. Was LLVector2 before 8a; every caller updated.
    LLVector3 precipNoiseAt(const LLVector3& pos_agent) const;

    // The weather deck's base height, metres, for the same tilt maths - how far above the
    // ground a drop's column reaches.
    F32 precipBaseZ() const;

    // <SS:Nexii> The weather deck's ceiling, metres - the top of the band precipitation forms in. Mirrors precipBaseZ's deck choice, so both ends of the weather span come from one deck. -FLT_MAX when no weather deck is built (nothing gates on it). [interaction: precipitation]
    F32 precipTopZ() const;

    // Whether the weather deck has a built field and a noise map read back - checked before
    // precipitation pays for any of the gating.
    bool precipNoiseReady() const;

    // <SS:Nexii> The generated stand-ins a picker previews for each deck's noise map and profile ramp: the procedural map or the built-in strip while it is what the deck runs, null once an authored texture covers the field (the picker then shows the real asset). These are live previews of the built decks, not of the edited asset.
    LLViewerTexture* noisePreviewTexture(bool under_deck) const;
    LLViewerTexture* profilePreviewTexture(bool under_deck) const;

    // <SS:Nexii> The deck's GROUND SHADOW, bound into the deferred sun pass (pipeline.cpp's soften block): the baked transmittance map plus the projection uniforms (grid frame, world sun direction, casting plane), gated to zero whenever there is no deck, no direct light, a grazing sun, or the SSAtmoCloudGroundShadow dial at 0 - an unbound gate reads 0 in GLSL, so the soften shader is safe even if this is never called. [interaction: deferred sun lighting]
    void bindGroundShadow(LLGLSLShader& shader);

private:
    struct Puff
    {
        LLVector3 mPosAgent;
        F32 mRadius = 0.f;
        F32 mAlpha = 0.f;

        // <SS:Nexii> The puff's structural form term - facing toward the light and the exponential shade down through the deck, beam-flattened. This used to be a baked LLColor3: (ambient + sun * form) with the sun from a CPU replica of the beam extinction that crushed to grey at every low sun. The LIGHT half of that product now comes from the dome's own uniforms in the vertex shader (ssVolCloudV.glsl) so the deck takes the sunset the dome band does; only the structure - which the builder walks the deck's geometry to know - still rides the puff.
        F32 mForm = 1.f;

        // <SS:Nexii> How BURIED this puff is - the fraction of its own column standing above it, 0 at the lid and 1 at the floor. Sunless and directionless on purpose: mForm above is the sun's story and dies with the beam, while this is the cloud's own body, and the deck's water content has to read on a moonless overcast exactly as it does at noon. The render pass spends it on the spare GREEN vertex channel, where the fragment stage grades the deck's storm gloom over it (ssVolCloudF.glsl) - the lid keeps its light, the belly loses it, which is what "a heavy cloud is dark underneath" means and what a flat per-deck dim could never say. [interaction: storm darkening]
        F32 mBuried = 0.f;

        F32 mCamDistSq = 0.f;

        // <SS:Nexii> Distant rain shaft phase (ssvirgacore.h, doc/atmo_magic_far_clouds.md section 3): this Puff is
        // a virga card, not a puff body - emitted into the SAME mPuffs vector so the existing farthest-first sort
        // interleaves shafts with puffs correctly, flagged on the b vertex channel (render() writes
        // b = 0.5 + 0.5 * mDrive for a shaft, mPhase * 0.49 for an ordinary puff (S2, see mPhase below) - phase 8e
        // item 4, DRIVE REACHES THE FRAGMENT; the branch test vary_color.b > 0.5 is unchanged since a qualifying
        // cell's drive is always > 0 and mPhase * 0.49 never reaches it)
        // rather than a second draw pass. mHalfHeightM is the card's vertical half-extent (<= SSVirga::CARD_MAX_M
        // * 0.5) - render() billboards a shaft about a vertical axis, camera-facing in yaw only, at this
        // half-height instead of the puff's camera-facing disc at mRadius; mRadius is still the card's horizontal
        // half-width (SSVirga::halfWidthM). F7 (2026-09-06 review), stale claim corrected: that billboard axis is
        // NOT purely "the world Z axis" any more - phase 8e's wind skew (mShearXY below) leans it sideways by half
        // the card's own top-relative-to-bottom shear, so the card's height axis tilts with the wind rather than
        // staying strictly vertical.
        bool mShaft = false;
        F32 mHalfHeightM = 0.f;

        // <SS:Nexii> Phase 8e item 4 (doc/atmo_magic_phase8_show.md section 3b): the SAME SSVirga::drive the
        // cell qualified with, carried through so the fragment stage can shape a shaft's body floor/streak
        // amplitude/evaporation mask by intensity - a light-rain curtain a few eroded filaments, a heavy one a
        // near-solid wall. render() encodes it on the shaft-only vertex colour b channel (b = 0.5 + 0.5 * mDrive)
        // rather than a second uniform; 0 for every ordinary puff (mShaft false), unread there.
        F32 mDrive = 0.f;

        // <SS:Nexii> S2 (doc/atmo_magic_flow_field.md section 2): the per-puff advected-detail PHASE, hashed once
        // by buildDeck from (cell x, cell y, sub) same as every other per-sub-puff draw - never camera or build
        // time (I3/I5) - so it is a constant for this puff's whole life, not an accumulator. Ordinary puffs only
        // (mShaft false); render() spends it on the low half of the vertex colour's b channel (b = mPhase * 0.49,
        // see mDrive above for the shaft's own use of that same channel and ssVolCloudF.glsl's puff branch for the
        // decode). 0 for shafts and the sheet, both of which ignore this field entirely.
        F32 mPhase = 0.f;

        // <SS:Nexii> [interaction: ssdeckflowcore.h] The per-puff advected-detail SWIRL, in [-1, 1] - the fifth
        // build report's "not handling the different angles ... picking from a few different presets which dont
        // mash well together or look in unison". SSDeckFlow::swirlUnit read at this puff's own AIR-frame position:
        // a value-noise FIELD on a 780 m lattice, not a per-puff hash, so the angle varies continuously across the
        // deck AND neighbouring puffs agree (correlation 0.82 at one 260 m cell, -0.003 at 3120 m; an independent
        // per-puff hash measures -0.006 at both - unit_deckflow.cpp swirl_neighbour_correlation). The fragment
        // stage spends it as a bounded rotation of the billow's outflow azimuth inside the card's own plane
        // (SSDeckFlow::SWIRL_MAX_RAD, under a quarter turn, so outward never becomes inward). Carried on the
        // texcoord PAYLOAD channel (S1's own encode: texcoord = corner + 0.45 * payload), not on a colour channel,
        // because every colour channel is spent and the payload is exactly what S1 built this for. 0 for shafts
        // and the sheet, both of which never reach the billow block.
        F32 mFlowSwirl = 0.f;

        // <SS:Nexii> Phase 8e, 8e-b PROFILE SKEW (ssvirgacore.h CardGeom::shearXY, doc/atmo_magic_phase8_show.md
        // section 3b): the card's TOP-relative-to-BOTTOM wind skew - profileSkewM(params, baseAglM, aglTop,
        // fallSpeed) minus the same at aglBot - turning the card into a parallelogram whose top and bottom edges
        // both carry the wind-profile-integrated fall angle, rather than a flat vertical pillar. Zero for an
        // ordinary puff (only the shaft branch of buildDeck sets
        // it). render() spends it on the shaft billboard's `up` vector so the quad's slant matches the skew; it is
        // NOT added to mPosAgent - mPosAgent already carries the card's own CENTRE skew (CardGeom::centreXY), and
        // this is the additional lean across the card's own height on top of that.
        LLVector2 mShearXY;
    };

    // <SS:Nexii> One resolved cloud deck. The primary storm field and the optional under deck are the same renderer run twice - each with its own resolved field state, textures, puff set and uniforms - so a sky-themed build can hang a second layer at the bottom of the build while the weather-driven deck stays overhead. Drawn far deck first; within a deck the puffs stay depth-sorted, and decks separated by hundreds of metres hide the cross-deck ordering.
    struct Deck
    {
        // <SS:Nexii> The deck's bodies: the builder's cell-placed puffs (fine tier and macro tier alike - ordinary
        // Puffs, same sort, same budget, same fragment carve, so nothing downstream distinguishes them).
        std::vector<Puff> mPuffs;

        LLUUID mTexture;
        LLPointer<LLViewerFetchedTexture> mTextureRef;

        LLUUID mDetail;
        LLPointer<LLViewerFetchedTexture> mDetailRef;

        // <SS:Nexii> The base and detail maps' crossfade partners and the two eased weights, live while the day cycle fades between keyframed textures. Bound on the shader's spare reserved channels (bumpMap, specularMap) and mixed per sample; both weights 0 - and the partners pinned on the current maps - whenever no fade runs.
        LLUUID mTextureNext;
        LLPointer<LLViewerFetchedTexture> mTextureNextRef;
        F32 mTextureBlend = 0.f;

        LLUUID mDetailNext;
        LLPointer<LLViewerFetchedTexture> mDetailNextRef;
        F32 mDetailBlend = 0.f;

        // <SS:Nexii> The convection noise map and its CPU copy. The GPU sees the same map the field was shaped with - bound as the fragment stage's altDiffuseMap (a reserved LLShaderMgr channel; only reserved names can be bound as textures, see the depthMap note in ssVolCloudF.glsl) so the anvil's carving reads the same geography the towers were grown from. mNoiseW zero means "no map, or not read back yet" - every consumer then treats the field as unmodulated, and the cache fills a frame or two later once the fetch lands.
        LLUUID mNoise;
        LLPointer<LLViewerFetchedTexture> mNoiseRef;
        std::vector<F32> mNoiseLuma;
        S32 mNoiseW = 0;
        S32 mNoiseH = 0;
        S32 mNoiseSrcW = 0;     // the raw image's size at cache time, to re-cache on upgrade
        S32 mNoiseSrcH = 0;

        // <SS:Nexii> And when nothing is authored, a square tileable map generated from the weather seed - the same FBM for every client sharing the environment, so the tower geography (and the holes it cuts) is syncable without anyone having to upload a texture. The raw image is the single source: the CPU grid folds down out of it and the GPU texture uploads from it.
        U32 mNoiseProcSeed = 0;
        LLPointer<LLImageRaw> mNoiseProcRaw;
        LLPointer<LLViewerTexture> mNoiseProcRef;

        // <SS:Nexii> The vertical profile ramp, when one is authored: the same fetch/readback ladder as the noise map, but folded into a one-dimensional curve per channel - rows of the readback averaged across, row 0 the deck's BASE (the same v the shader's texture read samples). R tower weight, G carve guard, B cap band, A base fill. mProfileN zero means "none authored, or not read back yet" - every consumer then runs the built-in vertical curves.
        LLUUID mProfile;
        LLPointer<LLViewerFetchedTexture> mProfileRef;
        std::vector<F32> mProfileCurve;
        S32 mProfileN = 0;

        // <SS:Nexii> And when nothing is authored, a small strip painted from those built-in curves - display only (it never reaches the shader; the built-ins ARE the shader's maths), so a picker has something honest to preview for the None state.
        LLPointer<LLViewerTexture> mProfileProcRef;

        F32 mBaseZ = 0.f;
        F32 mThicknessM = 1.f;

        F32 mAnvil = 0.f;
        F32 mTextureMix = 0.f;
        F32 mPuffDensity = 0.8f;
        F32 mDetailScale = 1.f;
        F32 mDriftRate = 1.f;

        F32 mChurn = 0.f;
        F32 mCoverage = 0.f;

        // The weather this deck was built under, kept only so the debug overlay can replay the
        // builder's own column shaping - the anvil gate and the tower height ramp both read
        // these - rather than eyeball an approximation of it.
        F32 mConvection = 0.f;
        F32 mMoisture = 0.f;

        // The deck's cell-hash salt (0 primary, SS_UNDER_DECK_SALT under), kept so the render pass can hand the base veil's shader the same salt the builder gated cells with - the veil re-runs the gate per fragment to open its own gaps exactly where the builder skipped cells.
        U32 mSalt = 0;

        // <SS:Nexii> The noise map's resolved shaping, baked at build time so precipitation reads exactly the field the deck drew with. mNoiseTileM is metres per tile after the Noise Scale slider (zero when there is no map); mNoiseHole is how hard the map's low end cuts holes once moisture has lifted the floor and convection has kept the storm gaps open. mNoiseTowerLo/Hi are the tower ramp's window - widened as the weather consolidates into a storm, so the carving calms into large solid cells instead of shredding the deck - and the shader's own carving reads the same window back.
        F32 mNoiseTileM = 0.f;
        F32 mNoiseHole = 0.f;
        F32 mNoiseTowerLo = 0.42f;
        F32 mNoiseTowerHi = 0.78f;

        // <SS:Nexii> review 3b NEW-3: the storm-delegated consolidation figure (SSStormCouple::deckConsolidation)
        // this build actually used to derive mNoiseTowerLo/Hi and the tower ramp's conv_gain - baked here so the
        // V2 profile-outline debug view reads the SAME figure buildDeck used (including the delegation share any
        // coupled cells took) instead of respelling the raw, undelegated moisture/convection product by hand.
        F32 mStormEff = 0.f;

        // <SS:Nexii> Phase 4 (doc/atmo_magic_wind_profile.md section 4, doc/atmo_magic_storm_dynamics.md section 3
        // "Storm motion vs cloud drift"): this deck's own baked O(z) shear table (SSDeckFrame::bakeShearTable),
        // z0 = this deck's base, z1 = floor + the track's AUTHORED dome height at phase, groundZ = the track's
        // floor, params = SSAtmoEnvApplier::windProfileAt(track, phase) - the PURE profile path, never
        // cirrusAltitudeMetres() (a per-client setting through the live deck) - baked unconditionally every build
        // (SHAPE-only frame transform, not a storm phenomenon). Read by the puff placement loop below only; the
        // shadow bake and precipNoiseAt never see O(z) (frame contract: they are gate/presence/n_map OBSERVERS,
        // shifted by S alone) and foldFrameKey no longer folds this table.
        SSDeckFrame::ShearTable mShearTable;

        // <SS:Nexii> F2 (2026-09-06 review): the embedded virga stack's top Z (SSVirga::embedTopZ(mBaseZ,
        // mThicknessM)) as this build's shaft-emission loop actually used it - render() uploads (mBaseZ, this) as
        // ss_shaft_embed for the WEATHER deck's pass only, so the fragment stage's per-pixel embed fade
        // (ss_virga_embedAlpha) reads the exact span the CPU built cards against rather than a second, independent
        // embedTopZ() call. Reset to 0 at the top of every buildDeck() call and only set inside the shafts_active
        // block, so a deck whose shaft path did not run this build never carries a stale span forward.
        F32 mShaftEmbedTopZ = 0.f;

        // <SS:Nexii> Phase 8e item 2 (doc/atmo_magic_phase8_show.md section 3b): the ground reference Z the same
        // build's shaft-emission loop resolved (SSAtmoEnvApplier::windProfileGroundZ, the SAME value shaft_ground_z
        // holds there) - render() uploads (mBaseZ, mShaftEmbedTopZ, this, 0) as ss_shaft_embed for the WEATHER
        // deck's pass only, so the fragment stage's curtain-height fraction h reads the SAME [ground, base] span
        // the CPU built cards against rather than a second, independent ground-Z resolve. Reset to 0 at the top of
        // every buildDeck() call and only set inside the shafts_active block, same rule as mShaftEmbedTopZ.
        F32 mShaftGroundZ = 0.f;

        // <SS:Nexii> The base veil: one soft sheet inset into the deck's floor, drawn under the puffs so the field reads with its gaps filled instead of as balls over empty sky. Same texture as the puffs, sampled aperiodically in the shader; the form here is the shade a puff at the deck's floor would wear - the same formulas as the puff loop, lit by the same vertex-stage sky light - so sheet and lowest puffs share one lighting. mSheetZ is the sheet's altitude (the inset), mSheetAlpha its ceiling.
        F32 mSheetForm = 1.f;
        F32 mSheetZ = 0.f;
        F32 mSheetAlpha = 0.f;

        // <SS:Nexii> D2 (fourth build report: "the base veil is unnaturally dark near the camera compared to the puffs beside it"). THIS CONSTANT WAS THE MECHANISM. It rides the vertex colour's g
        // channel (render()'s color4f) and the fragment stage grades the deck's storm gloom over it - ssVolCloudF.glsl's `float gloom = mix(1.0, ss_gloom, vary_color.g)` - so at 1.0 the veil took the
        // gloom at FULL strength, the extreme of a ramp no puff ever reaches: a puff's mBuried is (cellHeight - up)/cellHeight eased to 0.5 at the rim (SSDeckShade::buried), so only a puff sitting
        // exactly on the deck floor with no rim ease is ever at 1. Measured in V:/Scratch/atmo/tests/unit_deck_radiance.cpp: at ss_gloom 0.40 the veil came out 37% darker than the low puff beside
        // it on identical light at the same point, and at 0.15 it came out 69% darker - and the veil is a flat plane, so that gap is most of the frame wherever the eye is under the deck.
        // It is VEIL_DEPTH now, the SAME representative depth SSDeckShade::veilForm already computes the veil's own form term at (0.65 of a layer above and below a point at the floor). That is the
        // point of the change: the veil stands for the deck's underside at ONE representative depth, and its two structural terms - the form it wears and the gloom it is graded over - have to agree
        // about what that depth is. They did not.
        // The old LOCKSTEP with SSVirga::BURIED is DELIBERATELY BROKEN, not overlooked. A virga curtain hangs BELOW the deck entirely, with the whole column over it, so 1.0 is the honest answer
        // there and ssvirgacore.h keeps it; the veil sits INSIDE the deck's own floor band (mSheetZ is base + 46..106 m). The two were made equal by an F12 fixup on the argument that "a curtain is
        // the deck's underside like the veil" - which is true of WHERE they hang and false of HOW MUCH cloud stands over them, and the gloom gradient reads the second.
        static constexpr F32 SHEET_BURIED = SSDeckShade::VEIL_DEPTH;

        // The deck's storm gloom, kept for the render pass's ss_gloom uniform - per deck, not per puff, so it never belonged in the vertex colour.
        F32 mGloom = 1.f;

        // <SS:Nexii> D3 [interaction: ssdeckboilcore.h]: THE BOIL CLOCK's two accumulators, per deck because both
        // rates are per deck (mDriftRate, mChurn). F64 by PLAN.md lesson 13 - these are summed every frame for as
        // long as the viewer is open - and folded to their own periods by SSDeckBoil::wrapLaps / wrapFallM only at
        // the uniform boundary, so the F32 the shader sees keeps its full mantissa forever. They replace the
        // `ss_time * rate` products ssVolCloudF.glsl used to compute; see SSDeckBoil's header for why that shape
        // was the once-a-second step the fourth build report describes. Advanced once per update() for every deck,
        // built or not, so a deck that drops under the coverage floor for a few seconds does not resume on a stale
        // phase. Animation only - it advances a texture lookup and positions nothing (PLAN.md lesson 31).
        F64 mBoilLaps = 0.0;
        F64 mFallM = 0.0;

        F32 mMeanDistSq = 0.f;

        // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): this build's own tally of SSDeckLod::subsAt(dist, dial)
        // summed over every occupied cell, and the cell count it was summed over - the V3 debug view's
        // "LOD-predicted" puff count against mPuffs.size() (what actually survived thinning/budget) and the
        // SSAtmoCloudPuffBudget dial. Reset and filled once per buildDeck() call; not read by anything else.
        S64 mLodSubsTally = 0;
        S32 mLodCellTally = 0;

        // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): the count of Tier B macro-puff bodies this build
        // actually emitted (occupancy > 0 and macroEligible, after the whole fine cell walk) - the V3 debug
        // legend's "Tier B" line. Reset to 0 at the top of every buildDeck() call alongside mLodSubsTally/
        // mLodCellTally; not read by anything else.
        S32 mTierBCount = 0;
    };

    // <SS:Nexii> Phase 4 fixup (#5/F2/F8, doc/atmo_magic_wind_profile.md section 4): track/phase are the PURE inputs
    // the O(z) table bakes from - SSAtmoEnvApplier::windProfileAt(track, phase) and the track's own authored dome
    // height, never the live applier's windProfile()/cirrusAltitudeMetres() (per-client setting + live deck lid).
    void buildDeck(Deck& deck, const SSAtmoEnvCloudFieldState& field, F32 convection, F32 moisture, U32 salt,
                   const SSAtmoEnvTrack& track, F64 phase);
    bool fetchDeckTextures(Deck& deck);

    // <SS:Nexii> The noise map's two answers for one point of a deck's field, in the AIR frame (drift already subtracted): presence after the moisture floor and convection's say, and the tower weight the gradient ramp hands back. Shared by the deck builder and the precipitation gate so both always agree about where the holes are.
    void noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower) const;

    // <SS:Nexii> Phase 3 overload: same two answers, plus the RAW map sample (0 when there is no map cached yet, matching noiseSample's own "not ready" reading before the hole/tower ramps run) - what SSStormCouple::towerFromMap needs to re-derive `tower` under the storm-widened window without this function's `presence` output changing at all (still just the moisture/convection hole ramp, untouched by the storm). The 4-arg overload above delegates here and drops raw_n.
    // <SS:Nexii> Phase 6b (doc/atmo_magic_far_clouds.md section 2 step 4, ssdecknoisecore.h CONTRACT): this is the
    // ONE place every CPU consumer of presence/n_map shares - internally it now reads noiseSample TWICE (the plain
    // point, then SSDeckNoise::detileCoord(point)) and mixes them with SSDeckNoise::mixDetile before running the
    // hole/tower ramps, so the map's own SS_NOISE_TILE_M repeat stops being a visible pattern at horizon reach.
    // raw_n is the de-tiled sample, not a single read - every caller of this overload (buildDeck, precipNoiseAt,
    // bakeGroundShadow's cell and texel loops) gets the de-tiled field. No caller of either overload may read
    // noiseSample directly for presence or n_map purposes any more; route through here instead.
    void noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower, F32& raw_n) const;

    // Wrapped bilinear sample of the cached grid, or -1 when the deck has no map cached yet. A single, un-mixed
    // read - noiseFieldAt is the only caller that may use this for presence/n_map; it applies the de-tile mix.
    F32 noiseSample(const Deck& deck, F32 air_x, F32 air_y) const;

    // Folds the noise map's raw readback into the deck's small wrapped sample grid.
    void cacheNoiseGrid(Deck& deck, LLImageRaw* raw);

    // Generates (once per seed) the deck's square procedural noise map when nothing is
    // authored, and folds it into the same grid an authored map would fill.
    void ensureProceduralNoise(Deck& deck, U32 salt);

    // The vertical profile ramp: fetched and read back like the noise map, folded into one
    // averaged curve per channel (row 0 = the deck's base), and read on the CPU by the puff
    // placement so geometry and fragment carving run the same authored profile.
    void cacheProfileCurve(Deck& deck, LLImageRaw* raw);
    F32 profileSample(const Deck& deck, F32 v, S32 channel) const;

    // Which deck the weather reads: the authored source when it names the under deck and that
    // deck is on, the main field otherwise - the same default every "how much cloud is overhead"
    // consumer uses.
    const Deck* weatherDeck() const;

    Deck mPrimary;
    Deck mUnder;

    S32 mWeatherDeck = 0;

    // <SS:Nexii> Phase 3 (doc/atmo_magic_storm_dynamics.md section 3): the storm cells coupled into THIS build of
    // the primary/weather deck, fetched once per buildDeck call via SSStormCells::fillUniforms rather than once per
    // grid cell - the selection (hero first, then anchor-nearest) does not depend on the sample point, only on the
    // frame's resolved cell set. mStormInterest is claimed lazily on the first build and held for the singleton's
    // life, exactly the SSAtmoInfoView/SSAtmoSyncConsole idiom, because SSStormCells::update() early-outs unless
    // something currently claims it - without this the coupling would silently see zero cells forever.
    SSStormCells::Interest mStormInterest;
    S32 mStormCellCount = 0;
    SSStormCouple::CellUniform mStormCells[SSStormCouple::MAX_CELLS];

    // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): the active squall line's deck coupling for
    // THIS build, fetched once per buildDeck call via SSStormCells::fillLineBand right next to mStormCells above -
    // class-level for the same reason (precipNoiseAt and the ground-shadow bake read it outside the Deck that
    // produced it). Default LineBand() (strength 0) reads as "no line" by the core's own invariant.
    SSStormCouple::LineBand mLineBand;

    // <SS:Nexii> Phase 4 (doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud drift"): the hero's
    // storm-local frame shift inputs for THIS build - class-level like mStormCells/mStormCellCount above (not
    // per-Deck) because precipNoiseAt and the ground-shadow bake read it outside the Deck that produced it, the
    // same pattern mStormCells already establishes. Built once per weather-deck build inside buildDeck, whenever
    // couple_storm is true AND SSStormCells::hero() names an active hero; zero (ageS 0) otherwise, which makes
    // SSDeckFrame::heroShift and foldFrameKey both read as "no hero" bit-exactly by their own invariants.
    SSDeckFrame::HeroFrame mHeroFrame;

    // <SS:Nexii> V4 debug snapshot (see the public SSVirgaDebug/virgaDebug() above): class-level like
    // mStormCells/mHeroFrame, for the same reason - buildDeck runs per Deck but the shaft path only ever coupled
    // to whichever deck IS weatherDeck(), so one snapshot per update() is correct, not one per Deck.
    SSVirgaDebug mVirgaDebug;

    F32 mLastBuildMS = 0.f;

    LLVector3 mLightDir;

    F32 mBeam = 1.f;

    // <SS:Nexii> The ground shadow's baked transmittance map: how much direct sun survives a straight fall through the primary deck, per point of the field, baked from the SAME cell gate, presence cut and noise mottle the builder places puffs by - so the shadows on the ground are cast by exactly the clouds in the sky. Air-frame anchored (origin/span in air metres, cell-quantised so the key below is honest); the bind translates by the live drift each frame, which is what makes the shadows crawl with the deck for free. Rebaked only when mShadowKey changes - update() rebuilds the FIELD every frame, and a per-frame texture upload is exactly the cost the key exists to refuse.
    void bakeGroundShadow(const Deck& deck, F32 air_x, F32 air_y);

    LLPointer<LLViewerTexture> mShadowRef;

    // The bake's CPU copy, kept for the debug overlay's ground-shadow view - it draws the very texels the shader samples, projected the same way, so map and rendered shadow can be read against each other.
    LLPointer<LLImageRaw> mShadowRaw;
    F32 mShadowOriginX = 0.f;
    F32 mShadowOriginY = 0.f;
    F32 mShadowSpanM = 0.f;
    U64 mShadowKey = 0;
    bool mShadowValid = false;

    F32 mEffRadius = 5000.f;
    F32 mSquashKnee = 1600.f;
    F32 mSquashCap = 2000.f;

    // <SS:Nexii> LOD phase (ssdecklodcore.h CONTRACT): render()'s own draw-order hysteresis state - the `prev`
    // SSDeckLod::underOnTop reads and updates each frame, so the primary/under blend order does not flip back
    // and forth on a frame-to-frame mMeanDistSq jitter right at the crossover.
    bool mUnderOnTop = false;

public:
    F32 squashScale(F32 true_dist) const;
    F32 squashKnee() const { return mSquashKnee; }
    F32 squashCap() const { return mSquashCap; }
    F32 virtualRadius() const { return mEffRadius; }

private:

    LLRenderTarget mDepthCopy;
    U32 mDepthCopyFrame = 0;

    std::vector<LLVector4> mStrikeLights;

    std::unordered_map<U64, std::vector<S32>> mOccGrid;
    std::vector<U32> mOccStamp;
    U32 mOccQuery = 0;
    F32 mMaxPuffR = 0.f;
    bool mOccGridDirty = true;
};

#endif
