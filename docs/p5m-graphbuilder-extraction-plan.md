# P5M - GraphBuilder Extraction Plan

Date: 2026-04-30

## 1. Executive Summary

`GraphBuilder` is the next dangerous extraction because it sits at the boundary between control-plane model state and the real runtime audio topology. Unlike the previous extractions, this code does not only move bookkeeping: it creates the `juce::AudioProcessorGraph`, instantiates processors, connects the graph, applies initial runtime parameters, prepares processors, rebuilds the graph, and captures latency used by dry/wet compensation.

This phase must not implement `GraphBuilder`. The safe path is to document the current behavior, define the future module boundary, identify tests that pin the behavior, and only then do a narrow implementation in P5N.

Main reasons this extraction is high risk:

- A small topology change can alter tone, routing, latency, bypass, or dry/wet alignment.
- `GraphRuntime`, `ChainRuntimeSlot`, and `ChainNodeSpec` are currently private `AudioEngine` types, so the implementation needs a careful type-boundary step.
- Build order matters: runtime params are applied before `graph.prepareToPlay()` and `graph.rebuild()`, and latency is captured only after rebuild.
- Connection order matters: runtime pedal slots are connected in stable `Pre -> Amp -> FX -> Cabinet` order.
- Failure behavior matters: unsupported pedal creation and failed graph node creation are logged and skipped, not fatal.

## 2. Current `buildGraphFromModelLocked` Map

Current call context:

- `AudioEngine::prepare()` takes `controlPlane.modelLock`, calls `buildGraphFromModelLocked(controlPlane.nextGeneration++)`, then publishes via `publishGraph(...)`.
- `AudioEngine::flushPendingGraphCommands()` applies queued model changes under `modelLock`, then calls `buildGraphFromModelLocked(...)` when topology changed or an explicit rebuild is requested.
- The function name is accurate today: it assumes the caller has locked the model, and it reads `controlPlane.modelChainA` and `controlPlane.modelChainB` directly.

Current behavior, step by step:

1. Creates `std::shared_ptr<GraphRuntime>`.
2. Allocates `std::unique_ptr<juce::AudioProcessorGraph>`.
3. Captures sanitized runtime dimensions:
   - `sampleRate = audioPlane.currentSampleRate > 0 ? currentSampleRate : 44100`
   - `blockSize = max(1, currentBlockSize)`
   - `numInputs = max(1, numInputChannels)`
   - `numOutputs = max(1, numOutputChannels)`
   - `generation = generation`
4. Calls `graph.setPlayConfigDetails(numInputs, numOutputs, sampleRate, blockSize)`.
5. Creates graph IO nodes:
   - audio input node
   - audio output node
6. Creates global processors:
   - `InputChainProcessor`
   - `ChannelStripProcessor` for Line A
   - `ChannelStripProcessor` for Line B
   - `OutputChainProcessor`
7. Caches raw processor pointers into `GraphRuntime`:
   - `inputChain`
   - `stripA`
   - `stripB`
   - `outputChain`
8. Sets telemetry tags:
   - `channel-strip-a`
   - `channel-strip-b`
   - `output-chain`
9. Connects graph input to `InputChainProcessor`:
   - input channel 0 to input-chain channel 0
   - input channel 1 to input-chain channel 1 when input is stereo
   - input channel 0 also to input-chain channel 1 when input is mono
10. Builds Line A pedal runtime slots from `controlPlane.modelChainA`.
11. Builds Line B pedal runtime slots from `controlPlane.modelChainB`.
12. For each model slot:
   - calls `PedalRegistry::createPedal(spec.pedalType)`
   - if creation fails, logs `engine.addPedal.failed` and skips that slot
   - calls `graph.addNode(std::move(pedal))`
   - if node creation fails, logs `engine.addPedal.failed` and skips that slot
   - fills `ChainRuntimeSlot` metadata: node, type, ID, zone, bypassed flag, processor pointer
   - caches `ProcessorBase*` with `dynamic_cast<ProcessorBase*>`
   - caches `TempoSyncable*` with `dynamic_cast<TempoSyncable*>`
   - applies initial bypass:
     - `ProcessorBase::setBypassed(spec.bypassed)` when available
     - otherwise `processor->suspendProcessing(spec.bypassed)`
