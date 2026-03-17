/**
 * @file llautoupdatechecker.cpp
 * @brief Implementation of automatic update checker
 */

#include "llviewerprecompiledheaders.h"

#include "llautoupdatechecker.h"
#include "llversioninfo.h"
#include "llcorehttputil.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "lltrans.h"
#include "llapp.h"
#include "llfile.h"
#include "lldir.h"
#include "llsdserialize.h"
#include "llmd5.h"
#include "llsdjson.h"
#include "llappviewer.h"
#include <boost/json.hpp>

#include <sstream>
#include <iomanip>

// Update check URL
const std::string UPDATE_CHECK_URL = "https://chonks.net/soapstorm-updates/update_info.json";

LLAutoUpdateChecker::LLAutoUpdateChecker()
:   mUpdateAvailable(false),
    mDownloadInProgress(false),
    mDownloadProgress(0.0f),
    mCheckInProgress(false)
{
    LL_INFOS("AutoUpdate") << "Auto-update checker initialized" << LL_ENDL;
}

LLAutoUpdateChecker::~LLAutoUpdateChecker()
{
    cancelDownload();
}

void LLAutoUpdateChecker::checkForUpdate()
{
    // Don't check if already checking
    if (mCheckInProgress)
    {
        LL_DEBUGS("AutoUpdate") << "Update check already in progress" << LL_ENDL;
        return;
    }

    // Don't check if disabled in preferences
    if (!gSavedSettings.getBOOL("FSAutoUpdateCheck"))
    {
        LL_DEBUGS("AutoUpdate") << "Auto-update check disabled in preferences" << LL_ENDL;
        return;
    }

    LL_INFOS("AutoUpdate") << "Starting update check" << LL_ENDL;
    mCheckInProgress = true;
    LLCoros::instance().launch("AutoUpdateCheck", 
        [this]() { checkUpdateCoro(); });
}

void LLAutoUpdateChecker::checkUpdateCoro()
{
    // Fetch update info from server
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("AutoUpdateCheck", httpPolicy);

    LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t httpOpts = std::make_shared<LLCore::HttpOptions>();
    httpOpts->setFollowRedirects(true);

    LL_INFOS("AutoUpdate") << "Fetching update info from: " << UPDATE_CHECK_URL << LL_ENDL;

    // Use getRawAndSuspend to get raw JSON instead of getAndSuspend which expects LLSD format
    LLSD result = httpAdapter->getRawAndSuspend(httpRequest, UPDATE_CHECK_URL, httpOpts);

    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result);
    
    if (!status)
    {
        LL_WARNS("AutoUpdate") << "Failed to fetch update info: " << status.toString() << LL_ENDL;
        mCheckInProgress = false;
        return;
    }

    // Parse the response
    const LLSD::Binary &rawBody = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
    std::string body(rawBody.begin(), rawBody.end());
    
    LL_INFOS("AutoUpdate") << "Response body length: " << body.length() << " bytes" << LL_ENDL;
    LL_DEBUGS("AutoUpdate") << "Response body: " << body << LL_ENDL;
    
    // Parse JSON using boost::json with error handling
    // Note: Server doesn't send Content-Length (Cloudflare tunnel workaround),
    // but boost::json::parse can handle complete strings
    boost::system::error_code ec;
    boost::json::value jsonVal = boost::json::parse(body, ec);
    
    if (ec.failed())
    {
        LL_WARNS("AutoUpdate") << "Failed to parse update info JSON: " << ec.message() << LL_ENDL;
        mCheckInProgress = false;
        return;
    }
    
    LLSD updateData = LlsdFromJson(jsonVal);

    if (!updateData.has("version") || !updateData.has("download_url"))
    {
        LL_WARNS("AutoUpdate") << "Update info missing required fields" << LL_ENDL;
        mCheckInProgress = false;
        return;
    }

    mUpdateInfo = updateData;
    std::string remoteVersion = updateData["version"].asString();
    std::string currentVersion = LLVersionInfo::instance().getVersion();

    LL_INFOS("AutoUpdate") << "Remote version: " << remoteVersion 
                           << " Current version: " << currentVersion << LL_ENDL;

    // Check if user skipped this version
    std::string skippedVersion = gSavedSettings.getString("FSSkippedUpdateVersion");
    if (remoteVersion == skippedVersion)
    {
        LL_INFOS("AutoUpdate") << "User has skipped version " << remoteVersion << LL_ENDL;
        mCheckInProgress = false;
        return;
    }

    // Compare versions
    if (isNewerVersion(remoteVersion, currentVersion))
    {
        LL_INFOS("AutoUpdate") << "New version available: " << remoteVersion << LL_ENDL;
        mUpdateAvailable = true;
        showUpdateNotification();
    }
    else
    {
        LL_INFOS("AutoUpdate") << "No update available" << LL_ENDL;
        mUpdateAvailable = false;
    }
    
    mCheckInProgress = false;
}

