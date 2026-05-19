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

# ==============================================================================
# P7B audio-thread RT-safety contract checks
#
# These checks codify the closure of P0 items in
# docs/audio-realtime-safety-audit.md. They run at file scope (not just inside
# processBlock ranges) because the offending patterns can also appear in helper
# functions reached from the audio path (for example `updateVoicingIfNeeded`
# called from amp processBlock).
# ==============================================================================

$audioThreadEmitterFiles = @(
    "Source/Core/DSP/Global/ChannelStrip.h",
    "Source/Core/DSP/Global/ChannelStrip.cpp",
    "Source/Core/DSP/Global/OutputChain.h",
    "Source/Core/DSP/Global/OutputChain.cpp",
    "Source/Effects/Pedals/Overdrive/OverdrivePedal.h",
    "Source/Effects/Amplifiers/CleanAmp.h",
    "Source/Effects/Pedals/Delay/DelayPedal.h",
    "Source/Effects/Pedals/Flanger/FlangerPedal.h",
    "Source/Effects/Pedals/Reverb/ReverbPedal.h"
)

$audioThreadEmitterPublishUsers = @(
    "Source/Core/DSP/Global/ChannelStrip.cpp",
    "Source/Core/DSP/Global/OutputChain.cpp",
    "Source/Effects/Pedals/Overdrive/OverdrivePedal.h",
    "Source/Effects/Amplifiers/CleanAmp.h",
    "Source/Effects/Pedals/Delay/DelayPedal.h",
    "Source/Effects/Pedals/Flanger/FlangerPedal.h",
    "Source/Effects/Pedals/Reverb/ReverbPedal.h"
)

foreach ($emitterRel in $audioThreadEmitterFiles) {
    $emitterFull = Join-Path $repoRoot $emitterRel
    if (-not (Test-Path $emitterFull)) {
        $contractChecks += [pscustomobject]@{
            checkId = "p7b_emitter_no_session_logger"
            target = $emitterRel
            passed = $false
            detail = "file missing"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_emitter_missing"
            file = Normalize-RelPath($emitterRel)
            function = "audio-thread emitter"
            detail = "Audio-thread emitter file is missing."
        }
        continue
    }

    $emitterText = Get-Content -LiteralPath $emitterFull -Raw
    $hasSessionLogger = $emitterText -match 'SessionLogger::logEvent|SessionLogger::logValueTree'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_emitter_no_session_logger"
        target = $emitterRel
        passed = (-not $hasSessionLogger)
        detail = if ($hasSessionLogger) { "SessionLogger call found" } else { "no SessionLogger call" }
    }
    if ($hasSessionLogger) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_emitter_calls_session_logger"
            file = Normalize-RelPath($emitterRel)
            function = "audio-thread emitter"
            detail = "Audio-thread emitter must not call SessionLogger directly."
        }
    }
}

foreach ($emitterRel in $audioThreadEmitterPublishUsers) {
    $emitterFull = Join-Path $repoRoot $emitterRel
    if (-not (Test-Path $emitterFull)) {
        continue
    }

    $emitterText = Get-Content -LiteralPath $emitterFull -Raw
    $usesPublish = $emitterText -match 'captureOutputAndPublishIfNeeded\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_emitter_uses_lock_free_publish"
        target = $emitterRel
        passed = $usesPublish
        detail = if ($usesPublish) { "captureOutputAndPublishIfNeeded call found" } else { "captureOutputAndPublishIfNeeded call missing" }
    }
    if (-not $usesPublish) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_emitter_missing_lock_free_publish"
            file = Normalize-RelPath($emitterRel)
            function = "audio-thread emitter"
            detail = "Audio-thread emitter must publish telemetry through the lock-free queue."
        }
    }
}

$pedalTelemetryRel = "Source/Core/PedalSignalTelemetry.h"
$pedalTelemetryPath = Join-Path $repoRoot $pedalTelemetryRel
if (Test-Path $pedalTelemetryPath) {
    $pedalTelemetryText = Get-Content -LiteralPath $pedalTelemetryPath -Raw

    $hasLockFreeQueue = $pedalTelemetryText -match 'class\s+RealtimeSignalTelemetryQueue\b'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_pedal_telemetry_lock_free_queue_present"
        target = "PedalSignalTelemetry.h"
        passed = $hasLockFreeQueue
        detail = if ($hasLockFreeQueue) { "RealtimeSignalTelemetryQueue declared" } else { "RealtimeSignalTelemetryQueue missing" }
    }
    if (-not $hasLockFreeQueue) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_pedal_telemetry_lock_free_queue_missing"
            file = Normalize-RelPath($pedalTelemetryRel)
            function = "PedalSignalTelemetry"
            detail = "PedalSignalTelemetry.h must declare the lock-free RealtimeSignalTelemetryQueue."
        }
    }

    $hasSessionLogger = $pedalTelemetryText -match 'SessionLogger::logEvent|SessionLogger::logValueTree'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_pedal_telemetry_no_session_logger"
        target = "PedalSignalTelemetry.h"
        passed = (-not $hasSessionLogger)
        detail = if ($hasSessionLogger) { "SessionLogger call found" } else { "no SessionLogger call" }
    }
    if ($hasSessionLogger) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_pedal_telemetry_calls_session_logger"
            file = Normalize-RelPath($pedalTelemetryRel)
            function = "PedalSignalTelemetry"
            detail = "PedalSignalTelemetry must not call SessionLogger from the audio path."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_pedal_telemetry_lock_free_queue_present"
        target = "PedalSignalTelemetry.h"
        passed = $false
        detail = "PedalSignalTelemetry.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7b_pedal_telemetry_header_missing"
        file = Normalize-RelPath($pedalTelemetryRel)
        function = "PedalSignalTelemetry"
        detail = "PedalSignalTelemetry header is missing."
    }
}

$ampFiles = @(
    "Source/Effects/Amplifiers/ClassicAmp.h",
    "Source/Effects/Amplifiers/HighGainAmp.h",
    "Source/Effects/Amplifiers/ChimeAmp.h",
    "Source/Effects/Amplifiers/BoutiqueAmp.h",
    "Source/Effects/Amplifiers/CleanAmp.h"
)

foreach ($ampRel in $ampFiles) {
    $ampFull = Join-Path $repoRoot $ampRel
    if (-not (Test-Path $ampFull)) {
        $contractChecks += [pscustomobject]@{
            checkId = "p7b_amp_no_vector_channel_alloc"
            target = $ampRel
            passed = $false
            detail = "file missing"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_amp_missing"
            file = Normalize-RelPath($ampRel)
            function = "amp"
            detail = "Amp file is missing."
        }
        continue
    }

    $ampText = Get-Content -LiteralPath $ampFull -Raw

    $hasVectorChannelAlloc = $ampText -match 'std::vector\s*<\s*float\s*\*\s*>'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_amp_no_vector_channel_alloc"
        target = $ampRel
        passed = (-not $hasVectorChannelAlloc)
        detail = if ($hasVectorChannelAlloc) { "std::vector<float*> found" } else { "no std::vector<float*> in amp" }
    }
    if ($hasVectorChannelAlloc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_amp_vector_channel_alloc"
            file = Normalize-RelPath($ampRel)
            function = "amp"
            detail = "Amp must use std::array for per-channel pointer scratch (no std::vector<float*>)."
        }
    }

    # Forbid the allocating juce::dsp::IIR::Coefficients<float>::make* factories
    # while explicitly allowing the value-type juce::dsp::IIR::ArrayCoefficients
    # variant. The negative lookbehind ensures we only match the allocating form.
    $hasJuceIirFactory = [System.Text.RegularExpressions.Regex]::IsMatch($ampText, '(?<!Array)IIR::Coefficients<float>::make')
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_amp_no_juce_iir_factory"
        target = $ampRel
        passed = (-not $hasJuceIirFactory)
        detail = if ($hasJuceIirFactory) { "IIR::Coefficients<float>::make found" } else { "amp uses ArrayCoefficients only" }
    }
    if ($hasJuceIirFactory) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_amp_juce_iir_factory"
            file = Normalize-RelPath($ampRel)
            function = "amp"
            detail = "Amp must not call juce::dsp::IIR::Coefficients<float>::make* (use ArrayCoefficients value-type form)."
        }
    }
}

$pluginProcessorRel = "Source/Core/PluginProcessor.cpp"
$pluginProcessorRange = $activeRanges | Where-Object { $_.file -eq $pluginProcessorRel -and $_.function -eq "processBlock" } | Select-Object -First 1
if ($null -ne $pluginProcessorRange) {
    $pluginProcessorText = ($pluginProcessorRange.lines[($pluginProcessorRange.startLine - 1)..($pluginProcessorRange.endLine - 1)] -join "`n")

    $hasSessionLogger = $pluginProcessorText -match 'SessionLogger::logEvent|SessionLogger::logValueTree'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_plugin_processor_processblock_no_session_logger"
        target = "NOVAAudioProcessor::processBlock"
        passed = (-not $hasSessionLogger)
        detail = if ($hasSessionLogger) { "SessionLogger call found in processBlock body" } else { "no SessionLogger call in processBlock body" }
    }
    if ($hasSessionLogger) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_plugin_processor_processblock_session_logger"
            file = Normalize-RelPath($pluginProcessorRel)
            function = "NOVAAudioProcessor::processBlock"
            detail = "NOVAAudioProcessor::processBlock must not call SessionLogger directly."
        }
    }

    $hasJuceString = $pluginProcessorText -match '\bjuce::String\b'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_plugin_processor_processblock_no_juce_string"
        target = "NOVAAudioProcessor::processBlock"
        passed = (-not $hasJuceString)
        detail = if ($hasJuceString) { "juce::String reference found in processBlock body" } else { "no juce::String in processBlock body" }
    }
    if ($hasJuceString) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_plugin_processor_processblock_juce_string"
            file = Normalize-RelPath($pluginProcessorRel)
            function = "NOVAAudioProcessor::processBlock"
            detail = "NOVAAudioProcessor::processBlock must not construct juce::String."
        }
    }

    $hasSpinLock = $pluginProcessorText -match 'SpinLock|ScopedLockType|juce::ScopedLock|CriticalSection|std::mutex|std::lock_guard'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_plugin_processor_processblock_no_lock"
        target = "NOVAAudioProcessor::processBlock"
        passed = (-not $hasSpinLock)
        detail = if ($hasSpinLock) { "lock primitive found in processBlock body" } else { "no lock primitive in processBlock body" }
    }
    if ($hasSpinLock) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_plugin_processor_processblock_lock"
            file = Normalize-RelPath($pluginProcessorRel)
            function = "NOVAAudioProcessor::processBlock"
            detail = "NOVAAudioProcessor::processBlock must not acquire a lock on the audio path."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_plugin_processor_processblock_present"
        target = "NOVAAudioProcessor::processBlock"
        passed = $false
        detail = "processBlock range not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7b_plugin_processor_processblock_missing"
        file = Normalize-RelPath($pluginProcessorRel)
        function = "NOVAAudioProcessor::processBlock"
        detail = "NOVAAudioProcessor::processBlock range was not located by the policy scanner."
    }
}

$sessionStoreRel = "Source/Core/SessionStore.h"
$sessionStorePath = Join-Path $repoRoot $sessionStoreRel
if (Test-Path $sessionStorePath) {
    $sessionStoreText = Get-Content -LiteralPath $sessionStorePath -Raw

    $hasAtomicCache = ($sessionStoreText -match 'RuntimeGlobalParamAtomics\s+runtimeParamsCache') `
        -and ($sessionStoreText -match 'std::atomic<bool>\s+engineEnabledCache')
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_session_store_atomic_runtime_cache"
        target = "SessionStore"
        passed = $hasAtomicCache
        detail = if ($hasAtomicCache) { "atomic runtime cache + engine flag present" } else { "atomic runtime cache markers missing" }
    }
    if (-not $hasAtomicCache) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_session_store_runtime_cache_not_atomic"
            file = Normalize-RelPath($sessionStoreRel)
            function = "SessionStore"
            detail = "SessionStore must expose RuntimeGlobalParamAtomics + atomic<bool> engine cache."
        }
    }

    $usesSpinLockOnHotPath = $sessionStoreText -match 'SpinLock::ScopedLockType|juce::ScopedLock|CriticalSection'
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_session_store_no_lock_on_hot_path"
        target = "SessionStore"
        passed = (-not $usesSpinLockOnHotPath)
        detail = if ($usesSpinLockOnHotPath) { "lock primitive present in SessionStore" } else { "no lock primitive present" }
    }
    if ($usesSpinLockOnHotPath) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7b_session_store_lock_on_hot_path"
            file = Normalize-RelPath($sessionStoreRel)
            function = "SessionStore"
            detail = "SessionStore must keep audio-readable cache lock-free."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7b_session_store_atomic_runtime_cache"
        target = "SessionStore"
        passed = $false
        detail = "SessionStore.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7b_session_store_header_missing"
        file = Normalize-RelPath($sessionStoreRel)
        function = "SessionStore"
        detail = "SessionStore header is missing."
    }
}

# ==============================================================================
# P7C - Allocation Fallback and Feedback Stress Closure contract checks
#
# These lock down the audit P1 items: setSize-only-at-prepare for the P1 pedal
# set (Compressor, Distortion, Fuzz, Neural, ClassicWah, CabinetPedal,
# Vintage2x12Cabinet, Modern4x12Cabinet), cabinet IIR coefficient hygiene
# (ArrayCoefficients only), Neural helper preallocation (std::array, not
# std::vector), Phaser DC accumulation defenses, and the existence of the
# deterministic stress / DC / NaN tests in AudioEngineTests.cpp.
# ==============================================================================

$p7cP1PedalFiles = @(
    "Source/Effects/Pedals/Compressor/CompressorPedal.h",
    "Source/Effects/Pedals/Distortion/DistortionPedal.h",
    "Source/Effects/Pedals/Fuzz/FuzzPedal.h",
    "Source/Effects/Pedals/Neural/NeuralPedal.h",
    "Source/Effects/Pedals/Wah/ClassicWahPedal.h",
    "Source/Effects/Cabinets/CabinetPedal.h",
    "Source/Effects/Cabinets/Vintage2x12Cabinet.h",
    "Source/Effects/Cabinets/Modern4x12Cabinet.h"
)

foreach ($pedalRel in $p7cP1PedalFiles) {
    $pedalFull = Join-Path $repoRoot $pedalRel
    if (-not (Test-Path $pedalFull)) {
        $contractChecks += [pscustomobject]@{
            checkId = "p7c_p1_pedal_present"
            target = $pedalRel
            passed = $false
            detail = "file missing"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_missing"
            file = Normalize-RelPath($pedalRel)
            function = "P1 pedal"
            detail = "P7C P1 pedal file is missing."
        }
        continue
    }

    $ranges = Get-ProcessRanges -FilePath $pedalFull -Root $repoRoot
    $processRange = $ranges | Where-Object { $_.function -eq "processBlock" } | Select-Object -First 1

    if ($null -eq $processRange) {
        $contractChecks += [pscustomobject]@{
            checkId = "p7c_p1_pedal_processblock_present"
            target = $pedalRel
            passed = $false
            detail = "processBlock not found"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_processblock_missing"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P7C P1 pedal processBlock body could not be located."
        }
        continue
    }

    $processText = ($processRange.lines[($processRange.startLine - 1)..($processRange.endLine - 1)] -join "`n")

    $hasSetSizeInProcess = $processText -match '\.setSize\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_p1_pedal_no_setsize_in_process"
        target = $pedalRel
        passed = (-not $hasSetSizeInProcess)
        detail = if ($hasSetSizeInProcess) { "setSize call found inside processBlock body" } else { "no setSize call in processBlock body" }
    }
    if ($hasSetSizeInProcess) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_setsize_in_process"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P1 pedal must not call setSize on the audio path; preallocate at prepareToPlay only."
        }
    }

    $hasScratchSetSizeInProcess = $processText -match 'scratchBuffer\.setSize\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_p1_pedal_no_scratchbuffer_setsize_in_process"
        target = $pedalRel
        passed = (-not $hasScratchSetSizeInProcess)
        detail = if ($hasScratchSetSizeInProcess) { "scratchBuffer.setSize call found inside processBlock body" } else { "no scratchBuffer.setSize in processBlock body" }
    }
    if ($hasScratchSetSizeInProcess) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_scratchbuffer_setsize_in_process"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P1 pedal must not resize scratchBuffer from processBlock."
        }
    }

    $hasDrySetSizeInProcess = $processText -match 'dryBuffer\.setSize\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_p1_pedal_no_drybuffer_setsize_in_process"
        target = $pedalRel
        passed = (-not $hasDrySetSizeInProcess)
        detail = if ($hasDrySetSizeInProcess) { "dryBuffer.setSize call found inside processBlock body" } else { "no dryBuffer.setSize in processBlock body" }
    }
    if ($hasDrySetSizeInProcess) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_drybuffer_setsize_in_process"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P1 pedal must not resize dryBuffer from processBlock."
        }
    }

    $hasVectorResizeInProcess = $processText -match '\.(?:resize|assign|push_back)\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_p1_pedal_no_vector_resize_in_process"
        target = $pedalRel
        passed = (-not $hasVectorResizeInProcess)
        detail = if ($hasVectorResizeInProcess) { "resize/assign/push_back call found inside processBlock body" } else { "no resize/assign/push_back call in processBlock body" }
    }
    if ($hasVectorResizeInProcess) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_vector_resize_in_process"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P1 pedal must not resize vector-like storage from processBlock."
        }
    }

    $hasCanProcessBlockGuard = $processText -match 'canProcessBlock\s*\(\s*buffer\s*\)'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_p1_pedal_canprocessblock_guard"
        target = $pedalRel
        passed = $hasCanProcessBlockGuard
        detail = if ($hasCanProcessBlockGuard) { "canProcessBlock(buffer) guard present" } else { "canProcessBlock(buffer) guard missing" }
    }
    if (-not $hasCanProcessBlockGuard) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_canprocessblock_guard_missing"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P1 pedal processBlock must call canProcessBlock(buffer) before processing to enable the no-allocation fallback."
        }
    }

    $hasFallbackCounter = $processText -match 'fallbackBlockCount\.fetch_add'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_p1_pedal_fallback_counter"
        target = $pedalRel
        passed = $hasFallbackCounter
        detail = if ($hasFallbackCounter) { "fallbackBlockCount.fetch_add present" } else { "fallbackBlockCount.fetch_add missing" }
    }
    if (-not $hasFallbackCounter) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_p1_pedal_fallback_counter_missing"
            file = Normalize-RelPath($pedalRel)
            function = "processBlock"
            detail = "P1 pedal must increment fallbackBlockCount on the realtime fallback path."
        }
    }
}

$p7cCabinetFiles = @(
    "Source/Effects/Cabinets/CabinetPedal.h",
    "Source/Effects/Cabinets/Vintage2x12Cabinet.h",
    "Source/Effects/Cabinets/Modern4x12Cabinet.h"
)

foreach ($cabinetRel in $p7cCabinetFiles) {
    $cabinetFull = Join-Path $repoRoot $cabinetRel
    if (-not (Test-Path $cabinetFull)) {
        continue
    }

    $cabinetText = Get-Content -LiteralPath $cabinetFull -Raw

    $hasArrayCoefficients = $cabinetText -match 'IIR::ArrayCoefficients<float>::make'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_cabinet_uses_array_coefficients"
        target = $cabinetRel
        passed = $hasArrayCoefficients
        detail = if ($hasArrayCoefficients) { "IIR::ArrayCoefficients in use" } else { "no IIR::ArrayCoefficients usage" }
    }
    if (-not $hasArrayCoefficients) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_cabinet_missing_array_coefficients"
            file = Normalize-RelPath($cabinetRel)
            function = "cabinet"
            detail = "Cabinet voicing must use juce::dsp::IIR::ArrayCoefficients (value-type, no allocation)."
        }
    }

    $hasAllocatingFactory = [System.Text.RegularExpressions.Regex]::IsMatch($cabinetText, '(?<!Array)IIR::Coefficients<float>::make')
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_cabinet_no_juce_iir_factory"
        target = $cabinetRel
        passed = (-not $hasAllocatingFactory)
        detail = if ($hasAllocatingFactory) { "allocating IIR::Coefficients<float>::make found" } else { "no allocating IIR factory" }
    }
    if ($hasAllocatingFactory) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_cabinet_juce_iir_factory"
            file = Normalize-RelPath($cabinetRel)
            function = "cabinet"
            detail = "Cabinet must not call juce::dsp::IIR::Coefficients<float>::make* (use ArrayCoefficients value-type)."
        }
    }
}

$neuralRel = "Source/Effects/Pedals/Neural/NeuralPedal.h"
$neuralPath = Join-Path $repoRoot $neuralRel
if (Test-Path $neuralPath) {
    $neuralText = Get-Content -LiteralPath $neuralPath -Raw

    $hasArrayStates = $neuralText -match 'std::array<float,\s*kMaxHelperChannels>\s+states'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_neural_helpers_array_states"
        target = $neuralRel
        passed = $hasArrayStates
        detail = if ($hasArrayStates) { "OnePoleFilterBank/EnvelopeFollower use std::array states" } else { "std::array states declaration missing" }
    }
    if (-not $hasArrayStates) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_neural_helpers_states_not_array"
            file = Normalize-RelPath($neuralRel)
            function = "Nova::NeuralDSP"
            detail = "Neural OnePoleFilterBank / EnvelopeFollower must declare std::array<float, kMaxHelperChannels> states (no std::vector)."
        }
    }

    $hasVectorOfFloat = $neuralText -match 'std::vector\s*<\s*float\s*>'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_neural_helpers_no_vector_states"
        target = $neuralRel
        passed = (-not $hasVectorOfFloat)
        detail = if ($hasVectorOfFloat) { "std::vector<float> found in Neural file" } else { "no std::vector<float> in Neural file" }
    }
    if ($hasVectorOfFloat) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_neural_helpers_vector_present"
            file = Normalize-RelPath($neuralRel)
            function = "Nova::NeuralDSP"
            detail = "Neural helpers must not use std::vector<float> for per-channel state (preallocate std::array instead)."
        }
    }

    $hasResizeAssign = $neuralText -match 'states\.(?:resize|assign)\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_neural_helpers_no_resize_assign"
        target = $neuralRel
        passed = (-not $hasResizeAssign)
        detail = if ($hasResizeAssign) { "states.resize/assign call found" } else { "no states.resize or states.assign call" }
    }
    if ($hasResizeAssign) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_neural_helpers_resize_assign"
            file = Normalize-RelPath($neuralRel)
            function = "Nova::NeuralDSP"
            detail = "Neural helpers must not call resize/assign on states (allocates on the prepare path; std::array is fixed size)."
        }
    }

    $hasEnsureChannels = $neuralText -match '\bensureChannels\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_neural_helpers_no_ensurechannels_resize"
        target = $neuralRel
        passed = (-not $hasEnsureChannels)
        detail = if ($hasEnsureChannels) { "ensureChannels helper found" } else { "no ensureChannels helper" }
    }
    if ($hasEnsureChannels) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_neural_helpers_ensurechannels_present"
            file = Normalize-RelPath($neuralRel)
            function = "Nova::NeuralDSP"
            detail = "Neural OnePoleFilterBank / EnvelopeFollower must not resize through ensureChannels from the audio path."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_neural_file_present"
        target = $neuralRel
        passed = $false
        detail = "NeuralPedal.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7c_neural_file_missing"
        file = Normalize-RelPath($neuralRel)
        function = "NeuralPedal"
        detail = "NeuralPedal.h is missing."
    }
}

$phaserRel = "Source/Effects/Pedals/Phaser/PhaserPedal.h"
$phaserPath = Join-Path $repoRoot $phaserRel
if (Test-Path $phaserPath) {
    $phaserText = Get-Content -LiteralPath $phaserPath -Raw

    $hasFeedbackDcBlocker = $phaserText -match '\bfeedbackDcBlock\b'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_phaser_feedback_dc_blocker"
        target = $phaserRel
        passed = $hasFeedbackDcBlocker
        detail = if ($hasFeedbackDcBlocker) { "feedbackDcBlock present" } else { "feedbackDcBlock missing" }
    }
    if (-not $hasFeedbackDcBlocker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_phaser_feedback_dc_blocker_missing"
            file = Normalize-RelPath($phaserRel)
            function = "PhaserPedal"
            detail = "PhaserPedal must keep a feedback-path DC blocker to prevent DC accumulation under sustained bias."
        }
    }

    $hasOutputDcBlocker = $phaserText -match '\boutputDcBlock\b'
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_phaser_output_dc_blocker"
        target = $phaserRel
        passed = $hasOutputDcBlocker
        detail = if ($hasOutputDcBlocker) { "outputDcBlock present" } else { "outputDcBlock missing" }
    }
    if (-not $hasOutputDcBlocker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7c_phaser_output_dc_blocker_missing"
            file = Normalize-RelPath($phaserRel)
            function = "PhaserPedal"
            detail = "PhaserPedal must keep an output-path DC blocker to keep the wet signal centered."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_phaser_file_present"
        target = $phaserRel
        passed = $false
        detail = "PhaserPedal.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7c_phaser_file_missing"
        file = Normalize-RelPath($phaserRel)
        function = "PhaserPedal"
        detail = "PhaserPedal.h is missing."
    }
}

$audioTestRel = "Source/Core/AudioEngineTests.cpp"
$audioTestPath = Join-Path $repoRoot $audioTestRel
$p7cTestNames = @(
    "DelayPedal feedback loop rejects DC accumulation under sustained bias",
    "DelayPedal max feedback under sustained input stays bounded",
    "DelayPedal sanitizes NaN/Inf input under aggressive feedback",
    "DelayPedal high peak input under feedback stays bounded",
    "FlangerPedal feedback loop rejects DC accumulation under sustained bias",
    "FlangerPedal max feedback under sustained input stays bounded",
    "FlangerPedal sanitizes NaN/Inf input under aggressive feedback",
    "FlangerPedal high peak input under feedback stays bounded",
    "ReverbPedal max decay under sustained input stays bounded",
    "ReverbPedal sanitizes NaN/Inf input under aggressive feedback",
    "ReverbPedal high peak input under max decay stays bounded",
    "PhaserPedal sanitizes NaN/Inf input under aggressive feedback",
    "PhaserPedal feedback loop rejects DC accumulation under sustained bias",
    "ReverbPedal loop rejects DC accumulation under sustained biased playing"
)

if (Test-Path $audioTestPath) {
    $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw

    foreach ($testName in $p7cTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p7c_audio_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p7c_audio_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P7C stress/DC/NaN test '$testName'."
            }
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7c_audio_test_file_present"
        target = $audioTestRel
        passed = $false
        detail = "AudioEngineTests.cpp not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7c_audio_test_file_missing"
        file = Normalize-RelPath($audioTestRel)
        function = "AudioEngineTests"
        detail = "AudioEngineTests.cpp is missing."
    }
}

$p7dTestNames = @(
    "P7D preset save-load-save remains canonical",
    "P7D catalog presets round-trip every registered pedal",
    "P7D chain/global preset round-trips routing modes and bypass state",
    "P7D schema canonicalization rejects unknowns and clamps topology",
    "P7D parameter boundary restore clamps unsafe values",
    "P7D pedal state payload restore rejects corrupt payloads safely",
    "P7D corrupt session recovery leaves engine processable"
)

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    foreach ($testName in $p7dTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p7d_audio_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p7d_audio_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P7D preset/session/parameter robustness test '$testName'."
            }
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7d_audio_test_file_present"
        target = $audioTestRel
        passed = $false
        detail = "AudioEngineTests.cpp not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7d_audio_test_file_missing"
        file = Normalize-RelPath($audioTestRel)
        function = "AudioEngineTests"
        detail = "AudioEngineTests.cpp is missing."
    }
}

$constantsRel = "Source/Core/Constants.h"
$constantsPath = Join-Path $repoRoot $constantsRel
if (Test-Path $constantsPath) {
    $constantsText = Get-Content -LiteralPath $constantsPath -Raw
    $schemaUnchanged = $constantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p7d_schema_version_unchanged"
        target = $constantsRel
        passed = $schemaUnchanged
        detail = if ($schemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $schemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7d_schema_version_changed"
            file = Normalize-RelPath($constantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P7D must not bump schema without a documented migration and compatibility tests."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7d_schema_file_present"
        target = $constantsRel
        passed = $false
        detail = "Constants.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7d_schema_file_missing"
        file = Normalize-RelPath($constantsRel)
        function = "Nova::Config"
        detail = "Constants.h is missing."
    }
}

$p7dDocRel = "docs/p7d-preset-session-parameter-validation-results.md"
$p7dDocPath = Join-Path $repoRoot $p7dDocRel
$hasP7dDoc = Test-Path $p7dDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p7d_results_doc_present"
    target = $p7dDocRel
    passed = $hasP7dDoc
    detail = if ($hasP7dDoc) { "P7D results doc found" } else { "P7D results doc missing" }
}
if (-not $hasP7dDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p7d_results_doc_missing"
        file = Normalize-RelPath($p7dDocRel)
        function = "P7D documentation"
        detail = "P7D closure requires docs/p7d-preset-session-parameter-validation-results.md."
    }
}

$p7eDocs = @(
    [pscustomobject]@{ id = "p7e_smoke_matrix_doc_present"; path = "docs/p7e-daw-standalone-smoke-matrix.md"; label = "P7E smoke matrix" },
    [pscustomobject]@{ id = "p7e_manual_checklist_doc_present"; path = "docs/p7e-manual-host-smoke-checklist.md"; label = "P7E manual host smoke checklist" },
    [pscustomobject]@{ id = "p7e_report_template_doc_present"; path = "docs/p7e-host-smoke-report-template.md"; label = "P7E host smoke report template" },
    [pscustomobject]@{ id = "p7e_readiness_results_doc_present"; path = "docs/p7e-daw-standalone-smoke-readiness-results.md"; label = "P7E readiness results" }
)

