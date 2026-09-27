/**
 * @file fsfloatervatstools.cpp
 * @brief VATs Tools: IK, Hold/Bind pins, hand and body poses (VATs spec 09 stage 6f), working on the
 *        VATs Animator's open clip through its undo history.
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

#include "fsfloatervatstools.h"

#include "fsfloateranimator.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llvoavatarself.h"

#include "vats/pose_ops.h"
#include "vats/pose_presets.h"
#include "vats/rig.h"

#include <algorithm>

FSFloaterVATsTools::FSFloaterVATsTools(const LLSD& key) : LLFloater(key) {}

bool FSFloaterVATsTools::postBuild()
{
    auto button = [this](const char* name, std::function<void()> f)
    { getChild<LLUICtrl>(name)->setCommitCallback([f](LLUICtrl*, const LLSD&) { f(); }); };
    button("ik_on_btn", [this] { onSwitchIk(true); });
    button("ik_off_btn", [this] { onSwitchIk(false); });
    button("ik_key_btn", [this] { onKeyIkFromAvatar(); });
    button("limb_combo", [this] { refreshLimb(); });
    button("hold_btn", [this] { onHold(); });
    button("bind_btn", [this] { onBind(); });
    button("release_btn", [this] { onRelease(); });
    button("delete_pin_btn", [this] { onDeletePin(); });
    button("apply_pose_btn", [this] { onApplyPose(); });
    return true;
}

void FSFloaterVATsTools::onOpen(const LLSD& key)
{
    animator(false);
    refreshLimb();
}

FSFloaterAnimator* FSFloaterVATsTools::animator(bool need_clip)
{
    FSFloaterAnimator* a = LLFloaterReg::findTypedInstance<FSFloaterAnimator>("fs_animator");
    if (!a || !a->ensureSkeleton())
    {
        setStatus("Open the VATs Animator first.");
        return nullptr;
    }
    if (!mFilled)
        fillLists(a);
    if (need_clip && !a->hasClip())
    {
        setStatus("Open or create an animation in the VATs Animator first.");
        return nullptr;
    }
    return a;
}

void FSFloaterVATsTools::fillLists(FSFloaterAnimator* a)
{
    const vats::Skeleton& skel = a->skeleton();
    LLComboBox* limbs = getChild<LLComboBox>("limb_combo");
    for (size_t l = 0; l < a->rig()->limbs().size(); ++l)
        limbs->add(a->rig()->limbs()[l].label, LLSD(S32(l)));
    limbs->selectFirstItem();
    LLComboBox* bind = getChild<LLComboBox>("bind_combo");
    for (int i = 0; i < skel.volume_start(); ++i)
        if (isAgentAvatarValid() && gAgentAvatarp->getJoint(skel[i].name))
            bind->add(skel[i].name, LLSD(i));
    bind->selectFirstItem();
    // Starter poses: hand shapes (left-hand items; Mirrored puts them on the right) and body poses.
    LLComboBox* poses = getChild<LLComboBox>("pose_combo");
    const std::vector<vats::LibraryItem>& items = vats::builtin_poses(skel);
    for (size_t i = 0; i < items.size(); ++i)
        poses->add((items[i].kind == "hand" ? "Hand: " : "Body: ") + items[i].name, LLSD(S32(i)));
    poses->selectFirstItem();
    mFilled = true;
}

void FSFloaterVATsTools::refreshLimb()
{
    FSFloaterAnimator* a = LLFloaterReg::findTypedInstance<FSFloaterAnimator>("fs_animator");
    std::string state = "-";
    if (a && a->hasClip() && a->rig())
    {
        const int l = getChild<LLComboBox>("limb_combo")->getValue().asInteger();
        const vats::Evaluation e = vats::evaluate(*a->rig(), a->clip(), a->currentFrame(), nullptr);
        if (l >= 0 && l < int(e.limbs.size()))
            state = llformat("%s at frame %d", e.limbs[l].ik_on ? "IK" : "FK", a->currentFrame());
    }
    getChild<LLUICtrl>("limb_state_text")->setTextArg("[STATE]", state);
}

void FSFloaterVATsTools::onSwitchIk(bool to_ik)
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const int l = getChild<LLComboBox>("limb_combo")->getValue().asInteger();
    const int f = a->currentFrame();
    const vats::Rig& rig = *a->rig();
    a->mergeTake(to_ik ? "Switch to IK" : "Switch to FK", [&](vats::Clip& c)
    {
        if (to_ik)
            vats::switch_to_ik(c, rig, f, l, nullptr);
        else
            vats::switch_to_fk(c, rig, f, l, nullptr);
    });
    refreshLimb();
    setStatus(llformat("%s is now %s from frame %d", rig.limbs()[l].label.c_str(), to_ik ? "IK" : "FK", f));
}

// IK targets and poles from the pose the avatar shows: place the hands and feet with the Firestorm Poser's
// gizmos (as for FK keys), then key every limb that is in IK at this frame to reach them.
// ponytail: evaluated without the avatar's shape, like the preview (spec 09 §5b); a very different shape
// can leave a few cm between the posed and the keyed end.
void FSFloaterVATsTools::onKeyIkFromAvatar()
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const vats::Rig& rig = *a->rig();
    const int f = a->currentFrame();
    const std::vector<vats::Xform> globals = a->skeleton().global_pose(a->shownPose());
    const vats::Evaluation e = vats::evaluate(rig, a->clip(), f, nullptr);
    int keyed = 0;
    a->mergeTake("Key IK from Avatar", [&](vats::Clip& c)
    {
        for (size_t l = 0; l < rig.limbs().size(); ++l)
        {
            if (!e.limbs[l].ik_on)
                continue;
            const vats::LimbInfo& limb = rig.limbs()[l];
            vats::key_limb_target(c, rig, f, int(l), globals[limb.end], nullptr);
            if (!limb.spine)
                vats::key_limb_pole(c, rig, f, int(l), vats::derive_pole(rig, int(l), globals), nullptr);
            ++keyed;
        }
    });
    setStatus(keyed ? llformat("Keyed the IK targets of %d limb(s) at frame %d", keyed, f)
                    : std::string("No limb is in IK at this frame. Pick a limb and Switch to IK first."));
}

void FSFloaterVATsTools::onHold()
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const std::vector<int> nodes = a->selectedNodes();
    if (nodes.empty())
        return setStatus("Select the joints to hold in the Animator's joint list.");
    const int f = a->currentFrame();
    std::string why, fail;
    a->mergeTake("Hold in World", [&](vats::Clip& c)
    {
        for (int n : nodes)
            if (!vats::pin_here(c, *a->rig(), f, n, -1, nullptr, why))
                fail = why;
    });
    setStatus(fail.empty() ? llformat("Holding %d joint(s) in place from frame %d", int(nodes.size()), f) : fail);
}

void FSFloaterVATsTools::onBind()
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const std::vector<int> nodes = a->selectedNodes();
    const int target = getChild<LLComboBox>("bind_combo")->getValue().asInteger();
    if (nodes.empty())
        return setStatus("Select the joints to bind in the Animator's joint list, then pick what they follow.");
    const int f = a->currentFrame();
    std::string why, fail;
    a->mergeTake("Bind to Bone", [&](vats::Clip& c)
    {
        for (int n : nodes)
            if (n != target && !vats::pin_here(c, *a->rig(), f, n, target, nullptr, why))
                fail = why;
    });
    setStatus(fail.empty() ? llformat("Bound %d joint(s) to %s from frame %d", int(nodes.size()),
                                      a->skeleton()[target].name.c_str(), f)
                           : fail);
}

void FSFloaterVATsTools::onRelease()
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const std::vector<int> nodes = a->selectedNodes();
    const int f = a->currentFrame();
    std::string why, fail;
    a->mergeTake("Release", [&](vats::Clip& c)
    {
        for (int n : nodes)
            if (!vats::unpin_here(c, *a->rig(), f, n, nullptr, why))
                fail = why;
    });
    setStatus(fail.empty() ? llformat("Released from frame %d", f) : fail);
}

void FSFloaterVATsTools::onDeletePin()
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const int f = a->currentFrame();
    std::vector<int> pins;
    for (int n : a->selectedNodes())
        if (int p = vats::pin_at(a->clip(), *a->rig(), n, f); p >= 0)
            pins.push_back(p);
    if (pins.empty())
        return setStatus("No selected joint is held or bound at this frame.");
    std::sort(pins.rbegin(), pins.rend());  // highest first, so earlier indices stay valid
    pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
    a->mergeTake("Delete Pin", [&](vats::Clip& c) { for (int p : pins) vats::delete_pin(c, *a->rig(), size_t(p)); });
    setStatus(llformat("Deleted %d pin(s)", int(pins.size())));
}

void FSFloaterVATsTools::onApplyPose()
{
    FSFloaterAnimator* a = animator();
    if (!a)
        return;
    const std::vector<vats::LibraryItem>& items = vats::builtin_poses(a->skeleton());
    const int i = getChild<LLComboBox>("pose_combo")->getValue().asInteger();
    if (i < 0 || i >= int(items.size()))
        return;
    const bool mirrored = getChild<LLUICtrl>("mirror_check")->getValue().asBoolean();
    const int f = a->currentFrame();
    a->mergeTake("Apply " + items[i].name, [&](vats::Clip& c) { vats::apply_pose(c, a->skeleton(), items[i], f, mirrored); });
    setStatus(llformat("Applied %s at frame %d", items[i].name.c_str(), f));
}

void FSFloaterVATsTools::setStatus(const std::string& text)
{
    getChild<LLUICtrl>("status_text")->setValue(text);
}
