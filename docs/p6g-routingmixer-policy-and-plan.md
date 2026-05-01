# P6G - RoutingMixer Policy Tests and Extraction Plan

Date: 2026-05-01  
Scope: freeze the current routing/gain/pan/width policy and prepare a narrow P6H extraction plan. No `RoutingMixer` implementation was added in P6G.

## Files modified

- `Source/Core/AudioEngineTests.cpp`
- `docs/p6g-routingmixer-policy-and-plan.md`

Validation refreshes generated report artifacts under `artifacts/` and `audio-base-test-report.txt`.

No DSP, tone, routing behavior, dry/wet behavior, graph lifecycle, command semantics, `RuntimeGraphManager`, `GraphBuilder` topology, `ChannelStripProcessor` DSP, IDs/schema, golden baselines, known failures, or existing tolerances were changed.

## Current routing policy

The current policy lives in `GraphBuilder::applyRuntimeParamsToGraph(...)`. It computes effective strip targets and then calls `stripA->setParams(...)` and `stripB->setParams(...)`. `ChannelStripProcessor` remains the owner of gain, pan, width smoothing and DSP math.

### LineA_Only

- Strip A is active.
- Strip B is muted with effective gain `0.0f`.
- Strip A receives `snapshot.gainA`, `snapshot.panA`, and `snapshot.widthA`.
- Strip B still receives pan/width targets, but its gain is forced to zero.
- B-side gain/pan/width changes must not contaminate the output.
- If A is active and `gainA <= 0.001f`, effective gain A is normalized to `1.0f`.

### LineB_Only

- Strip B is active.
- Strip A is muted with effective gain `0.0f`.
- Strip B receives `snapshot.gainB`, `snapshot.panB`, and `snapshot.widthB`.
- Strip A still receives pan/width targets, but its gain is forced to zero.
- A-side gain/pan/width changes must not contaminate the output.
- If B is active and `gainB <= 0.001f`, effective gain B is normalized to `1.0f`.

### Dual_Parallel

- Both strips are active.
- Current dual compensation is `0.5f` applied independently to each active line after low-gain fallback.
- Effective gain A is `effectiveLowGainFallback(gainA) * 0.5f`.
- Effective gain B is `effectiveLowGainFallback(gainB) * 0.5f`.
- The clean nominal dual path remains near the expected single-line level and must not collapse toward silence.
- The clean nominal dual path must not produce dangerous gain blow-up or limiter activity.

### Low-gain fallback

Current behavior:

- The fallback applies only to active, unmuted lines.
- For an active line, `gain <= 0.001f` is normalized to `1.0f`.
- For a muted line, gain is forced to `0.0f`; fallback is not applied.
- In `Dual_Parallel`, fallback is applied first and the `0.5f` dual compensation is applied after it.

### Pan and width

- Pan and width values are passed through as targets to each `ChannelStripProcessor`.
- The future `RoutingMixer` must only calculate policy targets.
- `ChannelStripProcessor` must keep pan/width DSP, smoothing, and stereo math.

## Tests added

P6G added two focused policy tests in `Source/Core/AudioEngineTests.cpp`.

1. `AudioEngine routing policy low-gain fallback and inactive-line isolation remain stable`
   - Active LineA gain `0.0f` matches LineA unity baseline.
   - Active LineB gain `0.0005f` matches LineB unity baseline.
   - Inactive LineB gain/pan/width changes do not affect `LineA_Only`.
   - Inactive LineA gain/pan/width changes do not affect `LineB_Only`.
   - Dual low-gain fallback remains near unity and does not collapse.

2. `AudioEngine routing policy LineB pan and width targets remain isolated`
   - LineB pan left/right changes produce the expected active-line stereo balance.
   - LineB pan target changes are observable and finite.
   - LineB width `0.0f` vs `1.0f` is observable and finite.
   - Inactive LineA pan changes do not affect `LineB_Only`.

Existing P6A/P6E-A/P6F coverage already covers these adjacent invariants, so P6G only reinforced gaps:

- `LineA_Only`, `LineB_Only`, and `Dual_Parallel` outputs remain finite and audible.
- A and B chains remain distinguishable.
- Dual clean nominal stays close to unity and does not trigger limiter in the clean nominal case.
- LineA pan and width target propagation are observable.
- Routing transition `LineA -> Dual -> LineB` settles deterministically without accidental silence.

## P6H extraction plan

P6H should introduce a policy-only header:

- `Source/Core/Audio/RoutingMixer.h`