foreach ($doc in $p7eDocs) {
    $docPath = Join-Path $repoRoot $doc.path
    $hasDoc = Test-Path $docPath
    $contractChecks += [pscustomobject]@{
        checkId = $doc.id
        target = $doc.path
        passed = $hasDoc
        detail = if ($hasDoc) { "$($doc.label) found" } else { "$($doc.label) missing" }
    }
    if (-not $hasDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = $doc.id -replace "_present$", "_missing"
            file = Normalize-RelPath($doc.path)
            function = "P7E documentation"
            detail = "$($doc.label) is required for P7E smoke readiness."
        }
    }
}

$p7ePreflightRel = "scripts/run-host-smoke-preflight.ps1"
$p7ePreflightPath = Join-Path $repoRoot $p7ePreflightRel
$hasP7ePreflight = Test-Path $p7ePreflightPath
$contractChecks += [pscustomobject]@{
    checkId = "p7e_host_smoke_preflight_script_present"
    target = $p7ePreflightRel
    passed = $hasP7ePreflight
    detail = if ($hasP7ePreflight) { "P7E host smoke preflight script found" } else { "P7E host smoke preflight script missing" }
}
if (-not $hasP7ePreflight) {
    $contractFailures += [pscustomobject]@{
        checkId = "p7e_host_smoke_preflight_script_missing"
        file = Normalize-RelPath($p7ePreflightRel)
        function = "P7E preflight"
        detail = "P7E requires either a safe preflight script or a documented reason not to create one."
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p7eHostStateTest = "P7E host state get/set survives corrupt and repeated engine toggles"
    $hasP7eHostStateTest = $audioTestText.Contains($p7eHostStateTest)
    $contractChecks += [pscustomobject]@{
        checkId = "p7e_host_state_test_present"
        target = $p7eHostStateTest
        passed = $hasP7eHostStateTest
        detail = if ($hasP7eHostStateTest) { "P7E host-state test found" } else { "P7E host-state test missing" }
    }
    if (-not $hasP7eHostStateTest) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7e_host_state_test_missing"
            file = Normalize-RelPath($audioTestRel)
            function = "AudioEngineValidationTests"
            detail = "AudioEngineTests.cpp must contain the P7E host-state bridge regression test."
        }
    }
}

$p7hDocRel = "docs/p7h-reverb-configure-automation-perf-audit-results.md"
$p7hDocPath = Join-Path $repoRoot $p7hDocRel
$hasP7hDoc = Test-Path $p7hDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p7h_reverb_audit_doc_present"
    target = $p7hDocRel
    passed = $hasP7hDoc
    detail = if ($hasP7hDoc) { "P7H Reverb configure audit doc found" } else { "P7H Reverb configure audit doc missing" }
}
if (-not $hasP7hDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p7h_reverb_audit_doc_missing"
        file = Normalize-RelPath($p7hDocRel)
        function = "P7H documentation"
        detail = "P7H closure requires docs/p7h-reverb-configure-automation-perf-audit-results.md."
    }
}

$p7hReverbRel = "Source/Effects/Pedals/Reverb/ReverbPedal.h"
$p7hReverbPath = Join-Path $repoRoot $p7hReverbRel
if (Test-Path $p7hReverbPath) {
    $p7hReverbText = Get-Content -LiteralPath $p7hReverbPath -Raw

    $hasConfigureCounter = $p7hReverbText.Contains("getReverbConfigureCallCount") -and $p7hReverbText.Contains("resetReverbConfigureDiagnostics")
    $contractChecks += [pscustomobject]@{
        checkId = "p7h_reverb_configure_diagnostics_present"
        target = $p7hReverbRel
        passed = $hasConfigureCounter
        detail = if ($hasConfigureCounter) { "configure diagnostics accessors found" } else { "configure diagnostics accessors missing" }
    }
    if (-not $hasConfigureCounter) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7h_reverb_configure_diagnostics_missing"
            file = Normalize-RelPath($p7hReverbRel)
            function = "ReverbPedal"
            detail = "P7H requires deterministic configure-call diagnostics for automation/perf tests."
        }
    }

    $p7hRanges = Get-ProcessRanges -FilePath $p7hReverbPath -Root $repoRoot
    $p7hProcessRange = $p7hRanges | Where-Object { $_.function -eq "processBlock" } | Select-Object -First 1
    if ($null -ne $p7hProcessRange) {
        $p7hProcessText = ($p7hProcessRange.lines[($p7hProcessRange.startLine - 1)..($p7hProcessRange.endLine - 1)] -join "`n")
        $hasProcessLogging = $p7hProcessText -match 'SessionLogger::|juce::String|Logger::|DBG\s*\('
        $contractChecks += [pscustomobject]@{
            checkId = "p7h_reverb_processblock_no_logging_or_string"
            target = "ReverbPedal::processBlock"
            passed = (-not $hasProcessLogging)
            detail = if ($hasProcessLogging) { "logging/String pattern found in processBlock" } else { "no logging/String pattern in processBlock" }
        }
        if ($hasProcessLogging) {
            $contractFailures += [pscustomobject]@{
                checkId = "p7h_reverb_processblock_logging_or_string"
                file = Normalize-RelPath($p7hReverbRel)
                function = "ReverbPedal::processBlock"
                detail = "ReverbPedal::processBlock must not log or construct strings on the audio path."
            }
        }
    }
    else {
        $contractChecks += [pscustomobject]@{
            checkId = "p7h_reverb_processblock_present"
            target = "ReverbPedal::processBlock"
            passed = $false
            detail = "processBlock not found"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p7h_reverb_processblock_missing"
            file = Normalize-RelPath($p7hReverbRel)
            function = "ReverbPedal::processBlock"
            detail = "ReverbPedal::processBlock range was not located by the policy scanner."
        }
    }

    $configureStart = $p7hReverbText.IndexOf("void configure(int mode, float decay01")
    $configureEnd = if ($configureStart -ge 0) { $p7hReverbText.IndexOf("void processSample", $configureStart) } else { -1 }
    $configureText = if ($configureStart -ge 0 -and $configureEnd -gt $configureStart) {
        $p7hReverbText.Substring($configureStart, $configureEnd - $configureStart)
    } else {
        ""
    }

    $hasConfigureSection = $configureText.Length -gt 0
    $contractChecks += [pscustomobject]@{
        checkId = "p7h_reverb_configure_section_present"
        target = "Nova::Reverb::Engine::configure"
        passed = $hasConfigureSection
        detail = if ($hasConfigureSection) { "configure section found" } else { "configure section missing" }
    }
    if (-not $hasConfigureSection) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7h_reverb_configure_section_missing"
            file = Normalize-RelPath($p7hReverbRel)
            function = "Nova::Reverb::Engine::configure"
            detail = "P7H policy could not locate Engine::configure."
        }
    }
    else {
        $hasAllocPattern = $configureText -match '\.(assign|resize|reserve|setSize)\s*\(|\bnew\s+|std::make_unique|std::make_shared'
        $contractChecks += [pscustomobject]@{
            checkId = "p7h_reverb_configure_no_obvious_allocation"
            target = "Nova::Reverb::Engine::configure"
            passed = (-not $hasAllocPattern)
            detail = if ($hasAllocPattern) { "obvious allocation pattern found in configure" } else { "no obvious allocation pattern in configure" }
        }
        if ($hasAllocPattern) {
            $contractFailures += [pscustomobject]@{
                checkId = "p7h_reverb_configure_obvious_allocation"
                file = Normalize-RelPath($p7hReverbRel)
                function = "Nova::Reverb::Engine::configure"
                detail = "Reverb configure must not allocate on the audio path."
            }
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7h_reverb_file_present"
        target = $p7hReverbRel
        passed = $false
        detail = "ReverbPedal.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7h_reverb_file_missing"
        file = Normalize-RelPath($p7hReverbRel)
        function = "ReverbPedal"
        detail = "ReverbPedal.h is missing."
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p7hTestNames = @(
        "P7H ReverbPedal configure is not called every block under stable params",
        "P7H ReverbPedal aggressive automation sweep remains finite and bounded",
        "P7H ReverbPedal mode changes remain bounded",
        "P7H ReverbPedal freeze gate reverse swell automation remains bounded"
    )

    foreach ($testName in $p7hTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p7h_reverb_automation_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p7h_reverb_automation_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P7H Reverb automation/perf test '$testName'."
            }
        }
    }
}

# ---------------------------------------------------------------------------
# P8A - DistortionPedal Surgery & Containment
# ---------------------------------------------------------------------------

$p8aDocRel = "docs/p8a-distortionpedal-surgery-containment-results.md"
$p8aDocPath = Join-Path $repoRoot $p8aDocRel
$hasP8aDoc = Test-Path $p8aDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p8a_distortion_containment_doc_present"
    target = $p8aDocRel
    passed = $hasP8aDoc
    detail = if ($hasP8aDoc) { "P8A Distortion containment doc found" } else { "P8A Distortion containment doc missing" }
}
if (-not $hasP8aDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8a_distortion_containment_doc_missing"
        file = Normalize-RelPath($p8aDocRel)
        function = "P8A documentation"
        detail = "P8A closure requires docs/p8a-distortionpedal-surgery-containment-results.md."
    }
}

$p8aDistortionRel = "Source/Effects/Pedals/Distortion/DistortionPedal.h"
$p8aDistortionPath = Join-Path $repoRoot $p8aDistortionRel
if (Test-Path $p8aDistortionPath) {
    $p8aDistortionText = Get-Content -LiteralPath $p8aDistortionPath -Raw

    $hasContainmentHelpers = $p8aDistortionText.Contains("containOutputSample") `
        -and ($p8aDistortionText.Contains("softCeiling(value, 0.995f)") -or $p8aDistortionText.Contains("softCeiling(value, 0.94f)")) `
        -and $p8aDistortionText.Contains("finalDcBlock")
    $contractChecks += [pscustomobject]@{
        checkId = "p8a_distortion_internal_containment_present"
        target = $p8aDistortionRel
        passed = $hasContainmentHelpers
        detail = if ($hasContainmentHelpers) { "post-level soft ceiling and final DC blocker present" } else { "containment/DC markers missing" }
    }
    if (-not $hasContainmentHelpers) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8a_distortion_internal_containment_missing"
            file = Normalize-RelPath($p8aDistortionRel)
            function = "DistortionPedal"
            detail = "DistortionPedal must keep P8A post-level soft ceiling and final DC blocker containment."
        }
    }

    $p8aRanges = Get-ProcessRanges -FilePath $p8aDistortionPath -Root $repoRoot
    $p8aProcessRange = $p8aRanges | Where-Object { $_.function -eq "processBlock" } | Select-Object -First 1
    if ($null -ne $p8aProcessRange) {
        $p8aProcessText = ($p8aProcessRange.lines[($p8aProcessRange.startLine - 1)..($p8aProcessRange.endLine - 1)] -join "`n")
        $hasProcessLogging = $p8aProcessText -match 'SessionLogger::|juce::String|Logger::|DBG\s*\('
        $contractChecks += [pscustomobject]@{
            checkId = "p8a_distortion_processblock_no_logging_or_string"
            target = "DistortionPedal::processBlock"
            passed = (-not $hasProcessLogging)
            detail = if ($hasProcessLogging) { "logging/String pattern found in processBlock" } else { "no logging/String pattern in processBlock" }
        }
        if ($hasProcessLogging) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8a_distortion_processblock_logging_or_string"
                file = Normalize-RelPath($p8aDistortionRel)
                function = "DistortionPedal::processBlock"
                detail = "DistortionPedal::processBlock must not log or construct strings on the audio path."
            }
        }

        $hasObviousAllocation = $p8aProcessText -match '\.setSize\s*\(|\.(?:resize|reserve|assign|push_back)\s*\(|\bnew\s+|std::make_unique|std::make_shared'
        $contractChecks += [pscustomobject]@{
            checkId = "p8a_distortion_processblock_no_obvious_allocation"
            target = "DistortionPedal::processBlock"
            passed = (-not $hasObviousAllocation)
            detail = if ($hasObviousAllocation) { "obvious allocation pattern found in processBlock" } else { "no obvious allocation pattern in processBlock" }
        }
        if ($hasObviousAllocation) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8a_distortion_processblock_obvious_allocation"
                file = Normalize-RelPath($p8aDistortionRel)
                function = "DistortionPedal::processBlock"
                detail = "DistortionPedal::processBlock must not allocate on the audio path."
            }
        }
    }
    else {
        $contractChecks += [pscustomobject]@{
            checkId = "p8a_distortion_processblock_present"
            target = "DistortionPedal::processBlock"
            passed = $false
            detail = "processBlock not found"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p8a_distortion_processblock_missing"
            file = Normalize-RelPath($p8aDistortionRel)
            function = "DistortionPedal::processBlock"
            detail = "DistortionPedal::processBlock range was not located by the policy scanner."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p8a_distortion_file_present"
        target = $p8aDistortionRel
        passed = $false
        detail = "DistortionPedal.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p8a_distortion_file_missing"
        file = Normalize-RelPath($p8aDistortionRel)
        function = "DistortionPedal"
        detail = "DistortionPedal.h is missing."
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p8aTestNames = @(
        "P8A DistortionPedal metal high-gain output stays bounded before downstream ambience",
        "P8A DistortionPedal rejects DC accumulation under biased high-gain input",
        "P8A Distortion Reverb Chorus bypass recovery stays bounded",
        "DistortionPedal mix zero keeps the dry path transparent",
        "DistortionPedal modes produce distinct drive signatures",
        "DistortionPedal automation stress remains finite under aggressive changes",
        "DistortionPedal round-trips its commercial state"
    )

    foreach ($testName in $p8aTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p8a_distortion_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8a_distortion_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P8A Distortion containment/regression test '$testName'."
            }
        }
    }
}

$baseValidationScriptRel = "scripts/run-base-audio-validation.ps1"
$baseValidationScriptPath = Join-Path $repoRoot $baseValidationScriptRel
if (Test-Path $baseValidationScriptPath) {
    $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    $hasKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p8a_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $hasKnownFailureBypass)
        detail = if ($hasKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8a_known_failure_ignore_present"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "base validation"
            detail = "P8A must not add known-failure ignores to base validation."
        }
    }
}

$gitChangedFiles = @()
try {
    $gitChangedFiles = & git -C $repoRoot diff --name-only --
}
catch {
    $gitChangedFiles = @()
}
$goldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p8a_no_golden_baseline_update"
    target = "git diff"
    passed = ($goldenBaselineChanged.Count -eq 0)
    detail = if ($goldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($goldenBaselineChanged -join ",") }
}
if ($goldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8a_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P8A baseline guard"
        detail = "P8A must not update golden baseline files."
    }
}

# ---------------------------------------------------------------------------
# P8B - DistortionPedal Focused QA & Real-Use Verification
# ---------------------------------------------------------------------------

$p8bDocRel = "docs/p8b-distortion-focused-qa-results.md"
$p8bDocPath = Join-Path $repoRoot $p8bDocRel
$hasP8bDoc = Test-Path $p8bDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p8b_distortion_focused_qa_doc_present"
    target = $p8bDocRel
    passed = $hasP8bDoc
    detail = if ($hasP8bDoc) { "P8B Distortion focused QA doc found" } else { "P8B Distortion focused QA doc missing" }
}
if (-not $hasP8bDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8b_distortion_focused_qa_doc_missing"
        file = Normalize-RelPath($p8bDocRel)
        function = "P8B documentation"
        detail = "P8B closure requires docs/p8b-distortion-focused-qa-results.md."
    }
}

$p8bManualDocRel = "docs/p8b-distortion-focused-manual-qa-checklist.md"
$p8bManualDocPath = Join-Path $repoRoot $p8bManualDocRel
$hasP8bManualDoc = Test-Path $p8bManualDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p8b_distortion_manual_qa_checklist_present"
    target = $p8bManualDocRel
    passed = $hasP8bManualDoc
    detail = if ($hasP8bManualDoc) { "P8B manual QA checklist found" } else { "P8B manual QA checklist missing" }
}
if (-not $hasP8bManualDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8b_distortion_manual_qa_checklist_missing"
        file = Normalize-RelPath($p8bManualDocRel)
        function = "P8B documentation"
        detail = "P8B closure requires docs/p8b-distortion-focused-manual-qa-checklist.md."
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p8bTestNames = @(
        "P8B Distortion nominal modes remain expressive below containment knee",
        "P8B Distortion level mix gain sweep remains bounded and audible",
        "P8B Distortion into amp and cabinet remains bounded",
        "P8B Distortion bypass unbypass stays click bounded",
        "P8A Distortion Reverb Chorus bypass recovery stays bounded"
    )

    foreach ($testName in $p8bTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p8b_distortion_real_use_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8b_distortion_real_use_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P8B Distortion focused QA test '$testName'."
            }
        }
    }
}

if (Test-Path $p8aDistortionPath) {
    if ($null -eq $p8aDistortionText) {
        $p8aDistortionText = Get-Content -LiteralPath $p8aDistortionPath -Raw
    }

    $p8bRanges = Get-ProcessRanges -FilePath $p8aDistortionPath -Root $repoRoot
    $p8bProcessRange = $p8bRanges | Where-Object { $_.function -eq "processBlock" } | Select-Object -First 1
    if ($null -ne $p8bProcessRange) {
        $p8bProcessText = ($p8bProcessRange.lines[($p8bProcessRange.startLine - 1)..($p8bProcessRange.endLine - 1)] -join "`n")
        $hasP8bProcessLogging = $p8bProcessText -match 'SessionLogger::|juce::String|Logger::|DBG\s*\('
        $contractChecks += [pscustomobject]@{
            checkId = "p8b_distortion_processblock_no_logging_or_string"
            target = "DistortionPedal::processBlock"
            passed = (-not $hasP8bProcessLogging)
            detail = if ($hasP8bProcessLogging) { "logging/String pattern found in processBlock" } else { "no logging/String pattern in processBlock" }
        }
        if ($hasP8bProcessLogging) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8b_distortion_processblock_logging_or_string"
                file = Normalize-RelPath($p8aDistortionRel)
                function = "DistortionPedal::processBlock"
                detail = "DistortionPedal::processBlock must not log or construct strings on the audio path."
            }
        }

        $hasP8bObviousAllocation = $p8bProcessText -match '\.setSize\s*\(|\.(?:resize|reserve|assign|push_back)\s*\(|\bnew\s+|std::make_unique|std::make_shared'
        $contractChecks += [pscustomobject]@{
            checkId = "p8b_distortion_processblock_no_obvious_allocation"
            target = "DistortionPedal::processBlock"
            passed = (-not $hasP8bObviousAllocation)
            detail = if ($hasP8bObviousAllocation) { "obvious allocation pattern found in processBlock" } else { "no obvious allocation pattern in processBlock" }
        }
        if ($hasP8bObviousAllocation) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8b_distortion_processblock_obvious_allocation"
                file = Normalize-RelPath($p8aDistortionRel)
                function = "DistortionPedal::processBlock"
                detail = "DistortionPedal::processBlock must not allocate on the audio path."
            }
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP8bKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p8b_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $hasP8bKnownFailureBypass)
        detail = if ($hasP8bKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP8bKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8b_known_failure_ignore_present"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "base validation"
            detail = "P8B must not add known-failure ignores to base validation."
        }
    }
}

$p8bGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p8b_no_golden_baseline_update"
    target = "git diff"
    passed = ($p8bGoldenBaselineChanged.Count -eq 0)
    detail = if ($p8bGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p8bGoldenBaselineChanged -join ",") }
}
if ($p8bGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8b_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P8B baseline guard"
        detail = "P8B must not update golden baseline files."
    }
}

# ---------------------------------------------------------------------------
# P8C - Pedal-by-pedal Technical QA Matrix
# ---------------------------------------------------------------------------

$p8cMatrixRel = "docs/p8c-pedal-by-pedal-technical-qa-matrix.md"
$p8cMatrixPath = Join-Path $repoRoot $p8cMatrixRel
$hasP8cMatrix = Test-Path $p8cMatrixPath
$contractChecks += [pscustomobject]@{
    checkId = "p8c_pedal_technical_matrix_present"
    target = $p8cMatrixRel
    passed = $hasP8cMatrix
    detail = if ($hasP8cMatrix) { "P8C pedal technical matrix found" } else { "P8C pedal technical matrix missing" }
}
if (-not $hasP8cMatrix) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8c_pedal_technical_matrix_missing"
        file = Normalize-RelPath($p8cMatrixRel)
        function = "P8C documentation"
        detail = "P8C closure requires docs/p8c-pedal-by-pedal-technical-qa-matrix.md."
    }
}

$p8cResultsRel = "docs/p8c-pedal-by-pedal-technical-qa-results.md"
$p8cResultsPath = Join-Path $repoRoot $p8cResultsRel
$hasP8cResults = Test-Path $p8cResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p8c_pedal_technical_results_present"
    target = $p8cResultsRel
    passed = $hasP8cResults
    detail = if ($hasP8cResults) { "P8C pedal technical results found" } else { "P8C pedal technical results missing" }
}
if (-not $hasP8cResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8c_pedal_technical_results_missing"
        file = Normalize-RelPath($p8cResultsRel)
        function = "P8C documentation"
        detail = "P8C closure requires docs/p8c-pedal-by-pedal-technical-qa-results.md."
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p8cTestNames = @(
        "P8C active pedal catalog processors remain finite under strong input",
        "P8C active pedal catalog automation extremes remain finite",
        "P8C active pedal catalog bypass transitions remain bounded"
    )

    foreach ($testName in $p8cTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p8c_pedal_matrix_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8c_pedal_matrix_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P8C pedal matrix test '$testName'."
            }
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP8cKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p8c_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $hasP8cKnownFailureBypass)
        detail = if ($hasP8cKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP8cKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8c_known_failure_ignore_present"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "base validation"
            detail = "P8C must not add known-failure ignores to base validation."
        }
    }
}

$p8cGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p8c_no_golden_baseline_update"
    target = "git diff"
    passed = ($p8cGoldenBaselineChanged.Count -eq 0)
    detail = if ($p8cGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p8cGoldenBaselineChanged -join ",") }
}
if ($p8cGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8c_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P8C baseline guard"
        detail = "P8C must not update golden baseline files."
    }
}

$p8cConstantsRel = "Source/Core/Constants.h"
$p8cConstantsPath = Join-Path $repoRoot $p8cConstantsRel
if (Test-Path $p8cConstantsPath) {
    $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    $p8cSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p8c_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p8cSchemaUnchanged
        detail = if ($p8cSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p8cSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8c_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P8C must not bump schema."
        }
    }
}

$p8cLegacyMarkers = @(
    "Source/Effects/Pedals/ChorusPedal.h",
    "Source/Effects/Pedals/CompressorPedal.h",
    "Source/Effects/Pedals/Wah/AutoWahPedal.h",
    "Source/Effects/Pedals/Metal/MetalDistortionPedal.h"
)
$policySelfText = Get-Content -LiteralPath $PSCommandPath -Raw
$hasP8cLegacyQuarantine = $true
foreach ($marker in $p8cLegacyMarkers) {
    if (-not $policySelfText.Contains($marker)) {
        $hasP8cLegacyQuarantine = $false
    }
}
$contractChecks += [pscustomobject]@{
    checkId = "p8c_legacy_quarantine_contract_present"
    target = "P7G legacy quarantine"
    passed = $hasP8cLegacyQuarantine
    detail = if ($hasP8cLegacyQuarantine) { "legacy quarantine markers remain present" } else { "one or more legacy quarantine markers missing" }
}
if (-not $hasP8cLegacyQuarantine) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8c_legacy_quarantine_contract_missing"
        file = Normalize-RelPath("scripts/check-audio-thread-policy.ps1")
        function = "P8C legacy quarantine guard"
        detail = "P8C requires the P7G legacy quarantine contract to remain active."
    }
}

if ($hasP8cMatrix -and $hasP8cResults) {
    $p8cDocText = (Get-Content -LiteralPath $p8cMatrixPath -Raw) + "`n" + (Get-Content -LiteralPath $p8cResultsPath -Raw)
    $manualListeningPending = $p8cDocText -match 'Distortion manual listening QA remains pending|Distortion manual listening QA queda pendiente'
    $manualListeningMarkedPass = $p8cDocText -match 'Distortion manual listening QA\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p8c_distortion_manual_listening_not_auto_passed"
        target = "P8C docs"
        passed = ($manualListeningPending -and -not $manualListeningMarkedPass)
        detail = if ($manualListeningPending -and -not $manualListeningMarkedPass) { "Distortion manual listening QA remains pending" } else { "Distortion manual listening QA pending marker missing or marked PASS" }
    }
    if (-not ($manualListeningPending -and -not $manualListeningMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8c_distortion_manual_listening_auto_passed"
            file = Normalize-RelPath($p8cResultsRel)
            function = "P8C documentation"
            detail = "P8C must not mark Distortion manual listening QA as PASS."
        }
    }
}

# ---------------------------------------------------------------------------
# P8D - Targeted Pedal Gap Closure
# ---------------------------------------------------------------------------

$p8dMatrixRel = "docs/p8d-targeted-pedal-gap-closure-matrix.md"
$p8dMatrixPath = Join-Path $repoRoot $p8dMatrixRel
$hasP8dMatrix = Test-Path $p8dMatrixPath
$contractChecks += [pscustomobject]@{
    checkId = "p8d_targeted_gap_matrix_present"
    target = $p8dMatrixRel
    passed = $hasP8dMatrix
    detail = if ($hasP8dMatrix) { "P8D targeted gap matrix found" } else { "P8D targeted gap matrix missing" }
}
if (-not $hasP8dMatrix) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8d_targeted_gap_matrix_missing"
        file = Normalize-RelPath($p8dMatrixRel)
        function = "P8D documentation"
        detail = "P8D closure requires docs/p8d-targeted-pedal-gap-closure-matrix.md."
    }
}

$p8dResultsRel = "docs/p8d-targeted-pedal-gap-closure-results.md"
$p8dResultsPath = Join-Path $repoRoot $p8dResultsRel
$hasP8dResults = Test-Path $p8dResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p8d_targeted_gap_results_present"
    target = $p8dResultsRel
    passed = $hasP8dResults
    detail = if ($hasP8dResults) { "P8D targeted gap results found" } else { "P8D targeted gap results missing" }
}
if (-not $hasP8dResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8d_targeted_gap_results_missing"
        file = Normalize-RelPath($p8dResultsRel)
        function = "P8D documentation"
        detail = "P8D closure requires docs/p8d-targeted-pedal-gap-closure-results.md."
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p8dTestNames = @(
        "P8D Wah sweep resonance bias and bypass remain bounded",
        "P8D amp variants targeted strong input automation and bypass remain bounded",
        "P8D cabinet variants and high-gain chains remain bounded"
    )

    foreach ($testName in $p8dTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p8d_targeted_gap_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p8d_targeted_gap_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "AudioEngineValidationTests"
                detail = "AudioEngineTests.cpp must contain the P8D targeted gap test '$testName'."
            }
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP8dKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p8d_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $hasP8dKnownFailureBypass)
        detail = if ($hasP8dKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP8dKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8d_known_failure_ignore_present"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "base validation"
            detail = "P8D must not add known-failure ignores to base validation."
        }
    }
}

$p8dGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p8d_no_golden_baseline_update"
    target = "git diff"
    passed = ($p8dGoldenBaselineChanged.Count -eq 0)
    detail = if ($p8dGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p8dGoldenBaselineChanged -join ",") }
}
if ($p8dGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8d_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P8D baseline guard"
        detail = "P8D must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p8dSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p8d_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p8dSchemaUnchanged
        detail = if ($p8dSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p8dSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8d_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P8D must not bump schema."
        }
    }
}

$contractChecks += [pscustomobject]@{
    checkId = "p8d_legacy_quarantine_contract_present"
    target = "P7G legacy quarantine"
    passed = $hasP8cLegacyQuarantine
    detail = if ($hasP8cLegacyQuarantine) { "legacy quarantine markers remain present" } else { "one or more legacy quarantine markers missing" }
}
if (-not $hasP8cLegacyQuarantine) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8d_legacy_quarantine_contract_missing"
        file = Normalize-RelPath("scripts/check-audio-thread-policy.ps1")
        function = "P8D legacy quarantine guard"
        detail = "P8D requires the P7G legacy quarantine contract to remain active."
    }
}

if ($hasP8dMatrix -and $hasP8dResults) {
    $p8dDocText = (Get-Content -LiteralPath $p8dMatrixPath -Raw) + "`n" + (Get-Content -LiteralPath $p8dResultsPath -Raw)
    $p8dManualListeningPending = $p8dDocText -match 'Distortion manual listening QA remains pending|Distortion manual listening QA queda pendiente'
    $p8dManualListeningMarkedPass = $p8dDocText -match 'Distortion manual listening QA\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p8d_distortion_manual_listening_not_auto_passed"
        target = "P8D docs"
        passed = ($p8dManualListeningPending -and -not $p8dManualListeningMarkedPass)
        detail = if ($p8dManualListeningPending -and -not $p8dManualListeningMarkedPass) { "Distortion manual listening QA remains pending" } else { "Distortion manual listening QA pending marker missing or marked PASS" }
    }
    if (-not ($p8dManualListeningPending -and -not $p8dManualListeningMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8d_distortion_manual_listening_auto_passed"
            file = Normalize-RelPath($p8dResultsRel)
            function = "P8D documentation"
            detail = "P8D must not mark Distortion manual listening QA as PASS."
        }
    }

    $p8dP7fPending = $p8dDocText -match 'P7F/Reaper remains pending|P7F/Reaper.*pendiente'
    $p8dP7fMarkedPass = $p8dDocText -match 'P7F/Reaper\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p8d_p7f_reaper_not_marked_pass"
        target = "P8D docs"
        passed = ($p8dP7fPending -and -not $p8dP7fMarkedPass)
        detail = if ($p8dP7fPending -and -not $p8dP7fMarkedPass) { "P7F/Reaper remains pending" } else { "P7F/Reaper pending marker missing or marked PASS" }
    }
    if (-not ($p8dP7fPending -and -not $p8dP7fMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8d_p7f_reaper_marked_pass"
            file = Normalize-RelPath($p8dResultsRel)
            function = "P8D documentation"
            detail = "P8D must not mark P7F/Reaper as PASS."
        }
    }
}

