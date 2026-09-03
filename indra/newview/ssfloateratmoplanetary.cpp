/**
 * @file ssfloateratmoplanetary.cpp
 * @brief See ssfloateratmoplanetary.h.
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

#include "ssfloateratmoplanetary.h"
#include "ssatmoenvmanager.h"
#include "ssdiscpad.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllineeditor.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltexturectrl.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "lluiimage.h"

#include <algorithm>
#include <cmath>

static LLDefaultChildRegistry::Register<SSOrbitViewCtrl> register_ss_orbit_view("ss_orbit_view");

static const F64 STATUS_POLL_INTERVAL = 0.5;

static const F32 ORBIT_VIEW_TILT = 0.45f;

static const F32 ORBIT_VIEW_LIFT =
    0.893f;

static const F32 ORBIT_RING_FLAT_EPS = 0.05f;

static const F32 ORBIT_PLANET_FIRST_RING = 90.f;
static const F32 ORBIT_MOON_FIRST_RING = 26.f;
static const F32 ORBIT_MOON_RING_SPACING = 16.f;

static const F32 ORBIT_RADIUS_LOG_WEIGHT = 0.4f;
static const F32 ORBIT_PLANET_RING_FLOOR = 30.f;
static const F32 ORBIT_MOON_RING_FLOOR = 10.f;
static const F32 ORBIT_PAIR_SEPARATION = 56.f;

static const F32 ORBIT_BODY_HIT_RADIUS = 10.f;
static const F32 ORBIT_RING_HIT_RADIUS = 5.f;

static const F32 ORBIT_PAIR_HANDLE_HIT_RADIUS = 8.f;
static const F32 ORBIT_PAIR_HANDLE_DRAW_RADIUS = 5.f;
static const F32 ORBIT_PAIR_HANDLE_DOT_RADIUS = 1.5f;

static const S32 ORBIT_RING_SEGMENTS = 96;

static const F32 ORBIT_ZOOM_MIN = 0.5f;
static const F32 ORBIT_ZOOM_MAX = 4.f;
static const F32 ORBIT_ZOOM_STEP = 1.2599f;

static const S32 ORBIT_HOME_ICON_SIZE = 16;

static const F32 ORBIT_LABEL_PAD = 2.f;
static const S32 ORBIT_LABEL_ANGLE_STEPS = 24;

static const F32 SS_METRES_PER_AU = 1.496e11f;
static const F32 SS_METRES_PER_KM = 1000.f;
static const F32 SS_METRES_PER_SOLAR_DIAMETER = 1.392e9f;

// Wraps to [0, 360).
static F32 ss_wrap_phase_deg(F32 deg)
{
    deg = fmodf(deg, 360.f);
    return (deg < 0.f) ? deg + 360.f : deg;
}

// Widget params.
SSOrbitViewCtrl::Params::Params()
{
}

// The angled top-down orbital map: bodies on display rings, selectable and draggable.
SSOrbitViewCtrl::SSOrbitViewCtrl(const Params& p) :
    LLUICtrl(p)
{
}

// A ring phase to screen coordinates under the view tilt.
void SSOrbitViewCtrl::projectOnRing(F32 anchor_x, F32 anchor_y, F32 ring_radius, F32 tilt_rad,
                                    F32 phase_deg, F32& out_x, F32& out_y)
{
    const F32 a = phase_deg * DEG_TO_RAD;
    const F32 in_plane = sinf(a) * ring_radius;

    const F32 dx = cosf(a) * ring_radius;
    const F32 dy = in_plane * cosf(tilt_rad);
    const F32 dz = in_plane * sinf(tilt_rad);

    out_x = anchor_x + dx;
    out_y = anchor_y + dy * ORBIT_VIEW_TILT + dz * ORBIT_VIEW_LIFT;
}

// Screen point back to a ring phase - the drag inverse of projectOnRing.
F32 SSOrbitViewCtrl::inversePhaseDeg(F32 anchor_x, F32 anchor_y, F32 tilt_rad, S32 x, S32 y)
{
    const F32 dx = (F32)x - anchor_x;
    const F32 dy = (F32)y - anchor_y;

    const F32 depth = cosf(tilt_rad) * ORBIT_VIEW_TILT + sinf(tilt_rad) * ORBIT_VIEW_LIFT;

    if (llabs(depth) < ORBIT_RING_FLAT_EPS)
    {
        F32 edge_deg = (dx >= 0.f) ? 0.f : 180.f;
        return edge_deg;
    }

    F32 phase_deg = atan2f(dy / depth, dx) * RAD_TO_DEG;
    if (phase_deg < 0.f) phase_deg += 360.f;
    return phase_deg;
}

// Both members of a bound sun pair, senior first.
bool SSOrbitViewCtrl::sunPairMembers(const SSAtmoEnvPlanetary& planetary, S32 index,
                                     S32& out_senior, S32& out_junior)
{
    const S32 n = (S32)planetary.mBodies.size();
    if (index < 0 || index >= n) return false;
    if (planetary.mBodies[index].mKind != SSAtmoEnvCelestialBody::SUN) return false;

    const S32 partner = planetary.mBodies[index].mBoundPartnerIndex;
    if (partner < 0 || partner >= n || partner == index) return false;
    if (planetary.mBodies[partner].mKind != SSAtmoEnvCelestialBody::SUN) return false;
    if (planetary.mBodies[partner].mBoundPartnerIndex != index) return false;

    out_senior = llmin(index, partner);
    out_junior = llmax(index, partner);
    return true;
}

// Places a bound pair about its shared centre, masses deciding the split.
void SSOrbitViewCtrl::placePairMembers(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out,
                                       S32 senior, S32 junior, F32 centre_x, F32 centre_y)
{
    const F32 mass_s = llmax(planetary.mBodies[senior].mMassRelative, 0.0001f);
    const F32 mass_j = llmax(planetary.mBodies[junior].mMassRelative, 0.0001f);
    const F32 total = mass_s + mass_j;
    const F32 phase_deg = planetary.mBodies[junior].mOrbitalPhaseDeg;
    const F32 tilt_rad = out[junior].mTiltRad;

    projectOnRing(centre_x, centre_y, ORBIT_PAIR_SEPARATION * (mass_s / total), tilt_rad,
                  phase_deg, out[junior].mX, out[junior].mY);
    projectOnRing(centre_x, centre_y, ORBIT_PAIR_SEPARATION * (mass_j / total), tilt_rad,
                  phase_deg + 180.f, out[senior].mX, out[senior].mY);

    out[senior].mPairPartner = junior;
    out[junior].mPairPartner = senior;
    out[senior].mPairCentreX = centre_x;
    out[senior].mPairCentreY = centre_y;
    out[junior].mPairCentreX = centre_x;
    out[junior].mPairCentreY = centre_y;
    out[junior].mAnchorX = centre_x;
    out[junior].mAnchorY = centre_y;
    out[senior].mResolved = true;
    out[junior].mResolved = true;
}

// Lays out every body: display rings around parents, pairs about their centres, zoom applied.
void SSOrbitViewCtrl::computeLayout(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out) const
{
    const S32 n = (S32)planetary.mBodies.size();
    out.assign((size_t)n, Placement());
    for (S32 i = 0; i < n; ++i)
    {
        out[i].mIndex = i;
        const F32 diameter = llmax(planetary.mBodies[i].mDiameterM, 2.f);
        out[i].mDrawRadius = llclamp(1.5f * log10f(diameter), 4.f, 14.f);
        out[i].mTiltRad = llclamp(planetary.mBodies[i].mOrbitalInclinationDeg, -90.f, 90.f)
                          * DEG_TO_RAD;
    }
    if (n == 0) return;

    std::vector<S32> eff_parent((size_t)n, -1);
    for (S32 i = 0; i < n; ++i)
    {
        eff_parent[i] = planetary.effectiveParent(i);
    }

    std::vector<S32> rank((size_t)n, 0);
    std::vector<F32> spread_frac((size_t)n, 0.f);
    for (S32 parent = -1; parent < n; ++parent)
    {
        std::vector<S32> siblings;
        for (S32 i = 0; i < n; ++i)
        {
            if (eff_parent[i] == parent) siblings.push_back(i);
        }
        std::stable_sort(siblings.begin(), siblings.end(),
            [&planetary](S32 a, S32 b)
            { return planetary.mBodies[a].mOrbitalRadius < planetary.mBodies[b].mOrbitalRadius; });

        const F32 log_min = (siblings.empty()) ? 0.f
            : log10f(llmax(planetary.mBodies[siblings.front()].mOrbitalRadius, 1.f));
        const F32 log_max = (siblings.empty()) ? 0.f
            : log10f(llmax(planetary.mBodies[siblings.back()].mOrbitalRadius, 1.f));
        const F32 log_span = log_max - log_min;

        for (size_t r = 0; r < siblings.size(); ++r)
        {
            rank[siblings[r]] = (S32)r;

            const F32 steps = (F32)llmax((S32)siblings.size() - 1, 1);
            const F32 rank_frac = (F32)r / steps;
            const F32 log_frac = (log_span > 0.0001f)
                ? (log10f(llmax(planetary.mBodies[siblings[r]].mOrbitalRadius, 1.f)) - log_min) / log_span
                : rank_frac;
            spread_frac[siblings[r]] = rank_frac * (1.f - ORBIT_RADIUS_LOG_WEIGHT)
                                     + log_frac * ORBIT_RADIUS_LOG_WEIGHT;
        }
    }

    std::vector<std::vector<S32>> units;
    {
        std::vector<bool> grouped((size_t)n, false);
        for (S32 i = 0; i < n; ++i)
        {
            if (eff_parent[i] != -1 || grouped[i]) continue;
            grouped[i] = true;
            const S32 partner = planetary.mBodies[i].mBoundPartnerIndex;
            if (partner >= 0 && partner < n && partner != i && !grouped[partner]
                && eff_parent[partner] == -1
                && planetary.mBodies[partner].mBoundPartnerIndex == i)
            {
                grouped[partner] = true;
                units.push_back({ i, partner });
            }
            else
            {
                units.push_back({ i });
            }
        }
    }

    const F32 width = (F32)getLocalRect().getWidth();
    const F32 height = (F32)getLocalRect().getHeight();
    const F32 centre_y = height * 0.5f;
    const S32 unit_count = (S32)units.size();
    const F32 unit_span = (unit_count > 1) ? (width - 80.f) / (F32)unit_count : width;

    F32 extent = llmin(width, height / ORBIT_VIEW_TILT) * 0.5f - 40.f;
    if (unit_count > 1) extent = llmin(extent, unit_span * 0.5f - 12.f);
    extent = llmax(extent, ORBIT_PLANET_FIRST_RING + 50.f);

    F32 sun_group_anchor_x = width * 0.5f;
    F32 sun_group_anchor_y = centre_y;
    bool have_sun_group_anchor = false;

    for (S32 u = 0; u < unit_count; ++u)
    {
        const F32 anchor_x = (unit_count > 1)
            ? 40.f + unit_span * ((F32)u + 0.5f)
            : width * 0.5f;

        if (!have_sun_group_anchor
            && planetary.mBodies[units[u][0]].mKind == SSAtmoEnvCelestialBody::SUN)
        {
            sun_group_anchor_x = anchor_x;
            sun_group_anchor_y = centre_y;
            have_sun_group_anchor = true;
        }

        if (units[u].size() == 2)
        {
            const S32 senior = units[u][0];
            const S32 junior = units[u][1];
            out[senior].mAnchorX = anchor_x;
            out[senior].mAnchorY = centre_y;
            placePairMembers(planetary, out, senior, junior, anchor_x, centre_y);
        }
        else
        {
            const S32 i = units[u][0];
            out[i].mX = anchor_x;
            out[i].mY = centre_y;
            out[i].mAnchorX = anchor_x;
            out[i].mAnchorY = centre_y;
            out[i].mResolved = true;
        }
    }

    const F32 sun_planet_scale = llclamp(planetary.mSunPlanetScale, 0.f, 1.f);
    const F32 planet_moon_scale = llclamp(planetary.mPlanetMoonScale, 0.f, 1.f);
    for (S32 pass = 0; pass < 4; ++pass)
    {
        bool progressed = false;
        for (S32 i = 0; i < n; ++i)
        {
            if (out[i].mResolved) continue;
            const S32 parent = eff_parent[i];
            if (parent < 0 || !out[parent].mResolved) continue;

            F32 anchor_x = out[parent].mX;
            F32 anchor_y = out[parent].mY;
            const S32 partner = planetary.mBodies[parent].mBoundPartnerIndex;
            if (partner >= 0 && partner < n && partner != parent && out[partner].mResolved)
            {
                const F32 mass_p = llmax(planetary.mBodies[parent].mMassRelative, 0.0001f);
                const F32 mass_q = llmax(planetary.mBodies[partner].mMassRelative, 0.0001f);
                anchor_x = (out[parent].mX * mass_p + out[partner].mX * mass_q) / (mass_p + mass_q);
                anchor_y = (out[parent].mY * mass_p + out[partner].mY * mass_q) / (mass_p + mass_q);
            }

            F32 base;
            F32 floor_radius;
            F32 scale;
            if (planetary.mBodies[i].mKind == SSAtmoEnvCelestialBody::MOON)
            {
                base = ORBIT_MOON_FIRST_RING + ORBIT_MOON_RING_SPACING * (F32)rank[i];
                floor_radius = ORBIT_MOON_RING_FLOOR;
                scale = planet_moon_scale;
            }
            else
            {
                base = ORBIT_PLANET_FIRST_RING
                     + (extent - ORBIT_PLANET_FIRST_RING) * spread_frac[i];
                floor_radius = ORBIT_PLANET_RING_FLOOR;
                scale = sun_planet_scale;
            }
            const F32 ring = floor_radius + (base - floor_radius) * scale;

            out[i].mAnchorX = anchor_x;
            out[i].mAnchorY = anchor_y;
            out[i].mRingCentreX = anchor_x;
            out[i].mRingCentreY = anchor_y;
            out[i].mRingRadius = ring;

            S32 pair_senior = -1;
            S32 pair_junior = -1;
            if (sunPairMembers(planetary, i, pair_senior, pair_junior)
                && pair_senior == i
                && eff_parent[pair_junior] == parent
                && !out[pair_junior].mResolved)
            {
                F32 pair_cx = 0.f;
                F32 pair_cy = 0.f;
                projectOnRing(anchor_x, anchor_y, ring, out[i].mTiltRad,
                              planetary.mBodies[i].mOrbitalPhaseDeg, pair_cx, pair_cy);
                placePairMembers(planetary, out, pair_senior, pair_junior, pair_cx, pair_cy);
            }
            else
            {
                projectOnRing(anchor_x, anchor_y, ring, out[i].mTiltRad,
                              planetary.mBodies[i].mOrbitalPhaseDeg, out[i].mX, out[i].mY);
                out[i].mResolved = true;
            }
            progressed = true;
        }
        if (!progressed) break;
    }

    if (have_sun_group_anchor)
    {
        F32 weighted_x = 0.f;
        F32 weighted_y = 0.f;
        F32 total_mass = 0.f;
        for (S32 i = 0; i < n; ++i)
        {
            if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN || !out[i].mResolved) continue;
            const F32 mass = llmax(planetary.mBodies[i].mMassRelative, 0.0001f);
            weighted_x += out[i].mX * mass;
            weighted_y += out[i].mY * mass;
            total_mass += mass;
        }
        if (total_mass > 0.f)
        {
            const F32 shift_x = sun_group_anchor_x - weighted_x / total_mass;
            const F32 shift_y = sun_group_anchor_y - weighted_y / total_mass;
            for (S32 i = 0; i < n; ++i)
            {
                if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN || !out[i].mResolved) continue;
                out[i].mX += shift_x;
                out[i].mY += shift_y;
                out[i].mAnchorX += shift_x;
                out[i].mAnchorY += shift_y;
                out[i].mPairCentreX += shift_x;
                out[i].mPairCentreY += shift_y;
            }
        }

        for (S32 i = 0; i < n; ++i)
        {
            if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN) continue;
            if (!out[i].mResolved || out[i].mRingRadius <= 0.5f) continue;
            const S32 parent = eff_parent[i];
            if (parent < 0 || parent >= n || !out[parent].mResolved) continue;
            if (planetary.mBodies[parent].mKind != SSAtmoEnvCelestialBody::SUN) continue;

            F32 mass_outer = llmax(planetary.mBodies[i].mMassRelative, 0.0001f);
            if (out[i].mPairPartner >= 0)
            {
                mass_outer += llmax(planetary.mBodies[out[i].mPairPartner].mMassRelative, 0.0001f);
            }
            F32 mass_inner = llmax(planetary.mBodies[parent].mMassRelative, 0.0001f);
            const S32 parent_partner = out[parent].mPairPartner;
            if (parent_partner >= 0)
            {
                mass_inner += llmax(planetary.mBodies[parent_partner].mMassRelative, 0.0001f);
            }
            const F32 total = mass_inner + mass_outer;
            const F32 r_full = out[i].mRingRadius;

            out[i].mRingCentreX = sun_group_anchor_x;
            out[i].mRingCentreY = sun_group_anchor_y;
            out[i].mRingRadius = r_full * (mass_inner / total);
            out[i].mCounterRingRadius = r_full * (mass_outer / total);
            if (parent_partner >= 0)
            {
                out[i].mHasCounterHandle = true;
                out[i].mCounterCentreX = out[parent].mPairCentreX;
                out[i].mCounterCentreY = out[parent].mPairCentreY;
            }
        }
    }

    S32 stranded = 0;
    for (S32 i = 0; i < n; ++i)
    {
        if (out[i].mResolved) continue;
        out[i].mX = 24.f + 28.f * (F32)stranded;
        out[i].mY = 24.f;
        out[i].mAnchorX = out[i].mX;
        out[i].mAnchorY = out[i].mY;
        out[i].mResolved = true;
        ++stranded;
    }

    if (mZoom != 1.f)
    {
        const F32 cx = width * 0.5f;
        const F32 cy = height * 0.5f;
        for (Placement& p : out)
        {
            p.mX = cx + (p.mX - cx) * mZoom;
            p.mY = cy + (p.mY - cy) * mZoom;
            p.mAnchorX = cx + (p.mAnchorX - cx) * mZoom;
            p.mAnchorY = cy + (p.mAnchorY - cy) * mZoom;
            p.mPairCentreX = cx + (p.mPairCentreX - cx) * mZoom;
            p.mPairCentreY = cy + (p.mPairCentreY - cy) * mZoom;
            p.mRingCentreX = cx + (p.mRingCentreX - cx) * mZoom;
            p.mRingCentreY = cy + (p.mRingCentreY - cy) * mZoom;
            p.mCounterCentreX = cx + (p.mCounterCentreX - cx) * mZoom;
            p.mCounterCentreY = cy + (p.mCounterCentreY - cy) * mZoom;
            p.mRingRadius *= mZoom;
            p.mCounterRingRadius *= mZoom;
        }
    }
}

// One tilted display ring.
void SSOrbitViewCtrl::drawRing(F32 centre_x, F32 centre_y, F32 radius, F32 tilt_rad, const LLColor4& color) const
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::LINE_LOOP);
    for (S32 seg = 0; seg < ORBIT_RING_SEGMENTS; ++seg)
    {
        const F32 phase_deg = 360.f * (F32)seg / (F32)ORBIT_RING_SEGMENTS;
        F32 x = 0.f, y = 0.f;
        projectOnRing(centre_x, centre_y, radius, tilt_rad, phase_deg, x, y);
        gGL.vertex2f(x, y);
    }
    gGL.end();
}

// Draws rings, bodies, selection and drag feedback.
void SSOrbitViewCtrl::draw()
{
    const LLRect local = getLocalRect();
    gl_rect_2d(local, LLColor4(0.06f, 0.07f, 0.10f, 1.f), true);
    gl_rect_2d(local, LLColor4(0.32f, 0.34f, 0.38f, 1.f), false);

    SSAtmoEnvPlanetary* planetary = mPlanetary ? mPlanetary() : nullptr;
    if (!planetary)
    {
        LLView::draw();
        return;
    }

    std::vector<Placement> placements;
    computeLayout(*planetary, placements);

    const std::vector<S32> emitters = planetary->lightEmitterIndices();

    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const bool selected = (p.mIndex == mSelectedIndex);
        const bool ring_hovered = (p.mIndex == mHoverIndex && !mHoverOnBody);
        const LLColor4 ring_color = selected ? LLColor4(0.75f, 0.78f, 0.85f, 0.9f)
                                             : ring_hovered ? LLColor4(0.62f, 0.65f, 0.72f, 0.6f)
                                                            : LLColor4(0.45f, 0.45f, 0.50f, 0.35f);
        if (p.mRingRadius > 0.5f)
        {
            drawRing(p.mRingCentreX, p.mRingCentreY, p.mRingRadius, p.mTiltRad, ring_color);
        }
        if (p.mCounterRingRadius > 0.5f)
        {
            drawRing(p.mRingCentreX, p.mRingCentreY, p.mCounterRingRadius, p.mTiltRad, ring_color);
        }
    }

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const SSAtmoEnvCelestialBody& body = planetary->mBodies[p.mIndex];
        const bool selected = (p.mIndex == mSelectedIndex);
        const bool is_emitter =
            std::find(emitters.begin(), emitters.end(), p.mIndex) != emitters.end();

        if (is_emitter)
        {
            gGL.color4f(1.f, 0.85f, 0.45f, 0.35f);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius + 5.f, 24, false);
        }

        LLColor4 fill;
        switch (body.mKind)
        {
            case SSAtmoEnvCelestialBody::SUN:    fill = LLColor4(1.00f, 0.82f, 0.35f, 1.f); break;
            case SSAtmoEnvCelestialBody::PLANET: fill = LLColor4(0.55f, 0.72f, 0.95f, 1.f); break;
            case SSAtmoEnvCelestialBody::MOON:   fill = LLColor4(0.72f, 0.72f, 0.78f, 1.f); break;
        }
        const bool body_hovered = (p.mIndex == mHoverIndex && mHoverOnBody);
        if (selected || body_hovered)
        {
            fill.mV[0] = llmin(fill.mV[0] * 1.25f, 1.f);
            fill.mV[1] = llmin(fill.mV[1] * 1.25f, 1.f);
            fill.mV[2] = llmin(fill.mV[2] * 1.25f, 1.f);
        }

        // <SS:Nexii> A custom-textured body draws as a quad with that art - the same billboard the world renderer shows - instead of the flat kind-coloured disc, fetched like the renderer's billboards (lldrawpoolwlsky.cpp) so the designer previews the exact disc a resident sees. Untextured bodies keep the disc.
        if (body.mCustomTexture.notNull())
        {
            LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
                body.mCustomTexture, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
            if (tex)
            {
                tex->addTextureStats(F32(llmax(1, (S32)(p.mDrawRadius * 2.f))
                                         * llmax(1, (S32)(p.mDrawRadius * 2.f))));
                const S32 r = (S32)p.mDrawRadius;
                gl_draw_scaled_image((S32)p.mX - r, (S32)p.mY - r, r * 2, r * 2, tex,
                                     UI_VERTEX_COLOR, LLRectf(0.f, 1.f, 1.f, 0.f));
                // Keep the hover/selection cue the old fill brightening gave,
                // without tinting the art itself.
                if (selected || body_hovered)
                {
                    gGL.color4f(1.f, 1.f, 1.f, body_hovered ? 0.25f : 0.45f);
                    gl_circle_2d(p.mX, p.mY, p.mDrawRadius, 24, true);
                }
            }
            else
            {
                gGL.color4fv(fill.mV);
                gl_circle_2d(p.mX, p.mY, p.mDrawRadius, 24, true);
            }
        }
        else
        {
            gGL.color4fv(fill.mV);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius, 24, true);
        }

        if (body.mIsHome)
        {
            gGL.color4f(0.30f, 0.95f, 0.70f, 0.9f);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius + 2.f, 24, false);
        }
        if (selected)
        {
            gGL.color4f(1.f, 1.f, 1.f, 0.9f);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius + (body.mIsHome ? 4.f : 3.f), 24, false);
        }

        if (body.mIsHome)
        {
            LLPointer<LLUIImage> home_icon = LLUI::getUIImage("Home_Off");
            if (home_icon)
            {
                home_icon->draw((S32)(p.mX - (F32)(ORBIT_HOME_ICON_SIZE / 2)),
                                (S32)(p.mY - (F32)(ORBIT_HOME_ICON_SIZE / 2)),
                                ORBIT_HOME_ICON_SIZE, ORBIT_HOME_ICON_SIZE,
                                LLColor4(0.30f, 0.95f, 0.70f, 0.9f));
            }
        }
    }

    {
        struct SSLabelRect { F32 mLeft; F32 mRight; F32 mTop; F32 mBottom; };
        std::vector<SSLabelRect> placed_rects;
        placed_rects.reserve(placements.size());

        std::vector<S32> label_order;
        label_order.reserve(placements.size());
        if (mSelectedIndex >= 0 && mSelectedIndex < (S32)placements.size())
        {
            label_order.push_back(mSelectedIndex);
        }
        for (S32 i = 0; i < (S32)placements.size(); ++i)
        {
            if (i != mSelectedIndex) label_order.push_back(i);
        }

        const F32 line_h = (F32)font->getLineHeight();
        const F32 angle_step = 360.f / (F32)ORBIT_LABEL_ANGLE_STEPS;

        for (const S32 index : label_order)
        {
            const Placement& p = placements[index];
            if (!p.mResolved) continue;
            const SSAtmoEnvCelestialBody& body = planetary->mBodies[p.mIndex];
            const bool selected = (p.mIndex == mSelectedIndex);

            const F32 half_w = 0.5f * (F32)font->getWidth(body.mName);
            const F32 exclusion = p.mDrawRadius + ORBIT_LABEL_PAD;

            SSLabelRect rect = SSLabelRect();
            bool found = false;
            for (S32 probe = 0; probe < ORBIT_LABEL_ANGLE_STEPS && !found; ++probe)
            {
                const F32 delta = (F32)((probe + 1) / 2) * angle_step
                                * ((probe % 2 == 1) ? 1.f : -1.f);
                const F32 theta = (-90.f + delta) * DEG_TO_RAD;
                const F32 dir_x = cosf(theta);
                const F32 dir_y = sinf(theta);
                const F32 offset = exclusion + half_w * llabs(dir_x) + 0.5f * line_h * llabs(dir_y);
                const F32 cx = p.mX + dir_x * offset;
                const F32 cy = p.mY + dir_y * offset;

                SSLabelRect candidate;
                candidate.mLeft = cx - half_w;
                candidate.mRight = cx + half_w;
                candidate.mTop = cy + 0.5f * line_h;
                candidate.mBottom = cy - 0.5f * line_h;

                bool collides = false;
                for (const SSLabelRect& other : placed_rects)
                {
                    if (candidate.mLeft < other.mRight && candidate.mRight > other.mLeft
                        && candidate.mBottom < other.mTop && candidate.mTop > other.mBottom)
                    {
                        collides = true;
                        break;
                    }
                }
                if (probe == 0) rect = candidate;
                if (!collides)
                {
                    rect = candidate;
                    found = true;
                }
            }

            placed_rects.push_back(rect);
            font->renderUTF8(body.mName, 0,
                             (S32)(0.5f * (rect.mLeft + rect.mRight)), (S32)rect.mTop,
                             selected ? LLColor4::white : LLColor4(0.85f, 0.85f, 0.85f, 0.9f),
                             LLFontGL::HCENTER, LLFontGL::TOP);
        }
    }

    const S32 handle_index = (mDragMode == DRAG_CENTRE) ? mDragIndex : mHoverHandleIndex;
    const bool handle_antipodal = (mDragMode == DRAG_CENTRE) ? mDragAntipodal : mHoverHandleAntipodal;
    if (handle_index >= 0 && handle_index < (S32)placements.size())
    {
        const Placement& p = placements[handle_index];
        const bool handle_valid = handle_antipodal
            ? p.mHasCounterHandle
            : (p.mPairPartner >= 0 && (p.mRingRadius > 0.5f || p.mCounterRingRadius > 0.5f));
        if (p.mResolved && handle_valid)
        {
            const F32 hx = handle_antipodal ? p.mCounterCentreX : p.mPairCentreX;
            const F32 hy = handle_antipodal ? p.mCounterCentreY : p.mPairCentreY;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.color4f(1.f, 1.f, 1.f, 0.9f);
            gl_circle_2d(hx, hy, ORBIT_PAIR_HANDLE_DRAW_RADIUS, 24, false);
            gl_circle_2d(hx, hy, ORBIT_PAIR_HANDLE_DOT_RADIUS, 8, true);
        }
    }

    LLView::draw();
}

// Which body (or whose ring) a point is on.
S32 SSOrbitViewCtrl::hitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_on_body) const
{
    const F32 fx = (F32)x;
    const F32 fy = (F32)y;

    S32 best = -1;
    F32 best_dist = F32_MAX;
    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const F32 dist = sqrtf((fx - p.mX) * (fx - p.mX) + (fy - p.mY) * (fy - p.mY));
        const F32 tolerance = llmax(ORBIT_BODY_HIT_RADIUS, p.mDrawRadius + 2.f);
        if (dist <= tolerance && dist < best_dist)
        {
            best = p.mIndex;
            best_dist = dist;
        }
    }
    if (best >= 0)
    {
        out_on_body = true;
        return best;
    }

    out_on_body = false;
    best_dist = F32_MAX;
    for (const Placement& p : placements)
    {
        if (!p.mResolved || p.mRingRadius <= 0.5f) continue;
        for (S32 seg = 0; seg < ORBIT_RING_SEGMENTS; ++seg)
        {
            const F32 phase_deg = 360.f * (F32)seg / (F32)ORBIT_RING_SEGMENTS;
            F32 px = 0.f, py = 0.f;
            projectOnRing(p.mRingCentreX, p.mRingCentreY, p.mRingRadius, p.mTiltRad, phase_deg, px, py);
            const F32 dist = sqrtf((fx - px) * (fx - px) + (fy - py) * (fy - py));
            if (dist <= ORBIT_RING_HIT_RADIUS && dist < best_dist)
            {
                best = p.mIndex;
                best_dist = dist;
            }
        }
    }
    return best;
}

// Hit test for drag handles, including a pair centre's antipodal handle.
S32 SSOrbitViewCtrl::handleHitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_antipodal) const
{
    const F32 fx = (F32)x;
    const F32 fy = (F32)y;
    S32 best = -1;
    F32 best_dist = F32_MAX;
    out_antipodal = false;
    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const bool orbiting = (p.mRingRadius > 0.5f || p.mCounterRingRadius > 0.5f);

        if (p.mPairPartner >= 0 && p.mIndex < p.mPairPartner && orbiting)
        {
            const F32 dist = sqrtf((fx - p.mPairCentreX) * (fx - p.mPairCentreX)
                                   + (fy - p.mPairCentreY) * (fy - p.mPairCentreY));
            if (dist <= ORBIT_PAIR_HANDLE_HIT_RADIUS && dist < best_dist)
            {
                best = p.mIndex;
                best_dist = dist;
                out_antipodal = false;
            }
        }

        if (p.mHasCounterHandle)
        {
            const F32 dist = sqrtf((fx - p.mCounterCentreX) * (fx - p.mCounterCentreX)
                                   + (fy - p.mCounterCentreY) * (fy - p.mCounterCentreY));
            if (dist <= ORBIT_PAIR_HANDLE_HIT_RADIUS && dist < best_dist)
            {
                best = p.mIndex;
                best_dist = dist;
                out_antipodal = true;
            }
        }
    }
    return best;
}

// Select and start a phase drag.
bool SSOrbitViewCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    SSAtmoEnvPlanetary* planetary = mPlanetary ? mPlanetary() : nullptr;
    if (!planetary) return LLUICtrl::handleMouseDown(x, y, mask);

    std::vector<Placement> placements;
    computeLayout(*planetary, placements);

    mHoverIndex = -1;
    mHoverOnBody = false;
    mHoverHandleIndex = -1;
    mHoverHandleAntipodal = false;

    bool handle_antipodal = false;
    const S32 handle = handleHitTest(placements, x, y, handle_antipodal);
    bool on_body = false;
    const S32 hit = (handle >= 0) ? -1 : hitTest(placements, x, y, on_body);

    if (handle >= 0)
    {
        if (mOnSelect) mOnSelect(handle);
        mSelectedIndex = handle;
        mDragMode = DRAG_CENTRE;
        mDragAntipodal = handle_antipodal;
        mDragIndex = handle;
        const Placement& p = placements[handle];
        const F32 raw = handle_antipodal
            ? inversePhaseDeg(p.mRingCentreX, p.mRingCentreY, p.mTiltRad, x, y)
            : inversePhaseDeg(p.mAnchorX, p.mAnchorY, p.mTiltRad, x, y);
        mDragOffsetDeg = planetary->mBodies[handle].mOrbitalPhaseDeg - raw;
        gFocusMgr.setMouseCapture(this);
    }
    else if (hit >= 0 && on_body)
    {
        if (mOnSelect) mOnSelect(hit);
        mSelectedIndex = hit;

        if (placements[hit].mPairPartner >= 0)
        {
            const S32 junior = llmax(hit, placements[hit].mPairPartner);
            mDragMode = DRAG_PAIR;
            mDragIndex = hit;
            const F32 raw = inversePhaseDeg(placements[hit].mPairCentreX,
                                            placements[hit].mPairCentreY,
                                            placements[junior].mTiltRad, x, y);
            mDragOffsetDeg = planetary->mBodies[junior].mOrbitalPhaseDeg - raw;
            gFocusMgr.setMouseCapture(this);
        }
        else if (placements[hit].mRingRadius > 0.5f)
        {
            mDragMode = DRAG_RING;
            mDragIndex = hit;
            const F32 raw = inversePhaseDeg(placements[hit].mAnchorX,
                                            placements[hit].mAnchorY,
                                            placements[hit].mTiltRad, x, y);
            mDragOffsetDeg = planetary->mBodies[hit].mOrbitalPhaseDeg - raw;
            gFocusMgr.setMouseCapture(this);
        }
    }
    else if (hit >= 0)
    {
        if (mOnSelect) mOnSelect(hit);
        mSelectedIndex = hit;
    }
    return true;
}

// Drag: writes the new orbital phase through the drag inverse.
bool SSOrbitViewCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    SSAtmoEnvPlanetary* planetary = mPlanetary ? mPlanetary() : nullptr;

    if (!hasMouseCapture() || mDragMode == DRAG_NONE || mDragIndex < 0)
    {
        mHoverHandleIndex = -1;
        mHoverHandleAntipodal = false;
        mHoverIndex = -1;
        mHoverOnBody = false;
        if (planetary)
        {
            std::vector<Placement> placements;
            computeLayout(*planetary, placements);
            bool antipodal = false;
            mHoverHandleIndex = handleHitTest(placements, x, y, antipodal);
            mHoverHandleAntipodal = antipodal;
            if (mHoverHandleIndex < 0)
            {
                bool on_body = false;
                mHoverIndex = hitTest(placements, x, y, on_body);
                mHoverOnBody = on_body;
            }
        }
        return LLUICtrl::handleHover(x, y, mask);
    }

    if (!planetary || mDragIndex >= (S32)planetary->mBodies.size())
    {
        mDragIndex = -1;
        mDragMode = DRAG_NONE;
        gFocusMgr.setMouseCapture(nullptr);
        return true;
    }

    std::vector<Placement> placements;
    computeLayout(*planetary, placements);
    const Placement& p = placements[mDragIndex];
    if (!p.mResolved) return true;

    if (mDragMode == DRAG_PAIR)
    {
        const S32 partner = p.mPairPartner;
        if (partner < 0 || partner >= (S32)placements.size())
        {
            mDragIndex = -1;
            mDragMode = DRAG_NONE;
            gFocusMgr.setMouseCapture(nullptr);
            return true;
        }
        const S32 junior = llmax(mDragIndex, partner);
        const F32 raw = inversePhaseDeg(p.mPairCentreX, p.mPairCentreY,
                                        placements[junior].mTiltRad, x, y);
        planetary->mBodies[junior].mOrbitalPhaseDeg = ss_wrap_phase_deg(raw + mDragOffsetDeg);
    }
    else
    {
        if (p.mRingRadius <= 0.5f && p.mCounterRingRadius <= 0.5f) return true;
        const F32 raw = (mDragMode == DRAG_CENTRE && mDragAntipodal)
            ? inversePhaseDeg(p.mRingCentreX, p.mRingCentreY, p.mTiltRad, x, y)
            : inversePhaseDeg(p.mAnchorX, p.mAnchorY, p.mTiltRad, x, y);
        planetary->mBodies[mDragIndex].mOrbitalPhaseDeg = ss_wrap_phase_deg(raw + mDragOffsetDeg);
    }

    if (mOnDrag) mOnDrag();
    return true;
}

// Ends a drag.
bool SSOrbitViewCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        mDragIndex = -1;
        mDragMode = DRAG_NONE;
        mDragAntipodal = false;
        mDragOffsetDeg = 0.f;
        gFocusMgr.setMouseCapture(nullptr);
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

// Clears hover.
void SSOrbitViewCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverHandleIndex = -1;
    mHoverHandleAntipodal = false;
    mHoverIndex = -1;
    mHoverOnBody = false;
    LLUICtrl::onMouseLeave(x, y, mask);
}

// Zoom step.
void SSOrbitViewCtrl::zoomBy(S32 steps)
{
    mZoom = llclamp(mZoom * powf(ORBIT_ZOOM_STEP, (F32)steps), ORBIT_ZOOM_MIN, ORBIT_ZOOM_MAX);
}

// Wheel zoom.
bool SSOrbitViewCtrl::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    zoomBy(-clicks);
    return true;
}

namespace
{
    // Display name for a body kind.
    const char* bodyKindName(SSAtmoEnvCelestialBody::EKind kind)
    {
        switch (kind)
        {
            case SSAtmoEnvCelestialBody::SUN:    return "Sun";
            case SSAtmoEnvCelestialBody::PLANET: return "Planet";
            case SSAtmoEnvCelestialBody::MOON:   return "Moon";
        }
        return "Body";
    }

    struct SSStarTypePreset
    {
        const char* mLabel;
        F32 mDiameterD;
        F32 mMassM;
    };
    const SSStarTypePreset STAR_TYPE_PRESETS[] = {
        { "M Dwarf",     0.3f,   0.3f },
        { "K Dwarf",     0.8f,   0.8f },
        { "G (Sol)",     1.f,    1.f  },
        { "F",           1.3f,   1.3f },
        { "A",           1.8f,   2.1f },
        { "B Giant",     5.f,   10.f  },
        { "O Giant",    10.f,   30.f  },
        { "White Dwarf", 0.013f, 0.6f },
        { "Red Giant", 100.f,    1.f  },
    };
    const S32 STAR_TYPE_PRESET_COUNT = (S32)(sizeof(STAR_TYPE_PRESETS) / sizeof(STAR_TYPE_PRESETS[0]));

    // Epsilon compare for spinner round-trips.
    bool nearlyEqual(F32 a, F32 b)
    {
        return llabs(a - b) <= llmax(llabs(b) * 0.001f, 0.0001f);
    }

    // Whether a sun's orbit fields are editable, and whether it is the junior of a bound pair.
    bool ss_sun_orbit_editable(const SSAtmoEnvPlanetary& p, S32 index, bool& out_pair_junior)
    {
        out_pair_junior = false;
        if (index < 0 || index >= (S32)p.mBodies.size()) return false;
        const SSAtmoEnvCelestialBody& body = p.mBodies[index];
        if (body.mKind != SSAtmoEnvCelestialBody::SUN) return false;

        const S32 partner = body.mBoundPartnerIndex;
        const bool paired = partner >= 0 && partner < (S32)p.mBodies.size()
            && partner != index
            && p.mBodies[partner].mKind == SSAtmoEnvCelestialBody::SUN
            && p.mBodies[partner].mBoundPartnerIndex == index;
        out_pair_junior = paired && index > partner;

        return out_pair_junior || body.mParentIndex >= 0;
    }
}

// Floater shell; all content is wired in postBuild.
SSFloaterAtmoPlanetary::SSFloaterAtmoPlanetary(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the orbit view, body list and every property field.
bool SSFloaterAtmoPlanetary::postBuild()
{
    LLScrollListCtrl* body_list = getChild<LLScrollListCtrl>("body_list");
    body_list->setCommitOnSelectionChange(true);
    body_list->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSelectBody(); });

    getChild<LLButton>("add_sun_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddBody((S32)SSAtmoEnvCelestialBody::SUN); });
    getChild<LLButton>("add_planet_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddBody((S32)SSAtmoEnvCelestialBody::PLANET); });
    getChild<LLButton>("add_moon_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddBody((S32)SSAtmoEnvCelestialBody::MOON); });
    getChild<LLButton>("remove_body_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveBody(); });

    getChild<LLUICtrl>("body_name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyName(); });
    const char* body_scalar_fields[] = { "body_diameter_spinner", "body_mass_spinner",
                                         "body_orbital_radius_spinner", "body_inclination_spinner",
                                         "body_phase_spinner", "body_axial_tilt_spinner",
                                         "body_latitude_spinner", "body_disc_padding_spinner" };
    for (const char* name : body_scalar_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitBodyScalars(); });
    }

    LLComboBox* star_type_combo = getChild<LLComboBox>("body_star_type_combo");
    for (S32 i = 0; i < STAR_TYPE_PRESET_COUNT; ++i)
    {
        star_type_combo->add(STAR_TYPE_PRESETS[i].mLabel, LLSD(i));
    }
    star_type_combo->add("(Custom)", LLSD(-1));
    star_type_combo->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyStarType(); });

    getChild<LLUICtrl>("body_home_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyHome(); });
    getChild<LLUICtrl>("body_emissive_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyShading(); });
    getChild<LLUICtrl>("body_phase_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyShading(); });

    getChild<LLUICtrl>("body_light_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyLight(); });
    const char* body_ring_fields[] = { "body_ring_check", "body_ring_inner_spinner",
                                       "body_ring_outer_spinner", "body_ring_texture" };
    for (const char* name : body_ring_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitBodyRing(); });
    }
    getChild<LLUICtrl>("body_custom_texture")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyTexture(); });

    SSOrbitViewCtrl* orbit = getChild<SSOrbitViewCtrl>("orbit_view");
    orbit->setPlanetaryAccessor([this]() { return planetary(); });
    orbit->setSelectCallback([this](S32 index) { onOrbitSelect(index); });
    orbit->setDragCallback([this]() { onOrbitDrag(); });

    getChild<LLButton>("orbit_zoom_in_button")->setClickedCallback(
        [orbit](LLUICtrl*, const LLSD&) { orbit->zoomBy(1); });
    getChild<LLButton>("orbit_zoom_reset_button")->setClickedCallback(
        [orbit](LLUICtrl*, const LLSD&) { orbit->resetZoom(); });
    getChild<LLButton>("orbit_zoom_out_button")->setClickedCallback(
        [orbit](LLUICtrl*, const LLSD&) { orbit->zoomBy(-1); });

    refreshAll();
    return true;
}

// Opens targeting the track index passed in the key.
void SSFloaterAtmoPlanetary::onOpen(const LLSD& key)
{
    setTrack(key.asInteger());
}

// Polls for external asset changes so the designer never shows a stale system.
void SSFloaterAtmoPlanetary::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;
        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            // A disc texture still decoding when the auto-derive ran may be decoded by
            // now - land its padding, refreshAll shows it. Skipped while the
            // user captures the mouse mid-edit, which must not be fought.
            ssDiscPadPoll();
            refreshAll();
        }
        else
        {
            refreshTitle();
        }
    }

    LLFloater::draw();

    // <SS:Nexii> The disc-padding guide. While the padding spinner is hovered - or held mid-drag with the cursor off it - draw the disc the renderer will use onto the Texture swatch: the central 1 - 2*padding circle, the fraction the celestial chain treats as the body (ss_disc_fraction, ssatmoenvapplier.cpp). Pure UI feedback; reads the spinner's live value, so the circle tracks the drag.
    LLView* padding_spinner = getChildView("body_disc_padding_spinner");
    S32 mouse_x, mouse_y;
    LLUI::getInstance()->getMousePositionLocal(padding_spinner, &mouse_x, &mouse_y);
    LLView* held = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
    const bool padding_hovered = padding_spinner->getEnabled()
        && (padding_spinner->pointInView(mouse_x, mouse_y)
            || (held && held->hasAncestor(padding_spinner)));
    if (!padding_hovered) return;

    LLView* picker = getChildView("body_custom_texture");
    const F32 padding = llclamp(
        (F32)getChild<LLUICtrl>("body_disc_padding_spinner")->getValue().asReal(), 0.f, 0.45f);

    // The picker's rect in THIS floater's local space - the drawChildren translate has been
    // popped back to it by the time LLFloater::draw() returns, so these are draw-ready.
    S32 picker_left = 0;
    S32 picker_bottom = 0;
    for (LLView* view = picker; view && view != this; view = view->getParent())
    {
        picker_left += view->getRect().mLeft;
        picker_bottom += view->getRect().mBottom;
    }
    const LLRect& picker_rect = picker->getRect();
    const F32 centre_x = (F32)picker_left + (F32)picker_rect.getWidth() * 0.5f;
    const F32 centre_y = (F32)picker_bottom + (F32)picker_rect.getHeight() * 0.5f;
    const F32 disc_radius = (1.f - 2.f * padding)
        * 0.5f * (F32)llmin(picker_rect.getWidth(), picker_rect.getHeight());

    gGL.color4f(1.f, 0.4f, 0.8f, 0.9f);
    gl_circle_2d(centre_x, centre_y, disc_radius, 48, false);
}

// Retargets the designer at another track.
void SSFloaterAtmoPlanetary::setTrack(S32 index)
{
    if (index != mTrackIndex)
    {
        flushFocusedPropertyField();
        mSelectedBodyIndex = 0;
    }
    mTrackIndex = index;
    refreshAll();
}

// The edited track's planetary block in the LIVE asset, or null.
SSAtmoEnvPlanetary* SSFloaterAtmoPlanetary::planetary()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return nullptr;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mTrackIndex < 0 || mTrackIndex >= (S32)asset.mTracks.size()) return nullptr;
    return &asset.mTracks[mTrackIndex].mPlanetary;
}

// The selected body, or null.
SSAtmoEnvCelestialBody* SSFloaterAtmoPlanetary::selectedBody()
{
    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return nullptr;
    if (mSelectedBodyIndex < 0 || mSelectedBodyIndex >= (S32)p->mBodies.size()) return nullptr;
    return &p->mBodies[mSelectedBodyIndex];
}

// Commits the property field still holding focus before selection changes so typed values are not lost.
void SSFloaterAtmoPlanetary::flushFocusedPropertyField()
{
    LLView* focused = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
    if (!focused || !focused->hasAncestor(this)) return;

    const char* property_controls[] = {
        "body_name_editor",
        "body_orbital_radius_spinner", "body_inclination_spinner", "body_phase_spinner",
        "body_axial_tilt_spinner", "body_latitude_spinner",
        "body_diameter_spinner", "body_mass_spinner",
        "body_ring_inner_spinner", "body_ring_outer_spinner",
        "body_star_type_combo",
    };
    for (const char* name : property_controls)
    {
        LLUICtrl* ctrl = findChild<LLUICtrl>(name);
        if (!ctrl || (focused != ctrl && !focused->hasAncestor(ctrl))) continue;

        if (LLSpinCtrl* spinner = dynamic_cast<LLSpinCtrl*>(ctrl))
        {
            spinner->forceEditorCommit();
        }
        else if (dynamic_cast<LLLineEditor*>(ctrl))
        {
            ctrl->onCommit();
        }

        gFocusMgr.setKeyboardFocus(nullptr);
        return;
    }
}

// Rebuilds list, fields and title from the asset.
void SSFloaterAtmoPlanetary::refreshAll()
{
    SSAtmoEnvPlanetary* p = planetary();
    const bool valid = (p != nullptr);

    getChild<LLUICtrl>("no_system_text")->setVisible(!valid);
    getChild<LLUICtrl>("designer_left_panel")->setVisible(valid);
    SSOrbitViewCtrl* orbit = getChild<SSOrbitViewCtrl>("orbit_view");
    orbit->setVisible(valid);
    getChild<LLUICtrl>("orbit_zoom_in_button")->setVisible(valid);
    getChild<LLUICtrl>("orbit_zoom_reset_button")->setVisible(valid);
    getChild<LLUICtrl>("orbit_zoom_out_button")->setVisible(valid);

    refreshTitle();
    if (!valid) return;

    const S32 body_count = (S32)p->mBodies.size();
    if (mSelectedBodyIndex >= body_count) mSelectedBodyIndex = body_count - 1;
    if (mSelectedBodyIndex < 0 && body_count > 0) mSelectedBodyIndex = 0;

    S32 sun_count = 0;
    bool any_planet = false;
    for (const SSAtmoEnvCelestialBody& body : p->mBodies)
    {
        if (body.mKind == SSAtmoEnvCelestialBody::SUN) ++sun_count;
        else if (body.mKind == SSAtmoEnvCelestialBody::PLANET) any_planet = true;
    }
    getChild<LLUICtrl>("add_sun_button")->setEnabled(sun_count < SS_ATMOENV_MAX_SUNS);
    getChild<LLUICtrl>("add_moon_button")->setEnabled(any_planet);

    rebuildBodyList();
    refreshBodyFields();
    orbit->setSelectedIndex(mSelectedBodyIndex);
}

// Title shows the track being edited.
void SSFloaterAtmoPlanetary::refreshTitle()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    std::string title = "Planetary System";
    if (mgr->hasAsset() && mTrackIndex >= 0 && mTrackIndex < (S32)mgr->asset().mTracks.size())
    {
        title += " - " + mgr->asset().mTracks[mTrackIndex].mName;
        if (mgr->isModified()) title += " - Unsaved changes*";
    }
    else
    {
        title += " - no system to edit";
    }
    setTitle(title);
}

// Rebuilds the scene-graph list, indented by orbital parentage.
void SSFloaterAtmoPlanetary::rebuildBodyList()
{
    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;

    const S32 body_count = (S32)p->mBodies.size();

    std::vector<S32> eff_parent((size_t)body_count, -1);
    for (S32 i = 0; i < body_count; ++i)
    {
        eff_parent[i] = p->effectiveParent(i);
    }

    auto sortedSiblings = [p, body_count, &eff_parent](S32 parent)
    {
        std::vector<S32> siblings;
        for (S32 i = 0; i < body_count; ++i)
        {
            if (eff_parent[i] == parent) siblings.push_back(i);
        }
        std::stable_sort(siblings.begin(), siblings.end(),
            [p](S32 a, S32 b)
            { return p->mBodies[a].mOrbitalRadius < p->mBodies[b].mOrbitalRadius; });
        return siblings;
    };

    std::vector<std::pair<S32, S32>> ordered;
    std::vector<bool> emitted((size_t)body_count, false);
    std::function<void(S32, S32)> emit = [&](S32 index, S32 depth)
    {
        emitted[index] = true;
        ordered.push_back(std::make_pair(index, depth));
        for (S32 child : sortedSiblings(index))
        {
            if (!emitted[child]) emit(child, depth + 1);
        }
    };
    std::vector<S32> sun_indices;
    for (S32 i = 0; i < body_count; ++i)
    {
        if (p->mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN)
        {
            emitted[i] = true;
            ordered.push_back(std::make_pair(i, 0));
            sun_indices.push_back(i);
        }
    }
    for (const S32 sun : sun_indices)
    {
        for (S32 child : sortedSiblings(sun))
        {
            if (!emitted[child]) emit(child, 1);
        }
    }
    for (S32 root : sortedSiblings(-1))
    {
        if (!emitted[root]) emit(root, 0);
    }
    for (S32 i = 0; i < body_count; ++i)
    {
        if (!emitted[i])
        {
            emitted[i] = true;
            ordered.push_back(std::make_pair(i, 0));
        }
    }

    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("body_list");
    const S32 scroll_pos = list->getScrollPos();
    list->deleteAllItems();
    for (const std::pair<S32, S32>& entry : ordered)
    {
        const SSAtmoEnvCelestialBody& body = p->mBodies[entry.first];

        std::string name(std::string((size_t)(entry.second * 2), ' ') + body.mName);
        if (eff_parent[entry.first] == -1 && body.mKind == SSAtmoEnvCelestialBody::MOON)
        {
            name += " (orphan)";
        }

        LLSD row;
        row["value"] = entry.first;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = name;
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"] = bodyKindName(body.mKind);
        list->addElement(row);
    }
    list->setScrollPos(scroll_pos);
    if (mSelectedBodyIndex >= 0)
    {
        list->setSelectedByValue(LLSD(mSelectedBodyIndex), true);
    }
}

// Rewrites every property field from the selected body, enabling only what applies to its kind.
void SSFloaterAtmoPlanetary::refreshBodyFields()
{
    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;

    const S32 body_count = (S32)p->mBodies.size();
    const bool has_body = mSelectedBodyIndex >= 0 && mSelectedBodyIndex < body_count;
    getChild<LLUICtrl>("remove_body_button")->setEnabled(has_body);

    const char* body_controls[] = {
        "body_name_editor",
        "body_orbital_radius_spinner", "body_inclination_spinner", "body_phase_spinner",
        "body_axial_tilt_spinner", "body_latitude_spinner", "body_star_type_combo",
        "body_diameter_spinner", "body_mass_spinner", "body_custom_texture",
        "body_disc_padding_spinner",
        "body_ring_check", "body_ring_inner_spinner", "body_ring_outer_spinner",
        "body_ring_texture", "body_home_check", "body_light_check",
        "body_emissive_check", "body_phase_check",
    };
    for (const char* name : body_controls)
    {
        getChild<LLUICtrl>(name)->setEnabled(has_body);
    }
    if (!has_body)
    {
        getChild<LLUICtrl>("body_star_type_label")->setVisible(false);
        getChild<LLUICtrl>("body_star_type_combo")->setVisible(false);
        return;
    }

    const SSAtmoEnvCelestialBody& body = p->mBodies[mSelectedBodyIndex];
    const bool is_sun  = (body.mKind == SSAtmoEnvCelestialBody::SUN);
    const bool is_moon = (body.mKind == SSAtmoEnvCelestialBody::MOON);

    LLLineEditor* name_editor = getChild<LLLineEditor>("body_name_editor");
    if (!name_editor->hasFocus())
    {
        name_editor->setText(body.mName);
    }

    bool sun_pair_junior = false;
    const bool orbit_editable = !is_sun
        || ss_sun_orbit_editable(*p, mSelectedBodyIndex, sun_pair_junior);
    getChild<LLUICtrl>("body_orbital_radius_spinner")->setEnabled(orbit_editable);
    getChild<LLUICtrl>("body_inclination_spinner")->setEnabled(orbit_editable);
    getChild<LLUICtrl>("body_phase_spinner")->setEnabled(orbit_editable);

    const F32 radius_display = is_moon ? body.mOrbitalRadius / SS_METRES_PER_KM
                                       : body.mOrbitalRadius / SS_METRES_PER_AU;
    LLSpinCtrl* radius_spinner = getChild<LLSpinCtrl>("body_orbital_radius_spinner");
    if (!radius_spinner->hasFocus())
    {
        if (is_moon)
        {
            radius_spinner->setMinValue(1000.f);
            radius_spinner->setMaxValue(10000000.f);
            radius_spinner->setIncrement(1000.f);
            radius_spinner->setPrecision(0);
        }
        else
        {
            radius_spinner->setMinValue(is_sun ? 0.f : 0.05f);
            radius_spinner->setMaxValue(100.f);
            radius_spinner->setIncrement(0.05f);
            radius_spinner->setPrecision(2);
        }
        radius_spinner->setValue(radius_display);
    }
    getChild<LLTextBox>("body_orbital_radius_label")->setText(
        sun_pair_junior ? std::string("Pair Separation (AU)")
        : is_moon ? std::string("Orbital Radius (km)")
                  : std::string("Orbital Radius (AU)"));
    radius_spinner->setToolTip(
        sun_pair_junior
        ? std::string("Separation of the bound sun pair - the distance between its two members, split about their shared barycenter by mass. Dragging either member on the map edits the pair's orientation, this sets how far apart they sit")
        : is_sun
        ? std::string("Distance of this sun (or its pair's centre) from the inner barycenter it orbits, in AU")
        : std::string("Distance from whatever this body orbits - AU for planets, km for moons; the tab's scale dials then compress it. The map leans toward relative distances rather than mapping them exactly"));

    const F32 diameter_display = is_sun ? body.mDiameterM / SS_METRES_PER_SOLAR_DIAMETER
                                        : body.mDiameterM / SS_METRES_PER_KM;
    LLSpinCtrl* diameter_spinner = getChild<LLSpinCtrl>("body_diameter_spinner");
    if (!diameter_spinner->hasFocus())
    {
        if (is_sun)
        {
            diameter_spinner->setMinValue(0.001f);
            diameter_spinner->setMaxValue(1000.f);
            diameter_spinner->setIncrement(0.01f);
            diameter_spinner->setPrecision(3);
        }
        else
        {
            diameter_spinner->setMinValue(1.f);
            diameter_spinner->setMaxValue(20000000.f);
            diameter_spinner->setIncrement(100.f);
            diameter_spinner->setPrecision(0);
        }
        diameter_spinner->setValue(diameter_display);
    }
    getChild<LLTextBox>("body_diameter_label")->setText(
        is_sun ? std::string("Diameter (D)") : std::string("Diameter (km)"));

    const struct { const char* mName; F32 mValue; } spinners[] = {
        { "body_inclination_spinner",    body.mOrbitalInclinationDeg },
        { "body_phase_spinner",          body.mOrbitalPhaseDeg },
        { "body_axial_tilt_spinner",     body.mAxialTiltDeg },
        { "body_latitude_spinner",       body.mLatitudeDeg },
        { "body_mass_spinner",           body.mMassRelative },
        { "body_ring_inner_spinner",     body.mRingInnerRadius },
        { "body_ring_outer_spinner",     body.mRingOuterRadius },
    };
    for (const auto& s : spinners)
    {
        LLUICtrl* spinner = getChild<LLUICtrl>(s.mName);
        if (!spinner->hasFocus())
        {
            spinner->setValue(s.mValue);
        }
    }

    getChild<LLUICtrl>("body_star_type_label")->setVisible(is_sun);
    LLComboBox* star_type_combo = getChild<LLComboBox>("body_star_type_combo");
    star_type_combo->setVisible(is_sun);
    if (is_sun && !star_type_combo->hasFocus())
    {
        S32 match = -1;
        for (S32 i = 0; i < STAR_TYPE_PRESET_COUNT; ++i)
        {
            if (nearlyEqual(diameter_display, STAR_TYPE_PRESETS[i].mDiameterD)
                && nearlyEqual(body.mMassRelative, STAR_TYPE_PRESETS[i].mMassM))
            {
                match = i;
                break;
            }
        }
        star_type_combo->setSelectedByValue(LLSD(match), true);
    }

    getChild<LLTextureCtrl>("body_custom_texture")->setValue(body.mCustomTexture);
    LLSpinCtrl* padding_spinner = getChild<LLSpinCtrl>("body_disc_padding_spinner");
    if (!padding_spinner->hasFocus())
    {
        padding_spinner->setValue(body.mDiscPadding);
    }
    getChild<LLTextureCtrl>("body_ring_texture")->setValue(body.mRingTexture);

    LLUICtrl* home_check = getChild<LLUICtrl>("body_home_check");
    home_check->setValue(body.mIsHome);
    home_check->setEnabled(!body.mIsLightEmitter);

    LLUICtrl* light_check = getChild<LLUICtrl>("body_light_check");
    light_check->setValue(body.mIsLightEmitter);
    light_check->setEnabled(p->canSetLightEmitter(mSelectedBodyIndex));

    getChild<LLUICtrl>("body_latitude_spinner")->setEnabled(body.mIsHome);

    getChild<LLUICtrl>("body_emissive_check")->setValue(body.mEmissive);
    LLUICtrl* phase_check = getChild<LLUICtrl>("body_phase_check");
    phase_check->setValue(body.mPhaseShaded);
    phase_check->setEnabled(!body.mEmissive);

    getChild<LLUICtrl>("body_ring_check")->setValue(body.mHasRing);
    getChild<LLUICtrl>("body_ring_inner_spinner")->setEnabled(body.mHasRing);
    getChild<LLUICtrl>("body_ring_outer_spinner")->setEnabled(body.mHasRing);
    getChild<LLUICtrl>("body_ring_texture")->setEnabled(body.mHasRing);
}

// List selection into the orbit view and fields.
void SSFloaterAtmoPlanetary::onSelectBody()
{
    LLScrollListItem* item = getChild<LLScrollListCtrl>("body_list")->getFirstSelected();
    if (!item) return;

    const S32 index = item->getValue().asInteger();
    if (index == mSelectedBodyIndex) return;

    flushFocusedPropertyField();
    mSelectedBodyIndex = index;
    refreshAll();
}

// Orbit-view click into the list and fields.
void SSFloaterAtmoPlanetary::onOrbitSelect(S32 index)
{
    if (index == mSelectedBodyIndex) return;

    flushFocusedPropertyField();
    mSelectedBodyIndex = index;
    refreshAll();
}

// A drag moved a phase - refresh the readouts.
void SSFloaterAtmoPlanetary::onOrbitDrag()
{
    refreshBodyFields();
    refreshTitle();
}

// Adds a body of the clicked kind and selects it.
void SSFloaterAtmoPlanetary::onClickAddBody(S32 kind)
{
    flushFocusedPropertyField();

    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;

    S32 preferred_parent = -1;
    if (kind == (S32)SSAtmoEnvCelestialBody::MOON)
    {
        const SSAtmoEnvCelestialBody* selected = selectedBody();
        if (selected)
        {
            if (selected->mKind == SSAtmoEnvCelestialBody::PLANET)
            {
                preferred_parent = mSelectedBodyIndex;
            }
            else if (selected->mKind == SSAtmoEnvCelestialBody::MOON)
            {
                preferred_parent = selected->mParentIndex;
            }
        }
    }

    const S32 index = p->addBody((SSAtmoEnvCelestialBody::EKind)kind, preferred_parent);
    if (index < 0) return;

    mSelectedBodyIndex = index;
    refreshAll();
}

// Removes the selected body.
void SSFloaterAtmoPlanetary::onClickRemoveBody()
{
    flushFocusedPropertyField();

    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;
    if (!p->removeBody(mSelectedBodyIndex)) return;

    mSelectedBodyIndex = 0;
    refreshAll();
}

// Name field into the body.
void SSFloaterAtmoPlanetary::onCommitBodyName()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    std::string name = getChild<LLLineEditor>("body_name_editor")->getText();
    LLStringUtil::trim(name);
    if (!name.empty())
    {
        body->mName = name;
        body->mNameCustom = true;
        p->autoNameBodies();
    }

    refreshAll();
}

// Shading toggle into the body.
void SSFloaterAtmoPlanetary::onCommitBodyShading()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body) return;

    body->mEmissive = getChild<LLUICtrl>("body_emissive_check")->getValue().asBoolean();
    body->mPhaseShaded = getChild<LLUICtrl>("body_phase_check")->getValue().asBoolean();

    refreshBodyFields();
}

// The numeric fields (diameter, orbit, phase, mass...) into the body.
void SSFloaterAtmoPlanetary::onCommitBodyScalars()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    const bool is_sun  = (body->mKind == SSAtmoEnvCelestialBody::SUN);
    const bool is_moon = (body->mKind == SSAtmoEnvCelestialBody::MOON);

    body->mDiameterM = (F32)getChild<LLUICtrl>("body_diameter_spinner")->getValue().asReal()
                     * (is_sun ? SS_METRES_PER_SOLAR_DIAMETER : SS_METRES_PER_KM);
    body->mMassRelative = (F32)getChild<LLUICtrl>("body_mass_spinner")->getValue().asReal();
    bool sun_pair_junior = false;
    if (!is_sun || ss_sun_orbit_editable(*p, mSelectedBodyIndex, sun_pair_junior))
    {
        body->mOrbitalRadius = (F32)getChild<LLUICtrl>("body_orbital_radius_spinner")->getValue().asReal()
                             * (is_moon ? SS_METRES_PER_KM : SS_METRES_PER_AU);
        body->mOrbitalInclinationDeg = (F32)getChild<LLUICtrl>("body_inclination_spinner")->getValue().asReal();
        body->mOrbitalPhaseDeg       = (F32)getChild<LLUICtrl>("body_phase_spinner")->getValue().asReal();
    }
    body->mAxialTiltDeg = (F32)getChild<LLUICtrl>("body_axial_tilt_spinner")->getValue().asReal();
    body->mLatitudeDeg = llclamp(
        (F32)getChild<LLUICtrl>("body_latitude_spinner")->getValue().asReal(), -90.f, 90.f);
    body->mDiscPadding = llclamp(
        (F32)getChild<LLUICtrl>("body_disc_padding_spinner")->getValue().asReal(), 0.f, 0.45f);

    p->autoNameBodies();

    refreshAll();
}

// Star type into the body.
void SSFloaterAtmoPlanetary::onCommitBodyStarType()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body || body->mKind != SSAtmoEnvCelestialBody::SUN) return;

    const S32 preset = getChild<LLComboBox>("body_star_type_combo")->getSelectedValue().asInteger();
    if (preset >= 0 && preset < STAR_TYPE_PRESET_COUNT)
    {
        body->mDiameterM = STAR_TYPE_PRESETS[preset].mDiameterD * SS_METRES_PER_SOLAR_DIAMETER;
        body->mMassRelative = STAR_TYPE_PRESETS[preset].mMassM;
    }

    refreshAll();
}

// Home toggle: moves the observer to this body.
void SSFloaterAtmoPlanetary::onCommitBodyHome()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    if (getChild<LLUICtrl>("body_home_check")->getValue().asBoolean())
    {
        p->setHomeBody(mSelectedBodyIndex);
    }
    else
    {
        body->mIsHome = false;
    }

    refreshAll();
}

// Light-emitter toggle, within the slot limit.
void SSFloaterAtmoPlanetary::onCommitBodyLight()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    const bool want = getChild<LLUICtrl>("body_light_check")->getValue().asBoolean();
    if (want && !p->canSetLightEmitter(mSelectedBodyIndex))
    {
        refreshAll();
        return;
    }
    body->mIsLightEmitter = want;

    refreshAll();
}

// Ring toggle into the body.
void SSFloaterAtmoPlanetary::onCommitBodyRing()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body) return;

    body->mHasRing = getChild<LLUICtrl>("body_ring_check")->getValue().asBoolean();
    body->mRingInnerRadius = (F32)getChild<LLUICtrl>("body_ring_inner_spinner")->getValue().asReal();
    body->mRingOuterRadius = (F32)getChild<LLUICtrl>("body_ring_outer_spinner")->getValue().asReal();
    body->mRingTexture = getChild<LLTextureCtrl>("body_ring_texture")->getValue().asUUID();

    refreshAll();
}

// Texture pick into the body. A newly picked texture gets its disc padding auto-derived
// from the alpha (ssDiscPadAutoDerive; gated on SSAtmoDiscPadAuto) - the derivation writes
// the body's padding and the spinner refresh below shows it, leaving the spinner the
// authority for whatever the author dials after.
void SSFloaterAtmoPlanetary::onCommitBodyTexture()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body) return;

    const LLUUID new_texture = getChild<LLTextureCtrl>("body_custom_texture")->getValue().asUUID();
    if (new_texture != body->mCustomTexture)
    {
        body->mCustomTexture = new_texture;
        ssDiscPadAutoDerive(mTrackIndex, mSelectedBodyIndex, new_texture);
    }

    refreshAll();
}
