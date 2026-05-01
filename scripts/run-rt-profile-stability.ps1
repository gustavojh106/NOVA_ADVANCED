param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [ValidateRange(1, 50)]
    [int]$Runs = 5,

    [int]$TimeoutSeconds = 240,

    [string]$BaselinePath = "docs/rt-profile/p4b-rt-profile-baseline.json",

    [string]$OutputPath = "",

    [string[]]$ScenarioFilter = @(),

    [ValidateRange(0, 10000)]
    [int]$PauseMilliseconds = 500,

    [switch]$CiMode,
    [switch]$NightlyMode,

    [bool]$FailOnReleaseWarn = $false,
    [bool]$FailOnReleaseFail = $true,
    [bool]$AllowDebugWarn = $true,
    [bool]$AllowDebugFail = $false
)

$ErrorActionPreference = "Stop"

function Get-Stats {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return [pscustomobject]@{
            min = $null
            median = $null
            max = $null
        }
    }

    $sorted = $Values | Sort-Object
    $count = $sorted.Count
    if ($count % 2 -eq 1) {
        $median = $sorted[[int](($count - 1) / 2)]
    }
    else {
        $upperIndex = [int]($count / 2)
        $lowerIndex = $upperIndex - 1
        $median = ($sorted[$lowerIndex] + $sorted[$upperIndex]) / 2.0
    }

    return [pscustomobject]@{
        min = [double]$sorted[0]
        median = [double]$median
        max = [double]$sorted[$count - 1]
    }
}

function Should-BlockRun {
    param(
        [string]$ConfigurationName,
        [int]$WarnCount,
        [int]$FailCount,
        [bool]$FailOnReleaseWarnValue,
        [bool]$FailOnReleaseFailValue,
        [bool]$AllowDebugWarnValue,
        [bool]$AllowDebugFailValue
    )

    $reasons = New-Object System.Collections.Generic.List[string]

    if ($ConfigurationName -eq "Release") {
        if ($FailCount -gt 0 -and $FailOnReleaseFailValue) {
            $reasons.Add("Release run has FAIL and -FailOnReleaseFail is enabled.")
        }
        if ($WarnCount -gt 0 -and $FailOnReleaseWarnValue) {
            $reasons.Add("Release run has WARN and -FailOnReleaseWarn is enabled.")
        }
    }
    else {
        if ($FailCount -gt 0 -and -not $AllowDebugFailValue) {
            $reasons.Add("Debug run has FAIL and -AllowDebugFail is disabled.")
        }
        if ($WarnCount -gt 0 -and -not $AllowDebugWarnValue) {
            $reasons.Add("Debug run has WARN and -AllowDebugWarn is disabled.")
        }
    }

    return $reasons
}