# ---------------------------------------------------------------------------
# P8E - Manual Listening QA Readiness
# ---------------------------------------------------------------------------

$p8eMatrixRel = "docs/p8e-manual-listening-qa-matrix.md"
$p8eMatrixPath = Join-Path $repoRoot $p8eMatrixRel
$hasP8eMatrix = Test-Path $p8eMatrixPath
$contractChecks += [pscustomobject]@{
    checkId = "p8e_manual_listening_matrix_present"
    target = $p8eMatrixRel
    passed = $hasP8eMatrix
    detail = if ($hasP8eMatrix) { "P8E manual listening matrix found" } else { "P8E manual listening matrix missing" }
}
if (-not $hasP8eMatrix) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8e_manual_listening_matrix_missing"
        file = Normalize-RelPath($p8eMatrixRel)
        function = "P8E documentation"
        detail = "P8E readiness requires docs/p8e-manual-listening-qa-matrix.md."
    }
}

$p8eChecklistRel = "docs/p8e-manual-listening-session-checklist.md"
$p8eChecklistPath = Join-Path $repoRoot $p8eChecklistRel
$hasP8eChecklist = Test-Path $p8eChecklistPath
$contractChecks += [pscustomobject]@{
    checkId = "p8e_manual_listening_session_checklist_present"
    target = $p8eChecklistRel
    passed = $hasP8eChecklist
    detail = if ($hasP8eChecklist) { "P8E manual listening session checklist found" } else { "P8E manual listening session checklist missing" }
}
if (-not $hasP8eChecklist) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8e_manual_listening_session_checklist_missing"
        file = Normalize-RelPath($p8eChecklistRel)
        function = "P8E documentation"
        detail = "P8E readiness requires docs/p8e-manual-listening-session-checklist.md."
    }
}

$p8eIssueTemplateRel = "docs/p8e-listening-qa-issue-template.md"
$p8eIssueTemplatePath = Join-Path $repoRoot $p8eIssueTemplateRel
$hasP8eIssueTemplate = Test-Path $p8eIssueTemplatePath
$contractChecks += [pscustomobject]@{
    checkId = "p8e_listening_qa_issue_template_present"
    target = $p8eIssueTemplateRel
    passed = $hasP8eIssueTemplate
    detail = if ($hasP8eIssueTemplate) { "P8E listening QA issue template found" } else { "P8E listening QA issue template missing" }
}
if (-not $hasP8eIssueTemplate) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8e_listening_qa_issue_template_missing"
        file = Normalize-RelPath($p8eIssueTemplateRel)
        function = "P8E documentation"
        detail = "P8E readiness requires docs/p8e-listening-qa-issue-template.md."
    }
}

$p8eResultsRel = "docs/p8e-manual-listening-qa-readiness-results.md"
$p8eResultsPath = Join-Path $repoRoot $p8eResultsRel
$hasP8eResults = Test-Path $p8eResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p8e_manual_listening_readiness_results_present"
    target = $p8eResultsRel
    passed = $hasP8eResults
    detail = if ($hasP8eResults) { "P8E readiness results found" } else { "P8E readiness results missing" }
}
if (-not $hasP8eResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8e_manual_listening_readiness_results_missing"
        file = Normalize-RelPath($p8eResultsRel)
        function = "P8E documentation"
        detail = "P8E readiness requires docs/p8e-manual-listening-qa-readiness-results.md."
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP8eKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p8e_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $hasP8eKnownFailureBypass)
        detail = if ($hasP8eKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP8eKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8e_known_failure_ignore_present"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "base validation"
            detail = "P8E must not add known-failure ignores to base validation."
        }
    }
}

$p8eGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p8e_no_golden_baseline_update"
    target = "git diff"
    passed = ($p8eGoldenBaselineChanged.Count -eq 0)
    detail = if ($p8eGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p8eGoldenBaselineChanged -join ",") }
}
if ($p8eGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p8e_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P8E baseline guard"
        detail = "P8E must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p8eSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p8e_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p8eSchemaUnchanged
        detail = if ($p8eSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p8eSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8e_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P8E must not bump schema."
        }
    }
}

if ($hasP8eMatrix -and $hasP8eChecklist -and $hasP8eIssueTemplate -and $hasP8eResults) {
    $p8eDocText = (Get-Content -LiteralPath $p8eMatrixPath -Raw) + "`n" +
        (Get-Content -LiteralPath $p8eChecklistPath -Raw) + "`n" +
        (Get-Content -LiteralPath $p8eIssueTemplatePath -Raw) + "`n" +
        (Get-Content -LiteralPath $p8eResultsPath -Raw)

    $p8eDistortionPending = $p8eDocText -match 'Distortion manual listening QA remains pending|Distortion manual listening QA queda pendiente'
    $p8eDistortionMarkedPass = $p8eDocText -match 'Distortion manual listening QA\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p8e_distortion_manual_listening_not_marked_pass"
        target = "P8E docs"
        passed = ($p8eDistortionPending -and -not $p8eDistortionMarkedPass)
        detail = if ($p8eDistortionPending -and -not $p8eDistortionMarkedPass) { "Distortion manual listening QA remains pending" } else { "Distortion manual listening QA pending marker missing or marked PASS" }
    }
    if (-not ($p8eDistortionPending -and -not $p8eDistortionMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8e_distortion_manual_listening_marked_pass"
            file = Normalize-RelPath($p8eResultsRel)
            function = "P8E documentation"
            detail = "P8E must not mark Distortion manual listening QA as PASS."
        }
    }

    $p8eP7fPending = $p8eDocText -match 'P7F/Reaper remains pending|P7F/Reaper.*pendiente'
    $p8eP7fMarkedPass = $p8eDocText -match 'P7F/Reaper\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p8e_p7f_reaper_not_marked_pass"
        target = "P8E docs"
        passed = ($p8eP7fPending -and -not $p8eP7fMarkedPass)
        detail = if ($p8eP7fPending -and -not $p8eP7fMarkedPass) { "P7F/Reaper remains pending" } else { "P7F/Reaper pending marker missing or marked PASS" }
    }
    if (-not ($p8eP7fPending -and -not $p8eP7fMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p8e_p7f_reaper_marked_pass"
            file = Normalize-RelPath($p8eResultsRel)
            function = "P8E documentation"
            detail = "P8E must not mark P7F/Reaper as PASS."
        }
    }
}

# ---------------------------------------------------------------------------
# P9 - Factory Presets / Gain Staging Framework
# ---------------------------------------------------------------------------

$p9FrameworkRel = "docs/p9-factory-presets-gain-staging-framework.md"
$p9FrameworkPath = Join-Path $repoRoot $p9FrameworkRel
$hasP9Framework = Test-Path $p9FrameworkPath
$contractChecks += [pscustomobject]@{
    checkId = "p9_factory_preset_framework_doc_present"
    target = $p9FrameworkRel
    passed = $hasP9Framework
    detail = if ($hasP9Framework) { "P9 factory presets framework found" } else { "P9 factory presets framework missing" }
}
if (-not $hasP9Framework) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9_factory_preset_framework_doc_missing"
        file = Normalize-RelPath($p9FrameworkRel)
        function = "P9 documentation"
        detail = "P9 requires docs/p9-factory-presets-gain-staging-framework.md."
    }
}

$p9ChecklistRel = "docs/p9-factory-preset-qa-checklist.md"
$p9ChecklistPath = Join-Path $repoRoot $p9ChecklistRel
$hasP9Checklist = Test-Path $p9ChecklistPath
$contractChecks += [pscustomobject]@{
    checkId = "p9_factory_preset_qa_checklist_present"
    target = $p9ChecklistRel
    passed = $hasP9Checklist
    detail = if ($hasP9Checklist) { "P9 factory preset QA checklist found" } else { "P9 factory preset QA checklist missing" }
}
if (-not $hasP9Checklist) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9_factory_preset_qa_checklist_missing"
        file = Normalize-RelPath($p9ChecklistRel)
        function = "P9 documentation"
        detail = "P9 requires docs/p9-factory-preset-qa-checklist.md."
    }
}

$p9IssueTemplateRel = "docs/p9-preset-gain-staging-issue-template.md"
$p9IssueTemplatePath = Join-Path $repoRoot $p9IssueTemplateRel
$hasP9IssueTemplate = Test-Path $p9IssueTemplatePath
$contractChecks += [pscustomobject]@{
    checkId = "p9_preset_gain_staging_issue_template_present"
    target = $p9IssueTemplateRel
    passed = $hasP9IssueTemplate
    detail = if ($hasP9IssueTemplate) { "P9 preset/gain staging issue template found" } else { "P9 preset/gain staging issue template missing" }
}
if (-not $hasP9IssueTemplate) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9_preset_gain_staging_issue_template_missing"
        file = Normalize-RelPath($p9IssueTemplateRel)
        function = "P9 documentation"
        detail = "P9 requires docs/p9-preset-gain-staging-issue-template.md."
    }
}

$p9DraftCatalogRel = "docs/p9-draft-factory-preset-catalog.md"
$p9DraftCatalogPath = Join-Path $repoRoot $p9DraftCatalogRel
$hasP9DraftCatalog = Test-Path $p9DraftCatalogPath
$contractChecks += [pscustomobject]@{
    checkId = "p9_draft_factory_preset_catalog_present"
    target = $p9DraftCatalogRel
    passed = $hasP9DraftCatalog
    detail = if ($hasP9DraftCatalog) { "P9 draft factory preset catalog found" } else { "P9 draft factory preset catalog missing" }
}
if (-not $hasP9DraftCatalog) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9_draft_factory_preset_catalog_missing"
        file = Normalize-RelPath($p9DraftCatalogRel)
        function = "P9 documentation"
        detail = "P9 requires docs/p9-draft-factory-preset-catalog.md."
    }
}

$p9ResultsRel = "docs/p9-factory-presets-gain-staging-framework-results.md"
$p9ResultsPath = Join-Path $repoRoot $p9ResultsRel
$hasP9Results = Test-Path $p9ResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p9_factory_preset_framework_results_present"
    target = $p9ResultsRel
    passed = $hasP9Results
    detail = if ($hasP9Results) { "P9 framework results found" } else { "P9 framework results missing" }
}
if (-not $hasP9Results) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9_factory_preset_framework_results_missing"
        file = Normalize-RelPath($p9ResultsRel)
        function = "P9 documentation"
        detail = "P9 requires docs/p9-factory-presets-gain-staging-framework-results.md."
    }
}

if ($hasP9DraftCatalog) {
    $p9DraftCatalogText = Get-Content -LiteralPath $p9DraftCatalogPath -Raw
    $p9DraftHasApproved = $p9DraftCatalogText -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9_draft_catalog_no_factory_approved"
        target = $p9DraftCatalogRel
        passed = (-not $p9DraftHasApproved)
        detail = if ($p9DraftHasApproved) { "FACTORY_APPROVED marker found in draft catalog" } else { "draft catalog has no FACTORY_APPROVED marker" }
    }
    if ($p9DraftHasApproved) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_draft_catalog_factory_approved_present"
            file = Normalize-RelPath($p9DraftCatalogRel)
            function = "P9 draft catalog"
            detail = "P9 draft catalog must not mark presets as FACTORY_APPROVED."
        }
    }

    $p9DraftNames = @()
    foreach ($line in ($p9DraftCatalogText -split "`r?`n")) {
        $isDraftPresetRow = $line -match '^\|\s*([^|]+?)\s*\|\s*[^|]+\s*\|\s*[^|]+\s*\|'
        $isDraftHeaderRow = $line -match '^\|\s*Name\s*\|' -or $line -match '^\|\s*---'
        if ($isDraftPresetRow -and -not $isDraftHeaderRow) {
            $p9DraftNames += $Matches[1].Trim()
        }
    }

    $p9DuplicateNames = @($p9DraftNames | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    $contractChecks += [pscustomobject]@{
        checkId = "p9_draft_catalog_names_unique"
        target = $p9DraftCatalogRel
        passed = ($p9DraftNames.Count -gt 0 -and $p9DuplicateNames.Count -eq 0)
        detail = if ($p9DraftNames.Count -eq 0) { "no draft names parsed" } elseif ($p9DuplicateNames.Count -eq 0) { "names=$($p9DraftNames.Count); no duplicates" } else { "duplicates=" + ($p9DuplicateNames -join ",") }
    }
    if ($p9DraftNames.Count -eq 0 -or $p9DuplicateNames.Count -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_draft_catalog_names_not_unique"
            file = Normalize-RelPath($p9DraftCatalogRel)
            function = "P9 draft catalog"
            detail = "P9 draft preset names must parse and remain unique."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP9KnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p9_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $hasP9KnownFailureBypass)
        detail = if ($hasP9KnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP9KnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_known_failure_ignore_present"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "base validation"
            detail = "P9 must not add known-failure ignores to base validation."
        }
    }
}

$p9GoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9GoldenBaselineChanged.Count -eq 0)
    detail = if ($p9GoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9GoldenBaselineChanged -join ",") }
}
if ($p9GoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9 baseline guard"
        detail = "P9 must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9SchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9SchemaUnchanged
        detail = if ($p9SchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9SchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9 must not bump schema."
        }
    }
}

if ($hasP9Framework -and $hasP9Checklist -and $hasP9IssueTemplate -and $hasP9DraftCatalog -and $hasP9Results) {
    $p9DocText = (Get-Content -LiteralPath $p9FrameworkPath -Raw) + "`n" +
        (Get-Content -LiteralPath $p9ChecklistPath -Raw) + "`n" +
        (Get-Content -LiteralPath $p9IssueTemplatePath -Raw) + "`n" +
        (Get-Content -LiteralPath $p9DraftCatalogPath -Raw) + "`n" +
        (Get-Content -LiteralPath $p9ResultsPath -Raw)

    $p9ManualListeningPending = $p9DocText -match 'Manual listening QA general remains pending|General manual listening QA remains pending'
    $p9ManualListeningMarkedPass = $p9DocText -match 'Manual listening QA\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p9_manual_listening_general_not_marked_pass"
        target = "P9 docs"
        passed = ($p9ManualListeningPending -and -not $p9ManualListeningMarkedPass)
        detail = if ($p9ManualListeningPending -and -not $p9ManualListeningMarkedPass) { "manual listening QA remains pending" } else { "manual listening pending marker missing or marked PASS" }
    }
    if (-not ($p9ManualListeningPending -and -not $p9ManualListeningMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_manual_listening_general_marked_pass"
            file = Normalize-RelPath($p9ResultsRel)
            function = "P9 documentation"
            detail = "P9 must not mark manual listening QA general as PASS."
        }
    }

    $p9DistortionPending = $p9DocText -match 'Distortion manual listening QA remains pending|Distortion manual listening QA queda pendiente'
    $p9DistortionMarkedPass = $p9DocText -match 'Distortion manual listening QA\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p9_distortion_manual_listening_not_marked_pass"
        target = "P9 docs"
        passed = ($p9DistortionPending -and -not $p9DistortionMarkedPass)
        detail = if ($p9DistortionPending -and -not $p9DistortionMarkedPass) { "Distortion manual listening QA remains pending" } else { "Distortion manual listening QA pending marker missing or marked PASS" }
    }
    if (-not ($p9DistortionPending -and -not $p9DistortionMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_distortion_manual_listening_marked_pass"
            file = Normalize-RelPath($p9ResultsRel)
            function = "P9 documentation"
            detail = "P9 must not mark Distortion manual listening QA as PASS."
        }
    }

    $p9P7fPending = $p9DocText -match 'P7F/Reaper remains pending|P7F/Reaper.*pendiente|Reaper/P7F remains pending'
    $p9P7fMarkedPass = $p9DocText -match 'P7F/Reaper\s*(?:[:=-]|\s+).*PASS|Reaper/P7F\s*(?:[:=-]|\s+).*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p9_p7f_reaper_not_marked_pass"
        target = "P9 docs"
        passed = ($p9P7fPending -and -not $p9P7fMarkedPass)
        detail = if ($p9P7fPending -and -not $p9P7fMarkedPass) { "P7F/Reaper remains pending" } else { "P7F/Reaper pending marker missing or marked PASS" }
    }
    if (-not ($p9P7fPending -and -not $p9P7fMarkedPass)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9_p7f_reaper_marked_pass"
            file = Normalize-RelPath($p9ResultsRel)
            function = "P9 documentation"
            detail = "P9 must not mark P7F/Reaper as PASS."
        }
    }
}

# ---------------------------------------------------------------------------
# P9B - Draft factory preset serialization / validation
# ---------------------------------------------------------------------------

$p9bManifestDocRel = "docs/p9b-draft-factory-bank-manifest.md"
$p9bManifestDocPath = Join-Path $repoRoot $p9bManifestDocRel
$hasP9bManifestDoc = Test-Path $p9bManifestDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p9b_draft_manifest_doc_present"
    target = $p9bManifestDocRel
    passed = $hasP9bManifestDoc
    detail = if ($hasP9bManifestDoc) { "P9B draft manifest doc found" } else { "P9B draft manifest doc missing" }
}
if (-not $hasP9bManifestDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9b_draft_manifest_doc_missing"
        file = Normalize-RelPath($p9bManifestDocRel)
        function = "P9B documentation"
        detail = "P9B requires docs/p9b-draft-factory-bank-manifest.md."
    }
}

$p9bResultsRel = "docs/p9b-draft-factory-preset-serialization-results.md"
$p9bResultsPath = Join-Path $repoRoot $p9bResultsRel
$hasP9bResults = Test-Path $p9bResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p9b_results_doc_present"
    target = $p9bResultsRel
    passed = $hasP9bResults
    detail = if ($hasP9bResults) { "P9B serialization results doc found" } else { "P9B serialization results doc missing" }
}
if (-not $hasP9bResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9b_results_doc_missing"
        file = Normalize-RelPath($p9bResultsRel)
        function = "P9B documentation"
        detail = "P9B requires docs/p9b-draft-factory-preset-serialization-results.md."
    }
}

$p9bManifestJsonRel = "Resources/Presets/DraftFactory/factory-bank.draft.json"
$p9bManifestJsonPath = Join-Path $repoRoot $p9bManifestJsonRel
$hasP9bManifestJson = Test-Path $p9bManifestJsonPath
$contractChecks += [pscustomobject]@{
    checkId = "p9b_draft_manifest_json_present"
    target = $p9bManifestJsonRel
    passed = $hasP9bManifestJson
    detail = if ($hasP9bManifestJson) { "P9B draft manifest JSON found" } else { "P9B draft manifest JSON missing" }
}
if (-not $hasP9bManifestJson) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9b_draft_manifest_json_missing"
        file = Normalize-RelPath($p9bManifestJsonRel)
        function = "P9B manifest"
        detail = "P9B requires Resources/Presets/DraftFactory/factory-bank.draft.json."
    }
}

$p9bManifest = $null
$p9bManifestRaw = ""
if ($hasP9bManifestJson) {
    try {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
        $p9bManifest = $p9bManifestRaw | ConvertFrom-Json
        $contractChecks += [pscustomobject]@{
            checkId = "p9b_draft_manifest_json_parses"
            target = $p9bManifestJsonRel
            passed = $true
            detail = "manifest JSON parsed"
        }
    }
    catch {
        $contractChecks += [pscustomobject]@{
            checkId = "p9b_draft_manifest_json_parses"
            target = $p9bManifestJsonRel
            passed = $false
            detail = "manifest JSON parse failed"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_draft_manifest_json_invalid"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B manifest JSON must parse."
        }
    }
}

if ($null -ne $p9bManifest) {
    $p9bAllowedReadiness = @("DRAFT_TECHNICAL", "LISTENING_CANDIDATE")
    $p9bPresetRecords = @($p9bManifest.presets)
    $p9bPresetNames = @($p9bPresetRecords | ForEach-Object { [string]$_.name })
    $p9bDuplicateNames = @($p9bPresetNames | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_draft_preset_names_unique"
        target = $p9bManifestJsonRel
        passed = ($p9bPresetNames.Count -gt 0 -and $p9bDuplicateNames.Count -eq 0)
        detail = if ($p9bPresetNames.Count -eq 0) { "no preset records" } elseif ($p9bDuplicateNames.Count -eq 0) { "names=$($p9bPresetNames.Count); no duplicates" } else { "duplicates=" + ($p9bDuplicateNames -join ",") }
    }
    if ($p9bPresetNames.Count -eq 0 -or $p9bDuplicateNames.Count -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_draft_preset_names_not_unique"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B manifest preset names must parse and remain unique."
        }
    }

    $p9bInvalidReadiness = @($p9bPresetRecords | Where-Object { $p9bAllowedReadiness -notcontains [string]$_.readiness })
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_readiness_limited_to_draft_or_candidate"
        target = $p9bManifestJsonRel
        passed = ($p9bInvalidReadiness.Count -eq 0 -and $p9bPresetRecords.Count -gt 0)
        detail = if ($p9bInvalidReadiness.Count -eq 0) { "all preset readiness values are allowed" } else { "invalid=" + (($p9bInvalidReadiness | ForEach-Object { "$($_.name):$($_.readiness)" }) -join ",") }
    }
    if ($p9bInvalidReadiness.Count -gt 0 -or $p9bPresetRecords.Count -eq 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_invalid_readiness"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B readiness must be DRAFT_TECHNICAL or LISTENING_CANDIDATE only."
        }
    }

    $p9bHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p9bHasApprovedMarker)
        detail = if ($p9bHasApprovedMarker) { "forbidden readiness marker found" } else { "no forbidden readiness marker in manifest" }
    }
    if ($p9bHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B manifest must not mark any preset as shipping approved."
        }
    }

    $p9bManualPending = @($p9bPresetRecords | Where-Object { [string]$_.manualListeningStatus -ne "pending" }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_manual_listening_pending"
        target = $p9bManifestJsonRel
        passed = $p9bManualPending
        detail = if ($p9bManualPending) { "manual listening remains pending for all drafts" } else { "one or more drafts changed manual listening status" }
    }
    if (-not $p9bManualPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_manual_listening_not_pending"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B must keep manual listening QA pending."
        }
    }

    $p9bDistortionStatusesOk = @($p9bPresetRecords | Where-Object {
        $status = [string]$_.distortionListeningStatus
        $status -ne "pending" -and $status -ne "not_applicable"
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_distortion_listening_pending_or_na"
        target = $p9bManifestJsonRel
        passed = $p9bDistortionStatusesOk
        detail = if ($p9bDistortionStatusesOk) { "Distortion listening remains pending or not_applicable" } else { "invalid Distortion listening status found" }
    }
    if (-not $p9bDistortionStatusesOk) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_distortion_listening_not_pending"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B must keep Distortion manual listening pending when applicable."
        }
    }

    $p9bReaperPending = @($p9bPresetRecords | Where-Object { [string]$_.reaperSmokeStatus -ne "pending" }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_reaper_smoke_pending"
        target = $p9bManifestJsonRel
        passed = $p9bReaperPending
        detail = if ($p9bReaperPending) { "Reaper smoke remains pending for all drafts" } else { "one or more drafts changed Reaper smoke status" }
    }
    if (-not $p9bReaperPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_reaper_smoke_not_pending"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B must keep P7F/Reaper pending."
        }
    }

    $p9bKnownTypeIds = @(
        "Boost", "Cabinet", "Chorus", "Classic Amp", "Clean Amp", "Compressor",
        "Delay", "EQ", "High Gain Amp", "Modern 4x12", "Noise Gate", "Overdrive", "Reverb"
    )
    $p9bReferencedTypeIds = @()
    foreach ($preset in $p9bPresetRecords) {
        foreach ($step in @($preset.chainTemplate)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$step.typeID)) {
                $p9bReferencedTypeIds += [string]$step.typeID
            }
        }
    }
    $p9bUnknownTypeIds = @($p9bReferencedTypeIds | Sort-Object -Unique | Where-Object { $p9bKnownTypeIds -notcontains $_ })
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_manifest_uses_registered_pedal_ids"
        target = $p9bManifestJsonRel
        passed = ($p9bUnknownTypeIds.Count -eq 0)
        detail = if ($p9bUnknownTypeIds.Count -eq 0) { "all manifest type IDs are registered by current catalog/registry review" } else { "unknown=" + ($p9bUnknownTypeIds -join ",") }
    }
    if ($p9bUnknownTypeIds.Count -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_manifest_unknown_pedal_ids"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "P9B manifest references unregistered type IDs."
        }
    }

    $p9bGeneratedPaths = @($p9bPresetRecords | ForEach-Object { [string]$_.filePath } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $p9bGeneratedPathsOutsideDraft = @($p9bGeneratedPaths | Where-Object { ($_ -replace '\\', '/') -notmatch '^Resources/Presets/DraftFactory/' })
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_manifest_generated_paths_stay_in_draft_folder"
        target = $p9bManifestJsonRel
        passed = ($p9bGeneratedPathsOutsideDraft.Count -eq 0)
        detail = if ($p9bGeneratedPaths.Count -eq 0) { "no generated preset paths in manifest" } elseif ($p9bGeneratedPathsOutsideDraft.Count -eq 0) { "all generated paths stay in draft folder" } else { "outside=" + ($p9bGeneratedPathsOutsideDraft -join ",") }
    }
    if ($p9bGeneratedPathsOutsideDraft.Count -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_manifest_generated_path_outside_draft_folder"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9B manifest"
            detail = "Generated draft preset paths must stay under Resources/Presets/DraftFactory."
        }
    }
}

if ($hasP9bManifestDoc -and $hasP9bResults) {
    $p9bDocText = (Get-Content -LiteralPath $p9bManifestDocPath -Raw) + "`n" +
        (Get-Content -LiteralPath $p9bResultsPath -Raw)
    $p9bDocsHaveApprovedMarker = $p9bDocText -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_docs_no_factory_approved"
        target = "P9B docs"
        passed = (-not $p9bDocsHaveApprovedMarker)
        detail = if ($p9bDocsHaveApprovedMarker) { "forbidden readiness marker found in P9B docs" } else { "P9B docs avoid forbidden readiness marker" }
    }
    if ($p9bDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_docs_factory_approved_present"
            file = Normalize-RelPath($p9bResultsRel)
            function = "P9B documentation"
            detail = "P9B docs must not mark presets as shipping approved."
        }
    }

    $p9bDocsKeepPending = ($p9bDocText -match 'Manual listening QA general remains pending') -and
        ($p9bDocText -match 'Distortion manual listening QA remains pending') -and
        ($p9bDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_docs_keep_pending_statuses"
        target = "P9B docs"
        passed = $p9bDocsKeepPending
        detail = if ($p9bDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending" } else { "one or more pending markers missing" }
    }
    if (-not $p9bDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_docs_pending_marker_missing"
            file = Normalize-RelPath($p9bResultsRel)
            function = "P9B documentation"
            detail = "P9B docs must keep manual listening, Distortion listening, and P7F/Reaper pending."
        }
    }
}

$p9bPresetFiles = @()
$p9bDraftFolder = Join-Path $repoRoot "Resources/Presets/DraftFactory"
if (Test-Path $p9bDraftFolder) {
    $p9bPresetFiles = @(Get-ChildItem -LiteralPath $p9bDraftFolder -Filter "*.nova-preset" -File -Recurse | ForEach-Object {
        Normalize-RelPath($_.FullName.Substring($repoRoot.Length + 1))
    })
}
$p9bRepoPresetFiles = @(Get-ChildItem -LiteralPath $repoRoot -Filter "*.nova-preset" -File -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    Normalize-RelPath($_.FullName.Substring($repoRoot.Length + 1))
})
$p9bPresetFilesOutsideDraft = @($p9bRepoPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/' })
$contractChecks += [pscustomobject]@{
    checkId = "p9b_generated_presets_only_in_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p9bPresetFilesOutsideDraft.Count -eq 0)
    detail = if ($p9bRepoPresetFiles.Count -eq 0) { "no .nova-preset files generated" } elseif ($p9bPresetFilesOutsideDraft.Count -eq 0) { "all .nova-preset files are under draft folder" } else { "outside=" + ($p9bPresetFilesOutsideDraft -join ",") }
}
if ($p9bPresetFilesOutsideDraft.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9b_generated_preset_outside_draft_folder"
        file = "repo"
        function = "P9B preset generation guard"
        detail = "P9B generated preset files must not be outside Resources/Presets/DraftFactory."
    }
}

$p9bStartupPointerChanged = @($gitChangedFiles | Where-Object { $_ -match 'startup-preset\.txt$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p9b_no_startup_preset_pointer_update"
    target = "git diff"
    passed = (-not $p9bStartupPointerChanged)
    detail = if ($p9bStartupPointerChanged) { "startup-preset.txt changed" } else { "startup-preset.txt not changed" }
}
if ($p9bStartupPointerChanged) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9b_startup_preset_pointer_updated"
        file = "startup-preset.txt"
        function = "P9B startup preset guard"
        detail = "P9B must not update startup-preset.txt."
    }
}

$p9bGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9b_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9bGoldenBaselineChanged.Count -eq 0)
    detail = if ($p9bGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9bGoldenBaselineChanged -join ",") }
}
if ($p9bGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9b_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9B baseline guard"
        detail = "P9B must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9bSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9b_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9bSchemaUnchanged
        detail = if ($p9bSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9bSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9b_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9B must not bump schema."
        }
    }
}

# ---------------------------------------------------------------------------
# P9C - Draft preset generator / round-trip gate
# ---------------------------------------------------------------------------

