/**
 * @file fsfloaterkillfeed.cpp
 * @brief Kill feed: Combat 2.0 DEATH events shown as a text-only screen
 *        overlay, fed by the LSL bridge combat log listener.
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

#include "fsfloaterkillfeed.h"

#include "fscombathitmarker.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llfloaterreg.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "lggcontactsets.h"
#include "llgroupcolormap.h"
#include "llnetmap.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

#include <boost/json.hpp>

static const std::string KILLFEED_PREFIX = "SSKF|";
static const size_t MAX_ENTRIES = 200;

FSFloaterKillFeed::entries_t FSFloaterKillFeed::sEntries;

FSFloaterKillFeed::FSFloaterKillFeed(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterKillFeed::postBuild()
{
    return true;
}

// static
bool FSFloaterKillFeed::handleChatMessage(const std::string& mesg, const LLUUID& from_id, const LLUUID& owner_id)
{
    if (mesg.compare(0, KILLFEED_PREFIX.size(), KILLFEED_PREFIX) != 0)
    {
        return false;
    }

    // The relay is a worn attachment; owner-say only reaches its owner,
    // but be explicit anyway.
    if (owner_id != gAgent.getID())
    {
        return true;
    }

    boost::system::error_code ec;
    boost::json::value jv = boost::json::parse(mesg.substr(KILLFEED_PREFIX.size()), ec);
    if (!ec && jv.is_object())
    {
        addEntry(LlsdFromJson(jv));
    }
    return true;
}

// static
void FSFloaterKillFeed::handleBridgeEvent(const std::string& json)
{
    boost::system::error_code ec;
    boost::json::value jv = boost::json::parse(json, ec);
    if (!ec && jv.is_object())
    {
        addEntry(LlsdFromJson(jv));
    }
}

// static
void FSFloaterKillFeed::clearEntries()
{
    sEntries.clear();
}

// static
void FSFloaterKillFeed::onAvatarNameResolved(const LLUUID& id, const LLAvatarName& av_name)
{
    // Names are looked up from the cache each frame by the overlay; nothing
    // to do here, the resolved name appears on the next draw.
}

// Apply the FSKillFeedShortNames setting: "Orion Starchild" becomes
// "Orion S.". Used for real names and the settings-panel example alike.
static std::string killfeed_maybe_shorten(std::string name)
{
    static LLCachedControl<bool> short_names(gSavedSettings, "FSKillFeedShortNames", true);
    if (short_names)
    {
        size_t last_space = name.find_last_of(' ');
        if (last_space != std::string::npos && last_space + 1 < name.size())
        {
            name = name.substr(0, name.find_first_of(' ') + 1) + name[last_space + 1] + ".";
        }
    }
    return name;
}

// Avatar display name (shortening setting applied), or a resolving
// placeholder. When allow_request is true a cache miss fires an async
// lookup; the per-frame overlay passes false since lookups are kicked
// once when the entry arrives.
static std::string killfeed_name(const LLUUID& id, bool allow_request = true)
{
    if (id.isNull())
    {
        return "(world)";
    }
    LLAvatarName av_name;
    if (LLAvatarNameCache::get(id, &av_name))
    {
        return killfeed_maybe_shorten(av_name.getDisplayName());
    }
    if (allow_request)
    {
        LLAvatarNameCache::get(id, boost::bind(&FSFloaterKillFeed::onAvatarNameResolved, _1, _2));
    }
    return "(resolving)";
}

// Live world position of the avatar behind a kill-feed id (self, or any rezzed avatar in the
// object list). Used for the killer->victim distance; the combat event's source_pos is the
// killing OBJECT (next to the victim at impact), not the killer avatar.
static bool killfeed_avatar_pos(const LLUUID& id, LLVector3d& out)
{
    if (id.isNull())
    {
        return false;
    }
    if (id == gAgent.getID())
    {
        out = gAgent.getPositionGlobal();
        return true;
    }
    // mKiller can carry the killing OBJECT's UUID (owner-less/deeded kills); a non-avatar
    // match would re-introduce the bogus near-zero distance this function exists to avoid.
    LLViewerObject* obj = gObjectList.findObject(id);
    if (obj && obj->asAvatar())
    {
        out = obj->getPositionGlobal();
        return true;
    }
    return false;
}

// static
void FSFloaterKillFeed::addEntry(const LLSD& event)
{
    if (event["event"].asString() != "DEATH")
    {
        return;
    }

    KillEntry entry;
    entry.mVictim = event["target"].asUUID();
    entry.mKiller = event["owner"].asUUID();
    if (entry.mKiller.isNull())
    {
        entry.mKiller = event["source"].asUUID();
    }
    entry.mWeapon = event["weapon_name"].asString();
    // Damage type cached onto the DEATH by the bridge; -1 (Impact) is a
    // valid type, so the unknown sentinel sits outside the real range.
    entry.mType = event.has("type") ? (S32)event["type"].asInteger() : -1000;
    entry.mDamage = event.has("damage") ? (F32)event["damage"].asReal() : -1.f;

    // Distance is KILLER avatar -> VICTIM avatar, NOT the killing object -> victim. The combat
    // event's source_pos is the projectile/weapon object (right next to the victim at impact, so it
    // read as ~0m); use the two avatars' live world positions instead. If either avatar is not
    // loaded (e.g. a distant killer), leave the distance unset rather than show a bogus one.
    entry.mDistance = -1.f;
    LLVector3d killer_pos, victim_pos;
    if (killfeed_avatar_pos(entry.mKiller, killer_pos) && killfeed_avatar_pos(entry.mVictim, victim_pos))
    {
        entry.mDistance = (F32)(killer_pos - victim_pos).length();
    }

    entry.mReceivedTime = LLFrameTimer::getTotalSeconds();

    // Warm the name cache now so the overlay (which never fires requests)
    // has names by the time it draws.
    killfeed_name(entry.mVictim);
    killfeed_name(entry.mKiller);

    sEntries.push_back(entry);
    while (sEntries.size() > MAX_ENTRIES)
    {
        sEntries.pop_front();
    }
}

struct KFSegment
{
    std::string text;
    LLColor4    color;
};

// Resolve a kill-feed name color. Your own name uses your chosen color when
// FSKillFeedColorOwnName is on (kill feed only); everyone else - and you when
// that toggle is off - follows map-mark > contact-set > group > default.
static LLColor4 killfeed_name_color(const LLUUID& id, const LLColor4& default_color)
{
    if (id.isNull())
    {
        return default_color;
    }

    if (id == gAgent.getID())
    {
        static LLCachedControl<bool> color_own(gSavedSettings, "FSKillFeedColorOwnName", false);
        if (color_own)
        {
            static LLCachedControl<LLColor4> own_color(gSavedSettings, "FSKillFeedOwnNameColor", LLColor4(1.f, 0.9f, 0.4f, 1.f));
            return own_color;
        }
    }

    static LLCachedControl<bool> color_others(gSavedSettings, "FSKillFeedColorOtherNames", true);
    if (!color_others)
    {
        return default_color;
    }

    LLColor4 c;
    if (LLNetMap::getAvatarMarkColor(id, c))
    {
        return c; // map mark
    }
    if (LGGContactSets::instance().hasFriendColorThatShouldShow(id, ContactSetType::MINIMAP, c))
    {
        return c; // contact set
    }
    LLViewerObject* obj = gObjectList.findObject(id);
    LLVOAvatar* av = obj ? obj->asAvatar() : nullptr;
    if (av && av->getActiveGroupID().notNull())
    {
        LLColor4 g = LLGroupColorMap::getInstance()->getGroupColor(av->getActiveGroupID());
        if (g.mV[VW] >= 0.01f)
        {
            return g; // group
        }
    }
    return default_color;
}

// "Killer killed Victim with [symbol] Weapon (100dmg, 50m)" split into colored
// segments so the killer and victim names can be tinted independently of the
// surrounding text (which uses default_color).
static std::vector<KFSegment> killfeed_segments(const FSFloaterKillFeed::KillEntry& entry, const LLColor4& default_color)
{
    std::vector<KFSegment> segs;
    segs.push_back({ killfeed_name(entry.mKiller, false), killfeed_name_color(entry.mKiller, default_color) });
    segs.push_back({ " killed ", default_color });
    segs.push_back({ killfeed_name(entry.mVictim, false), killfeed_name_color(entry.mVictim, default_color) });

    std::string tail;

    // Damage type symbol from the shared hit marker symbol table
    // (customizable in the symbols editor).
    static LLCachedControl<bool> show_emoji(gSavedSettings, "FSKillFeedShowEmoji", true);
    std::string symbol;
    if (show_emoji && entry.mType > -1000)
    {
        symbol = FSCombatHitMarker::getEmojiForType(entry.mType);
    }

    if (!entry.mWeapon.empty() || !symbol.empty())
    {
        tail += " with";
        if (!symbol.empty())
        {
            tail += " " + symbol;
        }
        if (!entry.mWeapon.empty())
        {
            tail += " " + entry.mWeapon;
        }
    }

    std::string stats;
    if (entry.mDamage >= 0.f)
    {
        stats = llformat("%.0fdmg", entry.mDamage);
    }
    if (entry.mDistance >= 0.f)
    {
        if (!stats.empty())
        {
            stats += ", ";
        }
        stats += llformat("%.0fm", entry.mDistance);
    }
    if (!stats.empty())
    {
        tail += " (" + stats + ")";
    }

    if (!tail.empty())
    {
        segs.push_back({ tail, default_color });
    }
    return segs;
}

// static
void FSFloaterKillFeed::drawOverlay()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSKillFeedEnabled", false);

    // The settings panel shows a live example so position, font and color
    // can be customized without waiting for someone to die.
    const bool panel_open = LLFloaterReg::instanceVisible("fs_kill_feed");
    if (!enabled && !panel_open)
    {
        return;
    }

    static LLCachedControl<F32> text_scale(gSavedSettings, "FSKillFeedTextScale", 1.0f);
    static LLCachedControl<F32> screen_x(gSavedSettings, "FSKillFeedScreenX", 0.25f);
    static LLCachedControl<F32> screen_y(gSavedSettings, "FSKillFeedScreenY", 0.9f);
    static LLCachedControl<U32> max_lines(gSavedSettings, "FSKillFeedMaxLines", 5);
    static LLCachedControl<F32> hold_time(gSavedSettings, "FSKillFeedHoldTime", 20.f);
    static LLCachedControl<std::string> font_name(gSavedSettings, "FSKillFeedFontName", "SansSerif");
    static LLCachedControl<LLColor4> text_color(gSavedSettings, "FSKillFeedTextColor", LLColor4::white);
    static LLCachedControl<bool> bold_text(gSavedSettings, "FSKillFeedBoldText", false);
    static LLCachedControl<bool> grow_up(gSavedSettings, "FSKillFeedGrowUp", false);

    const F64 now = LLFrameTimer::getTotalSeconds();
    const LLColor4 default_color = text_color;

    // Collect the visible lines (newest first) as colored segments.
    std::vector<std::vector<KFSegment>> seglines;
    if (enabled)
    {
        for (entries_t::const_reverse_iterator it = sEntries.rbegin();
             it != sEntries.rend() && seglines.size() < (size_t)max_lines;
             ++it)
        {
            // Hold (sec) of 0 means "never fade" -- keep the newest lines on screen
            // forever (capped by Max lines / MAX_ENTRIES), so skip the age cutoff.
            if (hold_time > 0.f && now - it->mReceivedTime > (F64)hold_time)
            {
                break; // entries are time-ordered; everything older follows
            }
            seglines.push_back(killfeed_segments(*it, default_color));
        }
    }

    if (seglines.empty())
    {
        if (!panel_open)
        {
            return;
        }
        // Placeholder preview while the settings panel is open; runs
        // through the same name shortening and symbol option as real lines.
        static LLCachedControl<bool> example_emoji(gSavedSettings, "FSKillFeedShowEmoji", true);
        std::string example_symbol;
        if (example_emoji)
        {
            example_symbol = FSCombatHitMarker::getEmojiForType(5) + " "; // Fire
        }
        // Make the agent the killer so the own-name color shows live as it is
        // tuned; the victim stays a generic example in the default color.
        std::vector<KFSegment> example_segs;
        example_segs.push_back({ killfeed_name(gAgent.getID(), false), killfeed_name_color(gAgent.getID(), default_color) });
        example_segs.push_back({ " killed ", default_color });
        example_segs.push_back({ killfeed_maybe_shorten("Guy Linden"), default_color });
        example_segs.push_back({ " with " + example_symbol + "Death (100dmg, 0m)", default_color });
        seglines.assign((size_t)llmax((U32)1, (U32)max_lines), example_segs);
    }

    const LLFontGL* font = LLFontGL::getFontSansSerif();
    if (font_name() == "SansSerifSmall")
    {
        font = LLFontGL::getFontSansSerifSmall();
    }
    else if (font_name() == "SansSerifBig")
    {
        font = LLFontGL::getFontSansSerifBig();
    }
    else if (font_name() == "SansSerifHuge")
    {
        font = LLFontGL::getFontSansSerifHuge();
    }
    else if (font_name() == "Monospace")
    {
        font = LLFontGL::getFontMonospace();
    }

    const U8 font_style = bold_text ? LLFontGL::BOLD : LLFontGL::NORMAL;

    const S32 view_width = gViewerWindow->getWorldViewRectScaled().getWidth();
    const S32 view_height = gViewerWindow->getWorldViewRectScaled().getHeight();
    const F32 line_height = (F32)(font->getLineHeight() + 2);
    const F32 scale = llclamp((F32)text_scale, 0.5f, 3.f);

    gGL.pushMatrix();
    gGL.translatef((F32)view_width * llclamp((F32)screen_x, 0.f, 1.f),
                   (F32)view_height * llclamp((F32)screen_y, 0.f, 1.f), 0.f);
    gGL.scalef(scale, scale, 1.f);

    // The block ALWAYS occupies max_lines rows stacking down from FSKillFeedScreenX/Y, whichever
    // way it grows, so toggling the direction never moves the feed on screen. Reserving the full
    // height (instead of sizing to the current line count) also keeps the newest line put as the
    // feed fills: grow down puts it in the TOP row, grow up in the BOTTOM row with older lines
    // stacking upward above it. seglines[0] is always the newest; the marker (left gutter, so it
    // never shifts the text) points it out.
    //
    // U+2022 and not a triangle: Ubuntu, Droid, OpenDyslexic, NotoMono and ocra all lack
    // U+25B2/25BC, and the fallback chain those font sets declare only reaches DejaVuSans on
    // Linux -- a triangle renders as tofu for those skins on Windows and Mac. U+2022 is in every
    // bundled font. Direction needs no glyph of its own: the marker sits in the top row growing
    // down and the bottom row growing up.
    const std::string newest_marker = "\xE2\x80\xA2"; // U+2022 BULLET
    const F32 marker_gutter = font->getWidthF32(newest_marker) + 4.f;
    const size_t n_lines = seglines.size();
    const size_t n_rows = llmax((size_t)(U32)max_lines, n_lines); // reserved block height
    for (size_t idx = 0; idx < n_lines; ++idx) // idx 0 = the newest line
    {
        const F32 y = -(F32)(grow_up ? (n_rows - 1 - idx) : idx) * line_height;
        if (idx == 0)
        {
            font->renderUTF8(newest_marker, 0, -marker_gutter, y, default_color,
                             LLFontGL::LEFT, LLFontGL::TOP, font_style, LLFontGL::DROP_SHADOW_SOFT);
        }
        F32 x = 0.f;
        for (const KFSegment& seg : seglines[idx])
        {
            font->renderUTF8(seg.text, 0, x, y, seg.color,
                             LLFontGL::LEFT, LLFontGL::TOP, font_style, LLFontGL::DROP_SHADOW_SOFT);
            x += font->getWidthF32(seg.text);
        }
    }

    gGL.popMatrix();
}