13. Connects each runtime chain to its target strip using `connectRuntimeChain(...)`.
14. `connectRuntimeChain(...)`:
   - starts from `inputChainNode`
   - creates an index list for the runtime slots
   - stable-sorts indexes by `zoneRank`: `Pre`, `Amp`, `FX`, `Cabinet`, unknown last
   - connects source L/R to each pedal node L/R
   - updates the current source to that pedal node
   - connects the last source to the target strip L/R
15. Connects both channel strips into `OutputChainProcessor`:
   - strip A L/R to output-chain L/R
   - strip B L/R to output-chain L/R
16. Connects `OutputChainProcessor` L/R to graph audio output L/R.
17. Applies runtime params before prepare/rebuild via `applyRuntimeParamsToGraph(*runtime, params.load(), params.getRevision())`:
   - input gain, gate threshold, force mono to `InputChainProcessor`
   - output volume and limiter to `OutputChainProcessor`
   - switcher mode to strip mute behavior
   - dual parallel gain compensation `0.5`
   - per-line gain, pan, width to channel strips
   - tempo sync context to cached `TempoSyncable` slots
   - sets `runtime.appliedParamRevision`
18. Calls `graph.prepareToPlay(sampleRate, blockSize)`.
19. Calls `graph.rebuild()`.
20. Captures graph latency:
   - `runtime->latencySamples = jlimit(0, MAX_GRAPH_LATENCY_SAMPLES, graph.getLatencySamples())`
21. Returns the fully built runtime graph to `AudioEngine`.

Notable current fallbacks:

- Missing pedal factory: log and skip the model slot.
- Failed `graph.addNode`: log and skip the model slot.
- Missing IO/global node: later connection steps are guarded and skip missing pieces.
- Invalid sample rate/block/channels: sanitized to stable positive defaults.

## 3. State GraphBuilder Should Receive

Recommended request type:

```cpp
namespace Nova::Audio
{
struct GraphBuildRequest
{
    double sampleRate = 44100.0;
    int blockSize = 512;
    int numInputs = 2;
    int numOutputs = 2;
    uint64_t generation = 0;

    RuntimeGlobalParamsSnapshot runtimeParams;
    uint32_t runtimeParamRevision = 0;

    std::vector<GraphChainNodeSpec> modelChainA;
    std::vector<GraphChainNodeSpec> modelChainB;

    int diagnosticsMode = 0; // optional/reserved; current build path does not branch on it.
};
}
```

The request should contain model chain snapshots, not references to `AudioEngine::ControlPlane`. `AudioEngine` should copy `modelChainA/B` while holding `modelLock`, then release the lock before or during the build only if the implementation can preserve current behavior. The first implementation may keep the existing lock scope to reduce risk, but the builder must not own the lock or mutate the model.

Factory dependency options:

- Minimal first step: builder calls `PedalRegistry::createPedal(...)` directly, matching current behavior.
- More testable later step: pass a small non-owning factory hook:

```cpp
struct GraphPedalFactory
{
    std::unique_ptr<juce::AudioProcessor> (*createPedal)(const juce::String& type) = nullptr;
};
```

Avoid `std::function` here unless a real need appears. A function pointer is enough for a factory hook and avoids hidden type-erasure allocation behavior.

## 4. State GraphBuilder Should Produce

Recommended result shape:

