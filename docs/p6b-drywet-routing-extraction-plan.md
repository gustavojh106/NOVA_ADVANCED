# P6B - DryWetMixer / RoutingMixer Extraction Plan

Date: 2026-04-30  
Scope: design only. No implementation in this phase.

## 1. Executive Summary

Dry/wet and routing are sensitive because they sit directly in `AudioEngine::process`, where small order changes can alter tone, latency, gain, clicks, or realtime safety. The current code is not just formatting or ownership glue: it contains the dry capture point, wet graph invocation, sample-accurate mix ramp, dry-delay latency compensation, engine disabled/startup/tuner bypass paths, and the routing mode interpretation that eventually drives `ChannelStripProcessor`.

This should not be extracted as one large refactor. The safe path is to separate dry/wet first, under the P6A tests, while leaving graph ownership, graph build, command semantics, and pedal DSP untouched. Routing should follow later because current routing behavior is split between:

- `GraphBuilder::applyRuntimeParamsToGraph`: mode/gain/pan/width interpretation and dual-parallel compensation.
- `ChannelStripProcessor`: actual gain/pan/width DSP smoothing and stereo math.
- `AudioEngine::process`: orchestration around wet graph processing and dry/wet.

The P6A coverage now protects the highest-risk user-visible behavior: dry-only exactness, wet-only graph path, ramp discontinuity, routing modes, dual compensation, strip controls, and oversized block fallback.

## 2. Current AudioEngine Responsibility Map

### Process orchestration

Current owner: `AudioEngine::process`.

Responsibilities:

- bind the first audio thread id
- increment `audioBlockCounter`
- handle deferred audio runtime reset
- start/end `CpuMeter` timing
- set wet-mix target from `RuntimeParameterSnapshot`
- update light input meter
- branch for engine disabled, startup mute, tuner mode, missing graph
- load active raw graph via `runtimeGraphs.getActiveRaw()`
- call `processWithSampleAccurateDryWet`
- sanitize/meter output via `HealthMonitor`
- apply post-block health actions

This orchestration should remain in `AudioEngine` until dry/wet extraction is proven stable.

### Dry buffer capture

Current owner: `AudioEngine::processWithSampleAccurateDryWet`.

Current behavior:

- if mix is exactly dry and settled, no dry scratch copy and no graph processing occur
- otherwise input is copied into `audioPlane.dryScratch` before wet graph processing
- only prepared channel capacity is copied

Invariant:

- dry capture happens before `runtime.graph->processBlock(...)`
- dry-only endpoint remains exact and does not touch the graph

### Wet graph processing

Current owner: `AudioEngine::processWithSampleAccurateDryWet`.

Current behavior:

- normal path calls `runtime.graph->processBlock(buffer, midi)`
- oversized block fallback calls wet graph only and returns, avoiding audio-thread allocation
- wet-only endpoint returns after graph processing and consumes ramp samples

Invariant:

- `DryWetMixer` must not own `GraphRuntime`, `RuntimeGraphManager`, or active raw pointer.
- If a future mixer needs to process wet audio, it should receive a callback or be called around graph processing by `AudioEngine`.

### Dry/wet ramp and output mix target

Current owner:

- state: `AudioEngine::SampleAccurateRamp audioPlane.wetMixRamp`
- target update: `AudioEngine::process` via `params.getOutputMixNormalized()`
- per-sample interpolation: `mixWetDrySampleAccurate`

Current behavior:

- target is refreshed every block
- endpoints use `kEndpointEpsilon`
- endpoint paths still consume `wetMixRamp.getNext()` per sample

Invariant:

- target update remains before early returns and before graph processing branch
- no string/logging/allocation enters audio path

### Sample-accurate mix

Current owner: `AudioEngine::mixWetDrySampleAccurate`.

Current behavior:

- per sample: `wet = wetMixRamp.getNext()`, `dry = 1.0f - wet`
- per channel: `wetBuffer = delayedDry * dry + wetBuffer * wet`

