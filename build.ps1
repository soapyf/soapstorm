# PowerShell build script for Firestorm Viewer
# Supports branch-specific build directories

param(
    [switch]$Clean,
    [switch]$Configure,
    [switch]$Build,
    [switch]$All,
    [string]$Config = "ReleaseFS",
    [bool]$UseCache = $false,
    [int]$Jobs = 0,
    [string]$Channel = "Release",
    [bool]$AVX2 = $true,
    [bool]$Package = $true,
    [string]$OutputDir = ""
)

# ============================================
# USER CONFIGURATION
# Edit the default values above in the param() block
# ============================================

# If no action specified, default to All
if (-not ($Configure -or $Build)) {
    $All = $true
}

$ErrorActionPreference = "Stop"

# Determine current git branch
$gitBranch = ""
try {
    $gitBranch = git rev-parse --abbrev-ref HEAD 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Current git branch: $gitBranch" -ForegroundColor Cyan
    }
} catch {
    Write-Host "Not in a git repository or git not available" -ForegroundColor Yellow
}

# Set build directory - based on the autobuild VSVER (set later in configure section)
# AUTOBUILD_VSVER=171 → build-vc171-64, 170 → build-vc170-64, etc.
$vsver = "171"
$buildDir = "build-vc$vsver-64"
Write-Host "`nBuild directory: $buildDir" -ForegroundColor Cyan
if ($gitBranch -and $gitBranch -ne "master" -and $gitBranch -ne "main") {
    Write-Host "Note: Building feature branch '$gitBranch' in shared build directory" -ForegroundColor Yellow
}

# Clean
if ($Clean) {
    Write-Host "`n=== Cleaning build directory ===" -ForegroundColor Yellow
    if (Test-Path $buildDir) {
        # Try to remove with retries for locked files
        $maxRetries = 3
        $retryDelay = 2
        $success = $false
        
        for ($i = 1; $i -le $maxRetries; $i++) {
            try {
                Remove-Item -Recurse -Force $buildDir -ErrorAction Stop
                Write-Host "Removed $buildDir" -ForegroundColor Green
                $success = $true
                break
            }
            catch {
                if ($i -lt $maxRetries) {
                    Write-Host "Attempt $i failed: Some files are locked. Waiting ${retryDelay}s before retry..." -ForegroundColor Yellow
                    Start-Sleep -Seconds $retryDelay
                }
                else {
                    Write-Host "Warning: Could not remove all files after $maxRetries attempts" -ForegroundColor Yellow
                    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Yellow
                    Write-Host "Continuing anyway... You may need to manually delete: $buildDir" -ForegroundColor Yellow
                }
            }
        }
    } else {
        Write-Host "Build directory doesn't exist, nothing to clean" -ForegroundColor Gray
    }
}

