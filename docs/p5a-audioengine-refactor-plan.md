# P5A AudioEngine Refactor Plan

Fecha: 2026-04-29

## 1. Resumen ejecutivo

P0-P4C cerraron la etapa de seguridad y baseline: audio validation esta verde, golden metrics P4 no tienen coverage gaps, el profiling Release x64 esta en `16/16/0/0`, y el policy scan no encuentra FAIL en rutas activas. Ese es el momento correcto para planear el refactor de `AudioEngine`: ya existe una baseline que puede detectar cambios accidentales de routing, dry/wet, limiter, DC cleanup, latencia, regresiones P1/P2 y degradacion RT.

El objetivo de P5 no debe ser mejorar tono ni redisenar pedales. El objetivo es reducir riesgo estructural: hoy `AudioEngine` concentra graph ownership, graph build, comando/control plane, audio path, dry/wet, latency, metering, health recovery, tuner, diagnostics y profiling. Esa mezcla hace que cambios pequenos puedan tocar areas con contratos distintos de thread, lifetime y validacion.

La estrategia recomendada es una descomposicion incremental, con extracciones primero mecanicas y de bajo riesgo, dejando graph ownership/build para fases posteriores. Cada fase debe compilar y validar con la matriz P4C antes de continuar. No se debe mover comportamiento DSP ni cambiar output esperado salvo una razon explicitamente documentada y aprobada con update de golden metrics.

Nota de paths: el brief menciona `Source/Model/ChainModel.h` y `Source/State/SessionStore.h` / `SessionCoordinator.h`. En el repo actual esos paths no existen. Los equivalentes activos revisados son `Source/Core/PluginStateModel.h`, `Source/Core/SessionStore.h` y `Source/Core/SessionCoordinator.h`.

Fuentes revisadas para este plan:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/PluginProcessor.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `Source/Core/OfflineQADiagnostics.h`
- `Source/Core/DSP/Global/InputChain.h`
- `Source/Core/DSP/Global/ChannelStrip.h`
- `Source/Core/DSP/Global/OutputChain.h`
- `Source/Effects/Pedals/Base/ProcessorBase.h`
- `Source/Core/PluginStateModel.h`
- `Source/Core/SessionStore.h`
- `Source/Core/SessionCoordinator.h`
- `docs/p4b-rt-profiling-results.md`
- `docs/p4c-rt-policy-hardening.md`
- `docs/p4c-rt-policy-and-release-profile-results.md`

## 2. Responsabilidades actuales de AudioEngine

### Public surface

`Source/Core/AudioEngine.h` expone actualmente:

- Ciclo de vida: `prepare`, `process`, destructor/thread shutdown.
- Mutacion de graph: `addPedal`, `removePedal`, `movePedal`, `clearAll`, `setPedalBypassed`, `setEngineEnabled`, `synchronizeProcessingState`.
- Runtime params: `RuntimeGlobalParams`, `updateGlobalParams`, `updateMixer`.
- Lectura de runtime: `getNodes`, `getProcessorForPedal`, meters, latency, diagnostics report, output-chain debug snapshot.
- Diagnostics/profiling: `DiagnosticsMode`, `runRealtimeProfilingSuite`, `formatProfilingResults`.
- Tuner: enable, pitch, clarity, RMS, reference pitch, tuning offset.
- Thread/control loop: inherits `juce::Thread` and `juce::AsyncUpdater`.

### Graph ownership

`AudioEngine::GraphRuntime` owns:

- `std::unique_ptr<juce::AudioProcessorGraph> graph`.
- IO nodes, `InputChainProcessor`, `ChannelStripProcessor` A/B, `OutputChainProcessor`.
- Runtime chain slots for Line A and Line B.
- Cached processor pointers: `ProcessorBase*`, `TempoSyncable*`.
- Sample rate, block size, IO count, generation, latency and applied param revision.

`AudioEngine::ControlPlane` owns:

- The active graph owner (`std::shared_ptr<GraphRuntime> activeOwner`).
- Retired graphs with grace block counters.
- Model chains used for future graph builds.
- Command queue and locks.

`AudioEngine::AudioPlane` owns:

- Raw active graph pointer for audio thread access.
- Audio block counter and engine/tuner state atomics.
- Scratch buffers and dry delay used by global dry/wet.
- Realtime meters and health flags.
- Tuner service.

### Graph build

`buildGraphFromModelLocked` currently:

- Allocates and prepares a new `juce::AudioProcessorGraph`.
- Creates IO nodes and global processors.
- Instantiates pedal processors through `PedalRegistry::createPedal`.
- Builds `ChainRuntimeSlot` caches.
- Applies bypass state and tempo-cache pointers.
- Connects InputChain -> ordered pedal chain -> ChannelStrip for each line.
- Connects strips to OutputChain and OutputChain to graph output.
- Applies runtime params, prepares graph, rebuilds graph and captures graph latency.

This is control-thread work and must remain outside `process`.

### Graph swapping

`publishGraph`:

- Stores the new owner under `activeOwnerLock`.
- Publishes `activeGraphRaw` atomically for the audio path.
- Updates dry/wet latency compensation.
- Moves the previous owner into `retiredGraphs` with a block-based grace period.