if ($CiMode.IsPresent -and $NightlyMode.IsPresent) {
    throw "Use either -CiMode or -NightlyMode, not both."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

# Accept both repeated -ScenarioFilter args and a single comma-separated value.
$normalizedScenarioFilter = @()
foreach ($entry in $ScenarioFilter) {
    if ([string]::IsNullOrWhiteSpace($entry)) {
        continue
    }

    $normalizedScenarioFilter += ($entry -split ',') | ForEach-Object { $_.Trim() } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
}
$ScenarioFilter = $normalizedScenarioFilter | Sort-Object -Unique

# Mode presets: preserve standalone defaults unless mode is explicitly requested.
if ($CiMode.IsPresent) {
    if (-not $PSBoundParameters.ContainsKey("Runs")) {
        $Runs = 3
    }

    if ($Configuration -eq "Release") {
        $FailOnReleaseWarn = $true
        $FailOnReleaseFail = $true
    }
}

if ($NightlyMode.IsPresent) {
    if (-not $PSBoundParameters.ContainsKey("Runs")) {
        $Runs = 5
    }

    if ($Configuration -eq "Release") {
        $FailOnReleaseWarn = $true
        $FailOnReleaseFail = $true
    }
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = "artifacts/rt-profile-stability-$($Configuration.ToLowerInvariant())-$($Platform.ToLowerInvariant()).json"
}

$outputAbsolutePath = if ([System.IO.Path]::IsPathRooted($OutputPath)) { $OutputPath } else { Join-Path $repoRoot $OutputPath }
$outputDir = Split-Path -Parent $outputAbsolutePath
if (-not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory | Out-Null
}

$runDirName = "rt-profile-stability-runs-$($Configuration.ToLowerInvariant())-$($Platform.ToLowerInvariant())"
$runReportsDir = Join-Path $repoRoot "artifacts/$runDirName"
if (-not (Test-Path $runReportsDir)) {
    New-Item -Path $runReportsDir -ItemType Directory | Out-Null
}

Write-Host ("Stability gate policy: configuration={0} ciMode={1} nightlyMode={2} failOnReleaseWarn={3} failOnReleaseFail={4} allowDebugWarn={5} allowDebugFail={6}" -f `
    $Configuration,
    $CiMode.IsPresent,
    $NightlyMode.IsPresent,
    $FailOnReleaseWarn,
    $FailOnReleaseFail,
    $AllowDebugWarn,
    $AllowDebugFail)

$blockingEvents = New-Object System.Collections.Generic.List[string]
$runReports = @()

for ($runIndex = 1; $runIndex -le $Runs; $runIndex++) {
    $perRunReportPath = Join-Path $runReportsDir ("run-{0:D2}.json" -f $runIndex)
    if (Test-Path $perRunReportPath) {
        Remove-Item -LiteralPath $perRunReportPath -Force
    }

    Write-Host ("Stability run {0}/{1}: configuration={2} platform={3}" -f $runIndex, $Runs, $Configuration, $Platform)

    $invocationError = $null
    try {
        & (Join-Path $PSScriptRoot "run-rt-profile-scenarios.ps1") `
            -Configuration $Configuration `
            -Platform $Platform `
            -TimeoutSeconds $TimeoutSeconds `
            -BaselinePath $BaselinePath `
            -ReportPath $perRunReportPath
    }
    catch {
        $invocationError = $_
    }

    if (-not (Test-Path $perRunReportPath)) {
        if ($null -ne $invocationError) {
            throw "Run $runIndex did not produce a report. Invocation error: $($invocationError.Exception.Message)"
        }
        throw "Run $runIndex did not produce a report at $perRunReportPath."
    }

    $report = Get-Content -LiteralPath $perRunReportPath -Raw | ConvertFrom-Json
    $runFailCount = [int]$report.summary.fail
    $runWarnCount = [int]$report.summary.warn

    $effectiveGateFailCount = $runFailCount
    $effectiveGateWarnCount = $runWarnCount
    if ($ScenarioFilter.Count -gt 0) {
        $selectedForGate = $report.scenarios | Where-Object { $_.name -in $ScenarioFilter }
        if ($selectedForGate.Count -gt 0) {
            $effectiveGateFailCount = ($selectedForGate | Where-Object { $_.status -eq "FAIL" }).Count
            $effectiveGateWarnCount = ($selectedForGate | Where-Object { $_.status -eq "WARN" }).Count
        }
    }

    $gateReasons = Should-BlockRun `
        -ConfigurationName $Configuration `
        -WarnCount $effectiveGateWarnCount `
        -FailCount $effectiveGateFailCount `
        -FailOnReleaseWarnValue $FailOnReleaseWarn `
        -FailOnReleaseFailValue $FailOnReleaseFail `
        -AllowDebugWarnValue $AllowDebugWarn `
        -AllowDebugFailValue $AllowDebugFail

    if ($null -ne $invocationError) {
        $message = $invocationError.Exception.Message
        $isExpectedFailThrow = ($runFailCount -gt 0 -and $message -like "RT profile scenarios reported FAIL*")
        if (-not $isExpectedFailThrow) {
            $gateReasons.Add("Runner invocation error: $message")
        }
    }

    foreach ($reason in $gateReasons) {
        $blockingEvents.Add("run=$runIndex reason=$reason")
    }

    $runReports += [pscustomobject]@{
        run = $runIndex
        path = $perRunReportPath
        summary = $report.summary
        scenarios = $report.scenarios
        gate = [pscustomobject]@{
            blocked = ($gateReasons.Count -gt 0)
            reasons = $gateReasons
            evaluatedWarnCount = $effectiveGateWarnCount
            evaluatedFailCount = $effectiveGateFailCount
            invocationError = if ($null -eq $invocationError) { "" } else { $invocationError.Exception.Message }
        }
    }

    if ($runIndex -lt $Runs -and $PauseMilliseconds -gt 0) {
        # Small pause to reduce process launch contention when collecting repeated runs.
        Start-Sleep -Milliseconds $PauseMilliseconds
    }
}

if ($runReports.Count -eq 0) {
    throw "No RT profile runs were collected."
}

$scenarioNames = @()
foreach ($scenario in $runReports[0].scenarios) {
    $scenarioNames += $scenario.name
}

$missingScenarioFilters = @()
if ($ScenarioFilter.Count -gt 0) {
    $missingScenarioFilters = $ScenarioFilter | Where-Object { $_ -notin $scenarioNames }
    $scenarioNames = $scenarioNames | Where-Object { $_ -in $ScenarioFilter }
}

$scenarioStats = @()
foreach ($scenarioName in $scenarioNames) {
    $samples = @()
    foreach ($runReport in $runReports) {
        $scenario = $runReport.scenarios | Where-Object { $_.name -eq $scenarioName } | Select-Object -First 1
        if ($null -eq $scenario) {
            continue
        }

        $samples += [pscustomobject]@{
            run = $runReport.run
            status = $scenario.status
            maxBudgetRatio = [double]$scenario.maxBudgetRatio
            cpuAvgPercent = [double]$scenario.cpuAvgPercent
            cpuPeakPercent = [double]$scenario.cpuPeakPercent
            peakProcessMs = [double]$scenario.peakProcessMs
        }
    }

    if ($samples.Count -eq 0) {
        continue
    }

    $statuses = $samples | ForEach-Object { $_.status }
    $scenarioStats += [pscustomobject]@{
        name = $scenarioName
        runCount = $samples.Count
        statusCounts = [pscustomobject]@{
            pass = ($statuses | Where-Object { $_ -eq "PASS" }).Count
            warn = ($statuses | Where-Object { $_ -eq "WARN" }).Count
            fail = ($statuses | Where-Object { $_ -eq "FAIL" }).Count
        }
        maxBudgetRatio = Get-Stats -Values ($samples | ForEach-Object { $_.maxBudgetRatio })
        cpuAvgPercent = Get-Stats -Values ($samples | ForEach-Object { $_.cpuAvgPercent })
        cpuPeakPercent = Get-Stats -Values ($samples | ForEach-Object { $_.cpuPeakPercent })
        peakProcessMs = Get-Stats -Values ($samples | ForEach-Object { $_.peakProcessMs })
    }
}

$runStatusSummaries = $runReports | ForEach-Object {
    [pscustomobject]@{
        run = $_.run
        pass = [int]$_.summary.pass
        warn = [int]$_.summary.warn
        fail = [int]$_.summary.fail
        blocked = [bool]$_.gate.blocked
    }
}

$runsWithFail = ($runStatusSummaries | Where-Object { $_.fail -gt 0 }).Count
$runsWithWarn = ($runStatusSummaries | Where-Object { $_.warn -gt 0 }).Count
$runsBlocked = ($runStatusSummaries | Where-Object { $_.blocked }).Count

$stabilitySummary = [pscustomobject]@{
    generatedAt = (Get-Date).ToString("o")
    configuration = $Configuration
    platform = $Platform
    runs = $Runs
    timeoutSeconds = $TimeoutSeconds
    pauseMilliseconds = $PauseMilliseconds
    scenarioFilter = $ScenarioFilter
    missingScenarioFilters = $missingScenarioFilters
    gatePolicy = [pscustomobject]@{
        ciMode = $CiMode.IsPresent
        nightlyMode = $NightlyMode.IsPresent
        failOnReleaseWarn = $FailOnReleaseWarn
        failOnReleaseFail = $FailOnReleaseFail
        allowDebugWarn = $AllowDebugWarn
        allowDebugFail = $AllowDebugFail
    }
    gateSummary = [pscustomobject]@{
        runsWithFail = $runsWithFail
        runsWithWarn = $runsWithWarn
        runsBlocked = $runsBlocked
        blockingEvents = $blockingEvents
    }
    runReports = $runReports | ForEach-Object {
        [pscustomobject]@{
            run = $_.run
            path = $_.path
            summary = $_.summary
            gate = $_.gate
        }
    }
    scenarioStats = $scenarioStats
}

$stabilitySummary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputAbsolutePath

Write-Host ("RT stability summary written to {0}" -f $outputAbsolutePath)
foreach ($scenario in $scenarioStats) {
    Write-Host ("[{0}] runs={1} status(pass/warn/fail)={2}/{3}/{4} maxBudget(med)={5:N3} cpuAvg(med)={6:N2}% cpuPeak(med)={7:N2}% peakMs(med)={8:N4}" -f `
        $scenario.name,
        $scenario.runCount,
        $scenario.statusCounts.pass,
        $scenario.statusCounts.warn,
        $scenario.statusCounts.fail,
        $scenario.maxBudgetRatio.median,
        $scenario.cpuAvgPercent.median,
        $scenario.cpuPeakPercent.median,
        $scenario.peakProcessMs.median)
}

if ($missingScenarioFilters.Count -gt 0) {
    Write-Warning ("ScenarioFilter entries not found in reports: {0}" -f ($missingScenarioFilters -join ", "))
}

if ($blockingEvents.Count -gt 0) {
    Write-Host "RT stability blocking conditions:"
    foreach ($event in $blockingEvents) {
        Write-Host ("  - {0}" -f $event)
    }

    throw "RT stability gate failed. See $outputAbsolutePath"
}

exit 0
