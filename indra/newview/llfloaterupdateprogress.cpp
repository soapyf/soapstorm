/**
 * @file llfloaterupdateprogress.cpp
 * @brief Progress dialog for SoapStorm auto-update downloads
 *
 * Copyright (c) 2026, SoapStorm Project
 */

#include "llviewerprecompiledheaders.h"

#include "llfloaterupdateprogress.h"
#include "llfloaterreg.h"
#include "llprogressbar.h"
#include "lltextbox.h"
#include "llfile.h"

LLFloaterUpdateProgress::LLFloaterUpdateProgress(const LLSD& key)
:   LLFloater(key),
    mProgressBar(NULL),
    mUpdateText(NULL),
    mProgressText(NULL),
    mExpectedSize(0),
    mCurrentPercent(0.0f),
    mTargetPercent(0.0f)
{
}

LLFloaterUpdateProgress::~LLFloaterUpdateProgress()
{
}

bool LLFloaterUpdateProgress::postBuild()
{
    mProgressBar = getChild<LLProgressBar>("progress_bar");
    mUpdateText = getChild<LLTextBox>("update_text");
    mProgressText = getChild<LLTextBox>("progress_text");
    
    // Start with 0% progress
    if (mProgressBar)
    {
        mProgressBar->setValue(0.0f);
    }

    if (mProgressText)
    {
        mProgressText->setText(std::string("Starting download..."));
    }
    
    // Center the floater
    center();
    
    return true;
}

void LLFloaterUpdateProgress::onOpen(const LLSD& key)
{
    center();
    mAnimTimer.reset();
}

void LLFloaterUpdateProgress::draw()
{
    F32 dt = mAnimTimer.getElapsedTimeAndResetF32();
    if (dt > 0.1f) dt = 0.1f;

    if (!mInstallerPath.empty() && mExpectedSize > 0)
    {
        if (mUpdateTimer.getElapsedTimeF32() > 0.25f)
        {
            updateProgress();
            mUpdateTimer.reset();
        }
    }
    else
    {
        // Smooth interpolation to target milestone
        if (mCurrentPercent < mTargetPercent)
        {
            F32 diff = mTargetPercent - mCurrentPercent;
            F32 step = diff * dt * 5.0f;
            if (step < 1.0f * dt) step = 1.0f * dt;
            mCurrentPercent = llmin(mCurrentPercent + step, mTargetPercent);
            if (mProgressBar)
            {
                mProgressBar->setValue(mCurrentPercent);
            }
        }
    }
    
    LLFloater::draw();
}

void LLFloaterUpdateProgress::show(const std::string& version)
{
    LLFloaterUpdateProgress* self = LLFloaterReg::showTypedInstance<LLFloaterUpdateProgress>("update_progress");
    if (self)
    {
        self->center();
        if (!version.empty())
        {
            self->setUpdateText(llformat("Updating to SoapStorm %s...", version.c_str()));
        }
        else
        {
            self->setUpdateText("Updating SoapStorm...");
        }
        self->setProgressPercent(0.0f, "Connecting...");
    }
}

void LLFloaterUpdateProgress::setProgress(F32 percent, const std::string& statusText)
{
    LLFloaterUpdateProgress* self = LLFloaterReg::findTypedInstance<LLFloaterUpdateProgress>("update_progress");
    if (self)
    {
        self->setProgressPercent(percent, statusText);
    }
}

void LLFloaterUpdateProgress::hide()
{
    LLFloaterReg::hideInstance("update_progress");
}

void LLFloaterUpdateProgress::setProgressPercent(F32 percent, const std::string& statusText)
{
    mTargetPercent = llclamp(percent, 0.0f, 100.0f);
    if (mTargetPercent < mCurrentPercent || mTargetPercent >= 100.0f)
    {
        mCurrentPercent = mTargetPercent;
        if (mProgressBar)
        {
            mProgressBar->setValue(mCurrentPercent);
        }
    }

    if (mProgressText && !statusText.empty())
    {
        mProgressText->setText(statusText);
    }
    mAnimTimer.reset();
}

void LLFloaterUpdateProgress::setUpdateText(const std::string& text)
{
    if (mUpdateText && !text.empty())
    {
        mUpdateText->setText(text);
    }
}

void LLFloaterUpdateProgress::setDownloadInfo(const std::string& installerPath, S64 expectedSize)
{
    mInstallerPath = installerPath;
    mExpectedSize = expectedSize;
    
    // Reset timer
    mUpdateTimer.reset();
    
    LL_INFOS("AutoUpdate") << "Progress dialog tracking: " << installerPath 
                           << " expected size: " << expectedSize << " bytes" << LL_ENDL;
}

void LLFloaterUpdateProgress::updateProgress()
{
    if (mInstallerPath.empty() || mExpectedSize <= 0)
    {
        return;
    }
    
    // Check current file size
    S64 currentSize = 0;
    if (LLFile::isfile(mInstallerPath))
    {
        llstat file_stat;
        if (LLFile::stat(mInstallerPath, &file_stat) == 0)
        {
            currentSize = file_stat.st_size;
        }
    }
    
    // Calculate progress
    F32 progress = 0.0f;
    if (mExpectedSize > 0)
    {
        progress = (F32)currentSize / (F32)mExpectedSize;
        progress = llclamp(progress, 0.0f, 1.0f);
    }
    
    // Update progress bar
    if (mProgressBar)
    {
        mProgressBar->setValue(progress * 100.0f);
    }
    
    // Update text
    if (mProgressText)
    {
        F32 currentMB = (F32)currentSize / (1024.0f * 1024.0f);
        F32 expectedMB = (F32)mExpectedSize / (1024.0f * 1024.0f);
        
        std::string text = llformat("%.1f MB / %.1f MB", currentMB, expectedMB);
        mProgressText->setText(text);
    }
}
