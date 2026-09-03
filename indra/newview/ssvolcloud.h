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
#include "v2math.h"
#include "v3color.h"
#include "v3math.h"
#include "v4math.h"

#include <vector>
#include <unordered_map>

struct SSAtmoEnvCloudFieldState;
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

    // <SS:Nexii> The primary deck's geometry and coverage: the auto dome altitude derivation and every consumer that asks "how much cloud is overhead" mean the main field, not the under deck bolted on below a sky build.
    F32 cloudBaseZ() const { return mPrimary.mBaseZ; }
    F32 cloudTopZ() const { return mPrimary.mBaseZ + mPrimary.mThicknessM; }
    bool empty() const { return mPrimary.mPuffs.empty(); }

    // <SS:Nexii> The under deck's live world-frame band and whether it has a built field at all. Lightning reads these to decide whether a ground strike would cross the deck (and so be re-routed to cloud-to-cloud) and how deep its in-cloud crawl may dive. The band is only meaningful while underPresent() holds.
    F32 underBaseZ() const { return mUnder.mBaseZ; }
    F32 underTopZ() const { return mUnder.mBaseZ + mUnder.mThicknessM; }
    bool underPresent() const { return !mUnder.mPuffs.empty(); }

    // <SS:Nexii> How solid the under deck is over one point of the sky, 0-1 - the noise map's presence gate for the deck's own column, in the same air frame the clouds drift in. 1 is solid cloud at that spot, 0 a hole or gap. Neutral 1 when the deck has no map cached yet.
    F32 underPresenceAt(const LLVector3& pos_agent) const;

    F32 transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength);

    S32 puffCount() const { return (S32)(mPrimary.mPuffs.size() + mUnder.mPuffs.size()); }
    F32 lastBuildMS() const { return mLastBuildMS; }

    // <SS:Nexii> The convection noise map's gate for the weather. Precipitation asks the deck it falls from two questions about a point of the sky: how much cloud is over it (x, a hole in the map reads zero and takes the rain with it) and how tower-like the column is (y, which tweaks the intensity toward the dense parts). The point handed in must already be the WIND-TILTED one - where a drop falling at the weather's angle entered the deck, not where it lands - because only the caller knows the fall; this side supplies everything else, drift included. A deck with no map, or whose map has not read back yet, answers neutral: everything present, nothing tower-like. [interaction: precipitation]
    LLVector2 precipNoiseAt(const LLVector3& pos_agent) const;

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
    };

    // <SS:Nexii> One resolved cloud deck. The primary storm field and the optional under deck are the same renderer run twice - each with its own resolved field state, textures, puff set and uniforms - so a sky-themed build can hang a second layer at the bottom of the build while the weather-driven deck stays overhead. Drawn far deck first; within a deck the puffs stay depth-sorted, and decks separated by hundreds of metres hide the cross-deck ordering.
    struct Deck
    {
        // <SS:Nexii> The deck's bodies: the builder's cell-placed puffs, plus - when
        // SSAtmoCloudTessellation is on - the smaller refinement children hung on the ones near
        // the eye. Children are ordinary Puffs: same sort, same budget, same fragment carve, so
        // nothing downstream distinguishes them.
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

        // <SS:Nexii> The base veil: one soft sheet inset into the deck's floor, drawn under the puffs so the field reads with its gaps filled instead of as balls over empty sky. Same texture as the puffs, sampled aperiodically in the shader; the form here is the shade a puff at the deck's floor would wear - the same formulas as the puff loop, lit by the same vertex-stage sky light - so sheet and lowest puffs share one lighting. mSheetZ is the sheet's altitude (the inset), mSheetAlpha its ceiling.
        F32 mSheetForm = 1.f;
        F32 mSheetZ = 0.f;
        F32 mSheetAlpha = 0.f;

        // The veil IS the deck's floor, so it is buried under the whole column and takes the gloom gradient's dark end whole - see Puff::mBuried.
        static constexpr F32 SHEET_BURIED = 1.f;

        // The deck's storm gloom, kept for the render pass's ss_gloom uniform - per deck, not per puff, so it never belonged in the vertex colour.
        F32 mGloom = 1.f;

        F32 mMeanDistSq = 0.f;
    };

    void buildDeck(Deck& deck, const SSAtmoEnvCloudFieldState& field, F32 convection, F32 moisture, U32 salt);
    bool fetchDeckTextures(Deck& deck);

    // <SS:Nexii> The noise map's two answers for one point of a deck's field, in the AIR frame (drift already subtracted): presence after the moisture floor and convection's say, and the tower weight the gradient ramp hands back. Shared by the deck builder and the precipitation gate so both always agree about where the holes are.
    void noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower) const;

    // Wrapped bilinear sample of the cached grid, or -1 when the deck has no map cached yet.
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
