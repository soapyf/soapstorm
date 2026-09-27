/**
 * @file fsanimgraphctrl.cpp
 * @brief VATs Animator graph editor (VATs spec 09, stage 6e).
 *
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 Viewport Avatar Toolset contributors
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

#include "fsanimgraphctrl.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr S32 RULER = 16;   // frame numbers along the top; click or drag there to scrub
    constexpr F32 PICK = 7.f;   // pixels
    // The app's colours: X red, Y green, Z blue; selected keys yellow; handles tan.
    const LLColor4 AXIS[3] = { LLColor4(0.95f, 0.35f, 0.35f, 1.f), LLColor4(0.45f, 0.9f, 0.4f, 1.f),
                               LLColor4(0.4f, 0.6f, 1.f, 1.f) };
    const LLColor4 KEY_SELECTED(1.f, 0.87f, 0.27f, 1.f), HANDLE(0.84f, 0.71f, 0.51f, 1.f),
        HANDLE_FREE(0.43f, 0.84f, 0.92f, 1.f), PLAYHEAD(0.93f, 0.44f, 0.63f, 1.f), KEY_FILL(0.08f, 0.08f, 0.09f, 1.f);

    // A "nice" step (1, 2 or 5 x 10^n) about target_px apart.
    double niceStep(double span, double px, double target_px)
    {
        const double raw = span * target_px / std::max(px, 1.0);
        const double p = std::pow(10.0, std::floor(std::log10(raw)));
        for (double m : { 1.0, 2.0, 5.0 })
            if (p * m >= raw)
                return p * m;
        return p * 10;
    }

    std::string formatValue(double v)
    {
        if (std::fabs(v) >= 10 || v == std::floor(v))
            return llformat("%.0f", v);
        return llformat(std::fabs(v) < 1 ? "%.2f" : "%.1f", v);
    }

    void square(F32 x, F32 y, F32 r, const LLColor4& colour, bool filled)
    {
        gl_rect_2d(S32(x - r), S32(y + r), S32(x + r), S32(y - r), colour, filled);
    }
}

S32 FSAnimGraphCtrl::plotHeight() const { return std::max(getRect().getHeight() - RULER, 1); }
F32 FSAnimGraphCtrl::xOf(double f) const { return F32((f - mT0) / (mT1 - mT0) * getRect().getWidth()); }
F32 FSAnimGraphCtrl::yOf(double v) const { return F32((v - mV0) / (mV1 - mV0) * plotHeight()); }
double FSAnimGraphCtrl::fAt(S32 x) const { return mT0 + double(x) / std::max(getRect().getWidth(), 1) * (mT1 - mT0); }
double FSAnimGraphCtrl::vAt(S32 y) const { return mV0 + double(y) / plotHeight() * (mV1 - mV0); }

const vats::FCurve* FSAnimGraphCtrl::curve(const Channel& c) const
{
    if (!clip)
        return nullptr;
    auto t = clip->curves.find(c.track);
    if (t == clip->curves.end())
        return nullptr;
    auto ch = t->second.find(c.channel);
    return ch == t->second.end() ? nullptr : &ch->second;
}

bool FSAnimGraphCtrl::selected(const Channel& c, int key) const
{
    return std::find(mSel.begin(), mSel.end(), vats::KeyRef{ c.track, c.channel, key }) != mSel.end();
}

void FSAnimGraphCtrl::build()
{
    std::vector<std::string> names = clip && tracks ? tracks() : std::vector<std::string>();
    if (names != mTracks)
    {
        mTracks = names;
        mFitPending = true;
    }
    mChannels.clear();
    for (const std::string& t : mTracks)
    {
        for (int a = 0; a < 3; ++a)
            mChannels.push_back({ t, vats::kRotChannels[a], AXIS[a] });
        if (t == "mPelvis" || clip->has_channels(t, vats::kPosChannels))
            for (int a = 0; a < 3; ++a)
                mChannels.push_back({ t, vats::kPosChannels[a], AXIS[a] * 0.7f + LLColor4(0.3f, 0.3f, 0.3f, 0.3f) });
    }
    // Keys that went away (another edit, a different joint list) leave the selection.
    mSel.erase(std::remove_if(mSel.begin(), mSel.end(),
                              [&](const vats::KeyRef& k)
                              {
                                  for (const Channel& c : mChannels)
                                      if (c.track == k.track && c.channel == k.channel)
                                      {
                                          const vats::FCurve* cv = curve(c);
                                          return !cv || k.index < 0 || k.index >= int(cv->keys.size());
                                      }
                                  return true;
                              }),
               mSel.end());
}

void FSAnimGraphCtrl::fit(double f0, double f1, double v0, double v1)
{
    if (f1 - f0 < 4)
        f0 -= 2, f1 += 2;
    if (v1 - v0 < 2)
        v0 -= 5, v1 += 5;
    const double pf = (f1 - f0) * 0.08, pv = (v1 - v0) * 0.08;
    mT0 = f0 - pf, mT1 = f1 + pf, mV0 = v0 - pv, mV1 = v1 + pv;
}

void FSAnimGraphCtrl::frameAll()
{
    build();
    mFitPending = false;
    double f0 = 1e30, f1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (const Channel& c : mChannels)
        if (const vats::FCurve* cv = curve(c))
            for (const vats::Key& k : cv->keys)
            {
                f0 = std::min({ f0, k.frame, k.lx }), f1 = std::max({ f1, k.frame, k.rx });
                v0 = std::min({ v0, k.value, k.ly, k.ry }), v1 = std::max({ v1, k.value, k.ly, k.ry });
            }
    if (f0 > f1)
        fit(0, clip ? std::max(clip->end_frame, 1) : 30, -45, 45);
    else
        fit(f0, f1, v0, v1);
}

void FSAnimGraphCtrl::frameSelected()
{
    build();
    if (mSel.empty())
        return frameAll();
    double f0 = 1e30, f1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (const vats::KeyRef& s : mSel)
    {
        const vats::Key& k = clip->curves.at(s.track).at(s.channel).keys[s.index];
        f0 = std::min(f0, k.frame), f1 = std::max(f1, k.frame), v0 = std::min(v0, k.value), v1 = std::max(v1, k.value);
    }
    fit(f0, f1, v0, v1);
}

void FSAnimGraphCtrl::applyTangent(vats::Tangent t, const std::string& label)
{
    build();
    if (!clip || mSel.empty() || history->is_open())
        return;
    history->begin(*clip);
    vats::apply_tangent(*clip, mSel, t);
    if (history->commit(label, *clip) && changed)
        changed();
}

void FSAnimGraphCtrl::deleteSelected()
{
    build();
    if (!clip || mSel.empty() || history->is_open())
        return;
    history->begin(*clip);
    vats::delete_keys(*clip, mSel);
    mSel.clear();
    if (history->commit("Delete Keys", *clip) && changed)
        changed();
}

void FSAnimGraphCtrl::clipReplaced(bool refit)
{
    mSel.clear();
    mDrag = Drag::Nothing;  // a drag refers to the old clip; the owner's History was reset with it
    if (hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);
    mFitPending |= refit;
}

int FSAnimGraphCtrl::curveUnder(S32 x, S32 y) const
{
    int best = -1;
    F32 best_d = PICK;
    for (int c = 0; c < int(mChannels.size()); ++c)
        if (const vats::FCurve* cv = curve(mChannels[c]); cv && !cv->empty())
        {
            const F32 d = std::fabs(yOf(cv->evaluate(fAt(x))) - F32(y));
            if (d <= best_d)
                best_d = d, best = c;
        }
    return best;
}

void FSAnimGraphCtrl::draw()
{
    build();
    if (mFitPending && clip)
        frameAll();
    const S32 w = getRect().getWidth(), h = getRect().getHeight(), ph = plotHeight();
    LLLocalClipRect clip_rect(getLocalRect());
    gl_rect_2d(0, h, w, 0, LLColor4(0.f, 0.f, 0.f, 0.45f));
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (!clip || mChannels.empty())
    {
        font->renderUTF8(clip ? "Select joints to see their curves" : "Open or create an animation", 0, w / 2, h / 2,
                         LLColor4(1.f, 1.f, 1.f, 0.5f), LLFontGL::HCENTER, LLFontGL::VCENTER);
        LLUICtrl::draw();
        return;
    }

    // The clip's range, a grid and the ruler.
    gl_rect_2d(S32(xOf(0)), ph, S32(xOf(clip->end_frame)), 0, LLColor4(1.f, 1.f, 1.f, 0.04f));
    gl_rect_2d(0, h, w, ph, LLColor4(0.f, 0.f, 0.f, 0.35f));
    const double ft = std::max(1.0, niceStep(mT1 - mT0, w, 60));
    for (double f = std::ceil(mT0 / ft) * ft; f <= mT1; f += ft)
    {
        const S32 x = S32(xOf(f));
        gl_line_2d(x, ph, x, 0, LLColor4(1.f, 1.f, 1.f, 0.07f));
        font->renderUTF8(llformat("%.0f", f), 0, x + 2, h - 2, LLColor4(1.f, 1.f, 1.f, 0.6f), LLFontGL::LEFT, LLFontGL::TOP);
    }
    const double vt = niceStep(mV1 - mV0, ph, 36);
    for (double v = std::ceil(mV0 / vt) * vt; v <= mV1; v += vt)
    {
        const S32 y = S32(yOf(v));
        gl_line_2d(0, y, w, y, LLColor4(1.f, 1.f, 1.f, std::fabs(v) < vt / 2 ? 0.2f : 0.07f));
        font->renderUTF8(formatValue(std::fabs(v) < vt / 2 ? 0 : v), 0, 3, y + 1, LLColor4(1.f, 1.f, 1.f, 0.45f),
                         LLFontGL::LEFT, LLFontGL::BOTTOM);
    }

    // Curves, sampled every 3 px, holding the end values outside the keys.
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    for (const Channel& c : mChannels)
    {
        const vats::FCurve* cv = curve(c);
        if (!cv || cv->empty())
            continue;
        const bool hot = std::any_of(mSel.begin(), mSel.end(), [&](const vats::KeyRef& k)
                                     { return k.track == c.track && k.channel == c.channel; });
        LLColor4 col = c.colour;
        col.mV[VALPHA] = hot ? 1.f : 0.75f;
        gGL.color4fv(col.mV);
        gGL.begin(LLRender::LINE_STRIP);
        for (S32 x = 0; x <= w + 3 && x < 4000 * 3; x += 3)
            gGL.vertex2f(F32(x), yOf(cv->evaluate(fAt(x))));
        gGL.end();
    }

    // Keys, and the handles of selected keys.
    for (const Channel& c : mChannels)
    {
        const vats::FCurve* cv = curve(c);
        if (!cv)
            continue;
        for (int i = 0; i < int(cv->keys.size()); ++i)
        {
            const vats::Key& k = cv->keys[i];
            if (k.frame < mT0 - 1 || k.frame > mT1 + 1)
                continue;
            const F32 x = xOf(k.frame), y = yOf(k.value);
            const bool sel = selected(c, i);
            if (sel)
            {
                const LLColor4& hc = k.left == vats::Handle::Free || k.right == vats::Handle::Free ? HANDLE_FREE : HANDLE;
                if (i > 0 && cv->keys[i - 1].interp == vats::Interp::Bezier)
                {
                    gl_line_2d(S32(x), S32(y), S32(xOf(k.lx)), S32(yOf(k.ly)), hc);
                    square(xOf(k.lx), yOf(k.ly), 2.5f, hc, true);
                }
                if (i + 1 < int(cv->keys.size()) && k.interp == vats::Interp::Bezier)
                {
                    gl_line_2d(S32(x), S32(y), S32(xOf(k.rx)), S32(yOf(k.ry)), hc);
                    square(xOf(k.rx), yOf(k.ry), 2.5f, hc, true);
                }
                square(x, y, 4.f, KEY_SELECTED, true);
            }
            else
            {
                square(x, y, 3.f, KEY_FILL, true);
                square(x, y, 3.f, c.colour, false);
            }
        }
    }

    if (mDrag == Drag::Box && (std::abs(mX - mPressX) > 3 || std::abs(mY - mPressY) > 3))
    {
        const S32 l = std::min(mX, mPressX), r = std::max(mX, mPressX), b = std::min(mY, mPressY), t = std::max(mY, mPressY);
        gl_rect_2d(l, t, r, b, LLColor4(1.f, 1.f, 1.f, 0.07f));
        gl_rect_2d(l, t, r, b, LLColor4(1.f, 1.f, 1.f, 0.45f), false);
    }

    // Playhead, on whole frames as the timeline shows it.
    if (currentFrame)
    {
        const S32 x = S32(xOf(std::floor(currentFrame() + 1e-9)));
        gl_rect_2d(x - 1, h, x + 1, 0, PLAYHEAD);
    }
    LLUICtrl::draw();
}

bool FSAnimGraphCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    build();
    if (!clip || mDrag != Drag::Nothing)
        return true;
    gFocusMgr.setMouseCapture(this);
    mPressX = mX = x, mPressY = mY = y, mPressMask = mask;
    if (y >= plotHeight())
    {
        mDrag = Drag::Scrub;
        return handleHover(x, y, mask);
    }
    if (history->is_open())  // an edit from elsewhere is in progress
        return true;
    // A handle of a selected key.
    for (const vats::KeyRef& s : mSel)
    {
        const vats::FCurve& cv = clip->curves.at(s.track).at(s.channel);
        const vats::Key& k = cv.keys[s.index];
        const bool has_l = s.index > 0 && cv.keys[s.index - 1].interp == vats::Interp::Bezier;
        const bool has_r = s.index + 1 < int(cv.keys.size()) && k.interp == vats::Interp::Bezier;
        auto at = [&](double f, double v) { return std::hypot(xOf(f) - x, yOf(v) - y) <= PICK; };
        if (has_l && at(k.lx, k.ly))
            mDrag = Drag::Handle, mHandleRight = false;
        else if (has_r && at(k.rx, k.ry))
            mDrag = Drag::Handle, mHandleRight = true;
        if (mDrag == Drag::Handle)
        {
            mHandleKey = s;
            history->begin(*clip);
            return true;
        }
    }
    // The nearest key: Shift toggles it, Ctrl removes it, a plain click selects it alone unless it already is.
    F32 best_d = PICK;
    vats::KeyRef hit;
    bool found = false;
    for (const Channel& c : mChannels)
        if (const vats::FCurve* cv = curve(c))
            for (int i = 0; i < int(cv->keys.size()); ++i)
            {
                const F32 d = std::hypot(xOf(cv->keys[i].frame) - x, yOf(cv->keys[i].value) - y);
                if (d <= best_d)
                    best_d = d, hit = { c.track, c.channel, i }, found = true;
            }
    if (!found)
    {
        mDrag = Drag::Box;
        return true;
    }
    auto it = std::find(mSel.begin(), mSel.end(), hit);
    const bool was = it != mSel.end();
    if (mask & MASK_SHIFT)
        was ? (void)mSel.erase(it) : mSel.push_back(hit);
    else if (mask & MASK_CONTROL)
    {
        if (was)
            mSel.erase(it);
    }
    else if (!was)
        mSel.assign(1, hit);
    if (std::find(mSel.begin(), mSel.end(), hit) != mSel.end())
    {
        mDrag = Drag::Move;  // starts moving after 3 px
        mPressClip = *clip;
        mPressSel = mSel;
    }
    return true;
}

bool FSAnimGraphCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture() || !clip)
        return LLUICtrl::handleHover(x, y, mask);
    mX = x, mY = y;
    const S32 w = std::max(getRect().getWidth(), 1);
    double df = double(x - mPressX) / w * (mT1 - mT0), dv = double(y - mPressY) / plotHeight() * (mV1 - mV0);
    switch (mDrag)
    {
        case Drag::Scrub:
            if (onScrub)
                onScrub(std::clamp(std::round(fAt(x)), 0.0, double(clip->end_frame)));
            break;
        case Drag::Move:
            if (!history->is_open())
            {
                if (std::abs(x - mPressX) <= 3 && std::abs(y - mPressY) <= 3)
                    break;
                history->begin(mPressClip);
            }
            if ((mask & MASK_SHIFT) && std::abs(x - mPressX) > std::abs(y - mPressY))  // lock to the dominant axis
                dv = 0;
            else if (mask & MASK_SHIFT)
                df = 0;
            mSel = mPressSel;
            vats::move_keys(*clip, mPressClip, mSel, df, dv, true);
            break;
        case Drag::Handle:
            vats::drag_handle(clip->curves[mHandleKey.track][mHandleKey.channel], mHandleKey.index, mHandleRight, fAt(x),
                               vAt(y));
            break;
        case Drag::Pan:
            df = double(x - mPressX) / w * (mPressT1 - mPressT0);
            dv = double(y - mPressY) / plotHeight() * (mPressV1 - mPressV0);
            mT0 = mPressT0 - df, mT1 = mPressT1 - df, mV0 = mPressV0 - dv, mV1 = mPressV1 - dv;
            break;
        default:
            break;
    }
    return true;
}

void FSAnimGraphCtrl::finishDrag(bool cancel)
{
    const Drag drag = mDrag;
    mDrag = Drag::Nothing;
    if (hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);
    if (!clip)
        return;
    const bool open = history->is_open();
    if ((drag == Drag::Move || drag == Drag::Handle) && open)
    {
        if (cancel)
        {
            *clip = history->cancel();
            if (drag == Drag::Move)
                mSel = mPressSel;
            return;
        }
        if (drag == Drag::Move)
            vats::finish_transform(*clip, mSel);
        if (history->commit(drag == Drag::Move ? "Move Keys" : "Edit Tangent", *clip) && changed)
            changed();
    }
    else if (drag == Drag::Pan && cancel)
        mT0 = mPressT0, mT1 = mPressT1, mV0 = mPressV0, mV1 = mPressV1;
    else if (drag == Drag::Box && !cancel)
    {
        const bool add = mPressMask & (MASK_SHIFT | MASK_CONTROL);
        if (std::abs(mX - mPressX) <= 3 && std::abs(mY - mPressY) <= 3)
        {
            if (!add)  // a click on empty graph clears the selection
                mSel.clear();
            return;
        }
        const S32 l = std::min(mX, mPressX), r = std::max(mX, mPressX), b = std::min(mY, mPressY), t = std::max(mY, mPressY);
        if (!add)
            mSel.clear();
        for (const Channel& c : mChannels)
            if (const vats::FCurve* cv = curve(c))
                for (int i = 0; i < int(cv->keys.size()); ++i)
                {
                    const F32 x = xOf(cv->keys[i].frame), y = yOf(cv->keys[i].value);
                    if (x < l || x > r || y < b || y > t)
                        continue;
                    const vats::KeyRef k{ c.track, c.channel, i };
                    auto it = std::find(mSel.begin(), mSel.end(), k);
                    if ((mPressMask & MASK_CONTROL) && !(mPressMask & MASK_SHIFT))
                    {
                        if (it != mSel.end())
                            mSel.erase(it);
                    }
                    else if (it == mSel.end())
                        mSel.push_back(k);
                }
    }
}

bool FSAnimGraphCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mDrag != Drag::Pan)
    {
        mX = x, mY = y;
        finishDrag(false);
    }
    return true;
}

bool FSAnimGraphCtrl::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    build();
    if (!clip || y >= plotHeight() || history->is_open())
        return true;
    const int c = curveUnder(x, y);
    if (c < 0)
        return true;
    const Channel ch = mChannels[c];
    history->begin(*clip);
    const int idx = vats::insert_on_curve(clip->curves[ch.track][ch.channel], std::round(fAt(x)));
    mSel.assign(1, vats::KeyRef{ ch.track, ch.channel, idx });
    if (history->commit("Insert Key", *clip) && changed)
        changed();
    return true;
}

bool FSAnimGraphCtrl::handleMiddleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrag != Drag::Nothing)
        return true;
    gFocusMgr.setMouseCapture(this);
    mDrag = Drag::Pan;
    mPressX = x, mPressY = y;
    mPressT0 = mT0, mPressT1 = mT1, mPressV0 = mV0, mPressV1 = mV1;
    return true;
}

bool FSAnimGraphCtrl::handleMiddleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDrag == Drag::Pan)
        finishDrag(false);
    return true;
}

bool FSAnimGraphCtrl::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    // Zoom about the cursor: Shift zooms values only, Ctrl time only (as the app).
    const MASK mask = gKeyboard->currentMask(false);
    const double k = std::pow(1.15, clicks);
    if (!(mask & MASK_SHIFT))
    {
        const double at = fAt(x), span = std::clamp((mT1 - mT0) * k, 0.5, 1e5), r = (at - mT0) / (mT1 - mT0);
        mT0 = at - span * r, mT1 = mT0 + span;
    }
    if (!(mask & MASK_CONTROL))
    {
        const double at = vAt(y), span = std::clamp((mV1 - mV0) * k, 1e-4, 1e7), r = (at - mV0) / (mV1 - mV0);
        mV0 = at - span * r, mV1 = mV0 + span;
    }
    return true;
}

bool FSAnimGraphCtrl::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_ESCAPE && mDrag != Drag::Nothing)
    {
        finishDrag(true);
        return true;
    }
    if ((key == KEY_DELETE || key == KEY_BACKSPACE) && mDrag == Drag::Nothing && !mSel.empty())
    {
        deleteSelected();
        return true;
    }
    return LLUICtrl::handleKeyHere(key, mask);
}

void FSAnimGraphCtrl::onMouseCaptureLost()
{
    if (mDrag != Drag::Nothing)
        finishDrag(true);
}
