// Viewport Avatar Toolset - the user's own shortcuts on top of a control preset.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "keymap.h"

#include <cctype>

namespace vats {
namespace {

constexpr std::pair<ImGuiKeyChord, const char*> kMods[] = {
    {ImGuiMod_Ctrl, "Ctrl"}, {ImGuiMod_Shift, "Shift"}, {ImGuiMod_Alt, "Alt"}, {ImGuiMod_Super, "Super"}};

bool same_text(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

std::string chord_name(ImGuiKeyChord chord) {
    const ImGuiKey key = ImGuiKey(chord & ~ImGuiMod_Mask_);
    if (key == ImGuiKey_None) return "";
    std::string s;
    for (auto& [mod, name] : kMods)
        if (chord & mod) s += std::string(name) + "+";
    return s + ImGui::GetKeyName(key);
}

bool parse_chord(std::string_view text, ImGuiKeyChord& out) {
    out = 0;
    if (text.empty()) return true;
    ImGuiKeyChord mods = 0;
    for (size_t plus; (plus = text.find('+')) != std::string_view::npos && plus + 1 < text.size();) {
        const std::string_view part = text.substr(0, plus);
        bool found = false;
        for (auto& [mod, name] : kMods)
            if (same_text(part, name)) mods |= mod, found = true;
        if (!found) return false;
        text.remove_prefix(plus + 1);
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (bindable_key(ImGuiKey(k)) && same_text(text, ImGui::GetKeyName(ImGuiKey(k)))) return out = mods | k, true;
    return false;
}

KeyPair effective_keys(const KeyOverrides& overrides, const std::string& id, const KeyPair& preset) {
    auto it = overrides.find(id);
    if (it == overrides.end()) return preset;
    KeyPair k;
    for (int s = 0; s < 2; ++s)
        if (!parse_chord(it->second[s], k[s])) return preset;  // a hand-edited typo keeps the preset's keys
    if (!k[0] || k[0] == k[1]) k = {k[0] ? k[0] : k[1], 0};  // the first slot is the one the menus show
    return k;
}

void set_keys(KeyOverrides& overrides, const std::string& id, const KeyPair& keys, const KeyPair& preset) {
    KeyPair k = keys;
    if (!k[0] || k[0] == k[1]) k = {k[0] ? k[0] : k[1], 0};  // the menus show the first slot; no key twice
    if (k == preset)
        overrides.erase(id);
    else
        overrides[id] = {chord_name(k[0]), chord_name(k[1])};
}

std::pair<std::string, int> find_conflict(const std::vector<std::pair<std::string, KeyPair>>& bindings,
                                          const std::string& self, ImGuiKeyChord chord) {
    if (!chord) return {"", -1};
    for (auto& [id, keys] : bindings)
        for (int s = 0; s < 2; ++s)
            if (id != self && keys[s] == chord) return {id, s};
    return {"", -1};
}

bool needs_numpad(ImGuiKeyChord chord) {
    const int k = chord & ~ImGuiMod_Mask_;
    return k >= ImGuiKey_Keypad0 && k <= ImGuiKey_KeypadEqual;
}

bool bindable_key(ImGuiKey key) {
    if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_NamedKey_END) return false;
    if (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) return false;
    return key < ImGuiKey_GamepadStart;  // then the gamepad, the mouse and the modifier flags' keys
}

}  // namespace vats
