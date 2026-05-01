# P6E-B - DryWet Ramp State Migration Results

Date: 2026-04-30  
Scope: move only output mix/ramp state from `AudioEngine` into `DryWetMixer`.

## Files modified

- `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `docs/p6e-b-drywet-ramp-state-migration-results.md`

No tests or tolerances were modified. No golden baselines or known failures were added.

## State moved

Moved the output mix ramp state out of `AudioEngine`:

- previous owner: `AudioEngine::AudioPlane::wetMixRamp`
- previous type: `AudioEngine::SampleAccurateRamp`
- new owner: `Nova::Audio::DryWetMixer`
- new private state: `DryWetMixer::MixRamp wetMixRamp`

The ramp implementation was copied conservatively:

- default current/target: `1.0f`
- default ramp samples: `64`
- `prepare(...)` fallback sample rate: `44100.0`
- minimum ramp time clamp: `0.001` seconds
- target equality epsilon: `1.0e-7f`
- value clamp: `0.0f..1.0f`
- `getNext()` current/target/step settling behavior unchanged

Added stateful `DryWetMixer` APIs:

- `prepareMix(...)`
- `resetMix(...)`
- `setTargetMix(...)`
- `getCurrentMix()`
- `isMixSmoothing()`
- `classifyEndpoint()`
- `consumeRamp(...)`
- `processDryEndpoint(...)`
- `processWetEndpoint(...)`
- instance `mixCapturedDryWithWet(...)`

The existing stateless helpers remain available.

## State still in AudioEngine

`AudioEngine` still owns all non-ramp dry/wet state:

- `dryScratch`
- `delayedDryScratch`
- `dryDelay`
- `dryDelayWriteIndex`
- `dryDelayBufferSize`
- `currentDryLatencySamples`
- `scratchBlockCapacity`
- `scratchChannelCapacity`
- `prepareScratchBuffers(...)`
- `resetDryDelayLine()`
- `updateDryDelayLatency(...)`
- wet graph `processBlock(...)` call
- `processWithSampleAccurateDryWet(...)` orchestration
- routing mode logic

No routing, graph lifecycle, `RuntimeGraphManager`, `GraphBuilder`, global processors, DSP processors, parameter IDs, preset schema, or golden baselines were changed.

## Target mix timing preservation

`AudioEngine::process` still reads `params.getOutputMixNormalized()` and updates the target mix at the same logical point near block entry, before input metering and before engine/tuner/graph branches.

Only the receiver changed:

- before: `audioPlane.wetMixRamp.setTarget(mixTarget)`
- after: `audioPlane.dryWetMixer.setTargetMix(mixTarget)`

`prepareScratchBuffers(...)` and `resetAudioRuntimeState(...)` still reset the mix to `params.getOutputMixNormalized()` at the same lifecycle points.

## Ramp consumption preservation

Ramp consumption count is unchanged:

- dry endpoint: consumes `numSamples`
- wet endpoint: consumes `numSamples` after the graph call
- mixed path: consumes exactly one ramp value per sample inside the outer sample loop

The mixed-path loop order is unchanged:

1. get next wet value for sample `i`
2. compute dry value
3. iterate channels for that sample

## Dry/wet endpoint preservation

Endpoint epsilon remains `1.0e-5f`.

Endpoint classification still uses:

- current mix value
- smoothing state
- dry settled: `!isSmoothing && currentMix <= endpointEpsilon()`
- wet settled: `!isSmoothing && currentMix >= 1.0f - endpointEpsilon()`

Dry endpoint behavior remains an early return before dry capture and before wet graph processing. Wet endpoint behavior remains an early return after wet graph processing and after ramp consumption.

## Call order preservation

`AudioEngine::processWithSampleAccurateDryWet` remains the orchestrator and still performs:

1. empty block/channel guard
2. oversized block fallback check
3. endpoint classification
4. dry endpoint early return
5. dry capture
6. wet graph `processBlock(...)`
7. wet endpoint early return
8. dry-delay copy plus sample-accurate mix

`DryWetMixer` still does not know about `GraphRuntime` and does not call `processBlock(...)`.

## Validation results

Builds:

- `build NOVA_SharedCode Debug x64` PASS, 0 warnings
- `build NOVA_StandalonePlugin Debug x64` PASS, 0 warnings
- `build NOVA_SharedCode Release x64` PASS, 0 warnings
- `build NOVA_StandalonePlugin Release x64` PASS, 0 warnings

Diff hygiene:

- `git diff --check` PASS
- line-ending warnings only; no diff-check errors

Base validation, consecutive runs:

- Run 1: `results=159 passes=6002 failures=0 failingResults=0`
- Run 2: `results=159 passes=6002 failures=0 failingResults=0`

Golden metrics:

- `run-golden-audio-metrics.ps1` PASS against P4 baseline

RT Release single-run:

- `run-rt-profile-scenarios.ps1 Release` PASS
- summary: `total=16 pass=16 warn=0 fail=0`
- selected max budget ratios:
  - `overdrive_cleanamp_reverb_chain_nominal`: `0.101`
  - `stress_block_32`: `0.126`
  - `sample_rate_44100`: `0.104`
  - `sample_rate_96000`: `0.208`

RT Release stability, prioritized, `-CiMode -Runs 3`:

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.109`
- `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.120`
- `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.102`
- `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.233`

Policy scan:

- `check-audio-thread-policy.ps1` WARN non-blocking
- `summary.failures=0`
- `summary.contractFailures=0`
- `summary.legacyWarnings=4`

Wrapper:

- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

## Noise or transients observed

No routing/dry-wet transient reproduced during P6E-B validation.

The hardened dual-parallel tests from P6E-A passed in both consecutive base-validation runs and in the wrapper Fast run.

## Remaining risks

- `DryWetMixer` is now minimally stateful but still header-only; compile surface remains broader than a `.cpp` split.
- Dry buffers and dry-delay state are still in `AudioEngine`; moving those will be a higher-risk ownership migration.
- The control/reporting read of current mix remains equivalent to the previous non-atomic ramp read; this phase did not change thread semantics.
- Long-horizon RT stability should continue to watch `sample_rate_96000`, even though Release stayed clean.

## Recommendation for P6E-C

Proceed with another narrow slice only if it keeps `AudioEngine::processWithSampleAccurateDryWet` as the visible orchestrator.

Recommended P6E-C:

- either move only scratch buffer ownership into `DryWetMixer`, without moving dry-delay state
- or add direct unit coverage for `DryWetMixer` ramp endpoint behavior before moving buffers

Do not move routing, graph processing, latency update ownership, `RuntimeGraphManager`, `GraphBuilder`, or dry-delay write-index ownership in the same phase.
