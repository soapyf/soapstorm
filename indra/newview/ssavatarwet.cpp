/**
 * @file ssavatarwet.cpp
 * @brief See ssavatarwet.h.
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

#include "ssavatarwet.h"

#include "ssatmomagic.h"
#include "ssprecippreset.h"
#include "sssurfacefield.h"

#include "llcharacter.h"
#include "llglslshader.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <algorithm>

namespace
{
    const F32 TRACK_RADIUS = 96.f;

    const F64 FORGET_SECONDS = 120.0;

    const F32 SOAK_SECONDS = 45.f;
    const F32 DRY_SECONDS  = 240.f;

    const F32 EXPOSURE_TAU = 1.5f;

    const F32 COVER_CLEAR = 0.35f;
    const F32 COVER_FADE  = 1.25f;
}

// Drops all tracked wetness - the off switch's reset.
void SSAvatarWet::clear()
{
    mAvatars.clear();
    mShaded.clear();
    mPeakSoak = 0.f;
}

// How exposed to rain a standing avatar is: 1 in the open, fading to 0 as the surface field's cover closes overhead.
F32 SSAvatarWet::exposureAt(const LLVector3& foot_agent, F32 height) const
{
    const SSSurfaceField::Sample sample = SSSurfaceField::getInstance()->sample(foot_agent);

    if (!sample.mValid) return 1.f;

    const F32 head_z = foot_agent.mV[VZ] + height;
    const F32 above = sample.mSurfaceZ - head_z;

    if (above <= COVER_CLEAR) return 1.f;
    if (above >= COVER_FADE) return 0.f;
    return 1.f - (above - COVER_CLEAR) / (COVER_FADE - COVER_CLEAR);
}

// Per-frame soak/dry integration for avatars near the camera, keeping the nearest few as shader capsules.
void SSAvatarWet::idle(F32 dt)
{
    mShaded.clear();

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo || dt <= 0.f) return;

    static LLCachedControl<F32> enabled(gSavedSettings, "SSAtmoWetStrength", 1.f);
    if ((F32)enabled <= 0.f)
    {
        clear();
        return;
    }

    const SSPrecipPreset& preset = atmo->preset();
    const F32 intensity = (atmo->hasWeather() && preset.mWetRate > 0.f)
        ? llclamp(atmo->precipitation(), 0.f, 1.f) : 0.f;

    const F64 now = atmo->sharedTime();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    mPeakSoak = 0.f;

    std::vector<std::pair<F32, Capsule> > candidates;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead()) continue;

        if (avatar->isControlAvatar()) continue;

        const LLVector3 pos = avatar->getPositionAgent();
        const F32 dist_sq = (pos - cam).magVecSquared();
        if (dist_sq > TRACK_RADIUS * TRACK_RADIUS) continue;

        const F32 height = llclamp(avatar->mBodySize.mV[VZ], 0.4f, 4.f);
        const F32 girth = llmax(avatar->mBodySize.mV[VX], avatar->mBodySize.mV[VY]);

        LLVector3 foot = pos;
        foot.mV[VZ] -= avatar->getPelvisToFoot();

        const F32 target = exposureAt(foot, height);

        auto found = mAvatars.find(avatar->getID());
        const bool first_sight = (found == mAvatars.end());

        State& state = mAvatars[avatar->getID()];
        state.mLastSeen = now;

        if (first_sight)
        {
            state.mExposure = target;
            state.mSoak = llclamp(target * intensity, 0.f, 1.f);
        }
        else
        {
            state.mExposure += (target - state.mExposure)
                * llclamp(dt / EXPOSURE_TAU, 0.f, 1.f);
        }

        const F32 drive = state.mExposure * intensity;
        if (drive > 0.f)
        {
            const F32 rate = drive / SOAK_SECONDS;
            state.mSoak += (1.f - state.mSoak) * llclamp(rate * dt, 0.f, 1.f);
        }
        else if (state.mSoak > 0.f)
        {
            state.mSoak -= state.mSoak * llclamp(dt / DRY_SECONDS, 0.f, 1.f);
            if (state.mSoak < 0.002f) state.mSoak = 0.f;
        }

        mPeakSoak = llmax(mPeakSoak, state.mSoak);
        if (state.mSoak <= 0.004f) continue;

        Capsule cap;
        cap.mFootAgent = foot;
        cap.mHeight = height;
        cap.mRadius = llclamp(girth * 0.75f, 0.3f, 0.9f);
        cap.mSoak = state.mSoak;

        candidates.push_back(std::make_pair(dist_sq, cap));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const std::pair<F32, Capsule>& a, const std::pair<F32, Capsule>& b)
              { return a.first < b.first; });

    for (size_t i = 0; i < candidates.size() && (S32)i < MAX_SHADED; ++i)
    {
        mShaded.push_back(candidates[i].second);
    }

    for (auto it = mAvatars.begin(); it != mAvatars.end(); )
    {
        it = (now - it->second.mLastSeen > FORGET_SECONDS) ? mAvatars.erase(it) : std::next(it);
    }
}

// Uploads rain direction and the shaded avatar capsules; false when nothing is wet enough to matter.
bool SSAvatarWet::bindForShader(LLGLSLShader& shader) const
{
    static LLStaticHashedString rain_dir("ssRainDir");
    static LLStaticHashedString avatar_count("ssAvatarCount");
    static LLStaticHashedString avatar_pos("ssAvatarPos");
    static LLStaticHashedString avatar_shape("ssAvatarShape");

    LLVector3 dir = SSAtmoMagic::getInstance()->rainDirection();
    if (dir.normVec() < 0.001f) dir.setVec(0.f, 0.f, -1.f);
    shader.uniform3fv(rain_dir, 1, dir.mV);

    const S32 count = llmin((S32)mShaded.size(), MAX_SHADED);
    shader.uniform1i(avatar_count, count);
    if (count <= 0) return false;

    F32 pos[MAX_SHADED * 4];
    F32 shape[MAX_SHADED * 4];
    for (S32 i = 0; i < count; ++i)
    {
        const Capsule& cap = mShaded[(size_t)i];
        pos[i * 4 + 0] = cap.mFootAgent.mV[VX];
        pos[i * 4 + 1] = cap.mFootAgent.mV[VY];
        pos[i * 4 + 2] = cap.mFootAgent.mV[VZ];
        pos[i * 4 + 3] = cap.mRadius;

        shape[i * 4 + 0] = cap.mHeight;
        shape[i * 4 + 1] = cap.mSoak;
        shape[i * 4 + 2] = 0.f;
        shape[i * 4 + 3] = 0.f;
    }

    shader.uniform4fv(avatar_pos, count, pos);
    shader.uniform4fv(avatar_shape, count, shape);
    return true;
}
