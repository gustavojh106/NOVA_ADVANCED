param(
    [string]$BaselinePath = "artifacts/p14b-p1-drive-dynamics-measurement-baseline.json",

    [string]$CurrentPath,

    [switch]$WriteNormalizedArtifacts
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return Join-Path $repoRoot $PathValue
}

function Get-StableP14BJson {
    param([string]$PathValue)

    $json = Get-Content -LiteralPath $PathValue -Raw | ConvertFrom-Json

    if ($null -ne $json.metadata) {
        foreach ($field in @(
            "generatedAt",
            "commitHash",
            "branch",
            "buildConfiguration",
            "buildPlatform",
            "compiler",
            "osName"
        )) {
            if ($json.metadata.PSObject.Properties.Name -contains $field) {
                $json.metadata.$field = "<ignored-for-stable-comparison>"
            }
        }
    }

    return ($json | ConvertTo-Json -Depth 100)
}

function Get-TextSha256 {
    param([string]$Text)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $hashBytes = $sha.ComputeHash($bytes)
        return (($hashBytes | ForEach-Object { $_.ToString("x2") }) -join "")
    }
    finally {
        $sha.Dispose()
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if ([string]::IsNullOrWhiteSpace($CurrentPath)) {
    throw "CurrentPath is required. Generate a second report, then compare it to the baseline."
}

$baselineAbs = Resolve-RepoPath -PathValue $BaselinePath
$currentAbs = Resolve-RepoPath -PathValue $CurrentPath

if (-not (Test-Path $baselineAbs)) {
    throw "Baseline report not found: $baselineAbs"
}
if (-not (Test-Path $currentAbs)) {
    throw "Current report not found: $currentAbs"
}

$baselineStable = Get-StableP14BJson -PathValue $baselineAbs
$currentStable = Get-StableP14BJson -PathValue $currentAbs

if ($WriteNormalizedArtifacts.IsPresent) {
    Set-Content -LiteralPath ($baselineAbs + ".stable.json") -Value $baselineStable -Encoding UTF8
    Set-Content -LiteralPath ($currentAbs + ".stable.json") -Value $currentStable -Encoding UTF8
}

$baselineHash = Get-TextSha256 -Text $baselineStable
$currentHash = Get-TextSha256 -Text $currentStable

Write-Host "Baseline stable SHA256: $baselineHash"
Write-Host "Current stable SHA256:  $currentHash"

if ($baselineHash -ne $currentHash) {
    throw "P14B stable measurement comparison failed."
}

Write-Host "P14B stable measurement comparison passed."