$p9cResultsRel = "docs/p9c-draft-preset-generator-roundtrip-results.md"
$p9cResultsPath = Join-Path $repoRoot $p9cResultsRel
$hasP9cResults = Test-Path $p9cResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p9c_results_doc_present"
    target = $p9cResultsRel
    passed = $hasP9cResults
    detail = if ($hasP9cResults) { "P9C results doc found" } else { "P9C results doc missing" }
}
if (-not $hasP9cResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9c_results_doc_missing"
        file = Normalize-RelPath($p9cResultsRel)
        function = "P9C documentation"
        detail = "P9C requires docs/p9c-draft-preset-generator-roundtrip-results.md."
    }
}

$p9cGeneratorRel = "scripts/generate-draft-factory-presets.ps1"
$p9cGeneratorPath = Join-Path $repoRoot $p9cGeneratorRel
$hasP9cGenerator = Test-Path $p9cGeneratorPath
$contractChecks += [pscustomobject]@{
    checkId = "p9c_generator_script_present"
    target = $p9cGeneratorRel
    passed = $hasP9cGenerator
    detail = if ($hasP9cGenerator) { "P9C generator/auditor script found" } else { "P9C generator/auditor script missing" }
}
if (-not $hasP9cGenerator) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9c_generator_script_missing"
        file = Normalize-RelPath($p9cGeneratorRel)
        function = "P9C tooling"
        detail = "P9C requires scripts/generate-draft-factory-presets.ps1 or a documented blocked decision."
    }
}

$p9cReportRel = "artifacts/p9c-draft-preset-generator-report.json"
$p9cReportPath = Join-Path $repoRoot $p9cReportRel
$hasP9cReport = Test-Path $p9cReportPath
$contractChecks += [pscustomobject]@{
    checkId = "p9c_generator_report_present"
    target = $p9cReportRel
    passed = $hasP9cReport
    detail = if ($hasP9cReport) { "P9C generator report found" } else { "P9C generator report missing" }
}
if (-not $hasP9cReport) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9c_generator_report_missing"
        file = Normalize-RelPath($p9cReportRel)
        function = "P9C tooling"
        detail = "P9C generator/auditor must write artifacts/p9c-draft-preset-generator-report.json."
    }
}

$p9cReport = $null
if ($hasP9cReport) {
    try {
        $p9cReport = Get-Content -LiteralPath $p9cReportPath -Raw | ConvertFrom-Json
        $contractChecks += [pscustomobject]@{
            checkId = "p9c_generator_report_json_parses"
            target = $p9cReportRel
            passed = $true
            detail = "P9C generator report parsed"
        }
    }
    catch {
        $contractChecks += [pscustomobject]@{
            checkId = "p9c_generator_report_json_parses"
            target = $p9cReportRel
            passed = $false
            detail = "P9C generator report parse failed"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_generator_report_json_invalid"
            file = Normalize-RelPath($p9cReportRel)
            function = "P9C tooling"
            detail = "P9C generator report must parse as JSON."
        }
    }
}

if ($null -ne $p9cReport) {
    $p9cStatusOk = ([string]$p9cReport.status -eq "BLOCKED_SAFE_NO_GENERATION" -or [string]$p9cReport.status -eq "GENERATED_DRAFT" -or [string]$p9cReport.status -eq "PASS")
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_generator_status_allowed"
        target = $p9cReportRel
        passed = $p9cStatusOk
        detail = "status=$($p9cReport.status)"
    }
    if (-not $p9cStatusOk) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_generator_status_invalid"
            file = Normalize-RelPath($p9cReportRel)
            function = "P9C tooling"
            detail = "P9C/P9D generator status must be BLOCKED_SAFE_NO_GENERATION, GENERATED_DRAFT, or PASS."
        }
    }

    $p9cNoUserWrites = -not [bool]$p9cReport.wroteUserPresetDirectory
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_no_user_preset_dir_writes"
        target = $p9cReportRel
        passed = $p9cNoUserWrites
        detail = if ($p9cNoUserWrites) { "report says no user preset directory writes" } else { "report says user preset directory was written" }
    }
    if (-not $p9cNoUserWrites) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_user_preset_dir_write_detected"
            file = Normalize-RelPath($p9cReportRel)
            function = "P9C tooling"
            detail = "P9C must not write to the user preset directory."
        }
    }

    $p9cNoStartupWrite = -not [bool]$p9cReport.wroteStartupPresetPointer
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_no_startup_preset_pointer_write"
        target = $p9cReportRel
        passed = $p9cNoStartupWrite
        detail = if ($p9cNoStartupWrite) { "report says startup preset pointer was not written" } else { "report says startup preset pointer was written" }
    }
    if (-not $p9cNoStartupWrite) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_startup_preset_pointer_write_detected"
            file = Normalize-RelPath($p9cReportRel)
            function = "P9C tooling"
            detail = "P9C must not write startup-preset.txt."
        }
    }

    $p9cNoGeneratedFilesInBlockedMode = ([string]$p9cReport.status -ne "BLOCKED_SAFE_NO_GENERATION") -or ([int]$p9cReport.generatedPresetCount -eq 0)
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_blocked_mode_generates_no_presets"
        target = $p9cReportRel
        passed = $p9cNoGeneratedFilesInBlockedMode
        detail = "status=$($p9cReport.status); generatedPresetCount=$($p9cReport.generatedPresetCount)"
    }
    if (-not $p9cNoGeneratedFilesInBlockedMode) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_blocked_mode_generated_presets"
            file = Normalize-RelPath($p9cReportRel)
            function = "P9C tooling"
            detail = "Blocked P9C generator mode must not generate preset files."
        }
    }
}

if ($hasP9cGenerator) {
    $p9cGeneratorText = Get-Content -LiteralPath $p9cGeneratorPath -Raw
    $p9cGeneratorTargetsAppData = $p9cGeneratorText -match '%APPDATA%|userApplicationDataDirectory|getFolderPath'
    $p9cGeneratorWritesAppData = $p9cGeneratorText -match '%APPDATA%.*(Set-Content|Out-File|New-Item|Remove-Item)|userApplicationDataDirectory[\s\S]{0,240}(replaceWithText|replaceWithData|createOutputStream)'
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_generator_does_not_target_appdata"
        target = $p9cGeneratorRel
        passed = (-not $p9cGeneratorWritesAppData)
        detail = if ($p9cGeneratorTargetsAppData -and -not $p9cGeneratorWritesAppData) { "AppData marker is side-effect snapshot only" } elseif ($p9cGeneratorWritesAppData) { "AppData write marker found" } else { "no AppData target marker" }
    }
    if ($p9cGeneratorWritesAppData) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_generator_targets_appdata"
            file = Normalize-RelPath($p9cGeneratorRel)
            function = "P9C tooling"
            detail = "P9C generator must not target the user preset directory."
        }
    }

    $p9cGeneratorWritesStartup = $p9cGeneratorText -match 'startup-preset\.txt.{0,160}(Set-Content|replaceWithText|Out-File)|writeStartupPresetPointer\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_generator_no_startup_preset_update_logic"
        target = $p9cGeneratorRel
        passed = (-not $p9cGeneratorWritesStartup)
        detail = if ($p9cGeneratorWritesStartup) { "startup preset write logic found" } else { "no startup preset write logic" }
    }
    if ($p9cGeneratorWritesStartup) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_generator_updates_startup_preset"
            file = Normalize-RelPath($p9cGeneratorRel)
            function = "P9C tooling"
            detail = "P9C generator must not update startup-preset.txt."
        }
    }
}

if ($hasP9bManifestJson) {
    if ([string]::IsNullOrWhiteSpace($p9bManifestRaw)) {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
    }
    $p9cManifestHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p9cManifestHasApprovedMarker)
        detail = if ($p9cManifestHasApprovedMarker) { "forbidden readiness marker found" } else { "no forbidden readiness marker in manifest" }
    }
    if ($p9cManifestHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9C manifest"
            detail = "P9C manifest must not mark presets as shipping approved."
        }
    }

    if ($null -eq $p9bManifest) {
        try { $p9bManifest = $p9bManifestRaw | ConvertFrom-Json } catch { $p9bManifest = $null }
    }
    if ($null -ne $p9bManifest) {
        $p9cManifestPaths = @($p9bManifest.presets | ForEach-Object { [string]$_.filePath } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $p9cManifestPathsOutsideDraft = @($p9cManifestPaths | Where-Object { ($_ -replace '\\', '/') -notmatch '^Resources/Presets/DraftFactory/generated/' })
        $contractChecks += [pscustomobject]@{
            checkId = "p9c_manifest_references_only_generated_draft_folder"
            target = $p9bManifestJsonRel
            passed = ($p9cManifestPathsOutsideDraft.Count -eq 0)
            detail = if ($p9cManifestPaths.Count -eq 0) { "manifest has no generated file paths" } elseif ($p9cManifestPathsOutsideDraft.Count -eq 0) { "manifest paths are under generated draft folder" } else { "outside=" + ($p9cManifestPathsOutsideDraft -join ",") }
        }
        if ($p9cManifestPathsOutsideDraft.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9c_manifest_path_outside_generated_draft_folder"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9C manifest"
                detail = "P9C manifest filePath values must stay under Resources/Presets/DraftFactory/generated."
            }
        }
    }
}

$p9cGeneratedFolderRel = "Resources/Presets/DraftFactory/generated"
$p9cGeneratedFolderPath = Join-Path $repoRoot $p9cGeneratedFolderRel
$p9cGeneratedPresetFiles = @()
if (Test-Path $p9cGeneratedFolderPath) {
    $p9cGeneratedPresetFiles = @(Get-ChildItem -LiteralPath $p9cGeneratedFolderPath -Filter "*.nova-preset" -File -Recurse | ForEach-Object {
        Normalize-RelPath($_.FullName.Substring($repoRoot.Length + 1))
    })
}
$p9cAllPresetFiles = @(Get-ChildItem -LiteralPath $repoRoot -Filter "*.nova-preset" -File -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    Normalize-RelPath($_.FullName.Substring($repoRoot.Length + 1))
})
$p9cPresetFilesOutsideGenerated = @($p9cAllPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
$contractChecks += [pscustomobject]@{
    checkId = "p9c_generated_presets_only_in_generated_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p9cPresetFilesOutsideGenerated.Count -eq 0)
    detail = if ($p9cAllPresetFiles.Count -eq 0) { "no .nova-preset files generated" } elseif ($p9cPresetFilesOutsideGenerated.Count -eq 0) { "all .nova-preset files are under generated draft folder" } else { "outside=" + ($p9cPresetFilesOutsideGenerated -join ",") }
}
if ($p9cPresetFilesOutsideGenerated.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9c_generated_preset_outside_generated_draft_folder"
        file = "repo"
        function = "P9C preset generation guard"
        detail = "P9C generated preset files must not be outside Resources/Presets/DraftFactory/generated."
    }
}

if ($hasP9cResults) {
    $p9cDocText = Get-Content -LiteralPath $p9cResultsPath -Raw
    $p9cDocsHaveApprovedMarker = $p9cDocText -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_docs_no_factory_approved"
        target = $p9cResultsRel
        passed = (-not $p9cDocsHaveApprovedMarker)
        detail = if ($p9cDocsHaveApprovedMarker) { "forbidden readiness marker found in P9C docs" } else { "P9C docs avoid forbidden readiness marker" }
    }
    if ($p9cDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_docs_factory_approved_present"
            file = Normalize-RelPath($p9cResultsRel)
            function = "P9C documentation"
            detail = "P9C docs must not mark presets as shipping approved."
        }
    }

    $p9cDocsKeepPending = ($p9cDocText -match 'Manual listening QA general remains pending') -and
        ($p9cDocText -match 'Distortion manual listening QA remains pending') -and
        ($p9cDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_docs_keep_pending_statuses"
        target = $p9cResultsRel
        passed = $p9cDocsKeepPending
        detail = if ($p9cDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending" } else { "one or more pending markers missing" }
    }
    if (-not $p9cDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_docs_pending_marker_missing"
            file = Normalize-RelPath($p9cResultsRel)
            function = "P9C documentation"
            detail = "P9C docs must keep manual listening, Distortion listening, and P7F/Reaper pending."
        }
    }
}

$p9cGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9c_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9cGoldenBaselineChanged.Count -eq 0)
    detail = if ($p9cGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9cGoldenBaselineChanged -join ",") }
}
if ($p9cGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9c_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9C baseline guard"
        detail = "P9C must not update golden baseline files."
    }
}

$p9cStartupPointerChanged = @($gitChangedFiles | Where-Object { $_ -match 'startup-preset\.txt$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p9c_no_startup_preset_pointer_update"
    target = "git diff"
    passed = (-not $p9cStartupPointerChanged)
    detail = if ($p9cStartupPointerChanged) { "startup-preset.txt changed" } else { "startup-preset.txt not changed" }
}
if ($p9cStartupPointerChanged) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9c_startup_preset_pointer_updated"
        file = "startup-preset.txt"
        function = "P9C startup preset guard"
        detail = "P9C must not update startup-preset.txt."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9cSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9c_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9cSchemaUnchanged
        detail = if ($p9cSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9cSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9c_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9C must not bump schema."
        }
    }
}

# ---------------------------------------------------------------------------
# P9D - Side-effect-free draft preset builder
# ---------------------------------------------------------------------------

$p9dResultsRel = "docs/p9d-side-effect-free-draft-preset-builder-results.md"
$p9dResultsPath = Join-Path $repoRoot $p9dResultsRel
$hasP9dResults = Test-Path $p9dResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p9d_results_doc_present"
    target = $p9dResultsRel
    passed = $hasP9dResults
    detail = if ($hasP9dResults) { "P9D results doc found" } else { "P9D results doc missing" }
}
if (-not $hasP9dResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_results_doc_missing"
        file = Normalize-RelPath($p9dResultsRel)
        function = "P9D documentation"
        detail = "P9D requires docs/p9d-side-effect-free-draft-preset-builder-results.md."
    }
}

$p9dReportRel = "artifacts/p9d-draft-preset-builder-report.json"
$p9dReportPath = Join-Path $repoRoot $p9dReportRel
$hasP9dReport = Test-Path $p9dReportPath
$contractChecks += [pscustomobject]@{
    checkId = "p9d_builder_report_present"
    target = $p9dReportRel
    passed = $hasP9dReport
    detail = if ($hasP9dReport) { "P9D builder report found" } else { "P9D builder report missing" }
}
if (-not $hasP9dReport) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_builder_report_missing"
        file = Normalize-RelPath($p9dReportRel)
        function = "P9D tooling"
        detail = "P9D builder must write artifacts/p9d-draft-preset-builder-report.json."
    }
}

$p9dReport = $null
if ($hasP9dReport) {
    try {
        $p9dReport = Get-Content -LiteralPath $p9dReportPath -Raw | ConvertFrom-Json
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_builder_report_json_parses"
            target = $p9dReportRel
            passed = $true
            detail = "P9D builder report parsed"
        }
    }
    catch {
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_builder_report_json_parses"
            target = $p9dReportRel
            passed = $false
            detail = "P9D builder report parse failed"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_builder_report_json_invalid"
            file = Normalize-RelPath($p9dReportRel)
            function = "P9D tooling"
            detail = "P9D builder report must parse as JSON."
        }
    }
}

if ($null -ne $p9dReport) {
    $p9dStatusOk = [string]$p9dReport.status -eq "PASS"
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_builder_status_pass"
        target = $p9dReportRel
        passed = $p9dStatusOk
        detail = "status=$($p9dReport.status)"
    }
    if (-not $p9dStatusOk) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_builder_status_not_pass"
            file = Normalize-RelPath($p9dReportRel)
            function = "P9D tooling"
            detail = "P9D builder report must have status PASS."
        }
    }

    foreach ($preset in @($p9dReport.presets)) {
        $presetName = [string]$preset.name
        $generatedOk = [string]$preset.generationStatus -eq "GENERATED_DRAFT"
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_preset_generated_draft"
            target = $presetName
            passed = $generatedOk
            detail = "generationStatus=$($preset.generationStatus)"
        }
        if (-not $generatedOk) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9d_preset_not_generated"
                file = Normalize-RelPath($p9dReportRel)
                function = "P9D preset generation"
                detail = "$presetName must be GENERATED_DRAFT or P9D must document a block."
            }
        }

        $roundTripOk = [string]$preset.roundTripStatus -eq "ROUND_TRIP_PASS"
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_preset_round_trip_pass"
            target = $presetName
            passed = $roundTripOk
            detail = "roundTripStatus=$($preset.roundTripStatus)"
        }
        if (-not $roundTripOk) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9d_preset_round_trip_not_pass"
                file = Normalize-RelPath($p9dReportRel)
                function = "P9D round-trip"
                detail = "$presetName must pass canonical round-trip validation."
            }
        }

        $processOk = [string]$preset.processStatus -eq "PROCESS_FINITE_PASS"
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_preset_process_finite_pass"
            target = $presetName
            passed = $processOk
            detail = "processStatus=$($preset.processStatus)"
        }
        if (-not $processOk) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9d_preset_process_not_pass"
                file = Normalize-RelPath($p9dReportRel)
                function = "P9D process finite"
                detail = "$presetName must pass process finite validation."
            }
        }
    }

    $p9dNoUserWrites = -not [bool]$p9dReport.wroteUserPresetDirectory
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_no_user_preset_dir_writes"
        target = $p9dReportRel
        passed = $p9dNoUserWrites
        detail = if ($p9dNoUserWrites) { "report says no user preset directory writes" } else { "report says user preset directory changed" }
    }
    if (-not $p9dNoUserWrites) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_user_preset_dir_write_detected"
            file = Normalize-RelPath($p9dReportRel)
            function = "P9D side-effect guard"
            detail = "P9D must not write to the user preset directory."
        }
    }

    $p9dNoStartupWrite = -not [bool]$p9dReport.wroteStartupPresetPointer
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_no_startup_preset_pointer_write"
        target = $p9dReportRel
        passed = $p9dNoStartupWrite
        detail = if ($p9dNoStartupWrite) { "report says startup preset pointer was not written" } else { "report says startup preset pointer changed" }
    }
    if (-not $p9dNoStartupWrite) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_startup_preset_pointer_write_detected"
            file = Normalize-RelPath($p9dReportRel)
            function = "P9D side-effect guard"
            detail = "P9D must not write startup-preset.txt."
        }
    }
}

$p9dGeneratedFolderRel = "Resources/Presets/DraftFactory/generated"
$p9dGeneratedFolderPath = Join-Path $repoRoot $p9dGeneratedFolderRel
$p9dGeneratedFolderExists = Test-Path $p9dGeneratedFolderPath
$contractChecks += [pscustomobject]@{
    checkId = "p9d_generated_folder_present"
    target = $p9dGeneratedFolderRel
    passed = $p9dGeneratedFolderExists
    detail = if ($p9dGeneratedFolderExists) { "generated folder found" } else { "generated folder missing" }
}
if (-not $p9dGeneratedFolderExists) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_generated_folder_missing"
        file = Normalize-RelPath($p9dGeneratedFolderRel)
        function = "P9D generated output"
        detail = "P9D generated output folder must exist after successful generation."
    }
}

$p9dGeneratedPresetFiles = if ($p9dGeneratedFolderExists) {
    @(Get-ChildItem -LiteralPath $p9dGeneratedFolderPath -Filter "*.nova-preset" -File -Recurse | ForEach-Object {
        Normalize-RelPath($_.FullName.Substring($repoRoot.Length + 1))
    })
} else { @() }

$p9dExpectedGeneratedCount = 6
$contractChecks += [pscustomobject]@{
    checkId = "p9d_generated_preset_count"
    target = $p9dGeneratedFolderRel
    passed = ($p9dGeneratedPresetFiles.Count -eq $p9dExpectedGeneratedCount)
    detail = "count=$($p9dGeneratedPresetFiles.Count)"
}
if ($p9dGeneratedPresetFiles.Count -ne $p9dExpectedGeneratedCount) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_generated_preset_count_invalid"
        file = Normalize-RelPath($p9dGeneratedFolderRel)
        function = "P9D generated output"
        detail = "P9D must generate exactly six draft preset files."
    }
}

$p9dPresetFilesOutsideGenerated = @($p9cAllPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
$contractChecks += [pscustomobject]@{
    checkId = "p9d_generated_presets_only_in_generated_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p9dPresetFilesOutsideGenerated.Count -eq 0)
    detail = if ($p9dPresetFilesOutsideGenerated.Count -eq 0) { "all .nova-preset files are under generated draft folder" } else { "outside=" + ($p9dPresetFilesOutsideGenerated -join ",") }
}
if ($p9dPresetFilesOutsideGenerated.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_generated_preset_outside_generated_draft_folder"
        file = "repo"
        function = "P9D generated output"
        detail = "P9D generated preset files must be under Resources/Presets/DraftFactory/generated only."
    }
}

if ($hasP9bManifestJson) {
    if ([string]::IsNullOrWhiteSpace($p9bManifestRaw)) {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
    }
    if ($null -eq $p9bManifest) {
        try { $p9bManifest = $p9bManifestRaw | ConvertFrom-Json } catch { $p9bManifest = $null }
    }

    $p9dManifestHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p9dManifestHasApprovedMarker)
        detail = if ($p9dManifestHasApprovedMarker) { "forbidden readiness marker found" } else { "no forbidden readiness marker in manifest" }
    }
    if ($p9dManifestHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9D manifest"
            detail = "P9D manifest must not mark presets as shipping approved."
        }
    }

    if ($null -ne $p9bManifest) {
        $p9dManifestPaths = @($p9bManifest.presets | ForEach-Object { [string]$_.filePath } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $p9dManifestPathsOutsideDraft = @($p9dManifestPaths | Where-Object { ($_ -replace '\\', '/') -notmatch '^Resources/Presets/DraftFactory/generated/[^/]+\.nova-preset$' })
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_manifest_references_only_generated_draft_folder"
            target = $p9bManifestJsonRel
            passed = ($p9dManifestPaths.Count -eq 6 -and $p9dManifestPathsOutsideDraft.Count -eq 0)
            detail = "paths=$($p9dManifestPaths.Count); outside=$($p9dManifestPathsOutsideDraft.Count)"
        }
        if ($p9dManifestPaths.Count -ne 6 -or $p9dManifestPathsOutsideDraft.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9d_manifest_path_invalid"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9D manifest"
                detail = "P9D manifest must reference exactly six generated draft-folder .nova-preset paths."
            }
        }

        $p9dGeneratedManifestFiles = @($p9bManifest.generatedPresetFiles | ForEach-Object { [string]$_ })
        $p9dGeneratedManifestFilesOutsideDraft = @($p9dGeneratedManifestFiles | Where-Object { ($_ -replace '\\', '/') -notmatch '^Resources/Presets/DraftFactory/generated/[^/]+\.nova-preset$' })
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_manifest_generated_files_list_valid"
            target = $p9bManifestJsonRel
            passed = ($p9dGeneratedManifestFiles.Count -eq 6 -and $p9dGeneratedManifestFilesOutsideDraft.Count -eq 0)
            detail = "generatedPresetFiles=$($p9dGeneratedManifestFiles.Count); outside=$($p9dGeneratedManifestFilesOutsideDraft.Count)"
        }
        if ($p9dGeneratedManifestFiles.Count -ne 6 -or $p9dGeneratedManifestFilesOutsideDraft.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9d_manifest_generated_files_invalid"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9D manifest"
                detail = "P9D manifest generatedPresetFiles must list exactly six generated draft-folder files."
            }
        }

        $p9dPendingStatuses = @($p9bManifest.presets | Where-Object {
            [string]$_.manualListeningStatus -ne "pending" -or
            (([string]$_.distortionListeningStatus -ne "pending") -and ([string]$_.distortionListeningStatus -ne "not_applicable")) -or
            [string]$_.reaperSmokeStatus -ne "pending"
        })
        $contractChecks += [pscustomobject]@{
            checkId = "p9d_manifest_keeps_pending_statuses"
            target = $p9bManifestJsonRel
            passed = ($p9dPendingStatuses.Count -eq 0)
            detail = if ($p9dPendingStatuses.Count -eq 0) { "manual, Distortion, and Reaper statuses remain pending/not_applicable" } else { "invalid statuses=$($p9dPendingStatuses.Count)" }
        }
        if ($p9dPendingStatuses.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9d_manifest_pending_status_changed"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9D manifest"
                detail = "P9D must keep manual listening, Distortion listening, and Reaper smoke pending/not_applicable."
            }
        }
    }
}

if ($hasP9cGenerator) {
    if ($null -eq $p9cGeneratorText) {
        $p9cGeneratorText = Get-Content -LiteralPath $p9cGeneratorPath -Raw
    }

    $p9dGeneratorCallsSessionPersistenceSaveLoad = $p9cGeneratorText -match 'SessionPersistence::(?:savePresetToFile|loadPresetFromFile)'
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_generator_no_session_persistence_save_load"
        target = $p9cGeneratorRel
        passed = (-not $p9dGeneratorCallsSessionPersistenceSaveLoad)
        detail = if ($p9dGeneratorCallsSessionPersistenceSaveLoad) { "SessionPersistence save/load call found" } else { "no SessionPersistence save/load call" }
    }
    if ($p9dGeneratorCallsSessionPersistenceSaveLoad) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_generator_calls_session_persistence_save_load"
            file = Normalize-RelPath($p9cGeneratorRel)
            function = "P9D generator"
            detail = "P9D generator must not call SessionPersistence::savePresetToFile/loadPresetFromFile."
        }
    }

    $p9dGeneratorWritesStartup = $p9cGeneratorText -match 'startup-preset\.txt.{0,160}(Set-Content|replaceWithText|Out-File)|writeStartupPresetPointer\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_generator_no_startup_preset_update_logic"
        target = $p9cGeneratorRel
        passed = (-not $p9dGeneratorWritesStartup)
        detail = if ($p9dGeneratorWritesStartup) { "startup preset write logic found" } else { "no startup preset write logic" }
    }
    if ($p9dGeneratorWritesStartup) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_generator_updates_startup_preset"
            file = Normalize-RelPath($p9cGeneratorRel)
            function = "P9D generator"
            detail = "P9D generator must not update startup-preset.txt."
        }
    }
}

$p9dGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9d_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9dGoldenBaselineChanged.Count -eq 0)
    detail = if ($p9dGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9dGoldenBaselineChanged -join ",") }
}
if ($p9dGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9D baseline guard"
        detail = "P9D must not update golden baseline files."
    }
}

$p9dStartupPointerChanged = @($gitChangedFiles | Where-Object { $_ -match 'startup-preset\.txt$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p9d_no_startup_preset_pointer_update"
    target = "git diff"
    passed = (-not $p9dStartupPointerChanged)
    detail = if ($p9dStartupPointerChanged) { "startup-preset.txt changed" } else { "startup-preset.txt not changed" }
}
if ($p9dStartupPointerChanged) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9d_startup_preset_pointer_updated"
        file = "startup-preset.txt"
        function = "P9D startup preset guard"
        detail = "P9D must not update startup-preset.txt."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9dSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9dSchemaUnchanged
        detail = if ($p9dSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9dSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9D must not bump schema."
        }
    }
}

if ($hasP9dResults) {
    $p9dDocText = Get-Content -LiteralPath $p9dResultsPath -Raw
    $p9dDocsHaveApprovedMarker = $p9dDocText -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_docs_no_factory_approved"
        target = $p9dResultsRel
        passed = (-not $p9dDocsHaveApprovedMarker)
        detail = if ($p9dDocsHaveApprovedMarker) { "forbidden readiness marker found in P9D docs" } else { "P9D docs avoid forbidden readiness marker" }
    }
    if ($p9dDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_docs_factory_approved_present"
            file = Normalize-RelPath($p9dResultsRel)
            function = "P9D documentation"
            detail = "P9D docs must not mark presets as shipping approved."
        }
    }

    $p9dDocsKeepPending = ($p9dDocText -match 'Manual listening QA general remains pending') -and
        ($p9dDocText -match 'Distortion manual listening QA remains pending') -and
        ($p9dDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p9d_docs_keep_pending_statuses"
        target = $p9dResultsRel
        passed = $p9dDocsKeepPending
        detail = if ($p9dDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending" } else { "one or more pending markers missing" }
    }
    if (-not $p9dDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9d_docs_pending_marker_missing"
            file = Normalize-RelPath($p9dResultsRel)
            function = "P9D documentation"
            detail = "P9D docs must keep manual listening, Distortion listening, and P7F/Reaper pending."
        }
    }
}

# ---------------------------------------------------------------------------
# P9E - Draft preset technical gain-staging validation
# ---------------------------------------------------------------------------

$p9eResultsRel = "docs/p9e-draft-preset-technical-gain-staging-results.md"
$p9eResultsPath = Join-Path $repoRoot $p9eResultsRel
$hasP9eResults = Test-Path $p9eResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p9e_results_doc_present"
    target = $p9eResultsRel
    passed = $hasP9eResults
    detail = if ($hasP9eResults) { "P9E results doc found" } else { "P9E results doc missing" }
}
if (-not $hasP9eResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9e_results_doc_missing"
        file = Normalize-RelPath($p9eResultsRel)
        function = "P9E documentation"
        detail = "P9E requires docs/p9e-draft-preset-technical-gain-staging-results.md."
    }
}

$p9eValidatorRel = "scripts/validate-draft-factory-presets.ps1"
$p9eValidatorPath = Join-Path $repoRoot $p9eValidatorRel
$hasP9eValidator = Test-Path $p9eValidatorPath
$contractChecks += [pscustomobject]@{
    checkId = "p9e_validator_script_present"
    target = $p9eValidatorRel
    passed = $hasP9eValidator
    detail = if ($hasP9eValidator) { "P9E validator script found" } else { "P9E validator script missing" }
}
if (-not $hasP9eValidator) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9e_validator_script_missing"
        file = Normalize-RelPath($p9eValidatorRel)
        function = "P9E tooling"
        detail = "P9E requires scripts/validate-draft-factory-presets.ps1."
    }
}

