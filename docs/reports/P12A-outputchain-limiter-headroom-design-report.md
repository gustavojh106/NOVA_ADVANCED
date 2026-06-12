# P12A OutputChain Limiter Headroom Design Report

Date: 2026-05-23
Task ID: P12A-outputchain-limiter-headroom-design
Scope: read-only verification and design. No source, test, preset, or schema changes were made.

## Executive Verdict

The hypothesis is mostly correct and points at a real final-stage safety flaw. In the current repo, `OutputChainProcessor::processBlock()` skips the lookahead limiter whenever `isLimiterActiveDb(currentLimiterDb)` is false. `isLimiterActiveDb()` returns true only when the sanitized limiter value is below `-0.0001 dB`, so the default and factory value of `0 dB` does not run the limiter at all.

At the default `outputLimiterDb = 0.0f`, OutputChain still applies DC blocking, master gain, telemetry, and the final soft ceiling. The final soft ceiling is a very narrow emergency shaper with `threshold = 0.985f` and `ceiling = 0.999f`. That is enough to keep samples under full scale, but it is not an appropriate primary limiter for sustained high-gain material. With Output Volume allowed up to `+12 dB`, and Line A/B channel gains allowed up to `2.0x`, aggressive HighGainAmp -> Modern4x12 chains can plausibly hit the final soft ceiling repeatedly. That matches the reported manual symptom of light clipping when both gain and volume are high.

The real root cause is not HighGainAmp or Modern4x12 alone. Those stages are capable of high energy, but they already include local trims, saturation, DC cleanup, and cabinet trims. The bug is that the final "transparent safety mode" described in OutputChain comments is not actually transparent limiting; it is limiter bypass plus an emergency soft ceiling.

## Verified Facts

- `OutputChainProcessor::sanitizeLimiterDb()` clamps limiter control to `-12.0f..0.0f`; see `Source/Core/DSP/Global/OutputChain.cpp:13`.
- `OutputChainProcessor::isLimiterActiveDb()` returns `sanitizeLimiterDb(limiterDb) < -0.0001f`; see `Source/Core/DSP/Global/OutputChain.cpp:28`.
- `OutputChainProcessor::processBlock()` only calls `limiter.process()` inside `if (isLimiterActiveDb(currentLimiterDb))`; see `Source/Core/DSP/Global/OutputChain.cpp:278`.
- When the limiter is skipped, `limiterResult.thresholdLinearMin` and `thresholdLinearMax` are forced to `1.0f`, and no lookahead limiting occurs; see `Source/Core/DSP/Global/OutputChain.cpp:280`.
- `updateReportedLatencyForLimiter()` reports lookahead latency only when `isLimiterActiveDb(limiterDb)` is true; see `Source/Core/DSP/Global/OutputChain.cpp:172`.
- The soft ceiling uses `threshold = 0.985f`, `ceiling = 0.999f`, and a tanh knee of `0.014f`; see `Source/Core/DSP/Global/OutputChain.cpp:33`.
- Output volume is sanitized to `-36.0f..12.0f` inside OutputChain, while host parameter and state-model ranges allow `-60.0f..12.0f`; see `Source/Core/DSP/Global/OutputChain.cpp:3`, `Source/Core/PluginProcessor.cpp:449`, and `Source/Core/PluginStateModel.h:148`.
- Default/factory state stores `OUTPUT_LIMITER = 0.0f` and `OUTPUT_VOL = 0.0f`; see `Source/Core/PluginStateModel.h:305`.
- ChannelStrip line gain is `0.0f..2.0f`; see `Source/Core/DSP/Global/ChannelStrip.cpp:5`, `Source/Core/PluginProcessor.cpp:435`, and `Source/Core/PluginStateModel.h:156`.
- HighGainAmp drive is `1.0f..10.0f`, level is `0.0f..2.0f`, and the amp applies `kProfessionalOutputTrim = 0.32f`; see `Source/Effects/Amplifiers/HighGainAmp.h:18`, `Source/Effects/Amplifiers/HighGainAmp.h:24`, and `Source/Effects/Amplifiers/HighGainAmp.h:298`.
- Modern4x12 level is `0.0f..2.0f` and applies `kProfessionalCabinetTrim = 0.70f`; see `Source/Effects/Cabinets/Modern4x12Cabinet.h:25` and `Source/Effects/Cabinets/Modern4x12Cabinet.h:186`.
- Boost can add up to `+24 dB` before local containment; see `Source/Effects/Pedals/Boost/BoostPedal.h:119`.
- Existing tests explicitly assert that a `0 dB` limiter is "bypassed" and has zero latency; see `Source/Core/AudioEngineTests.cpp:3231`.
- P10C contains a test named `p10c_high_gain_chain_outputchain_limiter_independence` that expects nominal high-gain chains to avoid OutputChain limiter activity; see `Source/Core/AudioEngineTests.cpp:12171`.

