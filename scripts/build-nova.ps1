param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [ValidateSet("NOVA_StandalonePlugin", "NOVA_VST3", "NOVA_SharedCode", "NOVA_VST3ManifestHelper", "Solution")]
    [string]$Target = "NOVA_StandalonePlugin"
)

$ErrorActionPreference = "Stop"

function Resolve-MSBuild {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installationPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
        if ($installationPath) {
            $candidate = Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    throw "MSBuild.exe was not found."
}

Set-Location (Split-Path -Parent $PSScriptRoot)

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

# The Projucer-generated projects only declare these dependencies at solution level,
# which MSBuild ignores when handed a bare .vcxproj. Without building them first, a
# clean clone fails to link with "LNK1104: cannot open file NOVA.lib".
$dependencyMap = @{
    "NOVA_StandalonePlugin"   = @("NOVA_SharedCode")
    "NOVA_VST3"               = @("NOVA_SharedCode", "NOVA_VST3ManifestHelper")
    "NOVA_SharedCode"         = @()
    "NOVA_VST3ManifestHelper" = @()
}

$msbuild = Resolve-MSBuild
Write-Host "Using MSBuild: $msbuild"
Write-Host "Solution: $solution"

if ($Target -eq "Solution") {
    Write-Host "Target: $Target -> Build | Configuration: $Configuration | Platform: $Platform"
    & $msbuild $solution "/t:Build" "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/m"
} else {
    foreach ($name in ($dependencyMap[$Target] + @($Target))) {
        $project = Join-Path (Get-Location) $projectMap[$name]
        if (-not (Test-Path $project)) {
            throw "Project file not found at $project"
        }

        Write-Host "Target: $name -> $project | Configuration: $Configuration | Platform: $Platform"
        & $msbuild $project "/t:Build" "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/m"
        if ($LASTEXITCODE -ne 0) {
            throw "Build of $name failed with exit code $LASTEXITCODE"
        }
    }
}

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}