$p9eJsonReportRel = "artifacts/p9e-draft-preset-gain-staging-report.json"
$p9eJsonReportPath = Join-Path $repoRoot $p9eJsonReportRel
$hasP9eJsonReport = Test-Path $p9eJsonReportPath
$contractChecks += [pscustomobject]@{
    checkId = "p9e_json_report_present"
    target = $p9eJsonReportRel
    passed = $hasP9eJsonReport
    detail = if ($hasP9eJsonReport) { "P9E JSON report found" } else { "P9E JSON report missing; run scripts/validate-draft-factory-presets.ps1" }
}
if (-not $hasP9eJsonReport) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9e_json_report_missing"
        file = Normalize-RelPath($p9eJsonReportRel)
        function = "P9E tooling"
        detail = "P9E validator must write artifacts/p9e-draft-preset-gain-staging-report.json."
    }
}

$p9eTextReportRel = "artifacts/p9e-draft-preset-gain-staging-report.txt"
$p9eTextReportPath = Join-Path $repoRoot $p9eTextReportRel
$hasP9eTextReport = Test-Path $p9eTextReportPath
$contractChecks += [pscustomobject]@{
    checkId = "p9e_text_report_present"
    target = $p9eTextReportRel
    passed = $hasP9eTextReport
    detail = if ($hasP9eTextReport) { "P9E text report found" } else { "P9E text report missing; run scripts/validate-draft-factory-presets.ps1" }
}
if (-not $hasP9eTextReport) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9e_text_report_missing"
        file = Normalize-RelPath($p9eTextReportRel)
        function = "P9E tooling"
        detail = "P9E validator must write artifacts/p9e-draft-preset-gain-staging-report.txt."
    }
}

$p9eReport = $null
if ($hasP9eJsonReport) {
    try {
        $p9eReport = Get-Content -LiteralPath $p9eJsonReportPath -Raw | ConvertFrom-Json
        $contractChecks += [pscustomobject]@{
            checkId = "p9e_json_report_parses"
            target = $p9eJsonReportRel
            passed = $true
            detail = "P9E JSON report parsed"
        }
    }
    catch {
        $contractChecks += [pscustomobject]@{
            checkId = "p9e_json_report_parses"
            target = $p9eJsonReportRel
            passed = $false
            detail = "P9E JSON report parse failed"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_json_report_invalid"
            file = Normalize-RelPath($p9eJsonReportRel)
            function = "P9E tooling"
            detail = "P9E report must parse as JSON."
        }
    }
}

if ($null -ne $p9eReport) {
    $p9eStatusAllowed = [string]$p9eReport.status -eq "PASS" -or [string]$p9eReport.status -eq "WARN"
    $p9eFailureCount = @($p9eReport.failures).Count
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_report_status_pass_or_warn"
        target = $p9eJsonReportRel
        passed = ($p9eStatusAllowed -and $p9eFailureCount -eq 0)
        detail = "status=$($p9eReport.status); failures=$p9eFailureCount"
    }
    if (-not $p9eStatusAllowed -or $p9eFailureCount -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_report_has_failures"
            file = Normalize-RelPath($p9eJsonReportRel)
            function = "P9E tooling"
            detail = "P9E validator must be PASS or documented WARN with zero failures."
        }
    }

    $p9ePresetCount = @($p9eReport.presets).Count
    $p9ePresetStatusesAllowed = @($p9eReport.presets | Where-Object {
        ([string]$_.status -ne "PASS" -and [string]$_.status -ne "WARN") -or
        ([string]$_.technicalReadiness -ne "DRAFT_TECHNICAL" -and
         [string]$_.technicalReadiness -ne "LISTENING_CANDIDATE" -and
         [string]$_.technicalReadiness -ne "NEEDS_GAIN_STAGING_ADJUSTMENT" -and
         [string]$_.technicalReadiness -ne "BLOCKED_TECHNICAL")
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_per_preset_statuses_allowed"
        target = $p9eJsonReportRel
        passed = ($p9ePresetCount -eq 6 -and $p9ePresetStatusesAllowed)
        detail = "presetCount=$p9ePresetCount; statusesAllowed=$p9ePresetStatusesAllowed"
    }
    if ($p9ePresetCount -ne 6 -or -not $p9ePresetStatusesAllowed) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_per_preset_status_invalid"
            file = Normalize-RelPath($p9eJsonReportRel)
            function = "P9E tooling"
            detail = "P9E report must contain six presets with allowed technical readiness statuses only."
        }
    }

    $p9eNoUserWrites = [bool]$p9eReport.noUserPresetDirectoryWrites
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_no_user_preset_dir_writes"
        target = $p9eJsonReportRel
        passed = $p9eNoUserWrites
        detail = if ($p9eNoUserWrites) { "report confirms no user preset directory writes" } else { "report does not confirm no user preset directory writes" }
    }
    if (-not $p9eNoUserWrites) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_user_preset_dir_write_detected"
            file = Normalize-RelPath($p9eJsonReportRel)
            function = "P9E side-effect guard"
            detail = "P9E must not write to the user preset directory."
        }
    }

    $p9eNoStartupWrites = [bool]$p9eReport.noStartupPresetPointerWrites
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_no_startup_preset_pointer_writes"
        target = $p9eJsonReportRel
        passed = $p9eNoStartupWrites
        detail = if ($p9eNoStartupWrites) { "report confirms no startup preset pointer writes" } else { "report does not confirm startup pointer safety" }
    }
    if (-not $p9eNoStartupWrites) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_startup_preset_pointer_write_detected"
            file = Normalize-RelPath($p9eJsonReportRel)
            function = "P9E side-effect guard"
            detail = "P9E must not write startup-preset.txt."
        }
    }

    $p9eNoFactoryApproved = [bool]$p9eReport.noFactoryApproved
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_report_no_factory_approved"
        target = $p9eJsonReportRel
        passed = $p9eNoFactoryApproved
        detail = if ($p9eNoFactoryApproved) { "report confirms no shipping approval marker" } else { "report found shipping approval marker" }
    }
    if (-not $p9eNoFactoryApproved) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_report_factory_approved_present"
            file = Normalize-RelPath($p9eJsonReportRel)
            function = "P9E approval guard"
            detail = "P9E must not mark presets as shipping approved."
        }
    }
}

if ($hasP9eValidator) {
    $p9eValidatorText = Get-Content -LiteralPath $p9eValidatorPath -Raw
    $p9eValidatorTargetsUserPresetDir = $p9eValidatorText -match 'APPDATA|userApplicationDataDirectory|getFolderPath|NOVA[/\\]Presets'
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_validator_no_user_preset_dir_target"
        target = $p9eValidatorRel
        passed = (-not $p9eValidatorTargetsUserPresetDir)
        detail = if ($p9eValidatorTargetsUserPresetDir) { "user preset directory target found" } else { "validator does not target user preset directory" }
    }
    if ($p9eValidatorTargetsUserPresetDir) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_validator_targets_user_preset_dir"
            file = Normalize-RelPath($p9eValidatorRel)
            function = "P9E tooling"
            detail = "P9E validator must not target the user preset directory."
        }
    }

    $p9eValidatorStartupWriteLogic = $p9eValidatorText -match 'startup-preset\.txt.{0,160}(Set-Content|replaceWithText|Out-File)|writeStartupPresetPointer\s*\('
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_validator_no_startup_preset_update_logic"
        target = $p9eValidatorRel
        passed = (-not $p9eValidatorStartupWriteLogic)
        detail = if ($p9eValidatorStartupWriteLogic) { "startup preset write logic found" } else { "no startup preset write logic" }
    }
    if ($p9eValidatorStartupWriteLogic) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_validator_updates_startup_preset"
            file = Normalize-RelPath($p9eValidatorRel)
            function = "P9E tooling"
            detail = "P9E validator must not update startup-preset.txt."
        }
    }
}

if ($hasP9bManifestJson) {
    if ([string]::IsNullOrWhiteSpace($p9bManifestRaw)) {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
    }
    if ($null -eq $p9bManifest) {
        try { $p9bManifest = $p9bManifestRaw | ConvertFrom-Json } catch { $p9bManifest = $null }
    }

    $p9eManifestHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p9eManifestHasApprovedMarker)
        detail = if ($p9eManifestHasApprovedMarker) { "forbidden readiness marker found" } else { "manifest has no shipping approval marker" }
    }
    if ($p9eManifestHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9E manifest"
            detail = "P9E must not mark presets as shipping approved."
        }
    }

    if ($null -ne $p9bManifest) {
        $p9eManifestPaths = @($p9bManifest.presets | ForEach-Object { [string]$_.filePath } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $p9eManifestPathsOutsideDraft = @($p9eManifestPaths | Where-Object { ($_ -replace '\\', '/') -notmatch '^Resources/Presets/DraftFactory/generated/[^/]+\.nova-preset$' })
        $contractChecks += [pscustomobject]@{
            checkId = "p9e_manifest_references_only_generated_draft_folder"
            target = $p9bManifestJsonRel
            passed = ($p9eManifestPaths.Count -eq 6 -and $p9eManifestPathsOutsideDraft.Count -eq 0)
            detail = "paths=$($p9eManifestPaths.Count); outside=$($p9eManifestPathsOutsideDraft.Count)"
        }
        if ($p9eManifestPaths.Count -ne 6 -or $p9eManifestPathsOutsideDraft.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9e_manifest_path_invalid"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9E manifest"
                detail = "P9E manifest filePath values must stay under Resources/Presets/DraftFactory/generated."
            }
        }

        $p9ePendingStatuses = @($p9bManifest.presets | Where-Object {
            [string]$_.manualListeningStatus -ne "pending" -or
            (([string]$_.distortionListeningStatus -ne "pending") -and ([string]$_.distortionListeningStatus -ne "not_applicable")) -or
            [string]$_.reaperSmokeStatus -ne "pending"
        })
        $contractChecks += [pscustomobject]@{
            checkId = "p9e_manifest_keeps_pending_statuses"
            target = $p9bManifestJsonRel
            passed = ($p9ePendingStatuses.Count -eq 0)
            detail = if ($p9ePendingStatuses.Count -eq 0) { "manual, Distortion, and Reaper statuses remain pending/not_applicable" } else { "invalid statuses=$($p9ePendingStatuses.Count)" }
        }
        if ($p9ePendingStatuses.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9e_manifest_pending_status_changed"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9E manifest"
                detail = "P9E must keep manual listening, Distortion listening, and Reaper smoke pending/not_applicable."
            }
        }
    }
}

$p9ePresetFilesOutsideGenerated = @($p9cAllPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
$contractChecks += [pscustomobject]@{
    checkId = "p9e_generated_presets_only_in_generated_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p9ePresetFilesOutsideGenerated.Count -eq 0)
    detail = if ($p9ePresetFilesOutsideGenerated.Count -eq 0) { "all .nova-preset files are under generated draft folder" } else { "outside=" + ($p9ePresetFilesOutsideGenerated -join ",") }
}
if ($p9ePresetFilesOutsideGenerated.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9e_generated_preset_outside_generated_draft_folder"
        file = "repo"
        function = "P9E preset generation guard"
        detail = "P9E generated preset files must be under Resources/Presets/DraftFactory/generated only."
    }
}

$p9eGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9e_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9eGoldenBaselineChanged.Count -eq 0)
    detail = if ($p9eGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9eGoldenBaselineChanged -join ",") }
}
if ($p9eGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9e_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9E baseline guard"
        detail = "P9E must not update golden baseline files."
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP9eKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP9eKnownFailureBypass)
        detail = if ($hasP9eKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP9eKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P9E validation guard"
            detail = "P9E must not add known-failure ignores to base validation."
        }
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9eSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9eSchemaUnchanged
        detail = if ($p9eSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9eSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9E must not bump schema."
        }
    }
}

if ($hasP9eResults) {
    $p9eDocText = Get-Content -LiteralPath $p9eResultsPath -Raw
    $p9eDocsHaveApprovedMarker = $p9eDocText -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_docs_no_factory_approved"
        target = $p9eResultsRel
        passed = (-not $p9eDocsHaveApprovedMarker)
        detail = if ($p9eDocsHaveApprovedMarker) { "forbidden readiness marker found in P9E docs" } else { "P9E docs avoid shipping approval marker" }
    }
    if ($p9eDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_docs_factory_approved_present"
            file = Normalize-RelPath($p9eResultsRel)
            function = "P9E documentation"
            detail = "P9E docs must not mark presets as shipping approved."
        }
    }

    $p9eDocsKeepPending = ($p9eDocText -match 'Manual listening QA general remains pending') -and
        ($p9eDocText -match 'Distortion manual listening QA remains pending') -and
        ($p9eDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p9e_docs_keep_pending_statuses"
        target = $p9eResultsRel
        passed = $p9eDocsKeepPending
        detail = if ($p9eDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending" } else { "one or more pending markers missing" }
    }
    if (-not $p9eDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9e_docs_pending_marker_missing"
            file = Normalize-RelPath($p9eResultsRel)
            function = "P9E documentation"
            detail = "P9E docs must keep manual listening, Distortion listening, and P7F/Reaper pending."
        }
    }
}

# ---------------------------------------------------------------------------
# P9F - Draft preset direct limiter telemetry gate
# ---------------------------------------------------------------------------

$p9fResultsRel = "docs/p9f-draft-preset-direct-limiter-telemetry-results.md"
$p9fResultsPath = Join-Path $repoRoot $p9fResultsRel
$hasP9fResults = Test-Path $p9fResultsPath
$contractChecks += [pscustomobject]@{
    checkId = "p9f_results_doc_present"
    target = $p9fResultsRel
    passed = $hasP9fResults
    detail = if ($hasP9fResults) { "P9F results doc found" } else { "P9F results doc missing" }
}
if (-not $hasP9fResults) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9f_results_doc_missing"
        file = Normalize-RelPath($p9fResultsRel)
        function = "P9F documentation"
        detail = "P9F requires docs/p9f-draft-preset-direct-limiter-telemetry-results.md."
    }
}

$p9fJsonReportRel = "artifacts/p9f-draft-preset-limiter-telemetry-report.json"
$p9fJsonReportPath = Join-Path $repoRoot $p9fJsonReportRel
$hasP9fJsonReport = Test-Path $p9fJsonReportPath
$contractChecks += [pscustomobject]@{
    checkId = "p9f_json_report_present"
    target = $p9fJsonReportRel
    passed = $hasP9fJsonReport
    detail = if ($hasP9fJsonReport) { "P9F JSON report found" } else { "P9F JSON report missing" }
}
if (-not $hasP9fJsonReport) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9f_json_report_missing"
        file = Normalize-RelPath($p9fJsonReportRel)
        function = "P9F tooling"
        detail = "P9F validator must write artifacts/p9f-draft-preset-limiter-telemetry-report.json."
    }
}

$p9fTextReportRel = "artifacts/p9f-draft-preset-limiter-telemetry-report.txt"
$p9fTextReportPath = Join-Path $repoRoot $p9fTextReportRel
$hasP9fTextReport = Test-Path $p9fTextReportPath
$contractChecks += [pscustomobject]@{
    checkId = "p9f_text_report_present"
    target = $p9fTextReportRel
    passed = $hasP9fTextReport
    detail = if ($hasP9fTextReport) { "P9F text report found" } else { "P9F text report missing" }
}
if (-not $hasP9fTextReport) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9f_text_report_missing"
        file = Normalize-RelPath($p9fTextReportRel)
        function = "P9F tooling"
        detail = "P9F validator must write artifacts/p9f-draft-preset-limiter-telemetry-report.txt."
    }
}

$p9fReport = $null
if ($hasP9fJsonReport) {
    try {
        $p9fReport = Get-Content -LiteralPath $p9fJsonReportPath -Raw | ConvertFrom-Json
        $contractChecks += [pscustomobject]@{
            checkId = "p9f_json_report_parses"
            target = $p9fJsonReportRel
            passed = $true
            detail = "P9F JSON report parsed"
        }
    }
    catch {
        $contractChecks += [pscustomobject]@{
            checkId = "p9f_json_report_parses"
            target = $p9fJsonReportRel
            passed = $false
            detail = "P9F JSON report parse failed"
        }
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_json_report_invalid"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9F tooling"
            detail = "P9F report must parse as JSON."
        }
    }
}

if ($null -ne $p9fReport) {
    $p9fFailureCount = @($p9fReport.failures).Count
    $p9fStatusPass = [string]$p9fReport.status -eq "PASS"
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_report_status_pass"
        target = $p9fJsonReportRel
        passed = ($p9fStatusPass -and $p9fFailureCount -eq 0)
        detail = "status=$($p9fReport.status); failures=$p9fFailureCount"
    }
    if (-not $p9fStatusPass -or $p9fFailureCount -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_report_not_pass"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9F tooling"
            detail = "P9F direct limiter telemetry gate must PASS with zero failures."
        }
    }

    $p9fPresetCount = @($p9fReport.presets).Count
    $p9fLimiterTelemetryOk = @($p9fReport.presets | Where-Object {
        $m = $_.metrics
        $null -eq $m -or
        $null -eq $m.limiterTouchedSamples -or
        $null -eq $m.limiterActiveBlocks -or
        $null -eq $m.sustainedClampBlocks -or
        [int]$m.limiterTouchedSamples -ne 0 -or
        [int]$m.limiterActiveBlocks -ne 0 -or
        [int]$m.sustainedClampBlocks -ne 0
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_direct_limiter_telemetry_zero_for_drafts"
        target = $p9fJsonReportRel
        passed = ($p9fPresetCount -eq 6 -and $p9fLimiterTelemetryOk)
        detail = "presetCount=$p9fPresetCount; limiterTelemetryZero=$p9fLimiterTelemetryOk"
    }
    if ($p9fPresetCount -ne 6 -or -not $p9fLimiterTelemetryOk) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_limiter_telemetry_gate_failed"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9F telemetry gate"
            detail = "P9F requires limiterTouchedSamples, limiterActiveBlocks, and sustainedClampBlocks to be present and zero for all six drafts."
        }
    }

    $p9fReadinessAllowed = @($p9fReport.presets | Where-Object {
        ([string]$_.technicalReadiness -ne "LISTENING_CANDIDATE" -and
         [string]$_.technicalReadiness -ne "NEEDS_GAIN_STAGING_ADJUSTMENT" -and
         [string]$_.technicalReadiness -ne "BLOCKED_TECHNICAL")
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_readiness_values_allowed"
        target = $p9fJsonReportRel
        passed = $p9fReadinessAllowed
        detail = if ($p9fReadinessAllowed) { "readiness values are P9F-allowed" } else { "invalid readiness value found" }
    }
    if (-not $p9fReadinessAllowed) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_readiness_value_invalid"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9F readiness"
            detail = "P9F readiness must be LISTENING_CANDIDATE, NEEDS_GAIN_STAGING_ADJUSTMENT, or BLOCKED_TECHNICAL."
        }
    }

    $p9fNoUserWrites = [bool]$p9fReport.noUserPresetDirectoryWrites
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_no_user_preset_dir_writes"
        target = $p9fJsonReportRel
        passed = $p9fNoUserWrites
        detail = if ($p9fNoUserWrites) { "report confirms no user preset directory writes" } else { "report does not confirm no user preset directory writes" }
    }
    if (-not $p9fNoUserWrites) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_user_preset_dir_write_detected"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9F side-effect guard"
            detail = "P9F must not write to the user preset directory."
        }
    }

    $p9fNoStartupWrites = [bool]$p9fReport.noStartupPresetPointerWrites
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_no_startup_preset_pointer_writes"
        target = $p9fJsonReportRel
        passed = $p9fNoStartupWrites
        detail = if ($p9fNoStartupWrites) { "report confirms no startup preset pointer writes" } else { "report does not confirm startup pointer safety" }
    }
    if (-not $p9fNoStartupWrites) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_startup_preset_pointer_write_detected"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9F side-effect guard"
            detail = "P9F must not write startup-preset.txt."
        }
    }
}

if ($hasP9eValidator) {
    if ($null -eq $p9eValidatorText) {
        $p9eValidatorText = Get-Content -LiteralPath $p9eValidatorPath -Raw
    }

    $p9fValidatorUsesDirectTelemetry = $p9eValidatorText -match 'limiterTouchedSamples' -and
        $p9eValidatorText -match 'limiterActiveBlocks' -and
        $p9eValidatorText -match 'sustainedClampBlocks'
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_validator_uses_direct_limiter_telemetry"
        target = $p9eValidatorRel
        passed = $p9fValidatorUsesDirectTelemetry
        detail = if ($p9fValidatorUsesDirectTelemetry) { "validator reads direct limiter telemetry" } else { "direct limiter telemetry markers missing" }
    }
    if (-not $p9fValidatorUsesDirectTelemetry) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_validator_missing_direct_limiter_telemetry"
            file = Normalize-RelPath($p9eValidatorRel)
            function = "P9F tooling"
            detail = "P9F validator must consume direct limiter telemetry when available."
        }
    }
}

if ($hasP9bManifestJson) {
    if ([string]::IsNullOrWhiteSpace($p9bManifestRaw)) {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
    }
    if ($null -eq $p9bManifest) {
        try { $p9bManifest = $p9bManifestRaw | ConvertFrom-Json } catch { $p9bManifest = $null }
    }

    $p9fManifestHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p9fManifestHasApprovedMarker)
        detail = if ($p9fManifestHasApprovedMarker) { "forbidden readiness marker found" } else { "manifest has no shipping approval marker" }
    }
    if ($p9fManifestHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9F manifest"
            detail = "P9F must not mark presets as shipping approved."
        }
    }

    if ($null -ne $p9bManifest) {
        $p9fManifestPaths = @($p9bManifest.presets | ForEach-Object { [string]$_.filePath } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $p9fManifestPathsOutsideDraft = @($p9fManifestPaths | Where-Object { ($_ -replace '\\', '/') -notmatch '^Resources/Presets/DraftFactory/generated/[^/]+\.nova-preset$' })
        $contractChecks += [pscustomobject]@{
            checkId = "p9f_manifest_references_only_generated_draft_folder"
            target = $p9bManifestJsonRel
            passed = ($p9fManifestPaths.Count -eq 6 -and $p9fManifestPathsOutsideDraft.Count -eq 0)
            detail = "paths=$($p9fManifestPaths.Count); outside=$($p9fManifestPathsOutsideDraft.Count)"
        }
        if ($p9fManifestPaths.Count -ne 6 -or $p9fManifestPathsOutsideDraft.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9f_manifest_path_invalid"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9F manifest"
                detail = "P9F manifest filePath values must stay under Resources/Presets/DraftFactory/generated."
            }
        }

        $p9fPendingStatuses = @($p9bManifest.presets | Where-Object {
            [string]$_.manualListeningStatus -ne "pending" -or
            (([string]$_.distortionListeningStatus -ne "pending") -and ([string]$_.distortionListeningStatus -ne "not_applicable")) -or
            [string]$_.reaperSmokeStatus -ne "pending"
        })
        $contractChecks += [pscustomobject]@{
            checkId = "p9f_manifest_keeps_pending_statuses"
            target = $p9bManifestJsonRel
            passed = ($p9fPendingStatuses.Count -eq 0)
            detail = if ($p9fPendingStatuses.Count -eq 0) { "manual, Distortion, and Reaper statuses remain pending/not_applicable" } else { "invalid statuses=$($p9fPendingStatuses.Count)" }
        }
        if ($p9fPendingStatuses.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9f_manifest_pending_status_changed"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9F manifest"
                detail = "P9F must keep manual listening, Distortion listening, and Reaper smoke pending/not_applicable."
            }
        }
    }
}

$p9fPresetFilesOutsideGenerated = @($p9cAllPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
$contractChecks += [pscustomobject]@{
    checkId = "p9f_generated_presets_only_in_generated_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p9fPresetFilesOutsideGenerated.Count -eq 0)
    detail = if ($p9fPresetFilesOutsideGenerated.Count -eq 0) { "all .nova-preset files are under generated draft folder" } else { "outside=" + ($p9fPresetFilesOutsideGenerated -join ",") }
}
if ($p9fPresetFilesOutsideGenerated.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9f_generated_preset_outside_generated_draft_folder"
        file = "repo"
        function = "P9F preset generation guard"
        detail = "P9F generated preset files must be under Resources/Presets/DraftFactory/generated only."
    }
}

$p9fGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9f_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9fGoldenBaselineChanged.Count -eq 0)
    detail = if ($p9fGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9fGoldenBaselineChanged -join ",") }
}
if ($p9fGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9f_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9F baseline guard"
        detail = "P9F must not update golden baseline files."
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP9fKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP9fKnownFailureBypass)
        detail = if ($hasP9fKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP9fKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P9F validation guard"
            detail = "P9F must not add known-failure ignores to base validation."
        }
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9fSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9fSchemaUnchanged
        detail = if ($p9fSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9fSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9F must not bump schema."
        }
    }
}

$p9fForbiddenAudioPathChanges = @($gitChangedFiles | Where-Object {
    $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' -or
    $_ -match '^Source/Core/AudioEngine\.(h|cpp)$' -or
    $_ -match '^Source/Core/Audio/DryWetMixer\.h$' -or
    $_ -match '^Source/Core/Audio/RoutingMixer\.h$' -or
    $_ -match '^Source/Core/Audio/GraphBuilder\.h$'
})
$contractChecks += [pscustomobject]@{
    checkId = "p9f_no_audio_path_behavior_files_changed"
    target = "git diff"
    passed = ($p9fForbiddenAudioPathChanges.Count -eq 0)
    detail = if ($p9fForbiddenAudioPathChanges.Count -eq 0) { "no OutputChain/AudioEngine/DryWet/Routing/GraphBuilder file changes" } else { "changed=" + ($p9fForbiddenAudioPathChanges -join ",") }
}
if ($p9fForbiddenAudioPathChanges.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9f_audio_path_behavior_file_changed"
        file = "git diff"
        function = "P9F scope guard"
        detail = "P9F must not change OutputChain, AudioEngine, DryWetMixer, RoutingMixer, or GraphBuilder behavior files."
    }
}

if ($hasP9fResults) {
    $p9fDocText = Get-Content -LiteralPath $p9fResultsPath -Raw
    $p9fDocsHaveApprovedMarker = $p9fDocText -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_docs_no_factory_approved"
        target = $p9fResultsRel
        passed = (-not $p9fDocsHaveApprovedMarker)
        detail = if ($p9fDocsHaveApprovedMarker) { "forbidden readiness marker found in P9F docs" } else { "P9F docs avoid shipping approval marker" }
    }
    if ($p9fDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_docs_factory_approved_present"
            file = Normalize-RelPath($p9fResultsRel)
            function = "P9F documentation"
            detail = "P9F docs must not mark presets as shipping approved."
        }
    }

    $p9fDocsKeepPending = ($p9fDocText -match 'Manual listening QA general remains pending') -and
        ($p9fDocText -match 'Distortion manual listening QA remains pending') -and
        ($p9fDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p9f_docs_keep_pending_statuses"
        target = $p9fResultsRel
        passed = $p9fDocsKeepPending
        detail = if ($p9fDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending" } else { "one or more pending markers missing" }
    }
    if (-not $p9fDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9f_docs_pending_marker_missing"
            file = Normalize-RelPath($p9fResultsRel)
            function = "P9F documentation"
            detail = "P9F docs must keep manual listening, Distortion listening, and P7F/Reaper pending."
        }
    }
}

# ---------------------------------------------------------------------------
# P9G - Technical readiness snapshot and manual QA handoff
# ---------------------------------------------------------------------------

$p9gSnapshotRel = "docs/p9g-technical-readiness-snapshot.md"
$p9gHandoffRel = "docs/p9g-manual-qa-handoff.md"
$p9gChecklistRel = "docs/p9g-release-readiness-checklist.md"
$p9gResultsRel = "docs/p9g-technical-readiness-snapshot-results.md"
$p9gDocRels = @($p9gSnapshotRel, $p9gHandoffRel, $p9gChecklistRel, $p9gResultsRel)
$p9gDocTexts = @()

foreach ($p9gDocRel in $p9gDocRels) {
    $p9gDocPath = Join-Path $repoRoot $p9gDocRel
    $hasP9gDoc = Test-Path $p9gDocPath
    $p9gCheckName = ($p9gDocRel -replace '^docs/', '' -replace '[^A-Za-z0-9]+', '_' -replace '_$', '').ToLowerInvariant()
    $contractChecks += [pscustomobject]@{
        checkId = "p9g_${p9gCheckName}_present"
        target = $p9gDocRel
        passed = $hasP9gDoc
        detail = if ($hasP9gDoc) { "P9G document found" } else { "P9G document missing" }
    }
    if ($hasP9gDoc) {
        $p9gDocTexts += Get-Content -LiteralPath $p9gDocPath -Raw
    }
    else {
        $contractFailures += [pscustomobject]@{
            checkId = "p9g_${p9gCheckName}_missing"
            file = Normalize-RelPath($p9gDocRel)
            function = "P9G documentation"
            detail = "P9G requires $p9gDocRel."
        }
    }
}

$p9gCombinedDocText = $p9gDocTexts -join "`n"
$p9gDocsHaveApprovedMarker = $p9gCombinedDocText -match 'FACTORY_APPROVED'
$contractChecks += [pscustomobject]@{
    checkId = "p9g_docs_no_factory_approved"
    target = "docs/p9g-*"
    passed = (-not $p9gDocsHaveApprovedMarker)
    detail = if ($p9gDocsHaveApprovedMarker) { "forbidden readiness marker found in P9G docs" } else { "P9G docs avoid shipping approval marker" }
}
if ($p9gDocsHaveApprovedMarker) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9g_docs_factory_approved_present"
        file = "docs/p9g-*"
        function = "P9G documentation"
        detail = "P9G docs must not mark presets as shipping approved."
    }
}

