/**
 * @file sspanelprefs.cpp
 * @brief See sspanelprefs.h.
 *
 * $LicenseInfo:firstyear=2024&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sspanelprefs.h"

static LLPanelInjector<SSPanelPrefs> t_pref_ss("panel_preference_soapstorm");

// Pure pass-through panel so the XML can inject a Soapstorm preferences tab.
SSPanelPrefs::SSPanelPrefs() : LLPanelPreference()
{
}

// Stock behaviour; hook point for future Soapstorm prefs.
bool SSPanelPrefs::postBuild()
{
    return LLPanelPreference::postBuild();
}

// Stock behaviour; hook point for future Soapstorm prefs.
void SSPanelPrefs::apply()
{
    LLPanelPreference::apply();
}

// Stock behaviour; hook point for future Soapstorm prefs.
void SSPanelPrefs::cancel(const std::vector<std::string> settings_to_skip)
{
    LLPanelPreference::cancel(settings_to_skip);
}
