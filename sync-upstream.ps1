# PowerShell script to sync fork with upstream Firestorm repository
# This script will update your fork and rebase your feature branches

param(
    [string]$UpstreamUrl = "https://github.com/FirestormViewer/phoenix-firestorm.git",
    [string]$MainBranch = "master",
    [switch]$DryRun,
    [switch]$SkipFeatureBranches
)

$ErrorActionPreference = "Stop"

Write-Host "=== Firestorm Fork Sync Tool ===" -ForegroundColor Cyan
Write-Host ""

# Function to run git commands with dry-run support
function Invoke-GitCommand {
    param(
        [string]$Command,
        [string]$Description
    )
    
    Write-Host "=> $Description" -ForegroundColor Yellow
    
    if ($DryRun) {
        Write-Host "   [DRY RUN] Would execute: git $Command" -ForegroundColor Gray
        return $true
    } else {
        Write-Host "   Executing: git $Command" -ForegroundColor Gray
        Invoke-Expression "git $Command"
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Git command failed: $Command"
            return $false
        }
        return $true
    }
}

# Step 1: Check for uncommitted changes
Write-Host "Step 1: Checking for uncommitted changes..." -ForegroundColor Cyan
$status = git status --porcelain
if ($status) {
    Write-Host ""
    Write-Host "WARNING: You have uncommitted changes:" -ForegroundColor Red
    git status --short
    Write-Host ""
    
    $response = Read-Host "Do you want to stash these changes? (y/n)"
    if ($response -eq 'y') {
        if (-not $DryRun) {
            git stash push -m "Auto-stash before upstream sync $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
            Write-Host "Changes stashed successfully" -ForegroundColor Green
        } else {
            Write-Host "[DRY RUN] Would stash changes" -ForegroundColor Gray
        }
    } else {
        Write-Host "Please commit or stash your changes before running this script." -ForegroundColor Red
        exit 1
    }
}

# Step 2: Add upstream remote if it doesn't exist
Write-Host ""
Write-Host "Step 2: Setting up upstream remote..." -ForegroundColor Cyan
$remotes = git remote
if ($remotes -notcontains "upstream") {
    Invoke-GitCommand "remote add upstream $UpstreamUrl" "Adding upstream remote"
    Write-Host "   Upstream remote added: $UpstreamUrl" -ForegroundColor Green
} else {
    Write-Host "   Upstream remote already exists" -ForegroundColor Green
    
    # Update the upstream URL in case it changed
    if (-not $DryRun) {
        git remote set-url upstream $UpstreamUrl
    }
}

# Step 3: Fetch upstream changes
Write-Host ""
Write-Host "Step 3: Fetching upstream changes..." -ForegroundColor Cyan
Invoke-GitCommand "fetch upstream" "Fetching from upstream"
Invoke-GitCommand "fetch origin" "Fetching from origin (your fork)"

# Step 4: Get list of local branches (excluding current branch)
$currentBranch = git rev-parse --abbrev-ref HEAD
$allBranches = git branch --format='%(refname:short)' | Where-Object { $_ -ne $currentBranch }

Write-Host ""
Write-Host "Current branch: $currentBranch" -ForegroundColor Cyan
Write-Host "Other local branches: $($allBranches -join ', ')" -ForegroundColor Gray

# Step 5: Switch to main branch and update it
Write-Host ""
Write-Host "Step 4: Updating $MainBranch branch..." -ForegroundColor Cyan

if ($currentBranch -ne $MainBranch) {
    Invoke-GitCommand "checkout $MainBranch" "Switching to $MainBranch"
}

# Check if upstream has the main branch
$upstreamBranches = git branch -r | Where-Object { $_ -match "upstream/$MainBranch" }
if (-not $upstreamBranches) {
    Write-Host "   WARNING: upstream/$MainBranch not found. Available branches:" -ForegroundColor Yellow
    git branch -r | Where-Object { $_ -match "upstream/" }
    
    $altBranch = Read-Host "Enter the correct upstream branch name (or press Enter to skip)"
    if ($altBranch) {
        $MainBranch = $altBranch
    } else {
        Write-Host "Skipping upstream merge" -ForegroundColor Yellow
    }
}