$p9gDocsKeepPending = ($p9gCombinedDocText -match 'Manual listening QA general remains pending') -and
    ($p9gCombinedDocText -match 'Distortion manual listening QA remains pending') -and
    ($p9gCombinedDocText -match 'P7F/Reaper remains pending')
$contractChecks += [pscustomobject]@{
    checkId = "p9g_docs_keep_pending_statuses"
    target = "docs/p9g-*"
    passed = $p9gDocsKeepPending
    detail = if ($p9gDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending" } else { "one or more pending markers missing" }
}
if (-not $p9gDocsKeepPending) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9g_docs_pending_marker_missing"
        file = "docs/p9g-*"
        function = "P9G documentation"
        detail = "P9G docs must keep manual listening, Distortion listening, and P7F/Reaper pending."
    }
}

if ($hasP9bManifestJson) {
    if ([string]::IsNullOrWhiteSpace($p9bManifestRaw)) {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
    }
    if ($null -eq $p9bManifest) {
        try { $p9bManifest = $p9bManifestRaw | ConvertFrom-Json } catch { $p9bManifest = $null }
    }

    $p9gManifestHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p9g_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p9gManifestHasApprovedMarker)
        detail = if ($p9gManifestHasApprovedMarker) { "forbidden readiness marker found" } else { "manifest has no shipping approval marker" }
    }
    if ($p9gManifestHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9g_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P9G manifest"
            detail = "P9G must not mark presets as shipping approved."
        }
    }

    if ($null -ne $p9bManifest) {
        $p9gPendingStatuses = @($p9bManifest.presets | Where-Object {
            [string]$_.manualListeningStatus -ne "pending" -or
            (([string]$_.distortionListeningStatus -ne "pending") -and ([string]$_.distortionListeningStatus -ne "not_applicable")) -or
            [string]$_.reaperSmokeStatus -ne "pending"
        })
        $contractChecks += [pscustomobject]@{
            checkId = "p9g_manifest_keeps_pending_statuses"
            target = $p9bManifestJsonRel
            passed = ($p9gPendingStatuses.Count -eq 0)
            detail = if ($p9gPendingStatuses.Count -eq 0) { "manual, Distortion, and Reaper statuses remain pending/not_applicable" } else { "invalid statuses=$($p9gPendingStatuses.Count)" }
        }
        if ($p9gPendingStatuses.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p9g_manifest_pending_status_changed"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P9G manifest"
                detail = "P9G must keep manual listening, Distortion listening, and Reaper smoke pending/not_applicable."
            }
        }
    }
}

$p9gPresetFilesOutsideGenerated = @($p9cAllPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
$contractChecks += [pscustomobject]@{
    checkId = "p9g_generated_presets_only_in_generated_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p9gPresetFilesOutsideGenerated.Count -eq 0)
    detail = if ($p9gPresetFilesOutsideGenerated.Count -eq 0) { "all .nova-preset files are under generated draft folder" } else { "outside=" + ($p9gPresetFilesOutsideGenerated -join ",") }
}
if ($p9gPresetFilesOutsideGenerated.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9g_generated_preset_outside_generated_draft_folder"
        file = "repo"
        function = "P9G preset guard"
        detail = "P9G draft preset files must remain under Resources/Presets/DraftFactory/generated only."
    }
}

$p9gGeneratedPresetFiles = @(Get-ChildItem -Path (Join-Path $repoRoot "Resources/Presets/DraftFactory/generated") -Filter "*.nova-preset" -File -ErrorAction SilentlyContinue)
$contractChecks += [pscustomobject]@{
    checkId = "p9g_generated_preset_count"
    target = "Resources/Presets/DraftFactory/generated"
    passed = ($p9gGeneratedPresetFiles.Count -eq 6)
    detail = "generatedPresetCount=$($p9gGeneratedPresetFiles.Count)"
}
if ($p9gGeneratedPresetFiles.Count -ne 6) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9g_generated_preset_count_invalid"
        file = "Resources/Presets/DraftFactory/generated"
        function = "P9G preset guard"
        detail = "P9G expects exactly six generated draft preset files."
    }
}

$p9gGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p9g_no_golden_baseline_update"
    target = "git diff"
    passed = ($p9gGoldenBaselineChanged.Count -eq 0)
    detail = if ($p9gGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p9gGoldenBaselineChanged -join ",") }
}
if ($p9gGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9g_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P9G baseline guard"
        detail = "P9G must not update golden baseline files."
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP9gKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p9g_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP9gKnownFailureBypass)
        detail = if ($hasP9gKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP9gKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9g_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P9G validation guard"
            detail = "P9G must not add known-failure ignores to base validation."
        }
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p9gSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p9g_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p9gSchemaUnchanged
        detail = if ($p9gSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p9gSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9g_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P9G must not bump schema."
        }
    }
}

$p9gForbiddenAudioPathChanges = @($gitChangedFiles | Where-Object {
    $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' -or
    $_ -match '^Source/Core/AudioEngine\.(h|cpp)$' -or
    $_ -match '^Source/Core/Audio/DryWetMixer\.h$' -or
    $_ -match '^Source/Core/Audio/RoutingMixer\.h$' -or
    $_ -match '^Source/Core/Audio/GraphBuilder\.h$'
})
$contractChecks += [pscustomobject]@{
    checkId = "p9g_no_audio_path_behavior_files_changed"
    target = "git diff"
    passed = ($p9gForbiddenAudioPathChanges.Count -eq 0)
    detail = if ($p9gForbiddenAudioPathChanges.Count -eq 0) { "no OutputChain/AudioEngine/DryWet/Routing/GraphBuilder file changes" } else { "changed=" + ($p9gForbiddenAudioPathChanges -join ",") }
}
if ($p9gForbiddenAudioPathChanges.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p9g_audio_path_behavior_file_changed"
        file = "git diff"
        function = "P9G scope guard"
        detail = "P9G must not change OutputChain, AudioEngine, DryWetMixer, RoutingMixer, or GraphBuilder behavior files."
    }
}

if ($null -ne $p9fReport) {
    $p9gP9fReportStillPass = ([string]$p9fReport.status -eq "PASS") -and (@($p9fReport.failures).Count -eq 0)
    $p9gP9fLimiterTelemetryStillZero = @($p9fReport.presets | Where-Object {
        $m = $_.metrics
        $null -eq $m -or
        [int]$m.limiterTouchedSamples -ne 0 -or
        [int]$m.limiterActiveBlocks -ne 0 -or
        [int]$m.sustainedClampBlocks -ne 0 -or
        [double]$m.limiterMaxReductionDb -ne 0.0 -or
        [int]$m.nearClipSamples -ne 0 -or
        [int]$m.clippedSamples -ne 0 -or
        [int]$m.invalidSamples -ne 0
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p9g_p9f_limiter_report_still_pass"
        target = $p9fJsonReportRel
        passed = ($p9gP9fReportStillPass -and $p9gP9fLimiterTelemetryStillZero)
        detail = "status=$($p9fReport.status); limiterTelemetryZero=$p9gP9fLimiterTelemetryStillZero"
    }
    if (-not $p9gP9fReportStillPass -or -not $p9gP9fLimiterTelemetryStillZero) {
        $contractFailures += [pscustomobject]@{
            checkId = "p9g_p9f_limiter_report_not_pass"
            file = Normalize-RelPath($p9fJsonReportRel)
            function = "P9G technical snapshot"
            detail = "P9G expects the P9F limiter telemetry report to remain PASS with zero limiter, near-clip, clipped, and invalid samples."
        }
    }
}

# ---------------------------------------------------------------------------
# P10A - Manual listening QA run pack for six draft presets
# ---------------------------------------------------------------------------

$p10aRunPackRel = "docs/p10a-six-draft-preset-listening-run-pack.md"
$p10aChecklistRel = "docs/p10a-six-draft-preset-listening-checklist.md"
$p10aResultsTemplateRel = "docs/p10a-six-draft-preset-listening-results-template.md"
$p10aPromotionRulesRel = "docs/p10a-draft-preset-promotion-rules.md"
$p10aIssueRoutingRel = "docs/p10a-draft-preset-listening-issue-routing.md"
$p10aResultsRel = "docs/p10a-six-draft-preset-listening-run-pack-results.md"
$p10aArtifactReadmeRel = "artifacts/manual-listening/p10a-six-draft-presets/README.md"
$p10aArtifactChecklistRel = "artifacts/manual-listening/p10a-six-draft-presets/session-checklist.md"
$p10aArtifactCsvRel = "artifacts/manual-listening/p10a-six-draft-presets/results-template.csv"
$p10aDocRels = @(
    $p10aRunPackRel,
    $p10aChecklistRel,
    $p10aResultsTemplateRel,
    $p10aPromotionRulesRel,
    $p10aIssueRoutingRel,
    $p10aResultsRel,
    $p10aArtifactReadmeRel,
    $p10aArtifactChecklistRel,
    $p10aArtifactCsvRel
)
$p10aDocTexts = @()

foreach ($p10aDocRel in $p10aDocRels) {
    $p10aDocPath = Join-Path $repoRoot $p10aDocRel
    $hasP10aDoc = Test-Path $p10aDocPath
    $p10aCheckName = ($p10aDocRel -replace '^(docs|artifacts)/', '' -replace '[^A-Za-z0-9]+', '_' -replace '_$', '').ToLowerInvariant()
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_${p10aCheckName}_present"
        target = $p10aDocRel
        passed = $hasP10aDoc
        detail = if ($hasP10aDoc) { "P10A file found" } else { "P10A file missing" }
    }
    if ($hasP10aDoc) {
        $p10aDocTexts += Get-Content -LiteralPath $p10aDocPath -Raw
    }
    else {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_${p10aCheckName}_missing"
            file = Normalize-RelPath($p10aDocRel)
            function = "P10A run pack"
            detail = "P10A requires $p10aDocRel."
        }
    }
}

$p10aCombinedDocText = $p10aDocTexts -join "`n"
$p10aPresetNames = @(
    "Dry Reference",
    "Clean Studio",
    "Funk Comp Clean",
    "Classic Crunch",
    "Tight Modern Rhythm",
    "Wide Ambient Clean"
)
if (Test-Path (Join-Path $repoRoot $p10aChecklistRel)) {
    $p10aChecklistText = Get-Content -LiteralPath (Join-Path $repoRoot $p10aChecklistRel) -Raw
    $p10aMissingChecklistPresets = @($p10aPresetNames | Where-Object { $p10aChecklistText -notmatch [regex]::Escape($_) })
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_all_six_presets_in_checklist"
        target = $p10aChecklistRel
        passed = ($p10aMissingChecklistPresets.Count -eq 0)
        detail = if ($p10aMissingChecklistPresets.Count -eq 0) { "all six presets appear in checklist" } else { "missing=" + ($p10aMissingChecklistPresets -join ",") }
    }
    if ($p10aMissingChecklistPresets.Count -gt 0) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_checklist_missing_presets"
            file = Normalize-RelPath($p10aChecklistRel)
            function = "P10A checklist"
            detail = "P10A checklist must include all six draft presets."
        }
    }
}

if (Test-Path (Join-Path $repoRoot $p10aResultsTemplateRel)) {
    $p10aTemplateText = Get-Content -LiteralPath (Join-Path $repoRoot $p10aResultsTemplateRel) -Raw
    $p10aTemplateRowsNotRun = @($p10aPresetNames | Where-Object {
        $p10aTemplateText -notmatch ([regex]::Escape($_) + '\s*\|\s*manual_listening\s*\|\s*NOT_RUN')
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_results_template_starts_not_run"
        target = $p10aResultsTemplateRel
        passed = $p10aTemplateRowsNotRun
        detail = if ($p10aTemplateRowsNotRun) { "results template starts all six presets as NOT_RUN" } else { "one or more template rows are not NOT_RUN" }
    }
    if (-not $p10aTemplateRowsNotRun) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_results_template_not_run_missing"
            file = Normalize-RelPath($p10aResultsTemplateRel)
            function = "P10A results template"
            detail = "P10A results template must start all six presets as NOT_RUN."
        }
    }
}

if (Test-Path (Join-Path $repoRoot $p10aArtifactCsvRel)) {
    $p10aCsvText = Get-Content -LiteralPath (Join-Path $repoRoot $p10aArtifactCsvRel) -Raw
    $p10aCsvRowsNotRun = @($p10aPresetNames | Where-Object {
        $p10aCsvText -notmatch ([regex]::Escape($_) + ',manual_listening,NOT_RUN')
    }).Count -eq 0
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_csv_template_starts_not_run"
        target = $p10aArtifactCsvRel
        passed = $p10aCsvRowsNotRun
        detail = if ($p10aCsvRowsNotRun) { "CSV template starts all six presets as NOT_RUN" } else { "one or more CSV rows are not NOT_RUN" }
    }
    if (-not $p10aCsvRowsNotRun) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_csv_template_not_run_missing"
            file = Normalize-RelPath($p10aArtifactCsvRel)
            function = "P10A results CSV"
            detail = "P10A CSV template must start all six presets as NOT_RUN."
        }
    }
}

$p10aDocsMarkFactoryApproved = $p10aCombinedDocText -match '(?i)(Result|Recommendation|Readiness)\s*:\s*FACTORY_APPROVED'
$contractChecks += [pscustomobject]@{
    checkId = "p10a_docs_no_factory_approved_result"
    target = "docs/p10a-* and artifacts/manual-listening/p10a-*"
    passed = (-not $p10aDocsMarkFactoryApproved)
    detail = if ($p10aDocsMarkFactoryApproved) { "P10A docs contain an approved result marker" } else { "P10A docs do not mark any preset factory-approved" }
}
if ($p10aDocsMarkFactoryApproved) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10a_docs_factory_approved_result_present"
        file = "docs/p10a-*"
        function = "P10A approval guard"
        detail = "P10A must not mark any preset as factory-approved."
    }
}

$p10aDocsKeepPending = ($p10aCombinedDocText -match 'Manual listening QA remains pending or NOT_RUN') -and
    ($p10aCombinedDocText -match 'Distortion manual listening QA remains pending') -and
    ($p10aCombinedDocText -match 'P7F/Reaper remains pending')
$contractChecks += [pscustomobject]@{
    checkId = "p10a_docs_keep_pending_statuses"
    target = "docs/p10a-* and artifacts/manual-listening/p10a-*"
    passed = $p10aDocsKeepPending
    detail = if ($p10aDocsKeepPending) { "manual, Distortion, and Reaper statuses remain pending/NOT_RUN" } else { "one or more pending markers missing" }
}
if (-not $p10aDocsKeepPending) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10a_docs_pending_marker_missing"
        file = "docs/p10a-*"
        function = "P10A documentation"
        detail = "P10A docs must keep manual listening pending/NOT_RUN, Distortion listening pending, and P7F/Reaper pending."
    }
}

if ($hasP9bManifestJson) {
    if ([string]::IsNullOrWhiteSpace($p9bManifestRaw)) {
        $p9bManifestRaw = Get-Content -LiteralPath $p9bManifestJsonPath -Raw
    }
    if ($null -eq $p9bManifest) {
        try { $p9bManifest = $p9bManifestRaw | ConvertFrom-Json } catch { $p9bManifest = $null }
    }

    $p10aManifestHasApprovedMarker = $p9bManifestRaw -match 'FACTORY_APPROVED'
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_manifest_no_factory_approved"
        target = $p9bManifestJsonRel
        passed = (-not $p10aManifestHasApprovedMarker)
        detail = if ($p10aManifestHasApprovedMarker) { "forbidden readiness marker found" } else { "manifest has no shipping approval marker" }
    }
    if ($p10aManifestHasApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_manifest_factory_approved_present"
            file = Normalize-RelPath($p9bManifestJsonRel)
            function = "P10A manifest"
            detail = "P10A must not mark presets as shipping approved."
        }
    }

    if ($null -ne $p9bManifest) {
        $p10aPendingStatuses = @($p9bManifest.presets | Where-Object {
            [string]$_.manualListeningStatus -ne "pending" -or
            (([string]$_.distortionListeningStatus -ne "pending") -and ([string]$_.distortionListeningStatus -ne "not_applicable")) -or
            [string]$_.reaperSmokeStatus -ne "pending"
        })
        $contractChecks += [pscustomobject]@{
            checkId = "p10a_manifest_keeps_pending_statuses"
            target = $p9bManifestJsonRel
            passed = ($p10aPendingStatuses.Count -eq 0)
            detail = if ($p10aPendingStatuses.Count -eq 0) { "manual, Distortion, and Reaper statuses remain pending/not_applicable" } else { "invalid statuses=$($p10aPendingStatuses.Count)" }
        }
        if ($p10aPendingStatuses.Count -gt 0) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10a_manifest_pending_status_changed"
                file = Normalize-RelPath($p9bManifestJsonRel)
                function = "P10A manifest"
                detail = "P10A must keep manual listening, Distortion listening, and Reaper smoke pending/not_applicable."
            }
        }
    }
}

$p10aPresetFilesOutsideGenerated = @($p9cAllPresetFiles | Where-Object { $_ -notmatch '^Resources/Presets/DraftFactory/generated/' })
$contractChecks += [pscustomobject]@{
    checkId = "p10a_generated_presets_only_in_generated_draft_folder"
    target = "repo .nova-preset files"
    passed = ($p10aPresetFilesOutsideGenerated.Count -eq 0)
    detail = if ($p10aPresetFilesOutsideGenerated.Count -eq 0) { "all .nova-preset files are under generated draft folder" } else { "outside=" + ($p10aPresetFilesOutsideGenerated -join ",") }
}
if ($p10aPresetFilesOutsideGenerated.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10a_generated_preset_outside_generated_draft_folder"
        file = "repo"
        function = "P10A preset guard"
        detail = "P10A draft preset files must remain under Resources/Presets/DraftFactory/generated only."
    }
}

$p10aGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p10a_no_golden_baseline_update"
    target = "git diff"
    passed = ($p10aGoldenBaselineChanged.Count -eq 0)
    detail = if ($p10aGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p10aGoldenBaselineChanged -join ",") }
}
if ($p10aGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10a_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P10A baseline guard"
        detail = "P10A must not update golden baseline files."
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP10aKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP10aKnownFailureBypass)
        detail = if ($hasP10aKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP10aKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P10A validation guard"
            detail = "P10A must not add known-failure ignores to base validation."
        }
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p10aSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p10a_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p10aSchemaUnchanged
        detail = if ($p10aSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p10aSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10a_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P10A must not bump schema."
        }
    }
}

# ---------------------------------------------------------------------------
# P10C - High-gain professionalization / gain staging and voicing audit
# ---------------------------------------------------------------------------

$p10cAuditRel = "docs/p10c-high-gain-professionalization-audit-results.md"
$p10cResultsRel = "docs/p10c-high-gain-professionalization-results.md"
$p10cDocRels = @($p10cAuditRel, $p10cResultsRel)
$p10cCombinedDocText = ""
foreach ($p10cDocRel in $p10cDocRels) {
    $p10cDocPath = Join-Path $repoRoot $p10cDocRel
    $hasP10cDoc = Test-Path $p10cDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p10c_doc_present"
        target = $p10cDocRel
        passed = $hasP10cDoc
        detail = if ($hasP10cDoc) { "P10C doc found" } else { "P10C doc missing" }
    }
    if (-not $hasP10cDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10c_doc_missing"
            file = Normalize-RelPath($p10cDocRel)
            function = "P10C documentation"
            detail = "P10C requires $p10cDocRel."
        }
    }
    else {
        $p10cCombinedDocText += "`n" + (Get-Content -LiteralPath $p10cDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p10cScenarioNames = @(
        "p10c_high_gain_amp_nominal_palm_mute",
        "p10c_high_gain_amp_extreme_gain_bounded",
        "p10c_distortion_highgainamp_modern4x12_nominal",
        "p10c_boost_highgainamp_modern4x12_nominal",
        "p10c_fuzz_classicamp_cabinet_nominal",
        "p10c_distortion_cleanamp_cabinet_nominal",
        "p10c_high_gain_chain_bypass_recovery",
        "p10c_high_gain_chain_outputchain_limiter_independence"
    )

    foreach ($scenarioName in $p10cScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p10c_high_gain_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10c_high_gain_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10C high-gain diagnostics"
                detail = "AudioEngineTests.cpp must contain P10C scenario '$scenarioName'."
            }
        }
    }

    $p10cMetricsMarkers = @(
        "nearClipSamples",
        "clippedSamples",
        "invalidSamples",
        "brightnessProxy",
        "rumbleProxy",
        "adjacentDeltaPeak",
        "limiterTouchedSamples",
        "limiterActiveBlocks",
        "sustainedClampBlocks"
    )
    foreach ($marker in $p10cMetricsMarkers) {
        $hasMarker = $audioTestText.Contains($marker)
        $contractChecks += [pscustomobject]@{
            checkId = "p10c_metric_marker_present"
            target = $marker
            passed = $hasMarker
            detail = if ($hasMarker) { "metric marker present" } else { "metric marker missing" }
        }
        if (-not $hasMarker) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10c_metric_marker_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10C metric coverage"
                detail = "P10C diagnostics must cover metric '$marker'."
            }
        }
    }
}

$p10cGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p10c_no_golden_baseline_update"
    target = "git diff"
    passed = ($p10cGoldenBaselineChanged.Count -eq 0)
    detail = if ($p10cGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p10cGoldenBaselineChanged -join ",") }
}
if ($p10cGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10c_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P10C baseline guard"
        detail = "P10C must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p10cSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p10c_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p10cSchemaUnchanged
        detail = if ($p10cSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p10cSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10c_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P10C must not bump schema."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP10cKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p10c_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP10cKnownFailureBypass)
        detail = if ($hasP10cKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP10cKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10c_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P10C validation guard"
            detail = "P10C must not add known-failure ignores to base validation."
        }
    }
}

$p10cOutputChainOnlyMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p10c_no_outputchain_only_masking"
    target = "git diff"
    passed = (-not $p10cOutputChainOnlyMasking)
    detail = if ($p10cOutputChainOnlyMasking) { "OutputChain changed during P10C" } else { "OutputChain unchanged" }
}
if ($p10cOutputChainOnlyMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10c_outputchain_only_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P10C masking guard"
        detail = "P10C must not hide high-gain issues with OutputChain-only limiting changes."
    }
}

$p10cDocsHaveApprovedMarker = $p10cCombinedDocText -match 'FACTORY_APPROVED'
$contractChecks += [pscustomobject]@{
    checkId = "p10c_no_factory_approved"
    target = "P10C docs"
    passed = (-not $p10cDocsHaveApprovedMarker)
    detail = if ($p10cDocsHaveApprovedMarker) { "FACTORY_APPROVED marker found" } else { "no factory approval marker" }
}
if ($p10cDocsHaveApprovedMarker) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10c_factory_approved"
        file = "docs/p10c"
        function = "P10C approval guard"
        detail = "P10C must not approve factory presets."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p10cCombinedDocText)) {
    $p10cDocsKeepPending = ($p10cCombinedDocText -match 'Manual listening QA general remains pending') -and
        ($p10cCombinedDocText -match 'Distortion/high-gain listening QA remains pending') -and
        ($p10cCombinedDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p10c_pending_statuses_preserved"
        target = "P10C docs"
        passed = $p10cDocsKeepPending
        detail = if ($p10cDocsKeepPending) { "manual listening, high-gain listening, and Reaper remain pending" } else { "pending markers missing" }
    }
    if (-not $p10cDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10c_pending_status_changed"
            file = "docs/p10c"
            function = "P10C status guard"
            detail = "P10C must keep manual listening, Distortion/high-gain listening, and P7F/Reaper pending unless evidence is attached."
        }
    }
}

# ---------------------------------------------------------------------------
# P10D - High-gain artifact/fizz/helicopter investigation
# ---------------------------------------------------------------------------

$p10dInvestigationRel = "docs/p10d-high-gain-artifact-fizz-helicopter-investigation.md"
$p10dResultsRel = "docs/p10d-high-gain-professionalization-results.md"
$p10dDocRels = @($p10dInvestigationRel, $p10dResultsRel)
$p10dCombinedDocText = ""
foreach ($p10dDocRel in $p10dDocRels) {
    $p10dDocPath = Join-Path $repoRoot $p10dDocRel
    $hasP10dDoc = Test-Path $p10dDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p10d_doc_present"
        target = $p10dDocRel
        passed = $hasP10dDoc
        detail = if ($hasP10dDoc) { "P10D doc found" } else { "P10D doc missing" }
    }
    if (-not $hasP10dDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10d_doc_missing"
            file = Normalize-RelPath($p10dDocRel)
            function = "P10D documentation"
            detail = "P10D requires $p10dDocRel."
        }
    }
    else {
        $p10dCombinedDocText += "`n" + (Get-Content -LiteralPath $p10dDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p10dScenarioNames = @(
        "high_gain_helicopter_modulation_guard",
        "high_gain_fizz_brightness_guard",
        "high_gain_strong_input_no_clipping",
        "tight_modern_rhythm_high_gain_artifact_guard",
        "distortion_highgain_modern4x12_professional_bounds",
        "high_gain_noise_gate_chatter_guard",
        "modern4x12_high_gain_fizz_control"
    )

    foreach ($scenarioName in $p10dScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p10d_high_gain_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10d_high_gain_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10D high-gain diagnostics"
                detail = "AudioEngineTests.cpp must contain P10D scenario '$scenarioName'."
            }
        }
    }

    $p10dMetricsMarkers = @(
        "modulationDepth3To20",
        "blockRmsVariance",
        "peakVariance",
        "highFrequencyEnergyProxy",
        "gateTransitions",
        "gateDeltaPeak",
        "nearClipSamples",
        "clippedSamples",
        "limiterTouchedSamples",
        "sustainedClampBlocks"
    )
    foreach ($marker in $p10dMetricsMarkers) {
        $hasMarker = $audioTestText.Contains($marker)
        $contractChecks += [pscustomobject]@{
            checkId = "p10d_metric_marker_present"
            target = $marker
            passed = $hasMarker
            detail = if ($hasMarker) { "metric marker present" } else { "metric marker missing" }
        }
        if (-not $hasMarker) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10d_metric_marker_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10D metric coverage"
                detail = "P10D diagnostics must cover metric '$marker'."
            }
        }
    }
}

$p10dGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p10d_no_golden_baseline_update"
    target = "git diff"
    passed = ($p10dGoldenBaselineChanged.Count -eq 0)
    detail = if ($p10dGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p10dGoldenBaselineChanged -join ",") }
}
if ($p10dGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10d_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P10D baseline guard"
        detail = "P10D must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p10dSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p10d_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p10dSchemaUnchanged
        detail = if ($p10dSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p10dSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10d_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P10D must not bump schema."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP10dKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p10d_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP10dKnownFailureBypass)
        detail = if ($hasP10dKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP10dKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10d_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P10D validation guard"
            detail = "P10D must not add known-failure ignores to base validation."
        }
    }
}

$p10dOutputChainOnlyMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p10d_no_outputchain_only_masking"
    target = "git diff"
    passed = (-not $p10dOutputChainOnlyMasking)
    detail = if ($p10dOutputChainOnlyMasking) { "OutputChain changed during P10D" } else { "OutputChain unchanged" }
}
if ($p10dOutputChainOnlyMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10d_outputchain_only_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P10D masking guard"
        detail = "P10D must not hide high-gain issues with OutputChain-only limiting changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p10dCombinedDocText)) {
    $p10dDocsHaveApprovedMarker = $p10dCombinedDocText -match 'FACTORY_APPROVED|factory approval.*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p10d_no_factory_approved"
        target = "P10D docs"
        passed = (-not $p10dDocsHaveApprovedMarker)
        detail = if ($p10dDocsHaveApprovedMarker) { "factory approval marker found" } else { "no factory approval marker" }
    }
    if ($p10dDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10d_factory_approved"
            file = "docs/p10d"
            function = "P10D approval guard"
            detail = "P10D must not approve factory presets."
        }
    }

    $p10dDocsKeepPending = ($p10dCombinedDocText -match 'Manual listening QA general remains pending') -and
        ($p10dCombinedDocText -match 'Distortion/high-gain listening QA remains pending') -and
        ($p10dCombinedDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p10d_pending_statuses_preserved"
        target = "P10D docs"
        passed = $p10dDocsKeepPending
        detail = if ($p10dDocsKeepPending) { "manual listening, high-gain listening, and Reaper remain pending" } else { "pending markers missing" }
    }
    if (-not $p10dDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10d_pending_status_changed"
            file = "docs/p10d"
            function = "P10D status guard"
            detail = "P10D must keep manual listening, Distortion/high-gain listening, and P7F/Reaper pending unless evidence is attached."
        }
    }

    $p10dCleanPreservationNoted = ($p10dCombinedDocText -match 'Clean Impact') -and
        ($p10dCombinedDocText -match 'Clean Studio') -and
        ($p10dCombinedDocText -match 'Wide Ambient Clean')
    $contractChecks += [pscustomobject]@{
        checkId = "p10d_clean_preservation_noted"
        target = "P10D docs"
        passed = $p10dCleanPreservationNoted
        detail = if ($p10dCleanPreservationNoted) { "clean preservation documented" } else { "clean preservation markers missing" }
    }
    if (-not $p10dCleanPreservationNoted) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10d_clean_preservation_missing"
            file = "docs/p10d"
            function = "P10D clean guard"
            detail = "P10D docs must note Clean Studio/Wide Ambient Clean preservation."
        }
    }
}

# ---------------------------------------------------------------------------
# P10E - High-gain mute / helicopter / reverb interaction follow-up
# ---------------------------------------------------------------------------

