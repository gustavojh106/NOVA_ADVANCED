param(
    [switch]$FailOnWarn,
    [string]$ReportPath = "artifacts/audio-thread-policy-scan.txt",
    [string]$JsonReportPath = "artifacts/audio-thread-policy-scan.json"
)

$ErrorActionPreference = "Stop"

function Normalize-RelPath {
    param([string]$PathValue)
    return ($PathValue -replace '\\', '/')
}

function Ensure-ParentDirectory {
    param([string]$PathValue)
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
        New-Item -Path $parent -ItemType Directory | Out-Null
    }
}

function Resolve-ReportPath {
    param([string]$Root, [string]$PathValue)
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return Join-Path $Root $PathValue
}

function Get-ProcessRanges {
    param([string]$FilePath, [string]$Root)

    if (-not (Test-Path $FilePath)) {
        return @()
    }

    $lines = Get-Content -LiteralPath $FilePath
    $ranges = @()
    $relativePath = Normalize-RelPath((Resolve-Path -LiteralPath $FilePath -Relative).TrimStart('.\'))

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]

        $isProcessBlockDecl = $line -match '^\s*void\s+(?:[A-Za-z0-9_:<>]+\s*::\s*)?processBlock\s*\([^;]*\)\s*(?:override)?'
        $isAudioEngineProcessDecl = $line -match '^\s*void\s+AudioEngine::process\s*\('
        $isAudioEngineDryWetDecl = $line -match '^\s*void\s+AudioEngine::processWithSampleAccurateDryWet\s*\('
        $isCpuMeterBeginBlockDecl = $line -match '^\s*double\s+beginBlock\s*\(\)\s*noexcept'
        $isCpuMeterEndBlockDecl = $line -match '^\s*void\s+endBlock\s*\('
        $isHealthMonitorSanitizeDecl = $line -match '^\s*BlockStats\s+sanitizeAndMeterOutput\s*\('
        $isHealthMonitorAfterBlockDecl = $line -match '^\s*Actions\s+afterBlock\s*\('
        $isRuntimeParameterFile = $relativePath -eq "Source/Core/Audio/RuntimeParameterSnapshot.h"
        $isRuntimeParameterStoreDecl = $isRuntimeParameterFile -and $line -match '^\s*void\s+store\s*\('
        $isRuntimeParameterLoadDecl = $isRuntimeParameterFile -and $line -match '^\s*RuntimeGlobalParamsSnapshot\s+load\s*\('
        $isRuntimeParameterRevisionDecl = $isRuntimeParameterFile -and $line -match '^\s*uint32_t\s+getRevision\s*\('
        $isRuntimeParameterOutputMixDecl = $isRuntimeParameterFile -and $line -match '^\s*float\s+getOutputMixNormalized\s*\('

        $isTrackedAudioFunction = $isProcessBlockDecl -or $isAudioEngineProcessDecl -or $isAudioEngineDryWetDecl -or $isCpuMeterBeginBlockDecl -or $isCpuMeterEndBlockDecl -or $isHealthMonitorSanitizeDecl -or $isHealthMonitorAfterBlockDecl -or $isRuntimeParameterStoreDecl -or $isRuntimeParameterLoadDecl -or $isRuntimeParameterRevisionDecl -or $isRuntimeParameterOutputMixDecl
        if (-not $isTrackedAudioFunction) {
            continue
        }

        $start = $i
        $braceDepth = 0
        $seenOpenBrace = $false
        $end = $lines.Count - 1

        for ($j = $i; $j -lt $lines.Count; $j++) {
            $openCount = ([regex]::Matches($lines[$j], '\{')).Count
            $closeCount = ([regex]::Matches($lines[$j], '\}')).Count

            if ($openCount -gt 0) {
                $seenOpenBrace = $true
            }

            $braceDepth += $openCount
            $braceDepth -= $closeCount

            if ($seenOpenBrace -and $braceDepth -le 0) {
                $end = $j
                break
            }
        }

        $functionName = if ($isAudioEngineDryWetDecl) {
            'AudioEngine::processWithSampleAccurateDryWet'
        }
        elseif ($isAudioEngineProcessDecl) {
            'AudioEngine::process'
        }
        elseif ($isCpuMeterBeginBlockDecl) {
            'CpuMeter::beginBlock'
        }
        elseif ($isCpuMeterEndBlockDecl) {
            'CpuMeter::endBlock'
        }
        elseif ($isHealthMonitorSanitizeDecl) {
            'HealthMonitor::sanitizeAndMeterOutput'
        }
        elseif ($isHealthMonitorAfterBlockDecl) {
            'HealthMonitor::afterBlock'
        }
        elseif ($isRuntimeParameterStoreDecl) {
            'RuntimeParameterSnapshot::store'
        }
        elseif ($isRuntimeParameterLoadDecl) {
            'RuntimeParameterSnapshot::load'
        }
        elseif ($isRuntimeParameterRevisionDecl) {
            'RuntimeParameterSnapshot::getRevision'
        }
        elseif ($isRuntimeParameterOutputMixDecl) {
            'RuntimeParameterSnapshot::getOutputMixNormalized'
        }
        else {
            'processBlock'
        }

        $ranges += [pscustomobject]@{
            file = $relativePath
            function = $functionName
            startLine = $start + 1
            endLine = $end + 1
            lines = $lines
        }

        $i = $end
    }

    return $ranges
}

