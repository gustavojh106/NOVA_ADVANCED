# P5C HealthMonitor Extraction Results

Date: 2026-04-29

## Summary

P5C extracted the AudioEngine health/safety/auto-heal logic into a dedicated header-only `HealthMonitor` module without changing DSP, routing, dry/wet behavior, graph build/swap, runtime parameters, UI, parameter IDs, or preset schema.

The extraction is intentionally conservative:

- `AudioEngine` still owns graph reset flags, pending log flags, control-thread graph rebuild, diagnostics report assembly, routing, dry/wet, tuner, and runtime parameter reads.
- `HealthMonitor` performs only sample sanitization, output metering, corruption counting, silent-output detection, and action flag calculation.
- `HealthMonitor` does not log, build `juce::String`, allocate, lock, rebuild graphs, or call callbacks.
- No golden baseline files were updated.

## Files Modified

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/HealthMonitor.h`
- `scripts/check-audio-thread-policy.ps1`
- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`
- `artifacts/p4-offline-qa-report.txt`
- `artifacts/rt-profile-debug-x64-report-p5c.json`
- `artifacts/rt-profile-release-x64-report-p5c.json`

Existing uncommitted P5A/P5B files remain part of the working tree:

- `Source/Core/Audio/CpuMeter.h`
- `docs/p5a-audioengine-refactor-plan.md`
- `docs/p5b-cpumeter-extraction-results.md`
- `artifacts/rt-profile-debug-x64-report-p5b.json`
- `artifacts/rt-profile-release-x64-report-p5b.json`

## Module Created

Created `Source/Core/Audio/HealthMonitor.h`.

The module is header-only to avoid touching `NOVA.jucer` or Visual Studio project membership during this refactor phase. The code is small, has no private `.cpp` dependencies, and is included from `AudioEngine.h`.

Public surface:

- `HealthMonitor::BlockStats`
- `HealthMonitor::Context`
- `HealthMonitor::Actions`
- `reset(bool resetMeters) noexcept`
- `sanitizeAndMeterOutput(...) noexcept`
- `afterBlock(...) noexcept`
- `getLastOutputPeak() const noexcept`
- `getAutoHealCount() const noexcept`

## State Moved From AudioEngine

Moved into `HealthMonitor`:

- Former `AudioEngine::BlockHealthStats`, now `HealthMonitor::BlockStats`
- `lastOutputPeak`
- `autoHealCount`
- `silentOutputBlockCounter`
- `silentOutputIncidentActive`
- `consecutiveCorruptBlocks`
- `recoveryCooldownBlocks`

There was no existing `consecutiveRecoveredOutputBlocks` state in the current codebase. Recovery behavior remains represented by the existing `silentOutputIncidentActive` edge transition.

## Logic Moved From AudioEngine

Moved into `HealthMonitor`:

- `sanitizeAndMeterOutput(...)`
- Invalid sample replacement with `0.0f`
- Hard-limit clamping with the existing `Nova::Config::HARD_ABS_LIMIT_LINEAR`
- Output peak tracking
- Near-clip counting
- Click spike counting
- Consecutive corrupt block tracking
- Auto-heal trigger/cooldown counter logic
- Silent-output detection
- Silent-output recovery detection

All thresholds still come from the existing `Nova::Config` constants.

## Logic Kept In AudioEngine

Kept in `AudioEngine`:

- Graph ownership/build/swap/retirement
- `graphResetRequested`
- `pendingAutoHealLog`
- `pendingSilentOutputLog`
- `pendingSilentOutputRecoveryLog`
- Control-thread graph rebuild execution
- Session/event logging from non-audio paths
- Runtime parameter atomics and context reads
- Tuner handling
- Dry/wet and routing
- Latency and dry delay handling
- Diagnostics report formatting

`AudioEngine` now builds a small `HealthMonitor::Context`, calls `afterBlock(...)`, then applies returned `Actions` to existing atomics.

## Behavior Preservation

Sanitization order is unchanged:

- Early returns still sanitize in the same branches.
- Full processing still runs graph/dry-wet first, then sanitizes output.
- `handleHealthAfterBlock(...)` is still called after sanitization.
- `engineActuallyProcessed` values are unchanged.

Auto-heal preservation:

