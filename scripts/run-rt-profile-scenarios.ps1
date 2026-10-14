param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 240,

    [string]$BaselinePath = "docs/rt-profile/p4b-rt-profile-baseline.json",

    [string]$ReportPath = "",

    [switch]$UpdateBaseline,

    # Diagnostics (see "Procedimiento de medición del rt-profile" in docs/latency-optimization-roadmap.md).
    # Comma-separated scenario names; '*' wildcards allowed. Empty = all.
    [string]$Only = "",

    # Run every selected scenario N times back to back in the same process.
    [ValidateRange(1, 100)]
    [int]$Repeat = 1,

    # Write the time of every warm-up and measured block to this CSV.
    [string]$BlockCsvPath = "",

    # Add diag_* scenarios that time each cabinet directly, with its convolution
    # partitions aligned and misaligned with the host blocks.
    [switch]$CabinetProbe,

    # Warm-up used before B-20 (64-sample setup blocks, no settle waits), to reproduce older reports.
    [switch]$LegacyWarmUp,

    # Reuse the existing executable (it must have been built by this script for the same configuration).
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$reportPath = $ReportPath
if ([string]::IsNullOrWhiteSpace($reportPath)) {
    if ($Configuration -eq "Debug" -and $Platform -eq "x64") {
        $reportPath = "artifacts/p4b-rt-profile-report.json"
    }
    else {
        $reportPath = "artifacts/rt-profile-$($Configuration.ToLowerInvariant())-$($Platform.ToLowerInvariant())-report.json"
    }
}

if (-not [System.IO.Path]::IsPathRooted($reportPath)) {
    $reportPath = Join-Path $repoRoot $reportPath
}

$reportDir = Split-Path -Parent $reportPath
if (-not (Test-Path $reportDir)) {
    New-Item -Path $reportDir -ItemType Directory | Out-Null
}
if (Test-Path $reportPath) {
    Remove-Item -LiteralPath $reportPath -Force
}

if ($SkipBuild.IsPresent) {
    Write-Host "Skipping build; reusing the existing executable."
}
else {
    Write-Host "Building NOVA Standalone for RT profile scenarios..."
    $buildArgs = @{
        Configuration = $Configuration
        Platform = $Platform
        Target = "NOVA_StandalonePlugin"
    }
    if ($Configuration -eq "Release") {
        Write-Host "Release RT profiling requires an explicit diagnostic build flag."
        $buildArgs.EnableReleaseDiagnostics = $true
    }
    & (Join-Path $PSScriptRoot "build-nova.ps1") @buildArgs
}

$exePath = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/Standalone Plugin/NOVA.exe"
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$env:NOVA_RUN_RT_PROFILE = "1"
$env:NOVA_RT_PROFILE_REPORT_PATH = $reportPath

function Set-OptionalEnv([string]$Name, [string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        Remove-Item "Env:\$Name" -ErrorAction SilentlyContinue
    }
    else {
        Set-Item "Env:\$Name" $Value
    }
}

$blockCsvAbsolutePath = ""
if (-not [string]::IsNullOrWhiteSpace($BlockCsvPath)) {
    $blockCsvAbsolutePath = if ([System.IO.Path]::IsPathRooted($BlockCsvPath)) { $BlockCsvPath } else { Join-Path $repoRoot $BlockCsvPath }
    if (Test-Path $blockCsvAbsolutePath) {
        Remove-Item -LiteralPath $blockCsvAbsolutePath -Force
    }
}

Set-OptionalEnv "NOVA_RT_PROFILE_ONLY" $Only
Set-OptionalEnv "NOVA_RT_PROFILE_REPEAT" $(if ($Repeat -gt 1) { "$Repeat" } else { "" })
Set-OptionalEnv "NOVA_RT_PROFILE_BLOCK_CSV" $blockCsvAbsolutePath
Set-OptionalEnv "NOVA_RT_PROFILE_CAB_PROBE" $(if ($CabinetProbe.IsPresent) { "1" } else { "" })
Set-OptionalEnv "NOVA_RT_PROFILE_LEGACY_WARMUP" $(if ($LegacyWarmUp.IsPresent) { "1" } else { "" })

