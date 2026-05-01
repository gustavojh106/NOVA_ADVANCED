# P5H - Graph Lifecycle Coverage Results

Date: 2026-04-30

## Summary

P5H added targeted graph lifecycle coverage and created a concrete `RuntimeGraphManager` design without implementing the manager.

No active graph ownership was moved. `activeOwner`, `activeGraphRaw`, `activeOwnerLock`, `publishGraph`, `buildGraphFromModelLocked`, graph publication order, dry delay latency update, and graph build behavior remain in `AudioEngine`.

No DSP, tone, routing, dry/wet, latency behavior, runtime params, HealthMonitor, CpuMeter, AudioEngineCommandQueue, GraphCommandApplier, preset schema, parameter IDs, golden baselines, or known failures were changed.

## Files Modified

- `Source/Core/AudioEngineTests.cpp`
- `docs/p5h-runtime-graph-manager-design.md`
- `docs/p5h-graph-lifecycle-coverage-results.md`
- Validation artifacts:
  - `audio-base-test-report.txt`
  - `artifacts/rt-profile-debug-x64-report-p5h.json`
  - `artifacts/rt-profile-release-x64-report-p5h.json`
  - `artifacts/audio-thread-policy-scan.txt`
  - `artifacts/audio-thread-policy-scan.json`
  - `artifacts/p4-offline-qa-report.txt`

## Tests Added Or Reinforced

Added:

- `GraphRetirementQueue preserves grace period and bounded cleanup`
  - verifies release-after-block behavior
  - verifies max retired graph bound stays at 8
  - verifies `clear()` releases retained graph owners

- `AudioEngine publishes an active graph immediately after prepare`
  - verifies public diagnostics report `activeGraph=true` after `prepare`
  - verifies disabled engine state does not imply null active graph ownership

- `AudioEngine clean path remains stable after topology swaps`
  - add/remove graph swap around the clean path
  - verifies finite output and stable clean-path output before/after swap

- `AudioEngine add and remove during deterministic processing stays finite`
  - deterministic block loop with add/remove/clear operations between process calls
  - verifies no crash, finite output, bounded peak, and final clean topology

- `AudioEngine ClearAll followed by add rebuilds to the new topology`
  - verifies clear removes runtime topology
  - verifies a later add publishes the expected Reverb processor
  - verifies the rebuilt graph processes finite output

- `AudioEngine preserves command order across batched topology edits`
  - verifies add/insert/move command ordering through public `getNodes`

Reinforced:

- `AudioEngine rebuilds graph latency when bypass changes node latency`
  - existing latency coverage now also verifies bypass/unbypass keeps the same processor pointer
  - this pins params-only bypass behavior to in-place active graph rebuild rather than topology publication

Existing relevant coverage kept:

- clean single-line path
- disabled engine dry path
- dual parallel unity behavior
- dry-only wet-path bypass
- engine disable/re-enable recovery
- re-enable refresh after released pedal processors
- diagnostic report reflects queued topology
- pre-prepare topology survives `prepare`

## Tests Not Added

- Direct `activeGraphRaw` null checks were not added because the raw pointer remains private. P5H uses `buildDiagnosticReport()` and public graph access instead.
- A direct generation-counter or publish-count test was not added because exposing that would widen internals solely for tests. The bypass test uses processor pointer stability to cover the important params-only no-new-topology behavior.
- A true concurrent add/remove while the audio callback is running was not added. P5H avoids sleeps and timing-dependent tests; the added deterministic interleaving test exercises graph swaps between blocks without scheduler fragility.
- A direct "cleanup is never called on the audio thread" runtime test was not added because it would require instrumentation in production paths. This remains covered by call-site review plus `check-audio-thread-policy.ps1`.
- Exact output matching after graph rebuild was avoided where conditioning state could make it fragile. Tests use finite, bounded, topology, latency, pointer-stability, and before/after clean-path invariants instead.

## Design Created

Created:

- `docs/p5h-runtime-graph-manager-design.md`

The design specifies:

