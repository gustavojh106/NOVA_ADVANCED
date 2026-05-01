# P5H - RuntimeGraphManager Design

Date: 2026-04-30

## Scope

This document prepares a future `RuntimeGraphManager` extraction. P5H does not implement the manager and does not move active graph ownership.

The goal for the future phase is to isolate active graph lifetime without changing graph build, graph publish order, dry/wet latency behavior, routing, DSP, runtime params, diagnostics, or command semantics.

## Current Ownership State

`AudioEngine` still owns the runtime graph lifetime boundary.

Current active graph state:

- `ControlPlane::activeOwner`
  - `std::shared_ptr<GraphRuntime>`
  - owns the currently published graph from the control side
- `AudioPlane::activeGraphRaw`
  - `std::atomic<GraphRuntime*>`
  - audio-thread view of the active graph
- `ControlPlane::activeOwnerLock`
  - guards `activeOwner` and retired graph cleanup
- `ControlPlane::retiredGraphs`
  - `Nova::Audio::GraphRetirementQueue<GraphRuntime>`
  - stores old graph owners until their release block
- `AudioPlane::audioBlockCounter`
  - atomic block counter incremented by `AudioEngine::process`
  - used by control-plane cleanup and retirement release calculations

Current publish path:

1. `publishGraph` receives a fully built `std::shared_ptr<GraphRuntime>`.
2. It loads `nowBlock` from `audioBlockCounter`.
3. It locks `activeOwnerLock`.
4. It moves the old `activeOwner` into a local `oldGraph`.
5. It moves the new graph into `activeOwner`.
6. It stores `activeOwner.get()` into `activeGraphRaw` with release ordering.
7. It calls `updateDryDelayLatency(activeOwner->latencySamples)`.
8. It retires `oldGraph` with `nowBlock + kGraphRetireGraceBlocks`.
9. It unlocks.
10. It calls `cleanupRetiredGraphs()`.

Current cleanup path:

- `cleanupRetiredGraphs` loads `audioBlockCounter`, takes `activeOwnerLock`, and calls `GraphRetirementQueue::cleanup`.
- Cleanup is called from control/lifecycle paths only:
  - after `publishGraph`
  - `flushPendingGraphCommands` early path
  - `flushPendingGraphCommands` params-only path
  - `AudioEngine::run`
  - destructor clear
- `AudioEngine::process` does not call cleanup.

Current latency behavior:

- `buildGraphFromModelLocked` sets `GraphRuntime::latencySamples` from `graph.getLatencySamples()`.
- `publishGraph` mirrors that value into the dry/wet path by calling `updateDryDelayLatency`.
- `getLatencyNumSamples()` currently reads the active raw graph and returns `runtime->latencySamples`.
- Bypass latency changes happen in `applyPedalBypassToActiveGraph`, which rebuilds the active graph in place and calls `updateDryDelayLatency(runtime->latencySamples)`.

## Proposed Boundary

`RuntimeGraphManager` should own active graph lifetime and raw pointer publication. It should not build graphs and should not understand pedal models.

State it should own:

- `mutable juce::CriticalSection activeOwnerLock`
- `std::shared_ptr<GraphRuntime> activeOwner`
- `std::atomic<GraphRuntime*> activeGraphRaw`
- `Nova::Audio::GraphRetirementQueue<GraphRuntime> retiredGraphs`
- active latency mirror, for example `std::atomic<int> activeLatencySamples`
- graph retire grace block constant or constructor parameter, preserving the current value `8`

State it should not own:

- `audioBlockCounter`
- pedal model chains
- `GraphCommand`
- `AudioEngineCommandQueue`
- `GraphCommandApplier`
- graph build functions
- runtime parameter snapshots
- dry/wet buffers and dry delay line
- health monitor
- CPU meter
- tuner state
- diagnostics string building

The manager should receive the current audio block from `AudioEngine`; it should not increment or own the audio block counter.

## Minimal API

Preferred future API, templated over `GraphRuntime`:

```cpp
template <typename GraphRuntime>
class RuntimeGraphManager
{
public:
    GraphRuntime* getActiveRaw() const noexcept;

    template <typename LatencyChanged>
    bool publish(std::shared_ptr<GraphRuntime> newGraph,
        uint64_t currentAudioBlock,
        LatencyChanged&& onLatencyChanged);

    std::shared_ptr<GraphRuntime> getActiveOwnerForControl() const;
    void cleanupRetired(uint64_t currentAudioBlock);
    void clear();
    int getLatencySamples() const noexcept;
    size_t retiredSize() const noexcept;
};
```

The `publish` method should call `onLatencyChanged(activeOwner->latencySamples)` after raw pointer publication and before retiring the old graph, matching the current order.

Example future use from `AudioEngine::publishGraph`:

```cpp
const auto nowBlock = audioPlane.audioBlockCounter.load(std::memory_order_acquire);
runtimeGraphs.publish(std::move(newGraph), nowBlock,
    [this](int latencySamples) noexcept
    {
        updateDryDelayLatency(latencySamples);
    });
cleanupRetiredGraphs();
```

The callback must be a no-allocation callable and should be invoked only on the control path. A templated callback avoids the type erasure and potential allocation risks of `std::function`.

If a non-template API is preferred later, use a plain hook struct instead of `std::function`:

```cpp
struct LatencyChangedHook
{
    void* context = nullptr;
    void (*callback)(void* context, int latencySamples) noexcept = nullptr;
};
```

`std::function` is not recommended for the publish path because preserving order does not require type erasure, and the future manager should avoid adding hidden allocation behavior.

## Contracts And Invariants

