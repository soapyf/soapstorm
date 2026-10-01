// Viewport Avatar Toolset - the user's own shortcuts on top of a control preset (Edit > Keyboard Shortcuts...).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Pure helpers, no ImGui context needed (the tests call them): settings.json keeps each changed action as
// "key_overrides": {"action_id": ["Ctrl+Shift+K", ""]}, both slots of the action, "" for none.
#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imgui.h"

namespace vats {

using KeyPair = std::array<ImGuiKeyChord, 2>;                               // an action's key and key2
using KeyOverrides = std::map<std::string, std::array<std::string, 2>>;     // settings.json "key_overrides"

// "Ctrl+Shift+K", "Keypad5", "" for none: ImGui's key names with fixed modifier names, as settings.json keeps them.
std::string chord_name(ImGuiKeyChord chord);
// The reverse, ignoring case; "" gives 0. False when the text names no key.
bool parse_chord(std::string_view text, ImGuiKeyChord& out);

// The keys of an action: its override when it has one whose slots all parse, else the preset's.
KeyPair effective_keys(const KeyOverrides& overrides, const std::string& id, const KeyPair& preset);
// Records keys for an action; an action whose keys equal the preset's loses its override.
void set_keys(KeyOverrides& overrides, const std::string& id, const KeyPair& keys, const KeyPair& preset);

// The action (and its slot) other than self that already uses chord, or {"", -1}.
std::pair<std::string, int> find_conflict(const std::vector<std::pair<std::string, KeyPair>>& bindings,
                                          const std::string& self, ImGuiKeyChord chord);

// True for number pad keys (Keypad0..KeypadEqual): keyboards without one cannot press them.
bool needs_numpad(ImGuiKeyChord chord);
// Keys a shortcut may use: not a modifier on its own, a mouse button or a gamepad control.
bool bindable_key(ImGuiKey key);

}  // namespace vats
