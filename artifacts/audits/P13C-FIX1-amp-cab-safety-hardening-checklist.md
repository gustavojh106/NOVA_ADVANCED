# P13C-FIX1 Amp/Cab Safety Hardening Checklist

Date: 2026-06-12
Task ID: P13C-FIX1-amp-cab-safety-hardening

## Implementation

- [x] Add oversized-block guard before `oversampler.processSamplesUp()` in ClassicAmp.
- [x] Add oversized-block guard before `oversampler.processSamplesUp()` in HighGainAmp.
- [x] Add oversized-block guard before `oversampler.processSamplesUp()` in BoutiqueAmp.
- [x] Add oversized-block guard before `oversampler.processSamplesUp()` in ChimeAmp.
- [x] Avoid allocation or resize in amp `processBlock()` oversized fallback path.
- [x] Scrub NaN/Inf input to `0.0f` in ClassicAmp.
- [x] Scrub NaN/Inf input to `0.0f` in HighGainAmp.
- [x] Scrub NaN/Inf input to `0.0f` in BoutiqueAmp.
- [x] Scrub NaN/Inf input to `0.0f` in ChimeAmp.
- [x] Sanitize amp state variables that can latch invalid values.
- [x] Protect HighGainAmp input noise gate/envelope state from NaN latch and persistent silence.
- [x] Add `juce::ScopedNoDenormals` to ClassicAmp `processBlock()`.
- [x] Add `juce::ScopedNoDenormals` to HighGainAmp `processBlock()`.
- [x] Add `juce::ScopedNoDenormals` to BoutiqueAmp `processBlock()`.
- [x] Add `juce::ScopedNoDenormals` to ChimeAmp `processBlock()`.
- [x] Add `juce::ScopedNoDenormals` to CabinetPedal `processBlock()`.
- [x] Add `juce::ScopedNoDenormals` to Modern4x12Cabinet `processBlock()`.
- [x] Add `juce::ScopedNoDenormals` to Vintage2x12Cabinet `processBlock()`.
- [x] Leave SyntheticIR voicing unchanged.
- [x] Leave CleanAmp source unchanged.

## Tests

- [x] Add oversized-block safety coverage for all amp models.
- [x] Prove the four newly guarded amps use finite dry fallback on oversized blocks.
- [x] Add NaN/Inf recovery coverage for ClassicAmp.
- [x] Add NaN/Inf recovery coverage for HighGainAmp.
- [x] Add NaN/Inf recovery coverage for BoutiqueAmp.
- [x] Add NaN/Inf recovery coverage for ChimeAmp.
- [x] Add HighGainAmp no-stuck-silence recovery assertion.
- [x] Add cabinet near-silence/tail finiteness coverage.
- [x] Preserve existing P12D, P13A, and P13B-FIX1 test coverage through base validation.

## Validation

- [x] SharedCode rebuild: PASS.
- [x] Standalone build: PASS.
- [x] Base/full NOVA validation: PASS, 276 results, 7593 passes, 0 failures.
- [x] Relevant amp/cab tests: PASS as part of base validation.
- [x] RT/audio-thread policy scan: PASS.
- [x] Null-byte scan: PASS.
- [x] Findings JSON parse: PASS.

## Deferred

- [x] BoutiqueAmp waveshaper/discontinuity left unchanged.
- [x] Vintage2x12 output trim left unchanged.
- [x] SyntheticIR decay/sample-rate feel left unchanged.
- [x] Sag/cathode/touch time constants left unchanged.
- [x] Amp/cab voicing left unchanged.
- [x] Release VST3 and manual DAW PDC null validation carried forward.
