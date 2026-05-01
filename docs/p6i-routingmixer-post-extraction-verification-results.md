# P6I - RoutingMixer Post-Extraction Verification & P6 Closure Snapshot

Date: 2026-05-01
Scope: post-extraction verification of P6H. No code changes. No new extractions. No DSP/tono/routing/dry-wet behavior changes. No graph lifecycle changes. No golden baseline updates. No known failures introduced.

This phase confirms that the P6H extraction (`RoutingMixer` policy-only header in `Source/Core/Audio/RoutingMixer.h`) remains stable across the full validation surface, and closes the P6 stage.

## Subsystem state at start of P6I

- `DryWetMixer`: post-migration stable (P6F).
- `RoutingMixer`: extracted as policy-only header (P6H).
- `RoutingMixer` does no DSP.
- `RoutingMixer` does not touch `ChannelStripProcessor` internals.
- `RoutingMixer` does not touch graph topology.
- `RoutingMixer` does not touch `DryWetMixer`.
- `GraphBuilder::applyRuntimeParamsToGraph` still calls `stripA->setParams` / `stripB->setParams` at the same point.

## Files modified in P6I

- `docs/p6i-routingmixer-post-extraction-verification-results.md` (new, this file)
- regenerated artifacts only (no source code changes):
  - `artifacts/audio-thread-policy-scan.json`
  - `artifacts/audio-thread-policy-scan.txt`
  - `artifacts/rt-profile-release-x64-report.json`
  - `artifacts/rt-profile-stability-release-x64.json`
  - `audio-base-test-report.txt`

No source files under `Source/`, no scripts, no `.jucer`, no schema, no IDs, no tolerances, no known failures, no golden baselines were modified.

## Builds

| Target | Configuration | Result |
|--------|---------------|--------|
| `NOVA_StandalonePlugin` | Release x64 | PASS, 0 warnings, 0 errors |
| `NOVA_StandalonePlugin` | Debug x64 | PASS, 0 warnings, 0 errors |
| `NOVA_VST3` | Release x64 | PASS, 0 warnings, 0 errors |

`NOVA_SharedCode` Release/Debug compiled implicitly through above targets, PASS.

## Base validation (consecutive runs)

`scripts/run-base-audio-validation.ps1`:

- run 1: PASS, `results=169 passes=6146 failures=0 failingResults=0`
- run 2: PASS, `results=169 passes=6146 failures=0 failingResults=0`

Group breakdown both runs:

- Core: 0 failures
- P1 Pedal Safety: 0 failures
- Reverb: 0 failures
- Routing: 0 failures
- OutputChain: 0 failures
- AudioEngine: 0 failures
- Regression: 0 failures

No known failures are ignored by the script.

Coverage matches the P6H baseline exactly: results/passes did not regress.

## Golden audio metrics

`scripts/run-golden-audio-metrics.ps1`: PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`. No baseline updates.

## RT profile Release scenarios

`scripts/run-rt-profile-scenarios.ps1` Release: `total=16 pass=16 warn=0 fail=0`. All scenarios PASS:

- `clean_empty_chain`, `clean_line_a`, `dual_parallel_clean`
- `overdrive_v2_nominal`, `overdrive_cleanamp_reverb_chain_nominal`
- `high_gain_amp_nominal`, `cabinet_nominal`
- `delay_feedback_nominal`, `reverb_cloud_tail`, `reverb_reverse_swell`
- `stress_block_32`, `stress_block_64`, `stress_block_512`
- `sample_rate_44100`, `sample_rate_48000`, `sample_rate_96000`

No budget breaches; CPU peaks consistent with P6H.

## RT profile Release stability (priorizada, CiMode, 3 runs)

`scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`. All 16 scenarios `3/0/0`:

| Scenario | runs(pass/warn/fail) |
|----------|----------------------|
| clean_empty_chain | 3/0/0 |
| clean_line_a | 3/0/0 |
| dual_parallel_clean | 3/0/0 |
| overdrive_v2_nominal | 3/0/0 |
| overdrive_cleanamp_reverb_chain_nominal | 3/0/0 |
| high_gain_amp_nominal | 3/0/0 |
| cabinet_nominal | 3/0/0 |
| delay_feedback_nominal | 3/0/0 |
| reverb_cloud_tail | 3/0/0 |
| reverb_reverse_swell | 3/0/0 |
| stress_block_32 | 3/0/0 |
| stress_block_64 | 3/0/0 |
| stress_block_512 | 3/0/0 |
| sample_rate_44100 | 3/0/0 |
| sample_rate_48000 | 3/0/0 |
| sample_rate_96000 | 3/0/0 |

## Audio thread policy scan

`scripts/check-audio-thread-policy.ps1`:

- `status=WARN` (existing legacy warnings only — not P6I-introduced)
- `failures=0`
- `contractFailures=0`
- `contractChecks=20`, all `passed=true`

All P6H contract checks remain green:

- `process_no_routingmixer`
- `routingmixer_no_graphruntime`
- `routingmixer_no_processblock`
- `routingmixer_no_session_logger`
- `routingmixer_no_juce_string`

Other RoutingMixer/DryWetMixer/GraphBuilder/RuntimeGraphManager contracts continue to pass.

Acceptance criteria for policy scan met: `failures=0` and `contractFailures=0`.

## Audio quality gates (wrapper)

`scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS through all steps:

1. Build standalone Release: PASS
2. Base validation: PASS
3. RT profile Release scenarios: 16/16/0/0
4. Audio thread policy scan: failures=0, contractFailures=0
5. RT stability: skipped (Fast mode), executed separately above

## Behavior preservation (verified, not changed)

- `RoutingMixer::makeTargets(...)` still produces:
  - `LineA_Only`: A unmuted with A gain/pan/width pass-through and active low-gain fallback; B muted with gain forced to `0.0f`.
  - `LineB_Only`: B unmuted with B gain/pan/width pass-through and active low-gain fallback; A muted with gain forced to `0.0f`.
  - `Dual_Parallel`: both unmuted; low-gain fallback applied first, then `dualParallelCompensation()` (`0.5f`) applied; pan/width pass-through.
- `GraphBuilder::applyRuntimeParamsToGraph` still owns: revision gating, input-chain params, output-chain params, strip A/B `setParams` call sites, tempo-sync propagation, `appliedParamRevision` update.
- No DSP code, no smoothing constants, no schema versions, no IDs, no tolerances, no golden baselines modified.

## Acceptance criteria check

| Criterion | Result |
|-----------|--------|
| Builds PASS | YES |
| Base validation PASS twice consecutive | YES (169/6146/0/0 each) |
| Results/pass count not lower than P6H | YES (matches exactly) |
| Golden metrics PASS | YES |
| RT Release PASS 16/16/0/0 | YES |
| RT Stability Release priorizada PASS | YES (3/0/0 all 16) |
| Policy scan `failures=0` and `contractFailures=0` | YES |
| Wrapper Fast Release PASS | YES |
| No DSP/tono/routing/dry-wet behavior changes | YES |
| No graph lifecycle changes | YES |
| No golden baseline updates | YES |
| No new known failures | YES |

All acceptance criteria met.

## P6 closure snapshot

P6 stage is closed. Final state across all P6 sub-phases:

| Phase | Outcome |
|-------|---------|
| P6A | DryWet/Routing pre-extraction tests recorded |
| P6B | DryWet/Routing extraction plan written |
| P6C | DryWet pure helpers extracted |
| P6D | DryWet wrapper extracted |
| P6E-A | Routing/DryWet test hardening |
| P6E-B | DryWet ramp state migrated |
| P6E-C | DryWet scratch buffer migrated |
| P6E-D | DryWet direct tests added |
| P6E-E | DryWet delay state migrated |
| P6F | DryWet post-migration verification PASS |
| P6G | RoutingMixer policy and plan written |
| P6H | RoutingMixer extracted as policy-only header |
| P6I | Post-extraction verification PASS, P6 closed |

Key invariants entering P7:

- `DryWetMixer` is a stable header-only mixer with full direct test coverage.
- `RoutingMixer` is policy-only and does not touch DSP, graph, dry/wet, or `ChannelStripProcessor` internals.
- `GraphBuilder` retains ownership of strip `setParams` call sites.
- Audio thread policy contract surface is broader (20 checks) and fully green.
- Base validation coverage held at `results=169 passes=6146`.
- RT Release and RT Stability Release continue to PASS across the full 16-scenario matrix with no budget breaches.

P6 is closed. No follow-up extractions scheduled inside this phase. Future routing or dry/wet work should open P7+ and continue to honor the policy-only / DSP-only separation established here.
