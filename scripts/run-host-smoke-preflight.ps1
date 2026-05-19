param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 240,

    [string]$OutputRoot = "artifacts/host-smoke-preflight"
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return Join-Path $repoRoot $PathValue
}

function Add-Check {
    param(
        [string]$Id,
        [bool]$Passed,
        [string]$Detail
    )

    $script:checks += [pscustomobject]@{
        id = $Id
        passed = $Passed
        detail = $Detail
    }
}

function Copy-IfExists {
    param(
        [string]$SourcePath,
        [string]$DestinationDirectory
    )

    if (Test-Path -LiteralPath $SourcePath) {
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationDirectory -Force
        return $true
    }

    return $false
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outputRootAbs = Resolve-RepoPath -PathValue $OutputRoot
$runDir = Join-Path $outputRootAbs $timestamp
New-Item -Path $runDir -ItemType Directory -Force | Out-Null

$standalonePath = Join-Path $repoRoot ("Builds/VisualStudio2022/{0}/{1}/Standalone Plugin/NOVA.exe" -f $Platform, $Configuration)
$vst3BundlePath = Join-Path $repoRoot ("Builds/VisualStudio2022/{0}/{1}/VST3/NOVA.vst3" -f $Platform, $Configuration)
$vst3BinaryPath = Join-Path $vst3BundlePath "Contents/x86_64-win/NOVA.vst3"

$checks = @()
Add-Check -Id "standalone_artifact_exists" -Passed (Test-Path -LiteralPath $standalonePath) -Detail $standalonePath
Add-Check -Id "vst3_bundle_exists" -Passed (Test-Path -LiteralPath $vst3BundlePath) -Detail $vst3BundlePath
Add-Check -Id "vst3_binary_exists" -Passed (Test-Path -LiteralPath $vst3BinaryPath) -Detail $vst3BinaryPath

Write-Host "Host smoke preflight"
Write-Host ("Configuration={0} Platform={1}" -f $Configuration, $Platform)
Write-Host ("Output={0}" -f $runDir)

Write-Host "Running Fast audio quality gate before manual host smoke..."
& (Join-Path $PSScriptRoot "run-audio-quality-gates.ps1") `
    -Fast `
    -Configuration $Configuration `
    -Platform $Platform `
    -TimeoutSeconds $TimeoutSeconds

$reportsDir = Join-Path $runDir "reports"
New-Item -Path $reportsDir -ItemType Directory -Force | Out-Null

$reportSources = @(
    "audio-base-test-report.txt",
    "artifacts/audio-thread-policy-scan.txt",
    "artifacts/audio-thread-policy-scan.json",
    "artifacts/rt-profile-release-x64-report.json",
    "artifacts/rt-profile-release-x64-report-gate.json",
    "artifacts/rt-profile-stability-release-x64.json",
    "artifacts/p4-offline-qa-report.txt"
)

foreach ($relative in $reportSources) {
    $source = Join-Path $repoRoot $relative
    [void](Copy-IfExists -SourcePath $source -DestinationDirectory $reportsDir)
}

$appDataLog = Join-Path $env:APPDATA "NOVA/Logs/session-log.txt"
if (Test-Path -LiteralPath $appDataLog) {
    Copy-Item -LiteralPath $appDataLog -Destination (Join-Path $reportsDir "session-log.txt") -Force
}

$summary = [pscustomobject]@{
    generatedAt = (Get-Date).ToString("o")
    repoRoot = $repoRoot
    configuration = $Configuration
    platform = $Platform
    standalonePath = $standalonePath
    vst3BundlePath = $vst3BundlePath
    vst3BinaryPath = $vst3BinaryPath
    checks = $checks
    reportsDirectory = $reportsDir
}

$summaryPath = Join-Path $runDir "host-smoke-preflight-summary.json"
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

$summaryTextPath = Join-Path $runDir "host-smoke-preflight-summary.txt"
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("NOVA Host Smoke Preflight")
$lines.Add("Generated: $($summary.generatedAt)")
$lines.Add("Configuration: $Configuration")
$lines.Add("Platform: $Platform")
$lines.Add("Standalone: $standalonePath")
$lines.Add("VST3 Bundle: $vst3BundlePath")
$lines.Add("VST3 Binary: $vst3BinaryPath")
$lines.Add("")
$lines.Add("Checks:")
foreach ($check in $checks) {
    $lines.Add(("  [{0}] {1}: {2}" -f ($(if ($check.passed) { "PASS" } else { "FAIL" })), $check.id, $check.detail))
}
$lines.Add("")
$lines.Add("Reports copied to: $reportsDir")
$lines | Set-Content -LiteralPath $summaryTextPath -Encoding UTF8

$failedChecks = @($checks | Where-Object { -not $_.passed })
if ($failedChecks.Count -gt 0) {
    Write-Host ("Host smoke preflight failed artifact checks. Summary: {0}" -f $summaryPath)
    exit 1
}

Write-Host ("Host smoke preflight passed. Summary: {0}" -f $summaryPath)
exit 0