Write-Host "Running RT profile scenarios..."
$process = Start-Process -FilePath $exePath -PassThru -WindowStyle Hidden
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

while ((Get-Date) -lt $deadline -and -not (Test-Path $reportPath) -and -not $process.HasExited) {
    Start-Sleep -Milliseconds 500
}

if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
}

if (-not (Test-Path $reportPath)) {
    throw "RT profile report was not written to $reportPath"
}

$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json

Write-Host ("RT profile summary: total={0} pass={1} warn={2} fail={3}" -f `
    $report.summary.total, $report.summary.pass, $report.summary.warn, $report.summary.fail)

if ($null -ne $report.procedure) {
    Write-Host ("RT profile procedure: warmUp={0} engineSettleMs={1} cabinetSettleMs={2} repeat={3} only='{4}'" -f `
        $report.procedure.warmUp, $report.procedure.engineSettleMs, $report.procedure.cabinetSettleMs, $report.procedure.repeat, $report.procedure.only)
}

foreach ($scenario in $report.scenarios) {
    Write-Host ("[{0}] {1}: sr={2} block={3} blocks={4} avgMs={5:N4} peakMs={6:N4} cpuAvg={7:N2}% cpuPeak={8:N2}% maxBudget={9:N3}" -f `
        $scenario.status,
        $scenario.name,
        $scenario.sampleRate,
        $scenario.blockSize,
        $scenario.processedBlocks,
        $scenario.avgProcessMs,
        $scenario.peakProcessMs,
        $scenario.cpuAvgPercent,
        $scenario.cpuPeakPercent,
        $scenario.maxBudgetRatio)

    if (-not [string]::IsNullOrWhiteSpace($scenario.warnings)) {
        Write-Host ("  warnings: {0}" -f $scenario.warnings)
    }
}

if ($report.summary.fail -gt 0) {
    throw "RT profile scenarios reported FAIL. See $reportPath"
}

$baselineAbsolutePath = if ([System.IO.Path]::IsPathRooted($BaselinePath)) { $BaselinePath } else { Join-Path $repoRoot $BaselinePath }
$baselineDir = Split-Path -Parent $baselineAbsolutePath
if (-not (Test-Path $baselineDir)) {
    New-Item -Path $baselineDir -ItemType Directory | Out-Null
}

if ($UpdateBaseline.IsPresent -or -not (Test-Path $baselineAbsolutePath)) {
    Copy-Item -LiteralPath $reportPath -Destination $baselineAbsolutePath -Force
    Write-Host "RT profile baseline written to $baselineAbsolutePath"
}
else {
    $baseline = Get-Content -LiteralPath $baselineAbsolutePath -Raw | ConvertFrom-Json
    $baselineByName = @{}
    foreach ($scenario in $baseline.scenarios) {
        $baselineByName[$scenario.name] = $scenario
    }

    $comparisonWarnings = @()
    foreach ($scenario in $report.scenarios) {
        if (-not $baselineByName.ContainsKey($scenario.name)) {
            $comparisonWarnings += "Scenario '$($scenario.name)' is missing from baseline"
            continue
        }

        $baselineScenario = $baselineByName[$scenario.name]
        if ($baselineScenario.cpuAvgPercent -gt 0 -and $scenario.cpuAvgPercent -gt [Math]::Max($baselineScenario.cpuAvgPercent * 1.75, $baselineScenario.cpuAvgPercent + 10.0)) {
            $comparisonWarnings += "Scenario '$($scenario.name)' avg CPU rose from $($baselineScenario.cpuAvgPercent) to $($scenario.cpuAvgPercent)"
        }
    }

    if ($comparisonWarnings.Count -gt 0) {
        Write-Host "Baseline comparison warnings:"
        $comparisonWarnings | ForEach-Object { Write-Host "  - $_" }
    }
}

Write-Host "RT profile scenarios completed."
