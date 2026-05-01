# P5I - RuntimeGraphManager Extraction Results

Date: 2026-04-30

## Summary

P5I implemented a narrow, header-only `RuntimeGraphManager` that owns active graph lifetime and raw pointer publication. `AudioEngine` still builds graphs, decides when to rebuild, applies commands, owns routing/dry-wet processing, and controls the public `publishGraph` call site.

No DSP, tone, routing, dry/wet, graph topology build, command semantics, runtime parameter snapshot, HealthMonitor, CpuMeter, GraphCommandApplier, preset schema, parameter IDs, golden baselines, or known failures were changed.

## Files Modified

- `Source/Core/Audio/RuntimeGraphManager.h`
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `docs/p5i-runtime-graph-manager-extraction-results.md`
- Validation artifacts:
  - `audio-base-test-report.txt`
  - `artifacts/p4-offline-qa-report.txt`
  - `artifacts/rt-profile-debug-x64-report-p5i.json`
  - `artifacts/rt-profile-release-x64-report-p5i.json`
  - `artifacts/audio-thread-policy-scan.txt`
  - `artifacts/audio-thread-policy-scan.json`

## Module Created

Created `Nova::Audio::RuntimeGraphManager<GraphRuntime>` in `Source/Core/Audio/RuntimeGraphManager.h`.

The module is header-only to avoid changing `NOVA.jucer`.

Public API:

- `GraphRuntime* getActiveRaw() const noexcept`
- `template <typename LatencyChanged> bool publish(std::shared_ptr<GraphRuntime>, uint64_t, LatencyChanged&&)`
- `std::shared_ptr<GraphRuntime> getActiveOwnerForControl() const`
- `void cleanupRetired(uint64_t)`
- `void clear()`
- `int getLatencySamples() const noexcept`
- `size_t retiredSize() const noexcept`

`publish` uses a templated callback, not `std::function`, so the latency callback does not add type-erasure allocation overhead.

## State Moved

Moved from `AudioEngine` into `RuntimeGraphManager`:

- `activeOwnerLock`
- `activeOwner`
- `activeGraphRaw`
- `retiredGraphs`
- graph retirement grace constant, still `8` blocks

The active latency mirror is represented by `RuntimeGraphManager::getLatencySamples()`, which reads the active runtime through the same raw pointer publication path and returns `0` when no active runtime exists.

## Logic Moved

Moved into `RuntimeGraphManager`:

- active owner swap
- active raw pointer publication
- latency callback invocation at the publish point
- old graph retirement using `currentAudioBlock + 8`
- retired graph cleanup delegation
- active/retired graph clear for shutdown
- active owner access for control-thread diagnostics and node queries
- active latency getter

The existing `GraphRetirementQueue` cleanup semantics remain unchanged:

- release condition remains `releaseAfterAudioBlock <= currentAudioBlock`
- memory bound remains `8` retired graphs
- bounded cleanup still drops oldest retired graphs when the queue grows beyond the bound

## What Stayed In AudioEngine

Still owned by `AudioEngine`:

- `buildGraphFromModelLocked`
- `connectRuntimeChain`
- `applyRuntimeParamsToGraph`
- `applyPedalBypassToActiveGraph`
- `flushPendingGraphCommands`
- `requestControlGraphRebuild`
- `updateDryDelayLatency`
- `process`
- dry/wet processing
- routing and mixer behavior
- model chains
- command queue and command applier
- HealthMonitor, CpuMeter, Tuner, and diagnostics formatting
- GraphBuilder usage

`AudioEngine::publishGraph` remains as a wrapper that supplies `audioBlockCounter` and the dry-delay latency callback.

## Publish Order Preservation

The publish order remains equivalent to the pre-P5I code:

1. move old active owner into `oldGraph`
2. move `newGraph` into active owner
3. publish `activeGraphRaw` with `std::memory_order_release`
4. invoke the dry-delay latency callback with `activeOwner->latencySamples`
5. retire `oldGraph` with `currentAudioBlock + 8`
6. release `activeOwnerLock`
7. run retired graph cleanup from the control path

`AudioEngine::publishGraph` still calls `cleanupRetiredGraphs()` after publication. Cleanup is not called from `AudioEngine::process`.

## Active Raw Publication Preservation

`RuntimeGraphManager::publish` stores the active raw pointer with `std::memory_order_release` immediately after the new active owner is installed. `RuntimeGraphManager::getActiveRaw()` performs only:

```cpp
return activeGraphRaw.load(std::memory_order_acquire);
```

All previous `AudioEngine` raw pointer reads now go through `runtimeGraphs.getActiveRaw()`.

## Dry Delay Latency Preservation

`AudioEngine::publishGraph` passes a no-allocation templated callback:

```cpp
[this](int latencySamples) noexcept
{
    updateDryDelayLatency(latencySamples);
}
```

The callback is invoked by `RuntimeGraphManager::publish` after raw pointer publication and before retiring the old graph, matching the previous logical point.

Bypass in-place latency rebuild behavior remains in `AudioEngine::applyPedalBypassToActiveGraph` and still calls `updateDryDelayLatency(runtime->latencySamples)` directly after rebuilding the active graph latency.

## Grace Period Preservation

The retired graph grace period remains exactly `8` audio blocks.

`RuntimeGraphManager::publish` retires the old graph with:

```cpp
currentAudioBlock + kGraphRetireGraceBlocks
```

where `kGraphRetireGraceBlocks == 8`.

`GraphRetirementQueue` still releases retired graphs only when:

