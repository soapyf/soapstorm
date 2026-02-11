# clean-build.ps1 - Firestorm Clean Build Script
# Completely clean build (removes all previous build files)

# ============================================
# Check for admin rights and elevate if needed
# ============================================
if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
    Write-Host "Requesting administrator privileges..." -ForegroundColor Yellow
    Start-Process powershell.exe "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

# ============================================
# USER CONFIGURATION - Edit these paths
# ============================================
$SourceRepoPath = ""
$BuildVariablesPath = ""
$OutputDirectory = "" # Leave empty to keep output in repo, or set like "C:\FirestormBuilds\soapstorm"

# Build Configuration
$ChannelName = "soapstorm"
$EnableAVX2 = $true
$EnablePackaging = $true
# ============================================

# Force output to console
$ErrorActionPreference = "Continue"
$VerbosePreference = "Continue"

# Set required environment variable
$env:AUTOBUILD_VARIABLES_FILE = $BuildVariablesPath

# Navigate to project directory
Set-Location $SourceRepoPath

Write-Host "=== Cleaning previous build ===" -ForegroundColor Yellow
if (Test-Path "build-vc170-64") {
    Remove-Item -Recurse -Force "build-vc170-64"
    Write-Host "Previous build files removed" -ForegroundColor Green
}

# Build configuration command
$ConfigCommand = "autobuild configure -A 64 -c ReleaseFS_open -- --chan $ChannelName"
if ($EnableAVX2) {
    $ConfigCommand += " --avx2"
}
if ($EnablePackaging) {
    $ConfigCommand += " --package"
}

Write-Host "`n=== Configuring Firestorm ===" -ForegroundColor Cyan
Write-Host "Configuration: $ConfigCommand" -ForegroundColor Gray
Invoke-Expression $ConfigCommand

if ($LASTEXITCODE -ne 0) {
    Write-Host "Configuration failed!" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "`n=== Building Firestorm (this will take a while) ===" -ForegroundColor Cyan
autobuild build -A 64 -c ReleaseFS_open --no-configure

# Check if the executable was built
if (Test-Path "build-vc170-64\newview\Release\firestorm-bin.exe") {
    Write-Host "`n=== Build completed, running packaging step ===" -ForegroundColor Green
    
    Set-Location "build-vc170-64"
    msbuild newview\llpackage.vcxproj /p:Configuration=Release /v:minimal
    Set-Location ".."
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`n=== Packaging Success! ===" -ForegroundColor Green
    } else {
        Write-Host "`nPackaging completed with warnings" -ForegroundColor Yellow
    }
    
    # Copy to output directory if specified
    if ($OutputDirectory -ne "") {
        Write-Host "`n=== Copying build to $OutputDirectory ===" -ForegroundColor Cyan
        
        try {
            if (Test-Path $OutputDirectory) {
                Remove-Item -Recurse -Force $OutputDirectory -ErrorAction Stop
            }
            New-Item -ItemType Directory -Force -Path $OutputDirectory -ErrorAction Stop | Out-Null
            
            Copy-Item -Recurse -Force "build-vc170-64\newview\Release\*" $OutputDirectory -ErrorAction Stop
            
            Write-Host "`n=== Clean Build Success! ===" -ForegroundColor Green
            Write-Host "Viewer location: $OutputDirectory\firestorm-bin.exe" -ForegroundColor Cyan
        }
        catch {
            Write-Host "`n=== Copy Failed! ===" -ForegroundColor Red
            Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "Viewer is still available at: $SourceRepoPath\build-vc170-64\newview\Release\firestorm-bin.exe" -ForegroundColor Yellow
        }
    } else {
        Write-Host "`n=== Clean Build Success! ===" -ForegroundColor Green
        Write-Host "Viewer location: $SourceRepoPath\build-vc170-64\newview\Release\firestorm-bin.exe" -ForegroundColor Cyan
    }
} else {
    Write-Host "`n=== Build Failed! ===" -ForegroundColor Red
    Write-Host "Executable not found at expected location" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "`nPress Enter to exit..." -ForegroundColor Cyan
Read-Host