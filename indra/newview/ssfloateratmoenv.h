/**
 * @file ssfloateratmoenv.h
 * @brief Atmo Magic: environment editor floater.
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

#ifndef SS_FLOATERATMOENV_H
#define SS_FLOATERATMOENV_H

#include "llfloater.h"
#include "llinventorysettings.h" // <SS:Nexii> LLSettingsType, for classifying dropped settings
#include "ssatmoenvasset.h" // <SS:Nexii> SS_ATMOENV_REGION_CEILING, for the rail's track-mode range
#include "ssatmoenvkeyframe.h"
#include "ssatmoenvweatherstate.h" // <SS:Nexii> SSAtmoEnvPrecipIntensity, the forecast strip's bands

#include <functional>
#include <string>
#include <utility>
#include <vector>

class LLInventoryItem;
class LLViewerInventoryItem;

class SSFloaterAtmoEnv : public LLFloater
{
public:
    SSFloaterAtmoEnv(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void onVisibilityChange(bool new_visibility) override;
    void draw() override;

    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    // <SS:Nexii> The EEP sky import dialog drives the same preview/status refresh the drop path used to - the stamp happens there now, and the poll alone would leave fresh keyframes and the modified asterisk up to half a second late.
    friend class SSFloaterAtmoSkyImport;

private:
    void handleSettingsDrop(const LLInventoryItem* item);

    // <SS:Nexii> The drop session: a drag-and-drop can carry many cargo items, and the editor only acts once the whole batch is known. Only two shapes are accepted - an all-skies drop (any number, seeding a new environment when none is loaded or stamping a day cycle across the selected track) and a SINGLE day cycle or water preset. Mixed drops are refused at the hover pass, so they never reach the drop pass.
    enum class EDropKind
    {
        NONE,
        SKIES,        // one or more skies dropped together
        SINGLE_WATER, // a lone water preset
        SINGLE_DAY,   // a lone EEP day cycle
        MIXED         // the drag mixed kinds the editor refuses
    };

    struct DropItem
    {
        // <SS:Nexii> The item id is the inventory handle (re-looked-up on action); the asset id is
        // what the fetch-and-seed paths actually download, so both travel with the drop.
        LLUUID mItemId;
        LLUUID mAssetId;
        std::string mName;
        LLSettingsType::type_e mType = LLSettingsType::ST_NONE;
    };

    // Classifies one dropped settings item against the editor's rules; ok=false carries the
    // reason in tooltip_msg.
    EDropKind classifySettingsDrop(const LLViewerInventoryItem* item, bool& ok, std::string& tooltip_msg) const;
    // Acceptance + tooltip for the hover pass; accumulates the per-drag kind so a mixed batch
    // reads as refused. Runs once per cargo item, reset at cargo index 0.
    void hoverAcceptSettings(const LLViewerInventoryItem* item, bool& ok, std::string& tooltip_msg);
    // The drop pass: buffers each dropped settings item; on the last cargo of the
    // batch, runs the session's chosen action.
    void dropBufferSettings(const LLViewerInventoryItem* item);
    void flushDropSession();
    void handleWaterStamp(const DropItem& item);

    // The settings drop acted on only once the whole drag has settled.
    std::vector<DropItem> mDropItems;
    EDropKind mDropKind = EDropKind::NONE;
    // The hover pass's accumulating kind for the current drag - decides whether a mixed
    // batch is refused and whether the batch is a group or a single item.
    EDropKind mHoverKind = EDropKind::NONE;

    // <SS:Nexii> The transitionary state: an async fetch-and-seed (or notecard load) is in flight, so the floater refuses input until it settles. Counted so nested and racing completions cannot clear a busy state a sibling still uses.
    void setBusy(const std::string& label);
    void clearBusy();
    void refreshBusy();
    void refreshLandingBullets();
    S32 mBusyOps = 0;
    std::string mBusyLabel;
    // <SS:Nexii> The landing rows' bullet glyphs hang from code once so the en skin stays ASCII-only; guards against double-prefixing when the panel is shown again.
    bool mLandingBulletsPrepared = false;

    void refreshVisibility();

    void refreshStatus();

    void onClickCreateEmpty();
    void onClickCreateStock();
    void onClickLoadFromParcel();
    void onClickSave();
    void onClickRevert();

    void onClickUnload();
    void onClickAddTrack();
    void onClickRemoveTrack();
    void onCommitName();

    void refreshTrackRail();

    void repositionRailMarkers();

    void onCommitAltitudeSlider();

    void onMouseUpAltitudeSlider();

    void onClickGroundRow();

    void onClickWeatherInfluence();

    // <SS:Nexii> The weather cube's two wholesale operations - a fresh roll, and back to nothing.
    // See SSAtmoEnvWeatherGenerator; setWeatherRollText carries the roll's own description down to
    // the line under the rows.
    void onClickRandomizeWeather();
    void onClickRemoveWeather();
    void setWeatherRollText(const std::string& text);

    void refreshAltitudeLabel(S32 slot);

    S32 railCentreForValue(F32 value) const;

    static void centreViewOn(LLView* view, S32 centre_y);

    void selectTrack(S32 index);

    S32 mSelectedTrackIndex = 0;

    // <SS:Nexii> The altitude rail runs in two modes rather than there being two rails. Track mode is the region scale with a marker per track, as always. Any other tab switches to layer mode: the scale fits the selected track's own contents and the markers become the things stacked inside it - water plane, main deck, optional under deck - with Space and the Dome pinned above as fixed anchors excluded from the fit. One widget with two modes because the track markers already carry two meanings (altitude and tab selection) and a third kind of object would overload the control. A fixed scale cannot serve both a coastal build inside 100m and a sky archipelago spanning 10km, and a piecewise one is worse - the same drag would mean 64m at one end and 800m at the other. See doc/atmo_magic_env_ui.md.
    enum class ERailMode { TRACK, LAYER };

    // Marker slots in layer mode, in the order the rail lists them.
    static const S32 LAYER_NONE  = -1;
    static const S32 LAYER_WATER = 0;
    static const S32 LAYER_MAIN  = 1;
    static const S32 LAYER_UNDER = 2;
    static const S32 LAYER_COUNT = 3;

    void refreshRailMode();
    // <SS:Nexii> Tab to rail: a directly clicked tab must press the marker it belongs to, or clear the still-pressed one when nothing on the rail corresponds - the reverse half of the rail's marker-to-tab selection.
    void syncSelectionToTab();
    void refreshLayerRail();
    void railRangeForTrack(F32& out_min, F32& out_max) const;

    // The higher of the track's floor and its water plane: what precipitation lands on.
    F32 weatherReferenceSurface() const;
    // LAYER_MAIN or LAYER_UNDER, honouring the track's authored override; LAYER_NONE if neither
    // deck sits above the reference surface.
    S32 weatherDeliveringDeck() const;
    F32 layerAltitude(S32 layer) const;
    bool layerPresent(S32 layer) const;
    // Base and thickness a deck resolves to at the preview phase: the auto derivation off the
    // track's weather when the field owns its numbers, else the authored keyframes. The rail
    // reads this rather than the raw keyframes so markers, fit and weather bracket follow the
    // deck the renderer draws - an auto deck's height wanders; its authored row does not.
    void effectiveDeckSpan(const SSAtmoEnvTrack& track, bool under_deck,
                           F32& out_base, F32& out_thickness) const;

    void selectLayer(S32 layer);
    void onClickLayerMarker(S32 layer);
    void onClickAddDeck();
    void onClickRemoveDeck();
    void onCommitWeatherSource();
    void refreshWeatherSource();
    // What the source combo was last built from. The selection starts at a value no live track
    // can hold, so the first refresh always builds; later refreshes rebuild only on change.
    S32  mWeatherSourceStaged = S32_MIN;
    bool mWeatherSourceUnderStaged = false;

    // <SS:Nexii> The precipitation combo lists two tiers: the shipped derivation vocabulary, read once from the XUI so the panel stays the single place it is written, and whatever types this environment carries of its own. Rebuilt whenever the environment's set changes.
    void refreshPrecipitationTypes();
    void onClickNewPrecipType();
    void onClickEditPrecipTypes();

    std::vector<std::pair<std::string, std::string>> mBuiltinPrecipItems;

    void drawWeatherBracket();

    ERailMode mRailMode = ERailMode::TRACK;
    S32 mSelectedLayer = LAYER_NONE;

    // The rail's live value range, and where it is heading. Interpolated in draw() so the mode
    // switch reads as diving into the selected track rather than the widget swapping contents.
    F32 mRailMin = 0.f;
    F32 mRailMax = SS_ATMOENV_REGION_CEILING;
    F32 mRailMinFrom = 0.f;
    F32 mRailMaxFrom = SS_ATMOENV_REGION_CEILING;
    F32 mRailMinTo = 0.f;
    F32 mRailMaxTo = SS_ATMOENV_REGION_CEILING;
    bool mRailZooming = false;
    F64 mRailZoomStart = 0.0;

    void refreshTrackTab();
    void onCommitTrackName();

    // <SS:Nexii> Seeds the selected track from a world archetype - see ssAtmoEnvTemplates().
    void onClickApplyTemplate();
    void onCommitDayCycle();

    void onCommitWaterEnabled();

    void onCommitUnderEnabled();
    void onCommitUnderAuto();

    bool waterRowsInactive() const;

    void refreshWaterRows();

    void onCommitGustAuto();
    void onCommitLightningAuto();
    void onCommitLightningFlags();
    void refreshLightningRows();
    void refreshAutoRows();
    void onCommitCloudAuto();
    void onCommitDomeAuto();
    void onCommitHorizonClip();

    bool rowAutoOwned(const std::string& prefix) const;

    void refreshPlanetaryScales();

    void onCommitPlanetaryScales();

// <SS:Nexii> Space tab's disc perception dial - radios or the custom slider/spinner, one value whichever moved (the source control carries it).
    void onCommitCelestialPerception(LLUICtrl* src);

    void onClickOpenPlanetaryDesigner();

    struct FloatRow
    {
        std::string mPrefix;
        std::function<SSAtmoEnvKeyframed<F32>&()> mField;

        bool mIntegerDisplay = false;

        F32 mScale = 1.f;
    };
    std::vector<FloatRow> mFloatRows;

    template <typename T>
    struct KeyRow
    {
        std::string mPrefix;
        std::function<SSAtmoEnvKeyframed<T>&()> mField;

        F32 mScale = 1.f;
    };
    std::vector<KeyRow<LLColor3>>  mColorRows;
    std::vector<KeyRow<LLVector2>> mVectorRows;
    std::vector<KeyRow<LLUUID>>    mTextureRows;
    std::vector<KeyRow<std::string>> mStringRows;
    std::vector<KeyRow<bool>>      mBoolRows;

    void refreshPreview();
    void onCommitPreviewTime();

    // <SS:Nexii> A keyframe as the scrubber draws it: one phase, which is the key's own, carrying
    // the mark, the label and the filled/hollow test alike. A HOLD key held a span as well for a
    // while, so that the mark could read filled anywhere across the stretch the value covers - but
    // filled has to mean what it means on the row's own keyframe button, which is that the head is
    // ON this key. Anything looser and the two disagree about the same key at the same instant.
    struct GhostKeyframe
    {
        F64 mDrawPhase = 0.0;

        std::string mLabel;
    };

    bool rowHovered(const std::string& prefix) const;

    bool collectHoveredKeyframes(std::vector<GhostKeyframe>& out, bool& out_labels) const;

    bool scrubberHovered() const;

    template <typename T, typename FormatFn>
    static void buildGhosts(const std::vector<SSAtmoEnvKeyframe<T>>& keyframes,
                            FormatFn format, std::vector<GhostKeyframe>& out);

    bool scrubberGeometry(LLRect& out_rect, S32& out_left_edge, S32& out_travel) const;

    void drawRiseSetMarkers();

    // <SS:Nexii> One column of the forecast strip - the weather cube resolved at that hour,
    // reduced to exactly what the strip draws. Resolved on the same half-second poll the rest
    // of the readouts refresh on rather than per frame: thirteen full resolves a frame is a lot
    // of string building for a display that cannot change faster than an edit or a scrub, and
    // the prose forecast it stands in for was already refreshed on that clock.
    struct ForecastCell
    {
        F64 mPhase = 0.0;
        S32 mHour = 0;
        S32 mOkta = 0;
        F32 mTemperatureC = 0.f;
        F32 mWindSpeed = 0.f;
        F32 mWindHeading = 0.f;
        S32 mPrecipPercent = 0;
        bool mFalling = false;
        bool mThunder = false;
        bool mDaylight = true;
        std::string mPrecipType;
        SSAtmoEnvPrecipIntensity mBand = SSAtmoEnvPrecipIntensity::NONE;
    };
    std::vector<ForecastCell> mForecastCells;

    // <SS:Nexii> The precipitation band under the columns, sampled far finer than they are - one
    // slot every STRIP_PRECIP_PITCH pixels, kept only where something is actually falling. The
    // columns say what an HOUR is like, which is the wrong unit for a shower: a spell that starts
    // at 07:40 and stops at 09:20 lands between two-hourly columns and reads as a whole morning of
    // rain from them. The band draws the extent itself, so it starts and stops where the rain does.
    // The resolved BAND rides along rather than a drawn shape, because how heavy a fall looks is
    // the drawing layer's call, not the model's.
    struct ForecastMark
    {
        F64 mPhase = 0.0;
        SSAtmoEnvPrecipIntensity mBand = SSAtmoEnvPrecipIntensity::NONE;
        std::string mType;
    };
    std::vector<ForecastMark> mForecastMarks;

    void refreshForecastStrip();

    // <SS:Nexii> The hour-by-hour weather rack above the scrubber. Drawn, not built - see the
    // comment on the definition for why, and the STRIP_* constants for the vertical rack.
    void drawForecastStrip();

    void drawKeyframeGhosts();

    void drawSliderValueGhosts();

    void refreshFloatRow(const FloatRow& row, F64 phase);
    void commitFloatRow(const FloatRow& row);
    void commitFloatRowSpinner(const FloatRow& row);
    void toggleFloatRowKeyframe(const FloatRow& row);
    void jumpFloatRowPrev(const FloatRow& row);
    void jumpFloatRowNext(const FloatRow& row);

    template <typename T>
    void refreshKeyframeControls(const std::string& prefix, const SSAtmoEnvKeyframed<T>& field, F64 phase);
    template <typename T>
    void toggleKeyframe(SSAtmoEnvKeyframed<T>& field);
    template <typename T>
    void jumpKeyframe(const SSAtmoEnvKeyframed<T>& field, bool next);

    template <typename T>
    void bindKeyframeButtons(const std::string& prefix, std::function<SSAtmoEnvKeyframed<T>&()> field);

    void refreshColorRow(const KeyRow<LLColor3>& row, F64 phase);
    void commitColorRow(const KeyRow<LLColor3>& row);
    void refreshVectorRow(const KeyRow<LLVector2>& row, F64 phase);
    void commitVectorRow(const KeyRow<LLVector2>& row);
    void commitVectorSpinners(const KeyRow<LLVector2>& row);
    void refreshTextureRow(const KeyRow<LLUUID>& row, F64 phase);
    void commitTextureRow(const KeyRow<LLUUID>& row);
    void refreshStringRow(const KeyRow<std::string>& row, F64 phase);
    void commitStringRow(const KeyRow<std::string>& row);
    void refreshBoolRow(const KeyRow<bool>& row, F64 phase);
    void commitBoolRow(const KeyRow<bool>& row);

    F64 mPreviewPhase = 0.0;

    bool mPreviewPlaying = false;
    F64 mPreviewPlayLast = 0.0;
    void onClickPreviewPlay();
    void advancePreviewPlayback();

    F64 mLastPoll = 0.0;
};

#endif
