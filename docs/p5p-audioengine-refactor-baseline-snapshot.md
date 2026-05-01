# P5P - AudioEngine Refactor Baseline Snapshot

Date: 2026-04-30  
Scope: post-P5O technical snapshot only (no code changes)

## 1. Executive Summary

P5 delivered a conservative modularization of `AudioEngine` control-plane responsibilities without changing runtime audio behavior. The system is currently in a stable state with all baseline gates green and no evidence of functional regression in graph topology, latency lifecycle, or release-thread safety.

Current safety level is high for continuing refactor work, with two conditions:

1. Keep audio-thread contracts enforced by policy scan and tests.
2. Stage any remaining high-risk moves (routing/dry-wet/model-lock changes) behind dedicated pre-coverage.

Validation guardrails protecting this baseline:

- base validation suite
- golden metrics vs P4 baseline
- RT release single-run
- RT release stability multi-run (prioritized scenarios)
- audio-thread policy scanner contract checks
- fast quality gate wrapper

## 2. Extracted Modules

### 2.1 CpuMeter

- Responsibility: per-block timing and smoothed CPU telemetry.
- Files: `Source/Core/Audio/CpuMeter.h`
- Extracted from AudioEngine: process timing math, average/peak smoothing, CPU percent smoothing.
- Must NOT own: graph, routing, command/model state, logging policy decisions.
- Thread domain: audio thread write path (`beginBlock/endBlock`), lock-free atomic reads from control/UI.
- Critical invariants:
  - no locks in block timing updates
  - finite clamping on time and CPU metrics

### 2.2 HealthMonitor

- Responsibility: output sanitization/metering and recovery/silent-output action decisions.
- Files: `Source/Core/Audio/HealthMonitor.h`
- Extracted from AudioEngine: invalid/clipped sanitization, spike/near-clip counters, auto-heal and silent-output trigger logic.
- Must NOT own: graph rebuild execution, logging execution, graph publish/build.
- Thread domain: audio path for stats; control-plane consumes emitted actions.
- Critical invariants:
  - sanitize remains in audio block flow
  - monitor emits actions; engine applies actions
  - no topology mutation inside monitor

### 2.3 RuntimeParameterSnapshot

- Responsibility: atomic snapshot store/load for runtime global params + revision.
- Files: `Source/Core/Audio/RuntimeParameterSnapshot.h`
- Extracted from AudioEngine: atomic parameter mirror, output mix normalization, revision tracking.
- Must NOT own: graph build/publish or command semantics.
- Thread domain: control writes, audio/control reads.
- Critical invariants:
  - `store()` revision increment with release ordering
  - reads remain lock-free
  - normalized output mix access remains stable

### 2.4 AudioEngineCommandQueue

- Responsibility: enqueue/drain pending graph commands.
- Files: `Source/Core/Audio/AudioEngineCommandQueue.h`
- Extracted from AudioEngine: command deque + pending flag.
- Must NOT own: command interpretation, model mutation rules, rebuild policy.
- Thread domain: control thread enqueue/drain (not audio process path).
- Critical invariants:
  - deterministic FIFO drain
  - pending flag reflects queue state transition

### 2.5 GraphCommandApplier

- Responsibility: pure command-to-model mutation logic and result flags.
- Files: `Source/Core/Audio/GraphCommandApplier.h`
- Extracted from AudioEngine: add/remove/move/clear/bypass/engine-enabled/rebuild command semantics.
- Must NOT own: graph runtime, publish, routing, DSP processing.
- Thread domain: control thread only.
- Critical invariants:
  - command order preserved
  - topology vs params-only result flags preserved
  - no hidden rebuild side effects

### 2.6 GraphRetirementQueue

- Responsibility: retired graph lifetime queue and bounded cleanup.
- Files: `Source/Core/Audio/GraphRetirementQueue.h`
- Extracted from AudioEngine: retire list, release threshold cleanup, max-retired bound.
- Must NOT own: active graph pointer or publish ordering.
- Thread domain: control thread only.
- Critical invariants:
  - release condition `releaseAfterAudioBlock <= currentAudioBlock`
  - bounded memory policy (`kMaxRetiredGraphs = 8`)

### 2.7 RuntimeGraphManager

- Responsibility: active graph ownership, raw pointer publication, retired cleanup delegation.
- Files: `Source/Core/Audio/RuntimeGraphManager.h`
- Extracted from AudioEngine: `activeOwnerLock`, `activeOwner`, `activeGraphRaw`, publish swap, retired queue integration.
- Must NOT own: graph build, command queue, dry/wet mixing.
- Thread domain:
  - audio thread: `getActiveRaw()` only
  - control thread: `publish/cleanupRetired/getActiveOwnerForControl/clear`
- Critical invariants:
  - `getActiveRaw()` is acquire atomic load only
  - publish order preserved (swap owner -> publish raw -> callback latency -> retire old)
  - grace period preserved (`+8` blocks)

### 2.8 DiagnosticsManager