`cleanupRetiredGraphs`:

- Releases retired graphs after enough audio blocks.
- Bounds memory if audio is stopped.

### Retired graph cleanup

Retirement is coupled to `audioBlockCounter`. This is correct for avoiding use-after-free of the raw pointer, but it makes graph lifetime and process timing share state. Future extraction should keep this contract explicit and tested.

### Routing LineA/LineB/Dual

Routing is split between graph topology and runtime params:

- Topology always connects both line strips into `OutputChainProcessor`.
- `applyRuntimeParamsToGraph` mutes Line A or Line B by setting `ChannelStripProcessor` gain to zero depending on `switchMode`.
- Dual parallel applies `kParallelGainComp = 0.5f` to both active strips.
- Per-line gain/pan/width are pushed to each strip.

### Dry/wet global

Global dry/wet is implemented in `AudioEngine` rather than in `OutputChain`:

- `SampleAccurateRamp wetMixRamp` lives in `AudioPlane`.
- Dry copy uses `dryScratch`.
- Latency-aligned dry path uses `dryDelay` and `delayedDryScratch`.
- `processWithSampleAccurateDryWet` handles dry endpoint, wet endpoint and sample-accurate crossmix.

This is a critical behavior and must be preserved sample-for-sample as much as possible.

### Input/output processors

Global processor nodes are instantiated by graph build:

- `InputChainProcessor`: input gain, force mono, gate, input conditioning, single-jack handling.
- `ChannelStripProcessor`: per-line gain/pan/width and telemetry.
- `OutputChainProcessor`: output gain, DC cleanup, lookahead limiter, soft ceiling and debug snapshot.

`AudioEngine` owns their graph placement and parameter push, but not their DSP internals.

### Latency calculation

Latency currently comes from `graph.getLatencySamples()` after `graph.rebuild()`:

- `GraphRuntime::latencySamples` stores the clamped graph latency.
- `getLatencyNumSamples()` returns active runtime latency.
- `updateDryDelayLatency()` mirrors latency into `AudioPlane::currentDryLatencySamples`.
- `OutputChainProcessor` reports limiter lookahead latency.
- `ProcessorBase` handles bypass latency semantics inside individual pedals.

### Tuner tap

When tuner is enabled:

- `AudioEngine::process` pushes the incoming buffer into `TunerService`.
- The buffer is cleared instead of running graph processing.
- The engine thread calls `tunerService.process()` more frequently while tuner is enabled.
- Public getters read pitch/clarity/RMS/reference pitch.

### CPU/process-time meter

`AudioEngine::process` measures elapsed block time with `juce::Time::getMillisecondCounterHiRes()`.

`updateRealtimeTimingMeters` updates:

- last process time
- moving average process time
- decayed peak process time
- smoothed CPU load

This is low overhead but still embedded in the main process function.

### Diagnostics

Diagnostics are spread across:

- `DiagnosticsMode`.
- `buildDiagnosticReport`.
- Chain description helpers.
- OutputChain debug snapshot getter.
- Realtime profiling suite.
- Health meters stored in `AudioPlane`.
- `OfflineQADiagnostics` and scripts that consume public `AudioEngine` APIs.

### Health monitor

`sanitizeAndMeterOutput` and `handleHealthAfterBlock`:

- Sanitize NaN/Inf.
- Clamp hard limit violations.
- Count invalid, clipped, near-clip and click-spike samples.
- Detect repeated corrupt blocks and request graph reset.
- Detect sustained input-active/output-silent state.
- Set deferred logging flags consumed by the engine thread.

### Recovery/auto-heal

Auto-heal uses:

- `graphResetRequested`.
- `pendingAutoHealLog`.
- `autoHealCount`.
- `consecutiveCorruptBlocks`.
- `recoveryCooldownBlocks`.
- `audioRuntimeResetRequested`.

The rebuild itself is control-thread work through `flushPendingGraphCommands`.

### Command queue/control plane

Command flow:

- Public methods create `GraphCommand`.
- `enqueueGraphCommand` pushes into `pendingCommands` under `commandLock`.
- Message thread or engine thread calls `flushPendingGraphCommands`.
- Commands mutate model chains under `modelLock`.
- Topology changes rebuild/publish a new graph.
- Params-only changes update active graph where possible.

### Runtime params

Runtime params exist in multiple forms:

- `AudioEngine::RuntimeGlobalParams`.
- `AudioEngine::RuntimeParameterAtomics`.
- `SessionStore::RuntimeGlobalParamAtomics`.
- `NOVAAudioProcessor::RuntimeGlobalParamAtomics`.

`PluginProcessor` polls host transport every 8 blocks after P4C and pushes snapshots into `AudioEngine`.

### Interaction with PluginProcessor

`PluginProcessor`:

- Owns host parameters and `SessionCoordinator`.
- Mirrors parameter changes to `SessionStore`.
- Calls `refreshEngineEnabledIfNeeded` and `refreshEngineGlobalParamsIfNeeded` from `processBlock`.
- Calls `audioEngine.process`.
- Calls session persistence/rebuild APIs on preset changes.
- Exposes CPU/meter/tuner data to UI.

