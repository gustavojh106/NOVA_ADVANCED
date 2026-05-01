# P6E-A - Routing/DryWet Test Hardening Results

Date: 2026-04-30  
Scope: test hardening only (no production audio behavior changes).

## Probable cause of transient noise

Most likely cause was measurement timing inside `AudioEngine routing modes and strip controls remain finite and mode-correct`:

- mode/gain/pan/width subcases reused the same engine instance
- after each routing update, the test warmed up with silence and measured relatively early blocks
- occasional runs were sensitive to residual/smoothing state at the start of capture, producing a temporary low `rmsDual` window

This aligns with the observed signature:

- intermittent `Dual-parallel should not collapse to near-silence`
- immediate rerun passes without code changes

## Test changes made

File modified:

- `Source/Core/AudioEngineTests.cpp`

Hardening changes:

1. Added deterministic program pre-settle before each routing capture in the existing routing/strip test.
2. Shifted RMS/pan measurement windows later in the rendered buffer.
3. Added richer failure metrics for the dual-parallel boundedness checks.
4. Added a focused micro-test:
   - `AudioEngine dual-parallel nominal does not collapse after settled routing update`
5. Added helper `computeBufferPeak(...)` for diagnostic reporting.

What was not changed:

- no DSP code
- no routing production logic
- no dry/wet production logic
- no tolerances loosened
- no retries inside tests
- no golden baselines
- no known failures list

## New failure diagnostics

If dual-parallel boundedness fails, the message now includes:

- `rmsA`
- `rmsB`
- `rmsDual`
- `dualToMax` ratio
- `outputMixRaw`
- `switchMode`
- `gainA`/`gainB`
- measured `peakDual`
- `limiterActiveBlocks`

## Base validation stability (two consecutive runs)

Both runs passed consecutively:

- Run 1: `results=159 passes=6002 failures=0 failingResults=0`
- Run 2: `results=159 passes=6002 failures=0 failingResults=0`

This is up from prior `158/5998/0/0`, reflecting added coverage.

## Full validation results

Builds:

- `build NOVA_SharedCode Debug x64` PASS
- `build NOVA_StandalonePlugin Debug x64` PASS
- `build NOVA_SharedCode Release x64` PASS
- `build NOVA_StandalonePlugin Release x64` PASS

Diff hygiene:

- `git diff --check` PASS (line-ending warnings only)

Functional/perf gates:

- `run-base-audio-validation.ps1` PASS (twice consecutively)
- `run-golden-audio-metrics.ps1` PASS
- `run-rt-profile-scenarios.ps1 Release` PASS (`16/16/0/0`)
- `run-rt-profile-stability.ps1 Release -CiMode -Runs 3` PASS on prioritized scenarios (`3/0/0` for all four)
- `check-audio-thread-policy.ps1` WARN non-blocking, with:
  - `summary.failures=0`
  - `summary.contractFailures=0`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release` PASS

## Remaining risks

- The transient was reduced by test stabilization, but because it was intermittent, long-horizon confidence should still be monitored across future nightly runs.
- Routing and dry/wet state are still co-located in `AudioEngine`; state migration phases remain higher risk than this test-only phase.

## Recommendation for P6E-B

Proceed with state migration only as a narrow step and keep these constraints:

1. Preserve the exact `processWithSampleAccurateDryWet` orchestration order.
2. Keep the new routing/dry-wet stabilized tests as non-negotiable gates.
3. Run at least two consecutive base-validation passes during migration.
4. Roll back immediately on any dual-parallel audibility regression or RT Release degradation.
