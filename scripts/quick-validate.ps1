param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

Set-Location (Split-Path -Parent $PSScriptRoot)

Write-Host "Running context bootstrap..."
& (Join-Path $PSScriptRoot "context-bootstrap.ps1")

Write-Host ""
Write-Host "Building Standalone target..."
& (Join-Path $PSScriptRoot "build-nova.ps1") -Configuration $Configuration -Platform $Platform -Target "NOVA_StandalonePlugin"

Write-Host ""
Write-Host "Quick validation completed."
Write-Host "Note: JUCE unit tests are compiled into the Standalone app and may require launching it to execute."
