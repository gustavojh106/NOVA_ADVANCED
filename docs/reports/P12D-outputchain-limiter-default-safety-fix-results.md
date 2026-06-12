# P12D - OutputChain Limiter Default Safety Fix - Results

Task: `P12D-outputchain-limiter-default-safety-fix`
Branch: `master`
Build: Debug, Solution target.
Tests: NOVA validation suite, 267 results, 7526 passes, 0 failures.

## Change summary

The OutputChain previously bypassed the lookahead limiter whenever `outputLimiterDb = 0` (the ship default). The only full-scale protection at default was the `applySoftCeiling` band (0.985 -> 0.999 tanh), which behaved as the de facto main limiter under hot master gain. P12D makes the lookahead limiter **always armed** at a transparent safety threshold so the soft ceiling is only ever touched on rare overs.

## Files modified

| File | Change |
|------|--------|
| `Source/Core/DSP/Global/OutputChain.cpp` | `limiterDbToLinear` caps at 0.97 transparent safety; `isLimiterActiveDb` returns true unconditionally; `updateReportedLatencyForLimiter` reports lookahead unconditionally; `processBlock` removes the `if (isLimiterActiveDb)` branch around `limiter.process()`. |
| `Source/Core/DSP/Global/OutputChain.h` | `PeakLimiter::Result::active` semantic redefined: now means "limiter actually reduced gain this block" (`minGain < 0.999999f`), not "limiter armed". Applies to both lookahead and `processWithoutLookahead` paths. |
| `Source/Core/AudioEngineTests.cpp` | New helper `expectStereoSamplesMatchAfterLatency` for clean-path sample equality with the always-on lookahead; updated single-line, parallel routing, and disable/re-enable conditioning tests; updated wet-only path render loop, oversized fallback test, bypass-latency test, and reset-survival test; updated the "legacy clamp" test that previously asserted zero latency at limiterDb = 0. Test pattern in the single-line conditioning case was scaled to peak <= 0.9 so the transparent safety threshold is not engaged on the clean path. Three new P12D regression tests added. |

No changes to: `AudioEngine` graph swap, `RuntimeGraphManager`, `DryWetMixer`, `RoutingMixer`, `PluginStateModel` schema, pedal `processBlock` bodies, UI visual design, host parameter range, preset format.

## New test coverage

1. `P12D OutputChain transparent safety catches hot master before soft ceiling` - reproduces the "Preset con master alto" runtime (`outputVolumeDb = +10.73`, `outputLimiterDb = 0`) and asserts:
   - Output peak `< 0.98`
   - No sample at or above `0.985` (soft-ceiling threshold)
   - `limiterMaxReductionDb > 0.5` (lookahead actually reduces gain)
   - `softCeilingTouchedSamples == 0`
2. `P12D OutputChain stays transparent on quiet signal at default limiter` - default state with quiet signal asserts unity passthrough, no limiter clamping, no soft-ceiling engagement.
3. `P12D OutputChain reports lookahead latency at every limiter setting` - lookahead latency is non-zero and stable at `0 dB`, `-6 dB`, and `-12 dB`.

## Behavior contract

| Condition | Old behavior | New behavior |
|-----------|--------------|--------------|
| `limiterDb = 0`, quiet signal | limiter bypassed, latency = 0 | limiter armed at threshold 0.97 linear, no gain reduction, latency = lookahead |
| `limiterDb = 0`, hot master + hot chain | output pinned at 0.999 by soft ceiling, sustained near-clip | limiter clamps to ~0.97, soft ceiling does not engage |
| `limiterDb < -0.0001`, signal above threshold | clamps at `decibelsToGain(limiterDb)` | clamps at `min(decibelsToGain(limiterDb), 0.97)` (transparent floor still applies) |
| Reported latency | `lookaheadSamples` only when limiterDb < -0.0001 | `lookaheadSamples` always |

`PeakLimiter::Result::active` now reflects actual gain reduction, not armed state. Telemetry consumers (`limiterActiveBlocks`, `sustainedClampBlocks`) now report audible limiting work, which is the more useful semantic.

## Test deltas

Before P12D fix run: 7486 passes, 40 sub-failures across 8 tests.
After P12D fix run: 7526 passes, 0 failures across 267 results.

Sub-failure categories addressed:

- `OutputChain clamps legacy extreme limiter thresholds to an audible floor` - flipped the limiterDb=0 latency assertion to expect the now-stable lookahead.
- `Global processors preserve active params after reset` - replaced 4-sample impulse with sustained sine across `kSettleBlocks` to settle past lookahead.
- `AudioEngine single-line / parallel routing preserve clean input` - replaced sample-equality on a 4-sample buffer with `expectStereoSamplesMatchAfterLatency` that compensates the engine-reported latency.
- `AudioEngine wet-only path reflects topology changes and stays audible` - render loop now primes the lookahead with the same periodic source.
- `AudioEngine oversized process blocks stay safe and finite` - oversized fallback now runs after a 4-block sine prime.
- `AudioEngine recovers cleanly across engine disable and re-enable` - the engine-enabled rendering branches use the new latency-aware helper; the engine-disabled branch keeps the direct 4-sample equality (dry passthrough bypasses OutputChain).
- `AudioEngine rebuilds graph latency when bypass changes node latency` - bypass baseline is now the OutputChain transparent safety latency, not zero.

## Acceptance

- `limiterActiveBlocks` is observable when hot master / hot chain drives output above the transparent safety threshold.
- `softCeilingTouchedSamples` remains 0 under normal aggressive high-gain because the lookahead now catches the peak earlier.
- CleanAmp / quiet signal path unchanged (no audible artefact; no gain reduction; unity passthrough).
- RT policy scan unchanged at source level (no new allocations, no new locks, no new sample-rate-dependent state; only an unconditional dispatch into the existing `limiter.process` path that was already RT-safe).
- Schema unchanged. Preset compatibility preserved.

## Out of scope (left untouched)

- `AudioEngine` graph swap path
- `RuntimeGraphManager`
- `DryWetMixer`
- `RoutingMixer`
- `PluginStateModel` schema
- Pedal `processBlock` bodies
- Test file structure (helpers added next to existing ones; no fixture reshape)
- UI visual design
- Host parameter range for `OUTPUT_LIMITER` (kept `-12..0 dB`, default `0`)
