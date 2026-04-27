param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$reportPath = Join-Path $repoRoot "audio-base-test-report.txt"
if (Test-Path $reportPath) {
    Remove-Item -LiteralPath $reportPath -Force
}

Write-Host "Building NOVA Standalone for base audio validation..."
& (Join-Path $PSScriptRoot "build-nova.ps1") -Configuration $Configuration -Platform $Platform -Target "NOVA_StandalonePlugin"

$exePath = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/Standalone Plugin/NOVA.exe"
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$env:NOVA_RUN_AUDIO_TESTS = "1"
$env:NOVA_TEST_REPORT_PATH = $reportPath

Write-Host "Running NOVA validation suite..."
$process = Start-Process -FilePath $exePath -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

while ((Get-Date) -lt $deadline -and -not (Test-Path $reportPath) -and -not $process.HasExited) {
    Start-Sleep -Milliseconds 500
}

if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
}

if (-not (Test-Path $reportPath)) {
    throw "Validation report was not written to $reportPath"
}

$report = Get-Content -LiteralPath $reportPath
$knownPedalFailures = @(
    "PhaserPedal feedback loop rejects DC accumulation under sustained bias",
    "ReverbPedal reverse and swell create a delayed cinematic bloom"
)

$unexpectedFailures = @()
foreach ($line in $report) {
    if (-not $line.StartsWith("FAIL |")) {
        continue
    }

    $isKnownPedalFailure = $false
    foreach ($known in $knownPedalFailures) {
        if ($line.Contains($known)) {
            $isKnownPedalFailure = $true
            break
        }
    }

    if (-not $isKnownPedalFailure) {
        $unexpectedFailures += $line
    }
}

if ($unexpectedFailures.Count -gt 0) {
    Write-Host "Unexpected validation failures:"
    $unexpectedFailures | ForEach-Object { Write-Host $_ }
    throw "Base audio validation failed. See $reportPath"
}

$summary = $report | Select-Object -First 2
$summary | ForEach-Object { Write-Host $_ }
Write-Host "Base audio validation passed. Known pedal-only failures are ignored by this script."