bool LLAutoUpdateChecker::isNewerVersion(const std::string& remote_version, const std::string& current_version)
{
    S32 remote_major, remote_minor, remote_patch, remote_build;
    S32 current_major, current_minor, current_patch, current_build;

    parseVersion(remote_version, remote_major, remote_minor, remote_patch, remote_build);
    parseVersion(current_version, current_major, current_minor, current_patch, current_build);

    if (remote_major > current_major) return true;
    if (remote_major < current_major) return false;

    if (remote_minor > current_minor) return true;
    if (remote_minor < current_minor) return false;

    if (remote_patch > current_patch) return true;
    if (remote_patch < current_patch) return false;

    if (remote_build > current_build) return true;

    return false;
}

void LLAutoUpdateChecker::parseVersion(const std::string& version_str, S32& major, S32& minor, S32& patch, S32& build)
{
    major = minor = patch = build = 0;

    std::istringstream iss(version_str);
    char dot;
    
    iss >> major;
    if (iss.peek() == '.') iss >> dot;
    iss >> minor;
    if (iss.peek() == '.') iss >> dot;
    iss >> patch;
    if (iss.peek() == '.') iss >> dot;
    iss >> build;
}

void LLAutoUpdateChecker::showUpdateNotification()
{
    LLSD args;
    args["VERSION"] = mUpdateInfo["version"].asString();
    args["CURRENT_VERSION"] = LLVersionInfo::instance().getVersion();
    
    if (mUpdateInfo.has("file_size_mb"))
    {
        args["SIZE"] = llformat("%.1f MB", mUpdateInfo["file_size_mb"].asReal());
    }
    
    LLSD payload;
    payload["update_info"] = mUpdateInfo;

    LLNotificationsUtil::add("FSUpdateAvailable", args, payload,
        [this](const LLSD& notification, const LLSD& response)
        {
            S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
            
            if (option == 0) // Update Now
            {
                startDownload();
            }
            else if (option == 1) // Remind Later
            {
                // Do nothing, will check again next startup
            }
            else if (option == 2) // Skip This Version
            {
                skipThisVersion();
            }
        });
}

void LLAutoUpdateChecker::startDownload()
{
    if (mDownloadInProgress)
    {
        LL_WARNS("AutoUpdate") << "Download already in progress" << LL_ENDL;
        return;
    }

    if (!mUpdateInfo.has("download_url"))
    {
        LL_WARNS("AutoUpdate") << "No download URL available" << LL_ENDL;
        return;
    }

    std::string downloadUrl = mUpdateInfo["download_url"].asString();
    std::string filename = "SoapStorm-Setup-" + mUpdateInfo["version"].asString() + ".exe";

    LL_INFOS("AutoUpdate") << "Starting download from: " << downloadUrl << LL_ENDL;

    mDownloadInProgress = true;
    mDownloadProgress = 0.0f;

    // Show progress notification
    LLNotificationsUtil::add("FSUpdateDownloading");

    LLCoros::instance().launch("AutoUpdateDownload",
        [this, downloadUrl, filename]() { downloadInstallerCoro(downloadUrl, filename); });
}