function Get-FunctionRangesBySignature {
    param(
        [string]$FilePath,
        [object[]]$SignatureSpecs
    )

    $rangesByName = @{}
    if (-not (Test-Path $FilePath)) {
        return $rangesByName
    }

    $lines = Get-Content -LiteralPath $FilePath
    $relativePath = Normalize-RelPath((Resolve-Path -LiteralPath $FilePath -Relative).TrimStart('.\'))

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $matchedSpec = $null
        foreach ($spec in $SignatureSpecs) {
            if ($line -match $spec.regex) {
                $matchedSpec = $spec
                break
            }
        }

        if ($null -eq $matchedSpec) {
            continue
        }

        $start = $i
        $braceDepth = 0
        $seenOpenBrace = $false
        $end = $lines.Count - 1

        for ($j = $i; $j -lt $lines.Count; $j++) {
            $openCount = ([regex]::Matches($lines[$j], '\{')).Count
            $closeCount = ([regex]::Matches($lines[$j], '\}')).Count

            if ($openCount -gt 0) {
                $seenOpenBrace = $true
            }

            $braceDepth += $openCount
            $braceDepth -= $closeCount

            if ($seenOpenBrace -and $braceDepth -le 0) {
                $end = $j
                break
            }
        }

        $rangesByName[$matchedSpec.name] = [pscustomobject]@{
            file = $relativePath
            function = $matchedSpec.name
            startLine = $start + 1
            endLine = $end + 1
            lines = $lines
        }

        $i = $end
    }

    return $rangesByName
}

function Is-AllowListed {
    param(
        [pscustomobject]$Finding,
        [object[]]$AllowList
    )

    foreach ($entry in $AllowList) {
        if ($Finding.file -ne $entry.file) {
            continue
        }

        if ($Finding.patternId -ne $entry.patternId) {
            continue
        }

        if ($Finding.lineText -notmatch $entry.lineRegex) {
            continue
        }

        return $entry.reason
    }

    return $null
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$reportPathAbs = Resolve-ReportPath -Root $repoRoot -PathValue $ReportPath
$jsonReportPathAbs = Resolve-ReportPath -Root $repoRoot -PathValue $JsonReportPath
Ensure-ParentDirectory -PathValue $reportPathAbs
Ensure-ParentDirectory -PathValue $jsonReportPathAbs

$dangerPatterns = @(
    [pscustomobject]@{ id = "session_logger"; regex = "SessionLogger::logEvent"; label = "SessionLogger::logEvent" },
    [pscustomobject]@{ id = "juce_string"; regex = "\bjuce::String\b"; label = "juce::String in audio path" },
    [pscustomobject]@{ id = "spinlock_scoped"; regex = "SpinLock::ScopedLockType"; label = "SpinLock::ScopedLockType" },
    [pscustomobject]@{ id = "std_mutex"; regex = "\bstd::mutex\b|\bstd::lock_guard\b"; label = "std::mutex/std::lock_guard" },
    [pscustomobject]@{ id = "juce_scoped_lock"; regex = "\bjuce::ScopedLock\b|\bCriticalSection\b"; label = "juce::ScopedLock/CriticalSection" },
    [pscustomobject]@{ id = "waitable_event"; regex = "\bWaitableEvent\b"; label = "WaitableEvent" },
    [pscustomobject]@{ id = "buffer_set_size"; regex = "\.setSize\s*\("; label = "AudioBuffer::setSize in process path" },
    [pscustomobject]@{ id = "vector_resize_push"; regex = "\.resize\s*\(|\.push_back\s*\("; label = "std::vector resize/push_back in process path" },
    [pscustomobject]@{ id = "dynamic_cast"; regex = "\bdynamic_cast<"; label = "dynamic_cast in process path" },
    [pscustomobject]@{ id = "iir_make"; regex = "IIR::(?:Array)?Coefficients<float>::make"; label = "IIR::Coefficients make* in process path" },
    [pscustomobject]@{ id = "set_value_notifying_host"; regex = "setValueNotifyingHost\s*\("; label = "setValueNotifyingHost in process path" },
    [pscustomobject]@{ id = "graph_rebuild"; regex = "buildGraphFromModelLocked|requestControlGraphRebuild|(?:->|\.)rebuild\s*\("; label = "graph rebuild in process path" }
)

# Explicit allowlist for known false positives. Keep this narrow.
$allowList = @(
    [pscustomobject]@{
        file = "Source/Core/PluginProcessor.cpp"
        patternId = "dynamic_cast"
        lineRegex = "if \(auto\* ranged = dynamic_cast<juce::RangedAudioParameter\*>\(getParameters\(\)\[parameterIndex\]\)\)"
        reason = "Parameter listener path, not audio callback."
    },
    [pscustomobject]@{
        file = "Source/Core/AudioEngine.cpp"
        patternId = "graph_rebuild"
        lineRegex = "audioPlane\.graphResetRequested\.store\(true"
        reason = "Atomic flag only; rebuild is executed on control thread."
    }
)

$coreActiveFiles = @(
    "Source/Core/PluginProcessor.cpp",
    "Source/Core/AudioEngine.cpp",
    "Source/Core/Audio/CpuMeter.h",
    "Source/Core/Audio/HealthMonitor.h",
    "Source/Core/Audio/RuntimeParameterSnapshot.h",
    "Source/Core/DSP/Global/InputChain.cpp",
    "Source/Core/DSP/Global/ChannelStrip.cpp",
    "Source/Core/DSP/Global/OutputChain.cpp"
)

$registryFile = "Source/Core/PedalRegistry.h"
$registryIncludeMatches = Select-String -Path $registryFile -Pattern '^\s*#include\s+"\.\./(.+\.h)"'
$registryFiles = @()
foreach ($match in $registryIncludeMatches) {
    $candidate = ("Source/" + $match.Matches[0].Groups[1].Value).Replace('/', '\')
    if (Test-Path $candidate) {
        $registryFiles += Normalize-RelPath($candidate)
    }
}

$activeFiles = @()
$activeFiles += $coreActiveFiles | ForEach-Object { Normalize-RelPath($_) }
$activeFiles += $registryFiles
$activeFiles = $activeFiles | Sort-Object -Unique

$activeRanges = @()
foreach ($rel in $activeFiles) {
    $fullPath = Join-Path $repoRoot $rel
    $activeRanges += Get-ProcessRanges -FilePath $fullPath -Root $repoRoot
}

$activeFailures = @()
$activeAllowlistedWarnings = @()
foreach ($range in $activeRanges) {
    for ($lineNo = $range.startLine; $lineNo -le $range.endLine; $lineNo++) {
        $text = $range.lines[$lineNo - 1]
        foreach ($pattern in $dangerPatterns) {
            if ($text -notmatch $pattern.regex) {
                continue
            }

            $finding = [pscustomobject]@{
                file = $range.file
                function = $range.function
                line = $lineNo
                patternId = $pattern.id
                patternLabel = $pattern.label
                lineText = $text.Trim()
            }

            $allowReason = Is-AllowListed -Finding $finding -AllowList $allowList
            if ($null -ne $allowReason) {
                $activeAllowlistedWarnings += [pscustomobject]@{
                    file = $finding.file
                    function = $finding.function
                    line = $finding.line
                    patternId = $finding.patternId
                    patternLabel = $finding.patternLabel
                    lineText = $finding.lineText
                    reason = $allowReason
                }
            }
            else {
                $activeFailures += $finding
            }
        }
    }
}

$contractChecks = @()
$contractFailures = @()

$runtimeGraphManagerRel = "Source/Core/Audio/RuntimeGraphManager.h"
$runtimeGraphManagerPath = Join-Path $repoRoot $runtimeGraphManagerRel
$runtimeGraphManagerFunctions = @(
    [pscustomobject]@{ name = "RuntimeGraphManager::getActiveRaw"; regex = '^\s*GraphRuntime\*\s+getActiveRaw\s*\(' },
    [pscustomobject]@{ name = "RuntimeGraphManager::publish"; regex = '^\s*bool\s+publish\s*\(' },
    [pscustomobject]@{ name = "RuntimeGraphManager::getActiveOwnerForControl"; regex = '^\s*std::shared_ptr<GraphRuntime>\s+getActiveOwnerForControl\s*\(' },
    [pscustomobject]@{ name = "RuntimeGraphManager::cleanupRetired"; regex = '^\s*void\s+cleanupRetired\s*\(' }
)
$runtimeGraphManagerRanges = Get-FunctionRangesBySignature -FilePath $runtimeGraphManagerPath -SignatureSpecs $runtimeGraphManagerFunctions

foreach ($fn in $runtimeGraphManagerFunctions) {
    $exists = $runtimeGraphManagerRanges.ContainsKey($fn.name)
    $contractChecks += [pscustomobject]@{
        checkId = "manager_method_exists"
        target = $fn.name
        passed = $exists
        detail = if ($exists) { "found" } else { "missing" }
    }

    if (-not $exists) {
        $contractFailures += [pscustomobject]@{
            checkId = "manager_method_missing"
            file = Normalize-RelPath($runtimeGraphManagerRel)
            function = $fn.name
            detail = "Required RuntimeGraphManager method is missing."
        }
    }
}

$processRange = $activeRanges | Where-Object { $_.file -eq "Source/Core/AudioEngine.cpp" -and $_.function -eq "AudioEngine::process" } | Select-Object -First 1
if ($null -eq $processRange) {
    $contractChecks += [pscustomobject]@{
        checkId = "audioengine_process_found"
        target = "AudioEngine::process"
        passed = $false
        detail = "not found"
    }

    $contractFailures += [pscustomobject]@{
        checkId = "audioengine_process_missing"
        file = "Source/Core/AudioEngine.cpp"
        function = "AudioEngine::process"
        detail = "AudioEngine::process range was not found by policy scanner."
    }
}
else {
    $processLines = $processRange.lines[($processRange.startLine - 1)..($processRange.endLine - 1)]
    $processText = ($processLines -join "`n")

    $activeRawCallCount = ([regex]::Matches($processText, 'runtimeGraphs\.getActiveRaw\s*\(')).Count
    $hasActiveRawCall = $activeRawCallCount -gt 0
    $contractChecks += [pscustomobject]@{
        checkId = "process_uses_getActiveRaw"
        target = "AudioEngine::process"
        passed = $hasActiveRawCall
        detail = "callCount=$activeRawCallCount"
    }
    if (-not $hasActiveRawCall) {
        $contractFailures += [pscustomobject]@{
            checkId = "process_missing_getActiveRaw"
            file = $processRange.file
            function = $processRange.function
            detail = "AudioEngine::process does not call runtimeGraphs.getActiveRaw()."
        }
    }

    $forbiddenProcessCalls = @(
        "runtimeGraphs\.publish\s*\(",
        "runtimeGraphs\.cleanupRetired\s*\(",
        "runtimeGraphs\.getActiveOwnerForControl\s*\("
    )

    foreach ($forbidden in $forbiddenProcessCalls) {
        if ($processText -match $forbidden) {
            $contractFailures += [pscustomobject]@{
                checkId = "process_forbidden_manager_call"
                file = $processRange.file
                function = $processRange.function
                detail = "AudioEngine::process contains forbidden manager call matching '$forbidden'."
            }
        }
    }

    $callsBuildGraphFromModelLocked = $processText -match '\bbuildGraphFromModelLocked\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "process_no_buildGraphFromModelLocked"
        target = "AudioEngine::process"
        passed = (-not $callsBuildGraphFromModelLocked)
        detail = if ($callsBuildGraphFromModelLocked) { "buildGraphFromModelLocked call found" } else { "no buildGraphFromModelLocked call in process" }
    }
    if ($callsBuildGraphFromModelLocked) {
        $contractFailures += [pscustomobject]@{
            checkId = "process_calls_buildGraphFromModelLocked"
            file = $processRange.file
            function = $processRange.function
            detail = "AudioEngine::process must not call buildGraphFromModelLocked."
        }
    }

    $callsGraphBuilderBuild = ($processText -match 'GraphBuilder::build\s*\(') -or ($processText -match '\bbuilder\.build\s*\(')
    $contractChecks += [pscustomobject]@{
        checkId = "process_no_graphbuilder_build"
        target = "AudioEngine::process"
        passed = (-not $callsGraphBuilderBuild)
        detail = if ($callsGraphBuilderBuild) { "GraphBuilder::build call found" } else { "no GraphBuilder::build call in process" }
    }
    if ($callsGraphBuilderBuild) {
        $contractFailures += [pscustomobject]@{
            checkId = "process_calls_graphbuilder_build"
            file = $processRange.file
            function = $processRange.function
            detail = "AudioEngine::process must not call GraphBuilder::build."
        }
    }

    if ($processText -match '\bstd::shared_ptr\b') {
        $contractFailures += [pscustomobject]@{
            checkId = "process_shared_ptr_usage"
            file = $processRange.file
            function = $processRange.function
            detail = "AudioEngine::process references std::shared_ptr."
        }
    }

    if ($processText -match '\bjuce::ScopedLock\b|\bCriticalSection\b') {
        $contractFailures += [pscustomobject]@{
            checkId = "process_lock_usage"
            file = $processRange.file
            function = $processRange.function
            detail = "AudioEngine::process references juce::ScopedLock/CriticalSection."
        }
    }

    $processCallsRoutingMixer = $processText -match '\bRoutingMixer\b'
    $contractChecks += [pscustomobject]@{
        checkId = "process_no_routingmixer"
        target = "AudioEngine::process"
        passed = (-not $processCallsRoutingMixer)
        detail = if ($processCallsRoutingMixer) { "RoutingMixer reference found" } else { "RoutingMixer not referenced" }
    }
    if ($processCallsRoutingMixer) {
        $contractFailures += [pscustomobject]@{
            checkId = "process_references_routingmixer"
            file = $processRange.file
            function = $processRange.function
            detail = "AudioEngine::process must not call RoutingMixer directly."
        }
    }
}

$dryWetMixerRel = "Source/Core/Audio/DryWetMixer.h"
$dryWetMixerPath = Join-Path $repoRoot $dryWetMixerRel
if (Test-Path $dryWetMixerPath) {
    $dryWetMixerText = Get-Content -LiteralPath $dryWetMixerPath -Raw
    $dryWetForbiddenPatterns = @(
        [pscustomobject]@{ checkId = "drywet_no_graphruntime"; pattern = '\bGraphRuntime\b'; detail = "DryWetMixer must not know GraphRuntime." },
        [pscustomobject]@{ checkId = "drywet_no_processblock"; pattern = '\bprocessBlock\s*\('; detail = "DryWetMixer must not call or reference processBlock." },
        [pscustomobject]@{ checkId = "drywet_no_session_logger"; pattern = 'SessionLogger::logEvent'; detail = "DryWetMixer must not log from the audio path." },
        [pscustomobject]@{ checkId = "drywet_no_juce_string"; pattern = '\bjuce::String\b'; detail = "DryWetMixer must not use juce::String in the audio path." }
    )

    foreach ($check in $dryWetForbiddenPatterns) {
        $hasForbiddenPattern = $dryWetMixerText -match $check.pattern
        $contractChecks += [pscustomobject]@{
            checkId = $check.checkId
            target = "DryWetMixer"
            passed = (-not $hasForbiddenPattern)
            detail = if ($hasForbiddenPattern) { "forbidden pattern found" } else { "not present" }
        }

        if ($hasForbiddenPattern) {
            $contractFailures += [pscustomobject]@{
                checkId = $check.checkId
                file = Normalize-RelPath($dryWetMixerRel)
                function = "DryWetMixer"
                detail = $check.detail
            }
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "drywet_header_exists"
        target = "DryWetMixer.h"
        passed = $false
        detail = "DryWetMixer.h not found"
    }

    $contractFailures += [pscustomobject]@{
        checkId = "drywet_header_missing"
        file = Normalize-RelPath($dryWetMixerRel)
        function = "DryWetMixer"
        detail = "DryWetMixer header is missing."
    }
}

$routingMixerRel = "Source/Core/Audio/RoutingMixer.h"
$routingMixerPath = Join-Path $repoRoot $routingMixerRel
if (Test-Path $routingMixerPath) {
    $routingMixerText = Get-Content -LiteralPath $routingMixerPath -Raw
    $routingMixerForbiddenPatterns = @(
        [pscustomobject]@{ checkId = "routingmixer_no_graphruntime"; pattern = '\bGraphRuntime\b'; detail = "RoutingMixer must not know GraphRuntime." },
        [pscustomobject]@{ checkId = "routingmixer_no_processblock"; pattern = '\bprocessBlock\s*\('; detail = "RoutingMixer must not call or reference processBlock." },
        [pscustomobject]@{ checkId = "routingmixer_no_session_logger"; pattern = 'SessionLogger::logEvent'; detail = "RoutingMixer must not log from the audio path." },
        [pscustomobject]@{ checkId = "routingmixer_no_juce_string"; pattern = '\bjuce::String\b'; detail = "RoutingMixer must not use juce::String." }
    )

    foreach ($check in $routingMixerForbiddenPatterns) {
        $hasForbiddenPattern = $routingMixerText -match $check.pattern
        $contractChecks += [pscustomobject]@{
            checkId = $check.checkId
            target = "RoutingMixer"
            passed = (-not $hasForbiddenPattern)
            detail = if ($hasForbiddenPattern) { "forbidden pattern found" } else { "not present" }
        }

        if ($hasForbiddenPattern) {
            $contractFailures += [pscustomobject]@{
                checkId = $check.checkId
                file = Normalize-RelPath($routingMixerRel)
                function = "RoutingMixer"
                detail = $check.detail
            }
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "routingmixer_header_exists"
        target = "RoutingMixer.h"
        passed = $false
        detail = "RoutingMixer.h not found"
    }

    $contractFailures += [pscustomobject]@{
        checkId = "routingmixer_header_missing"
        file = Normalize-RelPath($routingMixerRel)
        function = "RoutingMixer"
        detail = "RoutingMixer header is missing."
    }
}

$dryWetProcessRange = $activeRanges | Where-Object { $_.file -eq "Source/Core/AudioEngine.cpp" -and $_.function -eq "AudioEngine::processWithSampleAccurateDryWet" } | Select-Object -First 1
if ($null -eq $dryWetProcessRange) {
    $contractChecks += [pscustomobject]@{
        checkId = "audioengine_drywet_orchestrator_found"
        target = "AudioEngine::processWithSampleAccurateDryWet"
        passed = $false
        detail = "not found"
    }

    $contractFailures += [pscustomobject]@{
        checkId = "audioengine_drywet_orchestrator_missing"
        file = "Source/Core/AudioEngine.cpp"
        function = "AudioEngine::processWithSampleAccurateDryWet"
        detail = "AudioEngine::processWithSampleAccurateDryWet range was not found by policy scanner."
    }
}
else {
    $dryWetProcessLines = $dryWetProcessRange.lines[($dryWetProcessRange.startLine - 1)..($dryWetProcessRange.endLine - 1)]
    $dryWetProcessText = ($dryWetProcessLines -join "`n")
    $graphProcessBlockCallCount = ([regex]::Matches($dryWetProcessText, 'runtime\.graph->processBlock\s*\(')).Count
    $hasGraphProcessBlockCall = $graphProcessBlockCallCount -gt 0

    $contractChecks += [pscustomobject]@{
        checkId = "drywet_orchestrator_keeps_graph_call"
        target = "AudioEngine::processWithSampleAccurateDryWet"
        passed = $hasGraphProcessBlockCall
        detail = "callCount=$graphProcessBlockCallCount"
    }

    if (-not $hasGraphProcessBlockCall) {
        $contractFailures += [pscustomobject]@{
            checkId = "drywet_orchestrator_missing_graph_call"
            file = $dryWetProcessRange.file
            function = $dryWetProcessRange.function
            detail = "AudioEngine::processWithSampleAccurateDryWet must keep the wet graph processBlock call."
        }
    }
}

$graphBuilderRel = "Source/Core/Audio/GraphBuilder.h"
$graphBuilderPath = Join-Path $repoRoot $graphBuilderRel
if (Test-Path $graphBuilderPath) {
    $graphBuilderText = Get-Content -LiteralPath $graphBuilderPath -Raw
    $includesAudioEngineHeader = $graphBuilderText -match '#include\s+"[^"]*AudioEngine\.h"'
    $contractChecks += [pscustomobject]@{
        checkId = "graphbuilder_no_audioengine_include"
        target = "GraphBuilder.h"
        passed = (-not $includesAudioEngineHeader)
        detail = if ($includesAudioEngineHeader) { "AudioEngine.h include found" } else { "AudioEngine.h include not present" }
    }
    if ($includesAudioEngineHeader) {
        $contractFailures += [pscustomobject]@{
            checkId = "graphbuilder_includes_audioengine"
            file = Normalize-RelPath($graphBuilderRel)
            function = "GraphBuilder"
            detail = "GraphBuilder must not include AudioEngine.h."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "graphbuilder_header_exists"
        target = "GraphBuilder.h"
        passed = $false
        detail = "GraphBuilder.h not found"
    }

    $contractFailures += [pscustomobject]@{
        checkId = "graphbuilder_header_missing"
        file = Normalize-RelPath($graphBuilderRel)
        function = "GraphBuilder"
        detail = "GraphBuilder header is missing."
    }
}

$graphBuildWrapperSpec = @(
    [pscustomobject]@{
        name = "AudioEngine::buildGraphFromModelLocked"
        regex = '^\s*std::shared_ptr<AudioEngine::GraphRuntime>\s+AudioEngine::buildGraphFromModelLocked\s*\('
    }
)
$graphBuildWrapperPath = Join-Path $repoRoot "Source/Core/AudioEngine.cpp"
$graphBuildWrapperRanges = Get-FunctionRangesBySignature -FilePath $graphBuildWrapperPath -SignatureSpecs $graphBuildWrapperSpec
if ($graphBuildWrapperRanges.ContainsKey("AudioEngine::buildGraphFromModelLocked")) {
    $wrapperRange = $graphBuildWrapperRanges["AudioEngine::buildGraphFromModelLocked"]
    $wrapperText = ($wrapperRange.lines[($wrapperRange.startLine - 1)..($wrapperRange.endLine - 1)] -join "`n")
    $looksLikeBuilderWrapper = ($wrapperText -match 'GraphBuildRequest') -and ($wrapperText -match 'GraphBuilder') -and ($wrapperText -match '\bbuilder\.build\s*\(')

    $contractChecks += [pscustomobject]@{
        checkId = "buildGraph_wrapper_uses_graphbuilder"
        target = "AudioEngine::buildGraphFromModelLocked"
        passed = $looksLikeBuilderWrapper
        detail = if ($looksLikeBuilderWrapper) { "wrapper request+builder call found" } else { "wrapper markers missing" }
    }

    if (-not $looksLikeBuilderWrapper) {
        $contractFailures += [pscustomobject]@{
            checkId = "buildGraph_wrapper_missing_builder_call"
            file = $wrapperRange.file
            function = $wrapperRange.function
            detail = "AudioEngine::buildGraphFromModelLocked should delegate to GraphBuilder via GraphBuildRequest."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "buildGraph_wrapper_exists"
        target = "AudioEngine::buildGraphFromModelLocked"
        passed = $false
        detail = "function not found"
    }

    $contractFailures += [pscustomobject]@{
        checkId = "buildGraph_wrapper_missing"
        file = "Source/Core/AudioEngine.cpp"
        function = "AudioEngine::buildGraphFromModelLocked"
        detail = "AudioEngine::buildGraphFromModelLocked was not found by policy scanner."
    }
}

if ($runtimeGraphManagerRanges.ContainsKey("RuntimeGraphManager::getActiveRaw")) {
    $getActiveRawRange = $runtimeGraphManagerRanges["RuntimeGraphManager::getActiveRaw"]
    $getActiveRawText = ($getActiveRawRange.lines[($getActiveRawRange.startLine - 1)..($getActiveRawRange.endLine - 1)] -join "`n")

    $isAtomicLoadOnly = $getActiveRawText -match 'return\s+activeGraphRaw\.load\s*\(\s*std::memory_order_acquire\s*\)\s*;'
    $contractChecks += [pscustomobject]@{
        checkId = "getActiveRaw_atomic_load"
        target = "RuntimeGraphManager::getActiveRaw"
        passed = $isAtomicLoadOnly
        detail = if ($isAtomicLoadOnly) { "acquire-load return found" } else { "acquire-load return not found" }
    }
    if (-not $isAtomicLoadOnly) {
        $contractFailures += [pscustomobject]@{
            checkId = "getActiveRaw_not_atomic_load_only"
            file = $getActiveRawRange.file
            function = $getActiveRawRange.function
            detail = "RuntimeGraphManager::getActiveRaw is not a direct acquire atomic load."
        }
    }

    if ($getActiveRawText -match '\bScopedLock\b|\bCriticalSection\b|\bstd::shared_ptr\b|\bactiveOwner\b|\bretiredGraphs\b') {
        $contractFailures += [pscustomobject]@{
            checkId = "getActiveRaw_contains_control_thread_state"
            file = $getActiveRawRange.file
            function = $getActiveRawRange.function
            detail = "RuntimeGraphManager::getActiveRaw references lock/shared_ptr/control-thread state."
        }
    }
}

$legacyFiles = @(
    "Source/Effects/Pedals/ChorusPedal.h",
    "Source/Effects/Pedals/CompressorPedal.h",
    "Source/Effects/Pedals/Wah/AutoWahPedal.h",
    "Source/Effects/Pedals/Metal/MetalDistortionPedal.h"
)

$legacyWarnings = @()
foreach ($legacyRel in $legacyFiles) {
    $legacyFull = Join-Path $repoRoot $legacyRel
    if (-not (Test-Path $legacyFull)) {
        $legacyWarnings += [pscustomobject]@{
            file = $legacyRel
            classification = "missing"
            inJucer = $false
            inRegistry = $false
            line = 0
            patternId = "missing"
            patternLabel = "Legacy file missing"
            lineText = "File not found"
            reason = "Listed in legacy policy but file is missing."
        }
        continue
    }

    $inJucer = Select-String -Path "NOVA.jucer" -SimpleMatch $legacyRel -Quiet
    $registrySearch = "../" + $legacyRel.Substring("Source/".Length)
    $inRegistry = Select-String -Path $registryFile -SimpleMatch $registrySearch -Quiet

    $ranges = Get-ProcessRanges -FilePath $legacyFull -Root $repoRoot
    foreach ($range in $ranges) {
        for ($lineNo = $range.startLine; $lineNo -le $range.endLine; $lineNo++) {
            $text = $range.lines[$lineNo - 1]
            foreach ($pattern in $dangerPatterns) {
                if ($text -notmatch $pattern.regex) {
                    continue
                }

                $legacyWarnings += [pscustomobject]@{
                    file = Normalize-RelPath($legacyRel)
                    classification = if ($inRegistry) { "legacy-registered" } elseif ($inJucer) { "legacy-in-jucer" } else { "legacy-no-active-reference" }
                    inJucer = $inJucer
                    inRegistry = $inRegistry
                    line = $lineNo
                    patternId = $pattern.id
                    patternLabel = $pattern.label
                    lineText = $text.Trim()
                    reason = "Legacy file is not treated as active audio route by this scanner."
                }
            }
        }
    }
}

$summaryWarnings = @()
$summaryWarnings += $activeAllowlistedWarnings
$summaryWarnings += $legacyWarnings

$totalFailures = $activeFailures.Count + $contractFailures.Count
$status = if ($totalFailures -gt 0) { "FAIL" } elseif ($summaryWarnings.Count -gt 0) { "WARN" } else { "PASS" }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("NOVA Audio Thread Policy Scan")
$lines.Add("Generated: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))")
$lines.Add("status=$status")
$lines.Add("summary.activeFiles=$($activeFiles.Count)")
$lines.Add("summary.activeRanges=$($activeRanges.Count)")
$lines.Add("summary.failures=$totalFailures")
$lines.Add("summary.warnings=$($summaryWarnings.Count)")
$lines.Add("summary.allowlistedWarnings=$($activeAllowlistedWarnings.Count)")
$lines.Add("summary.legacyWarnings=$($legacyWarnings.Count)")
$lines.Add("summary.contractChecks=$($contractChecks.Count)")
$lines.Add("summary.contractFailures=$($contractFailures.Count)")
$lines.Add("summary.allowListEntries=$($allowList.Count)")
$lines.Add("")

$lines.Add("Active files scanned:")
foreach ($file in $activeFiles) {
    $lines.Add("  - $file")
}
$lines.Add("")

$lines.Add("Allowlist entries:")
foreach ($entry in $allowList) {
    $lines.Add("  - file=$($entry.file) pattern=$($entry.patternId) reason=$($entry.reason)")
}
$lines.Add("")

if ($activeFailures.Count -gt 0) {
    $lines.Add("FAIL findings in active audio routes:")
    foreach ($finding in $activeFailures | Sort-Object file, line, patternId) {
        $lines.Add("  - [$($finding.patternLabel)] $($finding.file):$($finding.line) function=$($finding.function)")
        $lines.Add("    line=$($finding.lineText)")
    }
    $lines.Add("")
}
else {
    $lines.Add("No FAIL findings in active audio routes.")
    $lines.Add("")
}

if ($contractFailures.Count -gt 0) {
    $lines.Add("RuntimeGraphManager contract FAIL findings:")
    foreach ($finding in $contractFailures) {
        $lines.Add("  - [$($finding.checkId)] $($finding.file) function=$($finding.function)")
        $lines.Add("    detail=$($finding.detail)")
    }
    $lines.Add("")
}
else {
    $lines.Add("RuntimeGraphManager contract checks: PASS")
    $lines.Add("")
}

$lines.Add("RuntimeGraphManager contract checks detail:")
foreach ($check in $contractChecks) {
    $lines.Add("  - [$($check.checkId)] target=$($check.target) passed=$($check.passed) detail=$($check.detail)")
}
$lines.Add("")

if ($activeAllowlistedWarnings.Count -gt 0) {
    $lines.Add("Allowlisted findings (WARN):")
    foreach ($finding in $activeAllowlistedWarnings | Sort-Object file, line, patternId) {
        $lines.Add("  - [$($finding.patternLabel)] $($finding.file):$($finding.line) function=$($finding.function)")
        $lines.Add("    line=$($finding.lineText)")
        $lines.Add("    allowReason=$($finding.reason)")
    }
    $lines.Add("")
}

if ($legacyWarnings.Count -gt 0) {
    $lines.Add("Legacy findings (WARN, non-blocking):")
    foreach ($finding in $legacyWarnings | Sort-Object file, line, patternId) {
        $lines.Add("  - [$($finding.patternLabel)] $($finding.file):$($finding.line)")
        $lines.Add("    classification=$($finding.classification) inJucer=$($finding.inJucer) inRegistry=$($finding.inRegistry)")
        $lines.Add("    line=$($finding.lineText)")
    }
    $lines.Add("")
}

$lines.Add("Policy result: $status")

$lines | Set-Content -LiteralPath $reportPathAbs

$jsonPayload = [pscustomobject]@{
    generatedAt = (Get-Date).ToString("o")
    status = $status
    summary = [pscustomobject]@{
        activeFiles = $activeFiles.Count
        activeRanges = $activeRanges.Count
        failures = $totalFailures
        warnings = $summaryWarnings.Count
        allowlistedWarnings = $activeAllowlistedWarnings.Count
        legacyWarnings = $legacyWarnings.Count
        contractChecks = $contractChecks.Count
        contractFailures = $contractFailures.Count
        allowListEntries = $allowList.Count
    }
    activeFiles = $activeFiles
    allowList = $allowList
    failures = $activeFailures
    contractChecks = $contractChecks
    contractFailures = $contractFailures
    allowlistedWarnings = $activeAllowlistedWarnings
    legacyWarnings = $legacyWarnings
}

$jsonPayload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonReportPathAbs

Write-Host "Audio thread policy status: $status"
Write-Host "Report: $reportPathAbs"
Write-Host "JSON:   $jsonReportPathAbs"

if ($totalFailures -gt 0) {
    throw "Audio thread policy scan found blocking failures."
}

if ($FailOnWarn.IsPresent -and $summaryWarnings.Count -gt 0) {
    throw "Audio thread policy scan has warnings and -FailOnWarn is enabled."
}

exit 0
