# P5G - GraphRetirementQueue Extraction Results

Date: 2026-04-30

## Summary

P5G extracted only retired graph storage and cleanup into a small header-only helper:

- `Source/Core/Audio/GraphRetirementQueue.h`

The extraction is deliberately conservative. `AudioEngine` still owns the active graph, raw active graph publication, graph build, graph swap order, rebuild decisions, runtime params, HealthMonitor, CpuMeter, command queue, and command applier.

No DSP, tone, routing, dry/wet, latency, graph build, parameter IDs, preset schema, golden baselines, or known-failure lists were changed.

## Files Modified

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/GraphRetirementQueue.h`
- `docs/p5g-graph-retirement-queue-results.md`
- Validation artifacts:
  - `artifacts/rt-profile-debug-x64-report-p5g.json`
  - `artifacts/rt-profile-release-x64-report-p5g.json`
  - `artifacts/audio-thread-policy-scan.txt`
  - `artifacts/audio-thread-policy-scan.json`
  - `artifacts/p4-offline-qa-report.txt`

`NOVA.jucer` was not touched because the new module is header-only and included by `AudioEngine.h`.

## Module Created

Created `Nova::Audio::GraphRetirementQueue<GraphRuntime>`.

Public surface:

```cpp
void retire(std::shared_ptr<GraphRuntime> graph, uint64_t releaseAfterBlock);
void cleanup(uint64_t currentAudioBlock);
void clear();
size_t size() const noexcept;
```

The helper owns its private `RetiredGraph` record and `std::vector<RetiredGraph>` container. It has no locks, no logging, no JUCE string work, and no audio-thread call sites.

## State Moved

Moved from `AudioEngine` into `GraphRetirementQueue`:

- `RetiredGraph`
- `retiredGraphs` vector
- max-retired-graph bound constant, still `8`

`AudioEngine::ControlPlane` now stores:

```cpp
Nova::Audio::GraphRetirementQueue<GraphRuntime> retiredGraphs;
```

## Logic Moved

Moved into `GraphRetirementQueue`:

- holding retired graph `shared_ptr`s until a release block
- erasing retired graphs when `releaseAfterAudioBlock <= currentAudioBlock`
- bounding retained retired graphs to 8 when the audio block counter is not advancing
- clearing retired graph ownership during shutdown
- size helper for future diagnostics

## What Remains In AudioEngine

Kept in `AudioEngine`:

- `activeOwner`
- `activeGraphRaw`
- `activeOwnerLock`
- `publishGraph`
- `buildGraphFromModelLocked`
- graph swap order
- `kGraphRetireGraceBlocks`
- `nowBlock + kGraphRetireGraceBlocks` calculation
- all cleanup call sites
- all rebuild decisions
- all command flushing and model mutation
- all runtime parameter application

## Active Graph Ownership Preservation

`activeOwner` remains in `AudioEngine::ControlPlane`, guarded by the existing `activeOwnerLock`.

`activeGraphRaw` remains in `AudioEngine::AudioPlane` and is still published by `AudioEngine::publishGraph` with the same `std::memory_order_release` store.

The helper never sees or writes `activeGraphRaw`; it only holds old `shared_ptr<GraphRuntime>` instances after `AudioEngine` has already swapped ownership.

## publishGraph Preservation

The publish sequence remains:

1. load `nowBlock` from `audioBlockCounter`
2. lock `activeOwnerLock`
3. move current `activeOwner` into `oldGraph`
4. move `newGraph` into `activeOwner`
5. store `activeOwner.get()` into `activeGraphRaw`
6. update dry delay latency from the new active graph
7. retire `oldGraph` if non-null
8. unlock
9. call `cleanupRetiredGraphs()`

Only step 7 changed from a direct `push_back` to:

```cpp
controlPlane.retiredGraphs.retire(std::move(oldGraph), nowBlock + kGraphRetireGraceBlocks);
```

## Grace Period Preservation

`kGraphRetireGraceBlocks` remains in `AudioEngine.cpp` and is still `8`.

The release block calculation remains exactly:

```cpp
nowBlock + kGraphRetireGraceBlocks
```

`GraphRetirementQueue::cleanup` uses the same release condition as before:

```cpp
retired.releaseAfterAudioBlock <= currentAudioBlock
```

## Audio Thread Boundary

`AudioEngine::process` does not call `cleanupRetiredGraphs` and was not modified for P5G.

Cleanup call sites remain control-plane/lifecycle paths:

- after `publishGraph`
- `flushPendingGraphCommands` early path
- `flushPendingGraphCommands` params-only path
- `AudioEngine::run`
- shutdown clear under `activeOwnerLock`

The queue introduces no new locks. The existing `activeOwnerLock` boundary remains in `AudioEngine::cleanupRetiredGraphs`.

## Validation Results

Builds:

- PASS - `NOVA_SharedCode` Debug x64, 0 warnings, 0 errors.
- PASS - `NOVA_StandalonePlugin` Debug x64, 0 warnings, 0 errors.
- PASS - `NOVA_SharedCode` Release x64, 0 warnings, 0 errors.
- PASS - `NOVA_StandalonePlugin` Release x64, 0 warnings, 0 errors.

Static checks:

- PASS - `git diff --check`
  - Only existing LF-to-CRLF working-copy warnings were reported.
- PASS/WARN - `scripts/check-audio-thread-policy.ps1`
  - `status=WARN`
  - `activeFiles=32`
  - `activeRanges=38`
  - `failures=0`
  - `warnings=4`
  - No FAIL findings in active audio routes.

Audio validation:

- PASS - `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`
  - `results=136`
  - `passes=5758`
  - `failures=0`
  - `failingResults=0`
- PASS - `scripts/run-golden-audio-metrics.ps1`
  - Golden metrics passed against `docs/golden-metrics/p4-offline-qa-baseline.json`.
  - No golden baselines were updated.
  - No known failures were added.

RT profile:

- PASS/WARN - Debug x64
  - final artifact: `artifacts/rt-profile-debug-x64-report-p5g.json`
  - `total=16`
  - `pass=15`
  - `warn=1`
  - `fail=0`
  - WARN remains `sample_rate_96000`.
- PASS - Release x64
  - artifact: `artifacts/rt-profile-release-x64-report-p5g.json`
  - `total=16`
  - `pass=16`
  - `warn=0`
  - `fail=0`

Debug RT was repeated once because the first 96 kHz run was scheduler-sensitive; the final saved P5G artifact is the second run above.

## RT Profile Comparison Against P5F

Debug:

| Scenario | P5F | P5G |
| --- | ---: | ---: |
| Summary | 15 PASS / 1 WARN / 0 FAIL | 15 PASS / 1 WARN / 0 FAIL |
| `sample_rate_96000` maxBudgetRatio | 0.861075 | 0.961050 |
| `stress_block_32` maxBudgetRatio | 0.562500 | 0.508350 |

Release:

| Scenario | P5F | P5G |
| --- | ---: | ---: |
| Summary | 16 PASS / 0 WARN / 0 FAIL | 16 PASS / 0 WARN / 0 FAIL |
| `sample_rate_96000` maxBudgetRatio | 0.192825 | 0.216750 |
| `stress_block_32` maxBudgetRatio | 0.112650 | 0.129750 |

The RT status did not regress. Debug kept the same single expected 96 kHz warning class, and Release remained clean.

## Behavior Preservation Notes

- No DSP code changed.
- No routing code changed.
- No dry/wet code changed.
- No graph build code changed.
- No active graph ownership moved.
- No `activeGraphRaw` publication moved.
- No graph publish ordering changed.
- No retired graph grace period changed.
- No cleanup timing changed.
- No RuntimeParameterSnapshot, HealthMonitor, CpuMeter, AudioEngineCommandQueue, or GraphCommandApplier behavior changed.
- No UI, IDs, preset schema, golden baseline, or known-failure changes were made.

## Remaining Risks

- `GraphRetirementQueue` is intentionally passive and lock-free. Correct synchronization still depends on `AudioEngine` keeping cleanup and retire calls under the existing `activeOwnerLock`.
- `publishGraph` still mixes active-owner swap, raw pointer publication, latency update, and old graph retirement. This is deliberate for P5G but remains the central lifetime boundary.
- The helper is header-only and not listed in `NOVA.jucer`, matching the low-risk approach used by the previous P5 extractions.
- Debug RT remains scheduler-sensitive at 96 kHz.

## Recommendation For P5H

Keep P5H focused on one graph-lifecycle boundary only. If moving active graph ownership is the goal, make it an explicit `RuntimeGraphManager` phase with tests around publish order, raw pointer publication, grace period, and cleanup call sites. Do not combine that with GraphBuilder, RoutingMixer, DryWetMixer, diagnostics, or DSP changes.
