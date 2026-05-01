# P6E-D - Direct DryWetMixer Tests Results

Date: 2026-04-30  
Scope: add direct `DryWetMixer` coverage before any dry-delay/write-index/latency-state migration.

## Files modified

- `Source/Core/AudioEngineTests.cpp`
- `docs/p6e-d-drywet-direct-tests-results.md`

No production audio behavior was changed.  
No dry-delay/write-index/latency state was moved.  
No test tolerances were changed.  
No golden baseline or known-failure updates were made.

## Tests added

1. `DryWetMixer scratch preparation and fallback boundaries remain canonical`  
   Coverage:
   - `prepareScratch` capacity formula:
     - block capacity = `max(8192, samplesPerBlock * 4)`
     - channel capacity = `max(2, numChannels)`
   - `getScratchBlockCapacity` and `getScratchChannelCapacity`
   - `canUseScratch` and `shouldUseOversizedFallback` boundaries

2. `DryWetMixer capture and mixed output stay deterministic and range-safe`  
   Coverage:
   - direct `captureDry(...)` behavior on explicit test buffers
   - direct `mixCapturedDryWithWet(...)` deterministic output for simple signals
   - no writes past `numSamples`
   - finite output guard (`NaN/Inf` protection)
   - zero-latency path leaves `dryDelayWriteIndex` unchanged

3. `DryWetMixer ramp state and endpoint classification remain stable`  
   Coverage:
   - `prepareMix`, `resetMix`, `setTargetMix`, `getCurrentMix`
   - target clamp behavior (`<0` to `0`, `>1` to `1`)
   - endpoint classification (`Dry`, `Mixed`, `Wet`)
   - ramp consumption count by sample through `consumeRamp`, `processDryEndpoint`, and `processWetEndpoint`

## Determinism hardening applied

During this phase, the existing routing test was hardened to reduce residual cross-subcase state:

- `AudioEngine routing modes and strip controls remain finite and mode-correct`

Hardening change:
- each routing subcase now renders with a fresh, independently prepared `AudioEngine` instance
- warm-up and settle blocks remain explicit and deterministic
- thresholds/tolerances were not relaxed

Failure diagnostics in this test now include:
- `rmsA`, `rmsB`, `rmsDual`
- `dualToMax` ratio
- `outputMixRaw`
- `switchMode`
- `gainA`, `gainB`
- `peakDual`
- `limiterActiveBlocks`

Also retained:
- `AudioEngine dual-parallel nominal does not collapse after settled routing update`

## Minimal testability helpers

No new production helper was required in this phase.  
Changes were test-side only.

## Validation results

Builds:

- `build NOVA_SharedCode Debug x64` PASS
- `build NOVA_StandalonePlugin Debug x64` PASS
- `build NOVA_SharedCode Release x64` PASS
- `build NOVA_StandalonePlugin Release x64` PASS

Diff hygiene:

- `git diff --check` PASS (line-ending warnings only; no diff-check errors)

Base validation (two consecutive runs):

- Run 1: `results=162 passes=6051 failures=0 failingResults=0`
- Run 2: `results=162 passes=6051 failures=0 failingResults=0`

Golden metrics:

- `run-golden-audio-metrics.ps1` PASS against P4 baseline

RT Release single-run:

- `run-rt-profile-scenarios.ps1 Release` PASS
- summary: `total=16 pass=16 warn=0 fail=0`

RT Release stability (prioritized, `-CiMode -Runs 3`):

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.134`
- `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.207`
- `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.128`
- `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.287`

Policy scan:

- `check-audio-thread-policy.ps1` status `WARN` (non-blocking)
- `failureCount=0`
- `contractFailureCount=0`

Wrapper:

- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

## Remaining risks

- Dry-delay ownership is still split from `DryWetMixer`:
  - `dryDelay`
  - `dryDelayWriteIndex`
  - `dryDelayBufferSize`
  - `currentDryLatencySamples`
- The dry-delay migration step remains high-risk for:
  - read/write order drift
  - latency clamp drift
  - zero-latency fast-path drift

## Recommendation for P6E-E

Proceed with a narrow dry-delay migration only if these invariants are kept exact:

- same delay-buffer sizing formula
- same write-index reset timing
- same read-before-write order
- same zero-latency behavior
- same latency clamp behavior
- same oversized fallback (wet-only) behavior

Keep `AudioEngine::processWithSampleAccurateDryWet` as orchestration owner in P6E-E, and move only state/primitive operations first.
