/**
 * @file gmdxminimapimage.cpp
 * @brief GunMetal-DX: the region map image behind the mini-map's Copy / Save Map Image.
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

// <GMDX> whole file: see gmdxminimapimage.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxminimapimage.h"
#include "gmdxregioncapture.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llimagepng.h"
#include "llnotificationsutil.h"
#include "llsurface.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "rlvactions.h"

namespace
{
    typedef std::function<void(LLPointer<LLImageRaw>)> image_callback_t;

    // The map tile's loaded callback (the LLPreviewTexture::saveAs pattern): runs as the
    // resolution improves; the final call, or a failure, ends it.
    void on_tile_loaded(bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw* aux_src,
                        S32 discard_level, bool final, void* userdata)
    {
        if (success && !final)
        {
            return;
        }
        std::unique_ptr<image_callback_t> done(static_cast<image_callback_t*>(userdata));
        LLPointer<LLImageRaw> image;
        if (success && src && !src->isBufferInvalid())
        {
            // A copy: src is the texture's own saved image.
            LLImageDataSharedLock lock(src);
            image = new LLImageRaw(static_cast<const U8*>(src->getData()), src->getWidth(), src->getHeight(), src->getComponents());
        }
        else if (src_vi)
        {
            LL_WARNS("MinimapImage") << "Could not load the map tile " << src_vi->getID() << LL_ENDL;
        }
        (*done)(image);
    }

    void alert(const std::string& message)
    {
        LLSD args;
        args["MESSAGE"] = message;
        LLNotificationsUtil::add("GenericAlert", args);
    }

    const std::string NO_IMAGE = "The map image of this region is not available.";
}

bool gmdx_get_region_map_image(LLViewerRegion* regionp, std::function<void(LLPointer<LLImageRaw>)> done)
{
    if (!regionp || !done)
    {
        return false;
    }

    // A captured region map (gmdxregioncapture.h) wins over the server's tile while the mini-map
    // shows captured maps; the 4096 px PNG is decoded on a worker thread, done runs here later.
    static LLCachedControl<bool> use_captured(gSavedSettings, "GMDXMinimapUseCapturedMaps", true);
    if (use_captured && GMDXRegionCapture::loadCapturedImage(regionp, done))
    {
        return true;
    }

    // The server's world map tile, as the mini-map shows it (LLSurface::getSTexture).
    LLViewerFetchedTexture* tile = LLViewerTextureManager::staticCastToFetchedTexture(regionp->getLand().getSTexture());
    if (!tile)
    {
        return false;
    }
    tile->forceToSaveRawImage(0);   // the full resolution, re-fetched if the raw image was dropped
    tile->setLoadedCallback(on_tile_loaded, 0, true, false, new image_callback_t(std::move(done)), nullptr);
    return true;
}

void gmdx_save_region_map_image()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    // [RLVa] no region name in the proposed file name while the location is hidden
    const std::string name = (regionp && RlvActions::canShowLocation()) ? regionp->getName() : std::string("region map");
    const bool started = gmdx_get_region_map_image(regionp, [name](LLPointer<LLImageRaw> image)
        {
            if (image.isNull())
            {
                alert(NO_IMAGE);
                return;
            }
            LLFilePickerReplyThread::startPicker(
                [image](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    if (LLAppViewer::instance()->quitRequested() || filenames.empty())
                    {
                        return;
                    }
                    LLPointer<LLImagePNG> png = new LLImagePNG();
                    if (!png->encode(image, 0.f) || !png->save(filenames[0]))
                    {
                        LL_WARNS("MinimapImage") << "Could not save the map image to " << filenames[0] << LL_ENDL;
                        alert("Could not save the map image to " + filenames[0]);
                    }
                },
                LLFilePicker::FFSAVE_PNG, LLDir::getScrubbedFileName(name) + ".png");
        });
    if (!started)
    {
        alert(NO_IMAGE);
    }
}
