/**
 * @file ssdiscpad.cpp
 * @brief See ssdiscpad.h.
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

#include "ssdiscpad.h"

#include "ssatmoenvmanager.h"

#include "llimage.h"
#include "llmath.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    // The sample lines: eight axes every 22.5 degrees (cardinals, diagonals, intercardinals), each rotated off its exact direction by this angle. Diffraction-spike art (star bursts, flare crosses) paints spikes ALONG those axes, so a line on one would read the spike's tip as the disc edge; the rotation slides each line ~6.7 texels clear of the spoke by the rim (64 texels, 6 degrees off), so only a spike absurdly wide for its length survives.
    const F32 SS_DISC_PAD_SAMPLE_OFFSET_DEG = 6.f;

    // How many of the sixteen radial samples (eight lines, both directions) must fall in one agreement window for the disc to be trusted. A clean disc shows all sixteen agreeing; a couple clipped by art - a stray ray, a hole, a crescent's gap - still leave a clear majority. Half-plus-one IS a clear consensus - a glow sun whose rays ride the bright corona falloff reads 9 of 16 the same way. Less than this is no consensus, which is the error-out: 0 padding.
    const S32 SS_DISC_PAD_MIN_AGREE = 9;

    // The disc edge's alpha contour, as a fraction of the strongest alpha. 0.90: just inside where a radial glow falloff leaves the near-opaque plateau - the contour an author's hand-derived Nacon sun padding corresponds to. See the threshold comment in ssDiscPadAnalyze.
    const F32 SS_DISC_PAD_SOLID_ALPHA = 0.90f;

    // The agreement window's width: samples are "the same disc" when their quad fractions differ by no more than this (5% of the quad width, ~8 texels on a 128 texture).
    const F32 SS_DISC_PAD_AGREE_WINDOW = 0.05f;

    // Under this the derived padding is rounding noise - a disc so close to full-bleed that its margin means nothing (half a percent of the quad on each side).
    const F32 SS_DISC_PAD_NOISE_EPS = 0.005f;

    // The asset's own padding ceiling - see SSAtmoEnvCelestialBody::mDiscPadding.
    const F32 SS_DISC_PAD_MAX_PADDING = 0.45f;

    // A raw image this small is still the loading placeholder, not the art.
    const S32 SS_DISC_PAD_MIN_SIDE = 4;

    // Poll ticks a still-loading derivation may wait before erroring out to 0. At the floaters' half-second cadence that lets a slow disc texture ~15 seconds before giving up and showing the full-bleed fallback.
    const S32 SS_DISC_PAD_MAX_ATTEMPTS = 30;

    // The pending pool's cap. A sky import derives the sun and moon together, so a single slot would drop one whenever both are still loading; the cap only guards against an import on textures that never arrive.
    const S32 SS_DISC_PAD_MAX_PENDING = 8;

    // Still-loading derivations an auto-derive leaves behind, re-checked one by one by ssDiscPadPoll(). A job is superseded when its body gets a newer texture; a full pool drops the oldest - the newest disc art is the one whose padding matters.
    struct SsDiscPadPendingJob
    {
        S32 mTrack = -1;
        S32 mBody = -1;
        LLUUID mTexture;
        S32 mAttempts = 0;
    };

    std::vector<SsDiscPadPendingJob> gPendingPads;

    // One radial sample: walks the line from the texture centre, returns the disc as a fraction of the quad (diameter over the frame's shorter side) read along that line, or 1 when the energy is spread flat enough that even half of it is past the frame.
    //
    // The read is a HALF-ENERGY edge, not a threshold crossing. A glow-board sun is a tiny bright core inside a very large GENTLE gradient - the brightness may stay 97% of peak right out to the frame - so no fixed fraction of the peak (70%, 85%, 95%) ever finds the disc: it either never drops (padding 0, full-bleed) or clips near the core. The radial ENERGY profile still separates them: the bright core carries most of the light in a small radius, and the glow's spread adds a long low tail, so the radius that holds half the light is small for such art and large for a uniform disc - exactly what the authored disc is. The signal is luminance (RGB), since alpha in this art is either flat opaque or premultiplied into the same gradient; a texture with real transparency still reads correctly because the transparent region contributes no energy.
    //
    // Returns the disc fraction (diameter/ref) at the half-energy radius, with the half-texel correction, or 1.f when the line is flat enough that half the energy is at/past the edge.
    F32 sampleDiscLine(const LLImageRaw& raw, F32 centre_x, F32 centre_y,
                       F32 dir_x, F32 dir_y, F32 max_t, F32 ref, S32 step_sign, U8 threshold)
    {
        const S32 width = raw.getWidth();
        const S32 height = raw.getHeight();
        const U8* data = raw.getData();

        // The first sample must be ON the disc before any low alpha counts as an edge - a ring or glow disc's transparent centre is skipped, not read as a zero-radius disc. "Seen the disc" flips when a texel clears the threshold.
        bool seen = false;
        for (S32 t = 1; t <= (S32)max_t; ++t)
        {
            const S32 px = (S32)llroundf(centre_x + dir_x * (F32)t * (F32)step_sign);
            const S32 py = (S32)llroundf(centre_y + dir_y * (F32)t * (F32)step_sign);
            if (px < 0 || px >= width || py < 0 || py >= height) return 1.f;

            const F32 alpha = (F32)data[(py * width + px) * 4 + 3];
            if (!seen && alpha >= (F32)threshold)
            {
                seen = true;
                continue;
            }
            if (seen && alpha < (F32)threshold)
            {
                const F32 edge_radius = (F32)t - 0.5f;
                return llclamp(2.f * edge_radius / ref, 0.f, 1.f);
            }
        }
        return 1.f;
    }
}

// The analysis itself, agnostic of the asset: reads the texture's decoded pixels and finds the disc's share of the quad. See ssdiscpad.h for the status contract.
SSDiscPadStatus ssDiscPadAnalyze(const LLUUID& texture_id, F32& out_padding)
{
    if (texture_id.isNull())
    {
        out_padding = 0.f;
        return SSDiscPadStatus::FAILED;
    }

    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
        texture_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);

    if (!tex) return SSDiscPadStatus::LOADING;

    // <SS:Nexii> FULL-RESOLUTION pixels only. The display-level raw (getRawImage/readback) is a coarse GL mip when the disc renders small - a sun a few dozen pixels on screen keeps discard 3+, whose averaged alpha washes the 90% contour out to full-bleed (the logs: 12 of 16 lines reading "1.0"). The textured object keeps the full decode in its SAVED raw image - saveRawImage() runs when the loading decode is retired and keeps mRawImage at its decode level (0 for a normal texture) - but only when a save was requested. So: ask for a level-0 save, and LOADING until it lands. The retry cadence (the seeded write's 0.1s timer / the floaters' poll) is exactly the wait this needs; getFetchedTexture has already kicked the fetch.
    if (!tex->hasSavedRawImage() || tex->getSavedRawImageLevel() != 0)
    {
        tex->forceToSaveRawImage(0, F32_MAX);
        tex->addTextureStats((F32)MAX_IMAGE_AREA);
        LL_DEBUGS("AtmoMagicEnv") << "Disc pad: waiting for level-0 save of " << texture_id
            << " full " << tex->getFullWidth() << "x" << tex->getFullHeight() << LL_ENDL;
        return SSDiscPadStatus::LOADING;
    }

    LLImageRaw* raw = tex->getSavedRawImage();
    if (!raw)
    {
        return SSDiscPadStatus::LOADING;
    }

    const S32 width = raw->getWidth();
    const S32 height = raw->getHeight();
    if (width < SS_DISC_PAD_MIN_SIDE || height < SS_DISC_PAD_MIN_SIDE)
    {
        LL_DEBUGS("AtmoMagicEnv") << "Disc pad: raw too small " << width << "x" << height
                                  << " for " << texture_id << LL_ENDL;
        return SSDiscPadStatus::LOADING;
    }

    // No alpha channel: nothing transparent, so the whole quad IS the disc - full-bleed is the correct reading, not an error.
    if (raw->getComponents() != 4)
    {
        out_padding = 0.f;
        return SSDiscPadStatus::OK;
    }

    const U8* data = raw->getData();

    LL_DEBUGS("AtmoMagicEnv") << "Disc pad analyze " << texture_id << ": " << width << "x" << height
                              << " components " << raw->getComponents() << LL_ENDL;

    const F32 ref = (F32)llmin(width, height); // the frame the disc fractions live in
    const F32 centre_x = (F32)(width - 1) * 0.5f;
    const F32 centre_y = (F32)(height - 1) * 0.5f;
    const F32 max_t = ref * 0.5f;

    // The disc reference: the STRONGEST alpha near the centre - the exact centre texel can be transparent (glow, ring, crown discs), so probe a small disc-shaped patch around it.
    U8 ref_alpha = 0;
    const S32 probe_r = llmax(1, (S32)(ref * 0.25f));
    for (S32 py = (S32)centre_y - probe_r; py <= (S32)centre_y + probe_r; ++py)
    {
        for (S32 px = (S32)centre_x - probe_r; px <= (S32)centre_x + probe_r; ++px)
        {
            if (px < 0 || px >= width || py < 0 || py >= height) continue;
            ref_alpha = llmax(ref_alpha, data[(py * width + px) * 4 + 3]);
        }
    }
    if (ref_alpha <= 2)
    {
        // Nothing opaque anywhere near the centre (the shader discards under ~2/255): the lines have no edge to read - error out.
        out_padding = 0.f;
        return SSDiscPadStatus::FAILED;
    }

    // The disc edge: 90% of the strongest alpha. Not half-max (lands mid-glow, padding ~0.36 for the Nacon sun), not 95% (clips the tiny 255 plateau at ~0.44), but the 90% contour - exactly where the Nacon sun's radial alpha falloff crosses ~229/255 at disc_fraction ~0.145, the author-derived 0.43 padding. Reference is the STRONGEST centre-neighbour alpha (a ring/crown disc can have a transparent centre texel).
    const U8 threshold = llmax((U8)(ref_alpha * SS_DISC_PAD_SOLID_ALPHA), 1);

    // The full half-turn of axes, each rotated off its exact bearing by this angle.
    const F32 offset = SS_DISC_PAD_SAMPLE_OFFSET_DEG * DEG_TO_RAD;
    const F32 base_deg[8] = { 0.f, 22.5f, 45.f, 67.5f, 90.f, 112.5f, 135.f, 157.5f };

    std::vector<F32> fractions;
    fractions.reserve(16);
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 theta = base_deg[i] * DEG_TO_RAD + offset;
        const F32 dir_x = cosf(theta);
        const F32 dir_y = sinf(theta);
        fractions.push_back(sampleDiscLine(*raw, centre_x, centre_y, dir_x, dir_y,
                                           max_t, ref, 1, threshold));
        fractions.push_back(sampleDiscLine(*raw, centre_x, centre_y, dir_x, dir_y,
                                           max_t, ref, -1, threshold));
    }

    std::sort(fractions.begin(), fractions.end());

    // <SS:Nexii> Debug: the raw 16 fractions, so a wrong padding reads as data instead of a mystery - all lines full-bleed (alpha flat to the edge), a few clipped by art, or genuinely asymmetric?
    {
        std::string dump;
        for (const F32 f : fractions)
        {
            dump += llformat("%.3f ", f);
        }
        LL_INFOS("AtmoMagicEnv") << "Disc pad fractions for " << texture_id << ": ["
                                 << dump << "] ref_alpha " << (S32)ref_alpha
                                 << " threshold " << (S32)threshold << LL_ENDL;
    }

    // Where most lines agree: the densest band of samples within the agreement window. A lone line clipped by a ray or a gap lands outside it and costs nothing; the disc is whatever the sixteen-sample majority reads.
    const S32 count = (S32)fractions.size();
    S32 best_start = 0;
    S32 best_count = 0;
    for (S32 i = 0; i < count; ++i)
    {
        S32 in_window = 0;
        for (S32 j = i; j < count && fractions[j] - fractions[i] <= SS_DISC_PAD_AGREE_WINDOW; ++j)
        {
            ++in_window;
        }
        if (in_window > best_count)
        {
            best_count = in_window;
            best_start = i;
        }
    }

    if (best_count < SS_DISC_PAD_MIN_AGREE)
    {
        LL_WARNS("AtmoMagicEnv") << "Disc padding analysis of " << texture_id
                                 << ": the sample lines cannot agree where the disc is ("
                                 << best_count << " of " << count
                                 << " agree) - erroring out to 0 padding (full-bleed)"
                                 << LL_ENDL;
        out_padding = 0.f;
        return SSDiscPadStatus::FAILED;
    }

    // The disc fraction: the MEDIAN sample of the agreed window - a mean lets a barely-inside line pull the disc; the median is the sample most lines agree IS the disc.
    const S32 agreed_mid = best_start + best_count / 2;
    const F32 disc_fraction = fractions[agreed_mid];
    F32 padding = llclamp(0.5f * (1.f - disc_fraction), 0.f, SS_DISC_PAD_MAX_PADDING);
    if (padding < SS_DISC_PAD_NOISE_EPS)
    {
        padding = 0.f;
    }

    out_padding = padding;
    return SSDiscPadStatus::OK;
}

namespace
{
    // Writes a derived padding into a live asset body, only while it's still the body the derivation was requested for - the author may have deleted it or moved to another texture while the pixels decoded.
    void applyDerivedPadding(S32 track_index, S32 body_index, const LLUUID& texture_id, F32 padding)
    {
        SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        if (!mgr || !mgr->hasAsset()) return;

        SSAtmoEnvAsset& asset = mgr->editable();
        if (track_index < 0 || track_index >= (S32)asset.mTracks.size()) return;
        SSAtmoEnvPlanetary& planetary = asset.mTracks[(size_t)track_index].mPlanetary;
        if (body_index < 0 || body_index >= (S32)planetary.mBodies.size()) return;

        SSAtmoEnvCelestialBody& body = planetary.mBodies[(size_t)body_index];
        if (body.mCustomTexture != texture_id) return;

        if (body.mDiscPadding == padding) return;

        // <SS:Nexii> The body may carry a freshly-translated QUAD diameter (translateSettingsSky wrote the EEP glow-inclusive size and set mPadPendingTranslation). If so, this FIRST derive shrinks the diameter to the solid visible disc exactly once, then clears the mark - later author edits travel mark-less and never rescale; a re-import onto an already-worked body (mark cleared) keeps its size.
        if (body.mPadPendingTranslation)
        {
            const F32 new_fraction = llmax(1.f - 2.f * padding, 0.1f);
            body.mDiameterM *= new_fraction;
            body.mPadPendingTranslation = false;
        }

        LL_INFOS("AtmoMagicEnv") << "Auto-derived disc padding " << padding << " for body '"
                                 << body.mName << "' (track " << track_index << ") from texture "
                                 << texture_id << LL_ENDL;
        body.mDiscPadding = padding;
    }
}

// Derives and applies a body's disc padding when its disc texture changed - a texture pick or a sky import adopting the sky's disc art. See ssdiscpad.h.
void ssDiscPadAutoDerive(S32 track_index, S32 body_index, const LLUUID& texture_id)
{
    static LLCachedControl<bool> auto_pad(gSavedSettings, "SSAtmoDiscPadAuto", true);
    if (!auto_pad || texture_id.isNull())
    {
        if (!gPendingPads.empty())
        {
            gPendingPads.clear();
        }
        return;
    }

    // A job for this body is stale the moment its texture changes again - the new derivation below replaces it outright, before the old one's pixels can land late.
    auto superseded = [track_index, body_index](const SsDiscPadPendingJob& job)
    {
        return job.mTrack == track_index && job.mBody == body_index;
    };
    gPendingPads.erase(std::remove_if(gPendingPads.begin(), gPendingPads.end(), superseded),
                       gPendingPads.end());

    F32 padding = 0.f;
    const SSDiscPadStatus status = ssDiscPadAnalyze(texture_id, padding);
    LL_INFOS("AtmoMagicEnv") << "Disc pad derive for body " << body_index
                             << " texture " << texture_id << ": status "
                             << (S32)status << " padding " << padding << LL_ENDL;
    if (status == SSDiscPadStatus::LOADING)
    {
        if ((S32)gPendingPads.size() >= SS_DISC_PAD_MAX_PENDING)
        {
            gPendingPads.erase(gPendingPads.begin()); // the oldest waiting job
        }
        SsDiscPadPendingJob job;
        job.mTrack = track_index;
        job.mBody = body_index;
        job.mTexture = texture_id;
        gPendingPads.push_back(job);
        return;
    }

    applyDerivedPadding(track_index, body_index, texture_id, padding);
}

// Re-checks the still-loading derivations; ticked from the floaters' UI polls.
void ssDiscPadPoll()
{
    static LLCachedControl<bool> auto_pad(gSavedSettings, "SSAtmoDiscPadAuto", true);
    if (!auto_pad)
    {
        gPendingPads.clear();
        return;
    }
    if (gPendingPads.empty())
    {
        return;
    }

    for (size_t i = 0; i < gPendingPads.size();)
    {
        SsDiscPadPendingJob& job = gPendingPads[i];

        F32 padding = 0.f;
        const SSDiscPadStatus status = ssDiscPadAnalyze(job.mTexture, padding);
        LL_INFOS("AtmoMagicEnv") << "Disc pad poll re-check body " << job.mBody
                                 << " texture " << job.mTexture << ": status "
                                 << (S32)status << " padding " << padding << " (attempt "
                                 << job.mAttempts << ")" << LL_ENDL;
        if (status == SSDiscPadStatus::LOADING)
        {
            if (++job.mAttempts < SS_DISC_PAD_MAX_ATTEMPTS)
            {
                ++i;
                continue;
            }
            LL_WARNS("AtmoMagicEnv") << "Disc padding analysis of " << job.mTexture
                                     << " never got its pixels - erroring out to 0 padding"
                                     << LL_ENDL;
        }

        applyDerivedPadding(job.mTrack, job.mBody, job.mTexture, padding);
        gPendingPads.erase(gPendingPads.begin() + i);
    }
}