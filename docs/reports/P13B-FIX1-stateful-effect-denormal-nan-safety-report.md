# P13B-FIX1 Stateful Effect Denormal and NaN Safety Report

Date: 2026-06-12
Task ID: P13B-FIX1-stateful-effect-denormal-nan-safety

## Scope

Implemented the small safety fixes from the P13B stateful/time-based effects audit. The patch is limited to Delay, Chorus, Flanger, Octave, targeted tests, `CLAUDE.md`, and these P13B-FIX1 report artifacts.

## Code Changes

- Added `juce::ScopedNoDenormals` at the top of `DelayPedal::processBlock`.
- Added `juce::ScopedNoDenormals` at the top of `FlangerPedal::processBlock`.
- Added `juce::ScopedNoDenormals` at the top of `ChorusPedal::processBlock`.
- Added Chorus invalid-sample scrubbing before DSP processing.
- Hardened Chorus delay-line writes/reads, biquad state, wet state, and feedback state against NaN/Inf latching.
- Added Octave invalid-sample scrubbing before DSP processing.
- Hardened Octave biquad, envelope follower, and period-tracker state against NaN/Inf latching.
- Corrected `CLAUDE.md` to state that the current Neural pedal is analytic/model-inspired waveshaping, not RTNeural inference.

No intentional tone changes were made for normal finite signal.

## Tests Added

- Delay near-silence feedback tail remains finite.
- Flanger near-silence feedback tail remains finite.
- Chorus survives NaN/Inf input and recovers to finite output.
- Chorus dry finite input is materially unchanged by the safety scrub.
- Octave survives NaN/Inf input and recovers to finite output.

Existing Octave dry-only transparency coverage also verifies finite dry input remains unchanged with the new input scrub in place.

## Validation

- `scripts/build-nova.ps1`: PASS, Standalone Debug target, 0 warnings, 0 errors. Initial run was up-to-date.
- Forced `NOVA_SharedCode` rebuild: PASS, 0 warnings, 0 errors.
- Standalone relink after SharedCode rebuild: PASS, 0 warnings, 0 errors.
- `scripts/run-base-audio-validation.ps1`: PASS, 273 results, 7,553 passes, 0 failures.
- Relevant effect tests: PASS as part of the NOVA validation suite.
- Full NOVA validation suite: PASS as part of base audio validation.
- RT/audio-thread policy scan: PASS, 32 active files, 38 active ranges, 0 failures, 0 warnings.
- Null-byte scan: PASS after report generation.

## Deferred Items

- Neural and Reverb CPU hotspots remain deferred profiling items.
- No Neural or Reverb DSP optimization was attempted in this task.

## Verdict

PASS. The stateful/time-based safety gaps are closed with a minimal patch, normal finite dry paths remain materially unchanged, and the updated suite validates the fix.
