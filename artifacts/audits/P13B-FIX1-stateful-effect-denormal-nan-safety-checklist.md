# P13B-FIX1 Stateful Effect Denormal and NaN Safety Checklist

Date: 2026-06-12

## Scope

- [x] Delay effect files only.
- [x] Chorus effect files only.
- [x] Flanger effect files only.
- [x] Octave effect files only.
- [x] Tests for these effects.
- [x] `CLAUDE.md` documentation correction only.
- [x] P13B-FIX1 report files.

## Code Requirements

- [x] Add `juce::ScopedNoDenormals` to Delay audio entry.
- [x] Add `juce::ScopedNoDenormals` to Chorus audio entry.
- [x] Add `juce::ScopedNoDenormals` to Flanger audio entry.
- [x] Add Chorus NaN/Inf input scrub.
- [x] Prevent Chorus invalid delay/filter/feedback state from latching.
- [x] Add Octave NaN/Inf input scrub.
- [x] Prevent Octave invalid filter/envelope/tracker state from latching.
- [x] Keep normal finite signal unchanged except for invalid-sample replacement behavior.
- [x] Do not optimize Neural or Reverb CPU hotspots.

## Tests

- [x] Delay near-silence tail remains finite.
- [x] Chorus survives NaN/Inf input and recovers to finite output.
- [x] Flanger near-silence tail remains finite.
- [x] Octave survives NaN/Inf input and recovers to finite output.
- [x] Normal finite dry input is materially unchanged by the new safety scrub coverage.
- [x] Existing P13B/P12D/P13A base tests still pass.

## Documentation

- [x] Remove inaccurate claim that current Neural uses RTNeural inference.
- [x] State current Neural accurately as analytic/model-inspired waveshaping.
- [x] Leave Neural name and product behavior unchanged.

## Validation

- [x] Relevant effect tests.
- [x] Base audio validation.
- [x] Full NOVA validation suite.
- [x] Null-byte scan.
- [x] RT/audio-thread policy scan.

## Deferred

- [ ] Neural CPU profiling/optimization.
- [ ] Reverb CPU profiling/optimization.