Invariant:

- channel loop and sample order must remain equivalent
- no allocation, no locks, no graph access

### Dry delay / latency compensation

Current owner:

- state: `audioPlane.dryDelay`, `dryDelayWriteIndex`, `dryDelayBufferSize`, `currentDryLatencySamples`
- logic: `resetDryDelayLine`, `updateDryDelayLatency`, `copyDryThroughLatency`

Current behavior:

- delay buffer allocated in `prepareScratchBuffers`
- current dry latency is clamped to `MAX_GRAPH_LATENCY_SAMPLES`
- copy path clamps latency to fit current delay buffer
- latency 0 copies dry input directly
- nonzero latency reads old dry samples and writes current dry samples

Invariant:

- latency update remains tied to publish/bypass latency changes
- dry delay line reset remains tied to runtime reset
- no allocation in `process`

### Routing modes, dual compensation, gain/pan/width

Current owner:

- mode/gain/pan/width interpretation: `GraphBuilder::applyRuntimeParamsToGraph`
- actual strip DSP: `ChannelStripProcessor`
- runtime param source: `RuntimeParameterSnapshot`

Current behavior:

- `LineA_Only` mutes strip B
- `LineB_Only` mutes strip A
- `Dual_Parallel` keeps both strips active
- dual parallel applies `0.5f` compensation to active line gains
- gains `<= 0.001f` on active, unmuted lines are normalized to `1.0f`
- pan and width values are passed to each `ChannelStripProcessor`

Invariant:

- `RoutingMixer` must not duplicate or replace `ChannelStripProcessor` DSP.
- If extracted, it should calculate routing targets and leave DSP in `ChannelStripProcessor`.

### Oversized block fallback

Current owner: `processWithSampleAccurateDryWet`.

Current behavior:

- if `numSamples > scratchBlockCapacity`, process wet-only and return
- this avoids allocation in audio thread

Invariant:

- future `DryWetMixer` must preserve this exact fallback behavior.

### Startup mute / bypass / disabled paths

Current owner: `AudioEngine::process`.

Current behavior:

- engine disabled: input is not graph processed; output is sanitized/metered
- startup counter: input passes through current buffer path without graph processing
- tuner enabled: input is pushed to tuner, buffer is cleared
- missing graph: buffer is cleared

Invariant:

- these branches should remain in `AudioEngine`; DryWetMixer should not decide engine state.

## 3. Module Separation Options

### Option A: DryWetMixer + RoutingMixer

Modules:

- `DryWetMixer`: owns dry/wet scratch, ramp, dry delay, latency alignment helpers.
- `RoutingMixer`: owns routing policy/target calculation for LineA, LineB, Dual, dual compensation, and line strip targets.

Pros:

- clear mapping to current risk areas
- easy to stage DryWet first and Routing later
- avoids over-fragmentation before behavior is locked
- keeps `ChannelStripProcessor` DSP untouched

Cons:

- `DryWetMixer` still needs careful integration because it runs on audio thread
- `RoutingMixer` name may imply DSP ownership unless documented as policy-only

### Option B: GlobalMixProcessor + RoutingState/LineMixer + DryWetLatencyCompensator

Modules:

- `GlobalMixProcessor`: endpoint/ramp/mix orchestration
- `DryWetLatencyCompensator`: delay-line state and alignment
- `RoutingState` or `LineMixer`: mode/gain/pan/width policy

Pros:

- smaller conceptual components
- latency compensation can be tested independently

Cons:

- too much decomposition for the current code shape
- increases header/API surface before first extraction is proven
- higher risk of moving state and behavior in multiple places at once
- less sympathetic to the existing `AudioEngine` structure

### Recommendation

Use **Option A**, but implement it in staged form:

1. Start with `DryWetMixer` only.
2. Treat `RoutingMixer` as a later policy module, not as audio DSP.
3. If `DryWetMixer` grows too much, split an internal `DryWetLatencyCompensator` later, after behavior is protected by tests.

