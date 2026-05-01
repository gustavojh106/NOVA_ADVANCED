# P6F - DryWetMixer Post-Migration Verification Results

Date: 2026-05-01  
Scope: verify and close the stateful `DryWetMixer` migration before any RoutingMixer work.

## Files modified

- `Source/Core/AudioEngineTests.cpp`
- `scripts/check-audio-thread-policy.ps1`
- `docs/p6f-drywet-post-migration-verification-results.md`

Validation refreshed existing generated artifacts under `artifacts/` and `audio-base-test-report.txt`.

No DSP, tone, routing behavior, graph lifecycle, command semantics, IDs/schema, golden baselines, known failures, or existing test tolerances were changed.

## Tests added

Added direct `DryWetMixer` coverage:

1. `DryWetMixer nonzero latency preserves read-before-write and write-index state`
   - latency `> 0` uses read-before-write behavior
   - first delayed block outputs initial delay-line history before current dry samples
   - second delayed block outputs previous block tail according to latency
   - write index advances exactly `numSamples` per processed block
   - sample loop and channel loop expectations are encoded directly in output samples

2. `DryWetMixer delay reset and latency clamps remain stable`
   - `setLatencySamples(...)` clamps negative input to `0`
   - `setLatencySamples(...)` clamps above prepared max latency to prepared max
   - block-time clamp is covered via `clampLatencyForDelay(...)`
   - zero-latency still copies dry directly and does not advance write index
   - `resetDryDelayLine()` clears delay history and returns write index to `0`
   - first samples after reset are zero for nonzero latency

Existing AudioEngine coverage already continues to exercise wrapper/orchestration behavior:

- dry-only path stays exact with a latency-relevant wet chain
- graph latency updates across bypass/unbypass
- mixed-chain latency stays stable across equivalent rebuilds and bypass/unbypass
- `processWithSampleAccurateDryWet(...)` remains the graph-call orchestrator

## Policy scan reinforcement

Updated `scripts/check-audio-thread-policy.ps1` with P6F contract checks:

- `drywet_no_graphruntime`
- `drywet_no_processblock`
- `drywet_no_session_logger`
- `drywet_no_juce_string`
- `drywet_orchestrator_keeps_graph_call`

Final policy result:

- status `WARN` from existing non-blocking legacy warnings
- `failures=0`
- `contractFailures=0`
- `contractChecks=15`
- new DryWetMixer checks all passed
- `AudioEngine::processWithSampleAccurateDryWet` graph-call check passed with `callCount=2`

## Invariants covered

Dry delay:

- nonzero latency reads old delay-line contents before writing current dry samples
- write index advances exactly by processed sample count on nonzero latency
- zero latency does not write delay state and does not advance write index
- reset clears delay history and resets write index to zero
- latency clamps against prepared max latency
- block-time clamp stays bounded by `dryDelayBufferSize - numSamples - 1`

Ownership:

- `DryWetMixer` owns mix/ramp, scratch buffers, dry-delay storage, write index, delay size, max latency, and current latency state
- `DryWetMixer` does not know `GraphRuntime`
- `DryWetMixer` does not call or reference `processBlock`
- `DryWetMixer` contains no `SessionLogger::logEvent`
- `DryWetMixer` contains no `juce::String`

AudioEngine orchestration:

- `AudioEngine::prepareScratchBuffers(...)` remains the preparation wrapper
- `AudioEngine::resetDryDelayLine()` remains the reset wrapper
- `AudioEngine::updateDryDelayLatency(...)` remains the latency publication wrapper
- `AudioEngine::processWithSampleAccurateDryWet(...)` still owns endpoint selection, dry capture point, oversized fallback, wet graph `processBlock(...)` call, and final dry/wet mix call

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

- Run 1: `results=164 passes=6103 failures=0 failingResults=0`
- Run 2: `results=164 passes=6103 failures=0 failingResults=0`
- Count increased from P6E-E baseline `results=162 passes=6053 failures=0 failingResults=0`

Golden metrics:

- `run-golden-audio-metrics.ps1` PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`
- no golden baseline update

RT Release single-run:

- `run-rt-profile-scenarios.ps1 -Configuration Release` PASS
- summary: `total=16 pass=16 warn=0 fail=0`

RT Release stability, prioritized, `-CiMode -Runs 3`:

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.104`
- `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.150`
- `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.092`
- `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.242`

Policy scan:

- `check-audio-thread-policy.ps1` status `WARN` (non-blocking legacy warnings)
- `failureCount=0`
- `contractFailureCount=0`
- `legacyWarnings=4`

Wrapper:

- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS
- Fast mode base validation: `results=164 passes=6103 failures=0 failingResults=0`
- Fast mode RT Release: `total=16 pass=16 warn=0 fail=0`
- Fast mode policy scan: `WARN`, with `failures=0` and `contractFailures=0`

## Noise or transients observed

- No audio validation failures or dry/wet behavior regressions observed.
- No RT Release warnings or failures observed.
- `git diff --check` produced line-ending warnings only.
- Policy scan remains `WARN` only because of existing non-blocking legacy findings.

## Remaining risks

- `DryWetMixer` is still header-only; broader compile surface remains.
- The delay line still uses `std::vector<std::vector<float>>`, matching the migrated implementation rather than converting to `juce::AudioBuffer<float>`.
- Direct coverage now covers nonzero delay behavior, but broader future refactors should still avoid touching graph processing and routing in the same phase.

## Recommendation for P6G

Consider `DryWetMixer` post-migration stable.

Recommended P6G:

- start RoutingMixer only as a separate scoped extraction
- keep wet graph calls and graph lifecycle in `AudioEngine`
- preserve existing dry/wet tests as guardrails
- do not combine routing extraction with DSP or latency-state changes
