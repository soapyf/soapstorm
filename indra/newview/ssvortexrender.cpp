/**
 * @file ssvortexrender.cpp
 * @brief Atmo Magic: the vortex funnel renderer - see ssvortexrender.h.
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

#include "ssvortexrender.h"

#include "ssatmoenvapplier.h"
#include "ssatmomagic.h"
#include "ssstormcells.h"
#include "ssvolcloud.h"
#include "ssvortices.h"

#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <cmath>
#include <vector>

extern bool gCubeSnapshot; // <SS:Nexii> defined in llviewerdisplay.cpp, no header declares it - same local extern every ss* renderer uses

namespace
{
    // <SS:Nexii> The debris skirt's own particle source - one per live, ground-contact vortex (a gustnado, which is
    // dust its whole life - see the taxonomy row in doc/atmo_magic_storm_dynamics.md section 4 - or a funnel-having
    // vortex currently in PHASE_TOUCHDOWN, the only phase with real ground contact). SPAWN POSITIONS are a pure
    // function of (vortex id, spawn index): the same SSAtmoNoise chain-hash idiom every other Atmo Magic scheduler
    // uses, salted so it never collides with a storm-cell or vortex draw over the same raw numbers. The particle
    // SIM ITSELF (throttled spawn cadence off real dt, velocity, lifetime) is ordinary per-client LLViewerPartSim
    // physics, same "per-client jitter is accepted precedent" the task names for rain - this is not a determinism
    // claim about the sim, only about where each spawn LANDS.
    class SSVortexDebrisSource : public LLViewerPartSource
    {
    public:
        explicit SSVortexDebrisSource(U64 vortex_id)
            : LLViewerPartSource(LL_PART_SOURCE_NULL)
            , mVortexId(vortex_id)
        {
        }

        // Refreshed every frame by SSVortexRender::render() before LLViewerPartSim ticks update() below; mPosAgent
        // (inherited from LLViewerPartSource) is the ring's CENTRE - the vortex's own contact point.
        void setTarget(const LLVector3& contact_agent, F32 ring_radius_m, F32 intensity01)
        {
            mPosAgent = contact_agent;
            mRingRadiusM = ring_radius_m;
            mIntensity = llclamp(intensity01, 0.f, 1.f);
        }

        /*virtual*/ void update(const F32 dt)
        {
            if (!mImagep)
            {
                mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;
            }
            if (mRingRadiusM <= 0.f || mIntensity <= 0.f)
            {
                return;
            }

            // <SS:Nexii> The same throttle idiom LLViewerPartSourceSpiral::update uses: a spawn RATE in simulated
            // seconds (the minimum period between spawns), catch-up capped so a stall never bursts years of missed
            // spawns at once. RATE is now scaled by SSVortex::DebrisParams::mCount (ssvortexcore.h's own "count"
            // lerp, off live intensity) rather than fixed - a NEW intensity axis, not a preservation claim: a weak
            // vortex now throws debris more sparsely than a strong one, where before the period was a flat
            // constant regardless of intensity. Review 5b: the RATE_BASE constant and the floor it divides against
            // both moved into SSVortex::debrisSpawnPeriodS (ssvortexcore.h) - this is a call, not a formula.
            const F32 RATE = SSVortex::debrisSpawnPeriodS(mIntensity);
            mLastUpdateTime += dt;
            F32 dt_update = mLastUpdateTime - mLastPartTime;
            dt_update = llmin(dt_update, llmax(1.f, 10.f * RATE));
            if (dt_update <= RATE)
            {
                return;
            }
            mLastPartTime = mLastUpdateTime;

            if (!LLViewerPartSim::getInstance()->shouldAddPart())
            {
                return;
            }

            using SSAtmoNoise::combine;
            using SSAtmoNoise::hash01;
            const U32 idLo = (U32)(mVortexId & 0xffffffffu);
            const U32 idHi = (U32)((mVortexId >> 32) & 0xffffffffu);
            U32 chain = combine(idLo, idHi);
            chain = combine(chain, mSpawnIndex++);

            // <SS:Nexii> NEW-7: rewritten to describe behaviour, not claim equivalence with prior code. debrisParams
            // (ssvortexcore.h): radius share and lifetime each draw their OWN per-particle hash (SALT_RADIUS/
            // SALT_LIFE), so the two stay uncorrelated across particles; speed/count/alpha depend only on intensity,
            // so both calls return identical values for those three fields - every field is read from pr except
            // mLifetimeS, which comes from pl instead.
            const SSVortex::DebrisParams pr = SSVortex::debrisParams(hash01(combine(chain, SALT_RADIUS)), mIntensity);
            const SSVortex::DebrisParams pl = SSVortex::debrisParams(hash01(combine(chain, SALT_LIFE)), mIntensity);

            const F32 ang = hash01(combine(chain, SALT_ANGLE)) * 6.283185307f;
            const F32 rr = mRingRadiusM * pr.mRadiusShare;
            const F32 life = pl.mLifetimeS;
            const F32 lift = SSVortex::debrisLift(hash01(combine(chain, SALT_LIFT))); // Review 5b: DEBRIS_LIFT_MIN/MAX moved into ssvortexcore.h

            LLViewerPart* part = new LLViewerPart();
            part->init(this, mImagep, nullptr);

            const LLColor4 dust(0.55f, 0.50f, 0.42f, pr.mAlpha);
            part->mStartColor = dust;
            part->mEndColor = dust;
            part->mEndColor.mV[3] = 0.f;

            const F32 cx = cosf(ang), sy = sinf(ang);
            part->mPosAgent = mPosAgent + LLVector3(cx * rr, sy * rr, 0.f);
            part->mVelocity = LLVector3(-sy, cx, 0.f) * pr.mSpeedMS + LLVector3(0.f, 0.f, lift);
            part->mAccel = LLVector3(0.f, 0.f, 0.35f);
            part->mMaxAge = life;
            // <SS:Nexii> Review finding 2 (NEW-7: rewritten to describe behaviour, not claim equivalence with prior
            // code): LL_PART_INTERP_SCALE_MASK tells LLViewerPartSim::update() to overwrite mScale every tick with
            // lerp(mStartScale, mEndScale, frac) (llviewerpartsim.cpp) - neither of which this source ever set, so
            // every debris particle collapsed to a (0,0) scale on its very first update after spawn (invisible
            // skirt). Dropped rather than adding a size lerp to SSVortex::DebrisParams (no such field in the core's
            // own five-field contract, and this file is renderer-only per the task) - with the mask gone, the
            // constant part->mScale assignment below is the only thing that ever sets scale, so every particle keeps
            // the value assigned there for its whole life.
            part->mFlags = LLViewerPart::LL_PART_INTERP_COLOR_MASK;
            part->mLastUpdateTime = 0.f;
            // Review 5b: DEBRIS_SCALE_MIN/MAX folded into SSVortex::DebrisParams::mScale (ssvortexcore.h) - pr's
            // own h01 draw is irrelevant here (mScale is intensity-only, same as mSpeedMS/mCount/mAlpha).
            part->mScale.mV[0] = pr.mScale;
            part->mScale.mV[1] = part->mScale.mV[0];
            part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
            part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
            part->mStartGlow = 0.f;
            part->mEndGlow = 0.f;
            part->mGlow = LLColor4U(0, 0, 0, 0);

            LLViewerPartSim::getInstance()->addPart(part);
        }

    private:
        static constexpr U32 SALT_ANGLE  = 0x53440001u;
        static constexpr U32 SALT_RADIUS = 0x53440002u;
        static constexpr U32 SALT_LIFE   = 0x53440003u;
        static constexpr U32 SALT_LIFT   = 0x53440004u;

        U64 mVortexId;
        F32 mRingRadiusM = 0.f;
        F32 mIntensity = 0.f;
        U32 mSpawnIndex = 0;
    };

    // Global XY -> agent-frame XY, via SSStormCells' own converter - the same frame every storm-cell/vortex centre
    // lives in (ssvortices.h's own file-top comment). SSVortex::Vec2 and SSStormCell::Vec2 are the same shape in
    // two namespaces, so this is a field-for-field copy, not a cast - the same crossing ssvortices.cpp's own
    // toVortexVec2 makes, respelled here rather than shared because neither core may include the other's Vec2.
    LLVector2 vortexAgentXY(const SSVortex::Vec2& global_xy)
    {
        SSStormCell::Vec2 g;
        g.x = global_xy.x;
        g.y = global_xy.y;
        return SSStormCells::getInstance()->toAgentXY(g);
    }

    // <SS:Nexii> renderDebug's own line-subdivision length (ssvolcloud.cpp): squashScale's curve bends the same
    // way at the same knee/cap for every caller, so a collar span this long stays visually straight through the
    // knee exactly as a debug band-outline segment does. Every collar span is subdivided by this BEFORE any vertex
    // is submitted - the CPU never calls squashScale itself (that runs once per vertex, in ssVortexV.glsl, the same
    // radial-pull idiom ssVolCloudV.glsl/ssLightningV.glsl use), so "subdivides before squashScale" here means
    // choosing how many TRUE, unsquashed vertices to submit, not pre-squashing them on the CPU.
    constexpr F32 VORTEX_SQUASH_SEG_M = 200.f;

    // The soft depth-fade width, metres - SSVolCloud's own SOFT_M (ssvolcloud.cpp), reused so a funnel dissolves
    // into geometry it is standing in front of at the same rate the puffs do.
    constexpr F32 VORTEX_SOFT_M = 112.5f;
}