## 4. DryWetMixer Boundary

### Should own eventually

- prepared scratch capacity
- scratch channel capacity
- dry scratch buffer
- delayed dry scratch buffer
- dry delay line
- dry delay write index
- dry delay buffer size
- current dry latency sample mirror
- output mix ramp state
- endpoint mix checks
- dry capture helper
- dry latency alignment helper
- sample-accurate mix helper
- oversized block fallback decision helper

### Should NOT own

- `GraphRuntime`
- `RuntimeGraphManager`
- active graph pointer
- graph build or graph publish
- command queue or command applier
- routing mode semantics if `RoutingMixer` is separate
- `InputChain`, `ChannelStrip`, `OutputChain` DSP
- UI/state/preset schema
- logging/diagnostic formatting
- engine enabled/startup/tuner branch decisions

### Safe integration shape

DryWetMixer should not call `runtimeGraphs.getActiveRaw()` and should not own the wet graph. Prefer this shape:

```cpp
class DryWetMixer
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset(float currentMix, bool clearDelayLine);
    void setLatencySamples(int latencySamples) noexcept;
    void setTargetMix(float normalizedMix) noexcept;

    bool canProcessBlock(int numSamples, int numChannels) const noexcept;
    bool isDryEndpointSettled() const noexcept;
    bool isWetEndpointSettled() const noexcept;

    void consumeRamp(int numSamples) noexcept;
    void captureDry(const juce::AudioBuffer<float>& input, int numChannels, int numSamples) noexcept;
    void mixWithCapturedDry(juce::AudioBuffer<float>& wetBuffer, int numChannels, int numSamples) noexcept;
};
```

`AudioEngine` would still orchestrate:

1. set target mix
2. handle engine disabled/startup/tuner/missing graph
3. dry endpoint early return
4. capture dry
5. call `runtime.graph->processBlock`
6. wet endpoint early return
7. mix captured dry with wet

This preserves graph ownership and keeps the audio-thread call chain explicit.

## 5. RoutingMixer Boundary

### Should own eventually

- interpretation of `SwitcherMode`
- line active/mute decisions
- dual parallel compensation constant and helper
- line gain fallback policy (`<= 0.001f` active gain maps to `1.0f`)
- creation of sanitized strip target values for A/B:
  - effective gain
  - pan
  - width
  - muted state if useful for tests

### Should NOT own

- graph build
- graph publish/lifetime
- pedal chain construction
- dry/wet ramp
- dry delay line
- `RuntimeGraphManager`
- `GraphBuilder` topology construction
- `ChannelStripProcessor` gain/pan/width DSP math
- pedal DSP algorithms
- UI/state/preset schema

### Safe integration shape

RoutingMixer should start as policy-only:

```cpp
class RoutingMixer
{
public:
    struct LineInput
    {
        float gain = 1.0f;
        float pan = 0.0f;
        float width = 1.0f;
    };

    struct LineTarget
    {
        float gain = 1.0f;
        float pan = 0.0f;
        float width = 1.0f;
        bool muted = false;
    };

    struct Targets
    {
        LineTarget lineA;
        LineTarget lineB;
    };

    static Targets makeTargets(Nova::SwitcherMode mode,
        const LineInput& lineA,
        const LineInput& lineB) noexcept;

    static float dualParallelCompensation() noexcept;
};
```

`GraphBuilder::applyRuntimeParamsToGraph` would later delegate only the policy calculation, then still call `runtime.stripA->setParams(...)` and `runtime.stripB->setParams(...)`.

## 6. Proposed API

### DryWetMixer initial API (preferred)

