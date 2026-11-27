/**
 * @file ssfloaterimportsettings.h
 * @brief First-run settings importer dialog and shared settings import helpers
 *
 * Copyright (c) 2026, SoapStorm Project
 */

#ifndef SS_FLOATER_IMPORT_SETTINGS_H
#define SS_FLOATER_IMPORT_SETTINGS_H

#include "llfloater.h"

#include <ctime>
#include <string>
#include <vector>

class LLButton;
class LLScrollListCtrl;

struct SSImportSettingsSource
{
    std::string mLabel;
    std::string mPath;
    std::string mModifiedText;
    std::time_t mModifiedTime{ 0 };
};

bool ss_has_valid_settings_dir(const std::string& dir);
std::vector<SSImportSettingsSource> ss_find_settings_import_sources(bool include_soapstorm = true);
bool ss_import_settings_from_dir(const std::string& src_dir);

class SSFloaterImportSettings : public LLFloater
{
public:
    SSFloaterImportSettings(const LLSD& key);
    ~SSFloaterImportSettings() override = default;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    static void showIfNeeded();
    static void showPicker();

    void addAndSelectSource(const std::string& dir_path);

private:
    void populateSources(const std::vector<SSImportSettingsSource>& sources);
    void updateButtonState();
    void onClickImport();
    void onClickBrowse();
    void onClickSkip();

    LLScrollListCtrl* mSourcesList{ nullptr };
    LLButton*         mImportBtn{ nullptr };
    LLButton*         mSkipBtn{ nullptr };
    bool              mIsManual{ false };
};

#endif // SS_FLOATER_IMPORT_SETTINGS_H
