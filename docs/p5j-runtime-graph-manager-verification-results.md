# P5J - RuntimeGraphManager Post-Extraction Verification

Date: 2026-04-30

## Summary

P5J focused on verification only. No DSP, routing, dry/wet, graph build, command semantics, or RuntimeGraphManager API behavior was changed.

The audio-thread ownership contract remains valid after P5I extraction, and Release runtime profiling remains clean.

Debug profiling still shows WARN pressure in small-block/stress scenarios and at 96 kHz, with high run-to-run variance in peaks. Based on current evidence, this is consistent with Debug-mode volatility plus tighter headroom, not a functional graph-lifecycle regression.

## Files Reviewed

- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/RuntimeGraphManager.h`
- `scripts/check-audio-thread-policy.ps1`
- `scripts/run-rt-profile-scenarios.ps1`
- `docs/p5h-graph-lifecycle-coverage-results.md`
- `docs/p5h-runtime-graph-manager-design.md`
- `docs/p5i-runtime-graph-manager-extraction-results.md`

## Files Changed In P5J

- `scripts/check-audio-thread-policy.ps1`
- `scripts/run-rt-profile-stability.ps1` (new)
- `docs/p5j-runtime-graph-manager-verification-results.md` (new)
- Validation artifacts:
  - `audio-base-test-report.txt`
  - `artifacts/p4-offline-qa-report.txt`
  - `artifacts/rt-profile-debug-x64-report-p5j.json`
  - `artifacts/rt-profile-release-x64-report-p5j.json`
  - `artifacts/rt-profile-stability-debug-x64-p5j.json`
  - `artifacts/rt-profile-stability-release-x64-p5j.json`
  - `artifacts/rt-profile-stability-runs-debug-x64/run-*.json`
  - `artifacts/rt-profile-stability-runs-release-x64/run-*.json`
  - `artifacts/audio-thread-policy-scan.txt`
  - `artifacts/audio-thread-policy-scan.json`

## Audio-Thread Contract Verification

Verified in code and scanner output:

- `AudioEngine::process` accesses active runtime via one call:
  - `runtimeGraphs.getActiveRaw()` at `Source/Core/AudioEngine.cpp:883`.
- `RuntimeGraphManager::getActiveRaw()` is only:
  - `return activeGraphRaw.load(std::memory_order_acquire);`
- `AudioEngine::process` does not call:
  - `runtimeGraphs.publish(...)`
  - `runtimeGraphs.cleanupRetired(...)`
  - `runtimeGraphs.getActiveOwnerForControl(...)`
- No `shared_ptr` copy was found in `AudioEngine::process`.
- No lock usage was found in `AudioEngine::process`.
- No new logging/string/allocation path was introduced in `AudioEngine::process`.

## Policy Script Hardening

Updated `scripts/check-audio-thread-policy.ps1` to add explicit RuntimeGraphManager contract checks:

- method existence checks:
  - `RuntimeGraphManager::getActiveRaw`
  - `RuntimeGraphManager::publish`
  - `RuntimeGraphManager::cleanupRetired`
  - `RuntimeGraphManager::getActiveOwnerForControl`
- process contract checks:
  - `AudioEngine::process` must call `runtimeGraphs.getActiveRaw()`
  - `AudioEngine::process` must not call publish/cleanup/getActiveOwnerForControl
  - `AudioEngine::process` must not reference `std::shared_ptr` or `juce::ScopedLock/CriticalSection`
- method body check:
  - `getActiveRaw` must remain direct acquire-load return

Current policy output:

- `status=WARN`
- `summary.failures=0`
- `summary.contractChecks=6`
- `summary.contractFailures=0`
- no FAIL findings in active routes
- WARN only from existing legacy findings

## RT Stability Script

Created `scripts/run-rt-profile-stability.ps1`.

Capabilities:

- runs `N` repeated RT scenario passes for Debug or Release
- optional scenario filter
- writes aggregate JSON summary
- reports min/median/max for:
  - `maxBudgetRatio`
  - `cpuAvgPercent`
  - `cpuPeakPercent`
  - `peakProcessMs`
- includes optional short pause between runs (`PauseMilliseconds`, default `500`) to reduce process-launch contention while sampling repeated runs

## Validation Results

Builds:

- PASS - `NOVA_SharedCode` Debug x64
- PASS - `NOVA_StandalonePlugin` Debug x64
- PASS - `NOVA_SharedCode` Release x64
- PASS - `NOVA_StandalonePlugin` Release x64
- all with `0 warnings / 0 errors`

Diff check:

- PASS - `git diff --check`
- only existing LF/CRLF working-copy warnings reported

Base validation:

- PASS - `results=143 passes=5830 failures=0 failingResults=0`

Golden metrics:

- PASS - baseline P4 unchanged

RT single-run:

- Debug: `total=16 pass=14 warn=2 fail=0`
- Release: `total=16 pass=16 warn=0 fail=0`

Policy:

- PASS/WARN - `status=WARN`, `failures=0`, `contractFailures=0`

## Multi-Run RT Results (Stability)

### Debug (5 runs, prioritized scenarios)

Artifacts:

- `artifacts/rt-profile-stability-debug-x64-p5j.json`
- per-run: `artifacts/rt-profile-stability-runs-debug-x64/run-01..05.json`

`overdrive_cleanamp_reverb_chain_nominal`:

- status counts: `5 pass / 0 warn / 0 fail`
- maxBudgetRatio min/median/max: `0.525975 / 0.660263 / 0.696788`
- cpuAvgPercent min/median/max: `44.067277 / 44.651629 / 45.737612`
- cpuPeakPercent min/median/max: `52.597500 / 66.026250 / 69.678750`
- peakProcessMs min/median/max: `1.402600 / 1.760700 / 1.858100`

`stress_block_32`:

- status counts: `0 pass / 5 warn / 0 fail`
- maxBudgetRatio min/median/max: `0.769800 / 0.796950 / 0.912900`
- cpuAvgPercent min/median/max: `45.608844 / 46.789489 / 47.113111`
- cpuPeakPercent min/median/max: `76.980000 / 79.695000 / 91.290000`
- peakProcessMs min/median/max: `0.513200 / 0.531300 / 0.608600`

`sample_rate_44100`:

- status counts: `5 pass / 0 warn / 0 fail`
- maxBudgetRatio min/median/max: `0.457262 / 0.557452 / 0.588873`
- cpuAvgPercent min/median/max: `40.173366 / 40.420340 / 41.351130`
- cpuPeakPercent min/median/max: `45.726187 / 55.745156 / 58.887281`
- peakProcessMs min/median/max: `1.327200 / 1.618000 / 1.709200`

`sample_rate_96000`:

- status counts: `0 pass / 5 warn / 0 fail`
- maxBudgetRatio min/median/max: `1.256850 / 1.352250 / 1.376325`
- cpuAvgPercent min/median/max: `87.474592 / 87.769095 / 89.543234`
- cpuPeakPercent min/median/max: `125.685000 / 135.225000 / 137.632500`
- peakProcessMs min/median/max: `1.675800 / 1.803000 / 1.835100`

### Release (3 runs, prioritized scenarios)

Artifacts:

- `artifacts/rt-profile-stability-release-x64-p5j.json`
- per-run: `artifacts/rt-profile-stability-runs-release-x64/run-01..03.json`

All prioritized scenarios remained `PASS` across all runs:

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, mbr median `0.128813`
- `stress_block_32`: `3/0/0`, mbr median `0.142650`
- `sample_rate_44100`: `3/0/0`, mbr median `0.116486`
- `sample_rate_96000`: `3/0/0`, mbr median `0.269325`

## Interpretation: Jitter vs Real Regression

Observed:

- Release remains clean (`16/16/0/0`) in both single-run and repeated runs.
- Debug warnings concentrate in known pressure scenarios (`stress_block_32`, `sample_rate_96000`) and fluctuate in peak magnitude across runs.
- Audio-thread ownership contract checks pass with zero failures, and process-path constraints remain intact.

Conclusion:

- No evidence of functional runtime regression in graph ownership/lifecycle semantics from P5I.
- Debug WARN increases appear consistent with Debug-mode scheduling/peak sensitivity under stress and sample-rate pressure, not a structural publish/ownership bug.
- Performance headroom in Debug is tighter than P5H and should be monitored, but current data does not justify RuntimeGraphManager rollback or API changes.

## Remaining Risks

- Debug stress scenarios now sit near/above warning thresholds frequently; noisy hosts can trigger extra WARNs.
- Release is healthy, but repeated Debug trend should continue to be tracked in future phases.
- Policy contract checks currently validate method presence/use and key constraints; they do not prove full lock-free behavior outside the scoped checks.

## Recommendation For P5K

1. Keep RuntimeGraphManager unchanged; no extraction rollback.
2. Keep running `run-rt-profile-stability.ps1` in CI/nightly for Debug with prioritized scenarios.
3. Track medians and 95th-percentile-like peak trends over time before changing thresholds.
4. If Debug pressure continues increasing while Release remains clean, treat it as tooling/noise/perf-observability follow-up, not lifecycle ownership refactor.
5. Do not mix P5K with GraphBuilder/RoutingMixer/DryWetMixer/DiagnosticsManager refactors.

