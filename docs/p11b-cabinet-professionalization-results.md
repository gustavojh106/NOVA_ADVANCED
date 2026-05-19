# P11B Cabinet Professionalization Results

Date: 2026-05-19

## Summary

P11B implemented a first complete cabinet-focused professionalization pass. The active cabinets now expose direct low-cut, high-cut, and resonance controls, and cabinet cards/thumbnails now read as speaker cabinets rather than generic modules.

No existing parameter IDs were changed. No schema version was changed. No OutputChain masking, global output reduction, golden baseline update, factory approval, deploy, or Reaper smoke was performed.

## Processor Changes

Cabinet:

- Added `cabResonance`.
- Added `cabLowCut`.
- Added `cabHighCut`.
- Added a post-IR resonance peak filter at 92 Hz.
- Explicit Low Cut and High Cut now clamp the distance-driven contour filters safely.

Vintage 2x12:

- Added `v2x12Resonance`.
- Added `v2x12LowCut`.
- Added `v2x12HighCut`.
- Added a post-IR resonance peak filter at 118 Hz.
- Low Cut and High Cut give direct open-back rumble/fizz control without forcing Distance changes.

Modern 4x12:

- Added `m4x12Resonance`.
- Added `m4x12LowCut`.
- Added `m4x12HighCut`.
- Added a post-IR resonance peak filter at 82 Hz.
- High Cut gives explicit high-gain fizz control while preserving the existing distance-driven top contour.

## UI Changes

- Placed cabinet slots now render as cabinet/speaker modules with grille lines, speaker circles, and a `CAB IR` badge.
- Quick-add cabinet thumbnails now render with a compact cabinet grille and `CAB` badge.
- The active cabinet editors still use NOVA's premium control framework for consistency, but each cabinet now has a more useful cabinet-specific control surface.
- Product-wide layout, drag/drop behavior, routing UI, amps, pedals, and OutputChain were not redesigned.

## New Coverage

Added deterministic P11B scenarios in `Source/Core/AudioEngineTests.cpp`:

- `p11b_cabinet_catalog_surface_guard`
- `p11b_cabinet_state_roundtrip_new_controls`
- `p11b_cabinet_voicing_stability_guard`
- `p11b_modern4x12_highgain_baseline_preservation`
- `p11b_clean_cabinet_path_preservation`

These guards check:

- Active cabinets remain cataloged, constructible, and editor-backed.
- New cabinet parameters exist.
- New controls round-trip through generic processor state.
- Cabinets remain finite, audible, bounded, and free of clipping/near-clip samples.
- Rumble and high-frequency energy proxies remain bounded.
- HighGainAmp -> Modern4x12 and CleanAmp -> Cabinet stay preserved.

## Policy Guard

`scripts/check-audio-thread-policy.ps1` was updated with P11B checks for:

- Required docs.
- Required P11B scenarios.
- Required new cabinet parameter IDs.
- No schema bump.
- No golden baseline update.
- No known-failure ignore addition.
- No OutputChain masking.
- No factory approval closure.
- Manual listening remains separate.

## Risk Notes

- This phase adds processor-local cabinet parameters. Old presets should recall with defaults for the new controls.
- The IR generation itself was not surgically rewritten; this keeps the risk lower and preserves known tonal paths.
- Manual listening is required to tune final preferred defaults and ranges by ear.

## Manual Listening Recommendations

- HighGainAmp -> Modern4x12: sweep `m4x12HighCut` from 4.8 kHz to 7.0 kHz and verify fizz reduction without dull attack.
- HighGainAmp -> Modern4x12: sweep `m4x12LowCut` and `m4x12Resonance` with palm mutes for tightness and low-end bloom.
- CleanAmp -> Cabinet: confirm `cabHighCut` and `cabLowCut` do not make clean tone small or brittle.
- ClassicAmp -> Vintage 2x12: sweep `v2x12Resonance` and `v2x12HighCut` for open-back body and sparkle.
- BoutiqueAmp -> Vintage 2x12: test edge-of-breakup cleanup with different `Distance` and `High Cut` settings.

## Final P11B Status

Final implementation status after full validation: PASS.

Validation completed on 2026-05-19:

- `NOVA_SharedCode` Debug: PASS, 0 warnings, 0 errors.
- `NOVA_SharedCode` Release: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin` Debug: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin` Release: PASS, 0 warnings, 0 errors.
- `NOVA_VST3` Release: PASS, 0 warnings, 0 errors.
- `scripts/run-base-audio-validation.ps1`: PASS, results=264 passes=7512 failures=0.
- Second `scripts/run-base-audio-validation.ps1`: PASS, results=264 passes=7512 failures=0.
- `scripts/run-golden-audio-metrics.ps1`: PASS against existing baseline; no baseline update.
- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, total=16 pass=16 warn=0 fail=0.
- `scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, runs=3 passRuns=3 warnRuns=0 failRuns=0.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
- `scripts/run-diagnostics-bundle.ps1`: PASS summary generated.
- `scripts/check-audio-thread-policy.ps1`: PASS.

No manual listening approval is claimed in this phase. The cabinets are ready for a focused manual listening pass.