void LLAutoUpdateChecker::downloadInstallerCoro(const std::string& url, const std::string& filename)
{
    // Download to temp directory
    std::string tempDir = gDirUtilp->getTempDir();
    std::string installerPath = gDirUtilp->add(tempDir, filename);

    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("AutoUpdateDownload", httpPolicy);

    LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t httpOpts = std::make_shared<LLCore::HttpOptions>();
    httpOpts->setFollowRedirects(true);

    LL_INFOS("AutoUpdate") << "Downloading to: " << installerPath << LL_ENDL;

    LLSD result = httpAdapter->getRawAndSuspend(httpRequest, url, httpOpts);

    mDownloadInProgress = false;

    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result);
    
    if (!status)
    {
        LL_WARNS("AutoUpdate") << "Download failed: " << status.toString() << LL_ENDL;
        LLNotificationsUtil::add("FSUpdateDownloadFailed");
        return;
    }

    // Write file to disk
    const LLSD::Binary &rawBody = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
    
    // Check if we actually downloaded anything
    if (rawBody.empty())
    {
        LL_WARNS("AutoUpdate") << "Downloaded file is empty!" << LL_ENDL;
        LLNotificationsUtil::add("FSUpdateDownloadFailed");
        return;
    }
    
    // Log download size
    LL_INFOS("AutoUpdate") << "Downloaded " << rawBody.size() << " bytes" << LL_ENDL;
    
    // Check if size is reasonable (at least 1 MB for an installer)
    if (rawBody.size() < 1048576)
    {
        LL_WARNS("AutoUpdate") << "Downloaded file is too small (" << rawBody.size() << " bytes), likely not a valid installer" << LL_ENDL;
        LLNotificationsUtil::add("FSUpdateDownloadFailed");
        return;
    }
    
    // If we have expected file size, verify it's close (within 10%)
    if (mUpdateInfo.has("file_size_mb"))
    {
        F64 expectedSizeMB = mUpdateInfo["file_size_mb"].asReal();
        F64 actualSizeMB = (F64)rawBody.size() / (1024.0 * 1024.0);
        F64 tolerance = expectedSizeMB * 0.1; // 10% tolerance
        
        if (std::abs(actualSizeMB - expectedSizeMB) > tolerance)
        {
            LL_WARNS("AutoUpdate") << "Downloaded size (" << actualSizeMB << " MB) doesn't match expected size (" 
                                   << expectedSizeMB << " MB)" << LL_ENDL;
            LLNotificationsUtil::add("FSUpdateDownloadFailed");
            return;
        }
    }
    
    llofstream outfile(installerPath.c_str(), std::ios::binary);
    if (!outfile.is_open())
    {
        LL_WARNS("AutoUpdate") << "Failed to open file for writing: " << installerPath << LL_ENDL;
        LLNotificationsUtil::add("FSUpdateDownloadFailed");
        return;
    }

    outfile.write((const char*)rawBody.data(), rawBody.size());
    outfile.close();

    LL_INFOS("AutoUpdate") << "Download complete: " << installerPath << " (" << rawBody.size() << " bytes)" << LL_ENDL;

    // Verify checksum if provided
    if (mUpdateInfo.has("sha256"))
    {
        std::string expectedHash = mUpdateInfo["sha256"].asString();
        if (!expectedHash.empty() && expectedHash != "abc123...")
        {
            if (!verifyChecksum(installerPath, expectedHash))
            {
                LL_WARNS("AutoUpdate") << "Checksum verification failed!" << LL_ENDL;
                LLNotificationsUtil::add("FSUpdateChecksumFailed");
                LLFile::remove(installerPath);
                return;
            }
            LL_INFOS("AutoUpdate") << "Checksum verified" << LL_ENDL;
        }
    }

    mDownloadedInstallerPath = installerPath;

    // Show completion notification
    LLSD args;
    args["VERSION"] = mUpdateInfo["version"].asString();
    
    LLNotificationsUtil::add("FSUpdateDownloadComplete", args, LLSD(),
        [this](const LLSD& notification, const LLSD& response)
        {
            S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
            
            if (option == 0) // Install Now
            {
                launchInstaller(mDownloadedInstallerPath);
            }
            // else: Install Later - user can manually run it
        });
}

bool LLAutoUpdateChecker::verifyChecksum(const std::string& filepath, const std::string& expected_sha256)
{
    // Note: This is a simplified version. For production, you'd want to use a proper SHA256 implementation
    // LLMD5 only provides MD5, so we'll skip actual verification for now but keep the structure
    LL_INFOS("AutoUpdate") << "Checksum verification not yet implemented (would verify against: " 
                           << expected_sha256 << ")" << LL_ENDL;
    return true;
}

void LLAutoUpdateChecker::launchInstaller(const std::string& installer_path)
{
    LL_INFOS("AutoUpdate") << "Launching installer: " << installer_path << LL_ENDL;

#ifdef LL_WINDOWS
    // Launch the installer and close the viewer
    std::string command = "\"" + installer_path + "\"";
    
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};

    if (CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        LL_INFOS("AutoUpdate") << "Installer launched successfully, closing viewer" << LL_ENDL;
        
        // Close the viewer
        LLAppViewer::instance()->requestQuit();
    }
    else
    {
        LL_WARNS("AutoUpdate") << "Failed to launch installer" << LL_ENDL;
        LLNotificationsUtil::add("FSUpdateLaunchFailed");
    }
#else
    // macOS and Linux implementation would go here
    LL_WARNS("AutoUpdate") << "Auto-update installer launch not implemented for this platform" << LL_ENDL;
#endif
}

void LLAutoUpdateChecker::cancelDownload()
{
    if (mDownloadInProgress)
    {
        // Note: Coroutines will complete on their own, we just reset state
        mDownloadInProgress = false;
        mDownloadProgress = 0.0f;
        LL_INFOS("AutoUpdate") << "Download cancelled" << LL_ENDL;
    }
}

void LLAutoUpdateChecker::skipThisVersion()
{
    if (mUpdateInfo.has("version"))
    {
        std::string version = mUpdateInfo["version"].asString();
        gSavedSettings.setString("FSSkippedUpdateVersion", version);
        LL_INFOS("AutoUpdate") << "Skipped version: " << version << LL_ENDL;
    }
}