Audio thread:

- May call only `getActiveRaw()`.
- Must only perform an atomic raw pointer load.
- Must not take `activeOwnerLock`.
- Must not increment or copy `std::shared_ptr`.
- Must not call `publish`, `cleanupRetired`, `clear`, or `getActiveOwnerForControl`.

Control thread / message thread / engine thread:

- May call `publish`.
- May call `cleanupRetired`.
- May call `clear` during shutdown.
- May call `getActiveOwnerForControl` for diagnostics and node lookup.

Publish order:

1. Move old active owner aside.
2. Move new graph into active owner.
3. Store active raw pointer with release ordering.
4. Mirror latency to dry/wet path.
5. Retire old graph with `currentAudioBlock + 8`.
6. Release lock.
7. Cleanup retired graphs from control path.

Retirement:

- Old graph retirement must happen after raw pointer publication.
- Grace period remains 8 audio blocks.
- Release condition remains `releaseAfterAudioBlock <= currentAudioBlock`.
- Memory bound remains 8 retired graphs.
- Cleanup must not run from `AudioEngine::process`.

Graph build and model:

- No graph allocation or graph build inside `RuntimeGraphManager`.
- No pedal model mutation inside `RuntimeGraphManager`.
- No runtime parameter application inside `RuntimeGraphManager`.
- No routing or dry/wet decisions inside `RuntimeGraphManager`.

Latency:

- Manager may mirror active latency for public reads.
- Dry delay latency update remains an `AudioEngine`/dry-wet responsibility.
- Bypass in-place graph rebuild remains outside the manager unless a later phase explicitly designs an active-graph mutation API.

Diagnostics:

- `getActiveOwnerForControl` may return a `shared_ptr` copy under lock for control-thread diagnostics.
- Diagnostic string construction stays outside the manager.

## Functions That Stay In AudioEngine

For the initial implementation phase, these should remain in `AudioEngine`:

- `buildGraphFromModelLocked`
- `connectRuntimeChain`
- `applyRuntimeParamsToGraph`
- `applyPedalBypassToActiveGraph`
- `flushPendingGraphCommands`
- `requestControlGraphRebuild`
- `updateDryDelayLatency`
- `process`
- dry/wet processing
- health monitor actions
- diagnostics report formatting
- tuner flow
- CPU meter use

`publishGraph` may remain as a thin wrapper in `AudioEngine` during P5I. That keeps the external call sites stable while the internals delegate to the manager.

## Future P5I Implementation Plan

Files to touch:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- new `Source/Core/Audio/RuntimeGraphManager.h`
- `Source/Core/Audio/GraphRetirementQueue.h` only if a small API addition is needed
- `Source/Core/AudioEngineTests.cpp` only for targeted coverage adjustments
- `NOVA.jucer` only if a `.cpp` is created

Recommended implementation sequence:

1. Add a header-only `RuntimeGraphManager<GraphRuntime>` with the API above.
2. Move `activeOwnerLock`, `activeOwner`, `activeGraphRaw`, and `retiredGraphs` into the manager.
3. Keep `audioBlockCounter` in `AudioPlane`.
4. Keep `kGraphRetireGraceBlocks = 8` unchanged, either as a manager constant or constructor argument.
5. Keep `AudioEngine::publishGraph` as the only call site that delegates to manager `publish`.
6. Use a templated no-allocation latency callback so `updateDryDelayLatency` keeps the current order.
7. Convert `getLatencyNumSamples`, `getNodes`, `getProcessorForPedal`, diagnostics, and processing raw-load call sites to use the manager.
8. Convert destructor clear and `cleanupRetiredGraphs` to delegate to the manager.
9. Leave graph build, graph command flush, bypass in-place graph rebuild, runtime params, dry/wet, routing, health, CPU, tuner, and diagnostics formatting untouched.

What not to touch in P5I:

- `GraphBuilder`
- `RoutingMixer`
- `DryWetMixer`
- DSP processors
- parameter IDs
- preset schema
- golden baselines
- known failure lists
- command semantics
- graph build topology

Tests to run in P5I:

- `NOVA_SharedCode` Debug x64
- `NOVA_StandalonePlugin` Debug x64
- `NOVA_SharedCode` Release x64
- `NOVA_StandalonePlugin` Release x64
- `git diff --check`
- `scripts/run-base-audio-validation.ps1`
- `scripts/run-golden-audio-metrics.ps1`
- `scripts/run-rt-profile-scenarios.ps1` Debug
- `scripts/run-rt-profile-scenarios.ps1` Release
- `scripts/check-audio-thread-policy.ps1`

Targeted tests already available after P5H:

- `GraphRetirementQueue preserves grace period and bounded cleanup`
- `AudioEngine publishes an active graph immediately after prepare`
- `AudioEngine clean path remains stable after topology swaps`
- `AudioEngine add and remove during deterministic processing stays finite`
- `AudioEngine rebuilds graph latency when bypass changes node latency`
- `AudioEngine ClearAll followed by add rebuilds to the new topology`
- `AudioEngine preserves command order across batched topology edits`
- existing engine disable/re-enable refresh tests

Rollback criteria:

- Any crash or hang during validation.
- Any active-route policy FAIL.
- Any base validation failure.
- Any golden metric failure.
- Release RT status downgrade from clean `16/16/0/0`.
- New broad Debug RT degradation beyond the existing 96 kHz warning class.
- Any behavior suggesting `activeGraphRaw` can become null after `prepare`.
- Any evidence of `shared_ptr` or lock use entering `AudioEngine::process`.
- Any change to graph publish order or retired graph grace behavior.
