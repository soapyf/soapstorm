/**
 * @file ssatmoenvmanager.cpp
 * @brief See ssatmoenvmanager.h.
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

#include "ssatmoenvmanager.h"

#include "ssdiscpad.h" // <SS:Nexii> auto-derive the adopted disc faces' padding

#include "ssprecippreset.h"

#include "ssatmoenvplanetarystate.h"

#include "lleventtimer.h" // <SS:Nexii> the seeded write waits for still-loading disc textures

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <sstream>
#include <stdexcept>

#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

#include "llagent.h"
#include "llassetstorage.h"
#include "llbase64.h"
#include "lldate.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llpermissionsflags.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsettingssky.h"
#include "llsettingsvo.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llviewercontrol.h" // <SS:Nexii> SSAtmoDiscPadAuto - the seeded write obeys the same pad gate as the live editor
#include "apr_base64.h"
#include "roles_constants.h"

#include <ctime>

// Nothing loads implicitly - v3 is opt-in end to end.
SSAtmoEnvManager::SSAtmoEnvManager()
{
}

// Whether the working asset differs from its load-time baseline.
bool SSAtmoEnvManager::isModified() const
{
    if (!mHasAsset) return false;
    return !llsd_equals(mWorking.asLLSD(), mBaseline.asLLSD());
}

// Back to the load-time copy.
void SSAtmoEnvManager::revertToBaseline()
{
    if (!mHasAsset) return;
    mWorking = mBaseline;
}

namespace
{
    const char* SS_ATMO_ENV_MAGIC = "SS-ATMO-ENV-COMPRESSED";
    const size_t SS_ATMO_ENV_B64_COLS = 76;
    const size_t SS_ATMO_ENV_INFLATE_LIMIT = 16 * 1024 * 1024;

    std::string ss_atmo_b64_wrap(const std::string& b64)
    {
        std::string out;
        out.reserve(b64.size() + b64.size() / SS_ATMO_ENV_B64_COLS + 2);
        for (size_t i = 0; i < b64.size(); i += SS_ATMO_ENV_B64_COLS)
        {
            if (i > 0) out += '\n';
            out.append(b64, i, SS_ATMO_ENV_B64_COLS);
        }
        return out;
    }

    std::string ss_atmo_b64_strip(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
            {
                out += c;
            }
        }
        return out;
    }

    bool ss_atmo_b64decode(const std::string& b64, std::string& out)
    {
        const int size_guess = apr_base64_decode_len(b64.c_str());
        if (size_guess <= 0) return false;
        std::vector<U8> buf((size_t)size_guess);
        const int decoded = apr_base64_decode_binary(buf.data(), b64.c_str());
        if (decoded <= 0) return false;
        out.assign((const char*)buf.data(), (size_t)decoded);
        return true;
    }

    bool ss_atmo_deflate(const std::string& src, std::string& out)
    {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK) return false;
        strm.next_in = (Bytef*)src.data();
        strm.avail_in = (uInt)src.size();
        U8 chunk[16384];
        bool ok = true;
        for (;;)
        {
            strm.next_out = chunk;
            strm.avail_out = sizeof(chunk);
            const int ret = deflate(&strm, Z_FINISH);
            if (ret != Z_OK && ret != Z_STREAM_END)
            {
                ok = false;
                break;
            }
            out.append((const char*)chunk, sizeof(chunk) - strm.avail_out);
            if (ret == Z_STREAM_END) break;
        }
        deflateEnd(&strm);
        return ok;
    }

    bool ss_atmo_inflate(const std::string& src, std::string& out)
    {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit(&strm) != Z_OK) return false;
        strm.next_in = (Bytef*)src.data();
        strm.avail_in = (uInt)src.size();
        U8 chunk[16384];
        bool ok = false;
        for (;;)
        {
            strm.next_out = chunk;
            strm.avail_out = sizeof(chunk);
            const int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_END)
            {
                out.append((const char*)chunk, sizeof(chunk) - strm.avail_out);
                ok = true;
                break;
            }
            if (ret != Z_OK) break;
            out.append((const char*)chunk, sizeof(chunk) - strm.avail_out);
            if (out.size() > SS_ATMO_ENV_INFLATE_LIMIT) break;
        }
        inflateEnd(&strm);
        return ok;
    }
} // <SS:Nexii> Compressed notecard codec internals. The reader entry points below are shared with the legacy per-track layer (SSAtmoTrackManager::applyNotecardText), so the magic-header deflate+base64 format lives in exactly one place.

    // <SS:Nexii> The compressed notecard text codec, shared with the legacy per-track layer: a parcel can reference a card written by this environment's own save / drag-and-drop path, whose SS-ATMO-ENV-COMPRESSED deflate+base64 payload only this reader understands. Both layers route notecard text through ss_atmo_env_from_notecard_text, so saving compressed and loading anywhere agree.
    bool ss_atmo_env_payload_is_compressed(const std::string& text)
    {
        const size_t magic_len = strlen(SS_ATMO_ENV_MAGIC);
        return text.size() >= magic_len && text.compare(0, magic_len, SS_ATMO_ENV_MAGIC) == 0;
    }

    bool ss_atmo_env_from_notecard_text(const std::string& text, LLSD& out_sd, std::string& out_error)
    {
        const size_t magic_len = strlen(SS_ATMO_ENV_MAGIC);
        const bool compressed = ss_atmo_env_payload_is_compressed(text);

        if (compressed)
        {
            const size_t nl = text.find('\n', magic_len);
            if (nl == std::string::npos)
            {
                out_error = "compressed payload missing header line";
                return false;
            }
            const std::string header = text.substr(0, nl);
            std::string version = header.substr(magic_len);
            while (!version.empty()
                   && (version.back() == ' ' || version.back() == '\t' || version.back() == '\r'))
            {
                version.pop_back();
            }
            if (version != " 1" && version != "1")
            {
                out_error = "unknown compressed format version '" + version + "'";
                return false;
            }

            const std::string b64 = ss_atmo_b64_strip(text.substr(nl + 1));
            std::string packed;
            if (!ss_atmo_b64decode(b64, packed))
            {
                out_error = "base64 decode failed";
                return false;
            }

            std::string raw;
            if (!ss_atmo_inflate(packed, raw))
            {
                out_error = "inflate failed";
                return false;
            }

            std::istringstream stream(raw);
            if (LLSDSerialize::fromBinary(out_sd, stream, (llssize)raw.size())
                == LLSDParser::PARSE_FAILURE)
            {
                out_error = "binary LLSD parse failed";
                return false;
            }

            LL_INFOS("AtmoMagicEnv") << "Atmo v3 compressed payload check: b64 " << b64.size()
                                     << " chars -> " << packed.size() << " packed bytes -> "
                                     << raw.size() << " bytes LLSD, binary parse OK" << LL_ENDL;
            return true;
        }

        bool parsed = false;
        if (text.find("<llsd") != std::string::npos)
        {
            std::istringstream stream(text);
            parsed = (LLSDSerialize::fromXML(out_sd, stream) != LLSDParser::PARSE_FAILURE);
        }
        if (!parsed)
        {
            std::istringstream retry(text);
            parsed = (LLSDSerialize::fromNotation(out_sd, retry, (S32)text.size()) != LLSDParser::PARSE_FAILURE);
        }
        if (!parsed)
        {
            out_error = "not valid LLSD";
        }
        return parsed;
    }

    namespace
    {
    // <SS:Nexii> Debug cache: every save drops the environment's FULL asset LLSD into UserSettings/ss_weather/env_cache as timestamped pretty XML and rewrites last.xml naming the current one (plus the inventory asset id once the upload lands). The notecard payload is deflate+base64 and useless in a text editor; these files are reviewable while debugging and the history shows exactly what a session changed.
    const size_t SS_ATMO_ENV_CACHE_KEEP = 24;

    std::string ss_atmo_env_cache_dir()
    {
        const std::string parent = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather");
        if (!gDirUtilp->fileExists(parent))
        {
            LLFile::mkdir(parent);
        }
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather", "env_cache");
        if (!gDirUtilp->fileExists(dir))
        {
            LLFile::mkdir(dir);
        }
        return dir;
    }

    // Inventory names allow nearly anything; filenames do not.
    std::string ss_atmo_cache_name(std::string name)
    {
        for (char& c : name)
        {
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!keep) c = '_';
        }
        return name;
    }

    // Dumps the full LLSD pretty XML and prunes old dumps; returns the dump's FILE NAME (not path).
    std::string ss_atmo_env_cache_dump(const SSAtmoEnvAsset& asset, const std::string& name)
    {
        const std::string dir = ss_atmo_env_cache_dir();

        std::time_t now = std::time(nullptr);
        std::tm tm_now{};
#ifdef LL_WINDOWS
        localtime_s(&tm_now, &now);
#else
        localtime_r(&now, &tm_now);
#endif
        char stamp[32] = {};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);

        std::ostringstream fname;
        fname << stamp << "_" << ss_atmo_cache_name(name) << ".xml";
        std::string file = fname.str();
        std::string path = dir + gDirUtilp->getDirDelimiter() + file;
        while (gDirUtilp->fileExists(path))
        {
            file += "_b";
            path = dir + gDirUtilp->getDirDelimiter() + file;
        }

        llofstream out(path.c_str());
        if (!out.is_open())
        {
            LL_WARNS("AtmoMagicEnv") << "Could not write Atmo env cache dump " << path << LL_ENDL;
            return std::string();
        }
        LLSDSerialize::toPrettyXML(asset.asLLSD(), out);
        out.close();

        // Prune: newest SS_ATMO_ENV_CACHE_KEEP dumps survive; last.xml is the pointer, not a dump.
        std::vector<std::string> dumps;
        LLDirIterator dir_iter(dir, "*.xml");
        std::string found;
        while (dir_iter.next(found))
        {
            if (found != "last.xml") dumps.push_back(found);
        }
        std::sort(dumps.begin(), dumps.end(), std::greater<std::string>());
        for (size_t i = SS_ATMO_ENV_CACHE_KEEP; i < dumps.size(); ++i)
        {
            LLFile::remove((dir + gDirUtilp->getDirDelimiter() + dumps[i]).c_str());
        }

        LL_INFOS("AtmoMagicEnv") << "Atmo env cache: dumped full LLSD to " << path << LL_ENDL;
        return file;
    }

    // The last.xml pointer: which dump is current, when written, and (after the upload completes) which inventory asset carries it. Empty asset_id means "still uploading".
    void ss_atmo_env_cache_last(const std::string& name, const std::string& dump_file,
                                const LLUUID& asset_id)
    {
        const std::string dir = ss_atmo_env_cache_dir();
        const std::string path = dir + gDirUtilp->getDirDelimiter() + "last.xml";

        LLSD sd = LLSD::emptyMap();
        sd["name"] = name;
        sd["saved"] = LLDate::now().asString();
        sd["dump"] = dump_file;
        sd["asset_id"] = asset_id;

        llofstream out(path.c_str());
        if (!out.is_open())
        {
            LL_WARNS("AtmoMagicEnv") << "Could not write Atmo env cache pointer" << LL_ENDL;
            return;
        }
        LLSDSerialize::toPrettyXML(sd, out);
    }

    bool ss_atmo_env_to_notecard_text(const SSAtmoEnvAsset& asset, std::string& out_text, std::string& out_error)
    {
        const LLSD sd = asset.asLLSD();

        std::ostringstream bin;
        LLSDSerialize::toBinary(sd, bin);
        const std::string raw = bin.str();

        std::string packed;
        if (!ss_atmo_deflate(raw, packed))
        {
            out_error = "deflate failed";
            return false;
        }

        const std::string b64 = LLBase64::encode((const U8*)packed.data(), packed.size());
        out_text = std::string(SS_ATMO_ENV_MAGIC) + " 1\n" + ss_atmo_b64_wrap(b64);

        LL_INFOS("AtmoMagicEnv") << "Atmo v3 payload: LLSD " << raw.size() << "B -> deflate "
                                 << packed.size() << "B -> base64 " << b64.size()
                                 << "B -> notecard text " << out_text.size() << "B (limit "
                                 << LLNotecard::MAX_SIZE << ")" << LL_ENDL;

        if (out_text.size() >= (size_t)LLNotecard::MAX_SIZE)
        {
            out_error = "environment too large for a notecard (" + std::to_string(out_text.size())
                        + " bytes)";
            return false;
        }

        LLSD roundtrip;
        std::string check_error;
        if (!ss_atmo_env_from_notecard_text(out_text, roundtrip, check_error)
            || !llsd_equals(roundtrip, sd))
        {
            out_error = "round-trip self-check failed: " + check_error;
            return false;
        }

        LL_INFOS("AtmoMagicEnv") << "Atmo v3 payload round-trip self-check OK ("
                                 << out_text.size() << "B)" << LL_ENDL;
        return true;
    }
}

namespace
{
    void writeAssetAsNotecard(const SSAtmoEnvAsset& asset, const std::string& name,
                               const LLUUID& parent_id_in,
                               std::function<void(const LLUUID& item_id, const LLUUID& asset_id)> on_created)
    {
        // <SS:Nexii> Debug cache written FIRST - a serialization failure below is exactly what the dump is for.
        const std::string dump_file = ss_atmo_env_cache_dump(asset, name);
        ss_atmo_env_cache_last(name, dump_file, LLUUID::null);

        std::string env_text;
        std::string error;
        if (!ss_atmo_env_to_notecard_text(asset, env_text, error))
        {
            LL_WARNS("AtmoMagicEnv") << "Could not serialize Atmo v3 environment '" << name
                                     << "': " << error << LL_ENDL;
            if (on_created) on_created(LLUUID::null, LLUUID::null);
            return;
        }

        LLNotecard nc(LLNotecard::MAX_SIZE);
        nc.setText(env_text);
        std::ostringstream wrapped;
        nc.exportStream(wrapped);
        const std::string asset_text = wrapped.str();

        LL_INFOS("AtmoMagicEnv") << "Atmo v3 notecard for '" << name << "': full asset text "
                                 << asset_text.size() << "B" << LL_ENDL;

        const LLUUID parent_id = parent_id_in;

        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
            [asset_text, name, dump_file, on_created](const LLUUID& new_item_id)
            {
                LLViewerRegion* region = gAgent.getRegion();
                const std::string url = region
                    ? region->getCapability("UpdateNotecardAgentInventory")
                    : std::string();
                if (new_item_id.isNull() || url.empty())
                {
                    LL_WARNS("AtmoMagicEnv") << "Could not create Atmo v3 notecard item" << LL_ENDL;
                    if (on_created) on_created(LLUUID::null, LLUUID::null);
                    return;
                }

                LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
                    new_item_id, LLAssetType::AT_NOTECARD, asset_text,
                    [name, dump_file, new_item_id, on_created](LLUUID, LLUUID new_asset_id, LLUUID, LLSD)
                    {
                        LL_INFOS("AtmoMagicEnv") << "Saved Atmo v3 environment '" << name
                                                 << "' as asset " << new_asset_id << LL_ENDL;
                        // The pointer file now carries the asset id the dump was saved under.
                        ss_atmo_env_cache_last(name, dump_file, new_asset_id);
                        if (on_created) on_created(new_item_id, new_asset_id);
                    },
                    nullptr);

                LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            });

        create_inventory_item(gAgentID, gAgentSessionID, parent_id, LLTransactionID::tnull,
                               name, "Atmo Magic environment", LLAssetType::AT_NOTECARD,
                               LLInventoryType::IT_NOTECARD, NO_INV_SUBTYPE, PERM_ALL, cb);
    }
}

// Finds (or on first ever use creates) the inventory folder v3 notecards live in, async.
void SSAtmoEnvManager::atmoFolderId(std::function<void(const LLUUID&)> on_ready)
{
    static const std::string ATMO_FOLDER_NAME = "Atmo Magic";

    const LLUUID settings_folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_SETTINGS);

    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(settings_folder, cats, items);
    if (cats)
    {
        for (LLViewerInventoryCategory* cat : *cats)
        {
            if (cat->getName() == ATMO_FOLDER_NAME)
            {
                if (on_ready) on_ready(cat->getUUID());
                return;
            }
        }
    }

    gInventory.createNewCategory(settings_folder, LLFolderType::FT_NONE, ATMO_FOLDER_NAME,
        [on_ready](const LLUUID& new_cat_id)
        {
            if (on_ready) on_ready(new_cat_id);
        });
}

namespace
{
    // <SS:Nexii> Deep-copy guard. The create path copies the asset into its async callbacks, and two sessions in a row that copy died in std::vector::_Xlength - a wrecked container header inside the object, hit while the memory around it still reads as ordinary C++. Copying once here under a guard does three things: the insane-count case (the outer mTracks header) logs the raw numbers, the sane-count case gets the deep copy's throw logged, and everything downstream (captures, notecard, adoptCreated) copies a known-good object, not the tainted one. The fallback is the clean seed, so creation still completes; the log line is the diagnosis.
    SSAtmoEnvAsset ss_atmo_env_sanitize(const SSAtmoEnvAsset& def)
    {
        if (def.mTracks.size() > (size_t)SS_ATMOENV_MAX_TRACKS)
        {
            LL_WARNS("AtmoMagicEnv") << "Atmo v3 asset corruption: mTracks.size() 0x" << std::hex
                                     << def.mTracks.size() << std::dec << ", data "
                                     << (const void*)def.mTracks.data()
                                     << " - rebuilding a clean seed for the notecard" << LL_ENDL;
            return SSAtmoEnvAsset::makeDefault();
        }

        try
        {
            return def;
        }
        catch (const std::exception& e)
        {
            LL_WARNS("AtmoMagicEnv") << "Atmo v3 asset corruption: deep copy threw '" << e.what()
                                     << "' with a sane track count - a track's keyframe store is "
                                        "wrecked - rebuilding a clean seed for the notecard"
                                     << LL_ENDL;
            return SSAtmoEnvAsset::makeDefault();
        }
    }

    void writeDefaultNotecard(const SSAtmoEnvAsset& def, const LLUUID& parent_id,
                               std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
    {
        const SSAtmoEnvAsset safe_def = ss_atmo_env_sanitize(def);

        auto write = [safe_def, on_created](const LLUUID& folder_id)
        {
            writeAssetAsNotecard(safe_def, safe_def.mName, folder_id,
                [safe_def, on_created](const LLUUID& item_id, const LLUUID& asset_id)
                {
                    if (on_created) on_created(item_id, asset_id, safe_def);
                });
        };

        if (parent_id.notNull())
        {
            write(parent_id);
            return;
        }
        SSAtmoEnvManager::atmoFolderId(write);
    }
}

namespace
{
    // The shipped skies the stock day cycle seeds from.
    const S32 STOCK_SEED_SKY_COUNT = 4;
    const char* const STOCK_SEED_SKY_ID[STOCK_SEED_SKY_COUNT] = {
        "7250bab8-0a2c-0cb7-8161-6717e194da43",
        "db8115a4-9549-9f7d-97ca-a791d0a99a0f",
        "cd8afef7-4276-3f46-6122-6165d97f3e87",
        "7b43eefd-f390-0c79-c30e-a03b3e0ef9c8"
    };

    const char* const STOCK_SEED_SKY_NAME[STOCK_SEED_SKY_COUNT] = {
        "Daylight", "Night", "Sunrise", "Sunset"
    };

    struct SeedSkyCollector
    {
        std::vector<LLSettingsSky::ptr_t> mSkies;
        std::vector<std::string> mNames;
        std::vector<bool> mDone;
        S32 mPending = 0;
    };

    // Log label for a requested sky: the caller's name when it has one, otherwise an index.
    std::string seedSkyLabel(const SeedSkyCollector& skies, size_t slot)
    {
        if (slot < skies.mNames.size() && !skies.mNames[slot].empty())
        {
            return skies.mNames[slot];
        }
        return llformat("sky %d", (S32)slot);
    }

    // Fetches skies by asset id and calls on_done once every fetch has settled. A failed fetch logs and leaves a null slot; the seed builder decides what a hole means.
    void fetchSeedSkies(const std::vector<LLUUID>& asset_ids,
                        const std::vector<std::string>& names,
                        std::function<void(const SeedSkyCollector&)> on_done)
    {
        auto collector = std::make_shared<SeedSkyCollector>();
        collector->mSkies.resize(asset_ids.size());
        collector->mNames.resize(asset_ids.size());
        for (size_t slot = 0; slot < asset_ids.size() && slot < names.size(); ++slot)
        {
            collector->mNames[slot] = names[slot];
        }
        collector->mDone.resize(asset_ids.size(), false);
        collector->mPending = (S32)asset_ids.size();

        for (size_t slot = 0; slot < asset_ids.size(); ++slot)
        {
            LLSettingsVOBase::getSettingsAsset(asset_ids[slot],
                [collector, slot, on_done](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
                {
                    if (collector->mDone[slot]) return;
                    collector->mDone[slot] = true;

                    LLSettingsSky::ptr_t sky;
                    if (!status && settings)
                    {
                        sky = std::dynamic_pointer_cast<LLSettingsSky>(settings);
                    }
                    if (sky)
                    {
                        collector->mSkies[slot] = sky;
                    }
                    else
                    {
                        LL_WARNS("AtmoMagicEnv") << "Could not fetch seed sky " << seedSkyLabel(*collector, slot)
                                                 << " sky " << asset_id << " (status " << status
                                                 << "); seeding the new Atmo v3 environment without it" << LL_ENDL;
                    }

                    if (--collector->mPending > 0) return;

                    if (on_done) on_done(*collector);
                });
        }
    }

    // Even-spread fallback placement when no sun exists to measure against: one sky per equal slice of the arrived set.
    F64 seedSkyEvenPhase(size_t slot, size_t count)
    {
        return count > 0 ? ss_atmoenv_snap_phase((F64)slot / (F64)count) : 0.0;
    }

    // <SS:Nexii> What a sky's NAME says about its placement. Placement comes from the sky's measured dominant-body ELEVATION (see seedSkyMeasureBody) - order by height, not a fixed clock - and the name only sorts it to the right side of noon: Morning/Dawn/Sunrise claim the rising side, Evening/Dusk/Sunset/Night the setting side, Noon/Midnight the two extreme anchors. Sunrise/Sunset are the most specific side claims - a sky parked at the horizon IS exactly those - so they are checked before the qualifier words; "Afternoon" is not a "Noon" claim. 256 steps clock midnight 0.0, sunrise 0.25, noon 0.5, sunset 0.75, midnight 1.0.
    enum class SeedSkySide
    {
        NONE,
        RISING,
        SETTING
    };

    struct SeedSkyNameHint
    {
        SeedSkySide mSide = SeedSkySide::NONE;
        bool mMidnight = false;
        bool mNoon = false;
        // The day phase a name pins outright - 0.0 (midnight), 0.25 (sunrise), 0.5 (noon) or 0.75 (sunset) - or -1 when the name only claims a side. The pinned skies double as the fit data seedSkyFitSunArc sketches the pack's sun path from.
        F64 mAnchorPhase = -1.0;
    };

    SeedSkyNameHint seedSkyNameHint(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = (char)tolower((unsigned char)c);

        SeedSkyNameHint hint;
        if (lower.find("midnight") != std::string::npos)
        {
            hint.mMidnight = true;
            hint.mAnchorPhase = 0.0;
        }
        else if (lower.find("daylight") != std::string::npos)
        {
            // "Daylight" alone states the noon side of the day.
            hint.mNoon = true;
            hint.mAnchorPhase = 0.5;
        }
        else if (lower.find("noon") != std::string::npos
                 && lower.find("afternoon") == std::string::npos)
        {
            hint.mNoon = true;
            hint.mAnchorPhase = 0.5;
        }
        else if (lower.find("golden hour") != std::string::npos)
        {
            // Golden hour is a window, not a horizon event: its own measured sun places
            // it; the adjacent qualifier word pieces which side of noon it owns.

            if (lower.find("morning") != std::string::npos)
            {
                hint.mSide = SeedSkySide::RISING;
            }
            else if (lower.find("evening") != std::string::npos)
            {
                hint.mSide = SeedSkySide::SETTING;
            }
        }
        else if (lower.find("post sunrise") != std::string::npos
                 || lower.find("after sunrise") != std::string::npos
                 || lower.find("late morning") != std::string::npos)
        {
            // Qualified sunrise/sunset skies ride their own measured sun on their side, never
            // parking on the bare event's anchor - so a Post Sunrise cannot overwrite the Sunrise.

 
            hint.mSide = SeedSkySide::RISING;
 
        }
        else if (lower.find("near sunset") != std::string::npos
                 || lower.find("before sunset") != std::string::npos
                 || lower.find("early sunset") != std::string::npos
                 || lower.find("early evening") != std::string::npos
                 || lower.find("post sunset") != std::string::npos
                 || lower.find("after sunset") != std::string::npos
                 || lower.find("late afternoon") != std::string::npos)
 
        {
            hint.mSide = SeedSkySide::SETTING; 
        }
        else if (lower.find("sunrise") != std::string::npos)

        {
            hint.mSide = SeedSkySide::RISING; 
            hint.mAnchorPhase = 0.25; 
 
        } 
 
        else if (lower.find("sunset") != std::string::npos) 

 
        {
            hint.mSide = SeedSkySide::SETTING; 
 
            hint.mAnchorPhase = 0.75; 
 
        } 
 
        else if (lower.find("night") != std::string::npos)
        {
            // "Night" alone states the midnight side - even without a moon to measure, it must never land in daylight.
            hint.mMidnight = true;
            hint.mAnchorPhase = 0.0;
        }
        else if (lower.find("morning") != std::string::npos
                 || lower.find("dawn") != std::string::npos)
        {
            hint.mSide = SeedSkySide::RISING;
        }
        else if (lower.find("evening") != std::string::npos
                 || lower.find("dusk") != std::string::npos)
        {
            hint.mSide = SeedSkySide::SETTING;
        }
        return hint;
    }

    // <SS:Nexii> Whether a sky's name makes a side or anchor claim at all - the skies the same-height squash below must not re-sort.
    bool seedSkyHasNameHint(const SeedSkyNameHint& hint)
    {
        return hint.mSide != SeedSkySide::NONE || hint.mMidnight || hint.mNoon;
    }

    // <SS:Nexii> An anchored sky: the phase its name pins (one of the four) and the height its sun was actually drawn at. Enough of these sketch the pack's authored sun path (see seedSkyFitSunArc) that every other sky places itself against.
    struct SeedSkySunAnchor
    {
        F64 mPhase;
        F32 mSinElevation;
    };

    // The daily elevation of a body follows a sine about the noon/midnight extremes: sin(alt) = a - b*cos(2PI*phase). Sunrise (0.25) / sunset (0.75) sit on the cosine's zero-crossings, so anchored horizon skies give the midpoint a directly; noon (0.5) / midnight (0.0) give the half-day swing b - the anchored pack's path is recovered without solving tilt or latitude; the phases are then just the roots of this curve. Returns false (the default arc stands in) when there aren't enough anchors, or when they disagree - a "sunrise" sky whose sun sits far from the fitted midpoint is not part of one path.
    bool seedSkyFitSunArc(const std::vector<SeedSkySunAnchor>& anchors,
                          const SSAtmoEnvDiurnalArc& default_arc,
                          F32& out_a, F32& out_b)
    {
        out_a = default_arc.mOffset;
        out_b = default_arc.mAmplitude;
        if (anchors.size() < 2) return false;

        F32 horizon_a_sum = 0.f;
        S32 horizon_a_count = 0;
        F32 noon_sum = 0.f, mid_sum = 0.f;
        S32 noon_count = 0, mid_count = 0;

        for (const SeedSkySunAnchor& an : anchors)
        {
            if (an.mPhase == 0.25 || an.mPhase == 0.75)
            {
                horizon_a_sum += an.mSinElevation;
                ++horizon_a_count;
            }
            else if (an.mPhase == 0.5)
            {
                noon_sum += an.mSinElevation;
                ++noon_count;
            }
            else if (an.mPhase == 0.0)
            {
                mid_sum += an.mSinElevation;
                ++mid_count;
            }
        }

        // The curve's midpoint: the horizon anchors when present, else the noon/midnight mean.
        F32 a = 0.f;
        S32 a_count = 0;
        if (horizon_a_count > 0)
        {
            a += horizon_a_sum / (F32)horizon_a_count;
            ++a_count;
        }
        if (noon_count > 0 && mid_count > 0)
        {
            a += 0.5f * (noon_sum / (F32)noon_count + mid_sum / (F32)mid_count);
            ++a_count;
        }
        if (a_count == 0) return false;
        a /= (F32)a_count;

        // The half-day swing: the noon/midnight span, or a single extreme against the midpoint.
        F32 b = default_arc.mAmplitude;
        if (noon_count > 0 && mid_count > 0)
        {
            b = 0.5f * (noon_sum / (F32)noon_count - mid_sum / (F32)mid_count);
        }
        else if (noon_count > 0)
        {
            b = noon_sum / (F32)noon_count - a;
        }
        else if (mid_count > 0)
        {
            b = a - mid_sum / (F32)mid_count;
        }
        b = llmax(b, 0.05f);

        // Physical sanity, and the horizon anchors' own agreement with the midpoint they made.
        if (a + b > 1.05f || a - b < -1.05f) return false;
        for (const SeedSkySunAnchor& an : anchors)
        {
            if (an.mPhase == 0.25 || an.mPhase == 0.75)
            {
                if (llabs(an.mSinElevation - a) > 0.25f) return false;
            }
        }

        out_a = a;
        out_b = b;
        return true;
    }

    // The phase on an elevation curve (a, b) at a given height, rising or setting side: root of a - b*cos(2PI p) = el, one p in [0, 0.5], mirrored to 1-p for the setting side.
    F64 seedSkySunCurvePhase(F32 sin_elevation, bool rising, F32 a, F32 b)
    {
        const double x = llclamp((double)(a - sin_elevation) / (double)b, -1.0, 1.0);
        const F64 p = std::acos(x) / (double)F_TWO_PI;
        return rising ? p : 1.0 - p;
    }

    // Where a fetched sky's own sun or moon stands, in the observer frame.
    LLVector3 seedSkyBodyDirection(const LLSettingsSky& sky, bool moon)
    {
        LLVector3 dir = LLVector3::x_axis * (moon ? sky.getMoonRotation()
                                                  : sky.getSunRotation());
        if (dir.normalize() < 0.0001f) dir = LLVector3::z_axis;
        return dir;
    }

    // <SS:Nexii> The reference a sky is measured against when no name pins it: the DOMINANT body wins - the HIGHER of the two. Sun when it stands higher (a pointed-at sun is a pointed-at time; a pre-dawn/dusk sun just under the horizon still rules the light), moon when the moon stands higher - night skies whose sun sits deep below the horizon while the moon does the moving. No moon role falls back to the sun, the pre-dominant-body behaviour.
    struct SeedSkyMeasure
    {
        LLVector3 mEcliptic;      // the track's reference body (sun or moon) in ecliptic space
        LLVector3 mTarget;        // where the sky asset drew that body, in the observer frame
        F32 mSinElevation = 0.f;  // the target's height above the horizon
        SSAtmoEnvDiurnalArc mArc; // the diurnal arc of mEcliptic at this track's tilt/latitude
        bool mMoon = false;
    };

    SeedSkyMeasure seedSkyMeasureBody(const LLSettingsSky& sky, const SSAtmoEnvResolvedBody& sun,
                                      const SSAtmoEnvResolvedBody& moon, bool have_moon,
                                      F32 tilt, F32 lat)
    {
        SeedSkyMeasure m;
        const LLVector3 sky_sun = seedSkyBodyDirection(sky, false);
        const LLVector3 sky_moon = have_moon ? seedSkyBodyDirection(sky, true) : LLVector3::zero;
        if (!have_moon || sky_sun.mV[VZ] >= sky_moon.mV[VZ])
        {
            m.mEcliptic = sun.mDirection;
            m.mTarget = sky_sun;
        }
        else
        {
            m.mEcliptic = moon.mDirection;
            m.mTarget = sky_moon;
            m.mMoon = true;
        }
        m.mSinElevation = m.mTarget.mV[VZ];
        m.mArc = SSAtmoEnvPlanetaryResolver::diurnalArc(m.mEcliptic, tilt, lat);
        return m;
    }

    void seedSkyPhases(const SSAtmoEnvTrack& track, const SeedSkyCollector& skies,
                       std::vector<F64>& out_phase)
    {
        const size_t count = skies.mSkies.size();

        out_phase.resize(count);
        for (size_t slot = 0; slot < count; ++slot)
        {
            out_phase[slot] = seedSkyEvenPhase(slot, count);
        }

        SSAtmoEnvResolvedBody sun;
        SSAtmoEnvResolvedBody moon;
        SSAtmoEnvPlanetaryResolver::resolveLightRoles(track.mPlanetary, sun, moon);
        if (sun.mBodyIndex < 0) return;
        const bool have_moon = moon.mBodyIndex >= 0;

        const S32 home = track.mPlanetary.homeBodyIndex();
        const F32 tilt = (home >= 0)
            ? track.mPlanetary.mBodies[static_cast<size_t>(home)].mAxialTiltDeg : 0.f;
        const F32 lat = (home >= 0)
            ? track.mPlanetary.mBodies[static_cast<size_t>(home)].mLatitudeDeg : 0.f;

        const SSAtmoEnvDiurnalArc sun_arc =
            SSAtmoEnvPlanetaryResolver::diurnalArc(sun.mDirection, tilt, lat);

        // <SS:Nexii> The confident anchors first: sunrise/sunset/noon/midnight-named skies pin BOTH a phase and the height their sun was drawn at, sketching the pack's authored sun path; the fit is what every other sun-elevated sky places itself on.
        std::vector<SeedSkySunAnchor> sun_anchors;
        for (size_t slot = 0; slot < count; ++slot)
        {
            if (!skies.mSkies[slot]) continue;
            const SeedSkyNameHint hint = seedSkyNameHint(seedSkyLabel(skies, slot));
            if (hint.mAnchorPhase < 0.0) continue;
            sun_anchors.push_back({ hint.mAnchorPhase,
                                    seedSkyBodyDirection(*skies.mSkies[slot], false).mV[VZ] });
        }

        F32 sun_a = 0.f, sun_b = 0.f;
        const bool sun_fit = seedSkyFitSunArc(sun_anchors, sun_arc, sun_a, sun_b);
        if (sun_fit)
        {
            LL_INFOS("AtmoMagicEnv") << "Seed sun path fit from "
                << sun_anchors.size() << " anchored skies: sin(alt) = " << sun_a
                << " - " << sun_b << "*cos(2PI*phase)" << LL_ENDL;
        }

        std::vector<SeedSkyMeasure> measures(count);
        std::vector<F64> measured(count);
        // <SS:Nexii> Placement by ELEVATION: an anchored sky pins its named phase, every other sky lands where its reference body reaches the height the sky drew it. A sun-side sky uses the fitted (or default) sun path, so the day's order falls out of the heights - a "Morning Umbra" with its sun below the horizon lands pre-dawn, not in daylight. A moon-side sky uses its own arc's crossing; an unnamed sky keeps the azimuth-derived phase.
        for (size_t slot = 0; slot < count; ++slot)
        {
            measured[slot] = out_phase[slot];
            if (!skies.mSkies[slot]) continue;

            measures[slot] = seedSkyMeasureBody(*skies.mSkies[slot], sun, moon, have_moon, tilt, lat);
            const SeedSkyNameHint hint = seedSkyNameHint(seedSkyLabel(skies, slot));

            if (hint.mAnchorPhase >= 0.0)
            {
                measured[slot] = hint.mAnchorPhase;
            }
            else if (hint.mSide != SeedSkySide::NONE)
            {
                if (!measures[slot].mMoon)
                {
                    // The sun's path, fitted from the anchors when they agreed.
                    F32 a = sun_a, b = sun_b;
                    if (!sun_fit)
                    {
                        a = sun_arc.mOffset;
                        b = sun_arc.mAmplitude;
                    }
                    measured[slot] = ss_atmoenv_snap_phase(
                        seedSkySunCurvePhase(measures[slot].mSinElevation,
                                             hint.mSide == SeedSkySide::RISING, a, b));
                }
                else
                {
                    // The moon's own arc: the crossing of the height this sky drew its moon.
                    F64 crossing = 0.0;
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(
                        measures[slot].mArc, measures[slot].mSinElevation,
                        hint.mSide == SeedSkySide::RISING, crossing);
                    measured[slot] = ss_atmoenv_snap_phase(crossing);
                }
            }
            else
            {
                measured[slot] = ss_atmoenv_snap_phase(
                    SSAtmoEnvPlanetaryResolver::phaseForSunDirection(
                        measures[slot].mEcliptic, tilt, lat, measures[slot].mTarget));
            }
        }

        {
            const F32 LOW_BODY_SIN = 0.25f;
            // <SS:Nexii> A named sky stays where its side put it - a same-height squash would move it to the opposite side of noon. If BOTH skies are unnamed (azimuth-measured), a rising/setting squash only happens when the reference body cannot rotate them apart.
            auto measured_by_rule = [&skies](size_t slot)
            {
                return seedSkyHasNameHint(seedSkyNameHint(seedSkyLabel(skies, slot)));
            };

            for (size_t a = 0; a < count; ++a)
            {
                if (!skies.mSkies[a]) continue;
                if (llabs(measures[a].mSinElevation) > LOW_BODY_SIN) continue;

                for (size_t b = a + 1; b < count; ++b)
                {
                    if (!skies.mSkies[b]) continue;
                    if (llabs(measures[b].mSinElevation) > LOW_BODY_SIN) continue;
                    if (measured_by_rule(a) || measured_by_rule(b)) continue;

                    F64 gap = llabs(measured[a] - measured[b]);
                    gap = llmin(gap, 1.0 - gap);
                    if (gap >= 0.08) continue;

                    F64 rising = 0.0, setting = 0.0;
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(measures[a].mArc, measures[a].mSinElevation, true, rising);
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(measures[b].mArc, measures[b].mSinElevation, false, setting);

                    LL_INFOS("AtmoMagicEnv") << "Seed skies " << seedSkyLabel(skies, a) << " and "
                        << seedSkyLabel(skies, b) << " have their reference bodies at the same height; putting "
                        << seedSkyLabel(skies, a) << " on the rising side (" << rising << ") and "
                        << seedSkyLabel(skies, b) << " on the setting side (" << setting << ")" << LL_ENDL;

                    measured[a] = ss_atmoenv_snap_phase(rising);
                    measured[b] = ss_atmoenv_snap_phase(setting);
                }
            }
        }

        const F64 SEED_PHASE_MIN_GAP = 1.0 / (F64)SS_ATMOENV_PREVIEW_STEPS;

        std::vector<size_t> order;
        for (size_t slot = 0; slot < count; ++slot)
        {
            if (skies.mSkies[slot]) order.push_back(slot);
        }
        std::sort(order.begin(), order.end(),
                  [&measured](size_t a, size_t b) { return measured[a] < measured[b]; });

        for (size_t k = 1; k < order.size(); ++k)
        {
            const size_t prev = order[k - 1];
            const size_t here = order[k];
            const F64 gap = measured[here] - measured[prev];
            if (gap >= SEED_PHASE_MIN_GAP) continue;
            // <SS:Nexii> Never move an anchored sky off its pinned phase for another's sake; a side-named sky may be nudged off a tie - its height claim is the natural order anyway.
            const SeedSkyNameHint hint_here = seedSkyNameHint(seedSkyLabel(skies, here));
            if (hint_here.mAnchorPhase >= 0.0) continue;

            const F64 pushed = ss_atmoenv_snap_phase(measured[prev] + SEED_PHASE_MIN_GAP);
            LL_INFOS("AtmoMagicEnv") << "Seed skies " << seedSkyLabel(skies, prev) << " and "
                << seedSkyLabel(skies, here) << " measure to the same point in the cycle ("
                << measured[here] << "); moving the second to " << pushed
                << " so both survive" << LL_ENDL;
            measured[here] = pushed;
        }

        if (!order.empty())
        {
            const size_t last = order.back();
            // <SS:Nexii> An anchored sky stays on its pinned phase, not squeezed to the far side of the day to make room for a later dusk.
            const SeedSkyNameHint hint_last = seedSkyNameHint(seedSkyLabel(skies, last));
            if (hint_last.mAnchorPhase < 0.0)
            {
                measured[last] = ss_atmoenv_snap_phase(
                    llmin(measured[last], 1.0 - SEED_PHASE_MIN_GAP));
            }
        }

        out_phase = measured;
    }

    // <SS:Nexii> The disc faces the seeded cycle carries. A day cycle has ONE sun and ONE moon body, and a body's texture/scale are not keyframes - so the bodies translate from the skies where each stands highest: the day sky for the sun (the fully-lit sun is the cycle's face), the night sky for the moon (the visible body art). translateSettingsSky skips a sky whose disc equals EEP's stock value, leaving the standard body texture. Any adopted texture is reported into out_pads as (standard body index, texture) so the CALLER can derive its disc padding - there is no live-editor polling during a create, the wrote notecard must carry the padding (see writeSeededDefaultWithPads).
    void seedSkyDiscs(SSAtmoEnvTrack& track, const SeedSkyCollector& skies,
                      std::vector<std::pair<S32, LLUUID>>& out_pads)
    {
        std::vector<size_t> arrived;
        for (size_t slot = 0; slot < skies.mSkies.size(); ++slot)
        {
            if (skies.mSkies[slot]) arrived.push_back(slot);
        }
        if (arrived.empty()) return;

        auto highestBody = [&arrived, &skies](bool moon) -> const LLSettingsSky*
        {
            const LLSettingsSky* best = nullptr;
            F32 best_sin = -2.f;
            for (size_t slot : arrived)
            {
                const LLVector3 dir = seedSkyBodyDirection(*skies.mSkies[slot], moon);
                if (dir.mV[VZ] > best_sin)
                {
                    best_sin = dir.mV[VZ];
                    best = skies.mSkies[slot].get();
                }
            }
            return best;
        };

        const LLSettingsSky* sun_sky = highestBody(false);
        const LLSettingsSky* moon_sky = highestBody(true);

        const S32 standard_sun = track.mPlanetary.standardSunIndex();
        const S32 standard_moon = track.mPlanetary.standardMoonIndex();

        if (sun_sky)
        {
            const LLUUID adopted = sun_sky->getSunTextureId();
            track.mPlanetary.translateSettingsSky(*sun_sky, SS_SKY_IMPORT_SUN);
            if (standard_sun >= 0 && adopted.notNull()
                && standard_sun < (S32)track.mPlanetary.mBodies.size()
                && track.mPlanetary.mBodies[(size_t)standard_sun].mCustomTexture == adopted)
            {
                out_pads.emplace_back(standard_sun, adopted);
            }
        }
        if (moon_sky)
        {
            const LLUUID adopted = moon_sky->getMoonTextureId();
            track.mPlanetary.translateSettingsSky(*moon_sky, SS_SKY_IMPORT_MOON);
            if (standard_moon >= 0 && adopted.notNull()
                && adopted != LLSettingsSky::GetDefaultMoonTextureId()
                && standard_moon < (S32)track.mPlanetary.mBodies.size()
                && track.mPlanetary.mBodies[(size_t)standard_moon].mCustomTexture == adopted)
            {
                out_pads.emplace_back(standard_moon, adopted);
            }
        }
    }

    // <SS:Nexii> The seed look's cloud image. A seeded cycle starts on Layered Clouds only when no seed sky carried a cloud image of its own: stock EEP noise imports as null (the applier maps null back to the stock map), so an all-null field means nothing authored arrived and the Atmo seed look may claim it - but a sky authored with a custom cloud image must carry it through the create intact.
    void seedCloudNoiseFallback(SSAtmoEnvCloudDome& dome)
    {
        if (dome.mNoiseTexture.hasKeyframes())
        {
            for (const SSAtmoEnvKeyframe<LLUUID>& kf : dome.mNoiseTexture.keyframes())
            {
                if (kf.mValue.notNull()) return;
            }
        }
        else if (dome.mNoiseTexture.valueAt(0.0).notNull())
        {
            return;
        }
        dome.mNoiseTexture =
            SSAtmoEnvKeyframed<LLUUID>(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_LAYERED));
    }

    // The seeded default asset: a day cycle from whichever seed skies arrived, or plain defaults from none.
    SSAtmoEnvAsset buildSeededDefault(const SeedSkyCollector& skies,
                                      std::vector<std::pair<S32, LLUUID>>& out_pads)
    {
        SSAtmoEnvAsset def = SSAtmoEnvAsset::makeDefault();
        if (def.mTracks.empty()) return def;

        std::vector<size_t> arrived;
        for (size_t slot = 0; slot < skies.mSkies.size(); ++slot)
        {
            if (skies.mSkies[slot]) arrived.push_back(slot);
        }
        if (arrived.empty()) return def;

        SSAtmoEnvTrack& ground = def.mTracks[0];
        if (arrived.size() == 1)
        {
            const LLSettingsSky::ptr_t& sky = skies.mSkies[arrived[0]];
            ground.mAtmosphere.fromSettingsSky(*sky);
            ground.mCloudDome.fromSettingsSky(*sky);
            seedCloudNoiseFallback(ground.mCloudDome);
            seedSkyDiscs(ground, skies, out_pads);
            return def;
        }

        std::vector<F64> phase;
        // <SS:Nexii> Multi-sky seed: every supplied sky is stamped at its elevation-derived place in the cycle (see seedSkyPhases).
        seedSkyPhases(ground, skies, phase);

        for (size_t slot : arrived)
        {
            ground.mAtmosphere.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
            ground.mCloudDome.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
        }

        seedCloudNoiseFallback(ground.mCloudDome);

        // <SS:Nexii> The day and night faces: the sun body translates from the sky where the sun stands highest, the moon body likewise.
        seedSkyDiscs(ground, skies, out_pads);

        ground.mAtmosphere.collapseConstantKeyframes();
        ground.mCloudDome.collapseConstantKeyframes();
        return def;
    }

    // <SS:Nexii> Writes the seeded asset as a notecard AFTER its disc-padding derivations land. A create has no live editor to poll into - the notecard must carry the padding itself, or a glow-boarded EEP sun (authored huge to compensate for the embedded glow) is written full-bleed and looks wrong until manually re-derived. Cached textures resolve synchronously on the first attempt; one still decoding is re-checked on a short one-shot timer (0.1s cadence, ~3s patience) before the write, and any that never land are written at 0 padding (full-bleed) rather than holding the create hostage.
    using SeedWriteCallback = std::function<void(const LLUUID&, const LLUUID&, const SSAtmoEnvAsset&)>;

    void writeSeededDefaultWithPads(SSAtmoEnvAsset def,
                                    std::vector<std::pair<S32, LLUUID>> pad_requests,
                                    const LLUUID& parent_id,
                                    SeedWriteCallback on_created)
    {
        auto def_sp = std::make_shared<SSAtmoEnvAsset>(std::move(def));
        auto requests_sp = std::make_shared<std::vector<std::pair<S32, LLUUID>>>(std::move(pad_requests));

        // <SS:Nexii> The same SSAtmoDiscPadAuto gate every live-asset derivation obeys: off, the
        // seeded write skips the texture wait entirely and carries the translated (glow-inclusive)
        // diameter with 0 padding - a create must not analyse what the editor is set to leave alone.
        static LLCachedControl<bool> auto_pad(gSavedSettings, "SSAtmoDiscPadAuto", true);
        if (!auto_pad)
        {
            requests_sp->clear();
            writeDefaultNotecard(*def_sp, parent_id, on_created);
            return;
        }

        auto attempts_sp = std::make_shared<S32>(0);
        auto done_sp = std::make_shared<bool>(false);

        auto apply = [def_sp](S32 body_index, F32 padding)
        {
            SSAtmoEnvAsset& def = *def_sp;
            if (body_index < 0 || def.mTracks.empty()
                || body_index >= (S32)def.mTracks[0].mPlanetary.mBodies.size()) return;
            SSAtmoEnvCelestialBody& body = def.mTracks[0].mPlanetary.mBodies[(size_t)body_index];
            // The seeded track's discs are freshly translated - the diameter is still the EEP QUAD size (glow-inclusive); shrink it to the solid visible disc exactly once, the same first-derive rule the live editor follows (see ssdiscpad.cpp).
            if (body.mPadPendingTranslation)
            {
                body.mDiameterM *= llmax(1.f - 2.f * padding, 0.1f);
                body.mPadPendingTranslation = false;
            }
            body.mDiscPadding = padding;
        };

        auto settle = [def_sp, requests_sp, apply]() -> bool
        {
            for (auto it = requests_sp->begin(); it != requests_sp->end();)
            {
                F32 padding = 0.f;
                const SSDiscPadStatus status = ssDiscPadAnalyze(it->second, padding);
                LL_INFOS("AtmoMagicEnv") << "Seeded write pad settle body " << it->first
                                         << " texture " << it->second << ": status "
                                         << (S32)status << " padding " << padding << LL_ENDL;
                if (status == SSDiscPadStatus::LOADING)
                {
                    ++it;
                    continue;
                }
                apply(it->first, padding);
                it = requests_sp->erase(it);
            }
            return requests_sp->empty();
        };

        const S32 MAX_PAD_WRITE_ATTEMPTS = 30;

        // One-shot timer chain: each shot tries to settle; success or exhausted patience writes. The chain self-references through a shared std::function so the lambda stays valid past return; it CLEARS that shared slot the moment it writes, breaking the reference cycle so everything frees once the last timer is deleted.
        auto step_sp = std::make_shared<std::function<void()>>();
        *step_sp = [step_sp, def_sp, requests_sp, attempts_sp, done_sp, settle, apply,
                    parent_id, on_created]()
        {
            if (*done_sp) return;

            if (settle() || ++(*attempts_sp) >= MAX_PAD_WRITE_ATTEMPTS)
            {
                // Finishing: release the shared self-reference so the timers free entirely once this one-shot is deleted.
                *step_sp = std::function<void()>();

                const size_t still_pending = requests_sp->size();
                if (still_pending > 0)
                {
                    LL_WARNS("AtmoMagicEnv") << "Disc padding for " << still_pending
                        << " seeded face(s) never decoded; writing at 0 padding (full-bleed)"
                        << LL_ENDL;
                    for (const auto& req : *requests_sp)
                    {
                        apply(req.first, 0.f);
                    }
                    requests_sp->clear();
                }

                *done_sp = true;
                writeDefaultNotecard(*def_sp, parent_id, on_created);
                return;
            }
            LLEventTimer::run_after(0.1f, *step_sp);
        };

        if (settle())
        {
            *done_sp = true;
            writeDefaultNotecard(*def_sp, parent_id, on_created);
        }
        else
        {
            LLEventTimer::run_after(0.1f, *step_sp);
        }
    }

    // <SS:Nexii> The template's atmosphere columns as a TINT over the seeded cycle: each column divides its value out of the reference sky (Daylight - the "no mood" anchor the dial was tuned against) and the factor multiplies every stamped keyframe, so the archetype's mood rides the whole day - Alien World stays alien at dawn and dusk - instead of flattening the cycle to a constant.
    void tintSeededCycle(SSAtmoEnvTrack& track, const SSAtmoEnvTemplate& tmpl, const LLSettingsSky& reference)
    {
        const LLColor3 ref_horizon = reference.getBlueHorizon();
        const LLColor3 ref_density = reference.getBlueDensity();
        const LLColor3 horizon_factor(tmpl.mBlueHorizon.mV[0] / llmax(ref_horizon.mV[0], 0.01f),
                                      tmpl.mBlueHorizon.mV[1] / llmax(ref_horizon.mV[1], 0.01f),
                                      tmpl.mBlueHorizon.mV[2] / llmax(ref_horizon.mV[2], 0.01f));
        const LLColor3 density_factor(tmpl.mBlueDensity.mV[0] / llmax(ref_density.mV[0], 0.01f),
                                      tmpl.mBlueDensity.mV[1] / llmax(ref_density.mV[1], 0.01f),
                                      tmpl.mBlueDensity.mV[2] / llmax(ref_density.mV[2], 0.01f));
        const F32 haze_factor   = tmpl.mHazeDensity  / llmax(reference.getHazeDensity(), 0.001f);
        const F32 maxalt_factor = tmpl.mMaxAltitudeM / llmax(reference.getMaxY(), 1.f);
        const F32 cover_factor  = tmpl.mDomeCoverage / llmax(reference.getCloudShadow(), 0.01f);

        for (SSAtmoEnvKeyframe<LLColor3>& kf : track.mAtmosphere.mBlueHorizon.keyframes())
        {
            kf.mValue = LLColor3(kf.mValue.mV[0] * horizon_factor.mV[0],
                                 kf.mValue.mV[1] * horizon_factor.mV[1],
                                 kf.mValue.mV[2] * horizon_factor.mV[2]);
        }
        for (SSAtmoEnvKeyframe<LLColor3>& kf : track.mAtmosphere.mBlueDensity.keyframes())
        {
            kf.mValue = LLColor3(kf.mValue.mV[0] * density_factor.mV[0],
                                 kf.mValue.mV[1] * density_factor.mV[1],
                                 kf.mValue.mV[2] * density_factor.mV[2]);
        }
        for (SSAtmoEnvKeyframe<F32>& kf : track.mAtmosphere.mHazeDensity.keyframes())
        {
            kf.mValue = llmax(kf.mValue * haze_factor, 0.f);
        }
        for (SSAtmoEnvKeyframe<F32>& kf : track.mAtmosphere.mMaxAltitude.keyframes())
        {
            kf.mValue = llmax(kf.mValue * maxalt_factor, 1.f);
        }
        for (SSAtmoEnvKeyframe<F32>& kf : track.mCloudDome.mCoverage.keyframes())
        {
            kf.mValue = llclamp(kf.mValue * cover_factor, 0.f, 1.f);
        }
    }
}

// Creates a plain midday-defaults environment: no fetching, no seeding.
void SSAtmoEnvManager::createEmptyNotecard(const LLUUID& parent_id,
                                           std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
}

// Seeds a day cycle from a list of the author's own skies, the same measure-and-stamp algorithm the stock seed uses. An empty list makes the empty environment.
void SSAtmoEnvManager::createFromSkies(const std::vector<LLUUID>& sky_asset_ids,
                                       const std::vector<std::string>& sky_names,
                                       const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    if (sky_asset_ids.empty())
    {
        createEmptyNotecard(parent_id, on_created);
        return;
    }

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; creating Atmo v3 environment with built-in defaults instead of the chosen skies" << LL_ENDL;
        writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
        return;
    }

    fetchSeedSkies(sky_asset_ids, sky_names,
        [parent_id, on_created](const SeedSkyCollector& skies)
        {
            std::vector<std::pair<S32, LLUUID>> pads;
            SSAtmoEnvAsset def = buildSeededDefault(skies, pads);
            writeSeededDefaultWithPads(std::move(def), std::move(pads), parent_id, on_created);
        });
}

// The stock create path: the four shipped seed skies.
void SSAtmoEnvManager::createDefaultNotecard(const LLUUID& parent_id,
                                         std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    std::vector<LLUUID> ids;
    std::vector<std::string> names;
    ids.reserve(STOCK_SEED_SKY_COUNT);
    names.reserve(STOCK_SEED_SKY_COUNT);
    for (S32 slot = 0; slot < STOCK_SEED_SKY_COUNT; ++slot)
    {
        ids.push_back(LLUUID(STOCK_SEED_SKY_ID[slot]));
        names.push_back(STOCK_SEED_SKY_NAME[slot]);
    }

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; creating Atmo v3 environment with built-in defaults instead of the stock sky cycle" << LL_ENDL;
        writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
        return;
    }

    fetchSeedSkies(ids, names,
        [parent_id, on_created](const SeedSkyCollector& skies)
        {
            std::vector<std::pair<S32, LLUUID>> pads;
            SSAtmoEnvAsset def = buildSeededDefault(skies, pads);
            writeSeededDefaultWithPads(std::move(def), std::move(pads), parent_id, on_created);
        });
}

// The template seed: the template's world settings overwrite wholesale, and the track's sky reseeds as the stock four-sky day cycle with the template's atmosphere columns tinted over it (see the header note). Falls back to the plain constant template when the skies cannot be fetched.
void SSAtmoEnvManager::applyTemplateToTrack(SSAtmoEnvAsset& asset, S32 track_index, const std::string& key,
                                            std::function<void(bool success)> on_done)
{
    const SSAtmoEnvTemplate* tmpl = ssAtmoEnvFindTemplate(key);
    if (!tmpl || track_index < 0 || track_index >= static_cast<S32>(asset.mTracks.size()))
    {
        if (on_done) on_done(false);
        return;
    }

    SSAtmoEnvTrack& track = asset.mTracks[static_cast<size_t>(track_index)];

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; seeding the template with its constant sky instead of the stock day cycle" << LL_ENDL;
        ssAtmoEnvApplyTemplate(track, key);
        if (on_done) on_done(true);
        return;
    }

    std::vector<LLUUID> ids;
    std::vector<std::string> names;
    ids.reserve(STOCK_SEED_SKY_COUNT);
    names.reserve(STOCK_SEED_SKY_COUNT);
    for (S32 slot = 0; slot < STOCK_SEED_SKY_COUNT; ++slot)
    {
        ids.push_back(LLUUID(STOCK_SEED_SKY_ID[slot]));
        names.push_back(STOCK_SEED_SKY_NAME[slot]);
    }

    fetchSeedSkies(ids, names,
        [tmpl, track_index, &track, on_done, key](const SeedSkyCollector& skies)
        {
            ssAtmoEnvApplyTemplateWorld(track, *tmpl);

            std::vector<size_t> arrived;
            for (size_t slot = 0; slot < skies.mSkies.size(); ++slot)
            {
                if (skies.mSkies[slot]) arrived.push_back(slot);
            }

            if (arrived.empty())
            {
                // No seed sky arrived: the template's constant sky is all there is.
                ssAtmoEnvApplyTemplate(track, key);
                if (on_done) on_done(true);
                return;
            }

            std::vector<F64> phase;
            seedSkyPhases(track, skies, phase);

            for (size_t slot : arrived)
            {
                track.mAtmosphere.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
                track.mCloudDome.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
            }

            seedCloudNoiseFallback(track.mCloudDome);

            // The tint reference: the Daylight slot when it arrived, the first arrival otherwise.
            const LLSettingsSky& reference =
                *(skies.mSkies[0] ? skies.mSkies[0] : skies.mSkies[arrived.front()]);

            tintSeededCycle(track, *tmpl, reference);

            // The day and night faces, from the stock seed skies (see seedSkyDiscs).
            std::vector<std::pair<S32, LLUUID>> pads;
            seedSkyDiscs(track, skies, pads);
            // <SS:Nexii> The template edits a LIVE asset, so disc-padding derivations run through the editor's own poll (ssDiscPadPoll) - the standard-body ids only map to this one track, and the writes stay gated by the ssDiscPadAuto setting.
            for (const auto& pad : pads)
            {
                ssDiscPadAutoDerive(track_index, pad.first, pad.second);
            }

            track.mAtmosphere.collapseConstantKeyframes();
            track.mCloudDome.collapseConstantKeyframes();

            if (on_done) on_done(true);
        });
}

// Maps an EEP day cycle over: every sky keyframe on its ground-level track stamps into the ground track at the day cycle's own keyframe time, so the authored timings carry across. The skies' noise textures are kept (no layered-noise override) - an authored asset, not a seed.
void SSAtmoEnvManager::createFromDayCycle(const LLUUID& day_cycle_asset_id,
                                          const LLUUID& parent_id,
                                          std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    if (!gAssetStorage || day_cycle_asset_id.isNull())
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable or no day cycle given; creating Atmo v3 environment with built-in defaults" << LL_ENDL;
        createEmptyNotecard(parent_id, on_created);
        return;
    }

    LLSettingsVOBase::getSettingsAsset(day_cycle_asset_id,
        [parent_id, on_created](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
        {
            LLSettingsDay::ptr_t day;
            if (!status && settings)
            {
                day = std::dynamic_pointer_cast<LLSettingsDay>(settings);
            }
            if (!day)
            {
                LL_WARNS("AtmoMagicEnv") << "Could not fetch day cycle " << asset_id
                                         << " (status " << status
                                         << "); creating an empty Atmo v3 environment instead" << LL_ENDL;
                writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
                return;
            }

            SSAtmoEnvAsset def = SSAtmoEnvAsset::makeDefault();
            if (def.mTracks.empty())
            {
                writeDefaultNotecard(def, parent_id, on_created);
                return;
            }

            const LLSettingsDay::CycleTrack_t& frames =
                day->getCycleTrackConst(LLSettingsDay::TRACK_GROUND_LEVEL);
            if (frames.empty())
            {
                LL_WARNS("AtmoMagicEnv") << "Day cycle " << asset_id
                                         << " has no sky keyframes; creating an empty Atmo v3 environment instead" << LL_ENDL;
                writeDefaultNotecard(def, parent_id, on_created);
                return;
            }

            SSAtmoEnvTrack& ground = def.mTracks[0];
            if (frames.size() == 1)
            {
                LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(frames.begin()->second);
                if (sky)
                {
                    ground.mAtmosphere.fromSettingsSky(*sky);
                    ground.mCloudDome.fromSettingsSky(*sky);
                }
            }
            else
            {
                for (const LLSettingsDay::CycleTrack_t::value_type& frame : frames)
                {
                    LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(frame.second);
                    if (!sky) continue;

                    const F64 phase = ss_atmoenv_snap_phase((F64)frame.first);
                    ground.mAtmosphere.addKeyframesFromSky(*sky, phase);
                    ground.mCloudDome.addKeyframesFromSky(*sky, phase);
                }
            }

            // <SS:Nexii> The day cycle's sun and moon faces: the highest sun across the cycle seeds the sun body's texture, the highest moon the moon body's; the adopted textures feed the same disc-padding derivation and deferred notecard write as the sky-seeded create (see writeSeededDefaultWithPads).
            std::vector<std::pair<S32, LLUUID>> pads;

            const LLSettingsSky* day_sun_sky = nullptr;
            const LLSettingsSky* day_moon_sky = nullptr;
            F32 sun_sin = -2.f, moon_sin = -2.f;
            for (const LLSettingsDay::CycleTrack_t::value_type& frame : frames)
            {
                const LLSettingsSky* sky = dynamic_cast<const LLSettingsSky*>(frame.second.get());
                if (!sky) continue;

                const F32 s = seedSkyBodyDirection(*sky, false).mV[VZ];
                const F32 m = seedSkyBodyDirection(*sky, true).mV[VZ];
                if (s > sun_sin) { sun_sin = s; day_sun_sky = sky; }
                if (m > moon_sin) { moon_sin = m; day_moon_sky = sky; }
            }

            const S32 standard_sun = ground.mPlanetary.standardSunIndex();
            const S32 standard_moon = ground.mPlanetary.standardMoonIndex();

            if (day_sun_sky)
            {
                const LLUUID adopted = day_sun_sky->getSunTextureId();
                ground.mPlanetary.translateSettingsSky(*day_sun_sky, SS_SKY_IMPORT_SUN);
                if (adopted.notNull() && standard_sun >= 0
                    && standard_sun < (S32)ground.mPlanetary.mBodies.size()
                    && ground.mPlanetary.mBodies[(size_t)standard_sun].mCustomTexture == adopted)
                {
                    pads.emplace_back(standard_sun, adopted);
                }
            }
            if (day_moon_sky)
            {
                const LLUUID adopted = day_moon_sky->getMoonTextureId();
                ground.mPlanetary.translateSettingsSky(*day_moon_sky, SS_SKY_IMPORT_MOON);
                if (adopted.notNull() && adopted != LLSettingsSky::GetDefaultMoonTextureId()
                    && standard_moon >= 0
                    && standard_moon < (S32)ground.mPlanetary.mBodies.size()
                    && ground.mPlanetary.mBodies[(size_t)standard_moon].mCustomTexture == adopted)
                {
                    pads.emplace_back(standard_moon, adopted);
                }
            }

            ground.mAtmosphere.collapseConstantKeyframes();
            ground.mCloudDome.collapseConstantKeyframes();

            writeSeededDefaultWithPads(std::move(def), std::move(pads), parent_id, on_created);
        });
}

// An EEP water preset onto the ground track of the empty environment: midday defaults plus the preset's water block. Fetched, classified, stamped synchronously - only the block's own fields move, so the defaults' plane stays enabled at its default height.
void SSAtmoEnvManager::createFromWater(const LLUUID& water_asset_id,
                                       const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    if (!gAssetStorage || water_asset_id.isNull())
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable or no water preset given; creating the plain Atmo v3 defaults" << LL_ENDL;
        createEmptyNotecard(parent_id, on_created);
        return;
    }

    LLSettingsVOBase::getSettingsAsset(water_asset_id,
        [parent_id, on_created](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
        {
            LLSettingsWater::ptr_t water;
            if (!status && settings)
            {
                water = std::dynamic_pointer_cast<LLSettingsWater>(settings);
            }
            if (!water)
            {
                LL_WARNS("AtmoMagicEnv") << "Could not fetch water preset " << asset_id
                                         << " (status " << status
                                         << "); creating the plain Atmo v3 defaults instead" << LL_ENDL;
                writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
                return;
            }

            SSAtmoEnvAsset def = SSAtmoEnvAsset::makeDefault();
            if (def.mTracks.empty())
            {
                writeDefaultNotecard(def, parent_id, on_created);
                return;
            }

            SSAtmoEnvTrack& ground = def.mTracks[0];
            ground.mWater.mEnabled = true;
            ground.mWater.fromSettingsWater(*water);

            writeDefaultNotecard(def, parent_id, on_created);
        });
}

// The loaded-environment multi-sky stamp: every dropped sky lands at the measured phase the seeding derives (see seedSkyPhases) and stamps the whole field grouping, so the drop becomes a day cycle on the selected track, not a one-point import. Mirrors applyTemplateToTrack - fetch, measure, stamp, then derive the adopted discs' padding.
void SSAtmoEnvManager::stampSkiesOnTrack(SSAtmoEnvAsset& asset, S32 track_index,
                                         const std::vector<LLUUID>& sky_asset_ids,
                                         const std::vector<std::string>& sky_names,
                                         std::function<void(bool success)> on_done)
{
    if (sky_asset_ids.empty()
        || track_index < 0 || track_index >= static_cast<S32>(asset.mTracks.size()))
    {
        if (on_done) on_done(false);
        return;
    }

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; could not stamp dropped skies onto the track" << LL_ENDL;
        if (on_done) on_done(false);
        return;
    }

    SSAtmoEnvTrack& track = asset.mTracks[static_cast<size_t>(track_index)];

    fetchSeedSkies(sky_asset_ids, sky_names,
        [&track, track_index, on_done](const SeedSkyCollector& skies)
        {
            std::vector<size_t> arrived;
            for (size_t slot = 0; slot < skies.mSkies.size(); ++slot)
            {
                if (skies.mSkies[slot]) arrived.push_back(slot);
            }

            if (arrived.empty())
            {
                LL_WARNS("AtmoMagicEnv") << "None of the dropped skies could be fetched; nothing was stamped" << LL_ENDL;
                if (on_done) on_done(false);
                return;
            }

            std::vector<F64> phase;
            seedSkyPhases(track, skies, phase);

            for (size_t slot : arrived)
            {
                track.mAtmosphere.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
                track.mCloudDome.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
                track.mPlanetary.translateSettingsSky(*skies.mSkies[slot], SS_SKY_IMPORT_ALL);
            }

            std::vector<std::pair<S32, LLUUID>> pads;
            seedSkyDiscs(track, skies, pads);
            for (const auto& pad : pads)
            {
                ssDiscPadAutoDerive(track_index, pad.first, pad.second);
            }

            track.mAtmosphere.collapseConstantKeyframes();
            track.mCloudDome.collapseConstantKeyframes();

            if (on_done) on_done(true);
        });
}

// Takes ownership of a just-created notecard as the live asset.
void SSAtmoEnvManager::adoptCreated(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)
{
    mItemID = item_id;
    mAssetID = asset_id;
    mBaseline = asset;

    mFromParcel = false;
    mWorking = asset;
    mHasAsset = true;
    mStatus = "Ready.";
}

// Writes the working asset as a NEW notecard item.
void SSAtmoEnvManager::saveNotecard(const std::string& name)
{
    if (!mHasAsset) return;

    std::string save_name = name;
    LLStringUtil::trim(save_name);
    if (save_name.empty()) save_name = "Atmo Environment";

    mWorking.mName = save_name;

    // Self-containment is a save-time property: whatever the keyframes name travels with the asset.
    ssAtmoEnvEmbedReferencedPrecipTypes(mWorking);

    const SSAtmoEnvAsset safe_working = ss_atmo_env_sanitize(mWorking);
    mWorking = safe_working;
    mBaseline = safe_working;

    if (mItemID.notNull())
    {
        updateExistingNotecard(save_name);
        return;
    }

    atmoFolderId([this, safe_working, save_name](const LLUUID& folder_id)
    {
        writeAssetAsNotecard(safe_working, save_name, folder_id,
            [this](const LLUUID& item_id, const LLUUID& asset_id)
            {
                if (item_id.notNull()) mItemID = item_id;
                if (asset_id.notNull()) mAssetID = asset_id;
            });
    });
}

// Overwrites the loaded notecard item's asset in place.
void SSAtmoEnvManager::updateExistingNotecard(const std::string& name)
{
    const LLUUID item_id = mItemID;
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (item && item->getName() != name
        && gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE))
    {
        LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
        new_item->rename(name);
        new_item->updateServer(false);
        gInventory.updateItem(new_item);
        gInventory.notifyObservers();
    }

    std::string env_text;
    std::string error;
    if (!ss_atmo_env_to_notecard_text(mWorking, env_text, error))
    {
        LL_WARNS("AtmoMagicEnv") << "Could not serialize Atmo v3 environment '" << name
                                 << "': " << error << LL_ENDL;
        return;
    }
    LLNotecard nc(LLNotecard::MAX_SIZE);
    nc.setText(env_text);
    std::ostringstream wrapped;
    nc.exportStream(wrapped);

    LLViewerRegion* region = gAgent.getRegion();
    const std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
    if (url.empty())
    {
        LL_WARNS("AtmoMagicEnv") << "No UpdateNotecardAgentInventory capability; could not update Atmo v3 notecard in place" << LL_ENDL;
        return;
    }

    LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
        item_id, LLAssetType::AT_NOTECARD, wrapped.str(),
        [name](LLUUID, LLUUID new_asset_id, LLUUID, LLSD)
        {
            LL_INFOS("AtmoMagicEnv") << "Updated Atmo v3 environment '" << name
                                     << "' in place as asset " << new_asset_id << LL_ENDL;
            SSAtmoEnvManager::getInstance()->mAssetID = new_asset_id;
        },
        nullptr);
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

// Loads a notecard item's asset, with an optional completion callback.
bool SSAtmoEnvManager::loadFromInventory(const LLInventoryItem* item, std::function<void(bool)> on_complete)
{
    if (!item || item->getAssetUUID().isNull()) return false;

    if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE)
        && !gAgent.isGodlike())
    {
        mStatus = "no permission to read that notecard";
        return false;
    }

    mPendingID = item->getAssetUUID();
    mPendingItemID = item->getUUID();
    mStatus = "loading environment...";
    mLoadCompleteCallback = on_complete;

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        finishLoad(false);
        return false;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoEnvManager::onAssetLoaded, nullptr, true);
    return true;
}

// Fires and clears the pending load callback.
void SSAtmoEnvManager::finishLoad(bool success)
{
    std::function<void(bool)> cb;
    cb.swap(mLoadCompleteCallback);
    if (cb) cb(success);
}

// Requests the notecard asset body from the asset system.
void SSAtmoEnvManager::loadFromAssetId(const LLUUID& asset_id)
{
    mPendingID = asset_id;
    mPendingItemID.setNull();
    mStatus = "loading environment...";

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        return;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoEnvManager::onAssetLoaded, nullptr, true);
}

// Asset arrival: unwrap the notecard, parse, adopt; failures leave the current state untouched.
void SSAtmoEnvManager::onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status)
{
    SSAtmoEnvManager* self = SSAtmoEnvManager::getInstance();

    if (asset_id != self->mPendingID) return;
    self->mPendingID.setNull();

    if (status != 0)
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment notecard " << asset_id
                                 << " failed to load, status " << status << LL_ENDL;
        self->mStatus = "notecard unavailable";
        self->finishLoad(false);
        return;
    }

    LLFileSystem file(asset_id, type, LLFileSystem::READ);
    const S32 length = file.getSize();
    if (length <= 0)
    {
        self->mStatus = "notecard empty";
        self->finishLoad(false);
        return;
    }

    std::vector<char> buffer(length + 1);
    file.read((U8*)buffer.data(), length);
    buffer[length] = '\0';

    std::string text(buffer.data(), length);
    if (length > 19 && strncmp(buffer.data(), "Linden text version", 19) == 0)
    {
        LLNotecard notecard;
        std::istringstream stream(text);
        if (!notecard.importStream(stream))
        {
            LL_WARNS("AtmoMagicEnv") << "Could not parse Atmo v3 notecard " << asset_id << LL_ENDL;
            self->mStatus = "notecard unreadable";
            self->finishLoad(false);
            return;
        }
        text = notecard.getText();
    }

    self->mAssetID = asset_id;
    self->mItemID = self->mPendingItemID;
    self->mPendingItemID.setNull();
    self->finishLoad(self->applyNotecardText(text, false));
}

// Parses notecard text and adopts it as the live asset.
bool SSAtmoEnvManager::applyNotecardText(const std::string& text, bool /*from_inventory_permission_check*/)
{
    LLSD sd;
    std::string error;
    if (!ss_atmo_env_from_notecard_text(text, sd, error))
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment notecard rejected: " << error << LL_ENDL;
        mStatus = "notecard is not valid LLSD";
        return false;
    }

    return adoptParsedAsset(sd);
}

