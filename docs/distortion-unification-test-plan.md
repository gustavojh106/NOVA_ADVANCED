# Distortion Unification Test Plan

## Objective

Validate that the unified `Distortion` pedal:

- replaces `Metal Distortion` without breaking stored sessions or catalog lookups
- delivers distinct commercial-grade gain voices from vintage drive to modern metal
- keeps the dry path transparent at `mix = 0`
- remains numerically stable under automation and long renders

## Build Order

Always rebuild in this order so DSP changes are actually present in the standalone binary:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' 'Builds/VisualStudio2022/NOVA_SharedCode.vcxproj' '/t:Rebuild' '/p:Configuration=Debug' '/p:Platform=x64' '/m'
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' 'Builds/VisualStudio2022/NOVA_StandalonePlugin.vcxproj' '/t:Rebuild' '/p:Configuration=Debug' '/p:Platform=x64' '/m'
```

If `NOVA.exe` is still running, stop it before rebuilding:

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'NOVA*' -or $_.ProcessName -eq 'NOVA' } | Stop-Process -Force
```

## Automated Regression

Run the audio validation suite with:

```powershell
$report = Join-Path $PWD 'distortion-unification-test-report.txt'
if (Test-Path $report) { Remove-Item -Force $report }
$env:NOVA_RUN_AUDIO_TESTS='1'
$env:NOVA_TEST_REPORT_PATH=$report
$exe = Resolve-Path 'Builds\VisualStudio2022\x64\Debug\Standalone Plugin\NOVA.exe'
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 20
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Content $report
```

Acceptance criteria for the unified pedal:

- `DistortionPedal round-trips its commercial state`
- `DistortionPedal restores legacy three-mode state without remapping its modes`
- `DistortionPedal migrates legacy metal state into the unified circuit`
- `DistortionPedal mix zero keeps the dry path transparent`
- `DistortionPedal modes produce distinct drive signatures`
- `DistortionPedal tight control audibly reduces low-end bloom in the modern voices`
- `DistortionPedal metal mode tight control closes the integrated gate harder`
- `DistortionPedal automation stress remains finite under aggressive changes`
- registry/catalog tests must keep resolving `Metal Distortion` to canonical `Distortion`

## Manual Audio QA

Use the standalone build and validate these scenarios by ear:

1. `Vintage` mode with medium gain: chords stay open, upper mids remain articulate, low end does not flap.
2. `Turbo` mode with higher gain: stronger push and saturation than `Vintage`, but still more open than `Metal`.
3. `Amp` mode with medium-high gain: amp-like compression and smoother transient rounding than `Turbo`.
4. `Metal` mode with high gain and high `Tight`: palm mutes clamp fast, tail noise closes decisively, fizz stays controlled.
5. `Studio` mode in a dense mix: less scoop than `Metal`, more polished top, fast to place without extra EQ.
6. Sweep `Tight` from low to high in `Metal` and `Studio`: low-end bloom should shrink and the gate should close more aggressively.
7. Set `Mix` to `0`: output must sound fully dry and level-consistent.
8. Load an old session or state that used `Metal Distortion`: it must reopen as `Distortion` with a modern metal voicing instead of a missing pedal.

## Compatibility Checks

- Browser/catalog must expose only `Distortion`, not a separate `Metal Distortion` entry.
- `PedalRegistry::isTypeSupported("Metal Distortion")` must stay true.
- `PedalRegistry::canonicalType("Metal Distortion")` must return `Distortion`.
- `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` remains a compatibility alias only.

## Current Validation Snapshot

Latest validated run after rebuilding `SharedCode` and `Standalone`:

- Distortion-specific regression checks passed
- full suite result: `results=67 passes=324 failures=64 failingResults=13`
- remaining failures are pre-existing baseline issues outside this pedal work
