# RT Profile Stability Guide

Date: 2026-04-30

## Purpose

`scripts/run-rt-profile-stability.ps1` runs repeated RT profile passes and aggregates min/median/max metrics per scenario.  
It is intended for trend tracking and CI/nightly guardrails, not for changing DSP thresholds.

## Script

`scripts/run-rt-profile-stability.ps1`

## Basic Usage

Debug, 5 runs:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-stability.ps1 `
  -Configuration Debug `
  -Platform x64 `
  -Runs 5 `
  -TimeoutSeconds 240 `
  -OutputPath artifacts\rt-profile-stability-debug-x64.json
```

Release, 3 runs:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-stability.ps1 `
  -Configuration Release `
  -Platform x64 `
  -Runs 3 `
  -TimeoutSeconds 240 `
  -BaselinePath docs\rt-profile\p4c-rt-profile-release-baseline.json `
  -OutputPath artifacts\rt-profile-stability-release-x64.json
```

With prioritized scenarios:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-stability.ps1 `
  -Configuration Release `
  -Platform x64 `
  -Runs 3 `
  -ScenarioFilter stress_block_32,sample_rate_44100,sample_rate_96000,overdrive_cleanamp_reverb_chain_nominal `
  -CiMode `
  -OutputPath artifacts\rt-profile-stability-release-priority.json
```

Nightly profile (Release gate):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-stability.ps1 `
  -Configuration Release `
  -Platform x64 `
  -NightlyMode `
  -ScenarioFilter stress_block_32,sample_rate_44100,sample_rate_96000,overdrive_cleanamp_reverb_chain_nominal `
  -OutputPath artifacts\rt-profile-stability-release-nightly.json
```

## Recommended Scenario Set

Use this set for stability tracking across phases:

- `stress_block_32`
- `sample_rate_44100`
- `sample_rate_96000`
- `overdrive_cleanamp_reverb_chain_nominal`

## Metrics Meaning

For each scenario, the script reports min/median/max across runs:

- `maxBudgetRatio`: peak processing-time budget ratio; values above `1.0` exceed block budget.
- `cpuAvgPercent`: average CPU percentage for scenario blocks.
- `cpuPeakPercent`: peak CPU percentage for scenario blocks.
- `peakProcessMs`: maximum per-block processing time in milliseconds.

Interpretation:

- `min`: best observed run.
- `median`: robust center trend; primary signal for comparisons.
- `max`: worst observed spike; useful for jitter visibility.

## Guardrail Modes

`-CiMode`:

- default `Runs=3` unless overridden
- in Release, enables strict blocking:
  - `FailOnReleaseWarn=true`
  - `FailOnReleaseFail=true`

`-NightlyMode`:

- default `Runs=5` unless overridden
- in Release, same strict blocking as CI

Manual gating flags:

- `-FailOnReleaseWarn <bool>`
- `-FailOnReleaseFail <bool>`
- `-AllowDebugWarn <bool>` (default `true`)
- `-AllowDebugFail <bool>` (default `false`)

If `-ScenarioFilter` is set, blocking decisions are computed from filtered scenarios only.

## Blocking Policy

Recommended:

- Release:
  - block on FAIL always
  - block on WARN in CI/nightly (`-CiMode`/`-NightlyMode`)
- Debug:
  - WARN should not block by default
  - FAIL should block by default
  - only allow Debug FAIL when explicitly needed: `-AllowDebugFail $true`

## Artifact Layout

Aggregate summary:

- `artifacts/rt-profile-stability-<config>-<platform>.json`

Per-run raw reports:

- `artifacts/rt-profile-stability-runs-<config>-<platform>/run-01.json`
- `.../run-02.json`
- etc.

## Comparing Against Earlier Phases

1. Keep phase-stamped outputs, for example:
   - `artifacts/rt-profile-stability-debug-x64-p5j.json`
   - `artifacts/rt-profile-stability-release-x64-p5j.json`
2. Compare medians first, then max values:
   - median shift suggests trend change
   - max-only shift often indicates host/scheduling jitter
3. Use Release multi-run as the primary gate signal; treat Debug as sensitivity/early-warning signal.
