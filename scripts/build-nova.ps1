param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [ValidateSet("NOVA_StandalonePlugin", "NOVA_VST3", "NOVA_SharedCode", "NOVA_VST3ManifestHelper", "Solution")]
    [string]$Target = "NOVA_StandalonePlugin",

    [switch]$Clean,

    [switch]$EnableReleaseDiagnostics
)

$ErrorActionPreference = "Stop"

function Resolve-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installationPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
        if ($installationPath) {
            $preferredCandidates = @(
                (Join-Path $installationPath "MSBuild\Current\Bin\amd64\MSBuild.exe"),
                (Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe")
            )

            foreach ($candidate in $preferredCandidates) {
                if (Test-Path $candidate) {
                    return $candidate
                }
            }
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "MSBuild.exe was not found."
}

Set-Location (Split-Path -Parent $PSScriptRoot)

$repoRoot = Get-Location
$solution = Join-Path (Get-Location) "Builds/VisualStudio2022/NOVA.sln"
if (-not (Test-Path $solution)) {
    throw "Solution not found at $solution"
}

$projectMap = @{
    "NOVA_StandalonePlugin"   = "Builds/VisualStudio2022/NOVA_StandalonePlugin.vcxproj"
    "NOVA_VST3"               = "Builds/VisualStudio2022/NOVA_VST3.vcxproj"
    "NOVA_SharedCode"         = "Builds/VisualStudio2022/NOVA_SharedCode.vcxproj"
    "NOVA_VST3ManifestHelper" = "Builds/VisualStudio2022/NOVA_VST3ManifestHelper.vcxproj"
}

$msbuild = Resolve-MSBuild
$buildAction = if ($Clean.IsPresent -or $EnableReleaseDiagnostics.IsPresent -or $Configuration -eq "Release") { "Rebuild" } else { "Build" }

function Invoke-MSBuildTarget {
    param(
        [string]$Path,
        [string]$TargetName
    )

    $previousProcessorArchitecture = $env:PROCESSOR_ARCHITECTURE
    $previousProcessorArchitectureW6432 = $env:PROCESSOR_ARCHITEW6432
    $previousCl = $env:CL

    try {
        if ($Platform -eq "x64") {
            $env:PROCESSOR_ARCHITECTURE = "AMD64"
            Remove-Item Env:\PROCESSOR_ARCHITEW6432 -ErrorAction SilentlyContinue
        }

        if ($EnableReleaseDiagnostics.IsPresent) {
            $diagnosticDefine = "/DNOVA_ENABLE_RELEASE_DIAGNOSTICS=1"
            $env:CL = if ([string]::IsNullOrWhiteSpace($previousCl)) {
                $diagnosticDefine
            }
            else {
                "$diagnosticDefine $previousCl"
            }
        }

        & $msbuild $Path "/t:$TargetName" "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/p:PreferredToolArchitecture=x64" "/m"
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        if ($null -eq $previousProcessorArchitecture) {
            Remove-Item Env:\PROCESSOR_ARCHITECTURE -ErrorAction SilentlyContinue
        }
        else {
            $env:PROCESSOR_ARCHITECTURE = $previousProcessorArchitecture
        }

        if ($null -eq $previousProcessorArchitectureW6432) {
            Remove-Item Env:\PROCESSOR_ARCHITEW6432 -ErrorAction SilentlyContinue
        }
        else {
            $env:PROCESSOR_ARCHITEW6432 = $previousProcessorArchitectureW6432
        }

        if ($null -eq $previousCl) {
            Remove-Item Env:\CL -ErrorAction SilentlyContinue
        }
        else {
            $env:CL = $previousCl
        }
    }
}

function Invoke-ProjectBuild {
    param(
        [string]$ProjectKey
    )

    $project = Join-Path (Get-Location) $projectMap[$ProjectKey]
    if (-not (Test-Path $project)) {
        throw "Project file not found at $project"
    }

    Write-Host "Target: $ProjectKey -> $project | Action: $buildAction | Configuration: $Configuration | Platform: $Platform"
    Invoke-MSBuildTarget -Path $project -TargetName $buildAction
}

function Update-VST3ModuleInfo {
    if ($Platform -ne "x64") {
        Write-Host "Skipping VST3 moduleinfo generation for non-x64 platform $Platform."
        return
    }

    $vst3Binary = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/VST3/NOVA.vst3/Contents/x86_64-win/NOVA.vst3"
    $moduleInfo = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/VST3/NOVA.vst3/Contents/Resources/moduleinfo.json"
    $helper = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/VST3 Manifest Helper/juce_vst3_helper.exe"

    if (-not (Test-Path $vst3Binary)) {
        throw "VST3 binary not found at $vst3Binary"
    }
    if (-not (Test-Path $helper)) {
        throw "VST3 manifest helper not found at $helper"
    }

    $moduleInfoDir = Split-Path -Parent $moduleInfo
    if (-not (Test-Path $moduleInfoDir)) {
        New-Item -Path $moduleInfoDir -ItemType Directory | Out-Null
    }

    $jsonLines = & $helper
    if ($LASTEXITCODE -ne 0) {
        throw "VST3 manifest helper failed with exit code $LASTEXITCODE"
    }

    $jsonText = ($jsonLines -join "`n").Trim()
    if ([string]::IsNullOrWhiteSpace($jsonText) -or -not $jsonText.StartsWith("{")) {
        throw "VST3 manifest helper did not produce JSON output."
    }

    # The Steinberg helper emits JSON5-compatible trailing commas; normalize so
    # release evidence and downstream tooling can parse the manifest as JSON.
    $jsonText = [regex]::Replace($jsonText, ",(\s*[\]}])", '$1').Trim()
    try {
        $null = $jsonText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Generated moduleinfo.json is not parseable JSON: $($_.Exception.Message)"
    }

    Set-Content -LiteralPath $moduleInfo -Value $jsonText -Encoding UTF8

    $binaryItem = Get-Item -LiteralPath $vst3Binary
    $moduleInfoItem = Get-Item -LiteralPath $moduleInfo
    if ($moduleInfoItem.LastWriteTimeUtc -lt $binaryItem.LastWriteTimeUtc.AddSeconds(-2)) {
        throw "Generated moduleinfo.json is older than the rebuilt VST3 binary."
    }

    Write-Host "Regenerated VST3 moduleinfo.json: $moduleInfo"
}

Write-Host "Using MSBuild: $msbuild"
Write-Host "Solution: $solution"
if ($EnableReleaseDiagnostics.IsPresent) {
    Write-Host "Release diagnostics compile flag: ENABLED (NOVA_ENABLE_RELEASE_DIAGNOSTICS=1)"
}

if ($Target -eq "Solution") {
    Write-Host "Target: $Target -> $buildAction | Configuration: $Configuration | Platform: $Platform"
    Invoke-MSBuildTarget -Path $solution -TargetName $buildAction
} else {
    $orderedTargets = switch ($Target) {
        "NOVA_StandalonePlugin" { @("NOVA_SharedCode", "NOVA_StandalonePlugin") }
        "NOVA_VST3" { @("NOVA_SharedCode", "NOVA_VST3ManifestHelper", "NOVA_VST3") }
        default { @($Target) }
    }

    foreach ($projectTarget in $orderedTargets) {
        Invoke-ProjectBuild -ProjectKey $projectTarget
    }

    if ($Target -eq "NOVA_VST3") {
        Update-VST3ModuleInfo
    }
}
