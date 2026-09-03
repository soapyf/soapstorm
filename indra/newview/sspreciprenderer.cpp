/**
 * @file sspreciprenderer.cpp
 * @brief See sspreciprenderer.h.
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

#include "sspreciprenderer.h"
#include "ssatmomagic.h"

#include "lldrawable.h"
#include "lldrawpoolalpha.h"
#include "llglstates.h"
#include "llrender.h"
#include "llfasttimer.h"
#include "llstaticstringtable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvovolume.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

static LLTrace::BlockTimerStatHandle FTM_SS_PRECIP_RENDER("Atmo Magic Render");
static LLTrace::BlockTimerStatHandle FTM_SS_PRECIP_BUILD("Build Quads");
static LLTrace::BlockTimerStatHandle FTM_SS_PRECIP_DRAW("Draw");

static const U32 MAX_QUADS = 48000;
static const U32 VB_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                         | LLVertexBuffer::MAP_TANGENT
                         | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;

static const F32 RIPPLE_DEPTH_BIAS = 0.0035f;
static const F32 RIPPLE_DEPTH_BIAS_MIN = 0.02f;

static const F32 RIPPLE_NORMAL_LIFT = 0.09f;

static const F32 CROWN_START_SCALE = 0.15f;
static const F32 CROWN_END_SCALE = 1.5f;

// Smoothstep clamp.
static inline F32 smooth01(F32 t)
{
    t = llclamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Fade in/out across a tier's distance band, so tiers cross-fade instead of popping.
static F32 bandFade(F32 dist, const F32* band)
{
    const F32 f_in = (band[1] > band[0]) ? smooth01((dist - band[0]) / (band[1] - band[0])) : 1.f;
    const F32 f_out = smooth01((band[3] - dist) / llmax(0.01f, band[3] - band[2]));
    return llmin(f_in, f_out);
}

// Fade in at birth and out toward death; sheets ease in slower.
static F32 ageFade(const SSPrecipParticle& p)
{
    const F32 t_in = (p.mTier == TIER_SHEETS) ? 0.8f : 0.15f;
    const F32 t_out = ssPrecipFadeOut(p.mTier);
    return llmin(1.f, p.mAge / t_in) * llclamp((p.mMaxAge - p.mAge) / t_out, 0.f, 1.f);
}

// <SS:Nexii> Fade in over the top of a falling run: a drop materializes across the first stretch of its fall rather than popping in, fully visible once past it. The band caps at SS_PRECIP_TOP_FADE and scales with the run, so a short near drop finishes fading half-way down (staying dense at the ground) while deck-spawned curtains keep the full band off the deck. Nothing that doesn't fall from a top (risers, ripples, streams, drift) carries a mFallTop and stays at full alpha.
static F32 topFade(const SSPrecipParticle& p)
{
    if (p.mFallTop <= 0.f) return 1.f;
    const F32 fallen = p.mFallTop - p.mPos.mV[VZ];
    const F32 span = llmax(p.mFallTop - p.mFloorZ, 0.5f);
    const F32 fade = llmin(SS_PRECIP_TOP_FADE, span * 0.6f);
    return smooth01(fallen / fade);
}

// Emits a stream particle as a chain of gravity-bent ribbon segments with scrolling texture.
template <typename EmitFn>
U32 SSPrecipRenderer::emitStream(const SSPrecipParticle& p, F32 alpha, F32 stretch,
                                 EmitFn& emit)
{
    const F32 fall_time = llmax(p.mPlaneD, 0.05f);
    const F32 g = 9.81f;

    LLVector3 along(p.mVel.mV[VX], p.mVel.mV[VY], 0.f);
    if (along.normVec() < 0.0001f) along = LLVector3::x_axis;
    along = LLVector3::z_axis % along;
    if (along.normVec() < 0.0001f) along = LLVector3::y_axis;

    if (p.mRunSlope != 0.f)
    {
        along.mV[VZ] += p.mRunSlope;
        along.normVec();
    }

    const F32 u_rep = llmax(0.05f, p.mSizeY);
    F32 u0 = SSAtmoNoise::hash01(p.mSeed ^ 0x6F4A21B3u);
    F32 u1 = u0 + u_rep;

    if (SSAtmoNoise::hashU32(p.mSeed ^ 0x2B7E1516u) & 1u)
    {
        const F32 swap = u0;
        u0 = u1;
        u1 = swap;
    }
    const F32 repeats = llmax(0.05f, p.mFloorZ);

    LLVector3 pt[SS_STREAM_SEGMENTS + 1];
    F32 walked[SS_STREAM_SEGMENTS + 1];
    pt[0] = p.mPos;
    walked[0] = 0.f;

    for (S32 k = 1; k <= SS_STREAM_SEGMENTS; ++k)
    {
        const F32 t = fall_time * (F32)k / (F32)SS_STREAM_SEGMENTS;
        pt[k] = p.mPos + p.mVel * t + p.mNormal * (0.5f * t * t)
              - LLVector3(0.f, 0.f, 0.5f * g * t * t);
        walked[k] = walked[k - 1] + (pt[k] - pt[k - 1]).magVec();
    }

    const F32 total = llmax(walked[SS_STREAM_SEGMENTS], 0.01f);

    const F32 boost = 1.f + p.mGlow * 1.5f;
    const LLColor4U tint((U8)llmin((S32)(p.mTint.mV[0] * boost), 255),
                         (U8)llmin((S32)(p.mTint.mV[1] * boost), 255),
                         (U8)llmin((S32)(p.mTint.mV[2] * boost), 255),
                         255);

    struct End
    {
        F32 v;
        LLColor4U col;
    };

    auto endAt = [&](S32 k) -> End
    {
        const F32 f = walked[k] / total;

        const F32 f_tex = lerp(f, (F32)k / (F32)SS_STREAM_SEGMENTS, stretch);

        const F32 fade = 1.f - llclamp((f - 0.66f) / 0.34f, 0.f, 1.f);

        End e;
        e.v = p.mPhase - f_tex * repeats;
        e.col = tint;
        e.col.mV[3] = (U8)llclamp((S32)(alpha * fade * 255.f), 0, 255);
        return e;
    };

    U32 written = 0;
    End top = endAt(0);

    for (S32 k = 0; k < SS_STREAM_SEGMENTS; ++k)
    {
        const End bot = endAt(k + 1);

        if ((pt[k + 1] - pt[k]).magVecSquared() < 1e-8f) { top = bot; continue; }

        if (top.col.mV[3] < 1 && bot.col.mV[3] < 1) { top = bot; continue; }

        emit(pt[k], pt[k + 1], along, p.mSizeX, top.col, bot.col,
             u0, u1, top.v, bot.v);

        ++written;
        top = bot;
    }

    return written;
}

// Grows the shared vertex buffer to at least this many quads; indices are a fixed quad pattern written once.
bool SSPrecipRenderer::ensureBuffer(U32 quads)
{
    if (mVB.notNull() && mVBQuads >= quads) return true;

    U32 alloc = llmax(1024u, mVBQuads);
    while (alloc < quads) alloc *= 2;
    alloc = llmin(alloc, MAX_QUADS);

    mVB = new LLVertexBuffer(VB_MASK);
    if (!mVB->allocateBuffer(alloc * 4, alloc * 6 * 2))
    {
        mVB = nullptr;
        mVBQuads = 0;
        return false;
    }

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

// Draws one material's buckets, binding each texture once and drawing its contiguous quad range.
void SSPrecipRenderer::drawMaterial(SSPrecipSim* sim, S32 material)
{
    U32 quads_total = 0;
    for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
    {
        quads_total += mRanges[material][t].mQuads;
    }
    if (quads_total == 0) return;

    for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
    {
        const Range& range = mRanges[material][t];
        if (range.mQuads == 0) continue;

        LLViewerTexture* texturep = sim->texture((U8)t);
        if (!texturep) continue;
        texturep->addTextureStats(128.f * 128.f);

        LLGLSLShader* cur = LLGLSLShader::sCurBoundShaderPtr;
        S32 tex_channel = cur ? cur->getTextureChannel(LLShaderMgr::DIFFUSE_MAP) : 0;
        if (tex_channel < 0) tex_channel = 0;
        gGL.getTexUnit(tex_channel)->bind(texturep);

        mVB->setBuffer();
        mVB->drawRange(LLRender::TRIANGLES,
                       range.mStartQuad * 4,
                       (range.mStartQuad + range.mQuads) * 4 - 1,
                       range.mQuads * 6,
                       range.mStartQuad * 6);
    }
}

// Buckets every live particle by material and texture, builds all quads into one buffer, then draws the material passes.
void SSPrecipRenderer::render()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_PRECIP_RENDER);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo->sim();
    if (!sim || sim->empty()) return;

    if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD) return;
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender || LLPipeline::sShadowRender || gCubeSnapshot) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (camera->cameraUnderWater()) return;

    const LLVector3 cam_pos = camera->getOrigin();
    const LLVector3 cam_right = -camera->getLeftAxis();
    const LLVector3 cam_up = camera->getUpAxis();

    const SSPrecipPreset& preset = atmo->preset();
    F32 bands[TIER_COUNT][4];
    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        SSPrecipSim::tierBands((SSPrecipTier)t, preset, bands[t][0], bands[t][1], bands[t][2], bands[t][3]);
    }

    for (S32 m = 0; m < MAT_COUNT; ++m)
    {
        for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
        {
            mBuckets[m][t].clear();
        }
    }

    static LLCachedControl<F32> drop_alpha_setting(gSavedSettings, "SSAtmoDropAlpha", 1.f);
    static LLCachedControl<F32> ripple_alpha_setting(gSavedSettings, "SSAtmoRippleAlpha", 1.f);
    const F32 drop_alpha = llclamp((F32)drop_alpha_setting, 0.f, 2.f);
    const F32 ripple_alpha = llclamp((F32)ripple_alpha_setting, 0.f, 2.f);

    const F32 stream_stretch = llclamp(preset.mStreamStretch, 0.f, 1.f);

    for (const SSPrecipParticle& p : sim->particles())
    {
        const F32 dx = p.mPos.mV[VX] - cam_pos.mV[VX];
        const F32 dy = p.mPos.mV[VY] - cam_pos.mV[VY];
        const F32 dist = sqrtf(dx * dx + dy * dy);
        const F32 alpha = p.mAlpha * drop_alpha * ageFade(p) * bandFade(dist, bands[p.mTier])
                        * topFade(p);
        if (alpha < 0.004f) continue;
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    for (const SSPrecipParticle& p : sim->ripples())
    {
        const bool drip = (p.mFlags & (PART_DRIP | PART_STREAM)) != 0;
        const F32 alpha = drip ? (p.mAlpha * drop_alpha * ageFade(p))
                               : (p.mAlpha * ripple_alpha * (1.f - p.mAge / p.mMaxAge));
        if (alpha < 0.004f) continue;
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    for (const SSPrecipParticle& p : sim->streams())
    {
        const F32 alpha = p.mAlpha * drop_alpha * ageFade(p);
        if (alpha < 0.004f) continue;
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    // <SS:Nexii> Blowing snow: the ground pool fades by its OWN cull radius - the falling tiers' bands start metres out and would blank drift exactly where it matters: under the camera, overhead views, the near field. Full alpha at zero distance, easing out across the outer half of the footprint.
    const F32 cull_r = SSPrecipSim::driftCullRadius();
    const F32 fade_start = cull_r * 0.55f;
    const F32 fade_span = llmax(cull_r - fade_start, 1.f);

    for (const SSPrecipParticle& p : sim->drift())
    {
        const F32 dx = p.mPos.mV[VX] - cam_pos.mV[VX];
        const F32 dy = p.mPos.mV[VY] - cam_pos.mV[VY];
        const F32 dist = sqrtf(dx * dx + dy * dy);
        const F32 far_fade = 1.f - llclamp((dist - fade_start) / fade_span, 0.f, 1.f);
        const F32 alpha = p.mAlpha * drop_alpha * ageFade(p) * far_fade * far_fade;
        if (alpha < 0.004f) continue;
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    U32 total = 0;
    for (S32 m = 0; m < MAT_COUNT; ++m)
    {
        for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
        {
            for (const Item& item : mBuckets[m][t])
            {
                total += (item.mPart->mKind == KIND_STREAM)
                       ? (U32)SS_STREAM_SEGMENTS : 1u;
            }
        }
    }
    total = llmin(total, MAX_QUADS);
    if (total == 0) return;

    if (!ensureBuffer(total)) return;

    {
    LL_RECORD_BLOCK_TIME(FTM_SS_PRECIP_BUILD);
    LLStrider<LLVector3> verticesp;
    LLStrider<LLVector3> normalsp;
    LLStrider<LLVector4a> tangentsp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;
    mVB->getVertexStrider(verticesp, 0, total * 4);
    mVB->getNormalStrider(normalsp, 0, total * 4);
    mVB->getTangentStrider(tangentsp, 0, total * 4);
    mVB->getColorStrider(colorsp, 0, total * 4);
    mVB->getTexCoord0Strider(texcoordsp, 0, total * 4);

    LLVector3 emit_normal(0.f, 0.f, 1.f);

    LLVector3 emit_axis(0.f, 0.f, 1.f);

    auto emitRibbon = [&](const LLVector3& top, const LLVector3& bot,
                          const LLVector3& across, F32 half,
                          const LLColor4U& col_top, const LLColor4U& col_bot,
                          F32 u0, F32 u1, F32 v_top, F32 v_bot)
    {
        *verticesp++ = bot - across * half;
        *verticesp++ = top - across * half;
        *verticesp++ = bot + across * half;
        *verticesp++ = top + across * half;

        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;

        const LLVector4a axis4(emit_axis.mV[VX], emit_axis.mV[VY], emit_axis.mV[VZ], 1.f);
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;

        *texcoordsp++ = LLVector2(u0, v_bot);
        *texcoordsp++ = LLVector2(u0, v_top);
        *texcoordsp++ = LLVector2(u1, v_bot);
        *texcoordsp++ = LLVector2(u1, v_top);

        *colorsp++ = col_bot;
        *colorsp++ = col_top;
        *colorsp++ = col_bot;
        *colorsp++ = col_top;
    };

    auto emitQuad = [&](const LLVector3& pos,
                        const LLVector3& x_axis, const LLVector3& y_axis,
                        const LLColor4U& col, F32 u0, F32 u1, F32 v0, F32 v1)
    {
        *verticesp++ = pos - x_axis + y_axis;
        *verticesp++ = pos - x_axis - y_axis;
        *verticesp++ = pos + x_axis + y_axis;
        *verticesp++ = pos + x_axis - y_axis;

        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;

        const LLVector4a axis4(emit_axis.mV[VX], emit_axis.mV[VY], emit_axis.mV[VZ], 1.f);
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;

        *texcoordsp++ = LLVector2(u0, v1);
        *texcoordsp++ = LLVector2(u0, v0);
        *texcoordsp++ = LLVector2(u1, v1);
        *texcoordsp++ = LLVector2(u1, v0);

        *colorsp++ = col;
        *colorsp++ = col;
        *colorsp++ = col;
        *colorsp++ = col;
    };

    U32 written = 0;
    for (S32 m = 0; m < MAT_COUNT; ++m)
    {
        for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
        {
            Range& range = mRanges[m][t];
            range.mStartQuad = written;
            U32 quads = 0;
            for (const Item& item : mBuckets[m][t])
            {
                const SSPrecipParticle& p = *item.mPart;

                const U32 need = (p.mKind == KIND_STREAM) ? (U32)SS_STREAM_SEGMENTS : 1u;
                if (written + quads + need > total) break;

                LLVector3 x_axis, y_axis;
                LLVector3 pos = p.mPos;

                LLVector3 to_cam = cam_pos - pos;
                const F32 cam_dist = to_cam.normVec();
                emit_normal = (cam_dist > 0.0001f) ? to_cam : LLVector3::z_axis;
                emit_axis = LLVector3::z_axis;

                switch (p.mKind)
                {
                    case KIND_STREAM:
                    {
                        emit_axis = LLVector3::z_axis;
                        quads += emitStream(p, item.mAlpha, stream_stretch, emitRibbon);
                        continue;
                    }
                    case KIND_STREAK:
                    case KIND_SHEET:
                    {
                        y_axis = -p.mVel;
                        if (y_axis.normVec() < 0.0001f) y_axis = LLVector3::z_axis;
                        x_axis = y_axis % (pos - cam_pos);
                        if (x_axis.normVec() < 0.0001f) x_axis = cam_right;

                        emit_axis = y_axis;

                        x_axis *= p.mSizeX;
                        y_axis *= p.mSizeY;
                        break;
                    }
                    case KIND_FLAT:
                    {
                        F32 tt = p.mAge / p.mMaxAge;
                        tt = 1.f - (1.f - tt) * (1.f - tt);
                        const F32 s = lerp(p.mSizeX, p.mSizeY, tt);
                        x_axis = p.mNormal % LLVector3::z_axis;
                        if (x_axis.normVec() < 0.0001f) x_axis = LLVector3::x_axis;
                        y_axis = p.mNormal % x_axis;
                        y_axis.normVec();
                        x_axis *= s;
                        y_axis *= s;

                        emit_normal = p.mNormal;
                        emit_axis = x_axis;

                        pos += p.mNormal * (RIPPLE_NORMAL_LIFT * s);

                        pos += to_cam * llmax(RIPPLE_DEPTH_BIAS_MIN + s * 0.1f,
                                              cam_dist * RIPPLE_DEPTH_BIAS);
                        break;
                    }
                    default:
                    {
                        F32 scale = 1.f;
                        if (p.mFlags & PART_CROWN)
                        {
                            F32 tt = llclamp(p.mAge / p.mMaxAge, 0.f, 1.f);
                            tt = 1.f - (1.f - tt) * (1.f - tt);
                            scale = lerp(CROWN_START_SCALE, CROWN_END_SCALE, tt);
                        }
                        emit_axis = (p.mVel.magVecSquared() > 0.0001f)
                                  ? -p.mVel * (1.f / p.mVel.magVec()) : LLVector3::z_axis;

                        x_axis = cam_right * (p.mSizeX * scale);
                        y_axis = cam_up * (p.mSizeY * scale);
                        break;
                    }
                }

                const F32 tier_boost = (p.mTier == TIER_DROPS) ? 1.f
                                     : (p.mTier == TIER_CLUSTERS) ? 0.55f : 0.3f;
                const F32 boost = 1.f + p.mGlow * 1.5f * tier_boost;
                LLColor4U col((U8)llmin((S32)(p.mTint.mV[0] * boost), 255),
                              (U8)llmin((S32)(p.mTint.mV[1] * boost), 255),
                              (U8)llmin((S32)(p.mTint.mV[2] * boost), 255),
                              (U8)llclamp((S32)(item.mAlpha * 255.f), 0, 255));

                emitQuad(pos, x_axis, y_axis, col, 0.f, 1.f, 0.f, 1.f);
                ++quads;
            }
            range.mQuads = quads;
            written += quads;
        }
    }
    mVB->unmapBuffer();
    }

    {
    LL_RECORD_BLOCK_TIME(FTM_SS_PRECIP_DRAW);
    LL_PROFILE_GPU_ZONE("atmo precip");
    LLGLSPipelineAlpha gls_pipeline_alpha;
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                  LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    auto bind_drop_shading = [](LLGLSLShader* shader)
    {
        static LLCachedControl<F32> drop_bulge(gSavedSettings, "SSAtmoDropBulge", 0.35f);
        static LLCachedControl<F32> drop_core(gSavedSettings, "SSAtmoDropCore", 2.0f);
        static LLCachedControl<F32> drop_sparkle(gSavedSettings, "SSAtmoDropSparkle", 6.0f);

        static LLStaticHashedString ssDropBulge("ss_drop_bulge");
        static LLStaticHashedString ssDropCore("ss_drop_core");
        static LLStaticHashedString ssDropSparkle("ss_drop_sparkle");

        shader->uniform1f(ssDropBulge, llclamp((F32)drop_bulge, 0.f, 4.f));
        shader->uniform1f(ssDropCore, llclamp((F32)drop_core, 0.f, 8.f));
        shader->uniform1f(ssDropSparkle, llclamp((F32)drop_sparkle, 0.f, 64.f));
    };

    const F32 decal_normals = atmo->rippleTexture() ? 0.f : 1.f;
    static LLStaticHashedString ssDecalNormals("ss_decal_normals");

    auto bind_fullbright = []()
    {
        LLGLSLShader* shader = &gDeferredFullbrightProgram;
        shader->bind();

        static LLCachedControl<F32> displayGamma(gSavedSettings, "RenderDeferredDisplayGamma");
        const F32 gamma = displayGamma;
        shader->uniform1f(LLShaderMgr::DISPLAY_GAMMA, (gamma > 0.1f) ? 1.0f / gamma : (1.0f / 2.2f));
        static LLStaticHashedString waterSign("waterSign");
        shader->uniform1f(waterSign, 1.f);
        shader->uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);
        shader->setMinimumAlpha(0.f);
    };

    {
        static LLStaticHashedString ssDecal("ss_decal");
        static LLStaticHashedString ssSceneLit("ss_scene_lit");
        static LLStaticHashedString ssGranular("ss_granular");
        LLGLSLShader* lit = &gSSPrecipLitProgram;
        if (lit->isComplete())
        {
            lit->mCanBindFast = false;
            gPipeline.bindDeferredShaderFast(*lit);
            lit->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);

            lit->uniform1f(ssSceneLit, (gPipeline.mSceneMap.getWidth() > 0) ? 1.f : 0.f);

            lit->uniform1f(ssDecalNormals, decal_normals);

            lit->uniform1f(ssDecal, 0.f);
            lit->uniform1f(ssGranular, 0.f);
            drawMaterial(sim, MAT_LIT);

            // The granular family draws through the same lit shading with the
            // screen-door on - same light, grain-stippled coverage.
            lit->uniform1f(ssGranular, 1.f);
            drawMaterial(sim, MAT_GRANULAR);
            lit->uniform1f(ssGranular, 0.f);

            lit->uniform1f(ssDecal, 1.f);
            drawMaterial(sim, MAT_DECAL);
            lit->uniform1f(ssDecal, 0.f);
        }
        else
        {
            bind_fullbright();
            drawMaterial(sim, MAT_LIT);
            drawMaterial(sim, MAT_GRANULAR);
            drawMaterial(sim, MAT_DECAL);
        }
    }

    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE,
                  LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE);
    bind_fullbright();
    drawMaterial(sim, MAT_EMISSIVE);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                  LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    {
        static LLCachedControl<bool> use_rain_shader(gSavedSettings, "SSAtmoRainShader", true);
        LLGLSLShader* rain = &gSSPrecipRainProgram;
        if (use_rain_shader && rain->isComplete())
        {
            rain->bind();
            rain->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);

            static LLStaticHashedString ssRefract("ss_refract_strength");
            const bool has_scene = gPipeline.mSceneMap.getWidth() > 0;
            rain->uniform1f(ssRefract, has_scene ? 0.035f : 0.f);
            bind_drop_shading(rain);

            gPipeline.bindReflectionProbes(*rain);
            drawMaterial(sim, MAT_WATER);
            gPipeline.unbindReflectionProbes(*rain);
        }
        else
        {
            drawMaterial(sim, MAT_WATER);
        }
    }

    {
        static LLCachedControl<bool> use_projectors(gSavedSettings, "SSAtmoProjectorLights", true);
        static LLCachedControl<U32> max_projectors(gSavedSettings, "SSAtmoProjectorLightCount", 2);
        static LLCachedControl<F32> scatter_gain(gSavedSettings, "SSAtmoProjectorGain", 0.1f);
        static LLCachedControl<F32> scatter_aniso(gSavedSettings, "SSAtmoProjectorAnisotropy", 0.3f);

        LLGLSLShader* proj = &gSSPrecipProjProgram;
        const U32 want = llclamp((U32)max_projectors, 0u, 8u);

        if (use_projectors && want > 0 && proj->isComplete())
        {
            std::vector<LLDrawable*> projectors;
            gPipeline.getNearbyProjectors(projectors, want);

            if (!projectors.empty())
            {
                LL_PROFILE_GPU_ZONE("atmo precip projectors");

                gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE,
                              LLRender::BF_ZERO, LLRender::BF_ONE);

                proj->bind();
                proj->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);

                static LLStaticHashedString ssScatterGain("ss_scatter_gain");
                static LLStaticHashedString ssScatterAniso("ss_scatter_aniso");
                static LLStaticHashedString ssProjDecal("ss_decal");
                proj->uniform1f(ssScatterGain, llclamp((F32)scatter_gain, 0.f, 2.f));
                proj->uniform1f(ssScatterAniso, llclamp((F32)scatter_aniso, 0.f, 0.95f));
                bind_drop_shading(proj);
                proj->uniform1f(ssDecalNormals, decal_normals);

                const glm::mat4 view = get_current_modelview();

                for (LLDrawable* drawablep : projectors)
                {
                    LLVOVolume* volume = drawablep->getVOVolume();
                    if (!volume) continue;

                    gPipeline.setupSpotLight(*proj, drawablep);

                    const LLVector3 center_agent = drawablep->getPositionAgent();
                    const glm::vec3 c = mul_mat4_vec3(view, glm::vec3(center_agent.mV[VX],
                                                                      center_agent.mV[VY],
                                                                      center_agent.mV[VZ]));
                    const LLVector3 center_view(c.x, c.y, c.z);

                    const LLColor3 col = volume->getLightLinearColor();

                    proj->uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, center_view.mV);
                    proj->uniform1f(LLShaderMgr::LIGHT_SIZE, volume->getLightRadius() * 1.5f);
                    proj->uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, col.mV);
                    proj->uniform1f(LLShaderMgr::LIGHT_FALLOFF, volume->getLightFalloff(0.5f));

                    proj->uniform1f(ssProjDecal, 0.f);
                    drawMaterial(sim, MAT_LIT);
                    drawMaterial(sim, MAT_WATER);

                    proj->uniform1f(ssProjDecal, 1.f);
                    drawMaterial(sim, MAT_DECAL);
                }

                proj->disableTexture(LLShaderMgr::DEFERRED_PROJECTION);
                gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                              LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
            }
        }
    }

    LLGLSLShader::unbind();
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    {
        static LLCachedControl<bool> sheet_markers(gSavedSettings, "SSAtmoDebugSheetMarkers", false);
        if (sheet_markers)
        {
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

            gDebugProgram.bind();
            LLGLDisable cull(GL_CULL_FACE);
            LLGLEnable blend(GL_BLEND);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);

            gGL.setColorMask(true, false);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.begin(LLRender::TRIANGLES);

            auto mark_ribbon = [&](const LLVector3& a, const LLVector3& b, F32 w)
            {
                LLVector3 side = (b - a) % ((a + b) * 0.5f - cam);
                if (side.normalize() <= 0.f) return;
                const LLVector3 sa = side * w;
                gGL.vertex3fv((a - sa).mV); gGL.vertex3fv((a + sa).mV); gGL.vertex3fv((b + sa).mV);
                gGL.vertex3fv((a - sa).mV); gGL.vertex3fv((b + sa).mV); gGL.vertex3fv((b - sa).mV);
            };

            for (const SSPrecipParticle& p : sim->particles())
            {
                if (p.mTier != TIER_SHEETS) continue;

                const F32 d = (p.mPos - cam).magVec();
                const F32 w = llmax(0.15f, d * 0.003f);

                gGL.color4f(0.1f, 0.9f, 1.f, 0.7f);
                mark_ribbon(p.mPos - p.mVel * p.mAge,
                            p.mPos + p.mVel * llmax(p.mMaxAge - p.mAge, 0.f), w * 0.5f);

                gGL.color4f(1.f, 0.15f, 0.9f, 0.9f);
                mark_ribbon(p.mPos, p.mPos + p.mVel, w * 1.6f);
                mark_ribbon(p.mPos - LLVector3(0.f, 0.f, w * 2.f),
                            p.mPos + LLVector3(0.f, 0.f, w * 2.f), w * 2.f);
            }

            gGL.end();
            gGL.flush();
            gGL.setColorMask(true, true);
            gDebugProgram.unbind();
        }
    }
    }
}