### Telemetry/debug snapshots

Telemetry currently comes from:

- `buildDiagnosticReport`.
- `getOutputChainDebugSnapshot`.
- Public meters.
- RT profile reports through `OfflineQADiagnostics`.
- Deferred logs in `AudioEngine::run`.

## 3. Problemas de diseno actuales

### Clase demasiado grande

`AudioEngine` is currently the runtime facade, graph builder, graph owner, command queue, audio processor, mixer, health monitor, diagnostics producer, tuner bridge and profiling host. This makes ownership boundaries difficult to reason about.

### Mezcla de graph management + DSP + diagnostics + control plane

The same class contains:

- Locking control-plane code.
- Audio-thread code with strict RT policy.
- Allocation-heavy graph construction.
- Lightweight diagnostics that are allowed on audio thread.
- String/report/logging diagnostics that must stay off audio thread.

This raises the chance that a future change accidentally moves a lock, string, allocation or graph rebuild into the audio path.

### Puntos dificiles de testear

Hard-to-isolate areas:

- `processWithSampleAccurateDryWet` depends on `AudioPlane` scratch/delay state.
- Graph retirement depends on block counters and owner locks.
- `applyRuntimeParamsToGraph` mixes input/output params, routing mode, strip params and tempo sync.
- Health monitor both mutates audio and schedules recovery.
- `buildDiagnosticReport` reads control and audio state at once.
- `runRealtimeProfilingSuite` mutates engine preparation and state.

### Puntos que aumentan riesgo de regresion

High-risk edges:

- Dry/wet at endpoint transitions (`0`, `100`, smoothing in between).
- Latency changes after bypass or graph rebuild.
- Retired graph lifetime around active raw pointer swaps.
- `graph.rebuild()` after bypass changes.
- Dual parallel gain compensation.
- Tuner mode clearing audio.
- Silent-output auto-heal false positives.
- Host transport snapshots becoming stale or too expensive.

### Areas que no conviene tocar todavia

Do not start P5 with:

- `GraphBuilder` extraction.
- `RuntimeGraphManager` ownership swap.
- Routing rewrites.
- Dry/wet DSP behavior changes.
- OutputChain/InputChain DSP internals.
- Session schema or state canonicalization.
- Pedal registry/legacy cleanup.

These are valid future phases, but not first extraction targets.

## 4. Propuesta de modulos

The target architecture keeps `AudioEngine` as a small facade and moves implementation behind focused modules. New source files must be added through `NOVA.jucer` and regenerated project files in implementation phases.

### AudioEngine

Responsibility:

- Public API facade for `PluginProcessor`, session persistence and tests.
- Owns module instances and wires them.
- Keeps behavior-compatible public methods during P5.

Suggested files:

- Existing `Source/Core/AudioEngine.h`
- Existing `Source/Core/AudioEngine.cpp`

Minimal public API:

- Keep current public API initially.
- Later expose only lifecycle, process, graph commands, runtime params, meters/tuner/diagnostics getters.

Owns:

- Module instances.
- Cross-module orchestration.

Must not own long term:

- Raw graph construction details.
- Dry delay buffers.
- Health state internals.
- CPU meter internals.
- Diagnostic string building internals.

Thread:

- Called by audio thread and control/message threads.

RT risks:

- Facade must not introduce hidden locks or allocations in `process`.

Tests:

- Entire P4C matrix per phase.

### RuntimeGraphManager

Responsibility:

- Own active graph owner, raw graph publication, retired graph queue and graph generation.
- Provide a stable audio-thread view of active graph.

Suggested files:

- `Source/Core/Audio/RuntimeGraphManager.h`
- `Source/Core/Audio/RuntimeGraphManager.cpp`

Minimal public API:

```cpp
struct RuntimeGraphView { GraphRuntime* raw = nullptr; };
void publish(std::shared_ptr<GraphRuntime> graph, uint64_t audioBlock);
GraphRuntime* getActiveRaw() const noexcept;
std::shared_ptr<GraphRuntime> getActiveOwnerForControl() const;
void cleanup(uint64_t audioBlock);
int getLatencySamples() const noexcept;
```

Owns:

- `activeOwner`
- `activeGraphRaw`
- `retiredGraphs`
- `nextGeneration`
- active-owner lock

Must not own:

- Pedal model chains.
- Runtime params.
- DSP scratch buffers.

Thread:

- Control thread for publish/cleanup.
- Audio thread for `getActiveRaw` only.

RT risks:

- Audio path must only atomic-load the raw pointer.
- No `shared_ptr` refcount operations in `process`.

Tests:

- Graph swap under processing.
- Preserved latency after add/remove/bypass.
- Diagnostic report generation after swap.

### GraphBuilder

Responsibility:

- Build a new immutable `GraphRuntime` from model chains and runtime params.
- Add nodes, connect topology, prepare graph and compute latency.

Suggested files:

- `Source/Core/Audio/GraphBuilder.h`
- `Source/Core/Audio/GraphBuilder.cpp`

