/**
 * @file ssatmoenvdiscovery.h
 * @brief Atmo Magic: parcel discovery and notecard fetch.
 *
 *        Fetch is a plain HTTP round trip through the existing Bridge
 *        plumbing (FSLSLBridge::viewerToLSL), not a chat-chunked relay -
 *        see indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt's
 *        "FetchNotecard" command for the LSL side: it reads the notecard
 *        synchronously (llGetNotecardLineSync, no object-inventory step
 *        needed) and replies over llHTTPResponse with the notecard body,
 *        passed straight through as the response's own LLSD document when
 *        it already starts with "<llsd>" (a hand-authored card), and
 *        otherwise wrapped in an LLSD string - so a card saved in the
 *        compressed SS-ATMO-ENV-COMPRESSED format arrives as plain text
 *        and is decoded by the shared ss_atmo_env_from_notecard_text
 *        reader, no text reassembly step of any kind.
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

#ifndef SS_ATMOENVDISCOVERY_H
#define SS_ATMOENVDISCOVERY_H

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "llviewerparcelmgr.h"

#include <string>

class SSAtmoEnvDiscoveryManager : public LLSingleton<SSAtmoEnvDiscoveryManager>, public LLParcelObserver
{
    LLSINGLETON(SSAtmoEnvDiscoveryManager);
    ~SSAtmoEnvDiscoveryManager();

public:
    void changed() override;

    static bool editorIsOpen();

    static LLUUID parseDescription(const std::string& desc);

    // The asset id the current parcel advertises in its description; the floater's Load From Parcel button keys its enabled state off this.
    static LLUUID parcelAssetId();

    // The explicit load, from the floater's Load From Parcel button: applies while the editor floater is open (the user asked from inside the editor) and lifts a decline recorded by an earlier Unload - the click is the user changing their mind.
    bool loadFromParcel();

private:
    void requestFetch(const LLUUID& asset_id, bool force = false);

    void onFetchResult(const LLUUID& asset_id, const LLSD& data, bool force);

    bool applyText(const LLUUID& asset_id, const std::string& text, bool force);

    LLUUID mAppliedAssetId;
    LLUUID mPendingAssetId;

    // The parcel environment the user unloaded by hand while the parcel still advertises it. Holds the unload down until the tag disappears or the parcel advertises a different environment - otherwise the next parcel property update re-applies the cached notecard and the weather, wind and rain beds come straight back mid-session.
    LLUUID mDeclinedAssetId;
};

#endif