$p10eInvestigationRel = "docs/p10e-high-gain-mute-helicopter-reverb-investigation.md"
$p10eResultsRel = "docs/p10e-high-gain-manual-warn-followup-results.md"
$p10eDocRels = @($p10eInvestigationRel, $p10eResultsRel)
$p10eCombinedDocText = ""
foreach ($p10eDocRel in $p10eDocRels) {
    $p10eDocPath = Join-Path $repoRoot $p10eDocRel
    $hasP10eDoc = Test-Path $p10eDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_doc_present"
        target = $p10eDocRel
        passed = $hasP10eDoc
        detail = if ($hasP10eDoc) { "P10E doc found" } else { "P10E doc missing" }
    }
    if (-not $hasP10eDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_doc_missing"
            file = Normalize-RelPath($p10eDocRel)
            function = "P10E documentation"
            detail = "P10E requires $p10eDocRel."
        }
    }
    else {
        $p10eCombinedDocText += "`n" + (Get-Content -LiteralPath $p10eDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p10eScenarioNames = @(
        "p10e_distortion_highgain_mute_repro",
        "p10e_distortion_cleanamp_helicopter_repro",
        "p10e_distortion_reverb_helicopter_guard",
        "p10e_distortion_reverb_chorus_recovery_guard",
        "p10e_boost_highgain_noise_clipping_guard",
        "p10e_fuzz_classicamp_stuck_silence_guard",
        "p10e_sample_rate_reset_recovers_stuck_chain",
        "p10e_tight_modern_rhythm_availability_doc_check"
    )

    foreach ($scenarioName in $p10eScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p10e_high_gain_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10e_high_gain_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10E high-gain diagnostics"
                detail = "AudioEngineTests.cpp must contain P10E scenario '$scenarioName'."
            }
        }
    }

    $p10eMetricsMarkers = @(
        "P10ELongStageMetrics",
        "silentWhileInputBlocks",
        "maxConsecutiveSilentBlocks",
        "tailRms",
        "modulationDepth3To20",
        "blockRmsVariance",
        "highFrequencyEnergyProxy",
        "nearClipSamples",
        "clippedSamples",
        "invalidSamples",
        "sample-rate-reset"
    )

    foreach ($marker in $p10eMetricsMarkers) {
        $hasMarker = $audioTestText.Contains($marker)
        $contractChecks += [pscustomobject]@{
            checkId = "p10e_metric_marker_present"
            target = $marker
            passed = $hasMarker
            detail = if ($hasMarker) { "metric marker present" } else { "metric marker missing" }
        }
        if (-not $hasMarker) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10e_metric_marker_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10E metric coverage"
                detail = "P10E diagnostics must cover metric '$marker'."
            }
        }
    }
}

$p10eGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p10e_no_golden_baseline_update"
    target = "git diff"
    passed = ($p10eGoldenBaselineChanged.Count -eq 0)
    detail = if ($p10eGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p10eGoldenBaselineChanged -join ",") }
}
if ($p10eGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10e_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P10E baseline guard"
        detail = "P10E must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p10eSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p10eSchemaUnchanged
        detail = if ($p10eSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p10eSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P10E must not bump schema."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP10eKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP10eKnownFailureBypass)
        detail = if ($hasP10eKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP10eKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P10E validation guard"
            detail = "P10E must not add known-failure ignores to base validation."
        }
    }
}

$p10eOutputChainOnlyMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p10e_no_outputchain_only_masking"
    target = "git diff"
    passed = (-not $p10eOutputChainOnlyMasking)
    detail = if ($p10eOutputChainOnlyMasking) { "OutputChain changed during P10E" } else { "OutputChain unchanged" }
}
if ($p10eOutputChainOnlyMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10e_outputchain_only_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P10E masking guard"
        detail = "P10E must not hide high-gain issues with OutputChain-only limiting changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p10eCombinedDocText)) {
    $p10eDocsHaveApprovedMarker = $p10eCombinedDocText -match 'FACTORY_APPROVED|factory approval.*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_no_factory_approved"
        target = "P10E docs"
        passed = (-not $p10eDocsHaveApprovedMarker)
        detail = if ($p10eDocsHaveApprovedMarker) { "factory approval marker found" } else { "no factory approval marker" }
    }
    if ($p10eDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_factory_approved"
            file = "docs/p10e"
            function = "P10E approval guard"
            detail = "P10E must not approve factory presets."
        }
    }

    $p10eNoUiDeployReaperClosure = -not ($p10eCombinedDocText -match 'UI/UX\s*[:=-]\s*PASS|deploy\s*[:=-]\s*PASS|Reaper smoke\s*[:=-]\s*PASS')
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_no_ui_deploy_reaper_closure"
        target = "P10E docs"
        passed = $p10eNoUiDeployReaperClosure
        detail = if ($p10eNoUiDeployReaperClosure) { "no UI/deploy/Reaper closure marker" } else { "UI/deploy/Reaper closure marker found" }
    }
    if (-not $p10eNoUiDeployReaperClosure) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_ui_deploy_reaper_closed"
            file = "docs/p10e"
            function = "P10E scope guard"
            detail = "P10E must not close UI/UX, deploy, or Reaper smoke."
        }
    }

    $p10eDocsKeepPending = ($p10eCombinedDocText -match 'Manual listening QA general remains pending') -and
        ($p10eCombinedDocText -match 'Distortion/high-gain listening QA remains pending') -and
        ($p10eCombinedDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_pending_statuses_preserved"
        target = "P10E docs"
        passed = $p10eDocsKeepPending
        detail = if ($p10eDocsKeepPending) { "manual listening, high-gain listening, and Reaper remain pending" } else { "pending markers missing" }
    }
    if (-not $p10eDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_pending_status_changed"
            file = "docs/p10e"
            function = "P10E status guard"
            detail = "P10E must keep manual listening, Distortion/high-gain listening, and P7F/Reaper pending unless evidence is attached."
        }
    }

    $p10eCleanPreservationNoted = ($p10eCombinedDocText -match 'Clean Impact') -and
        ($p10eCombinedDocText -match 'Clean Studio') -and
        ($p10eCombinedDocText -match 'Wide Ambient Clean')
    $contractChecks += [pscustomobject]@{
        checkId = "p10e_clean_preservation_noted"
        target = "P10E docs"
        passed = $p10eCleanPreservationNoted
        detail = if ($p10eCleanPreservationNoted) { "clean preservation documented" } else { "clean preservation markers missing" }
    }
    if (-not $p10eCleanPreservationNoted) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10e_clean_preservation_missing"
            file = "docs/p10e"
            function = "P10E clean guard"
            detail = "P10E docs must note Clean Studio/Wide Ambient Clean preservation."
        }
    }
}

# ---------------------------------------------------------------------------
# P10F - High-gain architecture root fix / Fuzz reference comparison
# ---------------------------------------------------------------------------

$p10fAuditRel = "docs/p10f-high-gain-root-cause-fuzz-reference-audit.md"
$p10fResultsRel = "docs/p10f-high-gain-architecture-root-fix-results.md"
$p10fDocRels = @($p10fAuditRel, $p10fResultsRel)
$p10fCombinedDocText = ""
foreach ($p10fDocRel in $p10fDocRels) {
    $p10fDocPath = Join-Path $repoRoot $p10fDocRel
    $hasP10fDoc = Test-Path $p10fDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_doc_present"
        target = $p10fDocRel
        passed = $hasP10fDoc
        detail = if ($hasP10fDoc) { "P10F doc found" } else { "P10F doc missing" }
    }
    if (-not $hasP10fDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_doc_missing"
            file = Normalize-RelPath($p10fDocRel)
            function = "P10F documentation"
            detail = "P10F requires $p10fDocRel."
        }
    }
    else {
        $p10fCombinedDocText += "`n" + (Get-Content -LiteralPath $p10fDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p10fScenarioNames = @(
        "p10f_fuzz_reference_gain_behavior",
        "p10f_distortion_gain_ducking_guard",
        "p10f_distortion_highgain_ducking_guard",
        "p10f_boost_highgain_ducking_guard",
        "p10f_highgainamp_internal_ducking_guard",
        "p10f_noise_gate_low_setting_reference",
        "p10f_highgain_noise_floor_after_silence",
        "p10f_highgain_no_perceptible_volume_collapse"
    )

    foreach ($scenarioName in $p10fScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p10f_high_gain_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10f_high_gain_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10F high-gain diagnostics"
                detail = "AudioEngineTests.cpp must contain P10F scenario '$scenarioName'."
            }
        }
    }

    $p10fMetricsMarkers = @(
        "P10FDuckingMetrics",
        "gainDropDuringActiveInput",
        "blockRmsDropRatio",
        "envelopeDuckDepth",
        "recoveryTimeAfterDuck",
        "activeInputToOutputRmsRatio",
        "consecutiveGainReductionBlocks",
        "outputRmsWhileInputActive",
        "gateGainProxy",
        "ceilingTouchedSamples"
    )

    foreach ($marker in $p10fMetricsMarkers) {
        $hasMarker = $audioTestText.Contains($marker)
        $contractChecks += [pscustomobject]@{
            checkId = "p10f_metric_marker_present"
            target = $marker
            passed = $hasMarker
            detail = if ($hasMarker) { "metric marker present" } else { "metric marker missing" }
        }
        if (-not $hasMarker) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10f_metric_marker_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10F metric coverage"
                detail = "P10F diagnostics must cover metric '$marker'."
            }
        }
    }
}

$p10fGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p10f_no_golden_baseline_update"
    target = "git diff"
    passed = ($p10fGoldenBaselineChanged.Count -eq 0)
    detail = if ($p10fGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p10fGoldenBaselineChanged -join ",") }
}
if ($p10fGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10f_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P10F baseline guard"
        detail = "P10F must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    if ($null -eq $p8cConstantsText) {
        $p8cConstantsText = Get-Content -LiteralPath $p8cConstantsPath -Raw
    }
    $p10fSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_no_schema_bump"
        target = $p8cConstantsRel
        passed = $p10fSchemaUnchanged
        detail = if ($p10fSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p10fSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "Nova::Config::STATE_SCHEMA_VERSION"
            detail = "P10F must not bump schema."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $hasP10fKnownFailureBypass = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_no_known_failure_ignore_added"
        target = "scripts/run-base-audio-validation.ps1"
        passed = (-not $hasP10fKnownFailureBypass)
        detail = if ($hasP10fKnownFailureBypass) { "known-failure ignore marker found" } else { "no known-failure ignore marker" }
    }
    if ($hasP10fKnownFailureBypass) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_known_failure_ignore_added"
            file = "scripts/run-base-audio-validation.ps1"
            function = "P10F validation guard"
            detail = "P10F must not add known-failure ignores to base validation."
        }
    }
}

$p10fOutputChainOnlyMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p10f_no_outputchain_only_masking"
    target = "git diff"
    passed = (-not $p10fOutputChainOnlyMasking)
    detail = if ($p10fOutputChainOnlyMasking) { "OutputChain changed during P10F" } else { "OutputChain unchanged" }
}
if ($p10fOutputChainOnlyMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10f_outputchain_only_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P10F masking guard"
        detail = "P10F must not hide high-gain issues with OutputChain-only limiting changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p10fCombinedDocText)) {
    $p10fHasFuzzReference = ($p10fCombinedDocText -match 'Fuzz') -and ($p10fCombinedDocText -match 'reference')
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_fuzz_reference_guard_present"
        target = "P10F docs"
        passed = $p10fHasFuzzReference
        detail = if ($p10fHasFuzzReference) { "Fuzz reference documented" } else { "Fuzz reference marker missing" }
    }
    if (-not $p10fHasFuzzReference) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_fuzz_reference_guard_missing"
            file = "docs/p10f"
            function = "P10F Fuzz reference guard"
            detail = "P10F docs must describe Fuzz as the positive high-gain reference."
        }
    }

    $p10fHasDuckingGuard = ($p10fCombinedDocText -match 'ducking|auto-attenuation|volume collapse') -and
        ($p10fCombinedDocText -match 'gainDropDuringActiveInput|blockRmsDropRatio|envelopeDuckDepth')
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_ducking_guard_present"
        target = "P10F docs"
        passed = $p10fHasDuckingGuard
        detail = if ($p10fHasDuckingGuard) { "ducking guard documented" } else { "ducking guard markers missing" }
    }
    if (-not $p10fHasDuckingGuard) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_ducking_guard_missing"
            file = "docs/p10f"
            function = "P10F ducking guard"
            detail = "P10F docs must describe ducking/auto-attenuation metrics."
        }
    }

    $p10fDocsHaveApprovedMarker = $p10fCombinedDocText -match 'FACTORY_APPROVED|factory approval.*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_no_factory_approved"
        target = "P10F docs"
        passed = (-not $p10fDocsHaveApprovedMarker)
        detail = if ($p10fDocsHaveApprovedMarker) { "factory approval marker found" } else { "no factory approval marker" }
    }
    if ($p10fDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_factory_approved"
            file = "docs/p10f"
            function = "P10F approval guard"
            detail = "P10F must not approve factory presets."
        }
    }

    $p10fNoUiDeployReaperClosure = -not ($p10fCombinedDocText -match 'UI/UX\s*[:=-]\s*PASS|deploy\s*[:=-]\s*PASS|Reaper smoke\s*[:=-]\s*PASS')
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_no_ui_deploy_reaper_closure"
        target = "P10F docs"
        passed = $p10fNoUiDeployReaperClosure
        detail = if ($p10fNoUiDeployReaperClosure) { "no UI/deploy/Reaper closure marker" } else { "UI/deploy/Reaper closure marker found" }
    }
    if (-not $p10fNoUiDeployReaperClosure) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_ui_deploy_reaper_closed"
            file = "docs/p10f"
            function = "P10F scope guard"
            detail = "P10F must not close UI/UX, deploy, or Reaper smoke."
        }
    }

    $p10fDocsKeepPending = ($p10fCombinedDocText -match 'Manual listening QA general remains pending') -and
        ($p10fCombinedDocText -match 'Distortion/high-gain listening QA remains pending') -and
        ($p10fCombinedDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_pending_statuses_preserved"
        target = "P10F docs"
        passed = $p10fDocsKeepPending
        detail = if ($p10fDocsKeepPending) { "manual listening, high-gain listening, and Reaper remain pending" } else { "pending markers missing" }
    }
    if (-not $p10fDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_pending_status_changed"
            file = "docs/p10f"
            function = "P10F status guard"
            detail = "P10F must keep manual listening, Distortion/high-gain listening, and P7F/Reaper pending unless evidence is attached."
        }
    }

    $p10fCleanPreservationNoted = ($p10fCombinedDocText -match 'Clean Impact') -and
        ($p10fCombinedDocText -match 'Clean Studio') -and
        ($p10fCombinedDocText -match 'Wide Ambient Clean')
    $contractChecks += [pscustomobject]@{
        checkId = "p10f_clean_preservation_noted"
        target = "P10F docs"
        passed = $p10fCleanPreservationNoted
        detail = if ($p10fCleanPreservationNoted) { "clean preservation documented" } else { "clean preservation markers missing" }
    }
    if (-not $p10fCleanPreservationNoted) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10f_clean_preservation_missing"
            file = "docs/p10f"
            function = "P10F clean guard"
            detail = "P10F docs must note Clean Studio/Wide Ambient Clean preservation."
        }
    }
}

# ---------------------------------------------------------------------------
# P10G - High-gain noise floor / gate feel / Distortion collapse follow-up
# ---------------------------------------------------------------------------

$p10gInvestigationRel = "docs/p10g-high-gain-noise-gate-root-investigation.md"
$p10gResultsRel = "docs/p10g-high-gain-noise-floor-collapse-results.md"
$p10gDocRels = @($p10gInvestigationRel, $p10gResultsRel)
$p10gCombinedDocText = ""
foreach ($p10gDocRel in $p10gDocRels) {
    $p10gDocPath = Join-Path $repoRoot $p10gDocRel
    $hasP10gDoc = Test-Path $p10gDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_doc_present"
        target = $p10gDocRel
        passed = $hasP10gDoc
        detail = if ($hasP10gDoc) { "P10G doc found" } else { "P10G doc missing" }
    }
    if (-not $hasP10gDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_doc_missing"
            file = Normalize-RelPath($p10gDocRel)
            function = "P10G documentation"
            detail = "P10G requires $p10gDocRel."
        }
    }
    else {
        $p10gCombinedDocText += "`n" + (Get-Content -LiteralPath $p10gDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p10gScenarioNames = @(
        "p10g_fuzz_low_gate_reference",
        "p10g_distortion_low_gate_noise_floor",
        "p10g_highgain_low_gate_noise_floor",
        "p10g_boost_highgain_ground_noise_guard",
        "p10g_distortion_highgain_ground_noise_guard",
        "p10g_distortion_active_volume_collapse_guard",
        "p10g_noise_gate_sustain_preservation_guard",
        "p10g_highgain_fizz_proxy_guard",
        "p10g_highgain_baseline_preservation"
    )

    foreach ($scenarioName in $p10gScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p10g_high_gain_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10g_high_gain_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10G high-gain diagnostics"
                detail = "AudioEngineTests.cpp must contain P10G scenario '$scenarioName'."
            }
        }
    }

    $p10gMetricMarkers = @(
        "P10GNoiseFloorMetrics",
        "idleNoiseRms",
        "postPhraseNoiseRms",
        "noiseToSignalRatio",
        "outputRmsWhileInputActive",
        "outputRmsDuringSilence",
        "gateReductionNeededForSilence",
        "sustainRmsAfterGate",
        "gateTransitions",
        "gateDeltaPeak",
        "volumeCollapseDuringActiveInput",
        "highFrequencyEnergyProxy",
        "rumbleProxy"
    )

    foreach ($marker in $p10gMetricMarkers) {
        $hasMarker = $audioTestText.Contains($marker)
        $contractChecks += [pscustomobject]@{
            checkId = "p10g_metric_marker_present"
            target = $marker
            passed = $hasMarker
            detail = if ($hasMarker) { "metric marker present" } else { "metric marker missing" }
        }
        if (-not $hasMarker) {
            $contractFailures += [pscustomobject]@{
                checkId = "p10g_metric_marker_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P10G metric coverage"
                detail = "P10G diagnostics must cover metric '$marker'."
            }
        }
    }
}

$p10gGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p10g_no_golden_baseline_update"
    target = "git diff --name-only"
    passed = ($p10gGoldenBaselineChanged.Count -eq 0)
    detail = if ($p10gGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p10gGoldenBaselineChanged -join ",") }
}
if ($p10gGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10g_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P10G baseline guard"
        detail = "P10G must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    $p10gSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_no_schema_bump"
        target = "STATE_SCHEMA_VERSION"
        passed = $p10gSchemaUnchanged
        detail = if ($p10gSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p10gSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "P10G schema guard"
            detail = "P10G must not bump schema."
        }
    }
}

$p10gKnownFailureAdded = $false
if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $p10gKnownFailureAdded = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $p10gKnownFailureAdded)
        detail = if ($p10gKnownFailureAdded) { "known-failure marker found" } else { "no known-failure marker in base validation" }
    }
    if ($p10gKnownFailureAdded) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_known_failure_ignore_added"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "P10G validation guard"
            detail = "P10G must not add known-failure ignores to base validation."
        }
    }
}

$p10gOutputChainMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p10g_no_outputchain_masking"
    target = "OutputChain"
    passed = (-not $p10gOutputChainMasking)
    detail = if ($p10gOutputChainMasking) { "OutputChain changed during P10G" } else { "OutputChain unchanged" }
}
if ($p10gOutputChainMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p10g_outputchain_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P10G masking guard"
        detail = "P10G must not hide high-gain issues with OutputChain limiting changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p10gCombinedDocText)) {
    $p10gDocsHaveFuzzReference = ($p10gCombinedDocText -match 'Fuzz') -and ($p10gCombinedDocText -match 'reference')
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_fuzz_low_gate_reference_present"
        target = "P10G docs"
        passed = $p10gDocsHaveFuzzReference
        detail = if ($p10gDocsHaveFuzzReference) { "Fuzz low-gate reference documented" } else { "Fuzz reference missing" }
    }
    if (-not $p10gDocsHaveFuzzReference) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_fuzz_low_gate_reference_missing"
            file = "docs/p10g"
            function = "P10G Fuzz reference guard"
            detail = "P10G docs must preserve Fuzz as the positive low-gate reference."
        }
    }

    $p10gHasVolumeCollapseGuard = ($p10gCombinedDocText -match 'volume-collapse|volume collapse') -and
        ($p10gCombinedDocText -match 'p10g_distortion_active_volume_collapse_guard')
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_active_volume_collapse_guard_present"
        target = "P10G docs"
        passed = $p10gHasVolumeCollapseGuard
        detail = if ($p10gHasVolumeCollapseGuard) { "active volume collapse guard documented" } else { "active collapse guard missing" }
    }
    if (-not $p10gHasVolumeCollapseGuard) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_active_volume_collapse_guard_missing"
            file = "docs/p10g"
            function = "P10G active collapse guard"
            detail = "P10G docs must describe the active volume collapse guard."
        }
    }

    $p10gHasNoiseFloorGuard = ($p10gCombinedDocText -match 'noise floor|idle noise') -and
        ($p10gCombinedDocText -match 'p10g_distortion_low_gate_noise_floor|p10g_highgain_low_gate_noise_floor')
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_noise_floor_guard_present"
        target = "P10G docs"
        passed = $p10gHasNoiseFloorGuard
        detail = if ($p10gHasNoiseFloorGuard) { "noise floor guards documented" } else { "noise floor guard missing" }
    }
    if (-not $p10gHasNoiseFloorGuard) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_noise_floor_guard_missing"
            file = "docs/p10g"
            function = "P10G noise floor guard"
            detail = "P10G docs must describe noise floor guards."
        }
    }

    $p10gDocsHaveApprovedMarker = $p10gCombinedDocText -match 'FACTORY_APPROVED|factory approval.*PASS'
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_no_factory_approved"
        target = "P10G docs"
        passed = (-not $p10gDocsHaveApprovedMarker)
        detail = if ($p10gDocsHaveApprovedMarker) { "factory approval marker found" } else { "no factory approval marker" }
    }
    if ($p10gDocsHaveApprovedMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_factory_approved"
            file = "docs/p10g"
            function = "P10G approval guard"
            detail = "P10G must not approve factory presets."
        }
    }

    $p10gNoUiDeployReaperClosure = -not ($p10gCombinedDocText -match 'UI/UX\s*[:=-]\s*PASS|deploy\s*[:=-]\s*PASS|Reaper smoke\s*[:=-]\s*PASS')
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_no_ui_deploy_reaper_closure"
        target = "P10G docs"
        passed = $p10gNoUiDeployReaperClosure
        detail = if ($p10gNoUiDeployReaperClosure) { "no UI/deploy/Reaper closure marker" } else { "UI/deploy/Reaper closure marker found" }
    }
    if (-not $p10gNoUiDeployReaperClosure) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_ui_deploy_reaper_closed"
            file = "docs/p10g"
            function = "P10G scope guard"
            detail = "P10G must not close UI/UX, deploy, or Reaper smoke."
        }
    }

    $p10gDocsKeepPending = ($p10gCombinedDocText -match 'Manual listening QA general remains pending') -and
        ($p10gCombinedDocText -match 'Distortion/high-gain listening QA remains pending') -and
        ($p10gCombinedDocText -match 'P7F/Reaper remains pending')
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_pending_statuses_preserved"
        target = "P10G docs"
        passed = $p10gDocsKeepPending
        detail = if ($p10gDocsKeepPending) { "manual listening, high-gain listening, and Reaper remain pending" } else { "pending markers missing" }
    }
    if (-not $p10gDocsKeepPending) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_pending_status_changed"
            file = "docs/p10g"
            function = "P10G status guard"
            detail = "P10G must keep manual listening, Distortion/high-gain listening, and P7F/Reaper pending unless evidence is attached."
        }
    }

    $p10gCleanPreservationNoted = ($p10gCombinedDocText -match 'Clean Studio') -and
        ($p10gCombinedDocText -match 'Wide Ambient Clean') -and
        ($p10gCombinedDocText -match 'Clean Amp')
    $contractChecks += [pscustomobject]@{
        checkId = "p10g_clean_preservation_noted"
        target = "P10G docs"
        passed = $p10gCleanPreservationNoted
        detail = if ($p10gCleanPreservationNoted) { "clean preservation documented" } else { "clean preservation markers missing" }
    }
    if (-not $p10gCleanPreservationNoted) {
        $contractFailures += [pscustomobject]@{
            checkId = "p10g_clean_preservation_missing"
            file = "docs/p10g"
            function = "P10G clean guard"
            detail = "P10G docs must note Clean Studio/Wide Ambient Clean/Clean Amp preservation."
        }
    }
}

# ---------------------------------------------------------------------------
# P11A - Amp interface and circuit professionalization
# ---------------------------------------------------------------------------

$p11aAuditRel = "docs/p11a-amp-interface-and-circuit-audit.md"
$p11aResultsRel = "docs/p11a-amp-professionalization-results.md"
$p11aDocRels = @($p11aAuditRel, $p11aResultsRel)
$p11aCombinedDocText = ""
foreach ($p11aDocRel in $p11aDocRels) {
    $p11aDocPath = Join-Path $repoRoot $p11aDocRel
    $hasP11aDoc = Test-Path $p11aDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p11a_doc_present"
        target = $p11aDocRel
        passed = $hasP11aDoc
        detail = if ($hasP11aDoc) { "P11A doc found" } else { "P11A doc missing" }
    }
    if (-not $hasP11aDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11a_doc_missing"
            file = Normalize-RelPath($p11aDocRel)
            function = "P11A documentation"
            detail = "P11A requires $p11aDocRel."
        }
    }
    else {
        $p11aCombinedDocText += "`n" + (Get-Content -LiteralPath $p11aDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p11aScenarioNames = @(
        "p11a_amp_catalog_surface_guard",
        "p11a_amp_state_roundtrip_new_controls",
        "p11a_amp_tonal_stability_guard",
        "p11a_highgain_baseline_preservation",
        "p11a_clean_path_preservation"
    )

    foreach ($scenarioName in $p11aScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p11a_amp_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p11a_amp_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P11A amp professionalization"
                detail = "AudioEngineTests.cpp must contain P11A scenario '$scenarioName'."
            }
        }
    }
}

$p11aAmpParameterChecks = @(
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/CleanAmp.h"; marker = "cleanHeadroom" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/ClassicAmp.h"; marker = "ampSag" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/ClassicAmp.h"; marker = "ampBright" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/HighGainAmp.h"; marker = "hgResonance" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/HighGainAmp.h"; marker = "hgFeel" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/ChimeAmp.h"; marker = "chimeSag" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/BoutiqueAmp.h"; marker = "boutTouch" }
)
foreach ($check in $p11aAmpParameterChecks) {
    $path = Join-Path $repoRoot $check.file
    $text = if (Test-Path $path) { Get-Content -LiteralPath $path -Raw } else { "" }
    $hasMarker = $text.Contains($check.marker)
    $contractChecks += [pscustomobject]@{
        checkId = "p11a_amp_parameter_present"
        target = "$($check.file):$($check.marker)"
        passed = $hasMarker
        detail = if ($hasMarker) { "parameter marker found" } else { "parameter marker missing" }
    }
    if (-not $hasMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11a_amp_parameter_missing"
            file = Normalize-RelPath($check.file)
            function = "P11A amp parameter surface"
            detail = "P11A expected amp parameter marker '$($check.marker)'."
        }
    }
}

$p11aGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p11a_no_golden_baseline_update"
    target = "git diff --name-only"
    passed = ($p11aGoldenBaselineChanged.Count -eq 0)
    detail = if ($p11aGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p11aGoldenBaselineChanged -join ",") }
}
if ($p11aGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p11a_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P11A baseline guard"
        detail = "P11A must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    $p11aSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p11a_no_schema_bump"
        target = "STATE_SCHEMA_VERSION"
        passed = $p11aSchemaUnchanged
        detail = if ($p11aSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p11aSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11a_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "P11A schema guard"
            detail = "P11A must not bump schema."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $p11aKnownFailureAdded = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p11a_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $p11aKnownFailureAdded)
        detail = if ($p11aKnownFailureAdded) { "known-failure marker found" } else { "no known-failure marker in base validation" }
    }
    if ($p11aKnownFailureAdded) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11a_known_failure_ignore_added"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "P11A validation guard"
            detail = "P11A must not add known-failure ignores to base validation."
        }
    }
}

$p11aOutputChainMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p11a_no_outputchain_masking"
    target = "OutputChain"
    passed = (-not $p11aOutputChainMasking)
    detail = if ($p11aOutputChainMasking) { "OutputChain changed during P11A" } else { "OutputChain unchanged" }
}
if ($p11aOutputChainMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p11a_outputchain_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P11A masking guard"
        detail = "P11A must not hide amp issues with OutputChain changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p11aCombinedDocText)) {
    $p11aDocsAvoidFactoryApproval = -not ($p11aCombinedDocText -match 'FACTORY_APPROVED|factory approval.*PASS')
    $contractChecks += [pscustomobject]@{
        checkId = "p11a_no_factory_approved"
        target = "P11A docs"
        passed = $p11aDocsAvoidFactoryApproval
        detail = if ($p11aDocsAvoidFactoryApproval) { "no factory approval marker" } else { "factory approval marker found" }
    }
    if (-not $p11aDocsAvoidFactoryApproval) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11a_factory_approved"
            file = "docs/p11a"
            function = "P11A approval guard"
            detail = "P11A must not approve factory presets."
        }
    }

    $p11aCabinetsOutOfScope = ($p11aCombinedDocText -match 'No cabinet work|cabinet.*out of scope|Defer.*cabinet')
    $contractChecks += [pscustomobject]@{
        checkId = "p11a_cabinets_out_of_scope_documented"
        target = "P11A docs"
        passed = $p11aCabinetsOutOfScope
        detail = if ($p11aCabinetsOutOfScope) { "cabinet deferral documented" } else { "cabinet deferral missing" }
    }
    if (-not $p11aCabinetsOutOfScope) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11a_cabinet_scope_missing"
            file = "docs/p11a"
            function = "P11A scope guard"
            detail = "P11A docs must state cabinets remain out of scope."
        }
    }
}

