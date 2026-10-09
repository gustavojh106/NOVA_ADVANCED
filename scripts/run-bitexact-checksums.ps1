param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 300,

    [string]$BaselinePath = "",

    [string]$ReportPath = "",

    [switch]$UpdateBaseline,

    [switch]$SkipBuild
)

# Renders every registered processor with deterministic input and parameter
# automation, hashes the output bits, and compares against a stored baseline.
# A mismatch means the audio output changed. Use it to prove that refactors
# which must not change the sound (roadmap tier T0) are bit-identical.
#
# Hashes depend on the compiler configuration, so Debug and Release keep
# separate baselines.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$configTag = "$($Configuration.ToLowerInvariant())-$($Platform.ToLowerInvariant())"

if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    $BaselinePath = "docs/golden-metrics/t0-bitexact-$configTag-baseline.json"
}

$reportPath = $ReportPath
if ([string]::IsNullOrWhiteSpace($reportPath)) {
    $reportPath = "artifacts/bitexact-$configTag-report.json"
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

if (-not $SkipBuild.IsPresent) {
    Write-Host "Building NOVA Standalone for bit-exact checksums..."
    $buildArgs = @{
        Configuration = $Configuration
        Platform = $Platform
        Target = "NOVA_StandalonePlugin"
    }
    if ($Configuration -eq "Release") {
        Write-Host "Release checksums require an explicit diagnostic build flag."
        $buildArgs.EnableReleaseDiagnostics = $true
    }
    & (Join-Path $PSScriptRoot "build-nova.ps1") @buildArgs
}

$exePath = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/Standalone Plugin/NOVA.exe"
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$env:NOVA_RUN_BITEXACT = "1"
$env:NOVA_BITEXACT_REPORT_PATH = $reportPath

Write-Host "Running bit-exact processor checksums..."
$process = Start-Process -FilePath $exePath -PassThru -WindowStyle Hidden
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

while ((Get-Date) -lt $deadline -and -not (Test-Path $reportPath) -and -not $process.HasExited) {
    Start-Sleep -Milliseconds 500
}

if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
}

Remove-Item Env:NOVA_RUN_BITEXACT -ErrorAction SilentlyContinue
Remove-Item Env:NOVA_BITEXACT_REPORT_PATH -ErrorAction SilentlyContinue

if (-not (Test-Path $reportPath)) {
    throw "Bit-exact report was not written to $reportPath"
}

$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json

$invalid = @($report.results | Where-Object { $_.invalidSamples -gt 0 })
foreach ($result in $invalid) {
    Write-Host ("WARNING: {0} @ {1} Hz produced {2} invalid samples" -f $result.type, $result.sampleRate, $result.invalidSamples)
}

$baselineAbsolutePath = if ([System.IO.Path]::IsPathRooted($BaselinePath)) { $BaselinePath } else { Join-Path $repoRoot $BaselinePath }

if ($UpdateBaseline.IsPresent -or -not (Test-Path $baselineAbsolutePath)) {
    $baselineDir = Split-Path -Parent $baselineAbsolutePath
    if (-not (Test-Path $baselineDir)) {
        New-Item -Path $baselineDir -ItemType Directory | Out-Null
    }
    Copy-Item -LiteralPath $reportPath -Destination $baselineAbsolutePath -Force
    Write-Host ("Bit-exact baseline written to {0} ({1} results)" -f $baselineAbsolutePath, $report.results.Count)
    exit 0
}

$baseline = Get-Content -LiteralPath $baselineAbsolutePath -Raw | ConvertFrom-Json
$baselineByKey = @{}
foreach ($result in $baseline.results) {
    $key = "{0}|{1}|{2}|{3}" -f $result.type, $result.sampleRate, $result.blockSize, $result.stereoInput
    $baselineByKey[$key] = $result.hash
}

$mismatches = @()
$matched = 0
foreach ($result in $report.results) {
    $key = "{0}|{1}|{2}|{3}" -f $result.type, $result.sampleRate, $result.blockSize, $result.stereoInput
    if (-not $baselineByKey.ContainsKey($key)) {
        $mismatches += "MISSING in baseline: $key"
        continue
    }

    if ($baselineByKey[$key] -ne $result.hash) {
        $mismatches += "CHANGED: $key baseline=$($baselineByKey[$key]) current=$($result.hash)"
    }
    else {
        $matched++
    }
}

Write-Host ("Bit-exact comparison: {0} identical, {1} different (baseline {2})" -f $matched, $mismatches.Count, $BaselinePath)

if ($mismatches.Count -gt 0) {
    $mismatches | ForEach-Object { Write-Host "  - $_" }
    throw "Bit-exact checksums differ from baseline. Audio output changed."
}

Write-Host "Bit-exact checksums: PASS (output is bit-identical to baseline)."
