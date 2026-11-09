/**
 * @file llfloaterupdateprogress.h
 * @brief Progress dialog for SoapStorm auto-update downloads
 *
 * Copyright (c) 2026, SoapStorm Project
 */

#ifndef LL_LLFLOATERUPDATEPROGRESS_H
#define LL_LLFLOATERUPDATEPROGRESS_H

#include "llfloater.h"
#include "llframetimer.h"

class LLProgressBar;
class LLTextBox;

class LLFloaterUpdateProgress : public LLFloater
{
public:
    LLFloaterUpdateProgress(const LLSD& key);
    virtual ~LLFloaterUpdateProgress();

    /*virtual*/ bool postBuild();
    /*virtual*/ void onOpen(const LLSD& key);
    /*virtual*/ void draw();

    // Static convenience methods for update flows
    static void show(const std::string& version = "");
    static void setProgress(F32 percent, const std::string& statusText = "");
    static void hide();

    // Set the download information
    void setDownloadInfo(const std::string& installerPath, S64 expectedSize);
    
    // Update progress based on current file size
    void updateProgress();

    // Explicit progress and status control
    void setProgressPercent(F32 percent, const std::string& statusText = "");
    void setUpdateText(const std::string& text);

private:
    LLProgressBar* mProgressBar;
    LLTextBox* mUpdateText;
    LLTextBox* mProgressText;
    
    std::string mInstallerPath;
    S64 mExpectedSize;
    LLFrameTimer mUpdateTimer;

    F32 mCurrentPercent;
    F32 mTargetPercent;
    LLFrameTimer mAnimTimer;
};

#endif // LL_LLFLOATERUPDATEPROGRESS_H