```cpp
class DryWetMixer
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset(float currentMix, bool clearDelayLine);
    void clearDelayLine() noexcept;

    void setLatencySamples(int latencySamples) noexcept;
    int getLatencySamples() const noexcept;

    void setTargetMix(float normalizedMix) noexcept;
    float getCurrentMix() const noexcept;

    bool canUseScratch(int numSamples, int numChannels) const noexcept;
    bool shouldReturnDryEndpoint() const noexcept;
    bool shouldReturnWetEndpoint() const noexcept;

    void consumeRamp(int numSamples) noexcept;
    void captureDry(const juce::AudioBuffer<float>& input, int numChannels, int numSamples) noexcept;
    void mixCapturedDryWithWet(juce::AudioBuffer<float>& wetBuffer, int numChannels, int numSamples) noexcept;
};
```

This avoids callbacks and keeps wet graph processing outside the mixer.

### RoutingMixer initial API (preferred)

```cpp
class RoutingMixer
{
public:
    static RoutingTargets makeTargets(const RuntimeGlobalParamsSnapshot& snapshot) noexcept;
};
```

Where `RoutingTargets` contains only effective strip values. No graph or processor pointers.

### API to avoid initially

Avoid:

```cpp
void DryWetMixer::process(GraphRuntime&, juce::AudioBuffer<float>&, juce::MidiBuffer&);
```

Reason: it would couple the mixer to graph runtime and blur audio-thread ownership.

Avoid `std::function` in the audio path. If a callback is ever needed, prefer a templated callback and keep it non-allocating, but current extraction can avoid callbacks entirely.

## 7. Implementation Plan by Phases

### P6C - DryWetMixer tests/policy refinements

Recommended model: `5.3-codex`

Files to touch:

- `Source/Core/AudioEngineTests.cpp`
- `scripts/check-audio-thread-policy.ps1` if needed
- docs report

Allowed work:

- add targeted policy checks that future `DryWetMixer` APIs are not allocation/logging/locking paths
- optionally add one direct helper-level test only if no internals are exposed

Do not touch:

- `AudioEngine.cpp` dry/wet implementation
- graph lifecycle
- DSP processors

Validation:

- 4 builds
- `git diff --check`
- base validation
- golden metrics
- RT Release
- RT stability Release prioritized
- policy scan
- wrapper Fast

Rollback criteria:

- any base validation failure
- RT Release warn/fail increase
- policy `failures > 0` or `contractFailures > 0`

### P6D - Extract pure dry/wet helpers without moving state

Recommended model: `GPT-5.5 xhigh`

Files to touch:

- create `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngine.h`
- docs report

Allowed work:

- move pure endpoint/ramp consumption/mix helper logic as static or stateless helpers
- keep `AudioEngine` as owner of all buffers/ramp/delay state
- keep `processWithSampleAccurateDryWet` wrapper in `AudioEngine`

Do not touch:

- graph ownership
- `RuntimeGraphManager`
- routing mode policy
- `ChannelStripProcessor`

Validation:

- full P6A validation matrix

Rollback criteria:

- dry-only exactness failure
- ramp discontinuity failure
- oversized fallback failure
- any policy active-route failure

### P6E - Extract DryWetMixer wrapper, AudioEngine still owns state

Recommended model: `GPT-5.5 xhigh`

Files to touch:

- `DryWetMixer.h`
- `AudioEngine.h`
- `AudioEngine.cpp`
- tests/report

Allowed work:

- introduce wrapper functions that take explicit state references from `AudioEngine`
- preserve call order in `processWithSampleAccurateDryWet`
- no state migration yet

Do not touch:

- `prepareScratchBuffers` ownership of buffers
- dry delay vector ownership
- routing/ChannelStrip policy

Validation:

- full P6A validation matrix
- inspect policy scan for new audio-thread calls

Rollback criteria:

- any P6A test failure
- any RT Release regression
- any audio-thread policy failure

### P6F - Move dry/wet state into DryWetMixer

Recommended model: `GPT-5.5 xhigh`

Files to touch:

- `DryWetMixer.h`
- `AudioEngine.h`
- `AudioEngine.cpp`
- tests/report

Allowed work:

- move dry scratch buffers, delay line, latency mirror, ramp, capacities into `DryWetMixer`
- keep `AudioEngine::processWithSampleAccurateDryWet` as wrapper/orchestrator initially

