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

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]

        $isProcessBlockDecl = $line -match '^\s*void\s+(?:[A-Za-z0-9_:<>]+\s*::\s*)?processBlock\s*\([^;]*\)\s*(?:override)?'
        $isAudioEngineProcessDecl = $line -match '^\s*void\s+AudioEngine::process\s*\('
        $isAudioEngineDryWetDecl = $line -match '^\s*void\s+AudioEngine::processWithSampleAccurateDryWet\s*\('

        if (-not ($isProcessBlockDecl -or $isAudioEngineProcessDecl -or $isAudioEngineDryWetDecl)) {
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
        else {
            'processBlock'
        }

        $ranges += [pscustomobject]@{
            file = Normalize-RelPath((Resolve-Path -LiteralPath $FilePath -Relative).TrimStart('.\'))
            function = $functionName
            startLine = $start + 1
            endLine = $end + 1
            lines = $lines
        }

        $i = $end
    }

    return $ranges
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

$status = if ($activeFailures.Count -gt 0) { "FAIL" } elseif ($summaryWarnings.Count -gt 0) { "WARN" } else { "PASS" }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("NOVA Audio Thread Policy Scan")
$lines.Add("Generated: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))")
$lines.Add("status=$status")
$lines.Add("summary.activeFiles=$($activeFiles.Count)")
$lines.Add("summary.activeRanges=$($activeRanges.Count)")
$lines.Add("summary.failures=$($activeFailures.Count)")
$lines.Add("summary.warnings=$($summaryWarnings.Count)")
$lines.Add("summary.allowlistedWarnings=$($activeAllowlistedWarnings.Count)")
$lines.Add("summary.legacyWarnings=$($legacyWarnings.Count)")
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
        failures = $activeFailures.Count
        warnings = $summaryWarnings.Count
        allowlistedWarnings = $activeAllowlistedWarnings.Count
        legacyWarnings = $legacyWarnings.Count
        allowListEntries = $allowList.Count
    }
    activeFiles = $activeFiles
    allowList = $allowList
    failures = $activeFailures
    allowlistedWarnings = $activeAllowlistedWarnings
    legacyWarnings = $legacyWarnings
}

$jsonPayload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonReportPathAbs

Write-Host "Audio thread policy status: $status"
Write-Host "Report: $reportPathAbs"
Write-Host "JSON:   $jsonReportPathAbs"

if ($activeFailures.Count -gt 0) {
    throw "Audio thread policy scan found blocking failures."
}

if ($FailOnWarn.IsPresent -and $summaryWarnings.Count -gt 0) {
    throw "Audio thread policy scan has warnings and -FailOnWarn is enabled."
}

exit 0
