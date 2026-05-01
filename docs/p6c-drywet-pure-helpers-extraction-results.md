# P6C - DryWet Pure Helpers Extraction Results

Date: 2026-04-30  
Scope: narrow helper extraction only. No dry/wet state moved.

## Files modified

- `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngine.h`
- `docs/p6c-drywet-pure-helpers-extraction-results.md`

No P6A tests or tolerances were modified. No golden baselines were updated.

## Module created

Created header-only `Nova::Audio::DryWetMixer` in `Source/Core/Audio/DryWetMixer.h`.

The class is stateless in P6C. It owns no buffers, no ramp state, no delay line, no graph pointer, and no routing policy.

## Helpers moved

Moved or introduced as stateless helpers:

- `endpointEpsilon()`
- `canUseScratch(...)`
- `isDryEndpointSettled(...)`
- `isWetEndpointSettled(...)`
- `clampLatencyForDelay(...)`
- `consumeRamp(...)`
- `mixWetDrySampleAccurate(...)`
- `copyDryThroughLatency(...)`

The helpers receive all mutable state explicitly by reference or value. `AudioEngine` still owns the data.

## What stayed in AudioEngine

`AudioEngine` still owns:

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
- wet graph processing call
- all routing mode logic

`AudioEngine::processWithSampleAccurateDryWet` remains the control point for call order and graph invocation.

## State movement

No dry/wet state was moved out of `AudioEngine`.

The new header has no member variables. It does not allocate, lock, log, or construct strings from the audio path.

## Endpoint behavior preservation

Endpoint behavior is unchanged:

- epsilon remains `1.0e-5f`
- dry endpoint still requires settled ramp and `currentMix <= epsilon`
- wet endpoint still requires settled ramp and `currentMix >= 1.0f - epsilon`
- dry-only early return still avoids wet graph processing
- wet-only early return still happens after wet graph processing

## Ramp consumption preservation

Ramp consumption is unchanged:

- dry-only endpoint consumes exactly `numSamples` ramp samples
- wet-only endpoint consumes exactly `numSamples` ramp samples
- mixed path consumes one ramp sample per output sample
- sample loop remains outermost and channel loop remains inner for the mix path

## Dry delay order preservation

Dry delay order is unchanged:

- latency clamp still uses `0..dryDelayBufferSize - numSamples - 1`
- zero latency still copies dry input directly
- nonzero latency still reads delayed dry sample before writing current dry sample
- sample loop remains outermost and channel loop remains inner
- write index is copied locally, advanced per sample, then written back once after the block

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
  - final run: `results=158 passes=5998 failures=0 failingResults=0`
  - note: an initial run reported one transient failure in `AudioEngine routing modes and strip controls remain finite and mode-correct`; immediate rerun without code changes passed, and wrapper Fast later passed the same base validation.
- `run-golden-audio-metrics.ps1` PASS against P4 baseline
- `run-rt-profile-scenarios.ps1 Release` PASS
  - `total=16 pass=16 warn=0 fail=0`
- `run-rt-profile-stability.ps1 Release -CiMode -Runs 3` PASS for prioritized scenarios
  - `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.144`
  - `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.180`
  - `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.130`
  - `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.364`
  - one non-prioritized internal run reported `cabinet_nominal` WARN; the prioritized stability gate remained unblocked.
- `check-audio-thread-policy.ps1` PASS/WARN non-blocking
  - `summary.failures=0`
  - `summary.contractFailures=0`
  - `summary.legacyWarnings=4`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

## Behavior impact

No DSP, tone, routing, dry/wet behavior, graph lifecycle, graph build, command semantics, parameter IDs, preset schema, golden baselines, or known failures were changed.

The P6A dry/wet and routing tests remained at `results=158 passes=5998 failures=0 failingResults=0`.

## Remaining risks

- `DryWetMixer` is header-only, so compile surface is slightly broader until a later source split is justified.
- Helpers are not yet directly unit-tested in isolation; behavior remains protected through P6A engine-level tests.
- The initial transient base-validation failure and one non-prioritized RT WARN should be watched, but neither reproduced in the final gates.
- Dry/wet state is still inside `AudioEngine`, so the next phase must preserve the same explicit state references or move state in a separate step.

## Recommendation for P6D

Proceed to a narrow `DryWetMixer` wrapper phase only if the wrapper keeps `AudioEngine` as the owner of dry/wet state.

Recommended P6D constraints:

- no routing changes
- no graph lifecycle changes
- no state migration yet unless explicitly scoped
- keep `AudioEngine::processWithSampleAccurateDryWet` as the orchestrator
- run the full P6A validation matrix again
