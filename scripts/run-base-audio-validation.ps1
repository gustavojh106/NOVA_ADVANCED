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
function Get-ValidationFailureGroup {
    param(
        [string]$TestName
    )

    if ($TestName -like "P1 *") { return "P1 Pedal Safety" }
    if ($TestName -like "Reverb*") { return "Reverb" }
    if ($TestName -like "OutputChain*") { return "OutputChain" }
    if ($TestName -like "AudioEngine*") { return "AudioEngine" }
    if ($TestName -like "InputChain*" -or $TestName -like "ChannelStrip*" -or $TestName -like "Processor switcher*") { return "Routing" }
    if ($TestName -like "Global processors*" -or $TestName -like "TunerService*") { return "Core" }
    return "Regression"
}

$allFailures = @()
$failureGroups = @{
    "Core" = 0
    "P1 Pedal Safety" = 0
    "Reverb" = 0
    "Routing" = 0
    "OutputChain" = 0
    "AudioEngine" = 0
    "Regression" = 0
}

foreach ($line in $report) {
    if (-not $line.StartsWith("FAIL |")) { continue }

    $allFailures += $line
    $parts = $line -split "\|"
    $testName = if ($parts.Count -ge 2) { $parts[1].Trim() } else { "" }
    $group = Get-ValidationFailureGroup -TestName $testName
    if (-not $failureGroups.ContainsKey($group)) { $group = "Regression" }
    $failureGroups[$group] += 1
}

if ($allFailures.Count -gt 0) {
    Write-Host "Validation failures by group:"
    foreach ($groupName in @("Core", "P1 Pedal Safety", "Reverb", "Routing", "OutputChain", "AudioEngine", "Regression")) {
        Write-Host ("  [{0}] {1}" -f $groupName, $failureGroups[$groupName])
    }
    Write-Host ""
    Write-Host "Validation failures:"
    $allFailures | ForEach-Object { Write-Host $_ }
    throw "Base audio validation failed. See $reportPath"
}

$summary = $report | Select-Object -First 2
$summary | ForEach-Object { Write-Host $_ }
Write-Host "Validation failures by group:"
foreach ($groupName in @("Core", "P1 Pedal Safety", "Reverb", "Routing", "OutputChain", "AudioEngine", "Regression")) {
    Write-Host ("  [{0}] {1}" -f $groupName, $failureGroups[$groupName])
}
Write-Host "Base audio validation passed. No known failures are ignored by this script."
