/**
 * @file ssgpucull.cpp
 * @brief See ssgpucull.h.
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

#include "ssgpucull.h"

#include "llcamera.h"
#include "llgl.h"
#include "llglheaders.h"
#include "pipeline.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "ssglreadback.h"

#include "glm/glm.hpp"
#include "glm/gtc/type_ptr.hpp"

extern bool gCubeSnapshot;

// <SS:Nexii> Candidate caps: one RGBA texel pair per group. 16384 wide x 4 rows = 32768 boxes; the visibility texture is one R8 byte each. The AABB upload rows and the readback are only a few KB regardless of scene size.
static constexpr S32 SS_MAX_GROUPS = 32768;
static constexpr S32 SS_BOX_TEX_W = 16384;
static constexpr S32 SS_BOX_TEX_H = SS_MAX_GROUPS / (SS_BOX_TEX_W / 2);

static GLint uniformLoc(LLGLSLShader& shader, const char* name)
{
    return glGetUniformLocation(shader.mProgramObject, name);
}

// LLMatrix4 is row-major, glm is column-major - build the transpose.
static glm::mat4 toGlm(const LLMatrix4& m)
{
    return glm::mat4(m.mMatrix[0][0], m.mMatrix[1][0], m.mMatrix[2][0], m.mMatrix[3][0],
                     m.mMatrix[0][1], m.mMatrix[1][1], m.mMatrix[2][1], m.mMatrix[3][1],
                     m.mMatrix[0][2], m.mMatrix[1][2], m.mMatrix[2][2], m.mMatrix[3][2],
                     m.mMatrix[0][3], m.mMatrix[1][3], m.mMatrix[2][3], m.mMatrix[3][3]);
}

SSGPUCull::SSGPUCull()
{
}

SSGPUCull::~SSGPUCull()
{
    shutdownGL();
}

void SSGPUCull::shutdownGL()
{
    if (mBoxTex) glDeleteTextures(1, &mBoxTex);
    if (mVisTex) glDeleteTextures(1, &mVisTex);
    if (mHiZTex) glDeleteTextures(1, &mHiZTex);
    mBoxTex = mVisTex = mHiZTex = 0;
}

bool SSGPUCull::supported()
{
#if LL_DARWIN
    return false;
#else
    if (gGLManager.mGLVersion < 4.29f) return false;
#if LL_WINDOWS
    if (glDispatchCompute == nullptr) return false;
#endif
    return gSSCullProgram.mProgramObject != 0 && gSSHiZProgram.mProgramObject != 0;
#endif
}

bool SSGPUCull::active()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSComputeCulling", false);
    if (!enabled || !supported()) return false;
    if (gCubeSnapshot
        || LLPipeline::sShadowRender
        || LLPipeline::sReflectionRender
        || LLPipeline::sRenderingHUDs)
    {
        return false;
    }
    return LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD;
}

void SSGPUCull::beginFrame()
{
    mBuild = std::make_shared<GateFrame>();
}

S32 SSGPUCull::registerGroup(LLSpatialGroup* group)
{
    if (!mBuild || !active() || group->isDead() || group->isEmpty()) return -1;
    const S32 id = (S32)mBuild->mGroups.size();
    if (id >= SS_MAX_GROUPS) return -1;

    // Group extents are agent-space for the world camera cull path (the same
    // boxes LLOctreeCull tests against the agent frustum planes).
    const LLVector4a* ext = group->getExtents();
    const F32 box[8] = { ext[0][0], ext[0][1], ext[0][2], ext[1][0],
                         ext[1][1], ext[1][2], 0.f, 0.f };
    mBuild->mBoxes.insert(mBuild->mBoxes.end(), box, box + 8);
    mBuild->mGroups.push_back(group);
    mBuild->mIdOf.emplace(group, id);
    return id;
}

void SSGPUCull::registerDrawInfo(LLDrawInfo* info, S32 group_id)
{
    if (!mBuild || !active() || group_id < 0) return;
    mBuild->mIdOf.emplace(info, group_id);
}

bool SSGPUCull::gating() const
{
    return mGate && mLastFrameEnabled
        && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD
        && !gCubeSnapshot
        && !LLPipeline::sShadowRender
        && !LLPipeline::sReflectionRender
        && !LLPipeline::sRenderingHUDs;
}

bool SSGPUCull::shouldDrawGroup(const LLSpatialGroup* group)
{
    if (!gating()) return true;
    auto id_it = mGate->mIdOf.find((const void*)group);
    if (id_it == mGate->mIdOf.end()) return true;
    if (mGate->mVis[id_it->second]) return true;

    static LLCachedControl<U32> hysteresis(gSavedSettings, "SSComputeCullingHysteresis", 2);
    auto streak = mStreak.find((const void*)group);
    return streak == mStreak.end() || streak->second < hysteresis;
}

bool SSGPUCull::shouldDrawInfo(const LLDrawInfo* info)
{
    if (!gating()) return true;
    auto id_it = mGate->mIdOf.find((const void*)info);
    if (id_it == mGate->mIdOf.end()) return true;
    const S32 id = id_it->second;
    if (mGate->mVis[id]) return true;

    static LLCachedControl<U32> hysteresis(gSavedSettings, "SSComputeCullingHysteresis", 2);
    if (id < (S32)mGate->mGroups.size())
    {
        auto streak = mStreak.find(mGate->mGroups[id]);
        if (streak != mStreak.end() && streak->second >= hysteresis) return false;
    }
    return true;
}

void SSGPUCull::ensureHiZ(GLuint depth_tex, U32 width, U32 height)
{
    LLTexUnit* unit = gGL.getTexUnit(0);

    if (!mBoxTex)
    {
        glGenTextures(1, &mBoxTex);
        unit->bindManual(LLTexUnit::TT_TEXTURE, mBoxTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, SS_BOX_TEX_W, SS_BOX_TEX_H);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!mVisTex)
    {
        // Every byte starts "visible" so an id that somehow slips past the
        // dispatch still draws.
        std::vector<U8> ones(SS_MAX_GROUPS, 0xFF);
        glGenTextures(1, &mVisTex);
        unit->bindManual(LLTexUnit::TT_TEXTURE, mVisTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SS_MAX_GROUPS, 1, 0, GL_RED, GL_UNSIGNED_BYTE, ones.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (mHiZTex && mHiZWidth == width && mHiZHeight == height) return;
    if (mHiZTex) glDeleteTextures(1, &mHiZTex);

    // Legal mip chain length for this size, capped for the screen-rect lod pick
    S32 max_levels = 1;
    for (U32 m = width > height ? width : height; m > 1; m >>= 1) ++max_levels;
    const S32 levels = llmin(max_levels, 12);

    glGenTextures(1, &mHiZTex);
    unit->bindManual(LLTexUnit::TT_TEXTURE, mHiZTex);
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_R32F, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    mHiZWidth = width;
    mHiZHeight = height;
    mHiZLevels = levels;
}

void SSGPUCull::buildHiZ(GLuint depth_tex)
{
    LLGLSLShader& prog = gSSHiZProgram;
    prog.bind();

    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, depth_tex);
    gGL.getTexUnit(1)->bindManual(LLTexUnit::TT_TEXTURE, mHiZTex);
    prog.uniform1i(LLStaticHashedString("uDepthTex"), 0);
    prog.uniform1i(LLStaticHashedString("uHiZTex"), 1);

    // Pass 0: copy the scene depth into level 0.
    glBindImageTexture(0, mHiZTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    prog.uniform1i(LLStaticHashedString("uMode"), 0);
    prog.uniform1i(LLStaticHashedString("uLevel"), 0);
    glUniform2i(uniformLoc(prog, "uSrcSize"), (GLint)mHiZWidth, (GLint)mHiZHeight);
    glUniform2i(uniformLoc(prog, "uDstSize"), (GLint)mHiZWidth, (GLint)mHiZHeight);
    glDispatchCompute((mHiZWidth + 15) / 16, (mHiZHeight + 15) / 16, 1);

    // One max-reduce per level; texelFetch reads the previous level while the
    // image unit writes the next, which needs a barrier between dispatches.
    for (S32 level = 1; level < mHiZLevels; ++level)
    {
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        const GLint src_w = (GLint)llmax(1u, mHiZWidth >> (level - 1));
        const GLint src_h = (GLint)llmax(1u, mHiZHeight >> (level - 1));
        const GLint dst_w = (GLint)llmax(1u, mHiZWidth >> level);
        const GLint dst_h = (GLint)llmax(1u, mHiZHeight >> level);

        glBindImageTexture(0, mHiZTex, level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        prog.uniform1i(LLStaticHashedString("uMode"), 1);
        prog.uniform1i(LLStaticHashedString("uLevel"), level);
        glUniform2i(uniformLoc(prog, "uSrcSize"), src_w, src_h);
        glUniform2i(uniformLoc(prog, "uDstSize"), dst_w, dst_h);
        glDispatchCompute((dst_w + 15) / 16, (dst_h + 15) / 16, 1);
    }

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    prog.unbind();
}

void SSGPUCull::submit(LLViewerCamera& camera, GLuint depth_tex, U32 width, U32 height)
{
    const bool enabled_now = active();
    mLastFrameEnabled = enabled_now;

    if (!enabled_now || !mBuild || mBuild->mGroups.empty() || mOutstanding)
    {
        mBuild.reset();
        return;
    }

    // A teleport or fast fling makes the one-frame-old depth worthless as an
    // occlusion oracle; drop the accumulated hidden streaks instead of risking
    // a run of false culls.
    const LLVector3& origin = camera.getOrigin();
    if (dist_vec(origin, mLastOrigin) > 2.f)
    {
        mStreak.clear();
        mGate.reset();
    }
    mLastOrigin = origin;

    ensureHiZ(depth_tex, width, height);

    const S32 count = (S32)mBuild->mGroups.size();
    const S32 rows = ((count * 2) + SS_BOX_TEX_W - 1) / SS_BOX_TEX_W;
    // Pad to whole rows: glTexSubImage2D reads every row in full.
    mBuild->mBoxes.resize((size_t)rows * SS_BOX_TEX_W * 4, 0.f);
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mBoxTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SS_BOX_TEX_W, rows, GL_RGBA, GL_FLOAT, mBuild->mBoxes.data());

    buildHiZ(depth_tex);

    LLGLSLShader& prog = gSSCullProgram;
    prog.bind();

    const glm::mat4 view_proj = toGlm(camera.getProjection()) * toGlm(camera.getModelview());    prog.uniformMatrix4fv(LLStaticHashedString("uViewProj"), 1, GL_FALSE, glm::value_ptr(view_proj));

    F32 planes[24];
    for (U32 i = 0; i < 6; ++i)
    {
        const LLPlane& p = camera.getAgentPlane(i);
        F32* dst = planes + i * 4;
        dst[0] = p[0];
        dst[1] = p[1];
        dst[2] = p[2];
        dst[3] = p[3];
    }
    prog.uniform4fv(LLStaticHashedString("uPlanes"), 6, planes);
    prog.uniform1i(LLStaticHashedString("uCount"), count);
    prog.uniform1i(LLStaticHashedString("uMips"), mHiZLevels);
    glUniform2f(uniformLoc(prog, "uViewport"), (GLfloat)width, (GLfloat)height);
    // Conservative slop against the one-frame depth and the coarse pyramid.
    glUniform1f(uniformLoc(prog, "uBias"), 0.0015f);

    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mHiZTex);
    gGL.getTexUnit(1)->bindManual(LLTexUnit::TT_TEXTURE, mBoxTex);
    prog.uniform1i(LLStaticHashedString("uHiZ"), 0);
    prog.uniform1i(LLStaticHashedString("uBoxes"), 1);

    glBindImageTexture(0, mVisTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glDispatchCompute((GLuint)((count + 63) / 64), 1, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    prog.unbind();

    // Read the visibility bytes back through the shared-context worker; the
    // result gates next frame's draw loop, one frame of latency by design.
    SSGLReadback::Job job;
    job.mTexture = mVisTex;
    job.mTarget = GL_TEXTURE_2D;
    job.mLevel = 0;
    job.mWidth = SS_MAX_GROUPS;
    job.mHeight = 1;
    job.mFormat = GL_RED;
    job.mType = GL_UNSIGNED_BYTE;

    std::shared_ptr<GateFrame> frame = mBuild;
    job.mDone = [this, frame](const U8* data, size_t bytes)
    {
        frame->mVis.assign(data, data + llmin(bytes, (size_t)SS_MAX_GROUPS));
        frame->mVis.resize(SS_MAX_GROUPS, 0xFF);

        for (size_t i = 0; i < frame->mGroups.size(); ++i)
        {
            const void* group = frame->mGroups[i];
            if (frame->mVis[i])
            {
                mStreak.erase(group);
            }
            else
            {
                ++mStreak[group];
            }
        }

        // Retire streaks for groups that have left the candidate set.
        if (mStreak.size() > frame->mGroups.size() * 2 + 4096)
        {
            for (auto it = mStreak.begin(); it != mStreak.end();)
            {
                if (frame->mIdOf.find(it->first) == frame->mIdOf.end())
                {
                    it = mStreak.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        mGate = frame;
        mOutstanding = false;
    };
    mOutstanding = true;
    SSGLReadback::getInstance()->submit(job);

    mBuild.reset();
}