- Corrupt blocks are counted the same way.
- Cooldown decrement and reset behavior are unchanged.
- Graph reset remains an atomic request in `AudioEngine`.
- Auto-heal logging remains outside the audio-thread helper and is still signaled via the existing pending flag.
- `getAutoHealCount()` keeps the same public API and returns the `HealthMonitor` counter.

Silent-output preservation:

- Suspicious silence uses the same conditions: processed block, engine on, tuner off, output mix above threshold, active input, silent output.
- Trigger block calculation uses the same sample-rate/block-size formula and minimum block count.
- Incident and recovery edges still produce the same pending log flags in `AudioEngine`.
- Reset behavior still clears silent-output state on runtime reset.

## Policy Scan Update

`scripts/check-audio-thread-policy.ps1` was updated because P5B/P5C moved audio-thread code into header-only active modules.

Added active scan coverage for:

- `Source/Core/Audio/CpuMeter.h`
- `Source/Core/Audio/HealthMonitor.h`
- `CpuMeter::beginBlock`
- `CpuMeter::endBlock`
- `HealthMonitor::sanitizeAndMeterOutput`
- `HealthMonitor::afterBlock`

This prevents false PASS results where active header-only audio-thread helpers are not scanned.

## Validation Commands

Executed:

- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode`
- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin`
- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_SharedCode`
- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin`
- `git diff --check`
- `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`
- `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1`
- `powershell -ExecutionPolicy Bypass -File scripts/run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240 -ReportPath artifacts/rt-profile-debug-x64-report-p5c.json`
- `powershell -ExecutionPolicy Bypass -File scripts/run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report-p5c.json`
- `powershell -ExecutionPolicy Bypass -File scripts/check-audio-thread-policy.ps1`

## Validation Results

Builds:

- `NOVA_SharedCode Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_SharedCode Release x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Release x64`: PASS, 0 warnings, 0 errors.

Static checks:

- `git diff --check`: PASS. Git reported LF-to-CRLF working-copy warnings only.
- `check-audio-thread-policy.ps1`: WARN, non-blocking.
- Policy scan active routes: `activeFiles=31`, `activeRanges=34`, `failures=0`.
- Policy scan warnings: 4 legacy warnings, unchanged class of issue from P4C.

Base validation:

- `run-base-audio-validation.ps1`: PASS.
- `results=136 passes=5758 failures=0 failingResults=0`.
- Known failures ignored by script: none.

Golden metrics:

- `run-golden-audio-metrics.ps1`: PASS.
- Baseline: `docs/golden-metrics/p4-offline-qa-baseline.json`.
- Coverage gaps: none reported.

RT profile Debug:

- P5C: `total=16 pass=15 warn=1 fail=0`.
- Remaining WARN: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=0.970575`.
- Warning text: peak budget ratio exceeded 75%; one or more blocks exceeded 90% budget.

RT profile Release:

- P5C: `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.225975`.

## RT Profile Comparison

P5B Debug:

- `total=16 pass=14 warn=2 fail=0`.
- WARN scenarios: `stress_block_32`, `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=1.808775`.

P5C Debug:

- `total=16 pass=15 warn=1 fail=0`.
- WARN scenarios: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=0.970575`.

P5B Release:

- `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.269625`.

P5C Release:

- `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.225975`.

Interpretation: no broad RT regression was observed. Debug timing remains noisy and still has the known 96 kHz warning class, while Release remains clean.

## Risks Remaining

- Debug profiling remains sensitive to scheduling noise, especially `sample_rate_96000`.
- `HealthMonitor` still executes deep-scan sample loops in diagnostic modes exactly as before; this is intentional preservation, not optimization.
- The next refactor phases must preserve the exact placement of health monitoring relative to dry/wet, tuner, startup mute, and graph processing.
- Legacy policy warnings remain outside active routes and should not be registered without P1 modernization.

## Recommendation For P5D

Proceed with `RuntimeParameterSnapshot / GlobalParams bridge` extraction only if it is kept smaller than the graph-management refactor.

Recommended P5D guardrails:

- Do not change parameter IDs, preset schema, smoothing behavior, or host notification semantics.
- Keep parameter atomics as the realtime-facing boundary.
- Add policy-scan coverage if more header-only audio-thread helpers are introduced.
- Run the full P5C validation matrix before and after the extraction.
