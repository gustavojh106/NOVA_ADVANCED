param(
    [string]$ManifestPath = "Resources/Presets/DraftFactory/factory-bank.draft.json",
    [string]$OutputDirectory = "Resources/Presets/DraftFactory/generated",
    [string]$ReportPath = "artifacts/p9d-draft-preset-builder-report.json",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Normalize-RelPath {
    param([string]$PathValue)
    return ($PathValue -replace '\\', '/')
}

function Resolve-RepoPath {
    param([string]$PathValue)
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }
    return Join-Path $repoRoot $PathValue
}

function Get-RelativeRepoPath {
    param([string]$FullPath)
    $root = [System.IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/')
    $full = [System.IO.Path]::GetFullPath($FullPath)
    if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside repo: $FullPath"
    }
    return Normalize-RelPath($full.Substring($root.Length).TrimStart('\', '/'))
}

function Get-FileSnapshot {
    param([string]$Directory)
    if (-not (Test-Path $Directory)) {
        return @()
    }
    return @(Get-ChildItem -LiteralPath $Directory -File -Recurse | ForEach-Object {
        [pscustomobject]@{
            Path = $_.FullName
            Length = $_.Length
            LastWriteUtc = $_.LastWriteTimeUtc.ToString("o")
        }
    } | Sort-Object Path)
}

function Compare-FileSnapshot {
    param($Before, $After)
    return (ConvertTo-Json @($Before) -Depth 4) -ne (ConvertTo-Json @($After) -Depth 4)
}

$manifestFullPath = Resolve-RepoPath $ManifestPath
$outputFullPath = Resolve-RepoPath $OutputDirectory
$reportFullPath = Resolve-RepoPath $ReportPath
$legacyP9cReportFullPath = Resolve-RepoPath "artifacts/p9c-draft-preset-generator-report.json"

if (-not (Test-Path $manifestFullPath)) {
    throw "Manifest not found: $ManifestPath"
}

$exePath = Join-Path $repoRoot ("Builds/VisualStudio2022/{0}/{1}/Standalone Plugin/NOVA.exe" -f $Platform, $Configuration)
if (-not (Test-Path $exePath)) {
    Write-Host "Standalone executable not found; building $Configuration $Platform..."
    & (Join-Path $PSScriptRoot "build-nova.ps1") -Configuration $Configuration -Platform $Platform -Target "NOVA_StandalonePlugin"
}
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$appDataNova = Join-Path $env:APPDATA "NOVA"
$userPresetDir = Join-Path $appDataNova "Presets"
$startupPointer = Join-Path $appDataNova "startup-preset.txt"
$startupBeforeExists = Test-Path $startupPointer
$startupBefore = if ($startupBeforeExists) { Get-Content -LiteralPath $startupPointer -Raw } else { $null }
$userPresetsBefore = Get-FileSnapshot $userPresetDir

if (Test-Path $reportFullPath) {
    Remove-Item -LiteralPath $reportFullPath -Force
}

$previousEnv = @{
    NOVA_RUN_P9D_DRAFT_BUILDER = $env:NOVA_RUN_P9D_DRAFT_BUILDER
    NOVA_P9D_MANIFEST_PATH = $env:NOVA_P9D_MANIFEST_PATH
    NOVA_P9D_OUTPUT_DIR = $env:NOVA_P9D_OUTPUT_DIR
    NOVA_P9D_REPORT_PATH = $env:NOVA_P9D_REPORT_PATH
}

try {
    $env:NOVA_RUN_P9D_DRAFT_BUILDER = "1"
    $env:NOVA_P9D_MANIFEST_PATH = $manifestFullPath
    $env:NOVA_P9D_OUTPUT_DIR = $outputFullPath
    $env:NOVA_P9D_REPORT_PATH = $reportFullPath

    Write-Host "Running P9D side-effect-free draft preset builder..."
    $process = Start-Process -FilePath $exePath -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline -and -not (Test-Path $reportFullPath) -and -not $process.HasExited) {
        Start-Sleep -Milliseconds 250
    }

    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
finally {
    foreach ($key in $previousEnv.Keys) {
        if ($null -eq $previousEnv[$key]) {
            Remove-Item -Path "env:$key" -ErrorAction SilentlyContinue
        }
        else {
            Set-Item -Path "env:$key" -Value $previousEnv[$key]
        }
    }
}

if (-not (Test-Path $reportFullPath)) {
    throw "P9D builder report was not written to $ReportPath"
}

$startupAfterExists = Test-Path $startupPointer
$startupAfter = if ($startupAfterExists) { Get-Content -LiteralPath $startupPointer -Raw } else { $null }
$userPresetsAfter = Get-FileSnapshot $userPresetDir

$startupChanged = ($startupBeforeExists -ne $startupAfterExists) -or ($startupBefore -ne $startupAfter)
$userPresetsChanged = Compare-FileSnapshot $userPresetsBefore $userPresetsAfter

$report = Get-Content -LiteralPath $reportFullPath -Raw | ConvertFrom-Json
$failures = New-Object System.Collections.Generic.List[string]

if ($report.status -ne "PASS") {
    $failures.Add("C++ builder status is $($report.status)")
}
if ($startupChanged) {
    $failures.Add("startup-preset.txt changed during generation")
}
if ($userPresetsChanged) {
    $failures.Add("User preset directory changed during generation")
}

$generated = @($report.presets | Where-Object { $_.generationStatus -eq "GENERATED_DRAFT" })
foreach ($preset in $generated) {
    $rel = Get-RelativeRepoPath $preset.filePath
    if ($rel -notlike "Resources/Presets/DraftFactory/generated/*.nova-preset") {
        $failures.Add("Generated preset outside allowed draft folder: $rel")
    }
    if (-not (Test-Path (Resolve-RepoPath $rel))) {
        $failures.Add("Generated preset missing: $rel")
    }
    if ($preset.roundTripStatus -ne "ROUND_TRIP_PASS") {
        $failures.Add("$($preset.name) round-trip did not pass")
    }
    if ($preset.processStatus -ne "PROCESS_FINITE_PASS") {
        $failures.Add("$($preset.name) process finite did not pass")
    }
}

if ($failures.Count -eq 0) {
    $manifest = Get-Content -LiteralPath $manifestFullPath -Raw | ConvertFrom-Json
    $generatedRelPaths = @()

    foreach ($preset in @($manifest.presets)) {
        $match = $generated | Where-Object { $_.name -eq $preset.name } | Select-Object -First 1
        if ($null -eq $match) {
            continue
        }

        $rel = Get-RelativeRepoPath $match.filePath
        $preset.filePath = $rel
        $preset.readiness = "DRAFT_TECHNICAL"
        $preset.manualListeningStatus = "pending"
        if ([string]::IsNullOrWhiteSpace([string]$preset.distortionListeningStatus)) {
            $preset.distortionListeningStatus = "not_applicable"
        }
        $preset.reaperSmokeStatus = "pending"
        $generatedRelPaths += $rel
    }

    $manifest | Add-Member -NotePropertyName generatedPresetFiles -NotePropertyValue $generatedRelPaths -Force
    $manifest.generatedBy = "P9D side-effect-free draft preset builder"
    $manifest.notes = @(
        "Internal source-controlled draft manifest.",
        "P9D generated draft .nova-preset files under Resources/Presets/DraftFactory/generated/ only.",
        "Manual listening, Distortion listening, and Reaper smoke remain pending."
    )
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestFullPath
}

$legacyReport = [pscustomobject]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    status = if ($failures.Count -eq 0) { "PASS" } else { "FAIL" }
    phase = "P9D"
    p9dReportPath = Normalize-RelPath($ReportPath)
    generatedPresetCount = $generated.Count
    wroteUserPresetDirectory = $userPresetsChanged
    wroteStartupPresetPointer = $startupChanged
    changedSchema = $false
    failures = @($failures)
}
$legacyReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $legacyP9cReportFullPath

Write-Host "P9D draft preset generation status: $($legacyReport.status)"
Write-Host "Manifest: $manifestFullPath"
Write-Host "Output directory: $outputFullPath"
Write-Host "Report: $reportFullPath"
Write-Host "Generated .nova-preset files: $($generated.Count)"

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    throw "P9D draft preset generator validation failed. See $ReportPath."
}

exit 0