## Incorrect Assumptions

- The phrase "0.0 = transparent safety mode" in `OutputChain.h` and `sanitizeLimiterDb()` is not true in implementation. At `0 dB`, the lookahead limiter is not a transparent safety limiter; it is bypassed.
- The final soft ceiling is not "only" protection in all cases: `HealthMonitor` and `SignalGuard` still sanitize invalid or extreme values. However, for normal audio peaks around or above full scale, the final soft ceiling is the only OutputChain peak protection when `limiterDb == 0`.
- This does not prove HighGainAmp or Modern4x12 is defective. The evidence says they can contribute level and spectral peaks, but the final-stage protection behavior is the narrow, verified defect.

## Signal Path Explanation

Relevant path:

Input -> InputChain -> Line A/B pedal chain -> ChannelStrip A/B -> OutputChain -> host output.

For the focused chain:

HighGainAmp -> Modern4x12 -> ChannelStrip -> OutputChain master gain -> limiter or soft ceiling.

For hotter chains:

Boost -> HighGainAmp -> Modern4x12 -> ChannelStrip -> OutputChain.
Distortion -> HighGainAmp -> Modern4x12 -> ChannelStrip -> OutputChain.

OutputChain order is:

1. `SignalGuard::scrub()`.
2. DC blockers.
3. Master gain.
4. Lookahead limiter only if `isLimiterActiveDb(currentLimiterDb)`.
5. Final soft ceiling.

That ordering means positive master gain is applied before the limiter/soft ceiling. If limiter is bypassed at `0 dB`, master gain can drive the final soft ceiling directly.

## Gain Budget Analysis

InputChain:

- Input gain range: code sanitizes `-36..+24 dB`; host/state expose `-60..+24 dB`.
- Stage type: static gain with smoothing, plus stateful auto-routing, force-mono blend, high-pass/DC conditioning, and input gate.
- Can increase peak/RMS: yes, up to `+24 dB`.
- Containment: signal scrub for NaN/Inf/extreme values; no musical limiter.

ChannelStrip:

- Gain range: `0.0..2.0x` per line.
- Pan range: `-1..+1`.
- Width range: `0..2`, with compensation above width `1.0`.
- Stage type: static/smoothed gain and stereo transform.
- Can increase peak/RMS: yes, line gain can add up to `+6 dB`; width can redistribute energy even with compensation.
- Containment: finite scrub only; no limiter.

HighGainAmp:

- Drive range: `1..10`.
- Level range: `0..2`.
- Stage type: nonlinear, stateful, oversampled multi-stage saturation with sag/noise reject and tone filters.
- Can increase peak/RMS: yes, especially with high drive, presence, resonance, level.
- Containment: tanh stages, input conditioning, DC block, and `kProfessionalOutputTrim = 0.32f`.

Modern4x12:

- Low end: `-12..+12 dB`.
- Presence: `-12..+12 dB`, internally compensated and limited to max `+3.5 dB` in the high shelf formula.
- Resonance: `-6..+6 dB`.
- Level: `0..2`.
- Stage type: stateful convolution plus filters, mix, level, DC block.
- Can increase peak/RMS: yes, from IR energy, low shelf, resonance, level, and dry/wet mix.
- Containment: `kProfessionalCabinetTrim = 0.70f`, DC block, no final limiter.

Boost:

- Gain range: `0..+24 dB`.
- Level range: `0.5..2.0`.
- Stage type: nonlinear boost/preamp with local filtering and containment.
- Can increase peak/RMS: yes, especially before HighGainAmp.
- Containment: `containBoostOutput()` knee `0.72`, ceiling `0.94`, and amp-input compatibility trim.

Distortion:

- Gain range: `0..100`.
- Level maps to `0.08..1.6`.
- Stage type: nonlinear, oversampled, mode-dependent clipping with adaptive gate in high-gain modes.
- Can increase peak/RMS: yes.
- Containment: per-mode output trims, `containOutputSample()` soft ceiling at `0.94`, adaptive gate for metal/studio modes.

OutputChain:

