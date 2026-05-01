param(
    [switch]$Fast,
    [switch]$Full,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 240,

    [ValidateRange(1, 50)]
    [int]$StabilityRuns = 3,

    [ValidateSet("Debug", "Release")]
    [string]$ValidationConfiguration = "Debug"
)

$ErrorActionPreference = "Stop"

if ($Fast.IsPresent -and $Full.IsPresent) {
    throw "Use either -Fast or -Full, not both."
}

$mode = if ($Fast.IsPresent) { "Fast" } else { "Full" }

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

Write-Host ("Audio quality gate mode: {0}" -f $mode)
Write-Host ("Configuration={0} Platform={1} TimeoutSeconds={2}" -f $Configuration, $Platform, $TimeoutSeconds)
Write-Host ("ValidationConfiguration={0}" -f $ValidationConfiguration)

Write-Host "Step 1/5: Base validation"
& (Join-Path $PSScriptRoot "run-base-audio-validation.ps1") `
    -Configuration $ValidationConfiguration `
    -Platform $Platform `
    -TimeoutSeconds $TimeoutSeconds

if ($mode -eq "Full") {
    Write-Host "Step 2/5: Golden metrics"
    & (Join-Path $PSScriptRoot "run-golden-audio-metrics.ps1") `
        -Configuration $ValidationConfiguration `
        -Platform $Platform `
        -TimeoutSeconds $TimeoutSeconds
}
else {
    Write-Host "Step 2/5: Golden metrics (skipped in Fast mode)"
}

Write-Host "Step 3/5: RT profile Release single-run"
& (Join-Path $PSScriptRoot "run-rt-profile-scenarios.ps1") `
    -Configuration "Release" `
    -Platform $Platform `
    -TimeoutSeconds $TimeoutSeconds `
    -BaselinePath "docs/rt-profile/p4c-rt-profile-release-baseline.json" `
    -ReportPath "artifacts/rt-profile-release-x64-report-gate.json"

Write-Host "Step 4/5: Audio thread policy scan"
& (Join-Path $PSScriptRoot "check-audio-thread-policy.ps1")

if ($mode -eq "Full") {
    Write-Host "Step 5/5: RT profile Release stability"
    & (Join-Path $PSScriptRoot "run-rt-profile-stability.ps1") `
        -Configuration "Release" `
        -Platform $Platform `
        -Runs $StabilityRuns `
        -TimeoutSeconds $TimeoutSeconds `
        -BaselinePath "docs/rt-profile/p4c-rt-profile-release-baseline.json" `
        -ScenarioFilter @("stress_block_32", "sample_rate_44100", "sample_rate_96000", "overdrive_cleanamp_reverb_chain_nominal") `
        -CiMode `
        -OutputPath "artifacts/rt-profile-stability-release-x64-gate.json"
}
else {
    Write-Host "Step 5/5: RT profile Release stability (skipped in Fast mode)"
}

Write-Host "Audio quality gates completed."
exit 0