# Configure
if ($Configure -or $All) {
    Write-Host "`n=== Configuring with autobuild ===" -ForegroundColor Yellow

    # -------------------------------------------------------------------------
    # Autobuild invokes configure_firestorm.sh via WSL bash. WSL does not
    # inherit Windows environment variables by default, so we use WSLENV.
    # -------------------------------------------------------------------------

    # Required by configure_firestorm.sh
    $buildVariablesPath = (Resolve-Path (Join-Path $PSScriptRoot "..\fs-build-variables\variables")).Path
    $env:AUTOBUILD_VARIABLES_FILE = $buildVariablesPath
    $env:AUTOBUILD_VSVER          = $vsver   # 171 = VS 2022 17.x

    # These are normally set by "autobuild source_environment", which requires
    # Cygwin's cygpath. Since Cygwin is not installed, we set them manually.
    $env:AUTOBUILD_WIN_CMAKE_GEN  = "Visual Studio 17 2022"
    $env:AUTOBUILD_WIN_VSPLATFORM = "x64"
    $env:AUTOBUILD_ADDRSIZE       = "64"

    # configure_firestorm.sh calls cmake, which must be the Windows cmake
    # (to generate VS solutions). It's bundled with VS but not in PATH.
    # We create a thin cmake wrapper in WSL that calls the VS cmake.exe,
    # then inject it into PATH via BASH_ENV (sourced by non-interactive bash).
    $vsCmakeBin = "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    if (Test-Path $vsCmakeBin) {
        $wslCmakeExe = '/mnt/c/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
        # Use pipe to write file contents, avoiding shell escaping issues with spaces in path
        wsl -- mkdir -p /tmp/wsl-build-tools
        "#!/bin/sh`nexec `"$wslCmakeExe`" `"`$@`"" | wsl -- bash -c "cat > /tmp/wsl-build-tools/cmake"
        wsl -- chmod +x /tmp/wsl-build-tools/cmake
        'export PATH=/tmp/wsl-build-tools:$PATH' | wsl -- bash -c "cat > /tmp/wsl-bash-env.sh"
        $env:BASH_ENV = "/tmp/wsl-bash-env.sh"  # sourced by non-interactive bash
        Write-Host "cmake wrapper created in WSL at /tmp/wsl-build-tools/cmake" -ForegroundColor Gray
    } else {
        Write-Warning "VS cmake not found at: $vsCmakeBin"
        Write-Warning "Either install CMake system-wide (cmake.org) or install it in WSL (wsl sudo apt install cmake)"
    }

    # Set revision explicitly so cmake uses ENV{revision} (priority 1) rather
    # than falling through to its own git invocation which can return the full
    # upstream commit count instead of the local fork count.
    $gitRevision = (git rev-list --count HEAD 2>$null).Trim()
    if ($gitRevision -match '^\d+$') {
        $env:VIEWER_REVISION = $gitRevision
        Write-Host "Git revision: $gitRevision" -ForegroundColor Gray
    } else {
        $env:VIEWER_REVISION = "0"
        Write-Host "Could not determine git revision, using 0" -ForegroundColor Yellow
    }

    $env:WSLENV = "AUTOBUILD_VARIABLES_FILE/p:AUTOBUILD_VSVER:AUTOBUILD_WIN_CMAKE_GEN:AUTOBUILD_WIN_VSPLATFORM:AUTOBUILD_ADDRSIZE:BASH_ENV:VIEWER_REVISION"
    Write-Host "AUTOBUILD_VARIABLES_FILE  = $($env:AUTOBUILD_VARIABLES_FILE)" -ForegroundColor Gray
    Write-Host "AUTOBUILD_VSVER           = $($env:AUTOBUILD_VSVER)" -ForegroundColor Gray
    Write-Host "AUTOBUILD_WIN_CMAKE_GEN   = $($env:AUTOBUILD_WIN_CMAKE_GEN)" -ForegroundColor Gray
    Write-Host "AUTOBUILD_WIN_VSPLATFORM  = $($env:AUTOBUILD_WIN_VSPLATFORM)" -ForegroundColor Gray
    Write-Host "AUTOBUILD_ADDRSIZE        = $($env:AUTOBUILD_ADDRSIZE)" -ForegroundColor Gray

    # Install packages - exclude proprietary/unavailable packages by parsing autobuild.xml (LLSD format)
    $excludedPackages = @('fmodstudio', 'havok-source', 'kdu', 'llphysicsextensions_source', 'llphysicsextensions_tpv', 'discord-rpc', 'discord_sdk')
    [xml]$autobuildXml = Get-Content (Join-Path $PSScriptRoot "autobuild.xml") -Raw
    $abNodes = $autobuildXml.llsd.map.ChildNodes
    $packageList = @()
    for ($i = 0; $i -lt $abNodes.Count; $i++) {
        if ($abNodes[$i].LocalName -eq 'key' -and $abNodes[$i].InnerText -eq 'installables') {
            $packageList = $abNodes[$i+1].ChildNodes |
                Where-Object { $_.LocalName -eq 'key' } |
                ForEach-Object { $_.InnerText } |
                Where-Object { $_ -notin $excludedPackages }
            break
        }
    }
    if ($packageList.Count -eq 0) {
        Write-Error "Failed to parse package list from autobuild.xml"
        exit 1
    }
    Write-Host "Running: autobuild install (excluding: $($excludedPackages -join ', '))" -ForegroundColor Cyan
    Write-Host "Installing $($packageList.Count) packages" -ForegroundColor Gray
    & autobuild install @packageList
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Autobuild install failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    # Run autobuild configure
    $autobuildArgs = @(
        "configure",
        "-c", $Config,
        "--"
    )
    
    # Add optional flags
    if ($Channel) {
        $autobuildArgs += "--chan"
        $autobuildArgs += $Channel
    }
    if ($AVX2) {
        $autobuildArgs += "--avx2"
    }
    if ($Package) {
        $autobuildArgs += "--package"
    }
    if ($UseCache) {
        $autobuildArgs += "--compiler-cache"
    }
    if ($Jobs -gt 0) {
        $autobuildArgs += "--jobs"
        $autobuildArgs += $Jobs.ToString()
    }
    
    $autobuildArgs += "-DLL_TESTS:BOOL=FALSE"
    $autobuildArgs += "-DUSE_FMODSTUDIO:BOOL=TRUE"
    $autobuildArgs += "-DFMODSTUDIO_LIBRARY=C:/fmod_lib/fmod_vc.lib"
    $autobuildArgs += "-DFMODSTUDIO_INCLUDE_DIR=C:/fmod_include"
    $autobuildArgs += "-DHAVOK:BOOL=FALSE"
    $autobuildArgs += "-DUSE_KDU:BOOL=FALSE"
    $autobuildArgs += "-DOPENSIM:BOOL=FALSE"
    
    Write-Host "Running: autobuild $($autobuildArgs -join ' ')" -ForegroundColor Cyan
    & autobuild @autobuildArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Autobuild configure failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
    Write-Host "Configure completed successfully" -ForegroundColor Green
}

