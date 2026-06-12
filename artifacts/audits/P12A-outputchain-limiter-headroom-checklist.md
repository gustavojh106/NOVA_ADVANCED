# P12A OutputChain Limiter Headroom Checklist

## P0 Blockers

- [ ] In P12B, fix OutputChain limiter semantics so `0 dB` is not silent bypass if it is intended to be transparent safety mode.
- [ ] Keep the fix local to OutputChain; do not touch graph swap, RuntimeGraphManager, DryWetMixer, RoutingMixer, PluginStateModel schema, or pedal `processBlock()` bodies.
- [ ] Ensure latency reporting is honest if the lookahead limiter runs at `0 dB`.
- [ ] Add deterministic coverage proving default `0 dB` safety limiting catches true overs before the final soft ceiling.
- [ ] Add high-gain acceptance coverage for HighGainAmp -> Modern4x12 at master `+6 dB` and `+12 dB`.
- [ ] Assert no NaN/Inf, no silence/collapse, and no excessive RMS ducking.
- [ ] Run `scripts/check-audio-thread-policy.ps1` after implementation.
- [ ] Run `scripts/run-base-audio-validation.ps1` after implementation.

## P1 Important

- [ ] Add Boost -> HighGainAmp -> Modern4x12 acceptance scenario.
- [ ] Add Distortion -> HighGainAmp -> Modern4x12 acceptance scenario.
- [ ] Add CleanAmp clean path preservation scenario.
- [ ] Add Fuzz reference preservation scenario.
- [ ] Track `limiterActiveBlocks`, `limiterTouchedSamples`, and `softCeilingTouchedSamples` in the new tests.
- [ ] Require `softCeilingTouchedSamples` to stay low or zero during normal aggressive high-gain after the limiter fix.
- [ ] Validate that limiter release/hold does not create audible ducking proxies such as excessive RMS drop.
- [ ] Update existing tests that assert zero latency at `0 dB` only if the implementation changes default lookahead behavior.

## P2 Later

- [ ] Consider widening or reshaping final soft ceiling only after limiter behavior is corrected and measured.
- [ ] Consider documentation/release-note wording for `0 dB` limiter behavior change.
- [ ] Re-evaluate master gain maximum only if measured evidence still shows users can easily force bad final-stage behavior after the limiter fix.
- [ ] Consider additional diagnostics exposing rolling ratios for limiter activity versus soft-ceiling activity.
- [ ] Review factory presets for output limiter defaults only as a separate preset task.
