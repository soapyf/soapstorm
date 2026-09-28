// Viewport Avatar Toolset - AO notecards for a project's clips (spec 08 CL-5). Sources: see ao_notecard.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/ao_notecard.h"

#include <algorithm>

namespace vats {
namespace {

// ZHAO-II.lsl's published tokens (its notes list these 23; "Striding", "Soft Landing" and "Taking Off" are internal).
const char* const kZhaoStates[] = {"Standing",      "Walking",        "Sitting",      "Sitting On Ground", "Crouching",
                                   "Crouch Walking", "Landing",        "Standing Up",  "Falling",           "Flying Down",
                                   "Flying Up",      "Flying",         "Flying Slow",  "Hovering",          "Jumping",
                                   "Pre Jumping",    "Running",        "Turning Right", "Turning Left",     "Floating",
                                   "Swimming Forward", "Swimming Up",  "Swimming Down"};
// The states ZHAO-II takes several animations for (multiAnimTokenIndexes).
const char* const kZhaoMulti[] = {"Standing", "Walking", "Sitting", "Sitting On Ground"};
constexpr size_t kZhaoLine = 255;  // llGetNotecardLine returns at most 255 bytes of a line

bool in(const char* const* b, const char* const* e, const std::string& s) {
    return std::find_if(b, e, [&](const char* x) { return s == x; }) != e;
}

}  // namespace

const std::vector<std::string>& ao_states() {
    // Firestorm's AOSet::AOSet stateNames, first names, in order.
    static const std::vector<std::string> s = {
        "Standing",      "Walking",       "Running",      "Sitting",      "Sitting On Ground", "Crouching",
        "Crouch Walking", "Landing",      "Soft Landing", "Standing Up",  "Falling",           "Flying Down",
        "Flying Up",     "Flying",        "Flying Slow",  "Hovering",     "Jumping",           "Pre Jumping",
        "Turning Right", "Turning Left",  "Typing",       "Floating",     "Swimming Forward",  "Swimming Up",
        "Swimming Down", "Always"};
    return s;
}

bool ao_state_known(AoFormat f, const std::string& state) {
    if (f == AoFormat::Zhao) return in(std::begin(kZhaoStates), std::end(kZhaoStates), state);
    return std::find(ao_states().begin(), ao_states().end(), state) != ao_states().end();
}

AoNotecard ao_notecard(AoFormat f, const std::vector<AoClip>& clips) {
    AoNotecard r;
    const std::string fmt = f == AoFormat::Zhao ? "ZHAO-II" : "Firestorm's AO";
    for (const AoClip& c : clips)
        if (!c.state.empty() && !ao_state_known(f, c.state))
            r.warnings.push_back(c.anim + ": " + fmt + " has no state \"" + c.state + "\"; left out");
    for (const std::string& state : ao_states()) {
        if (!ao_state_known(f, state)) continue;
        std::vector<std::string> anims;
        for (const AoClip& c : clips) {
            if (c.state != state) continue;
            if (c.anim.find(',') != std::string::npos || c.anim.find('|') != std::string::npos) {
                r.warnings.push_back(c.anim + ": a name with ',' or '|' reads as several animations; left out");
                continue;
            }
            anims.push_back(c.anim);
        }
        if (anims.empty()) continue;
        const std::string tag = "[ " + state + " ]";
        if (f == AoFormat::Firestorm) {
            std::string line = tag;
            for (size_t k = 0; k < anims.size(); ++k) line += (k ? "|" : "") + anims[k];
            r.text += line + "\n";
            continue;
        }
        if (anims.size() > 1 && !in(std::begin(kZhaoMulti), std::end(kZhaoMulti), state)) {
            for (size_t k = 1; k < anims.size(); ++k)
                r.warnings.push_back(anims[k] + ": ZHAO-II plays one animation for " + state + "; left out");
            anims.resize(1);
        }
        if (anims.size() > 12 && state != "Standing")  // "up to 12 animations each for Walks, Sits and GroundSits"
            r.warnings.push_back(state + ": ZHAO-II takes up to 12 animations here and complains about more");
        std::string line = tag;  // a long list continues on another line with the same token
        for (const std::string& a : anims) {
            if (line.size() > tag.size() && line.size() + 1 + a.size() > kZhaoLine) r.text += line + "\n", line = tag;
            line += (line.size() > tag.size() ? "|" : "") + a;
        }
        r.text += line + "\n";
    }
    return r;
}

}  // namespace vats
