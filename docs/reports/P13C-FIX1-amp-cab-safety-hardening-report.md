# P13C-FIX1 Amp/Cab Safety Hardening Report

Date: 2026-06-12
Task ID: P13C-FIX1-amp-cab-safety-hardening
Type: minimal safety patch

## Scope

This patch implements the non-tone-affecting safety fixes from the P13C amps/cabinets audit. It changes only the four lagging amp headers, the three cabinet headers, and the existing audio validation tests.

No graph architecture, state schema, OutputChain limiter logic, host PDC implementation, UI, drive pedals, dynamics pedals, delay/reverb/modulation pedals, or SyntheticIR voicing was changed.

## Changes

### Oversized-block guard

ClassicAmp, HighGainAmp, BoutiqueAmp, and ChimeAmp now store the prepared block capacity from `prepareToPlay()` and check the incoming block size before calling `oversampler.processSamplesUp()`.

If a block is larger than the prepared oversampling capacity, the processor returns a finite dry fallback for that block through the existing bypass wrapper. The guard does not allocate or resize in `processBlock()`, and it avoids passing oversized blocks into JUCE Oversampling.

### NaN/Inf scrub and state recovery

ClassicAmp, HighGainAmp, BoutiqueAmp, and ChimeAmp now scrub invalid input samples to `0.0f` before filters, envelopes, oversamplers, gates, sag/cathode state, or waveshapers can observe them. Stateful envelope variables that can latch invalid values are checked and reset to finite zero before update.

HighGainAmp's input noise rejection envelope is also guarded so NaN/Inf input cannot latch the gate into persistent silence after valid audio resumes.

Finite normal samples are passed through the scrub unchanged except sub-denormal values below `1.0e-30f`, which are flushed to zero for denormal safety.

### ScopedNoDenormals

`juce::ScopedNoDenormals` was added at the top of `processBlock()` for:

- ClassicAmp
- HighGainAmp
- BoutiqueAmp
- ChimeAmp
- CabinetPedal (Atlas 4x12)
- Modern4x12Cabinet
- Vintage2x12Cabinet

SyntheticIR was not changed because it does not own a realtime audio entry point in this path; it generates cabinet IR data offline during preparation.

### Tests

`Source/Core/AudioEngineTests.cpp` now includes P13C validation coverage for:

- Oversized blocks across Clean Amp, Classic Amp, High Gain Amp, Chime Amp, and Boutique Amp.
- Dry fallback preservation for the four newly guarded oversampled amps.
- NaN/Inf abuse followed by finite recovery for ClassicAmp, HighGainAmp, ChimeAmp, and BoutiqueAmp.
- HighGainAmp recovery RMS, guarding against a gate/envelope latch into persistent silence.
- Cabinet near-silence/tail finiteness for CabinetPedal, Modern4x12Cabinet, and Vintage2x12Cabinet.

Existing P12D, P13A, and P13B-FIX1 tests are still part of the same base NOVA validation suite.

## Validation

- SharedCode rebuild: PASS, 0 warnings, 0 errors.
- Standalone build: PASS, 0 warnings, 0 errors.
- Base/full NOVA validation suite via `scripts/run-base-audio-validation.ps1`: PASS, 276 results, 7593 passes, 0 failures, 0 failing results.
- Relevant amp/cab P13C tests: PASS as part of the NOVA validation suite.
- RT/audio-thread policy scan via `scripts/check-audio-thread-policy.ps1` with temp output paths: PASS.
- Null-byte scan: PASS after report generation.
- Findings JSON parse: PASS after report generation.

## Deferred Items

The following P13C audit topics were intentionally not changed because they can affect tone, gain, feel, or listening expectations:

- BoutiqueAmp waveshaper curve/discontinuity.
- Vintage2x12 output trim or containment changes.
- SyntheticIR decay and sample-rate feel.
- Sag, cathode, touch, and other envelope time constant behavior.
- General amp/cab voicing and product behavior.
- Release VST3 and manual DAW PDC null validation carried from P13A.

## Verdict

PASS. The requested P13C safety hardening is implemented without intentional tone changes. Oversized blocks are kept away from JUCE Oversampling in the four affected amps, invalid input cannot latch amp state into non-finite or silent recovery failures, denormal guards are present on the requested amp/cab audio entries, and the updated validation suite passes.
