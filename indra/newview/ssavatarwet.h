/**
 * @file ssavatarwet.h
 * @brief Atmo Magic: per-avatar wetness.
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

#ifndef SS_AVATARWET_H
#define SS_AVATARWET_H

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <vector>

class LLGLSLShader;

class SSAvatarWet : public LLSingleton<SSAvatarWet>
{
    LLSINGLETON_EMPTY_CTOR(SSAvatarWet);

public:
    void idle(F32 dt);

    void clear();

    struct Capsule
    {
        LLVector3 mFootAgent;
        F32 mHeight = 1.8f;
        F32 mRadius = 0.45f;
        F32 mSoak = 0.f;
    };

    static const S32 MAX_SHADED = 8;
    const std::vector<Capsule>& shaded() const { return mShaded; }

    bool bindForShader(LLGLSLShader& shader) const;

    S32 trackedCount() const { return (S32)mAvatars.size(); }
    F32 peakSoak() const { return mPeakSoak; }

private:
    struct State
    {
        F32 mSoak = 0.f;
        F32 mExposure = 0.f;
        F64 mLastSeen = 0.0;
    };

    F32 exposureAt(const LLVector3& foot_agent, F32 height) const;

    std::map<LLUUID, State> mAvatars;
    std::vector<Capsule> mShaded;
    F32 mPeakSoak = 0.f;
};

#endif