- Master range: `-36..+12 dB` internally.
- Limiter range: `-12..0 dB`, but active only below roughly `0 dB`.
- Stage type: static/smoothed gain, optional stateful lookahead limiter, always-on final soft ceiling.
- Can increase peak/RMS: yes, master can add up to `+12 dB`.
- Containment: lookahead limiter only for limiter values below `-0.0001 dB`; final soft ceiling always.

## Limiter Activation Analysis

Answer: yes, limiter processing really skips at `0 dB`.

The decisive implementation is:

- `isLimiterActiveDb(float limiterDb)` returns true only for sanitized values below `-0.0001f`.
- `processBlock()` does not call `limiter.process()` unless that function returns true.

Answer: yes, default/factory state skips lookahead limiting.

Defaults are:

- Output volume: `0.0f`.
- Output limiter: `0.0f`.

The host parameter default and state reset both use `0.0f`. Therefore default/factory sessions skip the lookahead limiter and report zero OutputChain limiter latency.

Latency implication:

- If P12B always runs the limiter at `0 dB`, latency behavior must be redesigned. Current tests expect `getLatencySamples() == 0` at `0 dB`; see `AudioEngineTests.cpp:3231`.
- If the limiter runs always-on, either OutputChain must always report lookahead latency, or it must use a zero-latency limiter path at `0 dB`. Always-on lookahead with zero reported latency would be wrong.

## Soft Ceiling Behavior Analysis

Answer: at default limiter `0 dB`, for ordinary audio overs the soft ceiling becomes the only final OutputChain peak shaper.

The soft ceiling is:

- Threshold: `0.985`.
- Ceiling: `0.999`.
- Knee width: `0.014`.

That knee is extremely narrow. It is a last-resort sample shaper, not a program-dependent limiter. It has no lookahead, no release, no hold, no linked stereo gain reduction, and no RMS-aware behavior. It will generate nonlinear shaping exactly at the end of the chain.

Answer: it is not musically appropriate as the main sustained high-gain safety device.

It is appropriate as an emergency guard for rare overs. For sustained high-gain peaks, it can become a high-frequency distortion source. Repeated `softCeilingTouchedSamples` under high-gain output would be a direct diagnostic sign that OutputChain is using the wrong protection stage.

Answer: yes, this can explain manual clipping at high volume plus high gain.

The symptom requires a signal near or over full scale after master gain. At default limiter `0 dB`, the lookahead limiter is absent and the final ceiling shapes every overshooting sample. That is consistent with "light clipping" rather than total collapse, mute, ducking, or graph instability.

## Master Gain And ChannelStrip Interaction

ChannelStrip can add up to `2.0x` gain before OutputChain. OutputChain master can then add up to `+12 dB`, about `3.98x`. Combined, these can add nearly `+18 dB` after the amp/cab chain, before final limiting/ceiling. In dual-parallel mode, RoutingMixer may also affect practical summed energy, but P12A should not touch RoutingMixer.

The main concern is not that positive master exists. Positive master gain is useful. The concern is that positive master gain is before a limiter that is bypassed by the default control value.

## HighGainAmp -> Modern4x12 Level Risk

HighGainAmp is internally trimmed (`0.32x`) and Modern4x12 is also trimmed (`0.70x`), so the chain is not obviously reckless. Existing P10/P11 tests also show multiple intentional guards around high-gain baselines.

However, HighGainAmp has:

- Drive up to `10`.
- Level up to `2`.
- Presence shelf up to about `+4.7 dB`.
- Resonance shelf up to about `+3.5 dB`.
- Tight/preboost up to about `+3 dB`.

Modern4x12 has:

- Level up to `2`.
- Low shelf up to `+12 dB`.
- Resonance up to `+6 dB`.
- IR/comb/convolution behavior with no normalisation.

Together, especially with ChannelStrip and OutputChain master gain, the chain can reasonably hit final-stage protection. Boost and Distortion before HighGainAmp increase the probability.

## Preset / Session Compatibility Concerns

No schema migration is required for the recommended fix. The existing limiter parameter can continue to store `-12..0 dB`.

The compatibility risk is behavioral:

- Existing presets with limiter at `0 dB` currently have zero limiter latency and no lookahead limiting.
- If `0 dB` becomes "true transparent safety limiting", those presets may gain limiter latency and may avoid final soft-ceiling distortion.
- Clean paths should remain effectively unchanged if the limiter threshold is near `0 dB` and only acts on overs.
- High-gain presets may sound cleaner at the top end, but if the limiter release is too slow they may feel ducked.

Because this is a behavior correction to match the documented "transparent safety mode", it should not require a schema bump. It does require tests and release notes.

