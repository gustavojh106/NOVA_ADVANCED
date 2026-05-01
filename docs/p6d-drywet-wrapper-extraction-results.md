# P6D - DryWet Wrapper Extraction Results

Date: 2026-04-30  
Scope: wrapper-only dry/wet extraction. No dry/wet state moved.

## Files modified

- `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/AudioEngine.cpp`
- `docs/p6d-drywet-wrapper-extraction-results.md`

`Source/Core/AudioEngine.h` was not changed in P6D.

No tests or tolerances were modified. No golden baselines were updated.

## Wrappers added

Added stateless wrappers to `Nova::Audio::DryWetMixer`:

- `EndpointPath`
- `shouldUseOversizedFallback(...)`
- `classifyEndpoint(...)`
- `processDryEndpoint(...)`
- `processWetEndpoint(...)`
- `captureDry(...)`
- `mixCapturedDryWithWet(...)`

Existing pure helpers remain available:

- `endpointEpsilon()`
- `canUseScratch(...)`
- `isDryEndpointSettled(...)`
- `isWetEndpointSettled(...)`
- `clampLatencyForDelay(...)`
- `consumeRamp(...)`
- `mixWetDrySampleAccurate(...)`
- `copyDryThroughLatency(...)`

## Logic moved out of AudioEngine

`AudioEngine::processWithSampleAccurateDryWet` now delegates these small steps to `DryWetMixer`:

- oversized-block fallback decision
- dry/wet/mixed endpoint classification
- dry endpoint ramp consumption
- dry scratch capture
- wet endpoint ramp consumption
- dry latency copy plus final sample-accurate dry/wet mix

`AudioEngine` still performs the wet graph call directly. `DryWetMixer` does not know about `GraphRuntime` or `juce::AudioProcessorGraph`.

## State still owned by AudioEngine

No dry/wet state moved. `AudioEngine` still owns:

- `dryScratch`
- `delayedDryScratch`
- `dryDelay`
- `dryDelayWriteIndex`
- `dryDelayBufferSize`
- `currentDryLatencySamples`
- `wetMixRamp`
- scratch capacities
- `prepareScratchBuffers(...)`
- `resetDryDelayLine()`
- `updateDryDelayLatency(...)`
- `processWithSampleAccurateDryWet(...)` orchestration

## Call order preservation

The call order remains:

1. reject empty block/channel count
2. oversized fallback check
3. dry endpoint early return before dry capture and before graph processing
4. capture dry input
5. call `runtime.graph->processBlock(buffer, midi)`
6. wet endpoint early return after graph processing
7. dry-delay copy and sample-accurate mix

This is the same logical order as P6C.

## Dry/wet endpoint preservation

Endpoint epsilon remains `1.0e-5f`.

Dry endpoint behavior remains:

- requires settled ramp
- requires `currentMix <= endpointEpsilon()`
- consumes exactly one ramp value per sample
- does not capture dry
- does not call the wet graph

Wet endpoint behavior remains:

- requires settled ramp
- requires `currentMix >= 1.0f - endpointEpsilon()`
- captures dry before graph processing, as before
- calls the wet graph
- consumes exactly one ramp value per sample after the graph
- returns before dry/wet mix

## Ramp consumption preservation

Ramp consumption count is unchanged:

- dry endpoint: `numSamples`
- wet endpoint: `numSamples`
- mixed path: `numSamples`

The mixed path still consumes ramp values in the outer sample loop before iterating channels.

## Oversized fallback preservation

Oversized fallback still checks `numSamples > scratchBlockCapacity`.

When triggered, `AudioEngine` still calls `runtime.graph->processBlock(buffer, midi)` directly and returns without allocation, dry capture, latency copy, or dry/wet mix.

## Validation results

Builds:

- `build NOVA_SharedCode Debug x64` PASS, 0 warnings
- `build NOVA_StandalonePlugin Debug x64` PASS, 0 warnings
- `build NOVA_SharedCode Release x64` PASS, 0 warnings
- `build NOVA_StandalonePlugin Release x64` PASS, 0 warnings

Diff hygiene:

- `git diff --check` PASS
- line-ending warnings only; no diff-check errors

Functional validation:

- `run-base-audio-validation.ps1` PASS
  - `results=158 passes=5998 failures=0 failingResults=0`
- `run-golden-audio-metrics.ps1` PASS against P4 baseline
- `run-rt-profile-scenarios.ps1 Release` PASS
  - `total=16 pass=16 warn=0 fail=0`
- `run-rt-profile-stability.ps1 Release -CiMode -Runs 3` PASS for prioritized scenarios
  - `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.139`
  - `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.151`
  - `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.118`
  - `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.265`
- `check-audio-thread-policy.ps1` PASS/WARN non-blocking
  - `summary.failures=0`
  - `summary.contractFailures=0`
  - `summary.legacyWarnings=4`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS on final run

## Noise observed

`run-audio-quality-gates.ps1 -Fast -Configuration Release` hit the same transient base-validation failure previously observed in P6C:

- `AudioEngine routing modes and strip controls remain finite and mode-correct`
- failure detail: `Dual-parallel should not collapse to near-silence`

The gate was rerun immediately without code changes and passed. Standalone base validation had already passed before the wrapper gate, and the final wrapper gate passed.

No thresholds, tolerances, baselines, or known failures were changed.

## Behavior impact

No DSP, tone, routing, dry/wet behavior, graph lifecycle, graph build, command semantics, UI, parameter IDs, preset schema, golden baselines, or known failures were changed.

P6A coverage remains at `results=158 passes=5998 failures=0 failingResults=0`.

## Remaining risks

- The repeated transient routing test noise should be handled in a later coverage-hardening phase if it continues to appear.
- `DryWetMixer` is still header-only and stateless; compile surface remains slightly broader than a `.cpp` split.
- The next stateful phase is higher risk because moving buffers/ramp/delay into `DryWetMixer` will change ownership, not just call shape.

## Recommendation for P6E

Proceed only if P6E keeps the scope narrow:

- either add direct wrapper-level tests for `DryWetMixer` without exposing audio internals
- or move dry/wet state in one clearly bounded step while keeping `AudioEngine::processWithSampleAccurateDryWet` as the visible orchestrator

Recommended constraints for P6E:

- no routing extraction
- no graph lifecycle changes
- no `GraphRuntime` dependency in `DryWetMixer`
- no callbacks or `std::function` in the audio path
- full P6A validation matrix required again
