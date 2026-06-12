# P12A OutputChain Limiter Headroom Summary

## Is Claude's Hypothesis Correct?

Mostly yes. The repo verifies the central claim: OutputChain skips the lookahead limiter at the default/factory limiter value of `0 dB`. `isLimiterActiveDb()` only returns true for values below `-0.0001 dB`, and `processBlock()` only calls `limiter.process()` in that active branch. Default state and host parameter defaults use `0.0f`, so normal factory sessions do not run lookahead limiting.

The only normal full-scale protection left in that state is the final soft ceiling, after master gain. That soft ceiling is narrow: threshold `0.985`, ceiling `0.999`, knee `0.014`. It is acceptable as an emergency guard, not as the main high-gain limiter.

## What Is The Root Cause?

The root cause is a mismatch between design intent and implementation. OutputChain comments describe `0 dB` as "transparent safety mode", but implementation treats it as limiter bypass. When master output is raised, high-gain chains can hit the final soft ceiling directly. That can produce the reported light clipping at high volume/high gain.

HighGainAmp and Modern4x12 are not proven defective by this pass. They can produce high energy, but they also contain trims and local containment. The verified defect is the final-stage protection gap.

## What Should Be Fixed First?

Fix OutputChain first:

- Make limiter `0 dB` run as transparent high-threshold safety limiting.
- Keep limiter parameter range `-12..0 dB`.
- Keep schema unchanged.
- Keep master gain range for now.
- Keep final soft ceiling as last-resort emergency protection.
- Report latency correctly if the lookahead limiter runs at `0 dB`.

This is the smallest fix that addresses the verified failure mode without weakening the product or changing amp/cab voicing.

## What Should Not Be Touched?

Do not touch these in P12B:

- `AudioEngine.cpp` graph swap path.
- `RuntimeGraphManager`.
- `DryWetMixer`.
- `RoutingMixer`.
- `PluginStateModel` schema.
- Pedal `processBlock()` bodies.
- Test file structure.
- Presets, unless opened as a separate preset-authoring task.

## Recommended Implementation Model / Effort

Recommended model effort: Codex Medium.

The code change should be local to OutputChain, but the test impact is not trivial because existing tests currently encode zero latency / bypass behavior at `0 dB`. P12B should update that expectation deliberately and add high-gain limiter-counter tests.

Acceptance should prove:

- `limiterActiveBlocks` is observable when high-gain output exceeds the default safety threshold.
- `softCeilingTouchedSamples` is not continuously high under normal aggressive high-gain.
- CleanAmp paths remain stable.
- Fuzz reference behavior remains stable.
- No NaN/Inf, no silence/collapse, no excessive RMS ducking.
- RT policy scan does not regress.