## Real-Time Safety Concerns

The OutputChain limiter implementation is already allocated in `prepareToPlay()` and processes with fixed arrays and a prepared delay buffer. Always running the existing limiter path is RT-safe in structure.

Risks:

- Always-on lookahead consumes more CPU on every OutputChain block.
- Always-on lookahead changes reported latency. The graph latency, DryWetMixer compensation, and host latency reporting must remain correct.
- Updating `setLatencySamples()` must remain outside the hot path or only when parameters change; current code calls it in `setParams()`, `prepareToPlay()`, and `reset()`.
- Do not solve this by modifying RuntimeGraphManager, DryWetMixer, RoutingMixer, graph swap, or pedal processBlock bodies.

## Recommended Fix Design

Recommended minimal fix for P12B: make OutputChain's default `0 dB` limiter a true transparent safety limiter, not bypass. Keep the parameter range and schema unchanged.

Design details:

1. Change limiter activation semantics in OutputChain only:
   - At `limiterDb == 0.0f`, run the limiter with a near-full-scale safety threshold.
   - Use a threshold slightly below hard full scale, for example `-0.1 dBFS` or equivalent linear value already referenced by `PeakLimiter::process()` as `0.9885531f`.
   - Do not add makeup gain.

2. Keep negative limiter values as intentional user ceilings:
   - `-12..0 dB` continues to map to limiter threshold.
   - Existing presets remain valid.

3. Report limiter latency correctly:
   - If the lookahead limiter runs at `0 dB`, OutputChain must report lookahead latency at `0 dB`.
   - Update tests that currently assert zero latency at `0 dB`.
   - The graph build already captures graph latency; keep the change local to OutputChain and existing graph latency plumbing.

4. Keep final soft ceiling as emergency guard:
   - Do not rely on it for normal high-gain control.
   - Consider a small soft-ceiling knee improvement only after the always-on limiter is validated.

5. Add diagnostics:
   - Acceptance should assert that `limiterActiveBlocks` becomes observable only when high-gain material crosses the threshold.
   - Assert `softCeilingTouchedSamples` is low or zero under aggressive but normal high-gain test material.

Suggested model effort: Codex Medium. The code change is local, but the test/latency impact is nontrivial.

## Alternatives Rejected

Option A: Always run the limiter, using `0 dB` as transparent/high-threshold limiting.

- Pros: directly fixes the verified defect; aligns implementation with comments; preserves master gain range; no schema migration; keeps upstream amp/cab baselines intact.
- Cons: changes latency at default if using existing lookahead; may need several tests updated; slight CPU increase.
- Tonal risk: low for clean paths, medium for extreme high-gain if release/hold causes audible gain reduction.
- Preset compatibility risk: medium behavioral risk, low data risk.
- RT safety risk: low if reusing prepared limiter.
- Test impact: moderate.
- Clean/fuzz/amp baseline risk: low if threshold is close to full scale and tests guard RMS/peak.
- Verdict: best primary fix.

Option B: Keep limiter optional, reduce master gain maximum from `+12 dB` to `0 dB` or lower.

- Pros: simple, reduces probability of final clipping.
- Cons: weakens product output, breaks user expectations/presets, does not fix default limiter bypass, hides the real final-stage safety bug.
- Tonal risk: medium; product may feel quieter.
- Preset compatibility risk: high for presets using positive master.
- RT safety risk: low.
- Test impact: moderate.
- Clean/fuzz/amp baseline risk: medium because global gain ceiling changes behavior.
- Verdict: reject as primary fix.

Option C: Widen or reshape soft ceiling knee.

- Pros: can make emergency clipping less harsh.
- Cons: still uses nonlinear final clipping as primary protection when limiter is bypassed; may color all near-full-scale material.
- Tonal risk: medium.
- Preset compatibility risk: low data risk, medium sound risk.
- RT safety risk: low.
- Test impact: moderate.
- Clean/fuzz/amp baseline risk: medium if near-ceiling material changes.
- Verdict: useful later, not sufficient first fix.

Option D: Add chain-level headroom trim before cabinet or output.

- Pros: targets high-gain chains before final stage.
- Cons: risks changing HighGainAmp -> Modern4x12 baseline, can hide upstream staging issues, may require new parameter/state policy if user-facing.
- Tonal risk: high.
- Preset compatibility risk: medium to high.
- RT safety risk: low to medium depending location.
- Test impact: high.
- Clean/fuzz/amp baseline risk: high.
- Verdict: reject for P12B.

Option E: Combination approach.

