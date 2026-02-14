# PowerShell build script for Firestorm Viewer
# Supports branch-specific build directories

param(
    [switch]$Clean,
    [switch]$Configure,
    [switch]$Build,
    [switch]$All,
    [string]$Config = "Release"
)

# If no action specified, default to All
if (-not ($Clean -or $Configure -or $Build)) {
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

# Set build directory based on branch
$buildDir = "build-vc170-64"
if ($gitBranch -and $gitBranch -ne "master" -and $gitBranch -ne "main") {
    # Use branch-specific build directory for feature branches
    $safeBranchName = $gitBranch -replace '[^a-zA-Z0-9-]', '-'
    $buildDir = "build-$safeBranchName"
    Write-Host "Using branch-specific build directory: $buildDir" -ForegroundColor Green
}

Write-Host "`nBuild directory: $buildDir" -ForegroundColor Cyan

# Clean
if ($Clean -or $All) {
    Write-Host "`n=== Cleaning build directory ===" -ForegroundColor Yellow
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir
        Write-Host "Removed $buildDir" -ForegroundColor Green
    } else {
        Write-Host "Build directory doesn't exist, nothing to clean" -ForegroundColor Gray
    }
}

# Configure
if ($Configure -or $All) {
    Write-Host "`n=== Configuring with autobuild ===" -ForegroundColor Yellow
    
    # Run autobuild configure
    $autobuildArgs = @(
        "configure",
        "-c", $Config,
        "--",
        "-DLL_TESTS:BOOL=FALSE"
    )
    
    Write-Host "Running: autobuild $($autobuildArgs -join ' ')" -ForegroundColor Cyan
    & autobuild @autobuildArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Autobuild configure failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
    
    # Autobuild always creates build-vc170-64, rename it if needed
    if ($buildDir -ne "build-vc170-64") {
        if (Test-Path "build-vc170-64") {
            Write-Host "Renaming build-vc170-64 to $buildDir" -ForegroundColor Cyan
            if (Test-Path $buildDir) {
                Remove-Item -Recurse -Force $buildDir
            }
            Move-Item "build-vc170-64" $buildDir
            Write-Host "Build directory renamed successfully" -ForegroundColor Green
        }
    }
    
    Write-Host "Configure completed successfully" -ForegroundColor Green
}

# Build
if ($Build -or $All) {
    Write-Host "`n=== Building solution ===" -ForegroundColor Yellow
    
    if (-not (Test-Path $buildDir)) {
        Write-Error "Build directory $buildDir doesn't exist. Run configure first."
        exit 1
    }
    
    $slnPath = Join-Path $buildDir "Firestorm.sln"
    if (-not (Test-Path $slnPath)) {
        Write-Error "Solution file not found at $slnPath"
        exit 1
    }
    
    # Use MSBuild to build the solution
    $msbuildArgs = @(
        $slnPath,
        "/p:Configuration=$Config",
        "/p:Platform=x64",
        "/m",  # Multi-processor build
        "/v:minimal"
    )
    
    Write-Host "Running: msbuild $($msbuildArgs -join ' ')" -ForegroundColor Cyan
    & msbuild @msbuildArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
    
    Write-Host "`nBuild completed successfully!" -ForegroundColor Green
    Write-Host "Executable location: $buildDir\newview\$Config\firestorm-bin.exe" -ForegroundColor Cyan
}

Write-Host "`n=== Build script completed ===" -ForegroundColor Green
