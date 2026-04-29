param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [int]$TimeoutSeconds = 180,

    [string]$BaselinePath = "docs/golden-metrics/p4-offline-qa-baseline.json",

    [switch]$UpdateBaseline
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$qaReportPath = Join-Path $repoRoot "artifacts/p4-offline-qa-report.txt"
$qaReportDir = Split-Path -Parent $qaReportPath
if (-not (Test-Path $qaReportDir)) {
    New-Item -Path $qaReportDir -ItemType Directory | Out-Null
}
if (Test-Path $qaReportPath) {
    Remove-Item -LiteralPath $qaReportPath -Force
}

Write-Host "Building NOVA Standalone for offline golden metrics..."
& (Join-Path $PSScriptRoot "build-nova.ps1") -Configuration $Configuration -Platform $Platform -Target "NOVA_StandalonePlugin"

$exePath = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/Standalone Plugin/NOVA.exe"
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$env:NOVA_RUN_AUDIO_QA = "1"
$env:NOVA_QA_REPORT_PATH = $qaReportPath

Write-Host "Running offline QA diagnostics for golden metrics..."
$process = Start-Process -FilePath $exePath -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

while ((Get-Date) -lt $deadline -and -not (Test-Path $qaReportPath) -and -not $process.HasExited) {
    Start-Sleep -Milliseconds 500
}

if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
}

if (-not (Test-Path $qaReportPath)) {
    throw "Offline QA report was not written to $qaReportPath"
}

$rawReport = Get-Content -LiteralPath $qaReportPath

$scenarios = @{}
$currentScenario = $null

foreach ($line in $rawReport) {
    if ($line -match '^\[(PASS|FAIL)\]\s+(.+)$') {
        $scenarioName = $Matches[2].Trim()
        $currentScenario = [ordered]@{
            status = $Matches[1]
            notes = ""
            metrics = @{}
        }
        $scenarios[$scenarioName] = $currentScenario
        continue
    }

    if ($null -eq $currentScenario) {
        continue
    }

    if ($line.StartsWith("notes=")) {
        $currentScenario.notes = $line.Substring(6)
        continue
    }

    if ($line -match '^([^=]+)=(.+)$') {
        $metricName = $Matches[1].Trim()
        $metricValueText = $Matches[2].Trim()
        $metricValue = 0.0
        if ([double]::TryParse($metricValueText, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$metricValue)) {
            $currentScenario.metrics[$metricName] = $metricValue
        }
    }
}

$caseDefinitions = @(
    [ordered]@{ case = "output_chain_clean_path"; scenario = "impulse_transparency_line_a"; metrics = @("output_peak", "output_rms", "peak_index_left", "peak_index_right", "first_sample_left", "first_sample_right") },
    [ordered]@{ case = "output_chain_biased_input_dc_cleanup"; scenario = "output_chain_biased_dc_cleanup"; metrics = @("input_peak", "input_dc", "output_peak", "output_dc", "dc_reduction_ratio", "output_rms", "output_early_rms", "output_late_rms", "rms_stability_ratio", "limiterTouchedSamples", "limiterMaxReductionDb", "finite") },
    [ordered]@{ case = "clean_dry_line_a"; scenario = "dry_only_null_test"; metrics = @("null_rms", "input_peak", "output_peak") },
    [ordered]@{ case = "dual_parallel_clean"; scenario = "parallel_noise_unity"; metrics = @("input_rms", "output_rms", "rms_ratio") },
    [ordered]@{ case = "reverb_cloud_tail"; scenario = "reverb_tail_quality"; metrics = @("peak", "rms", "late_rms_0p8_1p3s", "tail_end_rms_last_250ms", "finite") },
    [ordered]@{ case = "reverb_swell"; scenario = "reverb_swell_bloom"; metrics = @("baseline_early_rms", "swelled_early_rms", "baseline_bloom_rms", "swelled_bloom_rms", "finite") },
    [ordered]@{ case = "reverb_reverse"; scenario = "reverb_reverse_bloom"; metrics = @("baseline_early_rms", "reverse_early_rms", "baseline_late_rms", "reverse_late_rms", "finite") },
    [ordered]@{ case = "reverb_reverse_swell"; scenario = "reverb_reverse_swell_combo"; metrics = @("baseline_early_rms", "combo_early_rms", "baseline_late_rms", "combo_late_rms", "finite") },
    [ordered]@{ case = "overdrive_v2_musicalsafe_nominal"; scenario = "overdrive_flagship_recall"; metrics = @("state_size_bytes", "null_rms", "baseline_peak", "recalled_peak", "finite") },
    [ordered]@{ case = "overdrive_cleanamp_reverb_chain_nominal"; scenario = "overdrive_cleanamp_reverb_chain_nominal"; metrics = @("input_peak", "output_peak", "output_rms", "output_dc", "finite", "near_clip_count", "tail_rms", "body_rms", "late_tail_rms", "tail_decay_ratio") }
)

