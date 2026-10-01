// Viewport Avatar Toolset - AO notecards for a project's clips (spec 08 CL-5).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Both formats are lines of "[ State ]anim1|anim2", one state per line; they differ in the states they know and in
// how many animations a state may have.
//
// Firestorm's AO (Avatar > Movement > Animation Overrider, Import from a notecard in the animations' folder):
// AOEngine::parseNotecard in indra/newview/aoengine.cpp reads each line as "[ State ]Anim1|Anim2|Anim3": the state
// name is trimmed and looked up in AOSet's state names (aoset.cpp, the first name of each "Standing|Stand.1|..."
// entry), animations are split at '|' and trimmed, and each must be an animation of that name in the notecard's
// folder. Lines starting with '#' are skipped. Every state takes any number of animations.
//
// ZHAO-II (and AOs that read its notecards, such as Oracul): ZHAO-II.lsl's notecard notes: "[ Walking ]SexyWalk1|
// SexyWalk2|SexyWalk3", '|' between animations, "Do not add any spaces around animation names", several animations
// only for Standing, Walking, Sitting and Sitting On Ground (the script's multiAnimTokenIndexes), and "You can repeat
// tokens" to spread one state over several lines. Its reader skips lines starting with '#' and gets each line from
// llGetNotecardLine, which returns at most 255 bytes, so long lines are split by repeating the token. In both, a ','
// inside an animation name joins animations played together, so a name with a comma cannot be written.
#pragma once

#include <string>
#include <vector>

namespace vats {

enum class AoFormat { Firestorm, Zhao };

// Every state Firestorm's AO knows, in its order (aoset.cpp). ZHAO-II knows those ao_state_known says.
const std::vector<std::string>& ao_states();
bool ao_state_known(AoFormat f, const std::string& state);

struct AoClip {
    std::string state;  // an ao_states() name; "" = left out
    std::string anim;   // the animation's name in inventory: the export name, without .anim
};

struct AoNotecard {
    std::string text;                   // the notecard, one line per state (ZHAO-II: more for a long line)
    std::vector<std::string> warnings;  // what was left out, and why
};

// The notecard for the clips, states in the format's order, each state's animations in clip order.
AoNotecard ao_notecard(AoFormat f, const std::vector<AoClip>& clips);

}  // namespace vats