```cpp
namespace Nova::Audio
{
struct GraphBuildWarning
{
    juce::String code;
    juce::String pedalType;
    juce::String pedalID;
    juce::String message;
};

template <typename GraphRuntime>
struct GraphBuildResult
{
    std::shared_ptr<GraphRuntime> runtime;
    std::vector<GraphBuildWarning> warnings;
};
}
```

The runtime must contain the same state currently produced by `AudioEngine::GraphRuntime`:

- `std::unique_ptr<juce::AudioProcessorGraph> graph`
- IO nodes
- global processor nodes
- cached global processor pointers
- Line A and Line B runtime slots
- `sampleRate`
- `blockSize`
- `numInputs`
- `numOutputs`
- `generation`
- `latencySamples`
- `appliedParamRevision`

The result may carry warnings/errors for skipped pedals. P5N should either keep the exact existing `SessionLogger` messages or have `AudioEngine` emit equivalent messages from `GraphBuildWarning`. Do not make unsupported pedals fatal unless the current behavior changes by explicit later design.

## 5. What GraphBuilder Must Not Own

`GraphBuilder` must not own or mutate:

- active graph ownership
- `publishGraph`
- `RuntimeGraphManager`
- `GraphRetirementQueue`
- audio block counter
- command queue
- `GraphCommandApplier`
- model chains as durable state
- model mutation
- dry/wet scratch buffers
- dry delay line
- wet/dry ramp
- `HealthMonitor`
- `CpuMeter`
- tuner state
- diagnostics string formatting
- UI state
- preset schema
- parameter IDs

`GraphBuilder` should own only temporary build allocations and helper logic needed to construct one new runtime graph from immutable inputs.

## 6. Proposed API

Recommended narrow API:

```cpp
namespace Nova::Audio
{
class GraphBuilder
{
public:
    GraphBuildResult<GraphRuntime> build(const GraphBuildRequest& request);
};
}
```

Because `GraphRuntime` is currently private inside `AudioEngine`, P5N needs one of these boundary choices before a real implementation:

1. Preferred: extract graph data types into `Source/Core/Audio/GraphRuntimeTypes.h`.
   - `GraphChainNodeSpec`
   - `GraphRuntimeSlot`
   - `GraphRuntime`
   - `GraphBuildRequest`
   - `GraphBuildResult`
2. Transitional: keep `AudioEngine` aliases:
   - `using ChainNodeSpec = Nova::Audio::GraphChainNodeSpec;`
   - `using ChainRuntimeSlot = Nova::Audio::GraphRuntimeSlot;`
   - `using GraphRuntime = Nova::Audio::GraphRuntime;`
3. Avoid making `GraphBuilder` a `friend` of `AudioEngine`; that keeps the old private coupling under a new name.

Implementation file strategy:

- Header-only is viable for the first P5N extraction if `GraphBuilder.h` is included only by `AudioEngine.cpp`. This avoids `NOVA.jucer` changes.
- A `.cpp` is cleaner long-term because the builder will include `PedalRegistry`, global processors, and JUCE graph types. If P5N creates a `.cpp`, update `NOVA.jucer` and regenerate project files rather than editing generated Visual Studio files.

Dependency direction:

- `AudioEngine.cpp` may include `Audio/GraphBuilder.h`.
- `GraphBuilder.h/.cpp` may include:
  - `GraphRuntimeTypes.h`
  - `RuntimeParameterSnapshot.h`
  - `PedalRegistry.h`
  - `DSP/Global/InputChain.h`
  - `DSP/Global/ChannelStrip.h`
  - `DSP/Global/OutputChain.h`
  - `ProcessorBase.h`
- `GraphBuilder` must not include `AudioEngine.h`.

## 7. Principal Risks