```cpp
releaseAfterAudioBlock <= currentAudioBlock
```

## Audio Thread Contract

`AudioEngine::process` calls only `runtimeGraphs.getActiveRaw()` to access the graph. That call is a single atomic raw pointer load.

The audio thread does not call:

- `publish`
- `cleanupRetired`
- `clear`
- `getActiveOwnerForControl`
- `retiredSize`

No new lock, `shared_ptr` copy, string/logging path, or allocation was introduced in `AudioEngine::process`.

## Tests Added

Added:

- `RuntimeGraphManager publishes raw pointer before retiring old graph`

This test verifies:

- publish callback observes the newly published raw pointer
- callback receives the active graph latency
- `getActiveRaw()` returns the published graph
- `getLatencySamples()` reflects the active graph latency
- old graph remains alive before `publishBlock + 8`
- old graph is released at exactly the grace boundary
- `clear()` nulls the active raw pointer and releases the active owner

P5H graph lifecycle tests still pass.

## Validation Results

Builds:

- PASS - `NOVA_SharedCode` Debug x64, 0 warnings, 0 errors.
- PASS - `NOVA_StandalonePlugin` Debug x64, 0 warnings, 0 errors.
- PASS - `NOVA_SharedCode` Release x64, 0 warnings, 0 errors.
- PASS - `NOVA_StandalonePlugin` Release x64, 0 warnings, 0 errors.

Static checks:

- PASS - `git diff --check`
  - Only LF-to-CRLF working-copy warnings were reported.
- PASS/WARN - `scripts/check-audio-thread-policy.ps1`
  - `status=WARN`
  - `activeFiles=32`
  - `activeRanges=38`
  - `failures=0`
  - `warnings=4`
  - `legacyWarnings=4`
  - No FAIL findings in active audio routes.

Audio validation:

- PASS - `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`
  - `results=143`
  - `passes=5830`
  - `failures=0`
  - `failingResults=0`
  - The count increased from P5H because P5I added direct `RuntimeGraphManager` lifecycle coverage.

Golden metrics:

- PASS - `scripts/run-golden-audio-metrics.ps1`
  - Golden metrics passed against `docs/golden-metrics/p4-offline-qa-baseline.json`.
  - No golden baselines were updated.
  - No known failures were added.

RT profile:

- PASS/WARN - Debug x64
  - artifact: `artifacts/rt-profile-debug-x64-report-p5i.json`
  - `total=16`
  - `pass=13`
  - `warn=3`
  - `fail=0`
  - WARN scenarios in the latest run: `stress_block_32`, `sample_rate_44100`, `sample_rate_96000`

- PASS - Release x64
  - artifact: `artifacts/rt-profile-release-x64-report-p5i.json`
  - `total=16`
  - `pass=16`
  - `warn=0`
  - `fail=0`

## RT Profile Comparison Against P5H

Debug:

| Scenario | P5H | P5I |
| --- | ---: | ---: |
| Summary | 15 PASS / 1 WARN / 0 FAIL | 13 PASS / 3 WARN / 0 FAIL |
| `stress_block_32` maxBudgetRatio | 0.508050 | 0.806250 |
| `sample_rate_44100` maxBudgetRatio | 0.393282 | 1.496850 |
| `sample_rate_96000` maxBudgetRatio | 0.864525 | 1.002450 |

Release:

| Scenario | P5H | P5I |
| --- | ---: | ---: |
| Summary | 16 PASS / 0 WARN / 0 FAIL | 16 PASS / 0 WARN / 0 FAIL |
| `stress_block_32` maxBudgetRatio | 0.115650 | 0.139950 |
| `sample_rate_44100` maxBudgetRatio | 0.086960 | 0.121688 |
| `sample_rate_96000` maxBudgetRatio | 0.196575 | 0.268800 |

The Release profile remains clean. Debug showed additional WARNs in the latest run, driven by peak-budget outliers rather than validation failures. The P5I code path in `AudioEngine::process` still performs only an atomic raw pointer load for active graph access, so this should be watched in P5J but does not indicate a DSP or graph lifecycle semantic change by itself.

## Risks Remaining

- Debug RT profiles were noisier than P5H in the latest run. Release stayed clean, but P5J should rerun RT Debug and compare multiple runs before attributing the WARN deltas to code.
- `RuntimeGraphManager::publish` intentionally invokes the latency callback while holding the active owner lock to preserve the old order. Future changes should not move this callback unless the publish contract is updated and retested.
- `getActiveOwnerForControl()` copies a `shared_ptr` under lock and must remain control-thread only.
- `cleanupRetired()` takes the owner lock and erases shared owners; it must remain outside `AudioEngine::process`.
- The manager still depends on `GraphRuntime::latencySamples`; this is acceptable for the current narrow extraction but should stay documented if `GraphRuntime` changes.

## Recommendation For P5J

Proceed conservatively with post-extraction cleanup and verification, not GraphBuilder or DSP work.

Recommended P5J scope:

- keep `RuntimeGraphManager` API stable
- add a focused policy/test check that `AudioEngine::process` only uses `getActiveRaw()` for graph ownership
- rerun RT Debug several times or add a median/percentile comparison mode to separate jitter from regression
- consider moving only small diagnostics helpers that consume `getActiveOwnerForControl()`, if that reduces `AudioEngine` size without moving graph build or command semantics
- do not combine P5J with GraphBuilder, RoutingMixer, DryWetMixer, DiagnosticsManager, or DSP changes

