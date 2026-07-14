param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [string]$OutputPath = "artifacts/p14b-p1-drive-dynamics-measurement-baseline.json",

    [int]$TimeoutSeconds = 240
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return Join-Path $repoRoot $PathValue
}

function Restore-EnvValue {
    param(
        [string]$Name,
        [string]$Value
    )

    if ($null -eq $Value) {
        Remove-Item "Env:\$Name" -ErrorAction SilentlyContinue
    }
    else {
        Set-Item "Env:\$Name" $Value
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$outputPathAbs = Resolve-RepoPath -PathValue $OutputPath
$outputDir = Split-Path -Parent $outputPathAbs
if (-not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory | Out-Null
}

if (Test-Path $outputPathAbs) {
    Remove-Item -LiteralPath $outputPathAbs -Force
}

Write-Host "Building NOVA Standalone for P14B measurement generation..."
$buildArgs = @{
    Configuration = $Configuration
    Platform = $Platform
    Target = "NOVA_StandalonePlugin"
}
if ($Configuration -eq "Release") {
    Write-Host "Release P14B generation requires an explicit diagnostic build flag."
    $buildArgs.EnableReleaseDiagnostics = $true
}
& (Join-Path $PSScriptRoot "build-nova.ps1") @buildArgs

$exePath = Join-Path $repoRoot "Builds/VisualStudio2022/$Platform/$Configuration/Standalone Plugin/NOVA.exe"
if (-not (Test-Path $exePath)) {
    throw "Standalone executable not found at $exePath"
}

$previous = @{
    NOVA_P14B_MEASUREMENT_REPORT_PATH = $env:NOVA_P14B_MEASUREMENT_REPORT_PATH
    NOVA_BUILD_COMMIT = $env:NOVA_BUILD_COMMIT
    NOVA_BUILD_BRANCH = $env:NOVA_BUILD_BRANCH
    NOVA_BUILD_CONFIGURATION = $env:NOVA_BUILD_CONFIGURATION
    NOVA_BUILD_PLATFORM = $env:NOVA_BUILD_PLATFORM
}

try {
    $env:NOVA_P14B_MEASUREMENT_REPORT_PATH = $outputPathAbs
    $env:NOVA_BUILD_COMMIT = (& git rev-parse HEAD 2>$null)
    if ([string]::IsNullOrWhiteSpace($env:NOVA_BUILD_COMMIT)) {
        $env:NOVA_BUILD_COMMIT = "unknown"
    }

    $env:NOVA_BUILD_BRANCH = (& git branch --show-current 2>$null)
    if ([string]::IsNullOrWhiteSpace($env:NOVA_BUILD_BRANCH)) {
        $env:NOVA_BUILD_BRANCH = "unknown"
    }

    $env:NOVA_BUILD_CONFIGURATION = $Configuration
    $env:NOVA_BUILD_PLATFORM = "$Platform/$([Environment]::OSVersion.Platform)"

    Write-Host "Running P14B measurement writer..."
    $process = Start-Process -FilePath $exePath -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline -and -not (Test-Path $outputPathAbs) -and -not $process.HasExited) {
        Start-Sleep -Milliseconds 500
    }

    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }

    if (-not (Test-Path $outputPathAbs)) {
        throw "P14B measurement report was not written to $outputPathAbs"
    }

    $hash = (Get-FileHash -LiteralPath $outputPathAbs -Algorithm SHA256).Hash
    Write-Host "P14B measurement report written: $outputPathAbs"
    Write-Host "SHA256: $hash"
}
finally {
    foreach ($name in $previous.Keys) {
        Restore-EnvValue -Name $name -Value $previous[$name]
    }
}