Minimal public API:

```cpp
struct GraphBuildContext
{
    double sampleRate;
    int blockSize;
    int numInputs;
    int numOutputs;
    uint64_t generation;
};

std::shared_ptr<GraphRuntime> build(
    const GraphBuildContext& context,
    const ChainModelSnapshot& model,
    const RuntimeGlobalParams& params,
    uint32_t paramRevision);
```

Owns:

- Temporary graph-build allocations only.

Must not own:

- Active graph lifetime.
- Command queue.
- AudioPlane scratch buffers.

Thread:

- Control thread only.

RT risks:

- Must never be callable from `AudioEngine::process`.
- Scanner should catch graph rebuild patterns in process path.

Tests:

- Empty graph build.
- Single LineA pedal build.
- Dual-line graph build.
- Zone order preserved (`Pre -> Amp -> FX -> Cabinet`).
- Latency samples unchanged vs current baseline.

### GraphRetirementQueue

Responsibility:

- Encapsulate retired graph grace period and memory bound policy.

Suggested files:

- Can start embedded inside `RuntimeGraphManager`.
- Extract only if it remains non-trivial.

Minimal public API:

```cpp
void retire(std::shared_ptr<GraphRuntime> oldGraph, uint64_t releaseAfterBlock);
void cleanup(uint64_t currentBlock);
size_t size() const noexcept;
```

Owns:

- `std::vector<RetiredGraph>`.

Must not own:

- Active graph.
- Block counter.

Thread:

- Control thread only.

RT risks:

- No cleanup from audio thread.

Tests:

- Grace period respected.
- Bounded queue when audio is stopped.

### RoutingMixer

Responsibility:

- Convert `SwitcherMode`, line gain/pan/width and dual compensation into `ChannelStripProcessor::setParams` calls.

Suggested files:

- `Source/Core/Audio/RoutingMixer.h`
- `Source/Core/Audio/RoutingMixer.cpp`

Minimal public API:

```cpp
struct RoutingMixerTargets
{
    float gainA;
    float panA;
    float widthA;
    float gainB;
    float panB;
    float widthB;
};

RoutingMixerTargets calculate(const RuntimeGlobalParams& params) noexcept;
void apply(GraphRuntime& runtime, const RuntimeGlobalParams& params);
```

Owns:

- No persistent state initially.

Must not own:

- ChannelStrip DSP internals.
- Graph topology.

Thread:

- Control thread parameter application.

RT risks:

- Must not be called from process unless it stays pure/no allocation/no lock.

Tests:

- LineA-only mutes B.
- LineB-only mutes A.
- Dual parallel applies 0.5 compensation.
- Zero/near-zero gain fallback behavior remains identical.

### DryWetMixer

Responsibility:

- Own sample-accurate global mix ramp and latency-aligned dry path.

Suggested files:

- `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/Audio/DryWetMixer.cpp`

Minimal public API:

```cpp
void prepare(double sampleRate, int blockSize, int channels);
void reset(float currentMix);
void setTarget(float mix) noexcept;
void setLatencySamples(int latency) noexcept;
bool process(GraphRuntime& runtime, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept;
```

Owns:

- `SampleAccurateRamp`.
- `dryScratch`.
- `delayedDryScratch`.
- `dryDelay`.
- Dry delay write index/capacity.

Must not own:

- Runtime params atomics.
- OutputChain limiter.
- Graph lifetime.

Thread:

- Audio thread for `process`.
- Control thread for `prepare/reset/setLatencySamples`.

RT risks:

- No buffer resize in process.
- Preserve fallback for larger-than-prepared blocks: wet-only, no allocation.

Tests:

- Dry-only null.
- Wet-only unchanged.
- Sample-accurate ramp.
- Latency-aligned dry path after graph latency changes.
- Larger-than-prepared block behavior.

### LatencyManager

Responsibility:

- Centralize graph latency, dry/wet latency mirror and public latency reporting.

Suggested files:

- Start as part of `DryWetMixer` or `RuntimeGraphManager`.
- Extract later as `Source/Core/Audio/LatencyManager.h` only if responsibilities grow.

Minimal public API:

```cpp
void setGraphLatencySamples(int samples) noexcept;
int getGraphLatencySamples() const noexcept;
int getDryCompensationSamples() const noexcept;
```

Owns:

- Clamped atomic latency values.

Must not own:

- Delay buffers.
- Graph object.

Thread:

- Control thread writes.
- Audio/UI reads.

RT risks:

- Atomic-only in audio path.

Tests:

- `getLatencyNumSamples` stable.
- Bypass latency semantics stable.
- OutputChain lookahead latency reflected.

### RuntimeParameterSnapshot

Responsibility:

- Encapsulate `RuntimeGlobalParams` atomics, revisioning and value conversion.

Suggested files:

- `Source/Core/Audio/RuntimeParameterSnapshot.h`
- `Source/Core/Audio/RuntimeParameterSnapshot.cpp`

Minimal public API:

```cpp
void store(const AudioEngine::RuntimeGlobalParams& snapshot) noexcept;
AudioEngine::RuntimeGlobalParams load() const noexcept;
uint32_t revision() const noexcept;
float outputMixNormalized() const noexcept;
```

