param(
    [string]$OutputPath = "artifacts/diagnostics-bundle.json",
    [string]$RtProfileReportPath = "artifacts/rt-profile-release-x64-report.json",
    [string]$RtStabilityReportPath = "artifacts/rt-profile-stability-release-x64.json",
    [string]$PolicyScanJsonPath = "artifacts/audio-thread-policy-scan.json",
    [string]$BaseValidationReportPath = "audio-base-test-report.txt",
    [switch]$FailOnMissing
)

# P7I diagnostics bundle aggregator. Read-only over existing artifacts; never
# mutates DSP, schema, golden baselines, or audio-thread state. Safe to run any
# time after the underlying gates have been executed.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Resolve-RootedPath {
    param([string]$PathValue)
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }
    return Join-Path $repoRoot $PathValue
}

function Read-JsonIfExists {
    param([string]$PathValue)
    if (-not (Test-Path $PathValue)) {
        return $null
    }
    try {
        return Get-Content -LiteralPath $PathValue -Raw | ConvertFrom-Json
    }
    catch {
        return $null
    }
}

function Get-FileMetadata {
    param([string]$PathValue)
    if (-not (Test-Path $PathValue)) {
        return [pscustomobject]@{
            path = $PathValue
            exists = $false
            sizeBytes = 0
            lastWriteUtc = $null
        }
    }

    $item = Get-Item -LiteralPath $PathValue
    return [pscustomobject]@{
        path = $PathValue
        exists = $true
        sizeBytes = $item.Length
        lastWriteUtc = $item.LastWriteTimeUtc.ToString("o")
    }
}

$outputAbs = Resolve-RootedPath $OutputPath
$rtReportAbs = Resolve-RootedPath $RtProfileReportPath
$rtStabilityAbs = Resolve-RootedPath $RtStabilityReportPath
$policyJsonAbs = Resolve-RootedPath $PolicyScanJsonPath
$baseValidationAbs = Resolve-RootedPath $BaseValidationReportPath

$outputDir = Split-Path -Parent $outputAbs
if (-not [string]::IsNullOrWhiteSpace($outputDir) -and -not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory | Out-Null
}

$rtReport = Read-JsonIfExists $rtReportAbs
$rtStability = Read-JsonIfExists $rtStabilityAbs
$policyScan = Read-JsonIfExists $policyJsonAbs

$rtSummary = $null
if ($null -ne $rtReport -and $null -ne $rtReport.summary) {
    $rtSummary = [pscustomobject]@{
        total = $rtReport.summary.total
        pass = $rtReport.summary.pass
        warn = $rtReport.summary.warn
        fail = $rtReport.summary.fail
    }
}

$rtStabilitySummary = $null
if ($null -ne $rtStability) {
    $runCount = 0
    $passRuns = 0
    $warnRuns = 0
    $failRuns = 0
    $totalScenariosPass = 0
    $totalScenariosWarn = 0
    $totalScenariosFail = 0
    if ($null -ne $rtStability.runReports) {
        foreach ($run in $rtStability.runReports) {
            $runCount++
            if ($null -ne $run.summary) {
                $sPass = if ($null -ne $run.summary.pass) { [int]$run.summary.pass } else { 0 }
                $sWarn = if ($null -ne $run.summary.warn) { [int]$run.summary.warn } else { 0 }
                $sFail = if ($null -ne $run.summary.fail) { [int]$run.summary.fail } else { 0 }
                $totalScenariosPass += $sPass
                $totalScenariosWarn += $sWarn
                $totalScenariosFail += $sFail
                if ($sFail -gt 0) {
                    $failRuns++
                }
                elseif ($sWarn -gt 0) {
                    $warnRuns++
                }
                else {
                    $passRuns++
                }
            }
        }
    }
    $blockingEvents = 0
    if ($null -ne $rtStability.gateSummary -and $null -ne $rtStability.gateSummary.blockingEvents) {
        $blockingEvents = @($rtStability.gateSummary.blockingEvents).Count
    }
    $rtStabilitySummary = [pscustomobject]@{
        runs = $runCount
        passRuns = $passRuns
        warnRuns = $warnRuns
        failRuns = $failRuns
        totalScenariosPass = $totalScenariosPass
        totalScenariosWarn = $totalScenariosWarn
        totalScenariosFail = $totalScenariosFail
        blockingEvents = $blockingEvents
    }
}

