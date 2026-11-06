/**
 * @file gmdxregioncapture.cpp
 * @brief GunMetal-DX region maps: maps of whole regions, shared between users, on the mini-map and World Map.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> whole file: see gmdxregioncapture.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxregioncapture.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "lldate.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llfloaterpreference.h"
#include "llfloaterreg.h"
#include "llimagej2c.h"
#include "llimagepng.h"
#include "llmeshrepository.h"
#include "llnotificationsutil.h"
#include "llpresetsmanager.h"
#include "llregionhandle.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "llsurface.h"
#include "lltextbox.h"
#include "lltexturefetch.h"
#include "lluri.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llworld.h"
#include "llworldmap.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "workqueue.h"

#include <boost/json.hpp>
#include <algorithm>
#include <map>
#include <sstream>

bool GMDXRegionCapture::sLodOn = false;
F32  GMDXRegionCapture::sLodBox[4] = {0.f, 0.f, 0.f, 0.f};
U32  GMDXRegionCapture::sLodEpoch = 0;

namespace
{
constexpr S32 kMapSize       = 4096;   // output px (square)
constexpr S32 kGuardPx       = 16;     // per tile side, rendered and cropped away
inline F32 eyeAbove(F32)     { return 50.f; } // m above ceiling
constexpr F32 kFloorMargin   = 2.f;    // m below the lowest land / water
constexpr F32 kFarMargin     = 32.f;   // m past the farthest corner
constexpr F32 kNetFarMax     = 512.f;  // the simulator's object range cap
constexpr S32 kProbeSize     = 512;    // preparation render px
constexpr F64 kProbeEvery    = 1.5;    // s
constexpr F64 kMinPrepare    = 6.0;    // s before loading signals are believed
constexpr F64 kSettleHold    = 5.0;    // s signals must hold
constexpr S32 kMeshPendingOk = 2;
constexpr S32 kTexPendingOk  = 8;
constexpr F32 kSettleTimeout = 180.f;

// Windows that snapshot settings and write the snapshot back as saved values:
const char* openGraphicsWindow()
{
    static const char* const kNames[] = { "preferences", "performance", "save_pref_preset" };
    for (const char* n : kNames)
        if (LLFloaterReg::instanceVisible(n)) return n;
    return nullptr;
}

// Overridden settings must not clear active graphics preset
struct PresetsQuiet
{
    PresetsQuiet()  { if (LLPresetsManager::instanceExists()) LLPresetsManager::setIgnoreChangeSignal(true); }
    ~PresetsQuiet() { if (LLPresetsManager::instanceExists()) LLPresetsManager::setIgnoreChangeSignal(false); }
};

S32 texturesUnsettled()
{
    if (LLTextureFetch* fetch = LLAppViewer::getTextureFetch())
    {
        return fetch->getNumHTTPRequests();
    }
    return 0;
}

enum class Stage { Idle, Confirm, Prepare, Render, Save };

struct Override
{
    std::string name;
    LLSD original, original_saved, value;
};

// A shared map file (exportMap / importMap): this line, one line of JSON (the sidecar's fields +
// grid, region_grid_x / region_grid_y, payload "j2c" / "png", payload_size), then the image file.
const std::string kShareMagic = "GMDXMAP1\n";
// A map texture's description: "GMDXMAP1 <grid x> <grid y> <region width> <region id>" (the
// inventory description holds 127 characters), and its name: the prefix + the region's name.
const std::string kTextureTag = "GMDXMAP1 ";
const std::string kTextureNamePrefix = "Region map: ";
constexpr S64 kShareMaxBytes = 96 * 1024 * 1024;
constexpr S32 kShareMinPx = 256, kShareMaxPx = 8192;
constexpr S32 kGridMax = (1 << 24) - 1;   // a region's grid x / y (x * 256 fits a region handle's U32)
constexpr S32 kMinimapPx = 2048;          // the mini-map's texture of a map (a few hundred pixels on screen)

struct MapTex
{
    LLPointer<LLViewerTexture> tex;
    bool loading = false;
};

// This grid's name in the maps folder and in a map file's "grid", as GunMetal-DX writes it in
// shared maps: "Agni" / "Aditi" on Second Life, else the grid's login host and port, lowercased
// ("grid.example.org:8002"). (Viewers name OpenSim grids differently -- Firestorm's grid list
// here, a grids.xml entry in GunMetal-DX -- but agree on the login address.)
std::string grid_id()
{
    LLGridManager* grids = LLGridManager::getInstance();
#ifdef OPENSIM
    if (grids->isInSLMain()) return "Agni";
    if (grids->isInSLBeta()) return "Aditi";
#else
    if (grids->isSystemGrid()) return grids->getGridId();
#endif
    std::vector<std::string> uris;
    grids->getLoginURIs(uris);
    const LLURI uri(uris.empty() ? std::string() : uris.front());
    const std::string host = utf8str_tolower(uri.hostName());
    return host.empty() ? utf8str_tolower(grids->getGrid()) : host + llformat(":%u", (U32)uri.hostPort());
}
} // namespace

// The mini-map's loaded maps and the files on disk.
struct GMDXRegionCaptureState
{
    std::map<U64, MapTex> maps;
    F64 next_prune = 0.0;

    Stage stage = Stage::Idle;
    U64 handle = 0;
    LLUUID region_id;
    std::string region_name;
    F32 width = 256.f;
    F32 ceiling = 0.f, floor = 0.f;
    F32 water = 0.f;
    bool ocol_water = false;
    bool shadows = true;
    F32 far_needed = 0.f, far_reach = 0.f;
    std::vector<Override> overrides;
    F64 start = 0.0, settled_since = -1.0, next_probe = 0.0, count_at = 0.0, next_log = 0.0;
    S32 count = -1;
    U32 probes = 0;
    bool timed_out = false;
    S32 tiles = 4, tile_px = kMapSize / 4, next = 0;
    U32 aa = 0;
    F32 time = 0.f;
    LLPointer<LLImageRaw> image;
    LLNotificationPtr progress;
    std::string stop_request;
    bool far_capped_told = false;
    S32 textures = 0;
    F64 next_tex_scan = 0.0;

    static GMDXRegionCaptureState& get() { static GMDXRegionCaptureState s; return s; }

    // ---- why a capture cannot run now ("" = it can) ----
    static std::string blocked()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || gDisconnected || !gAgent.getRegion())
            return "You need to be in a region.";
        if (RlvActions::isRlvEnabled()
            && (RlvActions::hasBehaviour(RLV_BHVR_SETSPHERE) || RlvActions::hasBehaviour(RLV_BHVR_SHOWMINIMAP)
                || RlvActions::hasBehaviour(RLV_BHVR_SHOWLOC)
                || RlvActions::hasBehaviour(RLV_BHVR_SETCAM_TEXTURES) || RlvActions::hasBehaviour(RLV_BHVR_SETCAM_AVDISTMAX)
                || RlvActions::hasBehaviour(RLV_BHVR_SETCAM_ORIGINDISTMAX)))
            return "Region maps are not available under your current RLV restrictions.";
        if (openGraphicsWindow())
            return "Close Preferences and the graphics settings windows first: they would save the capture's "
                   "temporary settings as yours.";
        return std::string();
    }

    static F32 farNeeded(const LLViewerRegion* region)
    {
        const LLVector3 o = region->getOriginAgent();
        const LLVector3 from[2] = { gAgent.getPositionAgent(), gAgentCamera.getCameraPositionAgent() };
        const F32 w = region->getWidth();
        F32 d2 = 0.f;
        for (const LLVector3& a : from)
            for (int c = 0; c < 4; ++c)
            {
                const F32 dx = o.mV[VX] + ((c & 1) ? w : 0.f) - a.mV[VX];
                const F32 dy = o.mV[VY] + ((c & 2) ? w : 0.f) - a.mV[VY];
                d2 = llmax(d2, dx * dx + dy * dy);
            }
        return ceilf(sqrtf(d2) + kFarMargin);
    }

    static F32 netFarCap()
    {
        if (gSavedSettings.controlExists("SLNetDrawDistanceMax"))
        {
            const F32 s = gSavedSettings.getF32("SLNetDrawDistanceMax");
            return s > 0.f ? llmin(s, kNetFarMax) : kNetFarMax;
        }
        return kNetFarMax;
    }

    void setOverride(const char* name, const LLSD& value)
    {
        LLControlVariablePtr c = gSavedSettings.getControl(name);
        if (c.isNull() || llsd_equals(c->getValue(), value)) return;
        overrides.push_back({ name, c->getValue(), c->getSaveValue(), value });
        c->setValue(value, false);
        overrides.back().value = c->getValue();
    }

    void restoreAll()
    {
        PresetsQuiet quiet;
        for (auto it = overrides.rbegin(); it != overrides.rend(); ++it)
        {
            LLControlVariablePtr c = gSavedSettings.getControl(it->name);
            if (c.isNull() || !llsd_equals(c->getValue(), it->value)) continue;
            if (llsd_equals(it->original, it->original_saved))
            {
                c->setValue(it->original, true);
            }
            else
            {
                if (!llsd_equals(c->getSaveValue(), it->original_saved)) c->setValue(it->original_saved, true);
                c->setValue(it->original, false);
            }
        }
        if (!overrides.empty()) LL_INFOS("RegionMap") << "restored " << overrides.size() << " settings" << LL_ENDL;
        overrides.clear();
        if (GMDXRegionCapture::sLodOn) ++GMDXRegionCapture::sLodEpoch;
        GMDXRegionCapture::sLodOn = false;
    }

    void applyOverrides()
    {
        PresetsQuiet quiet;
        if (far_needed > gSavedSettings.getF32("RenderFarClip")) setOverride("RenderFarClip", LLSD((F64)far_needed));
        setOverride("SceneLoadFrontPixelThreshold", LLSD(0.0));
        setOverride("SceneLoadRearPixelThreshold", LLSD(0.0));
        setOverride("RenderTerrainLODFactor", LLSD(4.0));
        setOverride("RenderTreeLODFactor", LLSD(2.0));
        setOverride("RenderDeferredSSAO", LLSD(false));
        setOverride("RenderScreenSpaceReflections", LLSD(false));
        setOverride("RenderDepthOfField", LLSD(false));
        setOverride("RenderGlow", LLSD(false));
        if (!shadows)
        {
            setOverride("RenderShadowDetail", LLSD(0));
        }
        else
        {
            if (gSavedSettings.getS32("RenderShadowDetail") < 1) setOverride("RenderShadowDetail", LLSD(1));
            if (gSavedSettings.getU32("RenderShadowResolution") < 2048) setOverride("RenderShadowResolution", LLSD(2048));
        }
    }

    void setLodBox(const LLViewerRegion* region)
    {
        const LLVector3 o = region->getOriginAgent();
        GMDXRegionCapture::sLodBox[0] = o.mV[VX];
        GMDXRegionCapture::sLodBox[1] = o.mV[VY];
        GMDXRegionCapture::sLodBox[2] = o.mV[VX] + width;
        GMDXRegionCapture::sLodBox[3] = o.mV[VY] + width;
    }

    void stop(const std::string& why)
    {
        LL_INFOS("RegionMap") << "capture of " << region_name << " stopped: " << why << LL_ENDL;
        restoreAll();
        image = nullptr;
        stage = Stage::Idle;
        if (progress) { LLNotificationsUtil::cancel(progress); progress.reset(); }
        LLSD args;
        args["REGION"] = region_name;
        args["REASON"] = why;
        LLNotificationsUtil::add("RegionMapStopped", args);
    }

    void begin()
    {
        LLViewerRegion* region = gAgent.getRegion();
        std::string why = blocked();
        if (why.empty() && (!region || region->getHandle() != handle)) why = "You left the region.";
        stage = Stage::Idle;
        if (!why.empty()) { message(why); return; }
        if (!LLEnvironment::instance().getIsSunUp())
        {
            LLSD args;
            args["REGION"] = region->getName();
            LLNotificationsUtil::add("RegionMapNight", args);
            return;
        }
        region_id = region->getRegionID();
        region_name = region->getName();
        width = llmax(region->getWidth(), 16.f);
        ceiling = GMDXRegionCapture::ceilingFor(region);
        water = region->getWaterHeight();
        ocol_water = gSavedSettings.getBOOL("SLRegionMapOcolWater");
        shadows = gSavedSettings.getBOOL("SLRegionMapShadows");
        floor = llmin(region->getLand().getMinZ(), water) - kFloorMargin;
        ceiling = llmax(ceiling, floor + 1.f);
        far_needed = farNeeded(region);
        far_reach = llmin(far_needed, netFarCap());
        applyOverrides();
        setLodBox(region);
        GMDXRegionCapture::sLodOn = true;
        ++GMDXRegionCapture::sLodEpoch;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        start = now;
        settled_since = -1.0;
        next_probe = now;
        next_log = now + 10.0;
        count = -1;
        count_at = now;
        probes = 0;
        timed_out = false;
        stage = Stage::Prepare;
        LL_INFOS("RegionMap") << "capturing " << region_name << " (" << width << " m): ceiling " << ceiling
                              << " m, floor " << floor << " m, draw distance " << far_needed << " m (simulator "
                              << far_reach << " m), " << overrides.size() << " settings overridden" << LL_ENDL;
        far_capped_told = false;
        textures = 0;
        next_tex_scan = now;
        stop_request.clear();
        const F32 s_timeout = kSettleTimeout;
        showProgress(llformat("loading the whole region (up to %.0f s), then rendering it. Everything above %.0f m "
                              "is left out.", llmax((F32)s_timeout, (F32)(kMinPrepare + kSettleHold)), ceiling));
    }

    void showProgress(const std::string& phase)
    {
        if (progress) { LLNotificationsUtil::cancel(progress); progress.reset(); }
        LLSD args;
        args["REGION"] = region_name;
        args["PHASE"] = phase;
        progress = LLNotificationsUtil::add("RegionMapProgress", args, LLSD(),
            [](const LLSD& n, const LLSD& r)
            {
                GMDXRegionCaptureState& s = get();
                if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                s.progress.reset();
                if (s.stage == Stage::Prepare || s.stage == Stage::Render) s.stop("cancelled");
            });
    }

    bool renderWithoutAvatars(const LLRegionOrthoView& v, std::vector<U8>& rgb, std::vector<F32>* depth = nullptr) const
    {
        const U32 types[] = { LLPipeline::RENDER_TYPE_AVATAR, LLPipeline::RENDER_TYPE_PARTICLES,
                              ocol_water ? (U32)LLPipeline::RENDER_TYPE_WATER : 0u };
        bool off[3] = {};
        for (int k = 0; k < 3; ++k)
            if (types[k] && (off[k] = gPipeline.hasRenderType(types[k]))) LLPipeline::toggleRenderTypeControl(types[k]);
        const bool ok = glRenderRegionOrthoView(v, rgb, depth);
        for (int k = 0; k < 3; ++k)
            if (off[k]) LLPipeline::toggleRenderTypeControl(types[k]);
        return ok;
    }

    void paintOcolWater(std::vector<U8>& rgb, const std::vector<F32>& depth, S32 w, F32 ppm) const
    {
        constexpr F32 SATURATION = 0.4f, SHORE_GLOW = 0.05f;
        static const F32 tint[4] = { 0.30f, 0.46f, 0.48f, 0.35f };
        static const F32 shallow[3] = { 0.36f, 0.72f, 0.74f }, deep[3] = { 0.05f, 0.17f, 0.33f };
        const F32 span = ceiling - floor;
        auto Z = [&](S32 x, S32 y)
        {
            x = llclamp(x, 0, w - 1); y = llclamp(y, 0, w - 1);
            return floor + depth[(size_t)y * w + x] * span;
        };
        LLVector3 sun = LLEnvironment::instance().getSunDirection();
        if (sun.normalize() < 0.5f) sun.set(0.f, 0.f, 1.f);
        const S32 s = llmax(1, ll_round(0.5f * ppm));
        const F32 run = 2.f * s / ppm;
        for (S32 y = 0; y < w; ++y)
            for (S32 x = 0; x < w; ++x)
            {
                const F32 d = water - Z(x, y);
                if (d <= 0.f) continue;
                const F32 dzdx = (Z(x + s, y) - Z(x - s, y)) / run, dzdy = (Z(x, y + s) - Z(x, y - s)) / run;
                const F32 lam = llmax(0.f, (-dzdx * sun.mV[VX] - dzdy * sun.mV[VY] + sun.mV[VZ])
                                           / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.f));
                const F32 relief = 0.85f + 0.15f * lam;
                const F32 td = powf(llclamp(d / 14.f, 0.f, 1.f), 0.6f);
                const F32 tp = powf(llclamp(llmax(d, 4.f) / 14.f, 0.f, 1.f), 0.6f);
                const F32 foam = llclamp(1.f - d / 0.6f, 0.f, 1.f) * 0.35f;
                F32 band = llclamp(1.f - d / 4.f, 0.f, 1.f);
                band = band * band * (3.f - 2.f * band) * tint[3];
                F32 c[3];
                for (S32 k = 0; k < 3; ++k)
                {
                    F32 f = (shallow[k] + (deep[k] - shallow[k]) * td) * relief;
                    f = f * (1.f - foam) + foam;
                    const F32 p = (shallow[k] + (deep[k] - shallow[k]) * tp) * relief;
                    c[k] = p + (f - p) * SHORE_GLOW;
                    c[k] += (tint[k] - c[k]) * band;
                }
                const F32 l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                const F32 see = 0.5f * llclamp(1.f - d / 3.f, 0.f, 1.f);
                U8* px = rgb.data() + ((size_t)y * w + x) * 3;
                for (S32 k = 0; k < 3; ++k)
                {
                    const F32 wc = l + (c[k] - l) * SATURATION;
                    px[k] = (U8)llclamp(ll_round((wc + (px[k] / 255.f - wc) * see) * 255.f), 0, 255);
                }
            }
    }

    LLRegionOrthoView view(const LLViewerRegion* region, F32 x0, F32 y0, F32 x1, F32 y1, S32 w, S32 h) const
    {
        const LLVector3 o = region->getOriginAgent();
        LLRegionOrthoView v;
        v.eye.set(o.mV[VX] + 0.5f * width, o.mV[VY] + 0.5f * width, ceiling + eyeAbove(width));
        v.left = o.mV[VX] + x0 - v.eye.mV[VX];
        v.right = o.mV[VX] + x1 - v.eye.mV[VX];
        v.bottom = o.mV[VY] + y0 - v.eye.mV[VY];
        v.top = o.mV[VY] + y1 - v.eye.mV[VY];
        v.ceiling = ceiling;
        v.floor = floor;
        v.demand_ppm = (F32)kMapSize / width;
        v.shadows = shadows;
        v.width = w;
        v.height = h;
        return v;
    }

    void prepareTick(LLViewerRegion* region)
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        setLodBox(region);
        if (now >= next_probe)
        {
            next_probe = now + kProbeEvery;
            S32 pw = kProbeSize, ph = kProbeSize;
            if (gViewerWindow)
            {
                pw = llclamp(gViewerWindow->getWorldViewWidthRaw(), 64, 4096);
                ph = llclamp(gViewerWindow->getWorldViewHeightRaw(), 64, 4096);
            }
            const F32 bw = pw > ph ? width * pw / ph : width, bh = ph > pw ? width * ph / pw : width;
            LLRegionOrthoView v = view(region, 0.5f * (width - bw), 0.5f * (width - bh), 0.5f * (width + bw),
                                       0.5f * (width + bh), pw, ph);
            v.time = (F32)LLFrameTimer::getElapsedSeconds();
            std::vector<U8> rgb;
            if (renderWithoutAvatars(v, rgb)) ++probes;
        }
        const S32 n = gObjectList.getNumObjects();
        if (count < 0 || abs(n - count) > llmax(4, count / 200)) { count = n; count_at = now; }
        const F32 far_target = llmin(far_reach, gAgentCamera.mDrawDistance);
        if (gAgentCamera.mDrawDistance + 1.f < far_needed && !far_capped_told)
        {
            far_capped_told = true;
            LL_WARNS("RegionMap") << "draw distance held at " << gAgentCamera.mDrawDistance << " m (needed "
                                  << far_needed << " m)" << LL_ENDL;
            message(llformat("Draw distance is capped at %.0f m, so the parts of %s farther "
                             "than that from you may be missing from the map.", gAgentCamera.mDrawDistance,
                             region_name.c_str()));
        }
        const bool far_ok = gAgentCamera.mDrawDistance + 1.f >= far_target;
        const bool objects_ok = now - count_at >= kSettleHold;
        const S32 meshes = LLMeshRepoThread::sActiveHeaderRequests + LLMeshRepoThread::sActiveLODRequests
                         + (S32)LLMeshRepository::sLODPending;
        if (now >= next_tex_scan) { next_tex_scan = now + 1.0; textures = texturesUnsettled(); }
        const bool ok = far_ok && objects_ok && meshes <= kMeshPendingOk && textures <= kTexPendingOk
                     && probes >= 3 && now - start >= kMinPrepare;
        if (!ok) settled_since = -1.0;
        else if (settled_since < 0.0) settled_since = now;
        const F32 s_timeout = kSettleTimeout;
        timed_out = now - start >= llmax((F64)(F32)s_timeout, kMinPrepare + kSettleHold);
        if (now >= next_log || timed_out || (settled_since >= 0.0 && now - settled_since >= kSettleHold))
        {
            next_log = now + 10.0;
            LL_INFOS("RegionMap") << "preparing " << region_name << " " << (S32)(now - start) << " s: far "
                                  << gAgentCamera.mDrawDistance << "/" << far_target << " m, objects " << n
                                  << (objects_ok ? " (steady)" : "")
                                  << ", mesh requests " << meshes << ", world textures loading " << textures
                                  << ", preparation renders " << probes << LL_ENDL;
        }
        if (timed_out || (settled_since >= 0.0 && now - settled_since >= kSettleHold)) startRender();
    }

    void startRender()
    {
        if (!LLEnvironment::instance().getIsSunUp()) { stop("the sun went down"); return; }
        S32 n = 4;
        image = new LLImageRaw(kMapSize, kMapSize, 3);
        if (image->isBufferInvalid()) { stop("not enough free memory for the map"); return; }
        tiles = n;
        tile_px = kMapSize / n;
        aa = 0;
        next = 0;
        time = (F32)LLFrameTimer::getElapsedSeconds();
        stage = Stage::Render;
        LL_INFOS("RegionMap") << "rendering " << region_name << ": " << n << " x " << n << " tiles of "
                              << tile_px + 2 * kGuardPx << " px"
                              << (timed_out ? " (loading timed out)" : "") << LL_ENDL;
        showProgress(llformat("rendering %d x %d tiles%s.", n, n,
                              timed_out ? " (the region had not finished loading in time)" : ""));
    }

    void renderTick(LLViewerRegion* region)
    {
        const S32 i = next % tiles, j = next / tiles;
        const F32 ppm = (F32)kMapSize / width;
        const S32 px0 = i * tile_px - kGuardPx, py0 = j * tile_px - kGuardPx;
        const S32 w = tile_px + 2 * kGuardPx;
        LLRegionOrthoView v = view(region, px0 / ppm, py0 / ppm, (px0 + w) / ppm, (py0 + w) / ppm, w, w);
        v.time = time;
        v.frames = 2;
        v.aa_frames = aa;
        std::vector<U8> rgb;
        std::vector<F32> depth;
        if (!renderWithoutAvatars(v, rgb, ocol_water ? &depth : nullptr) || rgb.size() < (size_t)w * w * 3)
        {
            stop("a map tile could not be rendered");
            return;
        }
        if (ocol_water && depth.size() >= (size_t)w * w)
        {
            paintOcolWater(rgb, depth, w, ppm);
        }
        U8* out = image->getData();
        for (S32 r = 0; r < tile_px; ++r)
        {
            const U8* src = rgb.data() + ((size_t)(r + kGuardPx) * w + kGuardPx) * 3;
            U8* dst = out + ((size_t)(j * tile_px + r) * kMapSize + (size_t)i * tile_px) * 3;
            std::memcpy(dst, src, (size_t)tile_px * 3);
        }
        if (++next < tiles * tiles) return;
        save();
    }

    void save()
    {
        restoreAll();
        showProgress("saving the map.");
        stage = Stage::Save;
        const std::string dir = mapDir();
        {
            const std::string& delim = gDirUtilp->getDirDelimiter();
            LLFile::mkdir(gDirUtilp->getOSUserAppDir() + delim + "region_maps");
            LLFile::mkdir(dir);
        }
        const std::string j2c_path = mapPath(handle, "j2c"), json_path = mapPath(handle, "json");
        const std::string old_png = mapPath(handle, "png");
        const bool lossless = gSavedSettings.getBOOL("SLRegionMapLossless");
        LLSD meta;
        meta["region_name"] = region_name;
        meta["region_id"] = region_id;
        meta["region_width"] = width;
        meta["image_size"] = kMapSize;
        meta["captured_utc"] = LLDate::now().asString();
        meta["ceiling"] = ceiling;
        meta["floor"] = floor;
        meta["ocol_water"] = ocol_water;
        meta["shadows"] = shadows;
        meta["grid"] = grid_id();
        meta["viewer"] = LLVersionInfo::instance().getChannelAndVersion();
        meta["fully_loaded"] = !timed_out;
        const std::string json = boost::json::serialize(LlsdToJson(meta));
        LLPointer<LLImageRaw> raw = image;
        image = nullptr;
        const U64 h = handle;
        const F32 w = width;
        const std::string name = region_name;
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
        auto work = [raw, j2c_path, json_path, json, old_png, lossless]() -> std::string
        {
            try
            {
                LLPointer<LLImageJ2C> j2c = new LLImageJ2C();
                j2c->setReversible(lossless);
                if (!lossless) j2c->setMaxBytes((S32)(raw->getDataSize() / 3.5f));
                if (!j2c->encode(raw, 0.f)) return "the map image could not be encoded";
                const std::string j2c_tmp = j2c_path + ".tmp";
                {
                    llofstream f(j2c_tmp, std::ios::out | std::ios::binary | std::ios::trunc);
                    if (!f.is_open()) return "cannot write " + j2c_tmp;
                    f.write((const char*)j2c->getData(), j2c->getDataSize());
                    f.close();
                    if (f.fail()) { LLFile::remove(j2c_tmp, ENOENT); return "cannot write " + j2c_tmp; }
                }
                LLFile::remove(j2c_path, ENOENT);
                if (LLFile::rename(j2c_tmp, j2c_path) != 0) { LLFile::remove(j2c_tmp, ENOENT); return "cannot replace " + j2c_path; }
                const std::string json_tmp = json_path + ".tmp";
                {
                    llofstream j(json_tmp, std::ios::out | std::ios::trunc);
                    if (j.is_open()) j << json << "\n";
                }
                LLFile::remove(json_path, ENOENT);
                if (LLFile::rename(json_tmp, json_path) != 0) LLFile::remove(json_tmp, ENOENT);
                LLFile::remove(old_png, ENOENT);
                return std::string();
            }
            catch (const std::exception& e) { return e.what(); }
            catch (...) { return "unknown error"; }
        };
        auto done = [raw, h, w, name, j2c_path](std::string err)
        {
            GMDXRegionCaptureState& s = get();
            s.stage = Stage::Idle;
            if (s.progress) { LLNotificationsUtil::cancel(s.progress); s.progress.reset(); }
            LLSD args;
            args["REGION"] = name;
            if (!err.empty())
            {
                LL_WARNS("RegionMap") << "map of " << name << " not saved: " << err << LL_ENDL;
                args["REASON"] = err;
                LLNotificationsUtil::add("RegionMapStopped", args);
                return;
            }
            LL_INFOS("RegionMap") << "map of " << name << " saved: " << j2c_path << LL_ENDL;
            if (gSavedSettings.getBOOL("SLRegionMapUseCaptured")) s.maps[h] = MapTex{ minimapTexture(raw) };
            else s.maps.erase(h);
            args["PATH"] = j2c_path;
            LLNotificationsUtil::add("RegionMapSaved", args);
        };
        bool posted = false;
        if (main_queue && general)
        {
            posted = main_queue->postTo(general, work, [done](std::string err)
                {
                    if (!LLApp::isExiting()) done(err);
                });
        }
        if (!posted) done(work());
    }

    // ---- paths ----
    static std::string mapDir()
    {
        const std::string& delim = gDirUtilp->getDirDelimiter();
        std::string grid = LLDir::getScrubbedFileName(grid_id());
        if (grid.empty()) grid = "grid";
        return gDirUtilp->getOSUserAppDir() + delim + "region_maps" + delim + grid;
    }
    static std::string mapPath(U64 region_handle, const char* ext)
    {
        U32 x = 0, y = 0;
        from_region_handle(region_handle, &x, &y);
        return mapDir() + gDirUtilp->getDirDelimiter() + llformat("%u_%u.%s", x, y, ext);
    }
    // The saved map of a region: the JPEG 2000 file, or a PNG; "" = none.
    static std::string mapFile(U64 region_handle)
    {
        const std::string j2c = mapPath(region_handle, "j2c");
        if (LLFile::isfile(j2c)) return j2c;
        const std::string png = mapPath(region_handle, "png");
        return LLFile::isfile(png) ? png : std::string();
    }
    static void makeMapDir()
    {
        LLFile::mkdir(gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "region_maps");
        LLFile::mkdir(mapDir());
    }

    static void message(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("GMDXRegionMapMessage", args);
    }

    // ---- maps on disk ----
    static bool readFile(const std::string& path, std::string& bytes)
    {
        llifstream f(path, std::ios::in | std::ios::binary);
        if (!f.is_open()) return false;
        bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return !f.bad();
    }
    static LLPointer<LLImageRaw> loadMap(const std::string& path)   // (a JPEG 2000 or a PNG map)
    {
        std::string bytes;
        if (path.empty() || !readFile(path, bytes)) return nullptr;
        return decodeMap(bytes, path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0);
    }
    static LLPointer<LLImageRaw> decodeMap(const std::string& bytes, bool png_file)
    {
        if (bytes.empty()) return nullptr;
        LLPointer<LLImageFormatted> image = png_file ? (LLImageFormatted*)new LLImagePNG() : (LLImageFormatted*)new LLImageJ2C();
        U8* data = image->allocateData((S32)bytes.size());
        if (!data) return nullptr;
        std::memcpy(data, bytes.data(), bytes.size());
        if (!image->updateData()) return nullptr;
        // (a shared map comes from someone else: its header's size is checked before the decode allocates)
        if (image->getWidth() < kShareMinPx || image->getWidth() > kShareMaxPx || image->getHeight() != image->getWidth())
            return nullptr;
        image->setDiscardLevel(0);   // (the whole image)
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        if (!image->decode(raw, 0.f) || raw->getComponents() < 3) return nullptr;
        return raw;
    }
    // (on a worker where it can: a 4096 px map is scaled down once, not on every region crossing's frame)
    static LLPointer<LLImageRaw> forMinimap(LLPointer<LLImageRaw> raw)
    {
        if (raw.notNull() && raw->getWidth() > kMinimapPx) raw->scale(kMinimapPx, kMinimapPx);
        return raw;
    }
    static LLPointer<LLViewerTexture> minimapTexture(LLPointer<LLImageRaw> raw)
    {
        if (raw.isNull()) return nullptr;
        // ponytail: kMinimapPx caps each loaded map at 2048 px (about 16 MB with mips) instead of
        // GunMetal-DX's 4096; zooming far into a map is a little softer
        // (the mini-map draws each region at its own width, var-regions included, so the whole map is used)
        // mipmapped: the mini-map shows the 4096 px map a few hundred pixels across; without mips it shimmers
        LLPointer<LLViewerTexture> tex = LLViewerTextureManager::getLocalTexture(raw.get(), true);
        tex->dontDiscard();
        tex->setAddressMode(LLTexUnit::TAM_CLAMP);
        return tex;
    }
    void loadMinimap(const LLViewerRegion* region, MapTex& e)
    {
        const std::string path = mapFile(region->getHandle());
        if (path.empty()) return;   // (an entry without a texture: nothing to show)
        e.loading = true;
        const U64 h = region->getHandle();
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
        const bool posted = main_queue && general && main_queue->postTo(general,
            [path]() -> LLPointer<LLImageRaw>
            {
                try { return forMinimap(loadMap(path)); }
                catch (...) { return nullptr; }
            },
            [h](LLPointer<LLImageRaw> raw)
            {
                if (LLApp::isExiting()) return;
                auto it = get().maps.find(h);
                if (it == get().maps.end() || !it->second.loading) return;   // pruned / replaced meanwhile
                it->second.loading = false;
                it->second.tex = minimapTexture(raw);
            });
        if (!posted) { e.loading = false; e.tex = minimapTexture(forMinimap(loadMap(path))); }
    }
    // Maps of regions no longer around free their textures (every few seconds, from the mini-map).
    void pruneMaps()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < next_prune || maps.empty()) return;
        next_prune = now + 5.0;
        for (auto it = maps.begin(); it != maps.end(); )
            it = LLWorld::getInstance()->getRegionFromHandle(it->first) ? std::next(it) : maps.erase(it);
    }
};

bool glRenderRegionOrthoView(const LLRegionOrthoView& view, std::vector<U8>& rgb, std::vector<F32>* depth)
{
    if (view.width <= 0 || view.height <= 0 || !(view.right > view.left) || !(view.top > view.bottom) || !(view.ceiling > view.floor))
        return false;

    // Save camera state
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 saved_origin = cam->getOrigin();
    const LLVector3 saved_x = cam->getXAxis();
    const LLVector3 saved_y = cam->getYAxis();
    const LLVector3 saved_z = cam->getZAxis();
    const bool saved_ortho = cam->getOrtho();
    const F32 saved_focus = cam->getOrthoFocusDist();
    const F32 saved_near = cam->getNear();
    const F32 saved_far = cam->getFar();
    const F32 saved_aspect = cam->getAspect();
    const F32 saved_fov = cam->getView();
    const LLRect saved_rect = gViewerWindow->getWorldViewRectRaw();
    const S32 orig_w = LLPipeline::sRenderDeferred ? gPipeline.mRT->deferredScreen.getWidth() : gViewerWindow->getWorldViewWidthRaw();
    const S32 orig_h = LLPipeline::sRenderDeferred ? gPipeline.mRT->deferredScreen.getHeight() : gViewerWindow->getWorldViewHeightRaw();

    const bool prev_draw_ui = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    if (prev_draw_ui)
    {
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    }
    const bool prev_hide_hud = LLPipeline::sShowHUDAttachments;
    LLPipeline::sShowHUDAttachments = false;
    const S32 prev_shadows = LLPipeline::RenderShadowDetail;
    if (!view.shadows)
    {
        LLPipeline::RenderShadowDetail = 0;
    }

    // Setup camera
    const F32 tile_w = view.right - view.left;
    const F32 tile_h = view.top - view.bottom;
    const F32 tile_cx = view.eye.mV[VX] + 0.5f * (view.left + view.right);
    const F32 tile_cy = view.eye.mV[VY] + 0.5f * (view.bottom + view.top);
    const F32 eye_z = view.eye.mV[VZ];
    const F32 zn = llmax(eye_z - view.ceiling, 0.01f);
    const F32 zf = eye_z - view.floor;

    cam->setOrigin(LLVector3(tile_cx, tile_cy, eye_z));
    cam->lookDir(LLVector3(0.f, 0.f, -1.f), LLVector3(0.f, 1.f, 0.f));
    cam->setNear(zn);
    cam->setFar(zf);
    const F32 half_height = 0.5f * tile_h;
    cam->setOrthoMode(true, half_height / tanf(cam->getView() * 0.5f));
    cam->setAspect((F32)view.width / (F32)view.height);
    cam->calcProjection(zf);

    LLRenderTarget scratch;
    if (!scratch.allocate(view.width, view.height, GL_RGBA, true))
    {
        LLPipeline::RenderShadowDetail = prev_shadows;
        if (prev_draw_ui && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        if (prev_hide_hud) LLPipeline::sShowHUDAttachments = true;
        cam->setOrigin(saved_origin);
        cam->setAxes(saved_x, saved_y, saved_z);
        cam->setNear(saved_near);
        cam->setFar(saved_far);
        cam->setOrthoMode(saved_ortho, saved_focus);
        cam->setAspect(saved_aspect);
        cam->setViewNoBroadcast(saved_fov);
        cam->calcProjection(saved_far);
        return false;
    }

    if (!gPipeline.allocateScreenBuffer(view.width, view.height))
    {
        LLPipeline::RenderShadowDetail = prev_shadows;
        scratch.release();
        gPipeline.allocateScreenBuffer(orig_w, orig_h);
        if (prev_draw_ui && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        if (prev_hide_hud) LLPipeline::sShowHUDAttachments = true;
        cam->setOrigin(saved_origin);
        cam->setAxes(saved_x, saved_y, saved_z);
        cam->setNear(saved_near);
        cam->setFar(saved_far);
        cam->setOrthoMode(saved_ortho, saved_focus);
        cam->setAspect(saved_aspect);
        cam->setViewNoBroadcast(saved_fov);
        cam->calcProjection(saved_far);
        return false;
    }

    gViewerWindow->setWorldViewRectRaw(LLRect(0, view.height, view.width, 0));
    scratch.bindTarget();
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    const U32 passes = llmax(view.frames, 1u);
    for (U32 i = 0; i < passes; ++i)
    {
        gDisplaySwapBuffers = false;
        const bool do_rebuild = true;
        const F32 zoom = 1.0f;
        const U32 subfield = 0;
        const bool for_snapshot = true;
        display(do_rebuild, zoom, subfield, for_snapshot);
    }

    rgb.resize((size_t)view.width * view.height * 3);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, view.width, view.height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

    if (depth)
    {
        depth->resize((size_t)view.width * view.height);
        std::vector<F32> raw_depth((size_t)view.width * view.height);

        GLint prev_read_fbo = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);

        GLuint depth_fbo = 0;
        if (LLPipeline::sRenderDeferred && gPipeline.mRT && gPipeline.mRT->deferredScreen.isComplete())
        {
            depth_fbo = gPipeline.mRT->deferredScreen.getFBO();
        }
        else if (gPipeline.mRT && gPipeline.mRT->screen.isComplete())
        {
            depth_fbo = gPipeline.mRT->screen.getFBO();
        }

        if (depth_fbo != 0)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
        }

        glReadPixels(0, 0, view.width, view.height, GL_DEPTH_COMPONENT, GL_FLOAT, raw_depth.data());

        if (depth_fbo != 0)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
        }

        for (size_t idx = 0; idx < raw_depth.size(); ++idx)
        {
            const F32 z_win = raw_depth[idx];
            (*depth)[idx] = 1.0f - z_win;
        }
    }

    gDisplaySwapBuffers = false;
    gDepthDirty = true;
    scratch.flush();
    scratch.release();
    gViewerWindow->setWorldViewRectRaw(saved_rect);
    gPipeline.allocateScreenBuffer(orig_w, orig_h);
    gPipeline.resetDrawOrders();

    if (prev_draw_ui && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    }
    if (prev_hide_hud)
    {
        LLPipeline::sShowHUDAttachments = true;
    }

    cam->setOrigin(saved_origin);
    cam->setAxes(saved_x, saved_y, saved_z);
    cam->setNear(saved_near);
    cam->setFar(saved_far);
    cam->setOrthoMode(saved_ortho, saved_focus);
    cam->setAspect(saved_aspect);
    cam->setViewNoBroadcast(saved_fov);
    cam->calcProjection(saved_far);

    LLPipeline::RenderShadowDetail = prev_shadows;

    return true;
}

namespace
{
class LLFloaterRegionMapCapture : public LLFloater
{
public:
    LLFloaterRegionMapCapture(const LLSD& key) : LLFloater(key) {}
    bool postBuild() override
    {
        mReach = getChild<LLTextBox>("reach_text");
        getChild<LLUICtrl>("capture_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
            {
                closeFloater();
                GMDXRegionCapture::requestCapture();
            });
        getChild<LLUICtrl>("cancel_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
        return true;
    }
    void draw() override
    {
        if (LLViewerRegion* region = gAgent.getRegion())
        {
            bool hills = false;
            const F32 ceiling = GMDXRegionCapture::ceilingFor(region, &hills);
            LLStringUtil::format_map_t args;
            args["[CEILING]"] = llformat("%.0f", ceiling);
            args["[ALT]"] = llformat("%.0f", gAgent.getPositionAgent().mV[VZ]);
            mReach->setText(getString(hills ? "reach_hills" : "reach", args));
        }
        getChildView("capture_btn")->setEnabled(GMDXRegionCapture::canCapture());
        LLFloater::draw();
    }
private:
    LLTextBox* mReach = nullptr;
};
}

F32 GMDXRegionCapture::ceilingFor(const LLViewerRegion* region, bool* hills)
{
    static LLCachedControl<F32> s_ceiling(gSavedSettings, "SLRegionMapCeilingOffset", 50.f);
    const F32 mine = gAgent.getPositionAgent().mV[VZ] + llmax((F32)s_ceiling, 0.f);
    const F32 land = region ? region->getLand().getMaxZ() + 1.f : mine;
    if (hills) *hills = land > mine;
    return llmax(mine, land);
}

LLFloater* GMDXRegionCapture::buildWindow(const LLSD& key)
{
    return new LLFloaterRegionMapCapture(key);
}

void GMDXRegionCapture::openWindow()
{
    LLFloaterReg::showInstance("region_map_capture");
}

void GMDXRegionCapture::requestCapture()
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (s.stage != Stage::Idle)
    {
        GMDXRegionCaptureState::message("A region map capture is already running.");
        return;
    }
    const std::string why = GMDXRegionCaptureState::blocked();
    if (!why.empty()) { GMDXRegionCaptureState::message(why); return; }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    LLSD args;
    args["REGION"] = region->getName();
    if (!LLEnvironment::instance().getIsSunUp())
    {
        LLNotificationsUtil::add("RegionMapNight", args);
        return;
    }
    const F32 needed = GMDXRegionCaptureState::farNeeded(region);
    const F32 cap = GMDXRegionCaptureState::netFarCap();
    args["DISTANCE"] = llformat("%.0f", llmax(needed, gSavedSettings.getF32("RenderFarClip")));
    args["CEILING"] = llformat("%.0f", GMDXRegionCapture::ceilingFor(region));
    args["PARTIAL"] = needed > cap
        ? llformat("\n\nThe simulator sends objects only up to %.0f m away, so the parts of the region farther than that "
                   "from you may be missing. Stand nearer the middle of the region for a complete map.", cap)
        : std::string();
    s.stage = Stage::Confirm;
    s.handle = region->getHandle();
    LLNotificationsUtil::add("RegionMapConfirm", args, LLSD(),
        [](const LLSD& n, const LLSD& r)
        {
            GMDXRegionCaptureState& st = GMDXRegionCaptureState::get();
            if (st.stage != Stage::Confirm) return;
            st.stage = Stage::Idle;
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) st.begin();
        });
}

bool GMDXRegionCapture::canCapture()
{
    return GMDXRegionCaptureState::get().stage == Stage::Idle && GMDXRegionCaptureState::blocked().empty();
}

bool GMDXRegionCapture::isRunning()
{
    const Stage st = GMDXRegionCaptureState::get().stage;
    return st == Stage::Prepare || st == Stage::Render;
}

void GMDXRegionCapture::cancelCapture()
{
    if (isRunning()) GMDXRegionCaptureState::get().stop("cancelled");
}

void GMDXRegionCapture::requestStop(const std::string& why)
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (isRunning() && s.stop_request.empty()) s.stop_request = why;
}

void GMDXRegionCapture::frameTick()
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    s.pruneMaps();
    if (s.stage != Stage::Prepare && s.stage != Stage::Render) return;
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(s.handle);
    std::string why;
    bool prefs = false;
    if (!s.stop_request.empty()) why = s.stop_request;
    else if (LLApp::isExiting() || gDisconnected || LLStartUp::getStartupState() < STATE_STARTED
        || LLAppViewer::instance()->logoutRequestSent())
        why = "you logged out";
    else if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) why = "you teleported";
    else if (!region || gAgent.getRegion() != region) why = "you left the region";
    else if (openGraphicsWindow())
    {
        prefs = true;
        why = "Preferences or a graphics settings window was opened (it would have saved the capture's temporary "
              "settings as yours)";
    }
    else why = GMDXRegionCaptureState::blocked();
    if (!why.empty())
    {
        s.stop(why);
        if (prefs)
            if (LLFloaterPreference* p = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
                p->saveSettings();
        return;
    }
    if (s.stage == Stage::Prepare) s.prepareTick(region);
    else s.renderTick(region);
}

void GMDXRegionCapture::shutdown()
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    s.restoreAll();
    s.image = nullptr;
    s.progress.reset();
    s.maps.clear();
    s.stage = Stage::Idle;
    forEachWorldMapTexture(nullptr, nullptr);   // (the World Map's textures)
}

bool GMDXRegionCapture::hasCapture(const LLViewerRegion* region)
{
    return region && !GMDXRegionCaptureState::mapFile(region->getHandle()).empty();
}

void GMDXRegionCapture::deleteCapture(const LLViewerRegion* region)
{
    if (!region || isRunning()) return;
    const U64 handle = region->getHandle();
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, "j2c"), ENOENT);
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, "png"), ENOENT);
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, "json"), ENOENT);
    GMDXRegionCaptureState::get().maps.erase(handle);   // the mini-map goes back to its background
    LL_INFOS("GMDXRegionMap") << "deleted the map of " << region->getName() << LL_ENDL;
}

void GMDXRegionCapture::openMapsFolder()
{
    GMDXRegionCaptureState::makeMapDir();
    // (as Preferences opens the cache folder)
    if (gViewerWindow && gViewerWindow->getWindow())
    {
        gViewerWindow->getWindow()->openFile(GMDXRegionCaptureState::mapDir());
    }
}

// ---- sharing a map: one file, exported by one user and imported by another ----

namespace
{
std::string share_name(const LLViewerRegion* region)
{
    // [RLVa] no region name in the proposed file name while the location is hidden
    return LLDir::getScrubbedFileName(RlvActions::canShowLocation() ? region->getName() : std::string("region map"));
}

// Written next to it, then moved over the old file in one step.
bool write_replace(const std::string& path, const char* data, size_t size)
{
    const std::string tmp = path + ".tmp";
    {
        llofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write(data, size);
        f.close();
        if (f.fail()) { LLFile::remove(tmp, ENOENT); return false; }
    }
    if (LLFile::rename(tmp, path) != 0) { LLFile::remove(tmp, ENOENT); return false; }
    return true;
}

// A shared map file, read and decoded on a worker thread.
struct SharedMap
{
    std::string error;          // "" = readable
    LLSD meta;
    std::string payload;        // the image file's bytes
    LLPointer<LLImageRaw> raw;  // decoded (the check that it is an image, and the mini-map's texture)
};

SharedMap read_shared_map(const std::string& path)
{
    SharedMap m;
    llstat st;
    const S64 size = LLFile::stat(path, &st) == 0 ? (S64)st.st_size : 0;
    if (size <= 0 || size > kShareMaxBytes) { m.error = "it is not a region map file"; return m; }
    std::string bytes;
    if (!GMDXRegionCaptureState::readFile(path, bytes)) { m.error = "it could not be read"; return m; }
    const size_t eol = bytes.size() > kShareMagic.size() ? bytes.find('\n', kShareMagic.size()) : std::string::npos;
    if (bytes.compare(0, kShareMagic.size(), kShareMagic) != 0 || eol == std::string::npos)
    {
        m.error = "it is not a region map file";
        return m;
    }
    try { m.meta = LlsdFromJson(boost::json::parse(bytes.substr(kShareMagic.size(), eol - kShareMagic.size()))); }
    catch (...) { m.error = "its description is damaged"; return m; }
    const std::string format = m.meta["payload"].asString();
    if (!m.meta.isMap() || (format != "j2c" && format != "png")
        || m.meta["region_grid_x"].asInteger() <= 0 || m.meta["region_grid_x"].asInteger() > kGridMax
        || m.meta["region_grid_y"].asInteger() <= 0 || m.meta["region_grid_y"].asInteger() > kGridMax
        || (size_t)m.meta["payload_size"].asInteger() != bytes.size() - eol - 1)
    {
        m.error = "its description is damaged";
        return m;
    }
    m.payload = bytes.substr(eol + 1);
    m.raw = GMDXRegionCaptureState::decodeMap(m.payload, format == "png");
    if (m.raw.isNull()) m.error = "its image is damaged";
    return m;
}

void install_shared_map(const SharedMap& m, U64 handle)
{
    const std::string format = m.meta["payload"].asString();
    const std::string region = m.meta["region_name"].asString();
    GMDXRegionCaptureState::makeMapDir();
    const std::string path = GMDXRegionCaptureState::mapPath(handle, format.c_str());
    if (!write_replace(path, m.payload.data(), m.payload.size()))
    {
        GMDXRegionCaptureState::message("Could not save the imported map to " + path);
        return;
    }
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, format == "j2c" ? "png" : "j2c"), ENOENT);
    LLSD meta = m.meta;
    meta.erase("payload");
    meta.erase("payload_size");
    meta["imported_utc"] = LLDate::now().asString();
    const std::string json = boost::json::serialize(LlsdToJson(meta)) + "\n";
    write_replace(GMDXRegionCaptureState::mapPath(handle, "json"), json.data(), json.size());
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (gSavedSettings.getBOOL("GMDXMinimapUseCapturedMaps"))
        s.maps[handle] = MapTex{ GMDXRegionCaptureState::minimapTexture(GMDXRegionCaptureState::forMinimap(m.raw)) };
    else
        s.maps.erase(handle);
    LL_INFOS("GMDXRegionMap") << "imported the map of " << region << ": " << path << LL_ENDL;
    LLSD args;
    args["REGION"] = region;
    LLNotificationsUtil::add("GMDXRegionMapImported", args);
}

void import_checked(const std::string& file, SharedMap m)
{
    auto fail = [&file](const std::string& why)
    {
        LL_WARNS("GMDXRegionMap") << "not imported: " << file << ": " << why << LL_ENDL;
        GMDXRegionCaptureState::message("The region map was not imported: " + why + ".");
    };
    if (!m.error.empty()) { fail(m.error); return; }
    const std::string grid = grid_id();
    if (m.meta["grid"].asString() != grid)
    {
        fail("it is a map of a region on another grid (" + m.meta["grid"].asString() + ")");
        return;
    }
    const U64 handle = to_region_handle((U32)m.meta["region_grid_x"].asInteger() * 256u,
                                        (U32)m.meta["region_grid_y"].asInteger() * 256u);
    // A region this viewer knows at those coordinates must be the one that was mapped.
    if (const LLViewerRegion* known = LLWorld::getInstance()->getRegionFromHandle(handle))
    {
        if (m.meta["region_id"].asUUID() != known->getRegionID())
        {
            // [RLVa] (the region known here may be the one the agent is in)
            fail("it is a map of " + m.meta["region_name"].asString() + ", which is no longer at the coordinates of "
                 + (RlvActions::canShowLocation() ? known->getName() : std::string("that region")));
            return;
        }
        m.meta["region_width"] = known->getWidth();
    }
    // The width the map covers is the region's, as far as this viewer knows it (the World Map's
    // region info, else a standard 256 m region), not what the file claims.
    else if (const LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromHandle(handle))
    {
        m.meta["region_width"] = (F32)llmax((S32)info->getSizeX(), 256);
    }
    else
    {
        m.meta["region_width"] = 256.f;
    }
    if (GMDXRegionCaptureState::mapFile(handle).empty()) { install_shared_map(m, handle); return; }
    LLSD args;
    args["REGION"] = m.meta["region_name"].asString();
    LLNotificationsUtil::add("GMDXRegionMapReplace", args, LLSD(),
        [m, handle](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) install_shared_map(m, handle);
        });
}
} // namespace

void GMDXRegionCapture::exportMap(const LLViewerRegion* region)
{
    const std::string path = region ? GMDXRegionCaptureState::mapFile(region->getHandle()) : std::string();
    std::string payload;
    if (path.empty() || !GMDXRegionCaptureState::readFile(path, payload) || payload.empty())
    {
        GMDXRegionCaptureState::message("This region has no region map to export.");
        return;
    }
    LLSD meta;
    {
        std::string json;
        if (GMDXRegionCaptureState::readFile(GMDXRegionCaptureState::mapPath(region->getHandle(), "json"), json))
        {
            try { meta = LlsdFromJson(boost::json::parse(json)); }
            catch (...) { meta = LLSD(); }
        }
        if (!meta.isMap()) meta = LLSD::emptyMap();
    }
    U32 x = 0, y = 0;
    from_region_handle(region->getHandle(), &x, &y);
    meta["grid"] = grid_id();
    meta["region_id"] = region->getRegionID();
    meta["region_name"] = region->getName();
    meta["region_width"] = region->getWidth();
    meta["region_grid_x"] = (S32)(x / 256u);
    meta["region_grid_y"] = (S32)(y / 256u);
    meta["payload"] = path.compare(path.size() - 4, 4, ".png") == 0 ? "png" : "j2c";
    meta["payload_size"] = (S32)payload.size();
    const std::string data = kShareMagic + boost::json::serialize(LlsdToJson(meta)) + "\n" + payload;
    LLFilePickerReplyThread::startPicker(
        [data](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLAppViewer::instance()->quitRequested() || filenames.empty()) return;
            if (!write_replace(filenames[0], data.data(), data.size()))
                GMDXRegionCaptureState::message("Could not save the map file to " + filenames[0]);
        },
        LLFilePicker::FFSAVE_ALL, share_name(region) + ".gmdxmap");
}

void GMDXRegionCapture::importMap()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLAppViewer::instance()->quitRequested() || filenames.empty()) return;
            const std::string file = filenames[0];
            LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
            LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
            const bool posted = main_queue && general && main_queue->postTo(general,
                [file]() -> SharedMap
                {
                    try { return read_shared_map(file); }
                    catch (...) { SharedMap m; m.error = "it could not be read"; return m; }
                },
                [file](SharedMap m)
                {
                    if (!LLApp::isExiting()) import_checked(file, m);
                });
            if (!posted) import_checked(file, read_shared_map(file));
        },
        LLFilePicker::FFLOAD_ALL, false);
}

// ---- a map shared in world: an uploaded texture, used from inventory ----

bool GMDXRegionCapture::isMapTexture(const std::string& description)
{
    return description.compare(0, kTextureTag.size(), kTextureTag) == 0;
}

namespace
{
// The map texture's loaded callback (gmdxminimapimage.cpp on_tile_loaded): the final call, or a
// failure, ends it.
struct TextureMapRequest { std::string name, desc; };

void on_map_texture_loaded(bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw*,
                           S32, bool final, void* userdata)
{
    if (success && !final) return;
    std::unique_ptr<TextureMapRequest> req(static_cast<TextureMapRequest*>(userdata));
    if (!success || !src || src->isBufferInvalid() || src->getComponents() < 3)
    {
        GMDXRegionCaptureState::message("The map texture " + req->name + " could not be loaded.");
        return;
    }
    SharedMap m;
    {
        LLImageDataSharedLock lock(src);
        m.raw = new LLImageRaw(static_cast<const U8*>(src->getData()), src->getWidth(), src->getHeight(), src->getComponents());
    }
    if (m.raw->getComponents() > 3)
    {
        LLPointer<LLImageRaw> rgb = new LLImageRaw(m.raw->getWidth(), m.raw->getHeight(), 3);
        rgb->copy(m.raw);
        m.raw = rgb;
    }
    if (m.raw->getWidth() != m.raw->getHeight() || m.raw->getWidth() < kShareMinPx)
    {
        GMDXRegionCaptureState::message("The texture " + req->name + " is not a region map.");
        return;
    }
    // "GMDXMAP1 <grid x> <grid y> <region width> <region id>"
    std::istringstream in(req->desc.substr(kTextureTag.size()));
    S32 gx = 0, gy = 0;
    F32 width = 0.f;
    std::string id;
    in >> gx >> gy >> width >> id;
    if (in.fail() || gx <= 0 || gy <= 0 || gx > kGridMax || gy > kGridMax || width < 16.f || !LLUUID::validate(id))
    {
        GMDXRegionCaptureState::message("The texture " + req->name + " is not a region map (its description was changed).");
        return;
    }
    std::string region = req->name;
    if (region.compare(0, kTextureNamePrefix.size(), kTextureNamePrefix) == 0) region = region.substr(kTextureNamePrefix.size());
    m.meta["grid"] = grid_id();   // (a texture is on this grid)
    m.meta["region_name"] = region;
    m.meta["region_id"] = LLUUID(id);
    m.meta["region_width"] = width;
    m.meta["region_grid_x"] = gx;
    m.meta["region_grid_y"] = gy;
    m.meta["payload"] = "j2c";
    m.meta["from_texture"] = src_vi ? src_vi->getID() : LLUUID::null;
    // saved as a map file on a worker thread: JPEG 2000, reversible (the texture is lossy already;
    // this encoder has no rate setting, and its default rate would lose more)
    const std::string label = req->name;
    auto encode = [m]() -> SharedMap
    {
        SharedMap out = m;
        LLPointer<LLImageJ2C> j2c = new LLImageJ2C();
        j2c->setReversible(true);
        if (!j2c->encode(out.raw, 0.f)) { out.error = "it could not be encoded"; return out; }
        out.payload.assign((const char*)j2c->getData(), (size_t)j2c->getDataSize());
        return out;
    };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    const bool posted = main_queue && general && main_queue->postTo(general, encode,
        [label](SharedMap done)
        {
            if (!LLApp::isExiting()) import_checked(label, done);
        });
    if (!posted) import_checked(label, encode());
}
} // namespace

void GMDXRegionCapture::useMapTexture(const LLUUID& asset_id, const std::string& name, const std::string& description)
{
    if (asset_id.isNull() || !isMapTexture(description)) return;
    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(asset_id, FTT_DEFAULT, true,
                                                                            LLGLTexture::BOOST_PREVIEW);
    if (!tex) return;
    tex->forceToSaveRawImage(0);   // the full resolution
    tex->setLoadedCallback(on_map_texture_loaded, 0, true, false, new TextureMapRequest{ name, description }, nullptr);
}

bool GMDXRegionCapture::loadCapturedImage(const LLViewerRegion* region, std::function<void(LLPointer<LLImageRaw>)> done)
{
    if (!region || !done || !hasCapture(region)) return false;
    const std::string path = GMDXRegionCaptureState::mapFile(region->getHandle());
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    const bool posted = main_queue && general && main_queue->postTo(general,
        [path]() -> LLPointer<LLImageRaw>
        {
            try { return GMDXRegionCaptureState::loadMap(path); }
            catch (...) { return nullptr; }
        },
        [done](LLPointer<LLImageRaw> raw)
        {
            if (!LLApp::isExiting()) done(raw);
        });
    if (!posted) done(GMDXRegionCaptureState::loadMap(path));   // (no worker: here)
    return true;
}

namespace
{
// The World Map's maps: 512 px textures (a region is rarely larger on the World Map), by region
// handle, the least recently drawn dropped past kWorldMax.
constexpr S32 kWorldPx = 512;
constexpr size_t kWorldMax = 96;
struct WorldTex { LLPointer<LLViewerTexture> tex; F32 width = 256.f; bool loading = false; F64 used = 0.0; };
std::map<U64, WorldTex> sWorld;
std::vector<U64> sWorldHandles;   // the regions with a map file, from the last scan
std::string sWorldDir;
F64 sWorldNextScan = 0.0;
} // namespace

void GMDXRegionCapture::forEachWorldMapTexture(const std::function<bool(U64)>& visible,
                                               const std::function<void(U64, LLViewerTexture*, F32)>& draw)
{
    static LLCachedControl<bool> use(gSavedSettings, "GMDXWorldMapUseCapturedMaps", true);
    if (!use || !visible || !draw)   // (off, or shutdown)
    {
        sWorld.clear();
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const std::string dir = GMDXRegionCaptureState::mapDir();
    if (now >= sWorldNextScan || dir != sWorldDir)   // (an import or a delete shows within seconds)
    {
        sWorldNextScan = now + 5.0;
        if (dir != sWorldDir) sWorld.clear();   // (another grid)
        sWorldDir = dir;
        sWorldHandles.clear();
        if (LLFile::isdir(dir))
        {
            for (const std::string& name : gDirUtilp->getFilesInDir(dir))
            {
                U32 x = 0, y = 0;
                char ext[8] = {};
                if (sscanf(name.c_str(), "%u_%u.%7s", &x, &y, ext) == 3 && (!strcmp(ext, "j2c") || !strcmp(ext, "png")))
                {
                    const U64 h = to_region_handle(x, y);
                    if (std::find(sWorldHandles.begin(), sWorldHandles.end(), h) == sWorldHandles.end()) sWorldHandles.push_back(h);
                }
            }
        }
        for (auto it = sWorld.begin(); it != sWorld.end(); )   // (deleted maps)
            it = std::find(sWorldHandles.begin(), sWorldHandles.end(), it->first) == sWorldHandles.end() ? sWorld.erase(it) : std::next(it);
    }
    for (const U64 h : sWorldHandles)
    {
        if (!visible(h)) continue;   // (only what is on screen is loaded and kept)
        WorldTex& w = sWorld[h];
        w.used = now;
        if (w.tex.notNull())
        {
            draw(h, w.tex.get(), w.width);
            continue;
        }
        if (w.loading) continue;
        w.loading = true;
        const std::string path = GMDXRegionCaptureState::mapFile(h);
        const std::string meta_path = GMDXRegionCaptureState::mapPath(h, "json");
        struct Loaded { LLPointer<LLImageRaw> raw; F32 width = 256.f; };
        auto load = [path, meta_path]() -> Loaded
        {
            Loaded out;
            try { out.raw = GMDXRegionCaptureState::loadMap(path); }
            catch (...) { return Loaded(); }
            if (out.raw.notNull() && out.raw->getWidth() > kWorldPx) out.raw->scale(kWorldPx, kWorldPx);
            std::string json;   // (a var-region's width: the World Map's own tiles are all 256 m)
            if (GMDXRegionCaptureState::readFile(meta_path, json))
            {
                try
                {
                    const LLSD meta = LlsdFromJson(boost::json::parse(json));
                    if (meta.has("region_width")) out.width = llclamp((F32)meta["region_width"].asReal(), 16.f, 8192.f);
                }
                catch (...) {}
            }
            return out;
        };
        auto done = [h, dir](Loaded loaded)
        {
            LLPointer<LLImageRaw> raw = loaded.raw;
            if (LLApp::isExiting() || dir != sWorldDir) return;
            auto it = sWorld.find(h);
            if (it == sWorld.end()) return;   // (dropped meanwhile)
            if (raw.isNull())
            {
                sWorld.erase(it);   // unreadable: tried again at the next scan
                return;
            }
            it->second.tex = GMDXRegionCaptureState::minimapTexture(raw);
            it->second.width = loaded.width;
        };
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
        if (!(main_queue && general && main_queue->postTo(general, load, done))) done(load());
    }
    // the least recently drawn textures go past the cap
    while (sWorld.size() > kWorldMax)
    {
        auto oldest = sWorld.begin();
        for (auto it = sWorld.begin(); it != sWorld.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
        sWorld.erase(oldest);
    }
}

LLViewerTexture* GMDXRegionCapture::getMinimapTexture(const LLViewerRegion* region)
{
    static LLCachedControl<bool> use_captured(gSavedSettings, "GMDXMinimapUseCapturedMaps", true);
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (!use_captured)
    {
        s.maps.clear();   // (their 4096 px textures go)
        return nullptr;
    }
    s.pruneMaps();
    if (!region) return nullptr;
    auto it = s.maps.find(region->getHandle());
    if (it == s.maps.end())
    {
        it = s.maps.emplace(region->getHandle(), MapTex()).first;
        s.loadMinimap(region, it->second);
    }
    return it->second.tex.get();
}

bool GMDXRegionCapture::getCapturedImage(const LLViewerRegion* region, LLPointer<LLImageRaw>& out)
{
    out = region ? GMDXRegionCaptureState::loadMap(GMDXRegionCaptureState::mapFile(region->getHandle())) : nullptr;
    return out.notNull();
}