Owns:

- Atomic runtime global parameters.
- Revision counter.

Must not own:

- Host transport polling.
- Session state.
- Pedal parameters.

Thread:

- Plugin/audio/control may write snapshots.
- Audio thread reads only simple atomics.

RT risks:

- No `ValueTree` access in audio path.
- Avoid duplicate snapshot implementations drifting apart.

Tests:

- Param bridge equivalence.
- Output mix raw -> normalized behavior.
- Host tempo snapshot propagation.

### HealthMonitor

Responsibility:

- Sanitize output, track corruption/silence, set recovery flags and expose counters.

Suggested files:

- `Source/Core/Audio/HealthMonitor.h`
- `Source/Core/Audio/HealthMonitor.cpp`

Minimal public API:

```cpp
struct BlockHealthStats { ... };
BlockHealthStats sanitizeAndMeterOutput(juce::AudioBuffer<float>& buffer, float inputPeak, bool deepScan) noexcept;
HealthActions afterBlock(const BlockHealthStats& stats, const HealthContext& context) noexcept;
void reset(bool resetMeters) noexcept;
```

Owns:

- Corrupt block counters.
- Silent-output counters.
- Auto-heal count.
- Pending log flags.

Must not own:

- Graph rebuild execution.
- Session logging string construction.
- CPU timing meters.

Thread:

- Audio thread for counters/actions.
- Control thread for consuming pending log flags.

RT risks:

- No logging/string creation in `afterBlock`.
- Graph rebuild must remain deferred.

Tests:

- NaN/Inf sanitized.
- Hard clipping clamped.
- Auto-heal requested after configured corrupt blocks.
- Silent-output incident/recovery flags.

### DiagnosticsManager

Responsibility:

- Build diagnostic strings/reports and collect non-audio-thread snapshots.

Suggested files:

- `Source/Core/Audio/DiagnosticsManager.h`
- `Source/Core/Audio/DiagnosticsManager.cpp`

Minimal public API:

```cpp
juce::String buildReport(const AudioEngineSnapshot& snapshot) const;
juce::String describeModelChain(const ChainModelSnapshot& chain) const;
juce::String describeRuntimeChain(const GraphRuntime& runtime) const;
```

Owns:

- No realtime state.

Must not own:

- Graph lifetime.
- Health counters.
- CPU meter.

Thread:

- Control/UI/test threads only.

RT risks:

- Must not be called from `process`.

Tests:

- Existing "diagnostic report reflects queued topology" tests.
- Report still includes model and runtime chain details.

### CpuMeter

Responsibility:

- Track realtime block timing and CPU estimates.

Suggested files:

- `Source/Core/Audio/CpuMeter.h`
- `Source/Core/Audio/CpuMeter.cpp`

Minimal public API:

```cpp
struct CpuMeterSnapshot
{
    double cpuLoad;
    double lastMs;
    double averageMs;
    double peakMs;
};

double beginBlock() noexcept;
void endBlock(double startMs, int numSamples, double sampleRate) noexcept;
CpuMeterSnapshot snapshot() const noexcept;
void reset() noexcept;
```

Owns:

- CPU/load/process-time atomics.

Must not own:

- Diagnostics mode.
- Profiling scenario runner.

Thread:

- Audio thread writes.
- UI/test reads.

RT risks:

- Must keep current timing overhead profile.
- No strings, locks or allocation.

Tests:

- Getter behavior.
- RT profile Release comparison.

### TunerTap

Responsibility:

- Isolate tuner enable state, buffer push, background processing cadence and getter bridge.

Suggested files:

- `Source/Core/Audio/TunerTap.h`
- `Source/Core/Audio/TunerTap.cpp`

Minimal public API:

```cpp
void prepare(double sampleRate);
void reset();
void setEnabled(bool enabled) noexcept;
bool isEnabled() const noexcept;
void pushFromAudioThread(const juce::AudioBuffer<float>& buffer) noexcept;
void processOnEngineThread();
```

Owns:

- `TunerService`.
- Tuner enabled atomic.
- Tuning offset/reference pitch if still needed.

Must not own:

- Graph state.
- Output clearing policy unless explicitly included in process facade.

Thread:

- Audio thread push.
- Engine thread analysis.
- UI/control getters.

RT risks:

- `pushFromAudioThread` must remain RT-safe.

Tests:

- Tuner mode clears audio.
- Pitch/clarity/RMS still update.
- Engine disabled/tuner interaction unchanged.

### GlobalProcessorChain

Responsibility:

- Represent global processors around the pedal chains: input chain, strips and output chain node references.

Suggested files:

- `Source/Core/Audio/GlobalProcessorChain.h`

Minimal public API:

```cpp
struct GlobalProcessorNodes
{
    juce::AudioProcessorGraph::Node::Ptr inputChainNode;
    juce::AudioProcessorGraph::Node::Ptr stripNodeA;
    juce::AudioProcessorGraph::Node::Ptr stripNodeB;
    juce::AudioProcessorGraph::Node::Ptr outputChainNode;
};
```

