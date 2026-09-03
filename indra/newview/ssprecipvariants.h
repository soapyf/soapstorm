/**
 * @file ssprecipvariants.h
 * @brief Atmo Magic: procedural particle textures.
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

#ifndef SS_PRECIPVARIANTS_H
#define SS_PRECIPVARIANTS_H

#include "ssprecipitation.h"

#include "llpointer.h"
#include "llsingleton.h"

#include <map>

class LLViewerTexture;

class SSPrecipVariants : public LLSingleton<SSPrecipVariants>
{
    LLSINGLETON_EMPTY_CTOR(SSPrecipVariants);

public:
    static const U32 VARIANT_COUNT = 8;

    LLViewerTexture* get(const SSPrecipPreset& preset, SSPrecipTier tier, U32 variant,
                         LLViewerTexture* custom_drop = nullptr);

    void splatInflation(const SSPrecipPreset& preset, SSPrecipTier tier,
                        F32& scale_x, F32& scale_y);

    enum EUtility
    {
        UTIL_RING = 0,
        UTIL_DOT,
        UTIL_SHARD,
        UTIL_PUFF,
        UTIL_PLUME
    };
    LLViewerTexture* utility(EUtility kind);

    void clearCache() { mCache.clear(); }

private:
    LLPointer<LLViewerTexture> build(const SSPrecipPreset& preset, SSPrecipTier tier, U32 variant);
    LLPointer<LLViewerTexture> bakeFromCustom(const SSPrecipPreset& preset, SSPrecipTier tier,
                                              U32 variant, LLViewerTexture* custom_drop);
    LLPointer<LLViewerTexture> buildUtility(EUtility kind);

    std::map<U64, LLPointer<LLViewerTexture>> mCache;
};

#endif