# Build
if ($Build -or $All) {
    Write-Host "`n=== Building solution ===" -ForegroundColor Yellow

    # Kill processes that can lock build outputs
    Get-Process -Name "firestorm-bin" -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name "makensis" -ErrorAction SilentlyContinue | Stop-Process -Force

    if (-not (Test-Path $buildDir)) {
        Write-Error "Build directory $buildDir doesn't exist. Run configure first."
        exit 1
    }
    
    $slnPath = Join-Path $buildDir "Firestorm.sln"
    if (-not (Test-Path $slnPath)) {
        Write-Error "Solution file not found at $slnPath"
        exit 1
    }
    
    # Find MSBuild using vswhere
    $vsWherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWherePath) {
        $vsPath = & $vsWherePath -latest -property installationPath
        $msbuildPath = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
        if (-not (Test-Path $msbuildPath)) {
            # Try older VS2022 location
            $msbuildPath = Join-Path $vsPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
        }
    } else {
        # Fallback to common paths
        $msbuildPath = "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
        if (-not (Test-Path $msbuildPath)) {
            $msbuildPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
        }
    }
    
    if (-not (Test-Path $msbuildPath)) {
        Write-Error "MSBuild not found. Please ensure Visual Studio 2022 is installed."
        exit 1
    }
    
    Write-Host "Using MSBuild: $msbuildPath" -ForegroundColor Gray

    # Seed fmod.dll into the CMake shared-libs staging directory so the build's
    # copy_win_scripts step (and NSIS packager) can find it.
    $fmodSrcDll = "C:\fmod_lib\fmod.dll"
    $fmodStagingDir = Join-Path $buildDir "sharedlibs\Release"
    if (Test-Path $fmodSrcDll) {
        New-Item -ItemType Directory -Force $fmodStagingDir | Out-Null
        Copy-Item $fmodSrcDll (Join-Path $fmodStagingDir "fmod.dll") -Force
        Write-Host "Staged fmod.dll -> $fmodStagingDir" -ForegroundColor Gray

        # Also seed packages/lib/release so CMake's Copy3rdPartyLibs step can find it
        $fmodPkgDir = Join-Path $buildDir "packages\lib\release"
        New-Item -ItemType Directory -Force $fmodPkgDir | Out-Null
        Copy-Item $fmodSrcDll (Join-Path $fmodPkgDir "fmod.dll") -Force
        Write-Host "Staged fmod.dll -> $fmodPkgDir" -ForegroundColor Gray
    } else {
        Write-Warning "fmod.dll not found at $fmodSrcDll - build may fail or run without audio"
    }

    # Map autobuild config names to VS solution config names
    $vsConfig = "Release"  # All autobuild configs map to Release in the solution
    
    # Use MSBuild to build the solution
    $msbuildArgs = @(
        $slnPath,
        "/p:Configuration=$vsConfig",
        "/p:Platform=x64",
        "/m",  # Multi-processor build
        "/v:minimal"
    )
    
    Write-Host "Running: MSBuild $($msbuildArgs -join ' ')" -ForegroundColor Cyan
    & $msbuildPath @msbuildArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
    
    # Verify executable was built
    $exePath = Join-Path $buildDir "newview\Release\firestorm-bin.exe"
    if (-not (Test-Path $exePath)) {
        Write-Error "Build completed but executable not found at $exePath"
        exit 1
    }
    
    Write-Host "`nBuild completed successfully!" -ForegroundColor Green
    
    # Run packaging step if requested
    if ($Package) {
        Write-Host "`n=== Running packaging step ===" -ForegroundColor Cyan
        Push-Location $buildDir
        $packageProject = "newview\llpackage.vcxproj"
        if (Test-Path $packageProject) {
            & $msbuildPath $packageProject /p:Configuration=Release /v:minimal /nologo
            if ($LASTEXITCODE -eq 0) {
                Write-Host "Packaging completed successfully" -ForegroundColor Green
            } else {
                Write-Host "Packaging completed with warnings" -ForegroundColor Yellow
            }
        } else {
            Write-Host "Package project not found: $packageProject" -ForegroundColor Yellow
        }
        Pop-Location
    }
    
    # Copy to output directory if specified
    if ($OutputDir) {
        Write-Host "`n=== Copying build to $OutputDir ===" -ForegroundColor Cyan
        if (Test-Path $OutputDir) {
            Remove-Item -Recurse -Force $OutputDir
        }
        New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
        
        $releaseDir = Join-Path $buildDir "newview\Release"
        Copy-Item -Recurse -Force "$releaseDir\*" $OutputDir
        
        Write-Host "Build copied to: $OutputDir\firestorm-bin.exe" -ForegroundColor Cyan
    } else {
        Write-Host "Executable location: $buildDir\newview\Release\firestorm-bin.exe" -ForegroundColor Cyan
    }
}

Write-Host "`n=== Build script completed ===" -ForegroundColor Green
