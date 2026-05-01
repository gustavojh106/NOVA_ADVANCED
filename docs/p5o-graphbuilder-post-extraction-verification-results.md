# P5O — GraphBuilder Post-Extraction Verification Results

## Scope

Post-extraction verification only (no GraphBuilder redesign, no `.cpp` split, no DSP/routing/dry-wet/lifecycle behavior changes).

## Files reviewed

- `Source/Core/Audio/GraphBuilder.h`
- `Source/Core/Audio/GraphRuntimeTypes.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `scripts/check-audio-thread-policy.ps1`

## Changes made in P5O

### 1) Direct GraphBuilder tests added

File: `Source/Core/AudioEngineTests.cpp`

Added:

- `GraphBuilder builds runtime with global processors and captured latency`
- `GraphBuilder preserves canonical zone order for mixed insertion`
- `GraphBuilder captures latency from rebuilt graph on latency-relevant chain`
- `GraphBuilder skips invalid pedal without fatal build failure`
- `GraphBuilder preserves ProcessorBase and TempoSyncable caches`

Covered risks:

- topology/global-node construction drift
- canonical zone-order connection drift
- latency capture drift after `prepareToPlay` + `rebuild`
- non-fatal skip behavior drift for invalid pedals
- ProcessorBase/TempoSyncable cache population drift

### 2) Policy scan contract checks extended

File: `scripts/check-audio-thread-policy.ps1`

Added/confirmed checks:

- `AudioEngine::process` does not call `buildGraphFromModelLocked`
- `AudioEngine::process` does not call `GraphBuilder::build`
- `GraphBuilder.h` does not include `AudioEngine.h`
- `AudioEngine::buildGraphFromModelLocked` wrapper contains `GraphBuildRequest` + `GraphBuilder` + `builder.build(...)`
- existing RuntimeGraphManager contracts remain checked (`getActiveRaw`, `publish`, `cleanupRetired`, `getActiveOwnerForControl`, atomic raw-load contract)

## Audio-thread contract confirmation

Confirmed in code + policy output:

- `AudioEngine::process` accesses active graph via `runtimeGraphs.getActiveRaw()` only.
- `AudioEngine::process` has no `buildGraphFromModelLocked` call.
- `AudioEngine::process` has no `GraphBuilder::build` call.
- no new lock/shared_ptr usage was introduced in `AudioEngine::process`.
- `GraphBuilder` remains control-path build logic and is not routed from audio thread.

## Behavior-preservation confirmation

Confirmed by review + tests:

- GraphBuilder keeps topology construction and canonical zone ordering (`Pre -> Amp -> FX -> Cabinet -> unknown`).
- runtime params are applied before `graph.prepareToPlay(...)` and `graph.rebuild()`.
- latency capture still occurs after `rebuild()` and remains clamped to `MAX_GRAPH_LATENCY_SAMPLES`.
- ProcessorBase and TempoSyncable caches are still populated during build.
- pedal creation/node-add failures still skip non-fatally with warnings.
- `publishGraph` / lifecycle ownership / RuntimeGraphManager / dry-wet path remain outside GraphBuilder.

## Validation executed

### Build

- `build-nova.ps1 -Target NOVA_SharedCode -Configuration Debug -Platform x64` PASS
- `build-nova.ps1 -Target NOVA_StandalonePlugin -Configuration Debug -Platform x64` PASS
- `build-nova.ps1 -Target NOVA_SharedCode -Configuration Release -Platform x64` PASS
- `build-nova.ps1 -Target NOVA_StandalonePlugin -Configuration Release -Platform x64` PASS

### Static diff hygiene

- `git diff --check` PASS (line-ending warnings only, no diff-check errors)

### Functional validation

- `run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240` PASS  
  - `results=153 passes=5920 failures=0 failingResults=0`
- `run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240` PASS
- `run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240` PASS  
  - `total=16 pass=16 warn=0 fail=0`
- `run-rt-profile-stability.ps1 -Configuration Release -Platform x64 -Runs 3 -CiMode -TimeoutSeconds 240 -ScenarioFilter "stress_block_32,sample_rate_44100,sample_rate_96000,overdrive_cleanamp_reverb_chain_nominal"` PASS
  - gate summary: `runsWithFail=0 runsWithWarn=0 runsBlocked=0`
- `check-audio-thread-policy.ps1` PASS/WARN non-blocking  
  - `summary.failures=0`
  - `summary.contractFailures=0`
  - `summary.contractChecks=10`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

### RT stability prioritized (Release, 3 runs, CiMode)

- `overdrive_cleanamp_reverb_chain_nominal`: pass/warn/fail `3/0/0`  
  - `maxBudgetRatio min/med/max = 0.1299 / 0.1326 / 0.1347`
- `stress_block_32`: `3/0/0`  
  - `maxBudgetRatio min/med/max = 0.1527 / 0.1598 / 0.1611`
- `sample_rate_44100`: `3/0/0`  
  - `maxBudgetRatio min/med/max = 0.1176 / 0.1243 / 0.1799`
- `sample_rate_96000`: `3/0/0`  
  - `maxBudgetRatio min/med/max = 0.2855 / 0.2910 / 0.3212`

## Risks remaining

- Policy script now enforces key GraphBuilder/audio-thread contracts, but still relies on signature/text matching; semantic drift not reflected in those markers could evade detection.
- GraphBuilder is still header-only; compile-time impact and include-surface growth remain a maintainability risk (not a behavior risk).
- Existing legacy policy WARNs remain (non-active-route legacy files), unchanged by P5O.

## Recommendation for P5P

Proceed with a narrow maintenance phase only if needed:

- keep runtime behavior unchanged
- optionally evaluate `GraphBuilder` header-size/compile-cost mitigation (possible `.cpp` split) in an isolated phase
- preserve current policy contract checks and P5N-A/P5O GraphBuilder tests as mandatory guardrails
