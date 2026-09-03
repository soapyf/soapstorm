/**
 * @file sslightningrender.h
 * @brief Atmo Magic: lightning rendering.
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

#ifndef SS_LIGHTNINGRENDER_H
#define SS_LIGHTNINGRENDER_H

#include "llsingleton.h"
#include "lluuid.h"
#include "llpointer.h"
#include "llvertexbuffer.h"
#include "v2math.h"
#include "v3math.h"
#include "v4math.h"
#include "v4coloru.h"

#include <vector>

class LLViewerFetchedTexture;
struct SSStrike;

class SSLightningRender : public LLSingleton<SSLightningRender>
{
    LLSINGLETON_EMPTY_CTOR(SSLightningRender);

public:
    void renderFlash();
    void render();

    // Drops the pass's vertex buffer (shader reload, GL teardown); it is rebuilt lazily on the next draw.
    void releaseGL();

    struct DrawStats
    {
        bool mShaderOk = false;
        bool mGuarded = false;
        S32 mStrikes = 0;
        S32 mBright = 0;
        S32 mOffScreen = 0;
        S32 mSegments = 0;
        S32 mPlasma = 0;
        S32 mSparks = 0;
        S32 mDiscs = 0;
        S32 mSteam = 0;
        S32 mOccluded = 0;
        S32 mQuads = 0;
        bool mDepthCopy = false;
    };
    const DrawStats& stats() const { return mStats; }

private:
    // <SS:Nexii> The pass's one vertex buffer: every ribbon, spark, disc, marker and the flash discs are appended as quads (four vertices, the fixed six-index pattern) into these parallel arrays and drawn in one call per pass, additive blending being order-independent. Beside position, uv and the 8-bit tint ride two float attributes immediate mode could never carry (see ssLightningV.glsl); the arrays persist so a frame never allocates.
    struct Vertex
    {
        LLVector3 mPos;
        LLVector2 mUV;
        LLVector2 mUV1;
        LLColor4U mCol;
        LLVector3 mAux;
        LLVector4 mCtl;
    };

    bool ensureBuffer(U32 quads);
    void beginBatch();
    void pushQuad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d);
    void drawBatch();

    LLPointer<LLVertexBuffer> mVB;
    U32 mVBQuads = 0;

    std::vector<LLVector3> mPos;
    std::vector<LLVector2> mUV;
    std::vector<LLVector2> mUV1;
    std::vector<LLColor4U> mCol;
    std::vector<LLVector3> mAux;
    std::vector<LLVector4> mCtl;
    U32 mQuadCount = 0;

    // Per-strike scratch for the joint-merge pass, sized to the largest channel once.
    std::vector<S32> mSoleChild;
    std::vector<LLVector3> mJointSide;

    LLUUID mTexture;
    LLPointer<LLViewerFetchedTexture> mTextureRef;
    DrawStats mStats;
};

#endif
