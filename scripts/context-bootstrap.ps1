param(
    [switch]$IncludeGitDiffSummary
)

$ErrorActionPreference = "Stop"

function Write-Section {
    param([string]$Title)
    Write-Host ""
    Write-Host "=== $Title ==="
}

Set-Location (Split-Path -Parent $PSScriptRoot)

Write-Section "Repo"
Write-Host "Root: $(Get-Location)"
Write-Host "Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

Write-Section "Active Work"
$activeWork = Join-Path (Get-Location) "docs/active-work.md"
if (Test-Path $activeWork) {
    Get-Content $activeWork
} else {
    Write-Host "docs/active-work.md not found."
}

Write-Section "Git Status"
git status --short

if ($IncludeGitDiffSummary) {
    Write-Section "Diff Summary"
    git diff --stat
}

Write-Section "Core Paths"
@(
    "NOVA.jucer",
    "Builds/VisualStudio2022/NOVA.sln",
    "docs/codex/project-map.md",
    "docs/codex/task-brief-template.md"
) | ForEach-Object { Write-Host $_ }
