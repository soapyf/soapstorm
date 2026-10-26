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
#include "llappviewer.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llimagej2c.h"
#include "llimagepng.h"
#include "llnotificationsutil.h"
#include "llregionhandle.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llworld.h"
#include "llworldmap.h"
#include "rlvactions.h"
#include "workqueue.h"

#include <boost/json.hpp>
#include <algorithm>
#include <map>
#include <sstream>

namespace
{
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

// This grid's name in the maps folder and in a map file's "grid", as GunMetal-DX writes it: its
// grid manager's id ("Agni" on Second Life). Firestorm's OpenSim grid manager returns the grid's
// nick there instead, so its grid key is used (the login host GunMetal-DX's id is made from).
std::string grid_id()
{
#ifdef OPENSIM
    LLGridManager* grids = LLGridManager::getInstance();
    if (grids->isInSLMain()) return "Agni";
    if (grids->isInSLBeta()) return "Aditi";
    return utf8str_tolower(grids->getGrid());
#else
    return LLGridManager::getInstance()->getGridId();
#endif
}
} // namespace

// The mini-map's loaded maps and the files on disk.
struct GMDXRegionCaptureState
{
    std::map<U64, MapTex> maps;
    F64 next_prune = 0.0;

    static GMDXRegionCaptureState& get() { static GMDXRegionCaptureState s; return s; }

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

void GMDXRegionCapture::shutdown()
{
    GMDXRegionCaptureState::get().maps.clear();
    forEachWorldMapTexture(nullptr, nullptr);   // (the World Map's textures)
}

bool GMDXRegionCapture::hasCapture(const LLViewerRegion* region)
{
    return region && !GMDXRegionCaptureState::mapFile(region->getHandle()).empty();
}

void GMDXRegionCapture::deleteCapture(const LLViewerRegion* region)
{
    if (!region) return;
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