- Pros: most robust long-term if staged carefully: always-on transparent limiter first, then optional soft-ceiling refinement.
- Cons: too much at once if it includes master range or chain trims.
- Tonal risk: low to medium if limited to OutputChain limiter plus diagnostics; high if it includes chain trims.
- Preset compatibility risk: medium.
- RT safety risk: low if OutputChain-only.
- Test impact: moderate to high.
- Verdict: use a narrow combination: Option A now, diagnostics/tests now, Option C only later if evidence remains.

## Test Plan

Add deterministic tests in the existing `AudioEngineTests.cpp` structure, without changing file structure:

- OutputChain default limiter behavior at `0 dB`:
  - Feed isolated transients above `1.0`.
  - Expect `limiterActiveBlocks > 0` and `limiterTouchedSamples > 0`.
  - Expect `softCeilingTouchedSamples` low or zero after the lookahead delay settles.

- Latency at default limiter:
  - If always-on lookahead is used, expect `OutputChainProcessor::getLatencySamples() > 0` at `0 dB`.
  - Verify graph latency captures OutputChain latency.

- Master high setting plus high-gain chain:
  - HighGainAmp drive `10`, Modern4x12, OutputChain master `+6 dB` and `+12 dB`.
  - Expect finite output, no NaN/Inf, no silence/collapse, no sustained soft-ceiling activity.

- HighGainAmp drive `10` + Modern4x12 + master `+6/+12 dB`:
  - Assert `limiterActiveBlocks` becomes observable when threshold is exceeded.
  - Assert RMS does not drop excessively compared with pre-fix bounded reference windows.

- Boost -> HighGainAmp -> Modern4x12:
  - Use Boost gain near nominal/high range.
  - Expect limiter activity under aggressive output but no continuous soft-ceiling clamp.

- Distortion -> HighGainAmp -> Modern4x12:
  - Use Distortion studio/metal mode guarded settings.
  - Expect no NaN/Inf, no collapse, no excessive RMS ducking.

- CleanAmp clean path preservation:
  - CleanAmp + Cabinet/Modern4x12 at normal output.
  - Expect negligible limiter activity and stable RMS/peak versus current clean baseline.

- Fuzz reference preservation:
  - Keep existing P10F/P10G fuzz reference behavior intact.
  - Expect no new low-gate or volume-collapse regression.

- Diagnostic counters:
  - `softCeilingTouchedSamples` should not be continuously high under normal aggressive high-gain.
  - `limiterActiveBlocks` should be observable when high-gain exceeds the threshold.

- Safety:
  - No NaN/Inf.
  - No silence/collapse.
  - No excessive RMS drop/ducking.
  - `scripts/check-audio-thread-policy.ps1` has no regression.
  - `scripts/run-base-audio-validation.ps1` passes.

## What Not To Touch

Keep these untouched for P12B unless later evidence proves otherwise:

- `Source/Core/AudioEngine.cpp` graph swap path.
- `Source/Core/Audio/RuntimeGraphManager.h`.
- `Source/Core/Audio/DryWetMixer.h`.
- `Source/Core/Audio/RoutingMixer.h`.
- `Source/Core/PluginStateModel.h` schema and version.
- Pedal `processBlock()` bodies.
- Test file structure.
- Factory preset files unless a separate preset-authoring task is opened.

## Risk Assessment

Primary risk: latency compatibility. A true lookahead safety limiter at `0 dB` changes default OutputChain latency from zero to lookahead latency. That is correct if the limiter processes, but it requires tests and host behavior to be verified.

Secondary risk: perceived ducking. The existing limiter has hold and program-dependent release. If the threshold is too low or release too slow for default safety mode, high-gain material may feel clamped. This must be tuned with tests around RMS drop and `limiterMaxReductionDb`.

Low risk: schema and preset file compatibility. No new state is needed.

Medium risk: baseline churn. Some existing high-gain tests intentionally assert no OutputChain limiter dependency. Those should be reframed: nominal staged chains should not need heavy limiting, but default OutputChain must still catch real overs before the emergency soft ceiling.

## Final Recommendation

Proceed to P12B with an OutputChain-only fix: make `0 dB` a real transparent safety limiter threshold and keep the final soft ceiling as last-resort protection. Do not reduce global master range first, do not add headroom trims, and do not touch graph/runtime routing or pedal internals. The implementation should preserve clean and fuzz behavior by setting the default limiter threshold close to full scale and validating that it only acts on true overs. Latency reporting must be updated honestly if the lookahead path becomes always-on.