Owns:

- Initially just typed node/pointer grouping inside `GraphRuntime`.

Must not own:

- DSP code from InputChain/ChannelStrip/OutputChain.

Thread:

- Control thread build; audio thread uses graph only.

RT risks:

- Keep as plain data.

Tests:

- Graph build/regression tests.

### AudioEngineCommandQueue

Responsibility:

- Encapsulate `GraphCommand`, command lock, pending flag and command drain.

Suggested files:

- `Source/Core/Audio/AudioEngineCommandQueue.h`
- `Source/Core/Audio/AudioEngineCommandQueue.cpp`

Minimal public API:

```cpp
void enqueue(const GraphCommand& command);
bool hasPending() const noexcept;
std::deque<GraphCommand> drain();
```

Owns:

- `commandLock`.
- `pendingCommands`.
- `commandsPending`.

Must not own:

- Model chains.
- Graph builder.
- Async update scheduling.

Thread:

- Control/message threads enqueue/drain.
- Audio thread should not call.

RT risks:

- Never call from `process`.

Tests:

- Command order preserved.
- Multiple commands coalesce into one topology rebuild.

## 5. Invariantes que NO pueden romperse

- No graph rebuild in audio thread.
- No locks in `AudioEngine::process`.
- No allocations in audio path.
- No logging or `juce::String` construction in audio path.
- Dry/wet global remains sample-accurate.
- Dry-only mix remains exact dry path and bypasses wet graph processing.
- Wet-only mix remains existing graph output.
- Latency report remains stable and clamped to `MAX_GRAPH_LATENCY_SAMPLES`.
- Dry/wet latency compensation follows graph latency.
- Bypass latency semantics from `ProcessorBase` remain unchanged.
- Graph swapping remains safe: active raw pointer never outlives owner/retired graph.
- `OutputChainProcessor` limiter behavior, lookahead latency, DC cleanup and debug snapshot remain unchanged.
- `InputChainProcessor` single-jack promotion behavior remains unchanged.
- Line routing semantics remain unchanged: LineA-only, LineB-only and Dual parallel.
- Dual parallel 0.5 compensation remains unchanged.
- Tuner-enabled path still taps input and clears output.
- Recovery/auto-heal remains deferred to control thread.
- P1/P2/P3/P4A/P4B/P4C validations must stay PASS.
- Golden metrics must not change unless the phase explicitly documents why and updates baseline under review.
- Release RT profile must not degrade without justification.
- `check-audio-thread-policy.ps1` must remain 0 FAIL in active routes.
- No public parameter IDs change.
- No preset schema changes.

## 6. Plan incremental de refactor

### P5B: Extraer CpuMeter y process-time metering

Modelo recomendado: `5.3-codex`

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- New `Source/Core/Audio/CpuMeter.h`
- New `Source/Core/Audio/CpuMeter.cpp` if non-header implementation is useful
- `NOVA.jucer` if adding `.cpp`

Behavior identical:

- `getCpuLoad`, `getLastProcessTimeMs`, `getAverageProcessTimeMs`, `getPeakProcessTimeMs`.
- Timing start/end placement in `process`.
- RT profile outputs should stay within normal run-to-run variance.

Risks:

- Timing overhead increase.
- Reset semantics drift in `prepare`/runtime reset.
- `DiagnosticsMode` accidentally coupled to CPU meter.

Tests:

- Debug/Release builds.
- `run-rt-profile-scenarios.ps1` Debug and Release.
- `check-audio-thread-policy.ps1`.

Rollback criterion:

- Any active-route policy FAIL.
- Release RT profile `avgProcessMs` or `maxBudgetRatio` regresses broadly beyond baseline tolerance without clear noise explanation.

### P5C: Extraer HealthMonitor y auto-heal flags

Modelo recomendado: `GPT-5.5 xhigh`

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- New `Source/Core/Audio/HealthMonitor.h`
- New `Source/Core/Audio/HealthMonitor.cpp`
- Tests in `AudioEngineTests.cpp` only if needed
- `NOVA.jucer` if adding `.cpp`

Behavior identical:

- NaN/Inf sanitize.
- Hard limit clamp.
- Near clip and click spike counting.
- Auto-heal trigger threshold/cooldown.
- Silent-output incident/recovery flags.
- Deferred logging still happens from engine thread.

Risks:

- Audio mutation order changes.
- Auto-heal requested too early/late.
- Silent-output false positives.
- Missing meter updates.

Tests:

- Full base validation.
- Golden metrics.
- RT profile Debug/Release.
- Policy scan.

Rollback criterion:

- Any new validation failure in OutputChain, AudioEngine or Regression group.
- Golden metric drift in clean/dry/output-chain scenarios.

### P5D: Extraer RuntimeParameterSnapshot / GlobalParams bridge

