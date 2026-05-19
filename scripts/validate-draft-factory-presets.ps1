param(
    [string]$ManifestPath = "Resources/Presets/DraftFactory/factory-bank.draft.json",
    [string]$GeneratedDirectory = "Resources/Presets/DraftFactory/generated",
    [string]$BuilderReportPath = "artifacts/p9d-draft-preset-builder-report.json",
    [string]$JsonReportPath = "artifacts/p9e-draft-preset-gain-staging-report.json",
    [string]$TextReportPath = "artifacts/p9e-draft-preset-gain-staging-report.txt",
    [string]$P9FJsonReportPath = "artifacts/p9f-draft-preset-limiter-telemetry-report.json",
    [string]$P9FTextReportPath = "artifacts/p9f-draft-preset-limiter-telemetry-report.txt",
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

function Ensure-ParentDirectory {
    param([string]$PathValue)
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
        New-Item -Path $parent -ItemType Directory | Out-Null
    }
}

function Add-ResultCheck {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Id,
        [bool]$Passed,
        [string]$Detail,
        [string]$Severity = "FAIL"
    )

    $Checks.Add([pscustomobject]@{
        id = $Id
        passed = $Passed
        severity = $Severity
        detail = $Detail
    })
}

function Get-GateThresholds {
    param([string]$PresetName)

    switch ($PresetName) {
        "Dry Reference" {
            return [pscustomobject]@{
                peakMax = 0.25
                rmsMax = 0.08
                dcMax = 0.005
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "dry_reference_nominal"
            }
        }
        "Clean Studio" {
            return [pscustomobject]@{
                peakMax = 0.50
                rmsMax = 0.18
                dcMax = 0.010
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "clean_nominal_headroom"
            }
        }
        "Classic Crunch" {
            return [pscustomobject]@{
                peakMax = 0.95
                rmsMax = 0.50
                dcMax = 0.020
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "crunch_nominal_bounded"
            }
        }
        "Tight Modern Rhythm" {
            return [pscustomobject]@{
                peakMax = 0.90
                rmsMax = 0.30
                dcMax = 0.020
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "high_gain_staccato_proxy"
            }
        }
        "Wide Ambient Clean" {
            return [pscustomobject]@{
                peakMax = 0.55
                rmsMax = 0.16
                dcMax = 0.010
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "ambient_tail_recovery_proxy"
            }
        }
        "Funk Comp Clean" {
            return [pscustomobject]@{
                peakMax = 0.60
                rmsMax = 0.20
                dcMax = 0.010
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "funk_transient_comp_proxy"
            }
        }
        default {
            return [pscustomobject]@{
                peakMax = 0.95
                rmsMax = 0.50
                dcMax = 0.020
                nearClipMax = 0
                clippedMax = 0
                limiterTouchedMax = 0
                limiterActiveBlocksMax = 0
                sustainedClampBlocksMax = 0
                scenario = "default_nominal"
            }
        }
    }
}

function Test-JsonProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    return $null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]
}

function Get-JsonInt {
    param(
        [object]$Object,
        [string]$Name,
        [int]$DefaultValue = -1
    )

    if (Test-JsonProperty -Object $Object -Name $Name) {
        return [int]$Object.$Name
    }
    return $DefaultValue
}

function Get-JsonDouble {
    param(
        [object]$Object,
        [string]$Name,
        [double]$DefaultValue = [double]::NaN
    )

    if (Test-JsonProperty -Object $Object -Name $Name) {
        return [double]$Object.$Name
    }
    return $DefaultValue
}

$manifestFullPath = Resolve-RepoPath $ManifestPath
$generatedFullPath = Resolve-RepoPath $GeneratedDirectory
$builderReportFullPath = Resolve-RepoPath $BuilderReportPath
$jsonReportFullPath = Resolve-RepoPath $JsonReportPath
$textReportFullPath = Resolve-RepoPath $TextReportPath
$p9fJsonReportFullPath = Resolve-RepoPath $P9FJsonReportPath
$p9fTextReportFullPath = Resolve-RepoPath $P9FTextReportPath