$coverageGaps = @()

$runCases = @()
foreach ($caseDef in $caseDefinitions) {
    $scenarioName = $caseDef.scenario
    if (-not $scenarios.ContainsKey($scenarioName)) {
        $runCases += [ordered]@{
            case = $caseDef.case
            scenario = $scenarioName
            status = "missing-scenario"
            notes = "Scenario not found in offline QA report"
            metrics = [ordered]@{}
        }
        continue
    }

    $scenario = $scenarios[$scenarioName]
    $metricSet = [ordered]@{}
    foreach ($metricName in $caseDef.metrics) {
        if ($scenario.metrics.ContainsKey($metricName)) {
            $metricSet[$metricName] = $scenario.metrics[$metricName]
        }
    }

    $runCases += [ordered]@{
        case = $caseDef.case
        scenario = $scenarioName
        status = $scenario.status
        notes = $scenario.notes
        metrics = $metricSet
    }
}

$generatedAt = Get-Date -Format "yyyy-MM-ddTHH:mm:sszzz"
$payload = [ordered]@{
    generatedAt = $generatedAt
    commit = (git rev-parse HEAD)
    sourceReport = $qaReportPath
    configuration = $Configuration
    platform = $Platform
    cases = $runCases
    coverageGaps = $coverageGaps
}

$baselineAbsolutePath = if ([System.IO.Path]::IsPathRooted($BaselinePath)) { $BaselinePath } else { Join-Path $repoRoot $BaselinePath }
$baselineDir = Split-Path -Parent $baselineAbsolutePath
if (-not (Test-Path $baselineDir)) {
    New-Item -Path $baselineDir -ItemType Directory | Out-Null
}

if ($UpdateBaseline.IsPresent -or -not (Test-Path $baselineAbsolutePath)) {
    $payload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselineAbsolutePath -Encoding UTF8
    Write-Host "Golden metrics baseline written to $baselineAbsolutePath"
    Write-Host "Coverage gaps (tracked):"
    foreach ($gap in $coverageGaps) {
        Write-Host ("  - {0}: {1}" -f $gap.case, $gap.reason)
    }
    exit 0
}

$baseline = Get-Content -LiteralPath $baselineAbsolutePath -Raw | ConvertFrom-Json

function Get-AbsoluteTolerance {
    param(
        [double]$Reference,
        [string]$MetricName
    )

    if ($MetricName -like "peak_index*") { return 0.0 }
    if ($MetricName -eq "finite") { return 0.0 }

    $relativeTolerance = [Math]::Abs($Reference) * 0.08
    $minimumTolerance = 1.0e-4
    return [Math]::Max($relativeTolerance, $minimumTolerance)
}

$baselineByCase = @{}
foreach ($baselineCase in $baseline.cases) {
    $baselineByCase[$baselineCase.case] = $baselineCase
}

$driftFindings = @()

foreach ($runCase in $runCases) {
    if ($runCase.status -ne "PASS") {
        $driftFindings += "Case '$($runCase.case)' scenario '$($runCase.scenario)' returned status=$($runCase.status)"
        continue
    }

    if (-not $baselineByCase.ContainsKey($runCase.case)) {
        $driftFindings += "Case '$($runCase.case)' is missing in baseline file"
        continue
    }

    $baselineCase = $baselineByCase[$runCase.case]
    $baselineMetrics = @{}
    foreach ($metricProperty in $baselineCase.metrics.PSObject.Properties) {
        $baselineMetrics[$metricProperty.Name] = [double]$metricProperty.Value
    }

    foreach ($metricProperty in $runCase.metrics.GetEnumerator()) {
        $metricName = $metricProperty.Key
        $currentValue = [double]$metricProperty.Value

        if (-not $baselineMetrics.ContainsKey($metricName)) {
            $driftFindings += "Case '$($runCase.case)' metric '$metricName' is missing in baseline"
            continue
        }

        $baselineValue = $baselineMetrics[$metricName]
        $tolerance = Get-AbsoluteTolerance -Reference $baselineValue -MetricName $metricName
        $delta = [Math]::Abs($currentValue - $baselineValue)
        if ($delta -gt $tolerance) {
            $driftFindings += "Case '$($runCase.case)' metric '$metricName' drifted by $delta (baseline=$baselineValue current=$currentValue tol=$tolerance)"
        }
    }
}

if ($driftFindings.Count -gt 0) {
    Write-Host "Golden metrics drift detected:"
    $driftFindings | ForEach-Object { Write-Host "  - $_" }
    throw "Golden metrics validation failed. Review $qaReportPath and $baselineAbsolutePath"
}

Write-Host "Golden metrics validation passed against baseline $baselineAbsolutePath"
Write-Host "Coverage gaps (tracked):"
foreach ($gap in $coverageGaps) {
    Write-Host ("  - {0}: {1}" -f $gap.case, $gap.reason)
}