- Accidental topology change: missing or reordered graph connections can alter routing or mute one line.
- Zone order change: `std::stable_sort` by `Pre -> Amp -> FX -> Cabinet` must remain exact.
- Latency change: latency must still be captured after `prepareToPlay()` and `graph.rebuild()`.
- Dry/wet mismatch: `publishGraph` still uses `runtime->latencySamples` for dry delay; any build latency drift changes perceived wet/dry alignment.
- Bypass initial state: `ProcessorBase::setBypassed(...)` and fallback `suspendProcessing(...)` must stay equivalent.
- Bypass latency behavior: in-place bypass rebuild remains in `AudioEngine::applyPedalBypassToActiveGraph`; P5N must not move it accidentally.
- Runtime params order: initial params are currently applied before prepare/rebuild; moving this later can change smoothing, limiter latency, strip mute state, and tempo context.
- Tempo sync cache: every `TempoSyncable*` must still be cached during build and updated when params apply.
- `ProcessorBase` cache: every `ProcessorBase*` must still be cached to avoid dynamic_cast from the audio thread.
- Global processors: `InputChainProcessor`, both `ChannelStripProcessor`s, and `OutputChainProcessor` must still exist and be connected exactly as before.
- Failure behavior: unsupported pedals and failed graph node creation must continue to skip only that slot.
- Logging behavior: existing failure messages should either remain identical or be documented if warning aggregation changes timing.
- Allocation behavior: graph build already allocates on the control path; additional control-path allocation is acceptable but must not enter `AudioEngine::process`.
- Include/project risk: adding `.cpp` requires `NOVA.jucer` update and generated project regeneration.

## 8. Tests Needed Before Implementation

Existing useful coverage:

- active graph exists immediately after `prepare`
- disabled engine keeps active graph owner
- clean path remains stable after add/remove topology swaps
- add/remove while deterministic processing remains finite
- bypass latency rebuild updates active graph in place
- `ClearAll` followed by add rebuilds correctly
- queued topology edits preserve command order
- diagnostic report reflects queued topology
- pre-prepare topology survives `prepare`
- golden metrics validate broad audio behavior against P4
- RT Release profile remains clean

Recommended additional or reinforced tests before/with P5N:

- Graph contains expected runtime nodes after adding a single pedal.
- Line A and Line B both produce expected runtime slots after independent adds.
- Chain order is preserved in public model/runtime views after add/move.
- Zone connection order is preserved for intentionally mixed insertion order, e.g. FX inserted before Pre/Amp/Cabinet.
- LineA-only, LineB-only, and Dual topology remain equivalent after extraction.
- Initial bypass state is preserved at build time, including latency-relevant `ProcessorBase` pedals.
- Latency is identical before/after extraction for empty graph, Overdrive active/bypassed, OutputChain limiter active, and mixed chain.
- Tempo-sync pointers are preserved and receive host tempo context after build.
- `ProcessorBase` cache is populated for known ProcessorBase pedals.
- Global processor pointers exist after build (`inputChain`, `stripA`, `stripB`, `outputChain`).
- `ClearAll + add` behavior remains preserved after builder extraction.
- Add/remove/move batches preserve order and publish one correct rebuilt graph.
- Golden metrics remain unchanged after extraction.
- Diagnostic report still includes model/runtime topology fields after extraction.

Tests that may require new test hooks:

- Direct graph node count and exact connection graph inspection are not currently public.
- Exact zone connection order is only indirectly observable through diagnostics/audio unless `GraphRuntime` becomes test-visible.
- Unsupported pedal warning behavior may need a testable fake factory or diagnostic counter.

Do not expose broad internals just for tests. Prefer narrow test-only helpers or diagnostics that already exist.

## 9. P5N Implementation Plan

Recommended steps:

1. Add graph runtime types in a small header:
   - `Source/Core/Audio/GraphRuntimeTypes.h`
   - move only `ChainNodeSpec`, `ChainRuntimeSlot`, and `GraphRuntime` equivalents
   - keep `AudioEngine` aliases so public/internal call sites stay readable