- Responsibility: diagnostic and profiling string formatting from snapshots/data DTOs.
- Files: `Source/Core/Audio/DiagnosticsManager.h`
- Extracted from AudioEngine: report formatting helpers, chain descriptions, profiling text formatting.
- Must NOT own: graph state mutation, audio processing logic.
- Thread domain: control/diagnostics path only.
- Critical invariants:
  - no calls from `AudioEngine::process`
  - diagnostic field coverage remains stable for tests

### 2.9 GraphBuilder

- Responsibility: build `GraphRuntime` from request snapshot.
- Files:
  - `Source/Core/Audio/GraphRuntimeTypes.h`
  - `Source/Core/Audio/GraphBuilder.h`
- Extracted from AudioEngine: graph/node construction, chain build, zone ordering, runtime param apply, prepare/rebuild, latency capture.
- Must NOT own: runtime lifecycle publish/retire, command queue, process path.
- Thread domain: control-thread build path only.
- Critical invariants:
  - canonical zone order: `Pre -> Amp -> FX -> Cabinet -> unknown`
  - initial runtime params applied before `prepareToPlay/rebuild`
  - latency captured after `rebuild` and clamped
  - invalid pedal/node add remains non-fatal skip with warning

## 3. What Still Remains in AudioEngine

Primary remaining responsibilities:

- process orchestration (`process`, diagnostics mode decimation decisions)
- sample-accurate dry/wet pipeline:
  - `processWithSampleAccurateDryWet`
  - `mixWetDrySampleAccurate`
  - `copyDryThroughLatency`
  - dry-delay buffer lifecycle
- global routing behavior between input, line A/B, output chain
- `applyPedalBypassToActiveGraph` orchestration and in-place bypass behavior
- `flushPendingGraphCommands` orchestration (queue drain + applier result handling + rebuild trigger decisions)
- `buildGraphFromModelLocked` compatibility wrapper over `GraphBuilder`
- `publishGraph` wrapper over `RuntimeGraphManager`
- tuner flow (`setTunerEnabled`, push/process/readbacks, reference pitch)
- async/run-thread orchestration (`run`, `handleAsyncUpdate`, synchronize points)
- plugin/session bridge surface through public APIs (`updateGlobalParams`, command entrypoints, state sync hooks)

## 4. Current Validation Status

Baseline status after P5O:

- Base validation: `results=153 passes=5920 failures=0 failingResults=0` (PASS)
- Golden metrics vs P4 baseline: PASS
- RT Release single-run: PASS `16/16/0/0`
- RT stability Release prioritized: PASS
- Policy scan: `failures=0`, `contractFailures=0` (overall WARN only due to legacy non-blocking items)
- Wrapper fast gate: PASS

No golden baselines were updated.  
No new known failures were introduced.

## 5. Protected Invariants

- no graph build in audio thread
- audio thread active graph access via atomic `getActiveRaw()` contract
- no `shared_ptr` copy/owner lock in `AudioEngine::process`
- `GraphBuilder` not invoked from `process`
- sample-accurate dry/wet behavior unchanged
- zone order canonicalization protected by build + tests
- latency capture point protected (post-rebuild)
- ProcessorBase/TempoSyncable caches protected
- runtime param application order protected
- graph lifecycle publish/retire contracts protected

## 6. Remaining Risks

- legacy policy WARN items still present (non-blocking, but technical debt)
- `GraphBuilder` is header-only (include-surface/compile-cost risk)
- Debug RT jitter remains expected noise under pressure scenarios
- `RoutingMixer` / `DryWetMixer` are not yet extracted
- `applyPedalBypassToActiveGraph` still concentrated in `AudioEngine`
- model lock scope is still broad in command/rebuild path
- cumulative compile-cost growth from header-only modules
- external manual DAW smoke coverage is still desirable for release confidence

## 7. Recommended Next Steps (Prioritized)

### Option comparison

- A. RoutingMixer/DryWetMixer plan: high value, medium-high risk.
- B. DryWetMixer tests before extraction: very high value, low-medium risk, enables safe A.
- C. GraphBuilder `.cpp` split: maintainability/compile-time value, low behavior value.
- D. DAW smoke-test checklist: high release confidence value, low risk.
- E. Tonal QA/presets: high product confidence value, but not structural refactor progress.

### Recommended order

1. **B first**: add deterministic dry/wet and routing pre-extraction coverage.
2. **D in parallel**: define/lock DAW smoke checklist for external confidence.
3. **A next**: produce strict extraction plan for RoutingMixer/DryWetMixer using B+D guardrails.
4. **C optional after A planning**: only if compile cost or include churn is problematic.
5. **E staged around release windows**: tonal/preset QA as a non-structural confidence layer.

Rationale: this sequence keeps behavior risk low while preparing the two most sensitive remaining runtime areas with measurable guardrails first.

## 8. Suggested Prompt for Next Phase

Suggested next phase prompt:

> Tarea: P6A - DryWet and Routing Pre-Extraction Safety Coverage (no extraction yet).  
> Add deterministic tests and policy checks for sample-accurate dry/wet and routing invariants before any RoutingMixer/DryWetMixer extraction plan. No DSP behavior changes, no topology changes, no baseline updates.