// Adopts an already-parsed external document (Bridge fetch path).
bool SSAtmoEnvManager::applyExternalLLSD(const LLUUID& source_id, const LLSD& sd)
{
    mAssetID = source_id;
    mItemID.setNull();
    return adoptParsedAsset(sd);
}

// Adopts external notecard text (parcel discovery path).
bool SSAtmoEnvManager::applyExternalNotecardText(const LLUUID& source_id, const std::string& text)
{
    mAssetID = source_id;
    mItemID.setNull();
    return applyNotecardText(text, false);
}

// Drops the asset and restores stock EEP behaviour.
void SSAtmoEnvManager::unload()
{
    if (!mHasAsset) return;

    LL_INFOS("AtmoMagicEnv") << "Unloading the Atmo Magic environment; the world"
                                " falls back to the parcel or region setting" << LL_ENDL;

    mHasAsset = false;
    mWorking = SSAtmoEnvAsset();
    mBaseline = SSAtmoEnvAsset();
    mSourceAssetId.setNull();
    mFromParcel = false;
    SSPrecipPresetManager::instance().clearEnvironmentPresets();
    clearPreviewPhaseOverride();
}

// The one adoption point: validate, install, and set the modification baseline.
bool SSAtmoEnvManager::adoptParsedAsset(const LLSD& sd)
{
    SSAtmoEnvAsset parsed_asset;
    std::string error;
    if (!parsed_asset.fromLLSD(sd, error))
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment notecard rejected: " << error << LL_ENDL;
        mStatus = "notecard invalid: " + error;
        return false;
    }

    mBaseline = parsed_asset;
    mWorking = parsed_asset;
    mHasAsset = true;
    mStatus = "Ready.";
    // The environment's own precipitation types have to be live before anything resolves one.
    ssAtmoEnvStagePrecipTypes(mWorking);
    return true;
}