- current ownership state for `activeOwner`, `activeGraphRaw`, `activeOwnerLock`, `publishGraph`, `cleanupRetiredGraphs`, `GraphRetirementQueue`, `audioBlockCounter`, latency update, and dry delay latency update
- proposed `RuntimeGraphManager` ownership boundary
- state it should and should not own
- minimal API
- a no-`std::function` publish callback design to preserve order without type-erasure overhead
- audio-thread and control-thread contracts
- publish order invariants
- retired graph grace period invariants
- dry delay latency preservation
- P5I implementation sequence
- P5I rollback criteria

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
  - `legacyWarnings=4`
  - No FAIL findings in active audio routes.

Audio validation:

- PASS - `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`
  - `results=142`
  - `passes=5814`
  - `failures=0`
  - `failingResults=0`
  - The count increased from P5G because P5H added graph lifecycle unit tests.

Golden metrics:

- PASS - `scripts/run-golden-audio-metrics.ps1`
  - Golden metrics passed against `docs/golden-metrics/p4-offline-qa-baseline.json`.
  - No golden baselines were updated.
  - No known failures were added.

RT profile:

- PASS/WARN - Debug x64
  - artifact: `artifacts/rt-profile-debug-x64-report-p5h.json`
  - `total=16`
  - `pass=15`
  - `warn=1`
  - `fail=0`
  - WARN remains `sample_rate_96000`.
  - `sample_rate_96000.maxBudgetRatio=0.864525`

- PASS - Release x64
  - artifact: `artifacts/rt-profile-release-x64-report-p5h.json`
  - `total=16`
  - `pass=16`
  - `warn=0`
  - `fail=0`
  - `sample_rate_96000.maxBudgetRatio=0.196575`

## RT Profile Comparison Against P5G

Debug:

| Scenario | P5G | P5H |
| --- | ---: | ---: |
| Summary | 15 PASS / 1 WARN / 0 FAIL | 15 PASS / 1 WARN / 0 FAIL |
| `sample_rate_96000` maxBudgetRatio | 0.961050 | 0.864525 |
| `stress_block_32` maxBudgetRatio | 0.508350 | 0.508050 |

Release:

| Scenario | P5G | P5H |
| --- | ---: | ---: |
| Summary | 16 PASS / 0 WARN / 0 FAIL | 16 PASS / 0 WARN / 0 FAIL |
| `sample_rate_96000` maxBudgetRatio | 0.216750 | 0.196575 |
| `stress_block_32` maxBudgetRatio | 0.129750 | 0.115650 |

The RT status did not regress. Release remains clean, and Debug keeps the same single expected 96 kHz warning class.

## Risks Found

- Some exact dry-input comparisons are too brittle after graph rebuild because global conditioning state can differ slightly after topology reinitialization. P5H uses before/after clean-path comparison instead.
- Public APIs do not expose active raw pointer or graph generation. This is good encapsulation, but it limits direct unit coverage. P5I should avoid adding public test-only internals unless a clear invariant cannot be covered otherwise.
- Deterministic interleaving is safer than thread-race tests for unit coverage, but it does not replace stress testing in a host or future targeted concurrency harness.
- `RuntimeGraphManager` must preserve the current latency update order. A return-value-only publish API would be easier, but could accidentally move `updateDryDelayLatency` outside the current publish sequence.

## Recommendation For P5I

Proceed with a narrow `RuntimeGraphManager` implementation only.

P5I should:

- create `Source/Core/Audio/RuntimeGraphManager.h`
- move only active graph owner state, raw pointer publication, active owner lock, retired graph queue, and active latency mirror
- keep `AudioEngine::publishGraph` as a wrapper initially
- preserve publish order with a templated/no-allocation latency callback
- keep `audioBlockCounter` in `AudioEngine`
- keep graph build, command flush, bypass in-place rebuild, routing, dry/wet, runtime params, HealthMonitor, CpuMeter, diagnostics, and tuner out of the manager
- run the same validation matrix used in P5H

Do not combine P5I with GraphBuilder, RoutingMixer, DryWetMixer, DiagnosticsManager, or DSP changes.
