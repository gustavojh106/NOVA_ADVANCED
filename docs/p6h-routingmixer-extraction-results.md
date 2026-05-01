# P6H - RoutingMixer Extraction Results

Date: 2026-05-01  
Scope: extract routing/gain/pan/width policy into a header-only `RoutingMixer` without changing DSP, dry/wet, graph topology, graph lifecycle, or `ChannelStripProcessor` math.

## Files modified

- `Source/Core/Audio/RoutingMixer.h`
- `Source/Core/Audio/GraphBuilder.h`
- `Source/Core/AudioEngineTests.cpp`
- `scripts/check-audio-thread-policy.ps1`
- `docs/p6h-routingmixer-extraction-results.md`

Validation refreshed generated artifacts under `artifacts/` and `audio-base-test-report.txt`.

No golden baselines, known failures, IDs/schema, existing tolerances, DSP code, dry/wet code, graph lifecycle code, or `ChannelStripProcessor` DSP/smoothing/math were changed.

## Module created

Created header-only `Nova::Audio::RoutingMixer` in `Source/Core/Audio/RoutingMixer.h`.

The module owns only effective target calculation:

- `LineInput`
- `LineTarget`
- `Targets`
- `makeTargets(Nova::SwitcherMode, const LineInput&, const LineInput&) noexcept`
- `dualParallelCompensation() noexcept`

It does not include or reference `GraphRuntime`, `AudioProcessorGraph`, `ChannelStripProcessor`, `DryWetMixer`, `SessionLogger`, `juce::String`, or `processBlock`.

## Tests added

Added three direct `RoutingMixer` tests in `Source/Core/AudioEngineTests.cpp`:

1. `RoutingMixer LineA_Only targets preserve current GraphBuilder policy`
   - A remains active.
   - B is muted.
   - A gain/pan/width pass through.
   - B gain is forced to `0.0f`.
   - active A low-gain fallback applies.
   - muted B low-gain fallback does not apply.

2. `RoutingMixer LineB_Only targets preserve current GraphBuilder policy`
   - B remains active.
   - A is muted.
   - B gain/pan/width pass through.
   - A gain is forced to `0.0f`.
   - active B low-gain fallback applies.
   - muted A low-gain fallback does not apply.

3. `RoutingMixer Dual_Parallel targets preserve fallback then compensation policy`
   - both lines remain active.
   - low-gain fallback applies before compensation.
   - `dualParallelCompensation()` remains `0.5f`.
   - effective gain is `fallbackGain * 0.5f`.
   - pan/width pass through unchanged.
   - both `muted` flags remain false.

## GraphBuilder extraction

Moved out of `GraphBuilder::applyRuntimeParamsToGraph(...)`:

- local `muteA` / `muteB` policy calculation
- local `dualParallel` policy calculation
- local `kParallelGainComp = 0.5f`
- active-line low-gain fallback
- muted-line gain forcing
- dual gain compensation
- pan/width target grouping

Left in `GraphBuilder::applyRuntimeParamsToGraph(...)`:

- revision gating
- input-chain parameter application
- output-chain parameter application
- the `runtime.stripA->setParams(...)` call site
- the `runtime.stripB->setParams(...)` call site
- tempo-sync propagation
- `runtime.appliedParamRevision = revision`

`GraphBuilder` now calls:

- `RoutingMixer::makeTargets(...)`

and then passes the resulting targets to the same strip processors at the same point in the function.

## Behavior preservation

### LineA_Only

`RoutingMixer` sets line A `muted=false` and line B `muted=true`. Line A gain/pan/width pass through, with active-line low-gain fallback preserved. Line B gain is forced to `0.0f`, so B still cannot contaminate output.

### LineB_Only

`RoutingMixer` sets line B `muted=false` and line A `muted=true`. Line B gain/pan/width pass through, with active-line low-gain fallback preserved. Line A gain is forced to `0.0f`, so A still cannot contaminate output.

### Dual_Parallel

Both lines remain unmuted. Low-gain fallback applies to each active line first, then each effective gain is multiplied by `RoutingMixer::dualParallelCompensation()`, which returns `0.5f`.

### Low-gain fallback

The preserved rule is:

- active and unmuted line: `gain <= 0.001f` becomes `1.0f`
- muted line: gain is `0.0f`
- fallback does not run for muted lines

### Dual compensation

The preserved value is `0.5f`. It is applied only in `Dual_Parallel`, only to unmuted lines, and only after low-gain fallback.

### Pan/width pass-through

Pan and width are copied from `LineInput` to `LineTarget` without modification for both active and muted lines. `ChannelStripProcessor` remains the only owner of pan/width DSP and smoothing.

## Policy scan reinforcement

Updated `scripts/check-audio-thread-policy.ps1` with P6H contract checks:

- `process_no_routingmixer`
- `routingmixer_no_graphruntime`
- `routingmixer_no_processblock`
- `routingmixer_no_session_logger`
- `routingmixer_no_juce_string`

Final policy scan:

- status `WARN` from existing legacy warnings
- `failures=0`
- `contractFailures=0`
- `contractChecks=20`

## Validation

Builds:

- `build NOVA_SharedCode Debug x64`: PASS, 0 warnings, 0 errors
- `build NOVA_StandalonePlugin Debug x64`: PASS, 0 warnings, 0 errors
- `build NOVA_SharedCode Release x64`: PASS, 0 warnings, 0 errors
- `build NOVA_StandalonePlugin Release x64`: PASS, 0 warnings, 0 errors

Static checks:

- `git diff --check`: PASS; only existing CRLF normalization warnings were reported
- `check-audio-thread-policy.ps1`: PASS for acceptance criteria, `failures=0`, `contractFailures=0`

Base validation:

- first consecutive run: PASS, `results=169 passes=6146 failures=0 failingResults=0`
- second consecutive run: PASS, `results=169 passes=6146 failures=0 failingResults=0`

This increased coverage from the P6G reference `results=166 passes=6119 failures=0 failingResults=0`.

Audio quality:

- `run-golden-audio-metrics.ps1`: PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`
- `run-rt-profile-scenarios.ps1` Release: PASS, `total=16 pass=16 warn=0 fail=0`
- `run-rt-profile-stability.ps1` Release prioritized, `CiMode`, 3 runs: PASS
  - `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`
  - `stress_block_32`: `3/0/0`
  - `sample_rate_44100`: `3/0/0`
  - `sample_rate_96000`: `3/0/0`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS

## Risks remaining

- `RoutingMixer` is now policy-only, but it is still called from `GraphBuilder`; any future changes to routing targets should remain direct-tested before integration tests.
- The extraction intentionally keeps `GraphBuilder::applyRuntimeParamsToGraph(...)` as the owner of strip call timing. P6I should not move those calls unless it first adds coverage for timing/revision semantics.
- Pan/width audible behavior remains covered by integration tests because the actual DSP belongs to `ChannelStripProcessor`.

## Recommendation for P6I

Use P6I as a post-extraction verification phase before considering broader routing work:

- add direct checks for invalid/unknown switch mode behavior if that mode can be produced by future callers
- add GraphBuilder-level target equivalence tests around `applyRuntimeParamsToGraph(...)` if strip debug access is available without new DSP hooks
- keep `RoutingMixer` header-only and policy-only
- do not move `setParams(...)`, graph topology, dry/wet orchestration, or `ChannelStripProcessor` math in P6I
