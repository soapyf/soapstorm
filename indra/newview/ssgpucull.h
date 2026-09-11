/**
 * @file ssgpucull.h
 * @brief Soapstorm: optional compute-shader frustum + occlusion culling for the
 *        main view's deferred draw.
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

#ifndef SS_GPUCULL_H
#define SS_GPUCULL_H

#include "llsingleton.h"
#include "v3math.h"

#include <memory>
#include <unordered_map>
#include <vector>

class LLDrawInfo;
class LLSpatialGroup;
class LLViewerCamera;

// <SS:Nexii> GPU culling for the world camera. While the CPU frustum cull stays
// untouched, every spatial group that survives it is handed over here during
// postSort; at the end of renderGeomDeferred a compute pass tests each group's
// agent-space AABB against the frustum planes and a Hi-Z pyramid built from the
// just-finished depth buffer, and the one-byte-per-group visibility texture is
// read back through SSGLReadback so next frame's draw loop can skip whole
// groups the GPU says are occluded. The depth is one frame old by design, so a
// group only stops drawing after it has been reported hidden for
// SSComputeCullingHysteresis consecutive frames - everything else draws
// conservatively. Gating points are LLRenderPass::renderGroup and
// pushBatches/pushUntexturedBatches; shadow, reflection, cube-snapshot and HUD
// passes are never gated.
class SSGPUCull : public LLSingleton<SSGPUCull>
{
    LLSINGLETON(SSGPUCull);
    ~SSGPUCull() override;

public:
    // GL 4.3 context and the compute programs created (false on Darwin, like the wind flowmap).
    bool supported();

    // GL teardown: deletes the candidate, visibility and Hi-Z textures while the context still exists; the destructor calls it too.
    void shutdownGL();

    // True when the setting is on, the platform supports it, and the current
    // pipeline state is the main world camera's opaque draw.
    bool active();

    // Start a candidate frame. Called from LLPipeline::postSort for the world
    // camera only (the shadow pass re-enters postSort and must not clobber it).
    void beginFrame();

    // Register a frustum-visible group; returns its candidate id or -1 when
    // capped or inactive.
    S32 registerGroup(LLSpatialGroup* group);

    // Associate a DrawInfo with the group that produced it, so the batched
    // render loops can gate on the group's visibility bit.
    void registerDrawInfo(LLDrawInfo* info, S32 group_id);

    // End of renderGeomDeferred: build the Hi-Z pyramid from the finished depth
    // buffer, run the cull dispatch, and submit the async readback.
    void submit(LLViewerCamera& camera, GLuint depth_tex, U32 width, U32 height);

    // Render-time gates: true = draw (also true whenever not active).
    bool shouldDrawGroup(const LLSpatialGroup* group);
    bool shouldDrawInfo(const LLDrawInfo* info);

private:
    // Candidate bookkeeping for one frame; survives into the readback callback.
    struct GateFrame
    {
        std::unordered_map<const void*, S32> mIdOf;   // group or DrawInfo ptr -> candidate id
        std::vector<const void*> mGroups;             // candidate id -> group ptr
        std::vector<F32> mBoxes;                      // candidate id -> 8 floats: min.xyz, max.xyz, pad
        std::vector<U8> mVis;                         // candidate id -> visibility byte
    };

    bool gating() const; // gates consult last frame's results only when in world-camera draw
    void ensureHiZ(GLuint depth_tex, U32 width, U32 height);
    void buildHiZ(GLuint depth_tex);

    std::shared_ptr<GateFrame> mBuild; // frame being registered this frame
    std::shared_ptr<GateFrame> mGate;  // last delivered readback, consumed by the gates
    std::unordered_map<const void*, U32> mStreak; // group ptr -> consecutive hidden frames

    GLuint mBoxTex = 0;  // candidate AABBs, RGBA32F, 2 texels per candidate
    GLuint mVisTex = 0;  // visibility bytes, R8
    GLuint mHiZTex = 0;  // depth pyramid, R32F with mip chain
    U32 mHiZWidth = 0;
    U32 mHiZHeight = 0;
    S32 mHiZLevels = 0;

    bool mOutstanding = false; // a readback is in flight
    bool mLastFrameEnabled = false;
    LLVector3 mLastOrigin;
};

#endif