SSVortexRender::SSVortexRender()
{
}

SSVortexRender::~SSVortexRender()
{
    for (auto& kv : mDebrisSources)
    {
        if (kv.second.notNull())
        {
            kv.second->setDead();
        }
    }
}

// <SS:Nexii> See the header for the shape of this: debris-skirt bookkeeping first (a live LLViewerPartSource is the
// one piece of state that is not re-derivable from scratch every frame), then the funnel collar quads - both read
// SSVortices::active() fresh, never keep their own copy of it.
void SSVortexRender::render()
{
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }

    SSVortices* sched = SSVortices::getInstance();
    // <SS:Nexii> FIX 1: SSVortices::dustDevils() is fresh whenever update() has run at all (see its own header
    // comment) - only active()/the collar tables inside it are stale (the previous frame's kept set) when valid()
    // is false. This early-return now checks BOTH before killing every debris source and bailing: a track with no
    // storm cells resolved this frame (Allow Supercells/Tornadoes both off, the common case) can still have live
    // dust devils to draw, and previously did not - see SSVortices::update()'s own FIX 1 comment for the bug.
    if (!sched->valid() && sched->dustDevils().empty())
    {
        for (auto& kv : mDebrisSources)
        {
            if (kv.second.notNull())
            {
                kv.second->setDead();
            }
        }
        mDebrisSources.clear();
        return;
    }

    // FIX 1: never sched->active() directly when !valid() - that vector is last frame's stale kept set (see
    // SSVortices::active()'s own comment), not an empty one. An invalid scheduler still reaches here when it has
    // live dust devils, so this must degrade to an empty funnel/gustnado set rather than draw stale ones.
    static const std::vector<SSVortices::LiveVortex> sEmptyActive;
    const std::vector<SSVortices::LiveVortex>& active = sched->valid() ? sched->active() : sEmptyActive;

    // <SS:Nexii> Review finding 3: ONE ground read, hoisted above both the debris skirt and the funnel-quad loop
    // below, and shared by both (a gustnado's debris ring and every funnel's own funnel_height_m). Before this fix
    // the debris block below read its own ad hoc ground_z (v.mWallCloudZ - 1.f for a funnel-less gustnado, wrong -
    // that is the WALL CLOUD altitude, not the ground), and the funnel loop read the real ground reference
    // separately further down - two call sites, one of them stale. windProfileGroundZ() is the shell's single
    // ground reference (see ssvortexcore.h's own file-top comment and ssvortices.h's file-top comment).
    const F32 ground_z = SSAtmoEnvApplier::instance().windProfileGroundZ();

    // --- debris skirt: gustnadoes (funnel-less, dust their whole life), any funnel-having vortex currently in
    // PHASE_TOUCHDOWN (the only phase with real ground contact - ALOFT has not reached ground, ROPE narrows but
    // does not lift off), and (NEW-6) every live dust devil - the taxonomy's own funnel-less, dust-only kind, drawn
    // with no collar quads at all, just this same rotating debris ring at its own origin. Keyed on
    // SSVortex::Candidate::mId / SSVortex::DustCandidate::mId, sharing the one mDebrisSources map the same way every
    // funnel candidate and every gustnado already do (independent 64-bit hash chains, so a same-frame collision
    // between the two id spaces is as negligible as one already was between two vortex ids), so a vortex or dust
    // devil that survives several frames keeps its own source (and so its own hashed spawn sequence) rather than
    // restarting it every frame. ---
    {
        std::map<U64, LLPointer<LLViewerPartSource>>& keep = mDebrisKeepScratch;
        keep.clear();
        for (const SSVortices::LiveVortex& v : active)
        {
            const bool wants_debris = (v.mCandidate.mKind == SSVortex::KIND_GUSTNADO)
                                    || (v.mHasFunnel && v.mState.mPhase == SSVortex::PHASE_TOUCHDOWN);
            if (!wants_debris)
            {
                continue;
            }

            const U64 id = v.mCandidate.mId;
            LLPointer<LLViewerPartSource> src;
            auto it = mDebrisSources.find(id);
            if (it != mDebrisSources.end())
            {
                src = it->second;
                mDebrisSources.erase(it);
            }
            else
            {
                src = new SSVortexDebrisSource(id);
                LLViewerPartSim::getInstance()->addPartSource(src);
            }

            const LLVector2 contact_xy = vortexAgentXY(v.mContactGlobal);
            // <SS:Nexii> Review finding 3: a funnel's own bottom collar (already ground-anchored - buildCollars'
            // h01 == 0 row) when it has one, else the hoisted ground_z above - never v.mWallCloudZ (the funnel's
            // TOP), which is what a funnel-less gustnado's debris ring used to spawn at.
            const F32 contact_z = v.mHasFunnel ? v.mCollars[0].mAltitudeM : ground_z;
            const LLVector3 contact_agent(contact_xy.mV[0], contact_xy.mV[1], contact_z);

            // <SS:Nexii> Judgement call: the design names no debris-ring radius. A touched-down funnel throws its
            // skirt at its own ground-collar radius (mCollars[0], the contact-height row buildCollars laid down);
            // a gustnado has no collar table (funnel-less), so it uses a flat, generous ring instead - both floored
            // so even a drill-bit tip still throws a visible skirt. Review 5b: the two floors (6/25) moved into
            // SSVortex::ringRadiusM (ssvortexcore.h) - LiveVortex carries no parent-radius field today, so this
            // passes 0.f for the currently-unread parentRadius parameter (see that function's own comment).
            const F32 ring_radius = SSVortex::ringRadiusM(v.mHasFunnel ? v.mCollars[0].mRadiusM : 0.f, v.mHasFunnel, 0.f);

            static_cast<SSVortexDebrisSource*>(src.get())->setTarget(contact_agent, ring_radius, v.mState.mIntensity);
            keep[id] = src;
        }

        // <SS:Nexii> NEW-6: dust devils - the same debris-skirt machinery above, retargeted at each live dust
        // devil's own origin instead of a parented vortex's contact point. No funnel quads for these (the taxonomy's
        // funnel-less/dust-only kind - ssvortexcore.h's own file-top comment): this loop is the entirety of a dust
        // devil's render footprint, a small rotating debris column and nothing else.
        for (const SSVortices::DustVortex& d : sched->dustDevils())
        {
            const U64 id = d.mCandidate.mId;
            LLPointer<LLViewerPartSource> src;
            auto it = mDebrisSources.find(id);
            if (it != mDebrisSources.end())
            {
                src = it->second;
                mDebrisSources.erase(it);
            }
            else
            {
                src = new SSVortexDebrisSource(id);
                LLViewerPartSim::getInstance()->addPartSource(src);
            }

            // A dust devil is parentless and ground-level its whole life (no mContact/collar altitude the way a
            // parented vortex has) - its own hashed mOriginXY at the hoisted ground_z above.
            const LLVector2 origin_xy = vortexAgentXY(d.mCandidate.mOriginXY);
            const LLVector3 origin_agent(origin_xy.mV[0], origin_xy.mV[1], ground_z);

            // <SS:Nexii> Ring radius from SSVortex::ringRadiusM's own !hasFunnel branch (RING_RADIUS_GUSTNADO_M) -
            // the same flat, generous ring a gustnado throws its skirt at, reused here rather than a third constant
            // since a dust devil is architecturally the same funnel-less, dust-only shape that branch already names;
            // 0.f for collarRadius0 (dust devils have no collar table) and 0.f for the currently-unread parentRadius.
            const F32 ring_radius = SSVortex::ringRadiusM(0.f, false, 0.f);

            static_cast<SSVortexDebrisSource*>(src.get())->setTarget(origin_agent, ring_radius, d.mIntensity);
            keep[id] = src;
        }

        for (auto& kv : mDebrisSources)
        {
            if (kv.second.notNull())
            {
                kv.second->setDead();
            }
        }
        mDebrisSources.swap(keep);
        // <SS:Nexii> NEW-5: the swap above leaves `keep` (mDebrisKeepScratch) holding the PREVIOUS frame's
        // mDebrisSources contents - every entry already setDead()'d just above, but still LLPointer-held. Cleared
        // right here rather than only at the top of the next call so no early return further down this function (or
        // on a later frame, before this block runs again) leaves those dead pointers referenced past their retire.
        keep.clear();
    }

    // --- funnel collar quads ---
    if (!gSSVortexProgram.isComplete())
    {
        return;
    }

    S32 funnel_count = 0;
    for (const SSVortices::LiveVortex& v : active)
    {
        if (v.mHasFunnel)
        {
            ++funnel_count;
        }
    }
    if (funnel_count == 0)
    {
        return;
    }

    SSVolCloud* vol = SSVolCloud::getInstance();

    // <SS:Nexii> Must precede this pass's own program bind - ensureSceneDepthCopy binds ITS OWN program and
    // rebinds the screen target (see its own header comment; the same rule sslightningrender.cpp follows).
    LLRenderTarget* depth_rt = vol->ensureSceneDepthCopy();

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam_pos = camera->getOrigin();
    const LLVector3 cam_right_fallback = camera->getLeftAxis() * -1.f;

    // <SS:Nexii> Constraint 4 / ssvortexcore.h's own file-top comment: REAL alpha in the sky forward pass, never
    // post-deferred additive (a dark funnel that writes additive alpha blooms) - see doc/viewer/glow_and_alpha.md
    // rule 2: a post-deferred pass wanting transparency sets setColorMask(true, false) and lets frag_color.a be
    // ordinary coverage, exactly SSVolCloud::render()'s own setup below, copied verbatim.
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);
    // <SS:Nexii> Review finding 1 (CRITICAL): every sibling forward-sky pass disables face culling for its
    // billboards (sslightningrender.cpp:359's LLGLDisable cull(GL_CULL_FACE) idiom) - this pass never did, so
    // whichever winding the collar quads happened to submit in was a coin flip against the prevailing cull state,
    // and combined with the reversed winding below (finding 1's other half) the funnel never drew at all.
    LLGLDisable cull(GL_CULL_FACE);

    gSSVortexProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    static LLStaticHashedString s_squash("ss_squash");
    static LLStaticHashedString s_cam_pos("ss_cam_pos");
    static LLStaticHashedString s_time("ss_time");
    static LLStaticHashedString s_gloom("ss_gloom");
    static LLStaticHashedString s_soft_m("ss_soft_m");
    static LLStaticHashedString s_clip("ss_clip");
    static LLStaticHashedString s_vortex_n("ss_vortex_n");
    static LLStaticHashedString s_vortex_multi("ss_vortex_multi");

    gSSVortexProgram.uniform3f(s_squash, vol->squashKnee(), vol->squashCap(), vol->virtualRadius());
    gSSVortexProgram.uniform3fv(s_cam_pos, 1, cam_pos.mV);

    // <SS:Nexii> The SHARED wall clock (SSAtmoMagic::sharedTime), not per-client elapsed-since-launch time: this
    // feeds radiusModulation's omega*t term (ssvortexcore.h), and a suction-vortex phase built from a per-client
    // clock would rotate differently for every viewer looking at the same funnel. Wrapped the same way
    // sslightningrender.cpp wraps its own ss_time, so the float keeps sub-millisecond steps at any wall-clock hour.
    const F32 t_now = (F32)fmod(SSAtmoMagic::getInstance()->sharedTime(), 4096.0);
    gSSVortexProgram.uniform1f(s_time, t_now);
    gSSVortexProgram.uniform1f(s_gloom, vol->weatherGloom());

    // <SS:Nexii> The deck's own light direction - CPU-side only (no ss_light_dir uniform: the fragment stage never
    // needs it, since the per-corner lit-rim facing term is already baked into vary_color.r down in the collar
    // loop below, the same "structure rides the vertex, computed where the geometry is known" precedent
    // SSVolCloud::Puff::mForm sets - see ssVolCloud.h's own comment on it).
    LLVector3 light = vol->lightDir();
    if (light.normalize() < 0.001f)
    {
        light = LLVector3::z_axis;
    }

    const bool soft = depth_rt
        && gSSVortexProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, depth_rt, true) >= 0;
    gSSVortexProgram.uniform1f(s_soft_m, soft ? VORTEX_SOFT_M : 0.f);
    if (soft)
    {
        gSSVortexProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);
        gSSVortexProgram.uniform2f(s_clip, camera->getNear(), camera->getFar());
    }

    // <SS:Nexii> FIX 5: one slot per funnel-HAVING vortex (v.mHasFunnel, 0..funnel_count-1, hero-first/parent-id
    // order - the same order SSVortices::active() already ranked, see its own comment), uploaded as a fixed
    // SSVortex::MAX_ACTIVE-sized array every call (zero-filled past funnel_count) - the same always-upload-the-
    // full-array idiom SSVolCloud::render() uses for ss_storm_a/b/c, so a partial upload never leaves a stale
    // slot's data bound. NOT one slot per funnel actually drawn this frame: ss_vortex_n is uploaded here, before
    // the per-vortex loop below has a chance to skip a slot whose base_alpha rounds to 0 (that loop's own
    // `continue`, well past this point) - a funnel too faint to draw still reserves and uploads a slot. Harmless
    // for radiusModulation's own n==0 test (a skipped slot's multi_uniforms entry, if ever sampled, still decodes
    // to a valid, if unused, modulation), just not the live "how many are drawn" count its old name implied.
    // Review finding 12: to_draw is the member scratch mDrawScratch (reused across frames), cleared here rather
    // than freshly constructed.
    std::vector<DrawVortex>& to_draw = mDrawScratch;
    to_draw.clear();
    to_draw.reserve((size_t)llmin(funnel_count, SSVortex::MAX_ACTIVE));

    LLVector4 multi_uniforms[SSVortex::MAX_ACTIVE];
    for (S32 i = 0; i < SSVortex::MAX_ACTIVE; ++i)
    {
        multi_uniforms[i] = LLVector4(0.f, 0.f, 0.f, 0.f);
    }
    for (const SSVortices::LiveVortex& v : active)
    {
        if (!v.mHasFunnel)
        {
            continue;
        }
        const S32 slot = (S32)to_draw.size();
        if (slot >= SSVortex::MAX_ACTIVE)
        {
            break; // active() itself already caps at MAX_ACTIVE - belt and braces
        }
        multi_uniforms[slot] = LLVector4((F32)v.mCandidate.mMultiN, v.mCandidate.mMultiOmega,
                                          v.mCandidate.mMultiPhase, v.mState.mMultiWeight);
        to_draw.push_back({&v, slot});
    }

    gSSVortexProgram.uniform1i(s_vortex_n, (S32)to_draw.size());
    gSSVortexProgram.uniform4fv(s_vortex_multi, SSVortex::MAX_ACTIVE, (F32*)multi_uniforms);

    // <SS:Nexii> Review finding 3: the SAME hoisted ground_z the debris skirt above already used (not a second
    // read) - shared by funnel_height_m just below.
    const F32 slot_div = (F32)llmax(SSVortex::MAX_ACTIVE - 1, 1);

    for (const DrawVortex& dv : to_draw)
    {
        const SSVortices::LiveVortex& v = *dv.mVortex;
        const F32 slot_norm = (F32)dv.mSlot / slot_div;

        const F32 base_alpha = SSVortex::baseAlpha(v.mState.mIntensity);
        if (base_alpha <= 0.002f)
        {
            continue;
        }

        const LLVector2 contact_xy = vortexAgentXY(v.mContactGlobal);
        const F32 funnel_height_m = llmax(v.mWallCloudZ - ground_z, 1.f);
        const LLVector2 tilt_dir(v.mState.mTiltDir.x, v.mState.mTiltDir.y);
        const F32 tilt_frac = v.mState.mTiltFrac;
        const F32 condensation = llclamp(v.mState.mCondensation, 0.f, 1.f);

        LLVector3 sun_horiz = light;
        sun_horiz.mV[VZ] = 0.f;
        if (sun_horiz.normalize() < 0.001f)
        {
            sun_horiz = LLVector3::x_axis;
        }

        // <SS:Nexii> Review finding 10 (ordering residual, accepted): this draw call runs after SSVolCloud::render()
        // has already submitted every puff for this frame (pipeline.cpp - SSVolCloud::render() then
        // SSVortexRender::render(), same forward sky pass), both with ordinary depth testing (no depth pre-pass, no
        // sort). A puff nearer the camera than a given funnel fragment still depth-tests correctly against it
        // (LLGLDepthTest(GL_TRUE, GL_FALSE) is live on both), but a puff that occupies the SAME depth range and was
        // drawn moments earlier can never be occluded BY the funnel drawn after it in the alpha-blended sense
        // ordinary back-to-front transparency needs - there is no cross-pass sort between the two systems. Accepted:
        // funnels are rare, small relative to the deck, and the two already agree on soft depth fade width
        // (VORTEX_SOFT_M mirrors SSVolCloud's own SOFT_M) so the visible seam this leaves is a soft one, not a hard
        // draw-order pop.
        gGL.begin(LLRender::TRIANGLES);
        for (S32 c = 0; c + 1 < SSVortex::COLLARS; ++c)
        {
            const SSVortices::Collar& c0 = v.mCollars[(size_t)c];
            const SSVortices::Collar& c1 = v.mCollars[(size_t)(c + 1)];
            const F32 h0 = (F32)c / (F32)(SSVortex::COLLARS - 1);
            const F32 h1 = (F32)(c + 1) / (F32)(SSVortex::COLLARS - 1);

            // <SS:Nexii> ssvortexcore.h's own SSVortex::squashSegments, called exactly where its own comment says
            // to - once per collar span, before any vertex of that span is built - so a tall span crossing the
            // squash knee still bends smoothly through it instead of the two collar rows' single straight edge
            // cutting the curve short.
            const F32 span_m = llabs(c1.mAltitudeM - c0.mAltitudeM);
            const S32 segs = SSVortex::squashSegments(span_m, VORTEX_SQUASH_SEG_M, 24);

            LLVector3 prev_left, prev_right;
            F32 prev_h01 = h0;
            F32 prev_facing_l = 0.f, prev_facing_r = 0.f;

            for (S32 s = 0; s <= segs; ++s)
            {
                const F32 frac = (F32)s / (F32)segs;
                const F32 h01 = std::lerp(h0, h1, frac);
                const F32 radius = std::lerp(c0.mRadiusM, c1.mRadiusM, frac);
                const F32 z = std::lerp(c0.mAltitudeM, c1.mAltitudeM, frac);

                // <SS:Nexii> Judgement call (the core states tilt only as "a share of height" - PHASE_ROPE's own
                // comment in ssvortexcore.h - not a placement formula): the ground contact (h01 0) stays anchored,
                // the wall cloud (h01 1) leans the full ROPE_TILT_MAX * intensity share of the funnel's own true
                // height along mTiltDir - an ordinary rope-out tornado's silhouette. Geometry only, no new
                // simulation state; SSVortex::State already resolved tiltFrac/tiltDir per the core's own contract.
                const LLVector2 axis_xy = contact_xy + tilt_dir * SSVortex::tiltOffsetM(tilt_frac, h01, funnel_height_m);
                const LLVector3 axis_pos(axis_xy.mV[0], axis_xy.mV[1], z);

                // Z-axis billboard: only the horizontal facing rotates to the camera, "up" stays world +Z (the
                // vertical stack itself), exactly the collar-quad convention the design names.
                LLVector3 to_cam = cam_pos - axis_pos;
                to_cam.mV[VZ] = 0.f;
                LLVector3 right;
                if (to_cam.normalize() < 0.001f)
                {
                    // <SS:Nexii> Review finding 1: camera directly overhead/underneath the axis (to_cam degenerate)
                    // - the same overhead fallback every sibling billboard uses (ssvolcloud.cpp's own
                    // base_right = cam_right_fallback when its ref % normal degenerates), already the puff
                    // convention's own escape hatch, so no change needed here beyond what already matched it.
                    right = cam_right_fallback;
                }
                else
                {
                    // <SS:Nexii> Review finding 1 (CRITICAL): was to_cam % z_axis - the REVERSE of the puff
                    // convention (ssvolcloud.cpp's base_right = ref % normal, ref ~= z_axis, normal ~= to_cam), so
                    // this quad's front/back faces were swapped from every sibling billboard's. Paired with no
                    // LLGLDisable(GL_CULL_FACE) on this pass (fixed just above), the funnel was being culled
                    // outright - it never drew.
                    right = LLVector3::z_axis % to_cam;
                    if (right.normalize() < 0.001f)
                    {
                        right = cam_right_fallback;
                    }
                }

                // <SS:Nexii> The lit-rim term (r channel): computed here, once per corner, on the CPU - the same
                // precedent SSVolCloud::Puff::mForm sets (ssvolcloud.h's own comment: "the structure - which the
                // builder walks the deck's geometry to know - still rides the puff"). Which EDGE of this billboard
                // faces the sun horizontally: the +right corner earns it when right points toward the sun, the
                // -right corner when it points away, so the fragment stage's wrap (ssVortexF.glsl) reads as a lit
                // rim on the sun-facing side of the funnel and stays in the dark grey-blue base on the far side.
                const F32 facing_r = SSVortex::facingLight(right * sun_horiz);
                const F32 facing_l = 1.f - facing_r;

                // <SS:Nexii> Review finding 5: the multi-vortex term is a RADIUS mask (this file's own file-top
                // comment / ssvortexcore.h's cardHalfWidthM comment), never a brightness/alpha multiply - the
                // fragment stage needs geometry to discard/fade against out to radiusModulation's own upper bound
                // (1 + MULTI_AMP_MAX), so the true collar radius is not enough card width; widen to
                // SSVortex::cardHalfWidthM(radius) here and let ssVortexF.glsl's radius mask carve the true
                // silhouette back out of the padded card (texcoord0.x's own [0,1] span now covers the WIDENED
                // half-width, exactly what the fragment's radial_units conversion assumes - see its comment).
                const F32 card_half_width = SSVortex::cardHalfWidthM(radius);
                const LLVector3 offset = right * card_half_width;
                const LLVector3 left_pos = axis_pos - offset;
                const LLVector3 right_pos = axis_pos + offset;

                if (s > 0)
                {
                    gGL.color4f(prev_facing_l, condensation, slot_norm, base_alpha);
                    gGL.texCoord2f(0.f, prev_h01); gGL.vertex3fv(prev_left.mV);
                    gGL.color4f(prev_facing_r, condensation, slot_norm, base_alpha);
                    gGL.texCoord2f(1.f, prev_h01); gGL.vertex3fv(prev_right.mV);
                    gGL.color4f(facing_r, condensation, slot_norm, base_alpha);
                    gGL.texCoord2f(1.f, h01); gGL.vertex3fv(right_pos.mV);

                    gGL.color4f(prev_facing_l, condensation, slot_norm, base_alpha);
                    gGL.texCoord2f(0.f, prev_h01); gGL.vertex3fv(prev_left.mV);
                    gGL.color4f(facing_r, condensation, slot_norm, base_alpha);
                    gGL.texCoord2f(1.f, h01); gGL.vertex3fv(right_pos.mV);
                    gGL.color4f(facing_l, condensation, slot_norm, base_alpha);
                    gGL.texCoord2f(0.f, h01); gGL.vertex3fv(left_pos.mV);
                }

                prev_left = left_pos;
                prev_right = right_pos;
                prev_h01 = h01;
                prev_facing_l = facing_l;
                prev_facing_r = facing_r;
            }
        }
        gGL.end();
    }

    gGL.flush();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSVortexProgram.unbind();
    gGL.setColorMask(true, true);
}
