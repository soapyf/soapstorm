/**
 * @file ssfloateratmoinfluence.h
 * @brief Atmo Magic: weather influence sub-floater.
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

#ifndef SS_FLOATERATMOINFLUENCE_H
#define SS_FLOATERATMOINFLUENCE_H

#include "llfloater.h"

#include <functional>
#include <string>
#include <vector>

struct SSAtmoEnvWeatherInfluence;

class SSFloaterAtmoInfluence : public LLFloater
{
public:
    SSFloaterAtmoInfluence(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    void setTrack(S32 index);

private:
    struct Row
    {
        std::string mPrefix;
        std::function<bool&(SSAtmoEnvWeatherInfluence&)> mEnabled;
        std::function<F32&(SSAtmoEnvWeatherInfluence&)> mStrength;
        std::function<F32()> mEffect;
    };

    std::vector<Row> mRows;

    void buildRows();

    bool influence(SSAtmoEnvWeatherInfluence** out) const;

    void refreshAll();
    void refreshReadouts();

    void onCommitMaster();
    void onCommitRow(const Row& row);
    void onClickReset();

    S32 mTrackIndex = 0;
    F64 mLastPoll = 0.0;
};

#endif
