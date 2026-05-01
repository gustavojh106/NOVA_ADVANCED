# P5K Audio Quality Gates

Date: 2026-04-30

## Scope

This document defines execution levels for audio quality gates without changing DSP code or runtime architecture.

## Fast Local Gate

Purpose: quick feedback while iterating.

Run:

1. Build Debug (`NOVA_SharedCode` and/or `NOVA_StandalonePlugin` as needed)
2. `scripts/run-base-audio-validation.ps1`
3. `scripts/check-audio-thread-policy.ps1`

Expected policy outcome: `WARN` is acceptable if `failures=0` and `contractFailures=0`.

## Pre-Commit Local Gate

Purpose: stronger pre-push confidence.

Run:

1. Build Debug + Release
2. `scripts/run-base-audio-validation.ps1`
3. `scripts/run-golden-audio-metrics.ps1`
4. `scripts/check-audio-thread-policy.ps1`

Optional:

- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`

## Nightly Gate

Purpose: trend tracking and guardrails.

Run:

1. Build Debug + Release
2. `scripts/run-base-audio-validation.ps1`
3. `scripts/run-golden-audio-metrics.ps1`
4. `scripts/run-rt-profile-scenarios.ps1 -Configuration Debug`
5. `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`
6. `scripts/run-rt-profile-stability.ps1 -Configuration Release -NightlyMode`
7. `scripts/check-audio-thread-policy.ps1`

Optional WARN-only signal:

- `scripts/run-rt-profile-stability.ps1 -Configuration Debug -NightlyMode -AllowDebugWarn $true`

## Release Candidate Gate

Purpose: maximum confidence before cut.

Run:

1. Everything in Nightly Gate
2. Release stability with higher runs, example:
   - `scripts/run-rt-profile-stability.ps1 -Configuration Release -Runs 7 -NightlyMode`
3. Manual DAW smoke test
4. External profiling follow-up when available

## Wrapper Script

`scripts/run-audio-quality-gates.ps1` provides a simple orchestration path.

Modes:

- `-Fast`: base validation + release single-run RT + policy scan
- `-Full`: base + golden + release single-run RT + policy + release stability

Example:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-audio-quality-gates.ps1 `
  -Fast `
  -Configuration Release `
  -ValidationConfiguration Debug `
  -Platform x64 `
  -TimeoutSeconds 240
```

## Gate Interpretation Rules

- Release WARN/FAIL:
  - must be treated as blocking in CI/nightly guardrails.
- Debug WARN:
  - non-blocking by default, used for sensitivity tracking.
- Debug FAIL:
  - blocking by default unless explicitly allowed.

## Artifacts

Recommended to store per phase:

- `artifacts/rt-profile-*-report-<phase>.json`
- `artifacts/rt-profile-stability-*-<phase>.json`
- `artifacts/rt-profile-stability-runs-*/run-*.json`
- `artifacts/audio-thread-policy-scan.json`
