# P6E-E - DryWet Delay State Migration Results

Date: 2026-05-01  
Scope: move only dry-delay and latency-alignment state ownership from `AudioEngine` to `DryWetMixer`.

## Files modified

- `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `docs/p6e-e-drywet-delay-state-migration-results.md`

Validation refreshed existing artifact files under `artifacts/` and `audio-base-test-report.txt`.

No routing logic, graph processing lifecycle, graph builder code, command semantics, schema/IDs, golden baselines, known failures, or test tolerances were changed.

## State moved

`DryWetMixer` now owns the dry/wet runtime state:

- mix/ramp state
- `dryScratch`
- `delayedDryScratch`
- `scratchBlockCapacity`
- `scratchChannelCapacity`
- `dryDelay`
- `dryDelayWriteIndex`
- `dryDelayBufferSize`
- `dryDelayMaxLatencySamples`
- `currentDryLatencySamples`

The dry-delay copy path now uses `DryWetMixer` internal state. The previous public member overload that accepted an external delay buffer/write index was removed; the delay-copy helpers are private implementation details.

## AudioEngine wrappers retained

`AudioEngine` remains the orchestrator and keeps stable lifecycle call sites:

- `prepareScratchBuffers(...)` delegates dry/wet preparation to `DryWetMixer`:
  - `prepareScratch(...)`
  - `prepareDryDelay(Nova::Config::MAX_GRAPH_LATENCY_SAMPLES)`
  - `prepareMix(...)`
  - `resetMix(...)`
- `resetDryDelayLine()` delegates to `audioPlane.dryWetMixer.resetDryDelayLine()`.
- `updateDryDelayLatency(...)` delegates to `audioPlane.dryWetMixer.setLatencySamples(...)`.
- `processWithSampleAccurateDryWet(...)` still owns endpoint classification, dry capture point, wet graph `processBlock(...)` call point, oversized fallback, and final mix orchestration.

`DryWetMixer` does not know `GraphRuntime` and does not call `runtime.graph->processBlock(...)`.

## Behavior preservation

Delay size formula is unchanged:

- `DryWetMixer::prepareDryDelay(...)` computes `dryDelayBufferSize = maxLatencySamples + scratchBlockCapacity + 8`.
- `AudioEngine::prepareScratchBuffers(...)` still passes `Nova::Config::MAX_GRAPH_LATENCY_SAMPLES`.
- Delay channel count still follows `scratchChannelCapacity`.

Write-index reset timing is unchanged:

- prepare-time delay allocation sets `dryDelayWriteIndex = 0`
- runtime resets still flow through `AudioEngine::resetAudioRuntimeState(...) -> resetDryDelayLine() -> DryWetMixer::resetDryDelayLine()`
- `resetDryDelayLine()` still clears the delay line before setting `dryDelayWriteIndex = 0`

Read-before-write order is unchanged:

- per sample, `readIndex` is computed from the current write index
- each channel reads the delayed sample into `dryOut`
- only after that read, the current dry input is written into `dryDelay[writeIndex]`
- sample loop remains outer loop; channel loop remains inner loop
- write index increments after all channels for the sample are processed

Zero-latency behavior is unchanged:

- latency `0` copies dry input directly to delayed dry output
- the delay line is not written
- the write index is not advanced
- direct coverage now checks the internal `DryWetMixer` write index remains unchanged

Latency clamp behavior is unchanged:

- published graph latency is clamped through `DryWetMixer::setLatencySamples(...)` to the prepared max latency
- block-time copy still clamps to `max(0, dryDelayBufferSize - numSamples - 1)`

Oversized fallback is unchanged:

- `AudioEngine::processWithSampleAccurateDryWet(...)` checks `dryWetMixer.shouldUseOversizedFallback(numSamples)` before dry capture
- oversized blocks run wet graph only and return
- no dry capture, no dry/wet mix, no allocation

Ramp consumption, sample loop order, channel loop order, dry endpoint, wet endpoint, and mixed endpoint behavior were left intact.

## Validation

Builds:

- `build NOVA_SharedCode Debug x64` PASS, 0 warnings
- `build NOVA_StandalonePlugin Debug x64` PASS, 0 warnings
- `build NOVA_SharedCode Release x64` PASS, 0 warnings
- `build NOVA_StandalonePlugin Release x64` PASS, 0 warnings

Diff hygiene:

- `git diff --check` PASS
- observed only Git line-ending warnings; no whitespace errors

Base validation, consecutive runs:

- Run 1: `results=162 passes=6053 failures=0 failingResults=0`
- Run 2: `results=162 passes=6053 failures=0 failingResults=0`

Golden metrics:

- `run-golden-audio-metrics.ps1` PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`

RT Release single-run:

- `run-rt-profile-scenarios.ps1 -Configuration Release` PASS
- summary: `total=16 pass=16 warn=0 fail=0`

RT Release stability, prioritized, `-CiMode -Runs 3`:

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.095`
- `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.123`
- `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.091`
- `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.194`

Policy scan:

- `check-audio-thread-policy.ps1` status `WARN` (non-blocking legacy warnings)
- `failureCount=0`
- `contractFailureCount=0`
- `legacyWarnings=4`

Wrapper:

- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS
- Fast mode base validation: `results=162 passes=6053 failures=0 failingResults=0`
- Fast mode RT Release: `total=16 pass=16 warn=0 fail=0`
- Fast mode policy scan: `WARN`, with `failures=0` and `contractFailures=0`

## Noise or transients observed

- No audio validation failures or dry/wet transient regressions were observed.
- No RT profile warnings or failures were observed in Release.
- `git diff --check` reported line-ending warnings only.
- Policy scan remained at existing non-blocking legacy `WARN` state with zero failures.

## Remaining risks

- `DryWetMixer` remains header-only, so future changes still recompile broad audio/test surfaces.
- The delay line remains `std::vector<std::vector<float>>`, matching the pre-migration storage behavior rather than switching to `juce::AudioBuffer<float>`.
- `AudioEngine` intentionally keeps wrappers for lifecycle call-site stability; future cleanup should avoid moving orchestration into `DryWetMixer`.

## Recommendation for P6F

Proceed only with boundary cleanup or additional focused coverage. Keep graph processing, routing mode logic, graph lifecycle, and command semantics outside `DryWetMixer`.

Recommended P6F direction:

- add focused nonzero-latency direct coverage before changing delay internals further
- optionally split `DryWetMixer` implementation out of the header if project structure allows it
- do not move wet graph calls or routing decisions into `DryWetMixer`