Ensure-ParentDirectory -PathValue $jsonReportFullPath
Ensure-ParentDirectory -PathValue $textReportFullPath
Ensure-ParentDirectory -PathValue $p9fJsonReportFullPath
Ensure-ParentDirectory -PathValue $p9fTextReportFullPath

if (-not (Test-Path $manifestFullPath)) {
    throw "Manifest not found: $ManifestPath"
}

Write-Host "Refreshing P9D draft builder report before P9E validation..."
& (Join-Path $PSScriptRoot "generate-draft-factory-presets.ps1") `
    -ManifestPath $ManifestPath `
    -OutputDirectory $GeneratedDirectory `
    -ReportPath $BuilderReportPath `
    -Configuration $Configuration `
    -Platform $Platform `
    -TimeoutSeconds $TimeoutSeconds

if (-not (Test-Path $builderReportFullPath)) {
    throw "Builder report not found after generation: $BuilderReportPath"
}

$manifestRaw = Get-Content -LiteralPath $manifestFullPath -Raw
$manifest = $manifestRaw | ConvertFrom-Json
$builderReport = Get-Content -LiteralPath $builderReportFullPath -Raw | ConvertFrom-Json

$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]
$presetResults = New-Object System.Collections.Generic.List[object]

if ($manifestRaw -match 'FACTORY_APPROVED') {
    $failures.Add("Manifest contains forbidden FACTORY_APPROVED marker")
}

$manifestPresets = @($manifest.presets)
if ($manifestPresets.Count -ne 6) {
    $failures.Add("Manifest must contain exactly 6 draft presets; found $($manifestPresets.Count)")
}

if ([string]$manifest.shipping -ne "False" -and [bool]$manifest.shipping) {
    $failures.Add("Manifest shipping flag must remain false")
}
if ([bool]$manifest.automaticSeed) {
    $failures.Add("Manifest automaticSeed must remain false")
}
if ([bool]$manifest.userPresetDirectoryWrites) {
    $failures.Add("Manifest userPresetDirectoryWrites must remain false")
}
if ([bool]$manifest.startupPresetPointerUpdate) {
    $failures.Add("Manifest startupPresetPointerUpdate must remain false")
}
if ([int]$manifest.schemaVersionTarget -ne 1) {
    $failures.Add("Manifest schemaVersionTarget must remain 1")
}

$generatedFiles = @()
if (Test-Path $generatedFullPath) {
    $generatedFiles = @(Get-ChildItem -LiteralPath $generatedFullPath -Filter "*.nova-preset" -File -Recurse | ForEach-Object {
        Get-RelativeRepoPath $_.FullName
    } | Sort-Object)
}
if ($generatedFiles.Count -ne 6) {
    $failures.Add("Generated draft folder must contain exactly 6 .nova-preset files; found $($generatedFiles.Count)")
}

