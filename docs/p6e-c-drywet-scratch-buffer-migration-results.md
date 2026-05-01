# P6E-C - DryWet Scratch Buffer Migration Results

Date: 2026-04-30  
Scope: move only dry/wet scratch buffer ownership from `AudioEngine` into `DryWetMixer`.

## Files modified

- `Source/Core/Audio/DryWetMixer.h`
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `docs/p6e-c-drywet-scratch-buffer-migration-results.md`

No tests or tolerances were modified. No golden baselines or known failures were added.

## State moved

Moved these fields out of `AudioEngine::AudioPlane` and into `Nova::Audio::DryWetMixer`:

- `dryScratch`
- `delayedDryScratch`
- `scratchBlockCapacity`
- `scratchChannelCapacity`

Added `DryWetMixer` APIs for the new ownership:

- `prepareScratch(...)`
- `getScratchBlockCapacity()`
- `getScratchChannelCapacity()`
- `getProcessChannelCount(...)`
- instance `canUseScratch(...)`
- instance `shouldUseOversizedFallback(...)`
- instance `captureDry(...)`
- instance `mixCapturedDryWithWet(...)`

The previous stateless helpers remain available for compatibility and direct helper use.

## State still in AudioEngine

`AudioEngine` still owns all dry-delay and latency state:

- `dryDelay`
- `dryDelayWriteIndex`
- `dryDelayBufferSize`
- `currentDryLatencySamples`
- `resetDryDelayLine()`
- `updateDryDelayLatency(...)`

`AudioEngine` also still owns:

- wet graph `processBlock(...)` call
- `processWithSampleAccurateDryWet(...)` orchestration
- routing mode logic
- `RuntimeGraphManager`
- `GraphBuilder`
- `ChannelStripProcessor`
- `OutputChainProcessor`
- `InputChainProcessor`
- all DSP processors

## Scratch capacity preservation

Capacity behavior remains the same:

- block capacity is still `max(kMinimumScratchBlocks, samplesPerBlock * 4)`
- channel capacity is still `max(2, numChannels)`
- both scratch buffers are still sized with `setSize(channels, capacitySamples, false, false, true)`
- both buffers are still cleared during preparation

`AudioEngine::prepareScratchBuffers(...)` remains as the lifecycle wrapper. It now delegates scratch preparation to `DryWetMixer`, then reads the resulting capacity and channel count to size `AudioEngine`'s dry-delay line exactly as before:

- delay size remains `MAX_GRAPH_LATENCY_SAMPLES + capacitySamples + 8`
- dry-delay channel count remains the scratch channel capacity

No allocation was added to `AudioEngine::process`.

## Dry capture point preservation

The dry capture point is unchanged. In `AudioEngine::processWithSampleAccurateDryWet`:

1. endpoint classification still happens first
2. dry endpoint still returns before capture and before graph processing
3. `DryWetMixer::captureDry(...)` is called after the dry endpoint check
4. wet graph `processBlock(...)` is called immediately after dry capture

Only the storage target changed: capture now writes to `DryWetMixer`'s internal `dryScratch`.

## Oversized fallback preservation

Oversized fallback still compares `numSamples` against the same scratch block capacity.

When triggered, behavior remains:

- call wet graph `processBlock(...)`
- return immediately
- do not capture dry
- do not mix dry/wet
- do not allocate

## Wet endpoint preservation

Wet endpoint behavior is unchanged:

- dry input is still captured before the graph call
- wet graph processing still runs
- ramp is still consumed for exactly `numSamples`
- return still happens before dry-delay copy and dry/wet mix

Mixed path still uses the same dry-delay helper and sample-accurate mix helper, now with internal scratch buffers.

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
  - `overdrive_cleanamp_reverb_chain_nominal`: `0.124`
  - `stress_block_32`: `0.119`
  - `sample_rate_44100`: `0.127`
  - `sample_rate_96000`: `0.218`

RT Release stability, prioritized, `-CiMode -Runs 3`:

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.137`
- `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.119`
- `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.094`
- `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.213`

Policy scan:

- `check-audio-thread-policy.ps1` WARN non-blocking
- `summary.failures=0`
- `summary.contractFailures=0`
- `summary.legacyWarnings=4`

Wrapper:

- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

## Noise or transients observed

No routing/dry-wet transient reproduced during P6E-C.

The hardened dual-parallel tests from P6E-A passed in both consecutive base-validation runs and in the wrapper Fast run.

## Remaining risks

- `DryWetMixer` now owns mix/ramp plus scratch buffers, but dry-delay state remains split in `AudioEngine`.
- Moving dry-delay ownership will be higher risk because it includes write-index state and latency-aligned read/write order.
- `DryWetMixer` remains header-only, so compile surface continues to grow.
- Long-horizon RT stability should continue monitoring Release, especially `sample_rate_96000` and chain-heavy scenarios.

## Recommendation for P6E-D

Do not move routing or graph processing next.

Recommended P6E-D:

- add focused helper-level tests for `DryWetMixer` scratch fallback, capture, and mix behavior before moving dry-delay state
- or move only dry-delay storage and write index into `DryWetMixer`, keeping `currentDryLatencySamples`, `updateDryDelayLatency(...)`, and `AudioEngine::processWithSampleAccurateDryWet` orchestration in `AudioEngine`

If dry-delay state moves next, preserve exactly:

- delay size formula
- write-index reset timing
- read-before-write order
- latency clamp
- zero-latency copy path
- oversized fallback wet-only behavior