# Rebase local main branch on upstream
if ($upstreamBranches -or $altBranch) {
    Invoke-GitCommand "rebase upstream/$MainBranch" "Rebasing $MainBranch on upstream/$MainBranch"
    
    # Push updated main branch to your fork
    Write-Host ""
    Write-Host "Step 5: Pushing updated $MainBranch to your fork..." -ForegroundColor Cyan
    
    if (-not $DryRun) {
        $pushResult = git push origin $MainBranch 2>&1
        
        if ($LASTEXITCODE -ne 0) {
            if ($pushResult -match "rejected|non-fast-forward") {
                Write-Host "   Push rejected. Your fork's $MainBranch has diverged." -ForegroundColor Yellow
                $response = Read-Host "Force push? This will overwrite your fork's $MainBranch (y/n)"
                
                if ($response -eq 'y') {
                    git push --force-with-lease origin $MainBranch
                    Write-Host "   Force pushed successfully" -ForegroundColor Green
                } else {
                    Write-Host "   Skipping push. You'll need to resolve this manually." -ForegroundColor Yellow
                }
            }
        } else {
            Write-Host "   Pushed successfully" -ForegroundColor Green
        }
    } else {
        Write-Host "   [DRY RUN] Would push $MainBranch to origin" -ForegroundColor Gray
    }
}

# Step 6: Rebase feature branches
if (-not $SkipFeatureBranches -and $allBranches) {
    Write-Host ""
    Write-Host "Step 6: Rebasing feature branches..." -ForegroundColor Cyan
    
    foreach ($branch in $allBranches) {
        Write-Host ""
        Write-Host "Processing branch: $branch" -ForegroundColor Yellow
        
        # Switch to the feature branch
        if (-not $DryRun) {
            git checkout $branch 2>&1 | Out-Null
            
            if ($LASTEXITCODE -ne 0) {
                Write-Host "   Failed to checkout $branch, skipping..." -ForegroundColor Red
                continue
            }
        }
        
        # Rebase on updated main branch
        Write-Host "   Rebasing $branch on $MainBranch..." -ForegroundColor Gray
        
        if (-not $DryRun) {
            $rebaseResult = git rebase $MainBranch 2>&1
            
            if ($LASTEXITCODE -ne 0) {
                Write-Host "   REBASE CONFLICT!" -ForegroundColor Red
                Write-Host "   You have conflicts in $branch that need manual resolution" -ForegroundColor Yellow
                Write-Host "   After resolving conflicts, run:" -ForegroundColor Yellow
                Write-Host "     git rebase --continue" -ForegroundColor White
                Write-Host "     git push --force-with-lease origin $branch" -ForegroundColor White
                Write-Host ""
                
                $response = Read-Host "Abort this rebase and continue with other branches? (y/n)"
                if ($response -eq 'y') {
                    git rebase --abort
                    Write-Host "   Rebase aborted" -ForegroundColor Yellow
                } else {
                    Write-Host "Stopping script. Resolve conflicts and re-run when ready." -ForegroundColor Red
                    exit 1
                }
            } else {
                Write-Host "   Rebase successful" -ForegroundColor Green
                
                # Push rebased branch to fork
                Write-Host "   Pushing rebased $branch to origin..." -ForegroundColor Gray
                
                $pushResult = git push --force-with-lease origin $branch 2>&1
                
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "   Pushed successfully" -ForegroundColor Green
                } else {
                    Write-Host "   Push failed. You may need to push manually later." -ForegroundColor Yellow
                }
            }
        } else {
            Write-Host "   [DRY RUN] Would rebase $branch on $MainBranch" -ForegroundColor Gray
            Write-Host "   [DRY RUN] Would push $branch to origin with --force-with-lease" -ForegroundColor Gray
        }
    }
}

# Step 7: Return to original branch
Write-Host ""
Write-Host "Step 7: Returning to original branch..." -ForegroundColor Cyan
if ($currentBranch -ne $MainBranch) {
    Invoke-GitCommand "checkout $currentBranch" "Switching back to $currentBranch"
}

# Summary
Write-Host ""
Write-Host "=== Sync Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "Summary:" -ForegroundColor Cyan
Write-Host "  - Updated $MainBranch from upstream Firestorm" -ForegroundColor White
Write-Host "  - Pushed $MainBranch to your fork" -ForegroundColor White

if (-not $SkipFeatureBranches -and $allBranches) {
    Write-Host "  - Rebased and pushed feature branches:" -ForegroundColor White
    foreach ($branch in $allBranches) {
        Write-Host "    * $branch" -ForegroundColor Gray
    }
}

Write-Host ""
Write-Host "Your fork is now up to date with upstream Firestorm!" -ForegroundColor Green
Write-Host ""

# Check if stash exists
if (-not $DryRun) {
    $stashList = git stash list
    if ($stashList -match "Auto-stash before upstream sync") {
        Write-Host "Don't forget: You have stashed changes. Run 'git stash pop' to restore them." -ForegroundColor Yellow
    }
}
