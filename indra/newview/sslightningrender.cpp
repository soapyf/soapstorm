/**
 * @file sslightningrender.cpp
 * @brief See sslightningrender.h.
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

#include "sslightningrender.h"

#include "sslightning.h"
#include "ssatmomagic.h"
#include "ssvolcloud.h"

#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llvieweroctree.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvector4a.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    const F32 SPARK_GRAVITY = 9.8f;

    // <SS:Nexii> The ground strike's palette, read off the recorded frames: the amber foot of the bolt (core, over-exposed contact, sheath), the impact flare, the ground fire cooling from yellow-orange to dim red, and the hot metal sparks doing the same. doc/atmo_magic_lightning_strike.md
    const LLColor3 AMBER_CORE(1.f, 0.78f, 0.40f);
    const LLColor3 AMBER_HOT(1.f, 0.93f, 0.68f);
    const LLColor3 AMBER_GLOW(1.f, 0.42f, 0.07f);
    const LLColor3 FLARE_MID(1.f, 0.55f, 0.15f);
    const LLColor3 FIRE_0(1.f, 0.60f, 0.12f);
    const LLColor3 FIRE_1(0.95f, 0.38f, 0.06f);
    const LLColor3 FIRE_2(0.60f, 0.18f, 0.03f);
    // <SS:Nexii> Flash-boiled steam, cool and slightly blue the way water vapour reads against a storm sky rather than the warm white of the fire's smoke. This is an AMBIENT constant, not a sampled sky colour - the puff is mixed toward the bolt's own glow while the channel is still lit, which is the moment that carries the effect, and settles to this afterwards. If it reads too bright on a night storm that constant is the thing to take down. doc/atmo_magic_lightning_strike.md
    const LLColor3 STEAM_COLOR(0.72f, 0.75f, 0.80f);

    const LLColor3 SPARK_0(1.f, 0.97f, 0.85f);
    const LLColor3 SPARK_1(1.f, 0.60f, 0.15f);
    const LLColor3 SPARK_2(0.90f, 0.22f, 0.03f);

    // The pre-strike charge field: sparks live a fraction of a second then respawn elsewhere
    // (the duty cycle is at least double the life) until the strike fires.
    const F32 CHARGE_SPARK_LIFE_S = 0.36f;
    const S32 CHARGE_SPARK_MAX = 90;

    // Corona discharge / St. Elmo's fire: ionized air reads blue-violet whatever colour the
    // bolt is authored as.
    const LLColor3 CORONA_COLOR(0.5f, 0.36f, 1.f);

    // <SS:Nexii> The charge aura's brightness at dial 1.0: the old corona peaked at 0.5 x tint per disc on sub-metre discs that were black half of every pulse; the new haze breathes over larger fixed discs with a pulse floor, so a peak this low is what lands the time-integrated light at roughly a third of the old, per the calibration choice. Dial 3 is about the old amount of light, dial 10 the old peak.
    const F32 AURA_GAIN = 0.032f;

    const F32 CORE_WIDTH_M = 2.2f;
    const F32 GLOW_WIDTH_MULT = 7.f;

    const U32 MAX_QUADS = 48000;

    // Applies the shared far-field squash so bolts sit at the cloud field's drawn depth.
    LLVector3 drawnPoint(const LLVector3& p, const LLVector3& cam, F32& scale_out)
    {
        const LLVector3 rel = p - cam;
        scale_out = SSVolCloud::getInstance()->squashScale(rel.magVec());
        return cam + rel * scale_out;
    }

    // Cheap frustum test on the channel trunk (or flash sphere) so off-screen strikes skip vertex work.
    bool strikeOnScreen(const SSStrike& strike)
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 cam = camera->getOrigin();
        const F32 flash_r = llmax(350.f, strike.mDistanceM * 0.18f);

        F32 s = 1.f;
        if (strike.mChannel.empty())
        {
            const LLVector3 center = drawnPoint((strike.mOrigin + strike.mGround) * 0.5f, cam, s);
            return camera->sphereInFrustum(center,
                ((strike.mOrigin - strike.mGround).magVec() * 0.5f + flash_r) * s) != 0;
        }

        S32 trunk_n = 0;
        for (const SSStrikeNode& node : strike.mChannel) { if (node.mTrunk) ++trunk_n; else break; }
        if (trunk_n <= 0) return true;

        const F32 span = (strike.mChannel[0].mPos
                          - strike.mChannel[(size_t)(trunk_n - 1)].mPos).magVec();
        const F32 r = llmax(flash_r, span * 0.3f);

        const S32 samples[3] = { 0, trunk_n / 2, trunk_n - 1 };
        for (S32 i = 0; i < 3; ++i)
        {
            const LLVector3 p = drawnPoint(strike.mChannel[(size_t)samples[i]].mPos, cam, s);
            if (camera->sphereInFrustum(p, r * s) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Frustum test on the ground show's box (padded for the pulled discs), so a strike whose bolt is on screen but whose foot is not skips the ground work.
    bool groundShowOnScreen(const SSStrike& strike)
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 center = (strike.mGroundBoxMin + strike.mGroundBoxMax) * 0.5f;
        const F32 r = (strike.mGroundBoxMax - strike.mGroundBoxMin).magVec() * 0.5f + 20.f;
        return camera->sphereInFrustum(center, r) != 0;
    }

    // Stateless integer hash behind spark and corona randomness - deterministic per strike, no drifting RNG state.
    U32 hash3(U32 x)
    {
        x ^= x >> 16; x *= 0x7feb352du;
        x ^= x >> 15; x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }
    // Hash to [0,1).
    F32 hashUnit(U32 x) { return (F32)(hash3(x) & 0xffffffu) / (F32)0x1000000; }

    LLColor4U tint8(const LLColor3& c, F32 a)
    {
        return LLColor4U((U8)(llclamp(c.mV[0], 0.f, 1.f) * 255.f + 0.5f),
                         (U8)(llclamp(c.mV[1], 0.f, 1.f) * 255.f + 0.5f),
                         (U8)(llclamp(c.mV[2], 0.f, 1.f) * 255.f + 0.5f),
                         (U8)(llclamp(a, 0.f, 1.f) * 255.f + 0.5f));
    }

    LLColor3 mix3(const LLColor3& a, const LLColor3& b, F32 t)
    {
        return LLColor3(lerp(a.mV[0], b.mV[0], t), lerp(a.mV[1], b.mV[1], t), lerp(a.mV[2], b.mV[2], t));
    }

    // Three-stop colour walk for sparks and fire: c0 at 0, c1 at the middle, c2 at 1.
    LLColor3 ramp3(const LLColor3& c0, const LLColor3& c1, const LLColor3& c2, F32 u)
    {
        return (u < 0.5f) ? mix3(c0, c1, u * 2.f) : mix3(c1, c2, (u - 0.5f) * 2.f);
    }

    // Camera-facing axes for a billboard at a point: right (world-horizontal) and up.
    bool billboardAxes(const LLVector3& center, const LLVector3& cam, LLVector3& right, LLVector3& up)
    {
        LLVector3 to_cam = cam - center;
        if (to_cam.normalize() <= 0.f) return false;
        const LLVector3 ref = (llabs(to_cam.mV[VZ]) > 0.9f)
            ? LLVector3(1.f, 0.f, 0.f) : LLVector3(0.f, 0.f, 1.f);
        right = to_cam % ref;
        if (right.normalize() <= 0.f) return false;
        up = right % to_cam;
        if (up.normalize() <= 0.f) return false;
        return true;
    }
}

// Drops the vertex buffer so the next draw rebuilds it.
void SSLightningRender::releaseGL()
{
    mVB = nullptr;
    mVBQuads = 0;
}

// Grows the quad buffer to hold at least this many quads, with the fixed six-index pattern written once.
bool SSLightningRender::ensureBuffer(U32 quads)
{
    quads = llmin(quads, MAX_QUADS);
    if (mVB.notNull() && mVBQuads >= quads) return true;

    U32 alloc = llmax(4096u, mVBQuads);
    while (alloc < quads) alloc *= 2;
    alloc = llmin(alloc, MAX_QUADS);

    static const U32 VB_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                             | LLVertexBuffer::MAP_TANGENT
                             | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_TEXCOORD1
                             | LLVertexBuffer::MAP_COLOR;

    mVB = new LLVertexBuffer(VB_MASK);
    if (!mVB->allocateBuffer(alloc * 4, alloc * 6 * 2))
    {
        mVB = nullptr;
        mVBQuads = 0;
        return false;
    }

    // <SS:Nexii> The VBO pool hands back recycled buffers unbound, so bind before setIndexData streams the pattern - otherwise it lands in whichever pass drew last and this buffer keeps stale indices (the vertex mess). unbind() first because setBuffer only configures anything when it sees the binding CHANGE: on a pool miss LLVBOPool::allocate has already glBindBuffer'd the new name and assigned sGLRenderBuffer itself, so setBuffer would find them equal and skip the setup entirely. Zeroing the tracker makes both pool paths take the same one. doc/atmo_magic_lightning_strike.md
    LLVertexBuffer::unbind();
    mVB->setBuffer();

    std::vector<U32> indices((size_t)alloc * 6);
    U32 vtx = 0;
    for (U32 i = 0, o = 0; i < alloc; ++i, o += 6, vtx += 4)
    {
        indices[o + 0] = vtx + 0;
        indices[o + 1] = vtx + 1;
        indices[o + 2] = vtx + 2;
        indices[o + 3] = vtx + 1;
        indices[o + 4] = vtx + 3;
        indices[o + 5] = vtx + 2;
    }
    mVB->setIndexData(indices.data(), 0, (U32)indices.size());
    mVB->unmapBuffer();

    mVBQuads = alloc;
    return true;
}

// Empties the frame's quad arrays without releasing their storage.
void SSLightningRender::beginBatch()
{
    mPos.clear();
    mUV.clear();
    mUV1.clear();
    mCol.clear();
    mAux.clear();
    mCtl.clear();
    mQuadCount = 0;
}

// Appends one quad (a b / c d in the index pattern's order: a, b, c, then b, d, c).
void SSLightningRender::pushQuad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d)
{
    if (mQuadCount >= MAX_QUADS) return;
    const Vertex* v[4] = { &a, &b, &c, &d };
    for (S32 i = 0; i < 4; ++i)
    {
        mPos.push_back(v[i]->mPos);
        mUV.push_back(v[i]->mUV);
        mUV1.push_back(v[i]->mUV1);
        mCol.push_back(v[i]->mCol);
        mAux.push_back(v[i]->mAux);
        mCtl.push_back(v[i]->mCtl);
    }
    ++mQuadCount;
}

// Uploads the frame's quads and draws them in one call under the currently bound program.
void SSLightningRender::drawBatch()
{
    if (mQuadCount == 0) return;
    if (!ensureBuffer(mQuadCount)) return;

    const U32 n = mQuadCount;
    LLStrider<LLVector3> verticesp;
    LLStrider<LLVector3> normalsp;
    LLStrider<LLVector4a> tangentsp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;
    LLStrider<LLVector2> texcoords1p;
    mVB->getVertexStrider(verticesp, 0, n * 4);
    mVB->getNormalStrider(normalsp, 0, n * 4);
    mVB->getTangentStrider(tangentsp, 0, n * 4);
    mVB->getColorStrider(colorsp, 0, n * 4);
    mVB->getTexCoord0Strider(texcoordsp, 0, n * 4);
    mVB->getTexCoord1Strider(texcoords1p, 0, n * 4);
    for (U32 i = 0; i < n * 4; ++i)
    {
        *verticesp++ = mPos[i];
        *normalsp++ = mAux[i];
        LLVector4a t;
        t.set(mCtl[i].mV[0], mCtl[i].mV[1], mCtl[i].mV[2], mCtl[i].mV[3]);
        *tangentsp++ = t;
        *colorsp++ = mCol[i];
        *texcoordsp++ = mUV[i];
        *texcoords1p++ = mUV1[i];
    }
    mVB->unmapBuffer();

    // <SS:Nexii> The attribute pointers must be re-established here every draw, and unbind() is what forces it. setBuffer only calls setupVertexBuffer when sGLRenderBuffer != mGLBuffer, or when the shader's attribute mask changed - and by this line NEITHER is true: unmapBuffer has just bound this buffer to stream the vertex data (assigning sGLRenderBuffer without configuring a single glVertexAttribPointer), and the pass's own bind() already set sLastMask to this shader's mask. So the draw would run on whichever buffer the previous pass pointed the attributes at - the cloud deck's, or one already returned to the pool. That is the whole fault: geometry built, quads counted, occlusion queries coming back with zero samples on a strike in front of the eye, nothing on screen. The precipitation pass escapes it only by ordering - it unmaps BEFORE binding its shader, and the bind's own unbind() then zeroes the tracker for it. doc/atmo_magic_lightning_strike.md
    LLVertexBuffer::unbind();
    mVB->setBuffer();
    mVB->drawRange(LLRender::TRIANGLES, 0, n * 4 - 1, n * 6, 0);
    mStats.mQuads += (S32)n;
}

// Additive glow discs along the channel (or a sheet strike's origin) that wash the sky while a strike flashes, plus the ground fire's scene-wide amber veil.
void SSLightningRender::renderFlash()
{
    SSLightning* lightning = SSLightning::getInstance();

    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }
    if (!gSSLightningProgram.isComplete()) return;

    static LLCachedControl<F32> fire_light_setting(gSavedSettings, "SSAtmoLightningFireLight", 1.f);
    const F32 fire_light = llclamp((F32)fire_light_setting, 0.f, 4.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    bool any_flash = false;
    LLColor3 wash(0.f, 0.f, 0.f);
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mFlash > 0.002f) any_flash = true;
        if (s.mKind == STRIKE_GROUND && fire_light > 0.f)
        {
            // <SS:Nexii> The recorded scene goes orange in the sky and the rain, not just on the road: a point light cannot tint air, so the fire drives a fullscreen veil, faint, falling off with the strike's distance, that the deck then veils in turn since it draws before the puffs. No bloom seed - a fullscreen alpha would dip the exposure.
            const F32 w = 0.85f * s.mFire + 0.15f * s.mHit;
            if (w <= 0.01f) continue;
            const F32 gd = (s.mGround - cam).magVec();
            if (gd > 400.f) continue;
            wash += AMBER_GLOW * (0.12f * w * s.mIntensity * fire_light / (1.f + (gd / 80.f) * (gd / 80.f)));
        }
    }
    const bool any_wash = wash.mV[0] > 0.002f;
    if (!any_flash && !any_wash) return;

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.4f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);
    const LLColor3 GLOW_COLOR = SSAtmoMagic::getInstance()->lightningColor();

    gSSLightningProgram.bind();
    static LLStaticHashedString s_use_tex("ss_use_tex");
    static LLStaticHashedString s_glow("ss_glow");
    static LLStaticHashedString s_soft_on("ss_soft_on");
    static LLStaticHashedString s_time("ss_time");
    static LLStaticHashedString s_bead("ss_bead");
    static LLStaticHashedString s_squash("ss_squash");
    static LLStaticHashedString s_cam("ss_cam_pos");
    gSSLightningProgram.uniform1f(s_use_tex, 0.f);
    gSSLightningProgram.uniform1f(s_glow, glow);
    gSSLightningProgram.uniform1f(s_soft_on, 0.f);
    gSSLightningProgram.uniform1f(s_time, 0.f);
    gSSLightningProgram.uniform1f(s_bead, 0.f);
    {
        SSVolCloud* vol = SSVolCloud::getInstance();
        gSSLightningProgram.uniform3f(s_squash, vol->squashKnee(), vol->squashCap(), vol->virtualRadius());
        gSSLightningProgram.uniform3fv(s_cam, 1, cam.mV);
    }
    gSSLightningProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, LLViewerFetchedTexture::sWhiteImagep);

    LLGLDisable cull(GL_CULL_FACE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setColorMask(true, true);

    beginBatch();

    // The flash discs: tint x brightness in rgb, the bloom fraction in the 8-bit alpha, the shader's cubic disc and glow multiply on top.
    // <SS:Nexii> Pulled 30-60m toward the camera along the view ray with the radius rescaled by the same factor, so each disc projects exactly where its true point does (the aura discs' trick at a smaller range): at its true depth the lowest trunk disc's plane cut a hard chord across every rise in the landscape between it and the eye. The per-corner height above the strike's surface rides aux.y for the shader's ground fade.
    auto flashDisc = [&](const LLVector3& center_true, F32 radius_true, F32 bright, F32 a8, F32 surf_z)
    {
        LLVector3 right, up;
        if (!billboardAxes(center_true, cam, right, up)) return;
        const F32 d = (center_true - cam).magVec();
        if (d < 1.f) return;
        const F32 pull = llmin(llclamp(0.1f * d, 30.f, 60.f), 0.5f * d);
        const F32 k = 1.f - pull / d;
        const LLVector3 center = cam + (center_true - cam) * k;
        const F32 radius = radius_true * k;

        Vertex v;
        v.mCol = tint8(GLOW_COLOR, a8);
        v.mUV1.set(0.f, 0.f);
        v.mCtl.set(bright, 0.f, 0.f, 4.f);
        Vertex c[4];
        for (S32 i = 0; i < 4; ++i)
        {
            const F32 sx = (i & 1) ? 1.f : -1.f;
            const F32 sy = (i & 2) ? 1.f : -1.f;
            c[i] = v;
            c[i].mPos = center + right * (radius * sx) + up * (radius * sy);
            c[i].mUV.set((i & 1) ? 1.f : 0.f, (i & 2) ? 1.f : 0.f);
            const F32 z_true = center_true.mV[VZ] + right.mV[VZ] * radius_true * sx + up.mV[VZ] * radius_true * sy;
            c[i].mAux.set(0.f, (z_true - surf_z) / llmax(radius_true, 0.01f), 0.f);
        }
        pushQuad(c[0], c[1], c[2], c[3]);
    };

    if (any_flash)
    {
        for (const SSStrike& strike : lightning->strikes())
        {
            if (strike.mFlash <= 0.002f) continue;
            if (!strikeOnScreen(strike)) continue;

            const F32 a = llclamp(strike.mFlash, 0.f, 1.f);
            // Only a ground strike has a surface to fade against; a fork's or sheet's discs hang in cloud, so the sentinel keeps them clear of the fade.
            const F32 surf_z = (strike.mKind == STRIKE_GROUND)
                ? SSLightning::surfaceZ(strike.mGround) : -1.0e6f;

            if (!strike.mChannel.empty())
            {
                const F32 radius = llmax(160.f, strike.mDistanceM * 0.08f);
                const S32 DISCS = 5;

                // <SS:Nexii> Only the trunk the leader has REACHED carries discs: placing all five down the whole trunk at leader start lit the impact point ~leader_s before the bolt visibly arrived - an early flare at the ground that upstaged the real contact. The glow column now stretches downward with the front and reaches the ground the instant the channel does.
                S32 trunk_n = 0;
                for (const SSStrikeNode& node : strike.mChannel)
                {
                    if (!node.mTrunk || node.mReachedAt > strike.mLeaderProgress) break;
                    ++trunk_n;
                }

                if (trunk_n > 0)
                {
                    for (S32 di = 0; di < DISCS; ++di)
                    {
                        const S32 want = (S32)((F32)di / (F32)(DISCS - 1) * (F32)(trunk_n - 1));
                        flashDisc(strike.mChannel[(size_t)want].mPos, radius, a * 0.35f, a * 0.08f, surf_z);
                    }
                }
            }
            else
            {
                const F32 radius = llmax(350.f, strike.mDistanceM * 0.18f);
                flashDisc(strike.mOrigin, radius, a * 0.6f, a * 0.12f, surf_z);
            }
        }
    }

    if (any_wash)
    {
        // One quad half a metre ahead of the camera, wider than any field of view, flat-filled.
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 center = cam + camera->getAtAxis() * 0.5f;
        const LLVector3 right = camera->getLeftAxis() * -3.f;
        const LLVector3 up = camera->getUpAxis() * 3.f;
        Vertex v;
        v.mCol = tint8(wash, 0.f);
        v.mUV1.set(0.f, 0.f);
        v.mAux.set(0.f, 0.f, 0.f);
        v.mCtl.set(1.f, 0.f, 0.f, 5.f);
        Vertex bl = v, br = v, tl = v, tr = v;
        bl.mPos = center - right - up; bl.mUV.set(0.f, 0.f);
        br.mPos = center + right - up; br.mUV.set(1.f, 0.f);
        tl.mPos = center - right + up; tl.mUV.set(0.f, 1.f);
        tr.mPos = center + right + up; tr.mUV.set(1.f, 1.f);
        pushQuad(bl, br, tl, tr);
    }

    drawBatch();

    LLVertexBuffer::unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, true);
    gSSLightningProgram.unbind();
}

// Draws every live strike: layered core and sheath ribbons per return stroke with the amber foot, the plasma the latest stroke cools into, the ground crawl, the charge swarm, impact sparks, the aura / flare / fire discs, markers.
void SSLightningRender::render()
{
    SSLightning* lightning = SSLightning::getInstance();

    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        mStats.mGuarded = true;
        return;
    }

    mStats = DrawStats();
    mStats.mShaderOk = gSSLightningProgram.isComplete();
    mStats.mStrikes = (S32)lightning->strikes().size();
    if (lightning->strikes().empty()) return;
    if (!mStats.mShaderOk) return;

    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugStrikeMarkers", false);

    bool anything = false;
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mChannelBrightness > 0.001f || s.mCharge > 0.001f || s.mChargeHeld > 0.001f
            || s.mHit > 0.001f || s.mFire > 0.001f || s.mFlash > 0.001f
            || (markers && s.mT < 0.f))
        {
            anything = true;
            break;
        }
    }
    if (!anything) return;

    static LLCachedControl<std::string> tex_setting(gSavedSettings, "SSAtmoLightningTexture", "");
    const std::string tex_str = tex_setting;
    const LLUUID tex_id(tex_str);
    if (tex_id.notNull() && (mTextureRef.isNull() || mTexture != tex_id))
    {
        mTexture = tex_id;
        mTextureRef = LLViewerTextureManager::getFetchedTexture(
            tex_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (mTextureRef.notNull()) mTextureRef->setNoDelete();
    }
    if (tex_id.isNull()) { mTexture.setNull(); mTextureRef = NULL; }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    const LLVector3 cam_at = camera->getAtAxis();
    const F32 tnow = (F32)fmod(SSAtmoMagic::getInstance()->sharedTime(), 1024.0);
    const F32 now = (F32)LLFrameTimer::getElapsedSeconds();
    const U32 frame = LLFrameTimer::getFrameCount();
    const F32 px_per_rad = (F32)gGLViewport[3] / (2.f * tanf(llmax(camera->getView(), 0.05f) * 0.5f));

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.4f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);
    static LLCachedControl<F32> occl_setting(gSavedSettings, "SSAtmoLightningOcclusion", 0.85f);
    const F32 occ_strength = llclamp((F32)occl_setting, 0.f, 1.f);
    static LLCachedControl<F32> amber_setting(gSavedSettings, "SSAtmoLightningGroundAmber", 1.f);
    const F32 amber_str = llclamp((F32)amber_setting, 0.f, 2.f);
    static LLCachedControl<F32> amber_zone_setting(gSavedSettings, "SSAtmoLightningAmberZone", 25.f);
    const F32 amber_zone = llclamp((F32)amber_zone_setting, 5.f, 80.f);
    static LLCachedControl<F32> dissolve_setting(gSavedSettings, "SSAtmoLightningDissolve", 1.f);
    const F32 dissolve = llclamp((F32)dissolve_setting, 0.f, 2.f);
    static LLCachedControl<F32> plasma_setting(gSavedSettings, "SSAtmoLightningPlasma", 1.f);
    const F32 plasma_dial = llclamp((F32)plasma_setting, 0.f, 2.f);
    static LLCachedControl<F32> warp_setting(gSavedSettings, "SSAtmoLightningPlasmaWarp", 1.f);
    const F32 warp = llclamp((F32)warp_setting, 0.f, 3.f);
    static LLCachedControl<F32> bead_setting(gSavedSettings, "SSAtmoLightningBead", 0.35f);
    const F32 bead = llclamp((F32)bead_setting, 0.f, 0.8f);
    static LLCachedControl<F32> aura_setting(gSavedSettings, "SSAtmoLightningAura", 0.f);
    const F32 aura_dial = llclamp((F32)aura_setting, 0.f, 10.f);
    static LLCachedControl<F32> pull_setting(gSavedSettings, "SSAtmoLightningAuraPull", 1.f);
    const F32 pull_dial = llclamp((F32)pull_setting, 0.f, 2.f);
    static LLCachedControl<F32> soft_setting(gSavedSettings, "SSAtmoLightningSoftDepth", 1.f);
    const F32 soft_dial = llclamp((F32)soft_setting, 0.f, 3.f);
    static LLCachedControl<F32> hit_setting(gSavedSettings, "SSAtmoLightningHitFlare", 1.f);
    const F32 hit_dial = llclamp((F32)hit_setting, 0.f, 3.f);
    static LLCachedControl<F32> fire_setting(gSavedSettings, "SSAtmoLightningGroundFire", 1.f);
    const F32 fire_dial = llclamp((F32)fire_setting, 0.f, 3.f);
    static LLCachedControl<F32> fire_life_setting(gSavedSettings, "SSAtmoLightningGroundFireLife", 0.9f);
    const F32 tau_f = llclamp((F32)fire_life_setting, 0.2f, 2.5f) / 2.5f;
    static LLCachedControl<F32> secondary_setting(gSavedSettings, "SSAtmoLightningSecondarySparks", 1.f);
    const F32 secondary = llclamp((F32)secondary_setting, 0.f, 2.f);
    static LLCachedControl<F32> ribbon_drift_setting(gSavedSettings, "SSAtmoLightningRibbonDrift", 3.f);
    const F32 ribbon_drift = llclamp((F32)ribbon_drift_setting, 0.f, 5.f);
    static LLCachedControl<bool> occl_cull_setting(gSavedSettings, "SSAtmoLightningOcclusionCull", true);

    const LLColor3 CORE_COLOR = SSAtmoMagic::getInstance()->lightningCoreColor();
    const LLColor3 GLOW_COLOR = SSAtmoMagic::getInstance()->lightningColor();

    // The charge field leans into its physics: air breakdown is violet-blue regardless of the
    // bolt's authored colour, so the sparks and haze read electric even in a red storm.
    const LLColor3 SPARK_COLOR = CORONA_COLOR * 0.7f + GLOW_COLOR * 0.3f;
    const LLColor3 CORONA_TINT = CORONA_COLOR * 0.85f + GLOW_COLOR * 0.15f;

    const bool sparks_on = SSAtmoMagic::getInstance()->lightningSparks();
    const LLVector3 wind = SSAtmoMagic::getInstance()->windXY();
    SSVolCloud* vol = SSVolCloud::getInstance();

    // <SS:Nexii> The occlusion query's answers from earlier frames, read without ever waiting: a strike whose ground box drew no sample last time is hidden until it says otherwise. Names come from the octree's pool; the model hands them back on retirement. [interaction: LLOcclusionCullingGroup query pool]
    const bool queries_ok = occl_cull_setting && gGLManager.mGLVersion >= 3.3f && !LLGLSLShader::sProfileEnabled;
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mOccQuery == 0 || s.mOccIssuedFrame == 0 || s.mOccIssuedFrame >= frame) continue;
        GLuint avail = 0;
        glGetQueryObjectuiv(s.mOccQuery, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) continue;
        GLuint samples = 0;
        glGetQueryObjectuiv(s.mOccQuery, GL_QUERY_RESULT, &samples);
        s.mOccHidden = (samples == 0);
        s.mOccIssuedFrame = 0;
    }

    // The per-strike frame facts every element below reads.
    struct Facts
    {
        bool mOnScreen = false;
        bool mGroundOn = false;
        bool mHidden = false;
        F32 mGroundDist = 0.f;
        F32 mHeld = 0.f;
    };
    std::vector<Facts> facts(lightning->strikes().size());
    bool want_depth = false;
    for (size_t si = 0; si < lightning->strikes().size(); ++si)
    {
        const SSStrike& s = lightning->strikes()[si];
        Facts& f = facts[si];
        f.mOnScreen = strikeOnScreen(s);
        f.mGroundOn = groundShowOnScreen(s);
        f.mGroundDist = (s.mGround - cam).magVec();
        f.mHeld = (s.mT < 0.f) ? s.mChargeHeld : s.mChargeHeld * expf(-s.mT / 0.06f);
        // A hidden answer is honoured only away from the moments that must never be lost: the contact and every restrike.
        f.mHidden = s.mOccHidden && queries_ok && (s.mT < -0.3f || s.mPlasmaSince > 0.3f);
        if (f.mHidden) mStats.mOccluded++;

        if (soft_dial > 0.f && f.mGroundOn && !f.mHidden && f.mGroundDist < 400.f
            && (f.mHeld > 0.005f || s.mHit > 0.005f || s.mFire > 0.005f))
        {
            want_depth = true;
        }
    }

    // <SS:Nexii> The depth copy for the discs' soft fades, shared with the puff pass (one copy per frame is exact - no post-deferred pass writes depth) and taken here only on a clear sky with a ground show in view. Must precede the program bind: the copy binds its own program and rebinds the screen target. [interaction: SSVolCloud depth copy]
    LLRenderTarget* depth_copy = want_depth ? vol->ensureSceneDepthCopy() : nullptr;
    mStats.mDepthCopy = (depth_copy != nullptr);

    gSSLightningProgram.bind();

    static LLStaticHashedString s_squash("ss_squash");
    static LLStaticHashedString s_cam("ss_cam_pos");
    static LLStaticHashedString s_use_tex("ss_use_tex");
    static LLStaticHashedString s_glow("ss_glow");
    static LLStaticHashedString s_soft_on("ss_soft_on");
    static LLStaticHashedString s_clip("ss_clip");
    static LLStaticHashedString s_time("ss_time");
    static LLStaticHashedString s_bead("ss_bead");
    gSSLightningProgram.uniform3f(s_squash, vol->squashKnee(), vol->squashCap(), vol->virtualRadius());
    gSSLightningProgram.uniform3fv(s_cam, 1, cam.mV);
    gSSLightningProgram.uniform1f(s_glow, glow);
    gSSLightningProgram.uniform1f(s_time, tnow);
    gSSLightningProgram.uniform1f(s_bead, bead);

    const bool textured = mTextureRef.notNull() && mTextureRef->hasGLTexture();
    if (textured)
    {
        gSSLightningProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, mTextureRef);
        mTextureRef->addTextureStats(512.f * 512.f);
    }
    else
    {
        gSSLightningProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, LLViewerFetchedTexture::sWhiteImagep);
    }
    gSSLightningProgram.uniform1f(s_use_tex, textured ? 1.f : 0.f);

    bool soft_on = false;
    if (depth_copy && gSSLightningProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, depth_copy, true) >= 0)
    {
        soft_on = true;
        gSSLightningProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);
        // The projection's own planes: the constant far clip, not the draw distance the camera reports.
        gSSLightningProgram.uniform2f(s_clip, camera->getNear(), MAX_FAR_CLIP);
    }
    gSSLightningProgram.uniform1f(s_soft_on, soft_on ? 1.f : 0.f);

    LLGLDisable cull(GL_CULL_FACE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setColorMask(true, true);

    // Issue this frame's queries: the ground box of every live ground strike in view, one pending query per strike, the box drawn through the same program (so the far squash applies) in a mode that can never discard.
    if (queries_ok)
    {
        for (size_t si = 0; si < lightning->strikes().size(); ++si)
        {
            const SSStrike& s = lightning->strikes()[si];
            const Facts& f = facts[si];
            if (s.mKind != STRIKE_GROUND || !f.mGroundOn || f.mGroundDist > 1500.f) continue;
            if (s.mOccIssuedFrame != 0) continue;
            if (s.mChargeHeld <= 0.f && s.mT < -1.5f) continue;
            if (s.mOccQuery == 0) s.mOccQuery = LLOcclusionCullingGroup::getNewOcclusionQueryObjectName();
            if (s.mOccQuery == 0) continue;

            beginBatch();
            const LLVector3& lo = s.mGroundBoxMin;
            const LLVector3& hi = s.mGroundBoxMax;
            LLVector3 c[8];
            for (S32 i = 0; i < 8; ++i)
            {
                c[i].set((i & 1) ? hi.mV[VX] : lo.mV[VX], (i & 2) ? hi.mV[VY] : lo.mV[VY], (i & 4) ? hi.mV[VZ] : lo.mV[VZ]);
            }
            static const S32 faces[6][4] = { {0,1,2,3}, {4,5,6,7}, {0,1,4,5}, {2,3,6,7}, {0,2,4,6}, {1,3,5,7} };
            Vertex v;
            v.mCol = tint8(LLColor3(0.f, 0.f, 0.f), 0.f);
            v.mUV1.set(0.f, 0.f);
            v.mAux.set(0.f, 0.f, 0.f);
            v.mCtl.set(0.f, 0.f, 0.f, 6.f);
            for (S32 fi = 0; fi < 6; ++fi)
            {
                Vertex a = v, b = v, cc = v, d = v;
                a.mPos = c[faces[fi][0]]; a.mUV.set(0.f, 0.f);
                b.mPos = c[faces[fi][1]]; b.mUV.set(1.f, 0.f);
                cc.mPos = c[faces[fi][2]]; cc.mUV.set(0.f, 1.f);
                d.mPos = c[faces[fi][3]]; d.mUV.set(1.f, 1.f);
                pushQuad(a, b, cc, d);
            }
            gGL.setColorMask(false, false);
            glBeginQuery(GL_ANY_SAMPLES_PASSED, s.mOccQuery);
            drawBatch();
            glEndQuery(GL_ANY_SAMPLES_PASSED);
            gGL.setColorMask(true, true);
            s.mOccIssuedFrame = frame;
        }
    }

    beginBatch();

    // A strip segment: each end carries its own tint and data; the joint sides, when given, make a turn's two quads share one corner edge.
    auto ribbon = [&](const LLVector3& a, const LLVector3& b, F32 width_a, F32 width_b, F32 v0, F32 v1,
                      const Vertex& va, const Vertex& vb,
                      const LLVector3* side_a = nullptr, const LLVector3* side_b = nullptr)
    {
        LLVector3 seg = b - a;
        LLVector3 mid = (a + b) * 0.5f;
        LLVector3 view = mid - cam;
        LLVector3 side = seg % view;
        if (side.normalize() <= 0.f) return;
        const LLVector3& sa = side_a ? *side_a : side;
        const LLVector3& sb = side_b ? *side_b : side;

        Vertex a0 = va, a1 = va, b0 = vb, b1 = vb;
        a0.mPos = a - sa * width_a; a0.mUV.set(0.f, v0);
        a1.mPos = a + sa * width_a; a1.mUV.set(1.f, v0);
        b0.mPos = b - sb * width_b; b0.mUV.set(0.f, v1);
        b1.mPos = b + sb * width_b; b1.mUV.set(1.f, v1);
        pushQuad(a0, a1, b0, b1);
    };

    // A strip lying in the surface plane (the ground crawl): never pierces the road, so it needs no depth fade at all.
    auto groundRibbon = [&](const LLVector3& a, const LLVector3& b, F32 width_a, F32 width_b, F32 v0, F32 v1,
                            const Vertex& va, const Vertex& vb,
                            const LLVector3* side_a = nullptr, const LLVector3* side_b = nullptr)
    {
        LLVector3 seg = b - a;
        LLVector3 side = seg % LLVector3::z_axis;
        if (side.normalize() <= 0.f) return;
        const LLVector3& sa = side_a ? *side_a : side;
        const LLVector3& sb = side_b ? *side_b : side;
        const LLVector3 lift(0.f, 0.f, 0.12f);
        Vertex a0 = va, a1 = va, b0 = vb, b1 = vb;
        a0.mPos = a - sa * width_a + lift; a0.mUV.set(0.f, v0);
        a1.mPos = a + sa * width_a + lift; a1.mUV.set(1.f, v0);
        b0.mPos = b - sb * width_b + lift; b0.mUV.set(0.f, v1);
        b1.mPos = b + sb * width_b + lift; b1.mUV.set(1.f, v1);
        pushQuad(a0, a1, b0, b1);
    };

    // An aura / flare / fire disc: pulled along the live view ray toward the camera with its radius rescaled so it projects exactly where the true point does, the anchor depth and the per-corner height above the surface riding the vertices for the shader's fades.
    auto disc = [&](const LLVector3& center_true, F32 r_right, F32 r_up, F32 surf_z,
                    const LLColor3& tint, F32 bright, F32 a8, F32 q, F32 soft_m)
    {
        LLVector3 right, up;
        if (!billboardAxes(center_true, cam, right, up)) return;
        const F32 d = (center_true - cam).magVec();
        if (d < 0.5f) return;
        const F32 pull = llmin(llclamp(0.12f * d, 4.f, 120.f) * pull_dial, 0.5f * d);
        const F32 k = 1.f - pull / d;
        const LLVector3 center = cam + (center_true - cam) * k;
        const F32 rr = r_right * k;
        const F32 ru = r_up * k;

        F32 sc = 1.f;
        const F32 anchor = (drawnPoint(center_true, cam, sc) - cam) * cam_at;

        Vertex v;
        v.mCol = tint8(tint, a8);
        v.mUV1.set(0.f, 0.f);
        v.mCtl.set(bright, 0.f, soft_m, 2.f + 0.5f * llclamp(q, 0.f, 1.f));

        Vertex c[4];
        for (S32 i = 0; i < 4; ++i)
        {
            const F32 sx = (i & 1) ? 1.f : -1.f;
            const F32 sy = (i & 2) ? 1.f : -1.f;
            c[i] = v;
            c[i].mPos = center + right * (rr * sx) + up * (ru * sy);
            c[i].mUV.set((i & 1) ? 1.f : 0.f, (i & 2) ? 1.f : 0.f);
            const F32 z_true = center_true.mV[VZ] + right.mV[VZ] * r_right * sx + up.mV[VZ] * r_up * sy;
            c[i].mAux.set(anchor, (z_true - surf_z) / llmax(r_up, 0.01f), 0.f);
        }
        pushQuad(c[0], c[1], c[2], c[3]);
        mStats.mDiscs++;
    };

    for (size_t si = 0; si < lightning->strikes().size(); ++si)
    {
        const SSStrike& strike = lightning->strikes()[si];
        const Facts& fx = facts[si];
        if (strike.mChannelBrightness > 0.001f) mStats.mBright++;
        if (!fx.mOnScreen) { mStats.mOffScreen++; continue; }

        const F32 I = strike.mIntensity;
        const F32 gd = fx.mGroundDist;
        const bool ground = (strike.mKind == STRIKE_GROUND);
        const F32 dist_scale = llmax(1.f, strike.mDistanceM / 1000.f);
        const F32 ground_scale = llmax(1.f, gd / 250.f);
        const F32 ground_px_floor = gd * 0.0012f;
        const bool ground_show = ground && fx.mGroundOn && !fx.mHidden;

        // The strike's stable seed, so plasma and sparks hold the same pattern every frame and every return stroke.
        const U32 strike_seed = (U32)(strike.mFireAt * 3571.0) ^ 0x11feu;
        const F32 seed01 = hashUnit(strike_seed);

        // <SS:Nexii> The amber zone: path metres up from the attachment over which the bolt grades to amber - the dial's 25m near, tripled by a kilometre and held there, never less than forty pixels tall, so a far strike still shows its foot. Positive bolts, the heavy hitters, flare a little further.
        const F32 zone_m = llmax(amber_zone * (1.f + 2.f * llclamp(gd / 1000.f, 0.f, 1.5f)) * (strike.mPositive ? 1.3f : 1.f),
                                 40.f * gd / llmax(px_per_rad, 1.f));
        auto amberOf = [&](const SSStrikeNode& node, F32& w, F32& hot)
        {
            w = 0.f;
            hot = 0.f;
            if (!ground || node.mTipDistM > 1.0e8f) return;
            const F32 t = llclamp(node.mTipDistM / zone_m, 0.f, 1.f);
            w = llmin(amber_str * powf(1.f - t, 1.5f), 1.f);
            const F32 th = llclamp(node.mTipDistM / (0.12f * zone_m), 0.f, 1.f);
            hot = (1.f - th) * (1.f - th) * llmin(amber_str, 1.f);
        };

        if (strike.mChannelBrightness > 0.001f && !strike.mChannel.empty())
        {
            if (occ_strength > 0.f && !vol->empty()
                && (now - strike.mOccAt > 0.25f
                    || (cam - strike.mOccCam).magVecSquared() > 16.f))
            {
                for (const SSStrikeNode& node : strike.mChannel)
                {
                    node.mOcc = node.mCrawl ? 1.f : vol->transmittance(cam, node.mPos, occ_strength);
                }
                strike.mOccAt = now;
                strike.mOccCam = cam;
            }
            const bool occluding = occ_strength > 0.f && !vol->empty();

            // Which stroke copies still draw: any bright one, plus the plasma copies - the latest stroke, and the one before it when the gap between them was long enough for its wisps to still hang under the fresh bolt.
            const S32 last = strike.mStrokeCount - 1;
            const bool plasma_on = plasma_dial > 0.f && dissolve > 0.f && strike.mT >= 0.f && last >= 0;
            const S32 prev_plasma = (plasma_on && last >= 1
                && strike.mStrokeAt[last] - strike.mStrokeAt[last - 1] > 0.15f) ? last - 1 : -1;
            const F32 span_s = (dissolve > 0.f) ? SSDissolve::SPAN_S / dissolve : 0.f;
            const F32 plasma_end = SSDissolve::LAG_S + span_s
                + SSDissolve::PLASMA_S * SSDissolve::PLASMA_FOOT_MULT;
            const F32 px_core = CORE_WIDTH_M * dist_scale * px_per_rad / llmax(ground ? gd : strike.mDistanceM, 1.f);
            const F32 plasma_lod = plasma_dial * llclamp(px_core / 3.f, 0.f, 1.f);

            bool any_copy = false;
            for (S32 k = 0; k < strike.mStrokeCount; ++k)
            {
                const F32 b = strike.mStrokeBright[k] * I;
                const bool is_plasma = plasma_on && (k == last || k == prev_plasma)
                    && (strike.mT - strike.mStrokeAt[k]) < plasma_end;
                if (b > 0.012f || is_plasma || (dissolve > 0.f && plasma_dial <= 0.f)) { any_copy = true; break; }
            }

            if (any_copy)
            {
                // Merged corners. A node with exactly one child is a turn, not a fork, so the quad ending
                // there and the quad starting there share one side vector - the average of the two
                // segments' own sides - and both quads' corner edges at the node land on the same
                // two points. Forks (two or more children) and tips keep plain butts; crawl joints
                // lie in the ground plane and keep their own.
                const S32 node_n = (S32)strike.mChannel.size();
                if ((S32)mSoleChild.size() < node_n)
                {
                    mSoleChild.resize((size_t)node_n);
                    mJointSide.resize((size_t)node_n);
                }
                for (S32 i = 0; i < node_n; ++i) mSoleChild[(size_t)i] = -1;
                for (S32 i = 0; i < node_n; ++i)
                {
                    const S32 p = strike.mChannel[(size_t)i].mParent;
                    if (p >= 0)
                    {
                        mSoleChild[(size_t)p] = (mSoleChild[(size_t)p] == -1) ? i : -2;
                    }
                }
                for (S32 i = 0; i < node_n; ++i)
                {
                    mJointSide[(size_t)i].clear();
                    const S32 c = mSoleChild[(size_t)i];
                    if (c < 0) continue;
                    const SSStrikeNode& node = strike.mChannel[(size_t)i];
                    if (node.mParent < 0) continue;
                    if (node.mReachedAt > strike.mLeaderProgress
                        || strike.mChannel[(size_t)c].mReachedAt > strike.mLeaderProgress) continue;

                    // <SS:Nexii> The crawl merges too, in its OWN frame. Both of a joint's sides have to be built against the same reference or the averaged corner is meaningless, so a crawl joint - both segments lying in the ground plane and sided against z - merges against z, and an air joint merges against the view as before. A joint BETWEEN the two frames (the foot, where the falling channel becomes the crawl) is left as a plain butt, which is correct rather than lazy: there is no single side vector those two quads share. Without this the crawl was the only strip in the pass drawn as loose segments, and every change of direction showed the notch or the overlap. doc/atmo_magic_lightning_strike.md
                    const bool crawl_joint = node.mCrawl && strike.mChannel[(size_t)c].mCrawl;
                    if (!crawl_joint && (node.mCrawl || strike.mChannel[(size_t)c].mCrawl)) continue;

                    const LLVector3& p = node.mPos;
                    const LLVector3 view = crawl_joint ? LLVector3::z_axis : (p - cam);
                    LLVector3 s_in = (p - strike.mChannel[(size_t)node.mParent].mPos) % view;
                    LLVector3 s_out = (strike.mChannel[(size_t)c].mPos - p) % view;
                    s_in.normalize();
                    s_out.normalize();
                    LLVector3 merged = s_in + s_out;
                    if (merged.magVecSquared() < 1.e-10f) merged = s_in; else merged.normalize();
                    mJointSide[(size_t)i] = merged;
                }

                for (S32 k = 0; k < strike.mStrokeCount; ++k)
                {
                    const F32 b = strike.mStrokeBright[k] * I;
                    const F32 since = llmax(0.f, strike.mT - strike.mStrokeAt[k]);
                    const F32 scale_k = strike.mStrokeScale[k];
                    const bool is_plasma = plasma_on && (k == last || k == prev_plasma) && since < plasma_end;
                    const bool embers_on = dissolve > 0.f && plasma_dial <= 0.f;
                    if (b <= 0.012f && !is_plasma && !embers_on) continue;

                    const LLVector3 drift_off = wind * (strike.mStrokeDrift[k] * ribbon_drift);

                    // A late restrike re-lights a column that has already pinched into knots: the more the previous copy had cooled, the more distinct its beads.
                    const F32 bead_mul = (k == last && strike.mLastGap > 0.15f)
                        ? 1.f + 2.f * llclamp((strike.mLastGap - SSDissolve::LAG_S) / SSDissolve::PLASMA_S, 0.f, 1.f)
                        : 1.f;
                    const F32 copy_seed = seed01 + 0.173f * (F32)k;

                    for (S32 i = 0; i < node_n; ++i)
                    {
                        const SSStrikeNode& node = strike.mChannel[(size_t)i];
                        if (node.mParent < 0) continue;
                        if (node.mReachedAt > strike.mLeaderProgress) continue;
                        const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];

                        if (node.mCrawl)
                        {
                            if (!ground_show || strike.mT < 0.f || gd > 600.f) continue;
                        }

                        const F32 occ = occluding ? (node.mOcc + parent.mOcc) * 0.5f : 1.f;
                        if (occ < 0.01f) continue;

                        F32 wa_amber, hot_a, wb_amber, hot_b;
                        amberOf(parent, wa_amber, hot_a);
                        amberOf(node, wb_amber, hot_b);
                        if (node.mCrawl) { wa_amber = wb_amber = llmin(amber_str, 1.f); hot_a = hot_b = 0.f; }

                        // Ground strikes pivot about their foot: the wind carries the channel between strokes, never the attachment or the crawl.
                        const F32 da = ground ? llclamp(parent.mTipDistM / zone_m, 0.f, 1.f) : 1.f;
                        const F32 db = ground ? llclamp(node.mTipDistM / zone_m, 0.f, 1.f) : 1.f;
                        const LLVector3 pa = parent.mPos + drift_off * (da * da * (3.f - 2.f * da));
                        const LLVector3 pb = node.mPos + drift_off * (db * db * (3.f - 2.f * db));

                        // <SS:Nexii> The amber foot's widening belongs to the GLOW alone: core and sheath fan out toward the attachment, but the plasma copy below keeps the bare channel width, so the foot's triangle fades out with the glow instead of printing itself as a triangle of dissolving cloud.
                        F32 wa_thin = parent.mWidth * CORE_WIDTH_M * dist_scale;
                        F32 wb_thin = node.mWidth * CORE_WIDTH_M * dist_scale;
                        if (node.mCrawl)
                        {
                            wa_thin = llmax(llmin(wa_thin, 0.5f * CORE_WIDTH_M * dist_scale), ground_px_floor);
                            wb_thin = llmax(llmin(wb_thin, 0.5f * CORE_WIDTH_M * dist_scale), ground_px_floor);
                        }
                        const F32 wa = node.mCrawl ? wa_thin : wa_thin * (1.f + 0.9f * wa_amber);
                        const F32 wb = node.mCrawl ? wb_thin : wb_thin * (1.f + 0.9f * wb_amber);

                        const F32 v_unit = 2.f * CORE_WIDTH_M * dist_scale;
                        const F32 v0 = parent.mPathM / v_unit;
                        const F32 v1 = node.mPathM / v_unit;

                        // <SS:Nexii> The node's pop times still schedule when each stretch's plasma phase begins, but the pop itself belongs to the EMBERS fallback alone now: with the plasma on, the wide glow - core, sheath, the amber fan at the foot - simply FADES on its own stroke decay while the thin plasma copy dissolves over it. Discarding the core too is what broke every restrike: the fresh stroke's own segments started popping 50ms in, so it shredded while still at full brightness instead of flashing as a solid bolt over the old wisps.
                        const F32 pop_b = SSDissolve::LAG_S + node.mThr * span_s;
                        const F32 pop_a = SSDissolve::LAG_S + parent.mThr * span_s;
                        F32 seg = 1.f;
                        F32 ember = 0.f;
                        if (embers_on)
                        {
                            const F32 age = since - pop_b;
                            if (age >= 0.f)
                            {
                                seg = llclamp(1.f - age * 90.f, 0.f, 1.f);
                                ember = llclamp(1.f - age / (SSDissolve::EMBER_S / dissolve), 0.f, 1.f);
                            }
                        }

                        F32 u_a = 0.f, u_b = 0.f;
                        if (is_plasma)
                        {
                            const F32 crawl_speed = node.mCrawl ? 1.6f : 1.f;
                            u_a = llclamp((since - pop_a) / (SSDissolve::PLASMA_S * lerp(1.f, SSDissolve::PLASMA_FOOT_MULT, wa_amber) / crawl_speed), 0.f, 1.f);
                            u_b = llclamp((since - pop_b) / (SSDissolve::PLASMA_S * lerp(1.f, SSDissolve::PLASMA_FOOT_MULT, wb_amber) / crawl_speed), 0.f, 1.f);
                        }

                        const LLColor3 core_a = mix3(mix3(CORE_COLOR, AMBER_CORE, wa_amber), AMBER_HOT, hot_a);
                        const LLColor3 core_b = mix3(mix3(CORE_COLOR, AMBER_CORE, wb_amber), AMBER_HOT, hot_b);
                        const LLColor3 glow_a = mix3(GLOW_COLOR, AMBER_GLOW, wa_amber);
                        const LLColor3 glow_b = mix3(GLOW_COLOR, AMBER_GLOW, wb_amber);

                        // The crawl is no longer excluded here: the joint pass has built its corners in the ground plane, and a joint it declined to merge - the foot, where the two frames meet - left a zero vector that reads as the plain butt it should be.
                        const LLVector3* start_side = (mJointSide[(size_t)node.mParent].magVecSquared() > 0.f)
                            ? &mJointSide[(size_t)node.mParent] : nullptr;
                        const LLVector3* end_side = (mJointSide[(size_t)i].magVecSquared() > 0.f)
                            ? &mJointSide[(size_t)i] : nullptr;

                        const F32 bo = b * occ * seg;
                        if (bo > 0.012f)
                        {
                            Vertex va, vb;
                            va.mUV1.set(bead_mul, 0.f);
                            vb.mUV1.set(bead_mul, 0.f);

                            // The sheath: dimmer, wide, plain profile - the glow gradient, which fades with its stroke's own decay; only the thin plasma copy dissolves.
                            {
                                va.mCol = tint8(glow_a * 0.22f, 0.3f);
                                vb.mCol = tint8(glow_b * 0.22f, 0.3f);
                                va.mAux.set(copy_seed, wa_amber, 0.f);
                                vb.mAux.set(copy_seed, wb_amber, 0.f);
                                va.mCtl.set(bo, 0.f, 0.f, 1.f);
                                vb.mCtl.set(bo, 0.f, 0.f, 1.f);
                                const F32 sheath_mult = node.mCrawl ? 2.5f : GLOW_WIDTH_MULT;
                                if (node.mCrawl)
                                {
                                    groundRibbon(pa, pb, wa * sheath_mult, wb * sheath_mult, v0, v1, va, vb, start_side, end_side);
                                }
                                else
                                {
                                    ribbon(pa, pb, wa * sheath_mult, wb * sheath_mult, v0, v1, va, vb, start_side, end_side);
                                }
                            }

                            // The core: amber-graded per end, above white at the foot, beaded along its length.
                            va.mCol = tint8(core_a, 1.f);
                            vb.mCol = tint8(core_b, 1.f);
                            va.mAux.set(copy_seed, wa_amber, 0.f);
                            vb.mAux.set(copy_seed, wb_amber, 0.f);
                            va.mCtl.set(bo * (1.f + 2.2f * wa_amber * wa_amber), plasma_lod, 0.f, 0.f);
                            vb.mCtl.set(bo * (1.f + 2.2f * wb_amber * wb_amber), plasma_lod, 0.f, 0.f);
                            if (node.mCrawl)
                            {
                                groundRibbon(pa, pb, wa, wb, v0, v1, va, vb, start_side, end_side);
                            }
                            else
                            {
                                ribbon(pa, pb, wa, wb, v0, v1, va, vb, start_side, end_side);
                            }
                            mStats.mSegments++;
                        }

                        // <SS:Nexii> The plasma is the THIN CHANNEL DISSOLVING, not a second object laid over it: the same ribbon geometry at the bare channel width - no growth, not even the amber fan - taken away by an animated alpha mask in the shader while a flowmap curls what is left. The old 2x-to-6x growth was the original wave fault, and even the 1.4x that replaced it put dissolving cloud into the foot's widening triangle; the blobs belong ON the core, and the wide glow around it is the sheath's to fade. Everything that makes it read as plasma happens in the mask and the flow, and both need a strip no wider than the thing they are eating. doc/atmo_magic_lightning_strike.md
                        if (is_plasma && (u_a > 0.f || u_b > 0.f))
                        {
                            // <SS:Nexii> The wisps keep nearly their full glow to the end - the shader's mask is what kills them, not a fade. A ~30% slide over the plasma phase is all the cooling the eye needs; the old exp(-t/0.2s) had the copy at a fifth of its brightness before the mask had eaten half of it, which read as the whole column fading together. Per-end u so the amber foot, whose phase runs slower, cools slower with it.
                            const F32 pb_a = (u_a > 0.f) ? I * scale_k * 0.5f * (1.f - 0.30f * u_a) * plasma_dial * occ : 0.f;
                            const F32 pb_b = (u_b > 0.f) ? I * scale_k * 0.5f * (1.f - 0.30f * u_b) * plasma_dial * occ : 0.f;
                            if (pb_a > 0.01f || pb_b > 0.01f)
                            {
                                // Which way world up runs in the strip's own frame, so the shader's convection lifts the mask along the real vertical instead of along whichever way the quad happens to be wound. +1 when the strip runs straight down, 0 where it lies flat and convection has nowhere to go along it.
                                LLVector3 sdir = pb - pa;
                                const F32 up_along = (sdir.normalize() > 0.f) ? -sdir.mV[VZ] : 0.f;

                                Vertex va, vb;
                                va.mUV1.set(bead_mul, up_along);
                                vb.mUV1.set(bead_mul, up_along);
                                va.mCol = tint8(core_a, 0.5f);
                                vb.mCol = tint8(core_b, 0.5f);
                                va.mAux.set(copy_seed, wa_amber, llmax(u_a, 0.001f));
                                vb.mAux.set(copy_seed, wb_amber, llmax(u_b, 0.001f));
                                va.mCtl.set(pb_a, plasma_lod, warp, 0.f);
                                vb.mCtl.set(pb_b, plasma_lod, warp, 0.f);
                                // The bare channel width, no growth at all: the blobs form along the thin core and stay there, dissolving in place.
                                if (node.mCrawl)
                                {
                                    groundRibbon(pa, pb, wa_thin, wb_thin, v0, v1, va, vb, start_side, end_side);
                                }
                                else
                                {
                                    ribbon(pa, pb, wa_thin, wb_thin, v0, v1, va, vb, start_side, end_side);
                                }
                                mStats.mPlasma++;
                            }
                        }

                        // The plasma-off fallback: the dying spark a popped segment leaves behind.
                        if (ember > 0.03f)
                        {
                            const U32 eh = hash3(strike_seed ^ (U32)i * 977u);
                            const F32 ea = hashUnit(eh ^ 7u) * F_TWO_PI;
                            const F32 et = (hashUnit(eh ^ 13u) - 0.5f) * 1.3f;
                            const LLVector3 edir(cosf(ea) * cosf(et), sinf(ea) * cosf(et), sinf(et));
                            const F32 elen = llmax(wa, wb) * (1.5f + 3.5f * hashUnit(eh ^ 19u));
                            const F32 er = llmax(wa, wb) * (0.2f + 0.15f * hashUnit(eh ^ 23u));
                            const F32 eb = I * ember * (0.30f + 0.25f * hashUnit(eh ^ 29u));
                            if (eb > 0.02f)
                            {
                                const LLVector3 emid = (pa + pb) * 0.5f;
                                const LLVector3 e0 = emid - edir * elen * 0.5f;
                                const LLVector3 e1 = emid + edir * elen * 0.5f;
                                const LLColor3 ec = (wb_amber > 0.4f) ? SPARK_1 : LLColor3(core_b.mV[0], core_b.mV[1] * 0.95f, core_b.mV[2] * 0.9f);
                                Vertex ve;
                                ve.mCol = tint8(ec, 1.f);
                                ve.mUV1.set(1.f, 0.f);
                                ve.mAux.set(0.f, 0.f, 0.f);
                                ve.mCtl.set(eb, 0.f, 0.f, 3.f);
                                ribbon(e0, e1, er * 0.4f, er * 0.7f, 0.f, 1.f, ve, ve);
                            }
                        }
                    }
                }
            }
        }

        // The charge swarm, held through the leader and turning amber at contact.
        const F32 swarm_env = fx.mHeld;
        if (swarm_env > 0.001f && (!ground || fx.mGroundOn) && !fx.mHidden)
        {
            const U32 seed = (U32)(strike.mFireAt * 271.0);
            const F32 spread = 6.f * (1.2f - swarm_env);
            const F32 amber_mix = (ground && strike.mT >= 0.f) ? llclamp(strike.mT / 0.05f, 0.f, 1.f) : 0.f;
            const LLColor3 swarm_col = mix3(SPARK_COLOR, SPARK_1, amber_mix);

            // The ionizing field gathering around the attachment: a swarm of tiny sparks, each
            // living a fraction of a second, sprinting a small erratic spiral arc and vanishing.
            // Entirely stateless - a spark's whole life is hashed out of (strike, index,
            // respawn count) and the clock, so no per-spark state ticks and the field cannot
            // desync from its strike.
            const S32 count = (S32)(CHARGE_SPARK_MAX * swarm_env * (0.4f + 0.6f * I));

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 71u;

                const F32 life = CHARGE_SPARK_LIFE_S * (0.45f + 0.55f * hashUnit(h ^ 3u));
                const F32 duty = 2.f + 2.5f * hashUnit(h ^ 5u);
                const F32 offset = hashUnit(h ^ 7u);

                const F32 cycle = life * duty;
                const F32 age = fmodf(tnow / cycle + offset, 1.f) * cycle;
                if (age > life) continue;

                const U32 g = hash3(h ^ ((U32)(tnow / cycle + offset) * 31u + 97u));

                const F32 ang0 = hashUnit(g) * F_TWO_PI;
                const F32 rad = spread * sqrtf(hashUnit(g ^ 11u));
                const F32 field_h = 0.8f + spread * 0.8f;
                const LLVector3 spawn = strike.mGround
                    + LLVector3(cosf(ang0) * rad, sinf(ang0) * rad,
                                0.3f + field_h * hashUnit(g ^ 13u) * hashUnit(g ^ 13u));

                const F32 az = hashUnit(g ^ 17u) * F_TWO_PI;
                const F32 tilt = (hashUnit(g ^ 19u) - 0.5f) * 1.9f;
                const LLVector3 axis(cosf(az) * cosf(tilt), sinf(az) * cosf(tilt), sinf(tilt));
                LLVector3 arc_a = axis % LLVector3::z_axis;
                if (arc_a.normalize() <= 0.f) arc_a = LLVector3::x_axis;
                LLVector3 arc_b = axis % arc_a;
                arc_b.normalize();

                const F32 spin = (1.5f + 2.5f * hashUnit(g ^ 23u)) * (life * 4.f) * F_TWO_PI
                    * (hashUnit(g ^ 29u) < 0.5f ? -1.f : 1.f);
                const F32 w1 = hashUnit(g ^ 31u) * F_TWO_PI;
                const F32 r_arc = 0.1f + 0.35f * hashUnit(g ^ 37u);
                const F32 sway_hz = 4.f + 5.f * hashUnit(g ^ 41u);
                const F32 sway_amp = 0.7f + 0.9f * hashUnit(g ^ 43u);
                const F32 breathe = 0.4f + 0.6f * hashUnit(g ^ 47u);
                const F32 breathe_hz = 2.f + 2.f * hashUnit(g ^ 51u);
                const LLVector3 drift = axis * (0.5f + 0.8f * hashUnit(g ^ 53u))
                    + LLVector3(0.f, 0.f, 0.2f + 0.8f * hashUnit(g ^ 59u));

                auto sparkPos = [&](F32 t) -> LLVector3
                {
                    const F32 uu = llclamp(t / life, 0.f, 1.f);
                    const F32 a = w1 + uu * spin
                        + sinf(t * sway_hz * F_TWO_PI + w1 * 3.f) * sway_amp;
                    const F32 r = r_arc * (1.f - breathe
                        + breathe * (0.5f + 0.5f * sinf(t * breathe_hz * F_TWO_PI + w1)));
                    return spawn + drift * t + (arc_a * cosf(a) + arc_b * sinf(a)) * r;
                };

                const F32 env = sinf(age / life * F_PI);
                const F32 a = env * env * swarm_env;
                if (a < 0.03f) continue;

                const F32 r = 0.02f + 0.03f * hashUnit(g ^ 61u);

                Vertex vs;
                vs.mCol = tint8(swarm_col, 0.6f);
                vs.mUV1.set(1.f, 0.f);
                vs.mAux.set(0.f, 0.f, 0.f);
                vs.mCtl.set(a, 0.f, 0.f, 3.f);
                ribbon(sparkPos(llmax(0.f, age - 0.03f)), sparkPos(age), r * 0.35f, r, 0.f, 1.f, vs, vs);
            }
        }

        // <SS:Nexii> Impact sparks off the spawn table: closed-form arcs, hot metal cooling from white through orange to red, the head widened to a pixel floor at range with its brightness handed back so a floored spark dims instead of glaring, secondaries skittering where each one lands.
        if (sparks_on && ground_show && strike.mT >= 0.f && gd < 1200.f && !strike.mSparks.empty())
        {
            for (const SSStrikeSpark& sp : strike.mSparks)
            {
                const F32 t = strike.mT - sp.mT0;
                if (t <= 0.f) continue;
                const bool landed = sp.mHit > 0.f && t > sp.mHit;
                if (!landed && t > sp.mLife) continue;
                if (landed && t - sp.mHit >= SSGroundShow::SECONDARY_LIFE_S) continue;

                const LLVector3 dir(sp.mCos, sp.mSin, 0.f);
                if (!landed)
                {
                    LLVector3 pos = sp.mFrom + dir * (sp.mVH * t);
                    pos.mV[VZ] += sp.mVZ * t - 0.5f * SPARK_GRAVITY * t * t;
                    if (sp.mHit <= 0.f && pos.mV[VZ] < sp.mFrom.mV[VZ] - 60.f) continue;

                    const F32 uu = llclamp(t / sp.mLife, 0.f, 1.f);
                    const F32 fade = powf(1.f - uu, 1.5f) * I;
                    if (fade >= 0.02f)
                    {
                        const LLVector3 vel(sp.mCos * sp.mVH, sp.mSin * sp.mVH, sp.mVZ - SPARK_GRAVITY * t);
                        const LLVector3 tail = pos - vel * 0.035f;
                        const F32 r = llmax(sp.mRadius, ground_px_floor);
                        Vertex vs;
                        vs.mCol = tint8(ramp3(SPARK_0, SPARK_1, SPARK_2, uu), 0.5f);
                        vs.mUV1.set(1.f, 0.f);
                        vs.mAux.set(0.f, 0.f, 0.f);
                        vs.mCtl.set(fade * (sp.mRadius / r), 0.f, 0.f, 3.f);
                        ribbon(tail, pos, r * 0.35f, r, 0.f, 1.f, vs, vs);
                        mStats.mSparks++;
                    }
                }

                if (landed && secondary > 0.f)
                {
                    const F32 sec_age = t - sp.mHit;
                    LLVector3 hit = sp.mFrom + dir * (sp.mVH * sp.mHit);
                    hit.mV[VZ] = sp.mLandZ;

                    const U32 h = sp.mSeed;
                    const S32 sec_n = 1 + ((hashUnit(h ^ 37u) < 0.55f) ? 1 : 0);
                    const U32 sg = hash3(h ^ 0x5151u);
                    for (S32 j = 0; j < sec_n; ++j)
                    {
                        const U32 sh = hash3(sg + (U32)j * 331u);

                        const F32 s_ang = hashUnit(sh ^ 3u) * F_TWO_PI;
                        const F32 s_spd = (1.5f + 4.5f * hashUnit(sh ^ 7u)) * (0.4f + I * 0.6f) * secondary;
                        const F32 s_rise = 0.3f + 1.1f * hashUnit(sh ^ 11u);
                        const F32 s_life = SSGroundShow::SECONDARY_LIFE_S * (0.45f + 0.55f * hashUnit(sh ^ 13u));
                        if (sec_age > s_life) continue;

                        const F32 s_vz = s_spd * s_rise;
                        const LLVector3 s_vel(cosf(s_ang) * s_spd, sinf(s_ang) * s_spd, s_vz);
                        LLVector3 s_pos = hit + s_vel * sec_age;
                        s_pos.mV[VZ] -= 0.5f * SPARK_GRAVITY * sec_age * sec_age;
                        if (s_pos.mV[VZ] < sp.mLandZ) continue;

                        const F32 s_u = sec_age / s_life;
                        const F32 s_a = (1.f - s_u) * (1.f - s_u) * I * 0.55f;
                        if (s_a < 0.02f) continue;

                        const LLVector3 s_tail = s_pos - s_vel * 0.025f;
                        const F32 s_r = llmax(0.03f + 0.03f * hashUnit(sh ^ 17u), ground_px_floor * 0.7f);

                        Vertex vs;
                        vs.mCol = tint8(mix3(SPARK_1, SPARK_2, s_u), 0.4f);
                        vs.mUV1.set(1.f, 0.f);
                        vs.mAux.set(0.f, 0.f, 0.f);
                        vs.mCtl.set(s_a, 0.f, 0.f, 3.f);
                        ribbon(s_tail, s_pos, s_r * 0.35f, s_r, 0.f, 1.f, vs, vs);
                        mStats.mSparks++;
                    }
                }
            }
        }

        // <SS:Nexii> The aura and the flare on the same discs: violet St. Elmo's haze breathing in from nothing as the charge gathers (off by default - the aura dial ships at 0), held through the leader; at contact the amber flare rises in on top of it (the sum, no palette switch), a power-law spike at the attachment, fading with the bolt one decay behind it while the violet dies under it in 60ms. The flare has its own dial and survives an aura of zero, so the amber ground-strike glow stays.
        const bool aura_visible = (fx.mHeld > 0.005f || strike.mHit > 0.005f) && (!ground || fx.mGroundOn);
        if (aura_visible)
        {
            const F32 guard = llclamp((gd - 1.f) / 1.5f, 0.f, 1.f);
            const F32 envC = llclamp(fx.mHeld / 0.35f, 0.f, 1.f);
            const F32 env_c = envC * envC * (3.f - 2.f * envC) * powf(fx.mHeld, 1.5f);
            const F32 grow_t = llclamp(strike.mChargeHeld / 0.6f, 0.f, 1.f);
            const F32 grow = 0.30f + 0.70f * grow_t * grow_t * (3.f - 2.f * grow_t);
            const S32 patches = (gd < 40.f) ? 2 : SSGroundShow::AURA_PATCHES;

            for (S32 i = 0; i <= patches; ++i)
            {
                const bool centre = (i == patches);
                const F32 h = hashUnit(strike_seed ^ ((U32)i * 61u + 0xc0fau));
                if (!centre && strike.mChargeHeld <= 0.06f * (F32)i && strike.mHit <= 0.005f) continue;

                const F32 pulse = 0.45f + 0.55f * sinf(tnow * (1.2f + 1.6f * h) + 7.f * h) * sinf(tnow * (1.2f + 1.6f * h) + 7.f * h);
                const F32 A_c = AURA_GAIN * aura_dial * env_c * pulse * (0.4f + 0.6f * I) * guard;
                const F32 A_h = hit_dial * strike.mHit * I * (centre ? 1.0f : 0.5f);
                if (A_c + A_h < 0.003f) continue;

                const LLColor3 rgb_sum = CORONA_TINT * A_c + FLARE_MID * A_h;
                const F32 m = llmax(1.f, llmax(rgb_sum.mV[0], llmax(rgb_sum.mV[1], rgb_sum.mV[2])));
                const LLColor3 tint = rgb_sum * (1.f / m);
                const F32 q = A_h / (A_c + A_h + 1.e-3f);

                LLVector3 center_true;
                F32 r_right, r_up, surf;
                if (centre)
                {
                    const F32 R = strike.mAuraCentreR * ground_scale * (strike.mPositive ? 1.4f : 1.f);
                    r_right = R * 1.6f;
                    r_up = R * 0.8f;
                    surf = ground ? SSLightning::surfaceZ(strike.mGround) : strike.mGround.mV[VZ];
                    center_true = strike.mGround;
                    center_true.mV[VZ] = surf + 0.55f * r_up;
                }
                else
                {
                    r_right = r_up = strike.mAuraR[i] * ground_scale * grow;
                    surf = strike.mAuraSurfZ[i];
                    center_true = strike.mAuraPos[i];
                    center_true.mV[VZ] = surf + 0.5f * r_up;
                }
                // A fork's or sheet's aura hangs in cloud with no surface under it: nothing to fade against.
                if (!ground) surf = center_true.mV[VZ] - 100.f * r_up;
                const F32 d = (center_true - cam).magVec();
                const F32 soft_m = (soft_on ? llmax(2.f, 0.5f * r_up) * llmax(1.f, d / 250.f) * soft_dial : 0.f);
                disc(center_true, r_right, r_up, surf, tint, m, 0.25f, q, soft_m);
            }
        }

        // <SS:Nexii> The ground fire: blobs along the crawl and the impact fan igniting outward over the first frames, each on its own share of the slow tail, cooling from yellow-orange to dim red, re-lit by every restrike; the sparks' landing embers age from their own touchdown.
        if (ground_show && strike.mT >= 0.f && gd < 800.f && fire_dial > 0.f && !strike.mFireBlobs.empty())
        {
            for (const SSStrikeFire& fb : strike.mFireBlobs)
            {
                const F32 age = (fb.mEmber ? strike.mT : strike.mPlasmaSince) - fb.mIgnite;
                if (age < 0.f) continue;
                const F32 h = hashUnit(fb.mSeed);
                const F32 rise = llmin(1.f, age / SSGroundShow::FIRE_RISE_S);
                const F32 flicker = 0.8f + 0.2f * sinf(tnow * (9.f + 6.f * h) + 7.f * h);
                const F32 f = rise * rise * expf(-llmax(0.f, age - SSGroundShow::FIRE_RISE_S) / (tau_f * fb.mLifeMul)) * flicker;
                if (f < 0.02f) continue;

                const F32 R = llmax(fb.mRadius * ground_scale, gd * 0.002f);
                LLVector3 center_true = fb.mPos;
                center_true.mV[VZ] += 0.35f * R;
                const LLColor3 tint = (f > 0.6f) ? mix3(FIRE_1, FIRE_0, (f - 0.6f) / 0.4f) : mix3(FIRE_2, FIRE_1, f / 0.6f);
                const F32 d = (center_true - cam).magVec();
                const F32 soft_m = (soft_on ? llmax(1.f, 0.5f * R) * llmax(1.f, d / 250.f) * soft_dial : 0.f);
                disc(center_true, R, R, fb.mPos.mV[VZ], tint, 1.2f * f * I * fire_dial, 0.35f, 0.25f, soft_m);
            }
        }
    }

    drawBatch();

    // <SS:Nexii> The steam burst, in its own batch because it is the only element here that is not a light: scattered water vapour, alpha-blended over the scene rather than added to it. The alpha factors are the precipitation pass's - source ZERO, destination ONE_MINUS_SRC_ALPHA - so a puff standing in front of the channel takes the bolt's bloom seed down with it instead of glowing through. Drawn after the additive pass for the same reason: the cloud is in front of the light it came from. doc/atmo_magic_lightning_strike.md
    static LLCachedControl<F32> steam_setting(gSavedSettings, "SSAtmoLightningSteam", 1.f);
    const F32 steam_dial = llclamp((F32)steam_setting, 0.f, 3.f);
    if (steam_dial > 0.f)
    {
        bool any_steam = false;
        for (const SSStrike& s : lightning->strikes())
        {
            if (s.mSteamPeak > 0.f && s.mT >= 0.f) { any_steam = true; break; }
        }

        if (any_steam)
        {
            beginBatch();
            for (size_t si = 0; si < lightning->strikes().size(); ++si)
            {
                const SSStrike& strike = lightning->strikes()[si];
                if (strike.mSteamPeak <= 0.f || strike.mT < 0.f) continue;
                if (!facts[si].mOnScreen || !facts[si].mGroundOn) continue;

                for (const SSStrikeSteam& sb : strike.mSteam)
                {
                    if (sb.mWater <= 0.01f) continue;
                    const F32 t = strike.mT - sb.mDelay;
                    if (t < 0.f || t > SSGroundShow::STEAM_LIFE_S) continue;

                    // The boil is over in a breath and the cloud spends the rest of its life rising and thinning: the width snaps out over the burst, then keeps growing slowly while the whole thing lifts and its density falls away.
                    const F32 age = t / SSGroundShow::STEAM_LIFE_S;
                    const F32 burst = llclamp(t / SSGroundShow::STEAM_BURST_S, 0.f, 1.f);
                    const F32 r = sb.mRadius * (0.35f + 0.85f * sqrtf(burst) + 0.9f * age);
                    const F32 density = sb.mWater * steam_dial * burst * (1.f - age) * (1.f - age);
                    if (density < 0.004f) continue;

                    const LLVector3 center = sb.mPos
                        + LLVector3(0.f, 0.f, 0.4f * r + SSGroundShow::STEAM_RISE_M_S * t)
                        + wind * (t * 0.35f);

                    LLVector3 right, up;
                    if (!billboardAxes(center, cam, right, up)) continue;

                    // Lit by the channel while the channel is still there: the puff blows white under a live bolt and settles to its ambient grey as the light goes.
                    Vertex v;
                    v.mCol = tint8(mix3(STEAM_COLOR, GLOW_COLOR * 0.5f + LLColor3(0.5f, 0.5f, 0.5f),
                                        llclamp(strike.mChannelBrightness * 1.5f, 0.f, 1.f)), 0.f);
                    v.mUV1.set(0.f, 0.f);
                    v.mCtl.set(density, 0.f, 0.f, 7.f);
                    Vertex c[4];
                    for (S32 i = 0; i < 4; ++i)
                    {
                        const F32 sx = (i & 1) ? 1.f : -1.f;
                        const F32 sy = (i & 2) ? 1.f : -1.f;
                        c[i] = v;
                        c[i].mPos = center + right * (r * sx) + up * (r * sy);
                        c[i].mUV.set((i & 1) ? 1.f : 0.f, (i & 2) ? 1.f : 0.f);
                        c[i].mAux.set(hashUnit(sb.mSeed), age, 0.f);
                    }
                    pushQuad(c[0], c[1], c[2], c[3]);
                    mStats.mSteam++;
                }
            }

            if (mQuadCount > 0)
            {
                gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                              LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
                drawBatch();
                gGL.setSceneBlendType(LLRender::BT_ADD);
            }
        }
    }

    if (markers)
    {
        bool any_pending = false;
        for (const SSStrike& s : lightning->strikes())
        {
            if (s.mT <= -SSLightning::MARKER_HIDE_S && !s.mDone) { any_pending = true; break; }
        }

        if (any_pending)
        {
            LLGLDepthTest marker_depth(GL_FALSE);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, false);
            beginBatch();

            auto line = [&](const LLVector3& a, const LLVector3& b, F32 w, const LLColor4& c)
            {
                Vertex v;
                v.mCol = tint8(LLColor3(c.mV[0], c.mV[1], c.mV[2]), c.mV[3]);
                v.mUV1.set(0.f, 0.f);
                v.mAux.set(0.f, 0.f, 0.f);
                v.mCtl.set(1.f, 0.f, 0.f, 5.f);
                ribbon(a, b, w, w, 0.f, 1.f, v, v);
            };

            for (size_t si = 0; si < lightning->strikes().size(); ++si)
            {
                const SSStrike& strike = lightning->strikes()[si];
                if (strike.mT > -SSLightning::MARKER_HIDE_S || strike.mDone) continue;
                if (!facts[si].mOnScreen) continue;

                // One colour per kind, and only the geometry that kind actually has - a sheet has
                // no channel or attachment, so the old origin-to-ground line read as a down-strike.
                const LLColor4& kc = SSLightning::kindDebugColor(strike.mKind);
                const LLColor4 crawl_c(1.f, 0.55f, 0.1f, 0.85f);
                const LLColor4 box_c = facts[si].mHidden ? LLColor4(1.f, 0.2f, 0.2f, 0.6f) : LLColor4(0.2f, 1.f, 0.3f, 0.5f);

                const F32 mw = llmax(0.4f, strike.mDistanceM * 0.004f);

                if (strike.mKind == STRIKE_SHEET)
                {
                    const F32 r = llmax(40.f, strike.mDistanceM * 0.06f);
                    const S32 SIDES = 8;
                    for (S32 e = 0; e < SIDES; ++e)
                    {
                        const F32 a0 = (F32)e / (F32)SIDES * F_TWO_PI;
                        const F32 a1 = (F32)(e + 1) / (F32)SIDES * F_TWO_PI;
                        line(strike.mOrigin + LLVector3(cosf(a0) * r, sinf(a0) * r, 0.f),
                             strike.mOrigin + LLVector3(cosf(a1) * r, sinf(a1) * r, 0.f), mw, kc);
                    }
                    continue;
                }

                if (!strike.mChannel.empty())
                {
                    for (const SSStrikeNode& node : strike.mChannel)
                    {
                        if (node.mParent < 0) continue;
                        const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];
                        line(parent.mPos, node.mPos, node.mCrawl ? llmax(0.15f, mw * 0.5f) : mw, node.mCrawl ? crawl_c : kc);
                    }
                }
                else
                {
                    line(strike.mOrigin, strike.mGround, mw, kc);
                }

                if (strike.mKind != STRIKE_GROUND) continue;

                // The ground show's box, green while its query says visible, red while hidden.
                const LLVector3& lo = strike.mGroundBoxMin;
                const LLVector3& hi = strike.mGroundBoxMax;
                LLVector3 c[8];
                for (S32 i = 0; i < 8; ++i)
                {
                    c[i].set((i & 1) ? hi.mV[VX] : lo.mV[VX], (i & 2) ? hi.mV[VY] : lo.mV[VY], (i & 4) ? hi.mV[VZ] : lo.mV[VZ]);
                }
                static const S32 edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
                for (S32 e = 0; e < 12; ++e)
                {
                    line(c[edges[e][0]], c[edges[e][1]], mw * 0.4f, box_c);
                }
            }

            drawBatch();
        }
    }

    LLVertexBuffer::unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, true);
    gSSLightningProgram.unbind();
}