Proposed shape:

```cpp
class RoutingMixer
{
public:
    struct LineInput
    {
        float gain = 1.0f;
        float pan = 0.0f;
        float width = 1.0f;
    };

    struct LineTarget
    {
        float gain = 1.0f;
        float pan = 0.0f;
        float width = 1.0f;
        bool muted = false;
    };

    struct Targets
    {
        LineTarget lineA;
        LineTarget lineB;
    };

    static Targets makeTargets(Nova::SwitcherMode mode,
        const LineInput& lineA,
        const LineInput& lineB) noexcept;
};
```

P6H rules:

- `RoutingMixer` must not do DSP.
- `RoutingMixer` must not touch `ChannelStripProcessor` smoothing or stereo math.
- `RoutingMixer` must not touch graph topology.
- `RoutingMixer` must not touch `DryWetMixer`.
- `RoutingMixer` must only calculate effective targets.
- `GraphBuilder::applyRuntimeParamsToGraph(...)` should continue to call `stripA->setParams(...)` and `stripB->setParams(...)`.
- Routing behavior must be byte-for-byte equivalent at the policy level:
  - muted line gain `0.0f`
  - active low-gain fallback `gain <= 0.001f -> 1.0f`
  - dual compensation `0.5f` after fallback
  - pan/width pass-through

Suggested P6H sequence:

1. Add `RoutingMixer.h` with direct unit tests for `makeTargets(...)`.
2. Keep `GraphBuilder::applyRuntimeParamsToGraph(...)` as the only caller.
3. Replace only the local policy calculation in `GraphBuilder` with `RoutingMixer::makeTargets(...)`.
4. Preserve the existing `setParams(...)` call points and revision gating.
5. Re-run the same P6G validation matrix before considering any broader routing work.

## Validation

Final P6G validation results:

- `build NOVA_SharedCode Debug x64`: PASS, 0 warnings, 0 errors
- `build NOVA_StandalonePlugin Debug x64`: PASS, 0 warnings, 0 errors
- `build NOVA_SharedCode Release x64`: PASS, 0 warnings, 0 errors
- `build NOVA_StandalonePlugin Release x64`: PASS, 0 warnings, 0 errors
- `git diff --check`: PASS; only existing CRLF normalization warnings were reported
- `run-base-audio-validation.ps1` first consecutive run: PASS, `results=166 passes=6119 failures=0 failingResults=0`
- `run-base-audio-validation.ps1` second consecutive run: PASS, `results=166 passes=6119 failures=0 failingResults=0`
- `run-golden-audio-metrics.ps1`: PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`
- `run-rt-profile-scenarios.ps1` Release: PASS, `total=16 pass=16 warn=0 fail=0`
- `run-rt-profile-stability.ps1` Release prioritized, `CiMode`, 3 runs: PASS
  - `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`
  - `stress_block_32`: `3/0/0`
  - `sample_rate_44100`: `3/0/0`
  - `sample_rate_96000`: `3/0/0`
- `check-audio-thread-policy.ps1`: PASS for acceptance criteria
  - scanner status `WARN` from existing legacy warnings
  - `failures=0`
  - `contractFailures=0`
  - `contractChecks=15`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS

Early base validation after adding the policy tests:

- `run-base-audio-validation.ps1` Debug x64: PASS
- `results=166 passes=6119 failures=0 failingResults=0`

This increased coverage from the P6F reference `results=164 passes=6103 failures=0 failingResults=0`.

## Risks remaining

- P6G verifies behavior through `AudioEngine` integration tests; P6H should add direct `RoutingMixer::makeTargets(...)` tests before swapping `GraphBuilder` to the helper.
- Pan/width observability is DSP-dependent because `ChannelStripProcessor` owns the actual math. That is intentional; P6H should only assert target calculation directly and keep DSP verification in existing integration tests.
- Revision gating in `GraphBuilder::applyRuntimeParamsToGraph(...)` must remain untouched in P6H.

## Rollback criteria for P6H

Rollback P6H immediately if any of these occur:

- Base validation result count drops or any test fails.
- Golden metrics change or require baseline updates.
- RT Release scenario count changes from `16/16/0/0`.
- Policy scan reports failures or contract failures.
- `RoutingMixer` gains DSP, graph, dry/wet, logging/string, allocation-heavy, or process-path responsibilities.
- `GraphBuilder` topology, graph lifecycle, command semantics, or `ChannelStripProcessor` DSP changes are required to make the extraction pass.
