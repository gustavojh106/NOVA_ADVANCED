# P6A - DryWet and Routing Pre-Extraction Tests Results

Date: 2026-04-30  
Scope: coverage-only phase (no dry/wet or routing extraction, no DSP behavior changes)

## Files modified

- `Source/Core/AudioEngineTests.cpp`
- `docs/p6a-drywet-routing-pre-extraction-tests-results.md` (this report)

No runtime audio code (`AudioEngine.cpp`, DSP processors, routing implementation) was modified in P6A.

## Tests added

Added in `Source/Core/AudioEngineTests.cpp`:

1. `AudioEngine dry-only path stays exact with latency-relevant wet chain`  
2. `AudioEngine wet-only path reflects topology changes and stays audible`  
3. `AudioEngine sample-accurate dry-wet ramp avoids abrupt discontinuities`  
4. `AudioEngine routing modes and strip controls remain finite and mode-correct`  
5. `AudioEngine oversized process blocks stay safe and finite`  

Also added helper:

- `computeAdjacentDeltaPeakRange(...)` for deterministic discontinuity checks around mix transitions.

## Invariants covered by each test

### 1) Dry-only exactness

Test: `AudioEngine dry-only path stays exact with latency-relevant wet chain`

Covers:

- global mix `0%` keeps output on dry path
- wet chain presence (including latency-relevant pedals) does not alter audible output when dry-only
- output remains finite (no NaN/Inf)
- no drift across multiple blocks
- latency reporting remains stable while dry-only is active

### 2) Wet-only behavior

Test: `AudioEngine wet-only path reflects topology changes and stays audible`

Covers:

- global mix `100%` follows wet/graph path
- output remains finite
- output remains audible (no accidental silence)
- topology changes (add/remove pedal) affect wet output and restore after revert

### 3) Sample-accurate dry/wet ramp

Test: `AudioEngine sample-accurate dry-wet ramp avoids abrupt discontinuities`

Covers:

- deterministic dry->wet and wet->dry transitions under live mix updates
- continuity guard using peak sample-to-sample delta in transition windows vs steady windows
- finite output throughout transition

### 4) Routing modes + dual compensation + strip controls

Test: `AudioEngine routing modes and strip controls remain finite and mode-correct`

Covers:

- `LineA only`, `LineB only`, `Dual parallel`
- mode switching produces finite, audible output
- line-mode outputs remain distinct with different line chains
- dual output remains bounded (no dangerous gain blow-up, no near-silence collapse)
- line gain changes are observable in dual mode
- pan variants are finite/audible and observably different
- width variants are finite and observably different
- clean nominal dual compensation stays near unity and does not trigger limiter activity

### 5) Fallback/block-size safety

Test: `AudioEngine oversized process blocks stay safe and finite`

Covers:

- oversized process buffer (host contract violation case) does not crash
- output remains finite and audible in fallback path

## Tolerances used

Main thresholds introduced:

- dry-only null RMS exactness with wet chain: `maxNullRms < 2.5e-4`
- wet-path topology delta: `nullRms > 3.0e-3`
- wet-path restore delta: `nullRms < 4.5e-3`
- ramp discontinuity guard:
  - `transitionDelta < (steadyReference * 30.0 + 0.02)`
  - and absolute cap `transitionDelta < 0.30`
- routing mode distinctness with different chains: `nullRms > 8.0e-4`
- dual boundedness:
  - lower bound `rmsDual > max(rmsA, rmsB) * 0.40`
  - upper bound `rmsDual < max(rmsA, rmsB) * 1.65`
- pan observability: `nullRms(panLeft, panRight) > 5.0e-4`
- width observability: `nullRms(width0, width1) > 5.0e-4`
- clean dual compensation near unity: `0.90 <= dual/single <= 1.12`

These were chosen to remain deterministic but robust against expected non-tonal runtime variance.

## What could not be covered directly (and why)

1. Exact per-sample ramp envelope shape/curve was not asserted directly.  
Reason: internal ramp state/coefficients are not exposed as a public test surface; asserting exact curve would require intrusive internals exposure.

2. Direct comb-filter spectral metric for dry/wet latency alignment was not added as a strict frequency-domain invariant.  
Reason: robust spectral comb assertions at this layer are fragile across CPU/build variance; coverage stays on stable invariants (finiteness, latency stability, bounded behavior, no abrupt discontinuity).

## Validation executed

### Builds

- `build NOVA_SharedCode Debug x64` PASS
- `build NOVA_StandalonePlugin Debug x64` PASS
- `build NOVA_SharedCode Release x64` PASS
- `build NOVA_StandalonePlugin Release x64` PASS

### Diff hygiene

- `git diff --check` PASS (line-ending warnings only; no diff-check errors)

### Functional validation

- `run-base-audio-validation.ps1` PASS  
  - `results=158 passes=5998 failures=0 failingResults=0`
- `run-golden-audio-metrics.ps1` PASS
- `run-rt-profile-scenarios.ps1 Release` PASS  
  - `total=16 pass=16 warn=0 fail=0`
- `run-rt-profile-stability.ps1 Release -CiMode -Runs 3` PASS (prioritized scenarios)
- `check-audio-thread-policy.ps1` PASS/WARN non-blocking  
  - `summary.failures=0`
  - `summary.contractFailures=0`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

### RT stability (Release, prioritized, 3 runs)

- `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.148`
- `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.158`
- `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.124`
- `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.277`

## Recommendation for P6B

Proceed with **P6B as extraction planning (or first narrow extraction slice) for Routing/DryWet**, using the new P6A tests as non-negotiable guardrails:

1. Keep `AudioEngine::process` contracts unchanged (no new locks/shared_ptr/allocations).  
2. Move only one axis at a time (prefer DryWet surface first or strict wrapper-level split).  
3. Run full P6A validation matrix after each micro-step.  
4. Treat any RT Release degradation or policy contract failure as rollback criteria.