# ---------------------------------------------------------------------------
# P11B - Cabinet interface and voicing professionalization
# ---------------------------------------------------------------------------

$p11bAuditRel = "docs/p11b-cabinet-interface-and-voicing-audit.md"
$p11bResultsRel = "docs/p11b-cabinet-professionalization-results.md"
$p11bDocRels = @($p11bAuditRel, $p11bResultsRel)
$p11bCombinedDocText = ""
foreach ($p11bDocRel in $p11bDocRels) {
    $p11bDocPath = Join-Path $repoRoot $p11bDocRel
    $hasP11bDoc = Test-Path $p11bDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p11b_doc_present"
        target = $p11bDocRel
        passed = $hasP11bDoc
        detail = if ($hasP11bDoc) { "P11B doc found" } else { "P11B doc missing" }
    }
    if (-not $hasP11bDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11b_doc_missing"
            file = Normalize-RelPath($p11bDocRel)
            function = "P11B documentation"
            detail = "P11B requires $p11bDocRel."
        }
    }
    else {
        $p11bCombinedDocText += "`n" + (Get-Content -LiteralPath $p11bDocPath -Raw)
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p11bScenarioNames = @(
        "p11b_cabinet_catalog_surface_guard",
        "p11b_cabinet_state_roundtrip_new_controls",
        "p11b_cabinet_voicing_stability_guard",
        "p11b_modern4x12_highgain_baseline_preservation",
        "p11b_clean_cabinet_path_preservation"
    )

    foreach ($scenarioName in $p11bScenarioNames) {
        $hasScenario = $audioTestText.Contains($scenarioName)
        $contractChecks += [pscustomobject]@{
            checkId = "p11b_cabinet_scenario_present"
            target = $scenarioName
            passed = $hasScenario
            detail = if ($hasScenario) { "scenario found in AudioEngineTests.cpp" } else { "scenario missing" }
        }
        if (-not $hasScenario) {
            $contractFailures += [pscustomobject]@{
                checkId = "p11b_cabinet_scenario_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P11B cabinet professionalization"
                detail = "AudioEngineTests.cpp must contain P11B scenario '$scenarioName'."
            }
        }
    }
}

$p11bCabinetParameterChecks = @(
    [pscustomobject]@{ file = "Source/Effects/Cabinets/CabinetPedal.h"; marker = "cabResonance" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/CabinetPedal.h"; marker = "cabLowCut" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/CabinetPedal.h"; marker = "cabHighCut" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Vintage2x12Cabinet.h"; marker = "v2x12Resonance" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Vintage2x12Cabinet.h"; marker = "v2x12LowCut" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Vintage2x12Cabinet.h"; marker = "v2x12HighCut" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Modern4x12Cabinet.h"; marker = "m4x12Resonance" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Modern4x12Cabinet.h"; marker = "m4x12LowCut" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Modern4x12Cabinet.h"; marker = "m4x12HighCut" }
)
foreach ($check in $p11bCabinetParameterChecks) {
    $path = Join-Path $repoRoot $check.file
    $text = if (Test-Path $path) { Get-Content -LiteralPath $path -Raw } else { "" }
    $hasMarker = $text.Contains($check.marker)
    $contractChecks += [pscustomobject]@{
        checkId = "p11b_cabinet_parameter_present"
        target = "$($check.file):$($check.marker)"
        passed = $hasMarker
        detail = if ($hasMarker) { "parameter marker found" } else { "parameter marker missing" }
    }
    if (-not $hasMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11b_cabinet_parameter_missing"
            file = Normalize-RelPath($check.file)
            function = "P11B cabinet parameter surface"
            detail = "P11B expected cabinet parameter marker '$($check.marker)'."
        }
    }
}

$p11bGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p11b_no_golden_baseline_update"
    target = "git diff --name-only"
    passed = ($p11bGoldenBaselineChanged.Count -eq 0)
    detail = if ($p11bGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p11bGoldenBaselineChanged -join ",") }
}
if ($p11bGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p11b_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P11B baseline guard"
        detail = "P11B must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    $p11bSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p11b_no_schema_bump"
        target = "STATE_SCHEMA_VERSION"
        passed = $p11bSchemaUnchanged
        detail = if ($p11bSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p11bSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11b_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "P11B schema guard"
            detail = "P11B must not bump schema."
        }
    }
}

if (Test-Path $baseValidationScriptPath) {
    if ($null -eq $baseValidationScriptText) {
        $baseValidationScriptText = Get-Content -LiteralPath $baseValidationScriptPath -Raw
    }
    $p11bKnownFailureAdded = $baseValidationScriptText -match 'knownFailure|knownFailures|ignoredFailures'
    $contractChecks += [pscustomobject]@{
        checkId = "p11b_no_known_failure_ignore_added"
        target = $baseValidationScriptRel
        passed = (-not $p11bKnownFailureAdded)
        detail = if ($p11bKnownFailureAdded) { "known-failure marker found" } else { "no known-failure marker in base validation" }
    }
    if ($p11bKnownFailureAdded) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11b_known_failure_ignore_added"
            file = Normalize-RelPath($baseValidationScriptRel)
            function = "P11B validation guard"
            detail = "P11B must not add known-failure ignores to base validation."
        }
    }
}

$p11bOutputChainMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p11b_no_outputchain_masking"
    target = "OutputChain"
    passed = (-not $p11bOutputChainMasking)
    detail = if ($p11bOutputChainMasking) { "OutputChain changed during P11B" } else { "OutputChain unchanged" }
}
if ($p11bOutputChainMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p11b_outputchain_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P11B masking guard"
        detail = "P11B must not hide cabinet issues with OutputChain changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p11bCombinedDocText)) {
    $p11bDocsAvoidFactoryApproval = -not ($p11bCombinedDocText -match 'FACTORY_APPROVED|factory approval.*PASS')
    $contractChecks += [pscustomobject]@{
        checkId = "p11b_no_factory_approved"
        target = "P11B docs"
        passed = $p11bDocsAvoidFactoryApproval
        detail = if ($p11bDocsAvoidFactoryApproval) { "no factory approval marker" } else { "factory approval marker found" }
    }
    if (-not $p11bDocsAvoidFactoryApproval) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11b_factory_approved"
            file = "docs/p11b"
            function = "P11B approval guard"
            detail = "P11B must not approve factory presets."
        }
    }

    $p11bManualListeningSeparate = ($p11bCombinedDocText -match 'Manual listening is required|manual listening')
    $contractChecks += [pscustomobject]@{
        checkId = "p11b_manual_listening_separate"
        target = "P11B docs"
        passed = $p11bManualListeningSeparate
        detail = if ($p11bManualListeningSeparate) { "manual listening remains separate" } else { "manual listening note missing" }
    }
    if (-not $p11bManualListeningSeparate) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11b_manual_listening_missing"
            file = "docs/p11b"
            function = "P11B manual QA guard"
            detail = "P11B docs must keep manual listening separate from automated validation."
        }
    }
}

# ---------------------------------------------------------------------------
# P11C - Amp/Cab open editor visual differentiation
# ---------------------------------------------------------------------------

$p11cAuditRel = "docs/p11c-amp-cab-open-ui-redesign-audit.md"
$p11cResultsRel = "docs/p11c-amp-cab-open-ui-redesign-results.md"
$p11cDocRels = @($p11cAuditRel, $p11cResultsRel)
$p11cCombinedDocText = ""
foreach ($p11cDocRel in $p11cDocRels) {
    $p11cDocPath = Join-Path $repoRoot $p11cDocRel
    $hasP11cDoc = Test-Path $p11cDocPath
    $contractChecks += [pscustomobject]@{
        checkId = "p11c_doc_present"
        target = $p11cDocRel
        passed = $hasP11cDoc
        detail = if ($hasP11cDoc) { "P11C doc found" } else { "P11C doc missing" }
    }
    if (-not $hasP11cDoc) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11c_doc_missing"
            file = Normalize-RelPath($p11cDocRel)
            function = "P11C documentation"
            detail = "P11C requires $p11cDocRel."
        }
    }
    else {
        $p11cCombinedDocText += "`n" + (Get-Content -LiteralPath $p11cDocPath -Raw)
    }
}

$p11cUiRel = "Source/Effects/Pedals/Base/PremiumPedalUI.h"
$p11cUiPath = Join-Path $repoRoot $p11cUiRel
$p11cUiText = if (Test-Path $p11cUiPath) { Get-Content -LiteralPath $p11cUiPath -Raw } else { "" }
$p11cUiChecks = @(
    [pscustomobject]@{ marker = "class PremiumHardwareEditor"; detail = "shared hardware editor class" },
    [pscustomobject]@{ marker = "enum class Skin"; detail = "category skin selector" },
    [pscustomobject]@{ marker = "Amplifier"; detail = "amplifier skin" },
    [pscustomobject]@{ marker = "Cabinet"; detail = "cabinet skin" },
    [pscustomobject]@{ marker = "paintAmplifier"; detail = "amp-specific paint path" },
    [pscustomobject]@{ marker = "paintCabinet"; detail = "cabinet-specific paint path" }
)
foreach ($check in $p11cUiChecks) {
    $hasMarker = $p11cUiText.Contains($check.marker)
    $contractChecks += [pscustomobject]@{
        checkId = "p11c_hardware_editor_marker_present"
        target = "${p11cUiRel}:$($check.marker)"
        passed = $hasMarker
        detail = if ($hasMarker) { $check.detail } else { "missing $($check.detail)" }
    }
    if (-not $hasMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11c_hardware_editor_marker_missing"
            file = Normalize-RelPath($p11cUiRel)
            function = "P11C open editor UI"
            detail = "P11C expected UI marker '$($check.marker)'."
        }
    }
}

$p11cEditorAdoptionChecks = @(
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/CleanAmp.h"; marker = "PremiumHardwareEditor::Skin::Amplifier" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/ClassicAmp.h"; marker = "PremiumHardwareEditor::Skin::Amplifier" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/HighGainAmp.h"; marker = "PremiumHardwareEditor::Skin::Amplifier" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/ChimeAmp.h"; marker = "PremiumHardwareEditor::Skin::Amplifier" },
    [pscustomobject]@{ file = "Source/Effects/Amplifiers/BoutiqueAmp.h"; marker = "PremiumHardwareEditor::Skin::Amplifier" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/CabinetPedal.h"; marker = "PremiumHardwareEditor::Skin::Cabinet" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Vintage2x12Cabinet.h"; marker = "PremiumHardwareEditor::Skin::Cabinet" },
    [pscustomobject]@{ file = "Source/Effects/Cabinets/Modern4x12Cabinet.h"; marker = "PremiumHardwareEditor::Skin::Cabinet" }
)
foreach ($check in $p11cEditorAdoptionChecks) {
    $path = Join-Path $repoRoot $check.file
    $text = if (Test-Path $path) { Get-Content -LiteralPath $path -Raw } else { "" }
    $hasMarker = $text.Contains($check.marker)
    $contractChecks += [pscustomobject]@{
        checkId = "p11c_open_editor_skin_adopted"
        target = "$($check.file):$($check.marker)"
        passed = $hasMarker
        detail = if ($hasMarker) { "P11C editor skin adopted" } else { "P11C editor skin missing" }
    }
    if (-not $hasMarker) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11c_open_editor_skin_missing"
            file = Normalize-RelPath($check.file)
            function = "P11C editor adoption"
            detail = "Expected '$($check.marker)' in open editor creation."
        }
    }
}

$p11cGoldenBaselineChanged = @($gitChangedFiles | Where-Object { $_ -match '^docs/golden-metrics/' -or $_ -match 'golden.*baseline' })
$contractChecks += [pscustomobject]@{
    checkId = "p11c_no_golden_baseline_update"
    target = "git diff --name-only"
    passed = ($p11cGoldenBaselineChanged.Count -eq 0)
    detail = if ($p11cGoldenBaselineChanged.Count -eq 0) { "no golden baseline files changed" } else { "changed=" + ($p11cGoldenBaselineChanged -join ",") }
}
if ($p11cGoldenBaselineChanged.Count -gt 0) {
    $contractFailures += [pscustomobject]@{
        checkId = "p11c_golden_baseline_updated"
        file = "docs/golden-metrics"
        function = "P11C baseline guard"
        detail = "P11C must not update golden baseline files."
    }
}

if (Test-Path $p8cConstantsPath) {
    $p11cSchemaUnchanged = $p8cConstantsText -match "STATE_SCHEMA_VERSION\s*=\s*1\b"
    $contractChecks += [pscustomobject]@{
        checkId = "p11c_no_schema_bump"
        target = "STATE_SCHEMA_VERSION"
        passed = $p11cSchemaUnchanged
        detail = if ($p11cSchemaUnchanged) { "STATE_SCHEMA_VERSION remains 1" } else { "STATE_SCHEMA_VERSION is no longer 1" }
    }
    if (-not $p11cSchemaUnchanged) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11c_schema_bumped"
            file = Normalize-RelPath($p8cConstantsRel)
            function = "P11C schema guard"
            detail = "P11C must not bump schema."
        }
    }
}

$p11cOutputChainMasking = @($gitChangedFiles | Where-Object { $_ -match '^Source/Core/DSP/Global/OutputChain\.(h|cpp)$' }).Count -gt 0
$contractChecks += [pscustomobject]@{
    checkId = "p11c_no_outputchain_masking"
    target = "OutputChain"
    passed = (-not $p11cOutputChainMasking)
    detail = if ($p11cOutputChainMasking) { "OutputChain changed during P11C" } else { "OutputChain unchanged" }
}
if ($p11cOutputChainMasking) {
    $contractFailures += [pscustomobject]@{
        checkId = "p11c_outputchain_masking"
        file = "Source/Core/DSP/Global/OutputChain"
        function = "P11C masking guard"
        detail = "P11C must not hide UI issues with OutputChain changes."
    }
}

if (-not [string]::IsNullOrWhiteSpace($p11cCombinedDocText)) {
    $p11cDocsStateNoDsp = ($p11cCombinedDocText -match 'No DSP changes|does not change DSP')
    $contractChecks += [pscustomobject]@{
        checkId = "p11c_no_dsp_change_documented"
        target = "P11C docs"
        passed = $p11cDocsStateNoDsp
        detail = if ($p11cDocsStateNoDsp) { "no-DSP scope documented" } else { "no-DSP scope missing" }
    }
    if (-not $p11cDocsStateNoDsp) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11c_no_dsp_scope_missing"
            file = "docs/p11c"
            function = "P11C scope guard"
            detail = "P11C docs must state that DSP was not changed."
        }
    }

    $p11cManualQaSeparate = ($p11cCombinedDocText -match 'manual visual QA|Manual Visual QA|Manual listening')
    $contractChecks += [pscustomobject]@{
        checkId = "p11c_manual_qa_separate"
        target = "P11C docs"
        passed = $p11cManualQaSeparate
        detail = if ($p11cManualQaSeparate) { "manual QA remains separate" } else { "manual QA note missing" }
    }
    if (-not $p11cManualQaSeparate) {
        $contractFailures += [pscustomobject]@{
            checkId = "p11c_manual_qa_missing"
            file = "docs/p11c"
            function = "P11C manual QA guard"
            detail = "P11C docs must keep manual QA separate from automated validation."
        }
    }
}

# ---------------------------------------------------------------------------
# P7I - Diagnostics / Visualizer / CPU Profiler polish
# ---------------------------------------------------------------------------

$p7iDocRel = "docs/p7i-diagnostics-visualizer-cpu-profiler-polish-results.md"
$p7iDocPath = Join-Path $repoRoot $p7iDocRel
$hasP7iDoc = Test-Path $p7iDocPath
$contractChecks += [pscustomobject]@{
    checkId = "p7i_diagnostics_polish_doc_present"
    target = $p7iDocRel
    passed = $hasP7iDoc
    detail = if ($hasP7iDoc) { "P7I diagnostics polish doc found" } else { "P7I diagnostics polish doc missing" }
}
if (-not $hasP7iDoc) {
    $contractFailures += [pscustomobject]@{
        checkId = "p7i_diagnostics_polish_doc_missing"
        file = Normalize-RelPath($p7iDocRel)
        function = "P7I documentation"
        detail = "P7I closure requires docs/p7i-diagnostics-visualizer-cpu-profiler-polish-results.md."
    }
}

$p7iBundleRel = "scripts/run-diagnostics-bundle.ps1"
$p7iBundlePath = Join-Path $repoRoot $p7iBundleRel
$hasP7iBundle = Test-Path $p7iBundlePath
$contractChecks += [pscustomobject]@{
    checkId = "p7i_diagnostics_bundle_script_present"
    target = $p7iBundleRel
    passed = $hasP7iBundle
    detail = if ($hasP7iBundle) { "P7I diagnostics bundle script found" } else { "P7I diagnostics bundle script missing" }
}
if (-not $hasP7iBundle) {
    $contractFailures += [pscustomobject]@{
        checkId = "p7i_diagnostics_bundle_script_missing"
        file = Normalize-RelPath($p7iBundleRel)
        function = "P7I tooling"
        detail = "P7I tooling polish requires scripts/run-diagnostics-bundle.ps1 aggregator."
    }
}

$p7iDiagnosticsManagerRel = "Source/Core/Audio/DiagnosticsManager.h"
$p7iDiagnosticsManagerPath = Join-Path $repoRoot $p7iDiagnosticsManagerRel
if (Test-Path $p7iDiagnosticsManagerPath) {
    $p7iDiagnosticsManagerText = Get-Content -LiteralPath $p7iDiagnosticsManagerPath -Raw
    $hasFormatLine = $p7iDiagnosticsManagerText.Contains("formatProfilingLine")
    $hasFormatResults = $p7iDiagnosticsManagerText.Contains("formatProfilingResults")
    $hasBuildReport = $p7iDiagnosticsManagerText.Contains("buildDiagnosticReport")
    $contractChecks += [pscustomobject]@{
        checkId = "p7i_diagnostics_manager_format_profiling_line_present"
        target = $p7iDiagnosticsManagerRel
        passed = $hasFormatLine
        detail = if ($hasFormatLine) { "formatProfilingLine present" } else { "formatProfilingLine missing" }
    }
    if (-not $hasFormatLine) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7i_diagnostics_manager_format_profiling_line_missing"
            file = Normalize-RelPath($p7iDiagnosticsManagerRel)
            function = "DiagnosticsManager::formatProfilingLine"
            detail = "DiagnosticsManager must expose formatProfilingLine for the P7I profiler tooling."
        }
    }
    $contractChecks += [pscustomobject]@{
        checkId = "p7i_diagnostics_manager_format_profiling_results_present"
        target = $p7iDiagnosticsManagerRel
        passed = $hasFormatResults
        detail = if ($hasFormatResults) { "formatProfilingResults present" } else { "formatProfilingResults missing" }
    }
    if (-not $hasFormatResults) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7i_diagnostics_manager_format_profiling_results_missing"
            file = Normalize-RelPath($p7iDiagnosticsManagerRel)
            function = "DiagnosticsManager::formatProfilingResults"
            detail = "DiagnosticsManager must expose formatProfilingResults for the P7I profiler tooling."
        }
    }
    $contractChecks += [pscustomobject]@{
        checkId = "p7i_diagnostics_manager_build_report_present"
        target = $p7iDiagnosticsManagerRel
        passed = $hasBuildReport
        detail = if ($hasBuildReport) { "buildDiagnosticReport present" } else { "buildDiagnosticReport missing" }
    }
    if (-not $hasBuildReport) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7i_diagnostics_manager_build_report_missing"
            file = Normalize-RelPath($p7iDiagnosticsManagerRel)
            function = "DiagnosticsManager::buildDiagnosticReport"
            detail = "DiagnosticsManager must expose buildDiagnosticReport for the P7I diagnostics surface."
        }
    }
}
else {
    $contractChecks += [pscustomobject]@{
        checkId = "p7i_diagnostics_manager_present"
        target = $p7iDiagnosticsManagerRel
        passed = $false
        detail = "DiagnosticsManager.h not found"
    }
    $contractFailures += [pscustomobject]@{
        checkId = "p7i_diagnostics_manager_missing"
        file = Normalize-RelPath($p7iDiagnosticsManagerRel)
        function = "DiagnosticsManager"
        detail = "DiagnosticsManager.h is missing; P7I cannot codify its surface."
    }
}

$p7iAudioEngineCppRel = "Source/Core/AudioEngine.cpp"
$p7iAudioEngineCppPath = Join-Path $repoRoot $p7iAudioEngineCppRel
if (Test-Path $p7iAudioEngineCppPath) {
    $p7iAudioEngineText = Get-Content -LiteralPath $p7iAudioEngineCppPath -Raw
    $blockSizeArrayMatch = [regex]::Match($p7iAudioEngineText, 'std::array<\s*int\s*,\s*\d+\s*>\s*blockSizes\s*\{\s*\{\s*([0-9,\s]+)\s*\}\s*\}\s*;')
    $blockSizeTokens = if ($blockSizeArrayMatch.Success) { $blockSizeArrayMatch.Groups[1].Value } else { "" }
    $coversBlock32 = $blockSizeTokens -match '(^|[\s,])32([\s,]|$)'
    $coversBlock64 = $blockSizeTokens -match '(^|[\s,])64([\s,]|$)'

    $contractChecks += [pscustomobject]@{
        checkId = "p7i_runtime_profiling_covers_block_32"
        target = "AudioEngine::runRealtimeProfilingSuite"
        passed = $coversBlock32
        detail = if ($coversBlock32) { "block 32 present in profiling suite" } else { "block 32 missing in profiling suite" }
    }
    if (-not $coversBlock32) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7i_runtime_profiling_block_32_missing"
            file = Normalize-RelPath($p7iAudioEngineCppRel)
            function = "AudioEngine::runRealtimeProfilingSuite"
            detail = "P7I requires the realtime profiling suite to cover block size 32."
        }
    }
    $contractChecks += [pscustomobject]@{
        checkId = "p7i_runtime_profiling_covers_block_64"
        target = "AudioEngine::runRealtimeProfilingSuite"
        passed = $coversBlock64
        detail = if ($coversBlock64) { "block 64 present in profiling suite" } else { "block 64 missing in profiling suite" }
    }
    if (-not $coversBlock64) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7i_runtime_profiling_block_64_missing"
            file = Normalize-RelPath($p7iAudioEngineCppRel)
            function = "AudioEngine::runRealtimeProfilingSuite"
            detail = "P7I requires the realtime profiling suite to cover block size 64."
        }
    }
}

if (Test-Path $audioTestPath) {
    if ($null -eq $audioTestText) {
        $audioTestText = Get-Content -LiteralPath $audioTestPath -Raw
    }

    $p7iTestNames = @(
        "P7I CpuMeter reset clears all counters",
        "P7I CpuMeter ignores invalid sample rate / block size without polluting state",
        "P7I CpuMeter peak decays toward floor across many empty blocks",
        "P7I DiagnosticsManager::formatProfilingLine produces deterministic shape",
        "P7I DiagnosticsManager::formatProfilingLine appends notes when present",
        "P7I DiagnosticsManager::formatProfilingResults joins lines and includes header",
        "P7I runRealtimeProfilingSuite covers blocks 32 and 64 explicitly",
        "P7I AudioEngine::buildDiagnosticReport is non-empty and contains stable fields"
    )

    foreach ($testName in $p7iTestNames) {
        $hasTest = $audioTestText.Contains($testName)
        $contractChecks += [pscustomobject]@{
            checkId = "p7i_diagnostics_test_present"
            target = $testName
            passed = $hasTest
            detail = if ($hasTest) { "test name found in AudioEngineTests.cpp" } else { "test name missing" }
        }
        if (-not $hasTest) {
            $contractFailures += [pscustomobject]@{
                checkId = "p7i_diagnostics_test_missing"
                file = Normalize-RelPath($audioTestRel)
                function = "P7IDiagnosticsProfilerTests"
                detail = "AudioEngineTests.cpp must contain the P7I diagnostics/profiler test '$testName'."
            }
        }
    }
}

$legacyFiles = @(
    [pscustomobject]@{
        path = "Source/Effects/Pedals/ChorusPedal.h"
        classification = "DUPLICATE_SUPERSEDED"
        replacement = "Source/Effects/Pedals/Chorus/ChorusPedal.h"
        expectedInJucer = $false
        alias = ""
        canonical = "Chorus"
        rationale = "Root legacy header is superseded by the registered Chorus/ChorusPedal.h processor."
    },
    [pscustomobject]@{
        path = "Source/Effects/Pedals/CompressorPedal.h"
        classification = "DUPLICATE_SUPERSEDED"
        replacement = "Source/Effects/Pedals/Compressor/CompressorPedal.h"
        expectedInJucer = $false
        alias = ""
        canonical = "Compressor"
        rationale = "Root legacy header is superseded by the registered Compressor/CompressorPedal.h processor."
    },
    [pscustomobject]@{
        path = "Source/Effects/Pedals/Wah/AutoWahPedal.h"
        classification = "LEGACY_ALIAS_ONLY"
        replacement = "Source/Effects/Pedals/Wah/ClassicWahPedal.h"
        expectedInJucer = $true
        alias = "Auto Wah"
        canonical = "Wah"
        rationale = "Legacy Auto Wah names canonicalize to the registered Wah processor; AutoWahPedal is not registered."
    },
    [pscustomobject]@{
        path = "Source/Effects/Pedals/Metal/MetalDistortionPedal.h"
        classification = "LEGACY_ALIAS_ONLY"
        replacement = "Source/Effects/Pedals/Distortion/DistortionPedal.h"
        expectedInJucer = $true
        alias = "Metal Distortion"
        canonical = "Distortion"
        rationale = "Legacy Metal Distortion names canonicalize to the registered Distortion processor; MetalDistortionPedal is not registered."
    }
)

$legacyWarnings = @()
$legacyQuarantine = @()
$catalogFile = "Source/Core/PedalCatalog.h"
$catalogText = if (Test-Path $catalogFile) { Get-Content -LiteralPath $catalogFile -Raw } else { "" }
foreach ($legacySpec in $legacyFiles) {
    $legacyRel = $legacySpec.path
    $legacyFull = Join-Path $repoRoot $legacyRel
    $replacementFull = Join-Path $repoRoot $legacySpec.replacement
    if (-not (Test-Path $legacyFull)) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7g_legacy_quarantine_missing"
            file = $legacyRel
            function = "P7G legacy quarantine"
            detail = "Listed P7G legacy file is missing; update the quarantine classification before removing it."
        }
        continue
    }

    $inJucer = Select-String -Path "NOVA.jucer" -SimpleMatch $legacyRel -Quiet
    $registrySearch = "../" + $legacyRel.Substring("Source/".Length)
    $inRegistry = Select-String -Path $registryFile -SimpleMatch $registrySearch -Quiet
    $replacementExists = Test-Path $replacementFull
    $catalogOk = $true
    if (-not [string]::IsNullOrWhiteSpace($legacySpec.alias)) {
        $catalogOk = $catalogText.Contains($legacySpec.alias) -and $catalogText.Contains(('return "{0}"' -f $legacySpec.canonical))
    }
    $quarantinePassed = $replacementExists -and (-not $inRegistry) -and ($inJucer -eq $legacySpec.expectedInJucer) -and $catalogOk
    $contractChecks += [pscustomobject]@{
        checkId = "p7g_legacy_file_quarantined"
        target = Normalize-RelPath($legacyRel)
        passed = $quarantinePassed
        detail = "classification=$($legacySpec.classification); inJucer=$inJucer expectedInJucer=$($legacySpec.expectedInJucer); inRegistry=$inRegistry; replacement=$($legacySpec.replacement); catalogOk=$catalogOk"
    }
    if (-not $quarantinePassed) {
        $contractFailures += [pscustomobject]@{
            checkId = "p7g_legacy_quarantine_invalid"
            file = Normalize-RelPath($legacyRel)
            function = "P7G legacy quarantine"
            detail = "Legacy quarantine assumptions changed. $($legacySpec.rationale)"
        }
    }

    $ranges = Get-ProcessRanges -FilePath $legacyFull -Root $repoRoot
    foreach ($range in $ranges) {
        for ($lineNo = $range.startLine; $lineNo -le $range.endLine; $lineNo++) {
            $text = $range.lines[$lineNo - 1]
            foreach ($pattern in $dangerPatterns) {
                if ($text -notmatch $pattern.regex) {
                    continue
                }

                $legacyQuarantine += [pscustomobject]@{
                    file = Normalize-RelPath($legacyRel)
                    classification = $legacySpec.classification
                    inJucer = $inJucer
                    inRegistry = $inRegistry
                    replacement = Normalize-RelPath($legacySpec.replacement)
                    line = $lineNo
                    patternId = $pattern.id
                    patternLabel = $pattern.label
                    lineText = $text.Trim()
                    reason = $legacySpec.rationale
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
$lines.Add("summary.legacyQuarantined=$($legacyQuarantine.Count)")
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

if ($legacyQuarantine.Count -gt 0) {
    $lines.Add("P7G quarantined legacy findings (not WARN; not active audio routes):")
    foreach ($finding in $legacyQuarantine | Sort-Object file, line, patternId) {
        $lines.Add("  - [$($finding.patternLabel)] $($finding.file):$($finding.line)")
        $lines.Add("    classification=$($finding.classification) inJucer=$($finding.inJucer) inRegistry=$($finding.inRegistry) replacement=$($finding.replacement)")
        $lines.Add("    line=$($finding.lineText)")
        $lines.Add("    quarantineReason=$($finding.reason)")
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
        legacyQuarantined = $legacyQuarantine.Count
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
    legacyQuarantine = $legacyQuarantine
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