Do not touch:

- wet graph ownership/call site
- routing semantics
- graph lifecycle

Validation:

- full P6A validation matrix
- RT stability Release prioritized

Rollback criteria:

- dry-only exactness drift
- latency-aligned dry path drift
- oversized fallback drift
- RT Release downgrade

### P6G - RoutingMixer plan/tests

Recommended model: `5.3-codex`

Files to touch:

- `AudioEngineTests.cpp`
- docs

Allowed work:

- add tests focused on `RoutingMixer` target calculations before extraction
- document exact semantics:
  - line mute rules
  - dual `0.5f` compensation
  - active low-gain fallback
  - pass-through pan/width

Do not touch:

- `GraphBuilder::applyRuntimeParamsToGraph`
- `ChannelStripProcessor`

Validation:

- full P6A validation matrix

Rollback criteria:

- routing mode test failure
- golden metric drift

### P6H - RoutingMixer extraction

Recommended model: `GPT-5.5 xhigh`

Files to touch:

- create `Source/Core/Audio/RoutingMixer.h`
- `GraphBuilder.h`
- tests/report

Allowed work:

- move policy calculations from `GraphBuilder::applyRuntimeParamsToGraph`
- keep `ChannelStripProcessor` DSP unchanged
- keep graph topology unchanged

Do not touch:

- dry/wet
- graph lifecycle
- command semantics
- pedal DSP

Validation:

- full P6A validation matrix
- golden metrics
- RT Release/stability

Rollback criteria:

- LineA/LineB/Dual drift
- dual compensation ratio drift
- pan/width behavior drift
- output limiter activity changes in nominal clean dual test

## 8. Main Risks

- dry-only no longer exact
- wet-only bypasses or loses graph processing
- ramp creates audible clicks/discontinuities
- dry/wet latency alignment changes
- dry delay line reset behavior changes
- dual parallel compensation changes gain
- `LineA_Only` / `LineB_Only` mode drift
- gain low-value fallback changes behavior
- pan/width values are applied in different order or to wrong line
- output limiter receives different nominal level
- oversized block fallback allocates or crashes
- RT Release degradation
- policy scan failure in active audio route

## 9. Required Tests and Gates

P6A tests are non-negotiable for every extraction phase:

- dry-only exactness
- wet-only behavior
- sample-accurate ramp discontinuity
- routing modes
- dual parallel compensation
- gain/pan/width strip controls
- oversized block safety

Required gates:

- 4 builds: SharedCode/Standalone, Debug/Release
- `git diff --check`
- base validation PASS
- golden metrics PASS
- RT Release PASS `16/16/0/0`
- RT stability Release prioritized PASS
- policy scan `failures=0`, `contractFailures=0`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS
- no golden baseline updates
- no new known failures

## 10. Final Recommendation

P6C should be a small guardrail phase only if policy coverage needs tightening. Otherwise P6C can be the first narrow implementation phase: extract **pure dry/wet helpers without moving state**.

Do not start with RoutingMixer. Routing currently touches `GraphBuilder::applyRuntimeParamsToGraph` and `ChannelStripProcessor` semantics, so it has higher tone/gain drift risk. DryWet can be staged more safely because the current code has a clearer state cluster and P6A already covers the important endpoints.

Recommended P6C prompt:

> Tarea: P6C - Extraer helpers puros de DryWetMixer sin mover estado. Crear `Source/Core/Audio/DryWetMixer.h` header-only si es posible. Mover solo helpers puros/estaticos de endpoint, ramp consumption, dry latency copy y sample-accurate mix, manteniendo `AudioEngine` como owner de buffers/ramp/delay state y manteniendo `AudioEngine::processWithSampleAccurateDryWet` como wrapper. No tocar routing, graph lifecycle, RuntimeGraphManager, GraphBuilder topology, DSP processors, golden baselines ni known failures. Ejecutar la matriz completa P6A.

