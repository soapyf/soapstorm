/**
 * @file ssfloaterimportsettings.cpp
 * @brief First-run settings importer dialog and shared settings import helpers
 *
 * Copyright (c) 2026, SoapStorm Project
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloaterimportsettings.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "llbutton.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "llappviewer.h"
#include "llfeaturemanager.h"
#include "llfloaterpreference.h"
#include "llviewercontrol.h"

namespace
{
    std::string find_settings_xml_in_dir(const std::string& dir)
    {
        if (dir.empty() || !LLFile::isdir(dir))
        {
            return {};
        }

        std::string nested = gDirUtilp->add(gDirUtilp->add(dir, "user_settings"), "settings.xml");
        if (gDirUtilp->fileExists(nested))
        {
            return nested;
        }

        std::string flat = gDirUtilp->add(dir, "settings.xml");
        if (gDirUtilp->fileExists(flat))
        {
            return flat;
        }

        return {};
    }

    std::time_t get_file_mtime(const std::string& filepath)
    {
        llstat st{};
        if (LLFile::stat(filepath, &st) == 0)
        {
            return st.st_mtime;
        }
        return 0;
    }

    std::string format_mtime(std::time_t mtime)
    {
        if (mtime <= 0)
        {
            return "Unknown";
        }
        std::tm local_tm{};
#if LL_WINDOWS
        if (localtime_s(&local_tm, &mtime) != 0)
        {
            return "Unknown";
        }
#else
        if (!localtime_r(&mtime, &local_tm))
        {
            return "Unknown";
        }
#endif
        char buf[32]{};
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local_tm) == 0)
        {
            return "Unknown";
        }
        return std::string(buf);
    }

    bool paths_equivalent(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        std::error_code ec;
        if (std::filesystem::equivalent(a, b, ec))
        {
            return true;
        }
        return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
    }

    void on_import_quit_confirmed(const LLSD& /*notification*/, const LLSD& /*response*/)
    {
        // Keep copied settings intact by skipping shutdown save.
        LLAppViewer::instance()->setSaveSettingsOnExit(false);
        LL_INFOS("SettingsImport") << "Requesting quit to apply imported settings" << LL_ENDL;
        LLAppViewer::instance()->requestQuit();
    }

    void on_browse_dir_picked(LLHandle<SSFloaterImportSettings> handle,
                              const std::vector<std::string>& filenames,
                              std::string proposed_name)
    {
        if (filenames.empty())
        {
            return;
        }
        const std::string& dir_name = filenames[0];
        if (dir_name.empty() || dir_name == proposed_name)
        {
            return;
        }
        if (!ss_has_valid_settings_dir(dir_name))
        {
            LLNotificationsUtil::add("ImportSettingsNotFound");
            return;
        }

        if (SSFloaterImportSettings* self = handle.get())
        {
            self->addAndSelectSource(dir_name);
        }
    }
}

bool ss_has_valid_settings_dir(const std::string& dir)
{
    return !find_settings_xml_in_dir(dir).empty();
}

