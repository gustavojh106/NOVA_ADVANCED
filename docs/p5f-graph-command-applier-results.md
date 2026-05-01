# P5F - GraphCommandApplier Extraction Results

Date: 2026-04-29

## Summary

P5F extracted the model/control-plane command application logic from `AudioEngine` into a small header-only helper:

- `Source/Core/Audio/GraphCommandApplier.h`

The extraction is intentionally conservative. `GraphCommand` remains private to `AudioEngine`, graph build/swap ownership remains in `AudioEngine`, and the helper does not touch DSP, routing, dry/wet, runtime params, HealthMonitor, CpuMeter, parameter IDs, preset schema, or golden baselines.

## Files Modified

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/GraphCommandApplier.h`
- `docs/p5f-graph-command-applier-results.md`
- Validation artifacts:
  - `artifacts/rt-profile-debug-x64-report-p5f.json`
  - `artifacts/rt-profile-release-x64-report-p5f.json`
  - `artifacts/audio-thread-policy-scan.txt`
  - `artifacts/audio-thread-policy-scan.json`
  - `artifacts/p4-offline-qa-report.txt`

## Module Created

`Nova::Audio::GraphCommandApplier` is a header-only template helper. It receives command/model types through template parameters and a small traits adapter, avoiding a direct dependency from the new module back into `AudioEngine`.

`AudioEngine` now defines a private `GraphCommandTraits` adapter and a private alias:

```cpp
using ModelCommandApplier = Nova::Audio::GraphCommandApplier<GraphCommand,
    ChainNodeSpec,
    std::vector<ChainNodeSpec>,
    GraphCommandTraits>;
