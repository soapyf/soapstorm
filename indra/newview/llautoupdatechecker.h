/**
 * @file llautoupdatechecker.h
 * @brief Automatic update checker for SoapStorm viewer
 *
 * Checks for new viewer versions from a remote server and handles
 * the download and installation process with user consent.
 */

#ifndef LL_LLAUTOUPDATECHECKER_H
#define LL_LLAUTOUPDATECHECKER_H

#include "llsingleton.h"
#include "lleventcoro.h"
#include "llcoros.h"
#include "llsd.h"
#include <string>

class LLAutoUpdateChecker : public LLSingleton<LLAutoUpdateChecker>
{
    LLSINGLETON(LLAutoUpdateChecker);
    virtual ~LLAutoUpdateChecker();

public:
    // Main entry point - checks for updates
    void checkForUpdate();
    
    // Start downloading the update installer
    void startDownload();
    
    // Cancel ongoing download
    void cancelDownload();
    
    // Skip the current available version
    void skipThisVersion();
    
    // Get current update info
    const LLSD& getUpdateInfo() const { return mUpdateInfo; }
    
    // Check if an update is available
    bool isUpdateAvailable() const { return mUpdateAvailable; }
    
    // Check if download is in progress
    bool isDownloading() const { return mDownloadInProgress; }
    
    // Get download progress (0.0 to 1.0)
    F32 getDownloadProgress() const { return mDownloadProgress; }

private:
    // Coroutine for checking updates
    void checkUpdateCoro();
    
    // Coroutine for downloading installer
    void downloadInstallerCoro(const std::string& url, const std::string& filename);
    
    // Compare version strings
    bool isNewerVersion(const std::string& remote_version, const std::string& current_version);
    
    // Parse version string into components
    void parseVersion(const std::string& version_str, S32& major, S32& minor, S32& patch, S32& build);
    
    // Show update notification to user
    void showUpdateNotification();
    
    // Launch the installer
    void launchInstaller(const std::string& installer_path);
    
    // Verify downloaded file checksum
    bool verifyChecksum(const std::string& filepath, const std::string& expected_sha256);

private:
    LLSD mUpdateInfo;
    bool mUpdateAvailable;
    bool mDownloadInProgress;
    F32 mDownloadProgress;
    std::string mDownloadedInstallerPath;
    bool mCheckInProgress;
};

#endif // LL_LLAUTOUPDATECHECKER_H
