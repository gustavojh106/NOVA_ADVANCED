param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 240,

    [string]$BaselinePath = "docs/rt-profile/p4b-rt-profile-baseline.json",

    [string]$ReportPath = "",

    [switch]$UpdateBaseline
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

Write-Host "Building NOVA Standalone for RT profile scenarios..."
& (Join-Path $PSScriptRoot "build-nova.ps1") -Configuration $Configuration -Platform $Platform -Target "NOVA_StandalonePlugin"

$exePath = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/Standalone Plugin/NOVA.exe"
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$env:NOVA_RUN_RT_PROFILE = "1"
$env:NOVA_RT_PROFILE_REPORT_PATH = $reportPath

Write-Host "Running RT profile scenarios..."
$process = Start-Process -FilePath $exePath -PassThru
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