```

This keeps `GraphCommand`, `GraphCommandType`, and `ChainNodeSpec` ownership inside `AudioEngine` while moving the command interpretation and model-list mutation out of the `.cpp`.

## Logic Moved

Moved from `AudioEngine::applyGraphCommandToModel` into `GraphCommandApplier::apply`:

- Add pedal model-node creation and insertion.
- Remove pedal model-node erase.
- Move pedal model-node erase/insert, including the existing `from < to` index adjustment and `juce::jlimit` clamp.
- Clear all model chains.
- Set model bypass flag for a pedal.
- Detect params-only bypass updates.
- Detect topology-changing commands.
- Detect explicit rebuild commands.
- Detect engine-enable commands as a result flag for `AudioEngine` to handle.

The old `AudioEngine::applyGraphCommandToModel` method was removed.

## What Remains In AudioEngine

`AudioEngine` still owns all orchestration and graph lifecycle behavior:

- Private `GraphCommand` and `GraphCommandType`.
- `AudioEngineCommandQueue` drain and command-order loop.
- `modelLock` acquisition and release.
- `applyPedalBypassToActiveGraph`.
- `isEngineOn` exchange and `audioRuntimeResetRequested` behavior.
- `graphResetRequested` consumption.
- `buildGraphFromModelLocked`.
- `publishGraph`.
- runtime parameter application after command drain.
- retired graph cleanup.
- public pedal API and command enqueue semantics.

No graph build/swap internals were moved.

## Command Order Preservation

`flushPendingGraphCommands` still drains the command queue once into a deque and iterates with:

```cpp
for (const auto& cmd : commands)
```

Each command is passed to `ModelCommandApplier::apply` exactly once in that same loop. Side effects that must remain in `AudioEngine` (`applyPedalBypassToActiveGraph`, engine enable/disable atomics) are still executed immediately for the current command before the next command is processed.

No coalescing, reordering, filtering, or batching behavior was added.

## flushPendingGraphCommands Preservation

The overall flow remains unchanged:

1. Early return when there are no pending commands and no graph reset.
2. Runtime params are applied on the early path when revisions differ.
3. Pending commands are drained from `AudioEngineCommandQueue`.
4. Existing `modelLock` is acquired.
5. Commands are applied in order.
6. `graphResetRequested` is consumed inside the same model-lock boundary.
7. Rebuild/publish happens only when the existing topology/explicit-reset decision says so.
8. Runtime params are applied to the active graph after command drain.
9. Retired graph cleanup still only runs when topology changed.

The helper does not call `flushPendingGraphCommands` and is not called from `AudioEngine::process`.

## Rebuild Decision Preservation

The helper returns result flags instead of directly mutating outer booleans:

- `topologyChanged = true` for add/remove/move/clear/rebuild.
- `explicitRebuild = true` for explicit rebuild commands.
- `paramsOnly = true` for valid bypass changes.
- `bypassChanged = true` only when the model bypass flag was actually updated.
- `engineStateChanged = true` for engine-enable commands.

`AudioEngine` still handles the engine-enable special case exactly where graph/audio-plane state belongs:

- `isEngineOn.exchange(cmd.flag, std::memory_order_acq_rel)`
- if enabling from off to on, set `audioRuntimeResetRequested`
- mark `topologyChanged` to rebuild a fresh graph before processing

`graphResetRequested.exchange(false, std::memory_order_acq_rel)` remains in `AudioEngine`.

## modelLock / Control-Plane Boundary

`GraphCommandApplier::apply` is called only inside the existing `juce::ScopedLock lock(controlPlane.modelLock)` block in `flushPendingGraphCommands`.

The helper introduces no new locks and no audio-thread call sites. It does not touch `activeGraphRaw`, `activeOwner`, retired graphs, runtime params, session state, logging, `juce::String`, or host notification APIs.

The moved vector operations are the same model mutations that already existed in `AudioEngine`; no additional allocation behavior was introduced beyond those pre-existing add/move/clear semantics.

## Validation Results

Builds:

- PASS - `NOVA_SharedCode` Debug x64
- PASS - `NOVA_StandalonePlugin` Debug x64
- PASS - `NOVA_SharedCode` Release x64
- PASS - `NOVA_StandalonePlugin` Release x64

Static checks:

- PASS - `git diff --check`
  - Only existing CRLF conversion warnings were reported.
- PASS/WARN - `scripts/check-audio-thread-policy.ps1`
  - `status=WARN`
  - `summary.failures=0`
  - `summary.warnings=4`
  - No FAIL findings in active audio routes.
  - Warnings remain the existing legacy non-blocking findings.

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
  - `total=16`
  - `pass=15`
  - `warn=1`
  - `fail=0`
  - WARN remains `sample_rate_96000`.
- PASS - Release x64
  - `total=16`
  - `pass=16`
  - `warn=0`
  - `fail=0`

## RT Profile Comparison Against P5E

Debug:

| Scenario | P5E | P5F |
| --- | ---: | ---: |
| Summary | 15 PASS / 1 WARN / 0 FAIL | 15 PASS / 1 WARN / 0 FAIL |
| `sample_rate_96000` maxBudgetRatio | 1.392600 | 0.861075 |
| `stress_block_32` maxBudgetRatio | 0.732600 | 0.562500 |

Release:

| Scenario | P5E | P5F |
| --- | ---: | ---: |
| Summary | 16 PASS / 0 WARN / 0 FAIL | 16 PASS / 0 WARN / 0 FAIL |
| `sample_rate_96000` maxBudgetRatio | 0.286950 | 0.192825 |
| `stress_block_32` maxBudgetRatio | 0.264900 | 0.112650 |

The RT status did not regress. Release remains clean.

## Behavior Preservation Notes

- No DSP code changed.
- No routing code changed.
- No dry/wet code changed.
- No graph build/swap internals changed.
- No active graph lifetime behavior changed.
- No RuntimeParameterSnapshot behavior changed.
- No HealthMonitor or CpuMeter behavior changed.
- No UI, IDs, preset schema, or host notification behavior changed.
- No golden baselines were updated.
- No known failures were added.

## Remaining Risks

- `GraphCommand` remains private to `AudioEngine`, so the new helper uses a traits adapter rather than a concrete shared command type. This is deliberate for P5F but means the command model is not yet independently reusable.
- `flushPendingGraphCommands` still coordinates model mutation, graph rebuild decisions, graph publication, runtime param application, and cleanup. This keeps behavior stable, but the method remains the main control-plane orchestration point.
- The helper still operates on `std::vector<ChainNodeSpec>` model lists, so add/move/clear retain the same model-container behavior and potential reallocations as before.

## Recommendation For P5G

Keep P5G narrow. The next low-risk extraction should isolate graph retirement or graph rebuild orchestration only if it can keep `activeOwner`, `activeGraphRaw`, publish order, and audio-thread lifetime semantics unchanged. Do not combine RuntimeGraphManager, GraphBuilder, RoutingMixer, DryWetMixer, or graph retirement into one phase.