$allRepoPresetFiles = @(Get-ChildItem -Path $repoRoot -Filter "*.nova-preset" -File -Recurse | ForEach-Object {
    Get-RelativeRepoPath $_.FullName
})
$outsideGenerated = @($allRepoPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
if ($outsideGenerated.Count -gt 0) {
    $failures.Add("Generated .nova-preset files outside draft generated folder: $($outsideGenerated -join ', ')")
}

$builderStatusOk = [string]$builderReport.status -eq "PASS"
if (-not $builderStatusOk) {
    $failures.Add("P9D builder report status is $($builderReport.status)")
}
if ([bool]$builderReport.wroteUserPresetDirectory) {
    $failures.Add("P9D builder reported user preset directory writes")
}
if ([bool]$builderReport.wroteStartupPresetPointer) {
    $failures.Add("P9D builder reported startup preset pointer writes")
}
if ([bool]$builderReport.changedSchema) {
    $failures.Add("P9D builder reported schema changes")
}

$builderByName = @{}
foreach ($preset in @($builderReport.presets)) {
    $builderByName[[string]$preset.name] = $preset
}

foreach ($preset in $manifestPresets) {
    $name = [string]$preset.name
    $checks = New-Object System.Collections.Generic.List[object]
    $reasons = New-Object System.Collections.Generic.List[string]
    $warns = New-Object System.Collections.Generic.List[string]
    $thresholds = Get-GateThresholds -PresetName $name
    $builderPreset = $builderByName[$name]

    $filePath = Normalize-RelPath([string]$preset.filePath)
    $fileUnderGenerated = $filePath -match '^Resources/Presets/DraftFactory/generated/[^/]+\.nova-preset$'
    Add-ResultCheck -Checks $checks -Id "file_under_generated" -Passed $fileUnderGenerated -Detail $filePath
    if (-not $fileUnderGenerated) {
        $reasons.Add("preset path is outside generated draft folder")
    }

    $fileExists = Test-Path (Resolve-RepoPath $filePath)
    Add-ResultCheck -Checks $checks -Id "file_exists" -Passed $fileExists -Detail $filePath
    if (-not $fileExists) {
        $reasons.Add("generated preset file is missing")
    }

    $manualPending = [string]$preset.manualListeningStatus -eq "pending"
    Add-ResultCheck -Checks $checks -Id "manual_listening_pending" -Passed $manualPending -Detail "manualListeningStatus=$($preset.manualListeningStatus)"
    if (-not $manualPending) {
        $reasons.Add("manual listening status is not pending")
    }

    $distortionStatus = [string]$preset.distortionListeningStatus
    $distortionPending = $distortionStatus -eq "pending" -or $distortionStatus -eq "not_applicable"
    Add-ResultCheck -Checks $checks -Id "distortion_listening_pending" -Passed $distortionPending -Detail "distortionListeningStatus=$distortionStatus"
    if (-not $distortionPending) {
        $reasons.Add("Distortion listening status is not pending/not_applicable")
    }

    $reaperPending = [string]$preset.reaperSmokeStatus -eq "pending"
    Add-ResultCheck -Checks $checks -Id "reaper_pending" -Passed $reaperPending -Detail "reaperSmokeStatus=$($preset.reaperSmokeStatus)"
    if (-not $reaperPending) {
        $reasons.Add("Reaper smoke status is not pending")
    }

    $roundTripPass = $null -ne $builderPreset -and [string]$builderPreset.roundTripStatus -eq "ROUND_TRIP_PASS"
    Add-ResultCheck -Checks $checks -Id "round_trip_pass" -Passed $roundTripPass -Detail "roundTripStatus=$($builderPreset.roundTripStatus)"
    if (-not $roundTripPass) {
        $reasons.Add("round-trip validation did not pass")
    }

    $processPass = $null -ne $builderPreset -and [string]$builderPreset.processStatus -eq "PROCESS_FINITE_PASS"
    Add-ResultCheck -Checks $checks -Id "process_finite_pass" -Passed $processPass -Detail "processStatus=$($builderPreset.processStatus)"
    if (-not $processPass) {
        $reasons.Add("process finite validation did not pass")
    }

    $metrics = if ($null -ne $builderPreset) { $builderPreset.metrics } else { $null }
    $finite = $null -ne $metrics -and [bool]$metrics.finite
    $peak = if ($null -ne $metrics) { [double]$metrics.peak } else { [double]::NaN }
    $rms = if ($null -ne $metrics) { [double]$metrics.rms } else { [double]::NaN }
    $dc = if ($null -ne $metrics) { [double]$metrics.dc } else { [double]::NaN }
    $nearClip = if ($null -ne $metrics) { [int]$metrics.nearClipSamples } else { -1 }
    $clipped = if ($null -ne $metrics) { [int]$metrics.clippedSamples } else { -1 }
    $invalid = if ($null -ne $metrics -and (Test-JsonProperty -Object $metrics -Name "invalidSamples")) { [int]$metrics.invalidSamples } elseif ($finite) { 0 } else { 1 }
    $limiterTouchedSamples = Get-JsonInt -Object $metrics -Name "limiterTouchedSamples" -DefaultValue -1
    $limiterActiveBlocks = Get-JsonInt -Object $metrics -Name "limiterActiveBlocks" -DefaultValue -1
    $sustainedClampBlocks = Get-JsonInt -Object $metrics -Name "sustainedClampBlocks" -DefaultValue -1
    $limiterMaxReductionDb = Get-JsonDouble -Object $metrics -Name "limiterMaxReductionDb"
    $limiterDeltaPeak = Get-JsonDouble -Object $metrics -Name "limiterDeltaPeak"
    $softCeilingTouchedSamples = Get-JsonInt -Object $metrics -Name "softCeilingTouchedSamples" -DefaultValue -1

    Add-ResultCheck -Checks $checks -Id "finite_output" -Passed $finite -Detail "finite=$finite"
    if (-not $finite) {
        $reasons.Add("output contains invalid samples")
    }

    $peakOk = $finite -and $peak -le [double]$thresholds.peakMax
    Add-ResultCheck -Checks $checks -Id "peak_target" -Passed $peakOk -Detail ("peak={0:N8}; max={1:N2}" -f $peak, [double]$thresholds.peakMax)
    if (-not $peakOk) {
        $reasons.Add("peak exceeds P9E target")
    }

    $rmsOk = $finite -and $rms -le [double]$thresholds.rmsMax
    Add-ResultCheck -Checks $checks -Id "rms_target" -Passed $rmsOk -Detail ("rms={0:N8}; max={1:N2}" -f $rms, [double]$thresholds.rmsMax)
    if (-not $rmsOk) {
        $reasons.Add("RMS exceeds P9E target")
    }

    $dcOk = $finite -and $dc -le [double]$thresholds.dcMax
    Add-ResultCheck -Checks $checks -Id "dc_target" -Passed $dcOk -Detail ("dc={0:N8}; max={1:N3}" -f $dc, [double]$thresholds.dcMax)
    if (-not $dcOk) {
        $reasons.Add("DC exceeds P9E target")
    }

    $nearClipOk = $nearClip -ge 0 -and $nearClip -le [int]$thresholds.nearClipMax
    Add-ResultCheck -Checks $checks -Id "near_clip_target" -Passed $nearClipOk -Detail "nearClipSamples=$nearClip; max=$($thresholds.nearClipMax)"
    if (-not $nearClipOk) {
        $reasons.Add("nearClip samples exceed P9E target")
    }

    $clippedOk = $clipped -eq [int]$thresholds.clippedMax
    Add-ResultCheck -Checks $checks -Id "no_clipped_samples" -Passed $clippedOk -Detail "clippedSamples=$clipped"
    if (-not $clippedOk) {
        $reasons.Add("clipped samples detected")
    }

    $invalidOk = $invalid -eq 0
    Add-ResultCheck -Checks $checks -Id "no_invalid_samples" -Passed $invalidOk -Detail "invalidSamples=$invalid"
    if (-not $invalidOk) {
        $reasons.Add("invalid samples detected")
    }

    $directLimiterTelemetryAvailable = $limiterTouchedSamples -ge 0 -and $limiterActiveBlocks -ge 0 -and $sustainedClampBlocks -ge 0
    Add-ResultCheck -Checks $checks -Id "direct_limiter_telemetry_available" -Passed $directLimiterTelemetryAvailable -Detail "limiterTouchedSamples=$limiterTouchedSamples; limiterActiveBlocks=$limiterActiveBlocks; sustainedClampBlocks=$sustainedClampBlocks" -Severity "WARN"

    if ($directLimiterTelemetryAvailable) {
        $limiterTouchedOk = $limiterTouchedSamples -le [int]$thresholds.limiterTouchedMax
        Add-ResultCheck -Checks $checks -Id "limiter_touched_samples_target" -Passed $limiterTouchedOk -Detail "limiterTouchedSamples=$limiterTouchedSamples; max=$($thresholds.limiterTouchedMax)"
        if (-not $limiterTouchedOk) {
            $reasons.Add("limiter touched samples exceed P9F target")
        }

        $limiterActiveOk = $limiterActiveBlocks -le [int]$thresholds.limiterActiveBlocksMax
        Add-ResultCheck -Checks $checks -Id "limiter_active_blocks_target" -Passed $limiterActiveOk -Detail "limiterActiveBlocks=$limiterActiveBlocks; max=$($thresholds.limiterActiveBlocksMax)"
        if (-not $limiterActiveOk) {
            $reasons.Add("limiter active blocks exceed P9F target")
        }

        $sustainedClampOk = $sustainedClampBlocks -le [int]$thresholds.sustainedClampBlocksMax
        Add-ResultCheck -Checks $checks -Id "sustained_clamp_blocks_target" -Passed $sustainedClampOk -Detail "sustainedClampBlocks=$sustainedClampBlocks; max=$($thresholds.sustainedClampBlocksMax)"
        if (-not $sustainedClampOk) {
            $reasons.Add("sustained clamp blocks detected")
        }
    }
    else {
        $sustainedClampProxyOk = $finite -and $nearClip -eq 0 -and $clipped -eq 0 -and $peak -lt 0.98
        Add-ResultCheck -Checks $checks -Id "sustained_clamp_proxy" -Passed $sustainedClampProxyOk -Detail "P9D exposes no direct limiter telemetry; proxy requires finite output, peak<0.98, nearClip=0, clipped=0" -Severity "WARN"
        if (-not $sustainedClampProxyOk) {
            $reasons.Add("sustained clamp proxy failed")
        }
        $warns.Add("Direct limiter telemetry is not available in the P9D report; P9E uses peak/nearClip/clipped proxy gates.")
    }

    $gainStagingPass = $reasons.Count -eq 0
    $technicalReadiness = if ($gainStagingPass) { "LISTENING_CANDIDATE" } else { "NEEDS_GAIN_STAGING_ADJUSTMENT" }
    if (-not $builderStatusOk -or $null -eq $builderPreset) {
        $technicalReadiness = "BLOCKED_TECHNICAL"
    }

    $status = if ($gainStagingPass) {
        if ($warns.Count -gt 0) { "WARN" } else { "PASS" }
    } else {
        "FAIL"
    }

    if ($status -eq "FAIL") {
        $failures.Add("$name failed P9E gain-staging gate: $($reasons -join '; ')")
    }
    elseif ($status -eq "WARN") {
        $warnings.Add("$name passed P9E proxy gates with documented telemetry warning")
    }

    $presetResult = [pscustomobject]@{
        name = $name
        filePath = $filePath
        status = $status
        technicalReadiness = $technicalReadiness
        scenario = $thresholds.scenario
        metrics = [pscustomobject]@{
            finite = $finite
            peak = $peak
            rms = $rms
            dc = $dc
            nearClipSamples = $nearClip
            clippedSamples = $clipped
            invalidSamples = $invalid
            limiterTouchedSamples = if ($limiterTouchedSamples -ge 0) { $limiterTouchedSamples } else { $null }
            limiterActiveBlocks = if ($limiterActiveBlocks -ge 0) { $limiterActiveBlocks } else { $null }
            sustainedClampBlocks = if ($sustainedClampBlocks -ge 0) { $sustainedClampBlocks } else { $null }
            limiterMaxReductionDb = if ([double]::IsNaN($limiterMaxReductionDb)) { $null } else { $limiterMaxReductionDb }
            limiterDeltaPeak = if ([double]::IsNaN($limiterDeltaPeak)) { $null } else { $limiterDeltaPeak }
            softCeilingTouchedSamples = if ($softCeilingTouchedSamples -ge 0) { $softCeilingTouchedSamples } else { $null }
        }
        thresholds = $thresholds
        checks = $checks.ToArray()
        reasons = $reasons.ToArray()
        warnings = $warns.ToArray()
    }
    [void]$presetResults.Add($presetResult)
}

$overallStatus = if ($failures.Count -gt 0) {
    "FAIL"
} elseif ($warnings.Count -gt 0) {
    "WARN"
} else {
    "PASS"
}

function New-ReportPayload {
    param([string]$Phase)

    return [pscustomobject]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    phase = $Phase
    status = $overallStatus
    manifestPath = Normalize-RelPath($ManifestPath)
    builderReportPath = Normalize-RelPath($BuilderReportPath)
    generatedDirectory = Normalize-RelPath($GeneratedDirectory)
    generatedPresetCount = $generatedFiles.Count
    manifestUpdated = $false
    manifestUpdateDecision = "Manifest left unchanged; P9F is a technical recommendation layer and P9D remains the side-effect-free generation writer."
    noUserPresetDirectoryWrites = (-not [bool]$builderReport.wroteUserPresetDirectory)
    noStartupPresetPointerWrites = (-not [bool]$builderReport.wroteStartupPresetPointer)
    changedSchema = [bool]$builderReport.changedSchema
    noFactoryApproved = ($manifestRaw -notmatch 'FACTORY_APPROVED')
    manualListeningStatus = "pending"
    distortionListeningStatus = "pending_for_high_gain_or_not_applicable"
    reaperSmokeStatus = "pending"
    presets = $presetResults.ToArray()
    failures = $failures.ToArray()
    warnings = $warnings.ToArray()
    }
}