2. Add `GraphBuildRequest`, `GraphBuildWarning`, and `GraphBuildResult` types.
3. Keep `AudioEngine::buildGraphFromModelLocked(...)` as the wrapper.
   - It should gather/snapshot model chains and runtime params.
   - It should call the builder.
   - It should preserve existing logging/fallback behavior.
4. Move pure helper logic first:
   - `zoneRank(...)`
   - ordered chain index construction
   - runtime slot construction helpers
5. Move global processor node creation:
   - graph allocation
   - play config details
   - IO node creation
   - input/strip/output processor node creation
   - telemetry tags
6. Move chain slot construction:
   - `PedalRegistry::createPedal`
   - graph node addition
   - `ProcessorBase` cache
   - `TempoSyncable` cache
   - initial bypass application
7. Move graph connection logic:
   - input to input-chain
   - input-chain through ordered pedals to strip
   - strips to output-chain
   - output-chain to audio output
8. Move or share initial runtime param application.
   - Preserve current order: params before `prepareToPlay()` and `graph.rebuild()`.
   - Avoid duplicating parameter application logic long-term.
   - Do not alter runtime param snapshot semantics.
9. Keep `AudioEngine::applyRuntimeParamsToGraph(...)` available for params-only updates after publish unless a shared helper is created.
10. Keep `AudioEngine::applyPedalBypassToActiveGraph(...)` unchanged for P5N.
11. Keep `publishGraph`, `RuntimeGraphManager`, dry/wet, routing, health, CPU, tuner, diagnostics, command queue, and command applier unchanged.
12. Validate after the type move and again after builder delegation if practical.

P5N files likely to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/GraphRuntimeTypes.h` (new)
- `Source/Core/Audio/GraphBuilder.h` (new)
- `Source/Core/AudioEngineTests.cpp` only for focused coverage if needed
- `NOVA.jucer` only if a `.cpp` is created

P5N files not to touch:

- DSP processor algorithms
- `RuntimeGraphManager`
- command semantics
- `RuntimeParameterSnapshot` behavior
- `HealthMonitor`
- `CpuMeter`
- `DiagnosticsManager`
- UI
- preset schema
- golden baselines

## 10. Rollback Criteria

Rollback or stop P5N if any of these occur:

- base validation failure
- golden metrics failure
- RT Release downgrade from clean `16/16/0/0`
- RT Stability Release prioritized failure or WARN under CI policy
- active-route policy scan FAIL
- graph lifecycle test failure
- latency regression in any graph lifecycle or output limiter test
- active graph becomes null after `prepare`
- dry/wet behavior changes
- LineA/LineB/Dual routing changes
- diagnostic report loses topology information
- crash, hang, or leaked graph ownership
- new known failures added to pass the phase

## 11. Recommended P5N Prompt

```text
Tarea: P5N - Extraer GraphBuilder de forma estrecha sin cambiar comportamiento.

Implementar solo la extraccion planificada en docs/p5m-graphbuilder-extraction-plan.md.

Crear GraphRuntimeTypes/GraphBuilder si hace falta, mantener AudioEngine::buildGraphFromModelLocked como wrapper, preservar exactamente topology, zone order, runtime param application order, prepare/rebuild/latency capture, bypass initial state, ProcessorBase/TempoSyncable caches, failure skip behavior y publishGraph.

No tocar DSP, routing/dry-wet behavior, RuntimeGraphManager, command semantics, RuntimeParameterSnapshot, HealthMonitor, CpuMeter, DiagnosticsManager, UI, preset schema, golden baselines ni known failures.

Validar con builds Debug/Release, git diff --check, base validation, golden metrics, RT Release, RT Stability Release priorizada, policy scan y wrapper Fast.
```

## Recommendation

P5N can implement the extraction if it first introduces shared graph runtime types and keeps `AudioEngine::buildGraphFromModelLocked` as a compatibility wrapper. If the team wants lower risk before implementation, add one focused test for mixed-zone connection/order and one latency equivalence test for a mixed chain before moving builder code.