Modelo recomendado: `GPT-5.5 xhigh`

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/PluginProcessor.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/SessionStore.h` only if duplicate atomics are intentionally shared or wrapped
- New `Source/Core/Audio/RuntimeParameterSnapshot.h`
- New `Source/Core/Audio/RuntimeParameterSnapshot.cpp`

Behavior identical:

- Runtime params values and defaults.
- Output mix raw-to-normalized conversion.
- Host tempo/transport propagation.
- P4C 8-block transport polling remains or is explicitly moved to a non-audio snapshot design.

Risks:

- Stale snapshots.
- Revision not incrementing.
- Host tempo not reaching tempo-sync pedals.
- `setValueNotifyingHost` accidentally enters audio path.

Tests:

- Base validation.
- Golden metrics.
- RT profile Debug/Release.
- Policy scan.
- Delay sync timing tests.
- Reverb/delay reverse+swell regression coverage.

Rollback criterion:

- Tempo-sync tests or delay sync golden metrics drift.
- Policy scan FAIL for host/parameter bridge.

### P5E: Extraer RoutingMixer / DryWetMixer

Modelo recomendado: `GPT-5.5 xhigh`

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- New `Source/Core/Audio/RoutingMixer.h`
- Optional `Source/Core/Audio/RoutingMixer.cpp`
- New `Source/Core/Audio/DryWetMixer.h`
- New `Source/Core/Audio/DryWetMixer.cpp`
- `AudioEngineTests.cpp` for focused dry/wet tests if coverage needs to be pinned

Behavior identical:

- LineA-only/LineB-only/Dual routing.
- Dual parallel 0.5 compensation.
- Dry-only exact null.
- Wet-only graph path.
- Sample-accurate output mix ramp.
- Latency-aligned dry path.
- Larger-than-prepared block fallback.

Risks:

- Accidental tone/routing change.
- Latency compensation off by one sample.
- Dry delay reset changes.
- More CPU in process path.

Tests:

- Base validation.
- Golden metrics, especially clean dry, dual parallel, overdrive-cleanamp-reverb.
- RT profile Debug/Release.
- Policy scan.

Rollback criterion:

- Any golden metrics drift in clean dry, dual parallel or chain nominal.
- Release RT profile degradation beyond tolerance.

### P5F: Extraer GraphBuilder / RuntimeGraphManager

Modelo recomendado: `GPT-5.5 xhigh`

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- New `Source/Core/Audio/GraphTypes.h`
- New `Source/Core/Audio/GraphBuilder.h/.cpp`
- New `Source/Core/Audio/RuntimeGraphManager.h/.cpp`
- Possibly `Source/Core/Audio/AudioEngineCommandQueue.h/.cpp`
- `NOVA.jucer`

Behavior identical:

- Graph topology and zone ordering.
- Pedal creation and bypass state.
- Cached `ProcessorBase` and `TempoSyncable` pointers.
- Graph prepare/rebuild order.
- Graph latency calculation.
- Retired graph grace period and memory bound.

Risks:

- Use-after-free or raw pointer lifetime error.
- Graph rebuild accidentally callable from audio path.
- Missing cleanup when audio is stopped.
- Bypass latency graph rebuild changes.
- Diagnostic report loses runtime/model distinction.

Tests:

- Full validation matrix.
- Additional manual stress: add/remove/move pedals while processing if available.
- Policy scan with `-FailOnWarn` considered after legacy warnings are handled or filtered.

Rollback criterion:

- Any crash/hang.
- Any policy FAIL.
- Any latency regression.
- Any P1/P2/P3/P4 regression.

### P5G: Extraer DiagnosticsManager

Modelo recomendado: `5.3-codex`

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/OfflineQADiagnostics.h` only if public report wiring changes
- New `Source/Core/Audio/DiagnosticsManager.h/.cpp`

Behavior identical:

- `buildDiagnosticReport` content remains functionally equivalent.
- `getOutputChainDebugSnapshot` remains available for offline diagnostics.
- Logs remain off audio thread.

Risks:

- Tests/scripts expecting report text lose fields.
- Diagnostics accidentally read active graph without safe owner/raw rules.
- String building enters audio path.

Tests:

- Base validation.
- Golden metrics.
- RT profile.
- Policy scan.
- Existing diagnostic report unit tests.

Rollback criterion:

- Report loses model/runtime chain info.
- Policy scan flags `juce::String` or logging in active routes.

## 7. Testing plan por fase

### Minimum for every implementation phase