$report = New-ReportPayload -Phase "P9E"
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonReportFullPath
$p9fReport = New-ReportPayload -Phase "P9F"
$p9fReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $p9fJsonReportFullPath

function Write-TextReport {
    param(
        [string]$PathValue,
        [string]$Title
    )

    $lines = New-Object System.Collections.Generic.List[string]
$lines.Add($Title)
$lines.Add("Generated: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))")
$lines.Add("status=$overallStatus")
$lines.Add("manifest=$ManifestPath")
$lines.Add("builderReport=$BuilderReportPath")
$lines.Add("generatedPresetCount=$($generatedFiles.Count)")
$lines.Add("manifestUpdated=false")
$lines.Add("noUserPresetDirectoryWrites=$($report.noUserPresetDirectoryWrites)")
$lines.Add("noStartupPresetPointerWrites=$($report.noStartupPresetPointerWrites)")
$lines.Add("changedSchema=$($report.changedSchema)")
$lines.Add("noFactoryApproved=$($report.noFactoryApproved)")
$lines.Add("")
$lines.Add("Per-preset results:")
foreach ($result in $presetResults) {
    $m = $result.metrics
    $lines.Add(("  - {0}: status={1}; readiness={2}; scenario={3}; peak={4:N8}; rms={5:N8}; dc={6:N8}; nearClip={7}; clipped={8}; invalid={9}; limiterTouched={10}; limiterActiveBlocks={11}; sustainedClampBlocks={12}; limiterMaxReductionDb={13}" -f `
        $result.name, $result.status, $result.technicalReadiness, $result.scenario, $m.peak, $m.rms, $m.dc, $m.nearClipSamples, $m.clippedSamples, $m.invalidSamples, $m.limiterTouchedSamples, $m.limiterActiveBlocks, $m.sustainedClampBlocks, $m.limiterMaxReductionDb))
    foreach ($reason in @($result.reasons)) {
        $lines.Add("    FAIL: $reason")
    }
    foreach ($warning in @($result.warnings)) {
        $lines.Add("    WARN: $warning")
    }
}
$lines.Add("")
if ($failures.Count -gt 0) {
    $lines.Add("Failures:")
    foreach ($failure in $failures) {
        $lines.Add("  - $failure")
    }
}
else {
    $lines.Add("Failures: none")
}
if ($warnings.Count -gt 0) {
    $lines.Add("Warnings:")
    foreach ($warning in $warnings) {
        $lines.Add("  - $warning")
    }
}
else {
    $lines.Add("Warnings: none")
}
$lines.Add("")
$lines.Add("Policy: no FACTORY_APPROVED, no final factory approval, manual listening pending, Distortion listening pending, P7F/Reaper pending.")
    $lines | Set-Content -LiteralPath $PathValue
}

Write-TextReport -PathValue $textReportFullPath -Title "NOVA P9E Draft Preset Technical Gain-Staging Report"
Write-TextReport -PathValue $p9fTextReportFullPath -Title "NOVA P9F Draft Preset Direct Limiter Telemetry Report"

Write-Host "P9E draft preset gain-staging status: $overallStatus"
Write-Host "JSON report: $jsonReportFullPath"
Write-Host "Text report: $textReportFullPath"
Write-Host "P9F limiter telemetry JSON report: $p9fJsonReportFullPath"
Write-Host "P9F limiter telemetry text report: $p9fTextReportFullPath"
Write-Host "Manifest updated: false"

if ($failures.Count -gt 0) {
    throw "P9E draft preset gain-staging validation failed. See $JsonReportPath."
}

exit 0