std::vector<SSImportSettingsSource> ss_find_settings_import_sources(bool include_soapstorm)
{
    struct CandidateSpec
    {
        const char* label;
        const char* folder;
    };

    std::vector<CandidateSpec> specs;
    if (include_soapstorm)
    {
        specs.push_back({ "Soapstorm (64-bit)",         "Soapstorm_x64" });
        specs.push_back({ "Soapstorm OpenSim (64-bit)", "SoapstormOS_x64" });
        specs.push_back({ "Soapstorm",                  "Soapstorm" });
        specs.push_back({ "Soapstorm OpenSim",          "SoapstormOS" });
        specs.push_back({ "Soapstorm (64-bit)",         ".soapstorm_x64" });
        specs.push_back({ "Soapstorm OpenSim (64-bit)", ".soapstormos_x64" });
        specs.push_back({ "Soapstorm",                  ".soapstorm" });
    }
    specs.push_back({ "Firestorm (64-bit)",         "Firestorm_x64" });
    specs.push_back({ "Firestorm OpenSim (64-bit)", "FirestormOS_x64" });
    specs.push_back({ "Firestorm",                  "Firestorm" });
    specs.push_back({ "Firestorm OpenSim",          "FirestormOS" });
    specs.push_back({ "Firestorm (64-bit)",         ".firestorm_x64" });
    specs.push_back({ "Firestorm OpenSim (64-bit)", ".firestormos_x64" });
    specs.push_back({ "Firestorm",                  ".firestorm" });

    const std::string base = gDirUtilp->getOSUserDir();
    const std::filesystem::path self_dir(gDirUtilp->getOSUserAppDir());

    std::vector<std::filesystem::path> seen_paths;
    std::vector<SSImportSettingsSource> results;

    for (const CandidateSpec& spec : specs)
    {
        const std::string candidate = gDirUtilp->add(base, spec.folder);
        const std::string settings_xml = find_settings_xml_in_dir(candidate);
        if (settings_xml.empty())
        {
            continue;
        }

        const std::filesystem::path cand_path(candidate);
        if (paths_equivalent(cand_path, self_dir))
        {
            continue;
        }

        bool duplicate = false;
        for (const std::filesystem::path& prev : seen_paths)
        {
            if (paths_equivalent(cand_path, prev))
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        seen_paths.push_back(cand_path);

        SSImportSettingsSource src;
        src.mLabel = spec.label;
        src.mPath = candidate;
        src.mModifiedTime = get_file_mtime(settings_xml);
        src.mModifiedText = format_mtime(src.mModifiedTime);
        results.push_back(std::move(src));
    }

    std::stable_sort(results.begin(), results.end(),
        [](const SSImportSettingsSource& a, const SSImportSettingsSource& b)
        {
            return a.mModifiedTime > b.mModifiedTime;
        });

    return results;
}

bool ss_import_settings_from_dir(const std::string& src_dir)
{
    if (src_dir.empty() || !LLFile::isdir(src_dir) || !ss_has_valid_settings_dir(src_dir))
    {
        LLNotificationsUtil::add("BackupPathDoesNotExist");
        return false;
    }

    // Never overwrite active settings from self.
    if (paths_equivalent(std::filesystem::path(src_dir), std::filesystem::path(gDirUtilp->getOSUserAppDir())))
    {
        LLNotificationsUtil::add("BackupPathDoesNotExist");
        return false;
    }

    if (LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
    {
        instance->onBtnOK(LLSD());
    }

    std::string src_user_settings = gDirUtilp->add(src_dir, "user_settings");
    if (!LLFile::isdir(src_user_settings))
    {
        if (gDirUtilp->fileExists(gDirUtilp->add(src_dir, "settings.xml")))
        {
            src_user_settings = src_dir;
        }
    }

    std::string dst_user_settings = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "");
    LLFile::mkdir(dst_user_settings);

    // 1. Import global settings.xml
    std::string src_global = gDirUtilp->add(src_user_settings, "settings.xml");
    if (gDirUtilp->fileExists(src_global))
    {
        std::string global_name = gSavedSettings.getString("ClientSettingsFile");
        LL_INFOS("SettingsImport") << "Importing global settings from " << src_global << LL_ENDL;
        gSavedSettings.resetToDefaults();
        LLFeatureManager::getInstance()->applyRecommendedSettings();
        gSavedSettings.loadFromFile(src_global);
        // Ensure imported settings never leave FirstRunThisInstall true on disk.
        gSavedSettings.setBOOL("FirstRunThisInstall", false);
        gSavedSettings.saveToFile(global_name, true);
    }

    std::error_code ec;

    // 2. Copy user_settings XML files and presets
    if (LLFile::isdir(src_user_settings))
    {
        for (const auto& entry : std::filesystem::directory_iterator(src_user_settings, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".xml")
            {
                std::string filename = entry.path().filename().string();
                if (filename == "settings.xml")
                {
                    continue;
                }
                auto dst_file = std::filesystem::path(dst_user_settings) / filename;
                std::filesystem::copy_file(entry.path(), dst_file,
                    std::filesystem::copy_options::overwrite_existing, ec);
            }
        }

        std::string src_presets = gDirUtilp->add(src_user_settings, "presets");
        std::string dst_presets = gDirUtilp->add(dst_user_settings, "presets");
        if (std::filesystem::exists(src_presets))
        {
            std::filesystem::create_directories(dst_presets, ec);
            std::filesystem::copy(src_presets, dst_presets,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                LL_WARNS("SettingsImport") << "Failed to copy presets: " << ec.message() << LL_ENDL;
            }
        }
    }

    // 3. Copy per-account directories (directories containing settings_per_account.xml)
    for (const auto& entry : std::filesystem::directory_iterator(src_dir, ec))
    {
        if (entry.is_directory())
        {
            std::string dirname = entry.path().filename().string();
            if (dirname != "user_settings" && dirname != "logs" && dirname != "browser_profile" &&
                LLFile::isdir(entry.path().string()))
            {
                auto acct_xml = entry.path() / "settings_per_account.xml";
                if (std::filesystem::exists(acct_xml))
                {
                    auto dst_acct = std::filesystem::path(gDirUtilp->getOSUserAppDir()) / dirname;
                    std::filesystem::create_directories(dst_acct, ec);
                    std::filesystem::copy(entry.path(), dst_acct,
                        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec)
                    {
                        LL_WARNS("SettingsImport") << "Failed to copy account dir " << dirname << ": " << ec.message() << LL_ENDL;
                    }
                }
            }
        }
    }

    // 4. If logged in, reload current per-account settings
    std::string per_account_name = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT,
        LLAppViewer::instance()->getSettingsFilename("Default", "PerAccount"));
    if (!per_account_name.empty() && gDirUtilp->fileExists(per_account_name))
    {
        gSavedPerAccountSettings.loadFromFile(per_account_name);
        gSavedPerAccountSettings.saveToFile(per_account_name, true);
    }

    gSavedSettings.setBOOL("FSFirstRunAfterSettingsRestore", true);

    LLNotificationsUtil::add("RestoreFinished", LLSD(), LLSD(), on_import_quit_confirmed);
    return true;
}