Run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin
git diff --check
powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120
powershell -ExecutionPolicy Bypass -File scripts\run-golden-audio-metrics.ps1
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -ReportPath artifacts/rt-profile-debug-x64-report.json
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report.json
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1
```

### Phase-specific emphasis

| Phase | Extra focus | Acceptable differences |
| --- | --- | --- |
| P5B CpuMeter | RT profile Debug/Release timing only | Small timing variance; no status downgrade in Release |
| P5C HealthMonitor | invalid/clipped/autoHeal/silent-output counters | No output sample/golden drift; no new FAIL |
| P5D RuntimeParameterSnapshot | tempo sync, host transport, output mix | No golden drift; no tempo-sync regression |
| P5E Routing/DryWet | clean dry, dual parallel, dry/wet, latency | No golden drift unless explicitly approved |
| P5F GraphManager/Builder | graph add/remove/move/bypass, lifetime, latency | No behavior drift; no crash/hang; no policy FAIL |
| P5G Diagnostics | report content and scripts | Text formatting differences acceptable only if scripts/tests do not depend on old text |

### Baseline comparison rules

- Base validation failures are never acceptable in P5.
- Golden metric drift is unacceptable unless the phase explicitly changes expected behavior, which P5 should avoid.
- Release RT profile may vary run-to-run, but broad regression in `avgProcessMs`, `cpuAvgPercent` or `maxBudgetRatio` must be investigated.
- Debug RT warnings are informational unless they become FAIL or point to a new active-route policy issue.
- `check-audio-thread-policy.ps1` must stay 0 active-route FAIL.

## 8. Riesgos de implementacion

### Cambio accidental de latency

Cause:

- Moving graph latency calculation, bypass rebuild handling or dry-delay mirror.

Guard:

- Keep latency manager/extraction separate from graph builder until tests cover it.
- Compare `getLatencyNumSamples` and dry/wet golden metrics.

### Cambio accidental de dry/wet

Cause:

- Moving ramp state, dry scratch buffers or delay line.

Guard:

- Extract `DryWetMixer` after CpuMeter/HealthMonitor.
- Preserve endpoint fast paths and fallback behavior.

### Cambio accidental de routing

Cause:

- Reworking `applyRuntimeParamsToGraph` or graph connections.

Guard:

- Use `RoutingMixer::calculate` first as pure extraction.
- Keep graph topology unchanged.

### Lifetime de graphs retirados

Cause:

- Moving active owner/raw pointer/retired queue in one large edit.

Guard:

- Extract `RuntimeGraphManager` only after smaller modules are stable.
- Keep audio thread raw pointer contract explicit.

### Race conditions

Cause:

- Changing locks/atomics around commands, params or graph ownership.

Guard:

- No `shared_ptr` acquire/release in process.
- Keep command drain on control thread.
- Use policy scan and targeted stress tests.

### Snapshots stale

Cause:

- Moving runtime params or host transport bridge.

Guard:

- Keep revision semantics.
- Test tempo-sync and output mix changes.

### Regression en tuner

Cause:

- Moving tuner state or engine thread cadence.

Guard:

- Do not extract tuner before health/cpu/drywet are stable.
- Preserve buffer clear behavior when tuner is enabled.

### Regression en diagnostics

Cause:

- Moving report construction away from direct access to current internals.

Guard:

- Create explicit snapshot structs.
- Keep diagnostics off audio thread.

### Cambios de performance

Cause:

- Adding abstractions in `process`, indirect allocations, extra atomics or timing calls.

Guard:

- Prefer zero-allocation structs with inline/noexcept process APIs.
- Compare Release RT baseline every phase.

## 9. Que NO hacer en P5

- No UI.
- No wizards.
- No revoicing.
- No cambios tonales.
- No cambios DSP intencionales.
- No cambios de IDs de parametros.
- No cambios de preset schema.
- No redisenar pedales.
- No mezclar refactor con mejoras tonales.
- No eliminar telemetry util.
- No tocar legacy cleanup salvo fase separada.
- No mover `PedalRegistry`/catalog behavior salvo que una fase GraphBuilder lo necesite de forma mecanica.
- No actualizar golden baselines por comodidad.
- No convertir warnings legacy en allowlist silenciosa sin documentacion.

## 10. Recomendacion de primera fase implementable

Primera fase recomendada: P5B, extraer `CpuMeter`.

Razon:

- Es la extraccion de menor acoplamiento.
- No toca graph topology.
- No toca DSP tonal.
- No toca `RuntimeGlobalParams`.
- No toca dry/wet.
- Tiene verificacion directa con `run-rt-profile-scenarios.ps1`.
- Reduce ruido en `AudioEngine::process` antes de mover logic mas delicada.

Prompt sugerido para P5B:

```text
Tarea: P5B - Extraer CpuMeter de AudioEngine sin cambios DSP.

Crear Source/Core/Audio/CpuMeter.h/.cpp si hace falta y mover solo el estado/metodos de CPU/process-time metering:
- cpuUsage
- lastProcessTimeMs
- averageProcessTimeMs
- peakProcessTimeMs
- updateRealtimeTimingMeters

Mantener exactamente la misma semantica publica de:
- getCpuLoad
- getLastProcessTimeMs
- getAverageProcessTimeMs
- getPeakProcessTimeMs

No tocar graph build, dry/wet, routing, health monitor, tuner, IDs, schema, UI ni DSP.
Ejecutar builds Debug/Release, base validation, golden metrics, RT profile Debug/Release, policy scan y git diff --check.
```

Modelo recomendado para P5B: `5.3-codex`.

Modelo recomendado para fases de mayor riesgo:

- P5C HealthMonitor: `GPT-5.5 xhigh`
- P5D RuntimeParameterSnapshot: `GPT-5.5 xhigh`
- P5E RoutingMixer/DryWetMixer: `GPT-5.5 xhigh`
- P5F GraphBuilder/RuntimeGraphManager: `GPT-5.5 xhigh`
- P5G DiagnosticsManager: `5.3-codex`