$policySummary = $null
if ($null -ne $policyScan) {
    $summaryNode = $policyScan.summary
    $policySummary = [pscustomobject]@{
        status = $policyScan.status
        failures = if ($null -ne $summaryNode) { $summaryNode.failures } else { $null }
        warnings = if ($null -ne $summaryNode) { $summaryNode.warnings } else { $null }
        legacyWarnings = if ($null -ne $summaryNode) { $summaryNode.legacyWarnings } else { $null }
        legacyQuarantined = if ($null -ne $summaryNode) { $summaryNode.legacyQuarantined } else { $null }
        contractFailures = if ($null -ne $summaryNode) { $summaryNode.contractFailures } else { $null }
        contractChecks = if ($null -ne $summaryNode) { $summaryNode.contractChecks } else { $null }
    }
}

$baseValidationSummary = $null
if (Test-Path $baseValidationAbs) {
    $baseValidationContent = Get-Content -LiteralPath $baseValidationAbs -Raw
    $resultsMatch = [regex]::Match($baseValidationContent, 'results\s*=\s*(\d+)')
    $passesMatch = [regex]::Match($baseValidationContent, 'passes\s*=\s*(\d+)')
    $failuresMatch = [regex]::Match($baseValidationContent, 'failures\s*=\s*(\d+)')
    $failingResultsMatch = [regex]::Match($baseValidationContent, 'failingResults\s*=\s*(\d+)')

    $baseValidationSummary = [pscustomobject]@{
        results = if ($resultsMatch.Success) { [int]$resultsMatch.Groups[1].Value } else { $null }
        passes = if ($passesMatch.Success) { [int]$passesMatch.Groups[1].Value } else { $null }
        failures = if ($failuresMatch.Success) { [int]$failuresMatch.Groups[1].Value } else { $null }
        failingResults = if ($failingResultsMatch.Success) { [int]$failingResultsMatch.Groups[1].Value } else { $null }
    }
}

$missing = @()
foreach ($entry in @(
    [pscustomobject]@{ name = "rtProfileReport"; path = $rtReportAbs },
    [pscustomobject]@{ name = "rtStabilityReport"; path = $rtStabilityAbs },
    [pscustomobject]@{ name = "policyScanJson"; path = $policyJsonAbs },
    [pscustomobject]@{ name = "baseValidationReport"; path = $baseValidationAbs }
)) {
    if (-not (Test-Path $entry.path)) {
        $missing += $entry
    }
}

$bundle = [pscustomobject]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    kind = "p7i_diagnostics_bundle"
    sources = [pscustomobject]@{
        rtProfileReport = Get-FileMetadata $rtReportAbs
        rtStabilityReport = Get-FileMetadata $rtStabilityAbs
        policyScanJson = Get-FileMetadata $policyJsonAbs
        baseValidationReport = Get-FileMetadata $baseValidationAbs
    }
    rtProfile = $rtSummary
    rtStability = $rtStabilitySummary
    policyScan = $policySummary
    baseValidation = $baseValidationSummary
    missing = $missing
}

$json = $bundle | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($outputAbs, $json)

Write-Host ("Diagnostics bundle written to {0}" -f $outputAbs)

if ($null -ne $rtSummary) {
    Write-Host ("RT profile: total={0} pass={1} warn={2} fail={3}" -f `
        $rtSummary.total, $rtSummary.pass, $rtSummary.warn, $rtSummary.fail)
}
if ($null -ne $rtStabilitySummary) {
    Write-Host ("RT stability: runs={0} passRuns={1} warnRuns={2} failRuns={3} blockingEvents={4}" -f `
        $rtStabilitySummary.runs, $rtStabilitySummary.passRuns, $rtStabilitySummary.warnRuns, $rtStabilitySummary.failRuns, $rtStabilitySummary.blockingEvents)
}
if ($null -ne $policySummary) {
    Write-Host ("Policy scan: status={0} failures={1} contractFailures={2} contractChecks={3}" -f `
        $policySummary.status, $policySummary.failures, $policySummary.contractFailures, $policySummary.contractChecks)
}
if ($null -ne $baseValidationSummary) {
    Write-Host ("Base validation: results={0} passes={1} failures={2} failingResults={3}" -f `
        $baseValidationSummary.results, $baseValidationSummary.passes, $baseValidationSummary.failures, $baseValidationSummary.failingResults)
}

if ($missing.Count -gt 0) {
    Write-Host "Missing inputs:"
    foreach ($entry in $missing) {
        Write-Host ("  - {0}: {1}" -f $entry.name, $entry.path)
    }
    if ($FailOnMissing.IsPresent) {
        throw "Diagnostics bundle is missing one or more inputs. Run the underlying gates first."
    }
}