SSFloaterImportSettings::SSFloaterImportSettings(const LLSD& key)
    : LLFloater(key)
{
}

bool SSFloaterImportSettings::postBuild()
{
    mSourcesList = getChild<LLScrollListCtrl>("sources_list");
    mSourcesList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtonState(); });
    mSourcesList->setDoubleClickCallback([this]() { onClickImport(); });

    mImportBtn = getChild<LLButton>("import_btn");
    mImportBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClickImport(); });

    mSkipBtn = getChild<LLButton>("skip_btn");
    mSkipBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClickSkip(); });

    getChild<LLButton>("browse_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClickBrowse(); });

    return LLFloater::postBuild();
}

void SSFloaterImportSettings::onOpen(const LLSD& key)
{
    mIsManual = key.isMap() && key["manual"].asBoolean();

    if (LLTextBox* desc = findChild<LLTextBox>("import_desc"))
    {
        desc->setValue(getString(mIsManual ? "manual_desc" : "first_run_desc"));
    }
    if (mSkipBtn)
    {
        mSkipBtn->setLabel(getString(mIsManual ? "cancel_btn_label" : "skip_btn_label"));
    }

    populateSources(ss_find_settings_import_sources(true));
}

// static
void SSFloaterImportSettings::showIfNeeded()
{
    if (!LLAppViewer::instance()->isFirstRun())
    {
        return;
    }

    if (ss_find_settings_import_sources(true).empty())
    {
        return;
    }

    LLFloaterReg::showInstance("import_settings");
}

// static
void SSFloaterImportSettings::showPicker()
{
    LLFloaterReg::showInstance("import_settings", LLSD().with("manual", true));
}

void SSFloaterImportSettings::populateSources(const std::vector<SSImportSettingsSource>& sources)
{
    if (!mSourcesList)
    {
        return;
    }

    mSourcesList->deleteAllItems();
    if (sources.empty())
    {
        mSourcesList->setCommentText(getString("no_sources_text"));
        updateButtonState();
        return;
    }

    for (const SSImportSettingsSource& src : sources)
    {
        LLSD row;
        row["value"] = src.mPath;
        row["columns"][0]["column"] = "source";
        row["columns"][0]["value"] = src.mLabel;
        row["columns"][1]["column"] = "modified";
        row["columns"][1]["value"] = src.mModifiedText;
        row["columns"][2]["column"] = "path";
        row["columns"][2]["value"] = src.mPath;
        mSourcesList->addElement(row);
    }

    mSourcesList->selectFirstItem();
    updateButtonState();
}

void SSFloaterImportSettings::addAndSelectSource(const std::string& dir_path)
{
    if (!mSourcesList)
    {
        return;
    }

    std::string settings_xml = find_settings_xml_in_dir(dir_path);
    if (settings_xml.empty())
    {
        return;
    }

    const std::filesystem::path target_path(dir_path);
    for (LLScrollListItem* item : mSourcesList->getAllData())
    {
        if (paths_equivalent(std::filesystem::path(item->getValue().asString()), target_path))
        {
            mSourcesList->selectByValue(item->getValue());
            updateButtonState();
            return;
        }
    }

    LLSD row;
    row["value"] = dir_path;
    row["columns"][0]["column"] = "source";
    row["columns"][0]["value"] = getString("custom_source_label");
    row["columns"][1]["column"] = "modified";
    row["columns"][1]["value"] = format_mtime(get_file_mtime(settings_xml));
    row["columns"][2]["column"] = "path";
    row["columns"][2]["value"] = dir_path;
    mSourcesList->addElement(row, ADD_TOP);
    mSourcesList->selectByValue(LLSD(dir_path));
    updateButtonState();
}

void SSFloaterImportSettings::updateButtonState()
{
    if (mImportBtn && mSourcesList)
    {
        mImportBtn->setEnabled(!mSourcesList->getSelectedValue().asString().empty());
    }
}

void SSFloaterImportSettings::onClickImport()
{
    if (!mSourcesList)
    {
        return;
    }

    std::string selected = mSourcesList->getSelectedValue().asString();
    if (selected.empty())
    {
        return;
    }

    if (mIsManual)
    {
        LLSD args;
        args["DIRECTORY"] = selected;
        LLHandle<SSFloaterImportSettings> handle = getDerivedHandle<SSFloaterImportSettings>();
        LLNotificationsUtil::add("ImportSettingsConfirm", args, LLSD(),
            [handle, selected](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                {
                    return;
                }
                if (ss_import_settings_from_dir(selected))
                {
                    if (SSFloaterImportSettings* self = handle.get())
                    {
                        self->closeFloater();
                    }
                }
            });
        return;
    }

    if (ss_import_settings_from_dir(selected))
    {
        closeFloater();
    }
}

void SSFloaterImportSettings::onClickBrowse()
{
    LLHandle<SSFloaterImportSettings> handle = getDerivedHandle<SSFloaterImportSettings>();
    (new LLDirPickerThread(boost::bind(&on_browse_dir_picked, handle, _1, _2), gDirUtilp->getOSUserDir()))->getFile();
}

void SSFloaterImportSettings::onClickSkip()
{
    closeFloater();
}
