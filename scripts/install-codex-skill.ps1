param(
    [string]$SkillName = "nova-dev"
)

$ErrorActionPreference = "Stop"

Set-Location (Split-Path -Parent $PSScriptRoot)

$source = Join-Path (Get-Location) "tools/codex/$SkillName"
$targetRoot = Join-Path $env:USERPROFILE ".codex/skills"
$target = Join-Path $targetRoot $SkillName

if (-not (Test-Path $source)) {
    throw "Skill source not found at $source"
}

New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null
if (Test-Path $target) {
    Remove-Item -Recurse -Force $target
}

Copy-Item -Recurse -Force $source $target
Write-Host "Installed skill to $target"
