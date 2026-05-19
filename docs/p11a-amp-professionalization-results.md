# P11A Amp Professionalization Results

Date: 2026-05-19

## Summary

P11A implemented a first complete amp-focused phase. The change improves the five active NOVA amp processors, their usable parameter surface, placed-module dashboard treatment, quick-add thumbnails, and deterministic coverage.

No schema version was changed. Existing parameter IDs were preserved. No OutputChain masking, global output reduction, golden baseline update, known-failure update, cabinet redesign, deploy, factory approval, or Reaper smoke was performed.

## Processor Changes

Clean Amp:

- Added `cleanHeadroom`.
- The headroom control remaps clean-stage drive range, compensation, and clean triode color.
- Default `0.50` keeps the old behavior close while allowing cleaner headroom or warmer breakup.

Classic Amp:

- Added `ampSag`.
- Added `ampBright`.
- `ampSag` controls the existing sag depth inside the amp core with smoothing.
- `ampBright` adds a pre-voice high shelf and lightly informs presence, useful for classic amp cut without only raising post-gain presence.

High Gain Amp:

- Added `hgResonance`.
- Added `hgFeel`.
- `hgResonance` adds a local low-shelf voicing stage before the final contour.
- `hgFeel` exposes the internal input-noise rejection/sag feel tradeoff. Lower values are tighter and more controlled; higher values are more open.
- Defaults preserve the P10G HighGainAmp -> Modern4x12 baseline intent.

Chime Amp:

- Added `chimeSag`.
- The control scales cathode-bias shift and power sag, matching the Class A model behavior instead of leaving it fixed.

Boutique Amp:

- Added `boutTouch`.
- The control scales envelope-responsive dynamic gain and clean threshold, making the amp more or less touch-sensitive without changing global level.

## UI Changes

- Amp placed slots now render as amp heads with a faceplate, grille, small controls, and an `AMP HEAD` badge.
- Quick-add amp thumbnails now render as compact amp-head modules instead of generic cards.
- Amp editors remain based on the existing premium control system for consistency, but each amp now exposes a richer amp-specific panel with six or seven controls.
- No global layout, sidebar, routing, drag/drop, cabinet UI, or product-wide UX redesign was included.

## New Coverage

Added deterministic P11A scenarios in `Source/Core/AudioEngineTests.cpp`:

- `p11a_amp_catalog_surface_guard`
- `p11a_amp_state_roundtrip_new_controls`
- `p11a_amp_tonal_stability_guard`
- `p11a_highgain_baseline_preservation`
- `p11a_clean_path_preservation`

These guards check:

- All active amps remain cataloged and constructible.
- New amp parameters exist.
- New processor-local controls round-trip through generic pedal state.
- Amps remain finite, audible, and below local near-clip/clipping thresholds.
- HighGainAmp -> Modern4x12 remains controlled.
- Clean Amp remains stable with the new headroom control.

## Policy Guard

`scripts/check-audio-thread-policy.ps1` was updated with P11A checks for:

- Required docs.
- Required P11A scenarios.
- Required new amp parameter IDs.
- No schema bump.
- No golden baseline update.
- No known-failure ignore addition.
- No OutputChain masking.
- No factory approval closure.
- Cabinets documented as out of scope.

## Risk Notes

- This phase adds processor-local parameter IDs. It does not change existing IDs or the global state schema. Old presets should recall with defaults for the new controls.
- The amp editors still use `PremiumPedalEditor`; a future phase can introduce a dedicated shared amp editor surface if manual listening confirms the new control surface.
- Clean Amp remains technically deeper than the other amps because it already has telemetry and a bounded room block. A future refactor can extract common amp utilities after manual validation.

## Manual Listening Status

Automated validation can confirm stability, bounds, and preservation guards. It cannot approve the final subjective feel of the new controls.

Recommended next manual listening pass:

- Clean Amp: sweep `Headroom` at low and medium Drive.
- Classic Amp: compare `Sag` low/high with palm-muted rhythm and open chords; sweep `Bright` against Presence.
- High Gain Amp: verify `Feel` does not reintroduce gate-like ducking and `Resonance` does not bring back ground-like low noise.
- Chime Amp: sweep `Sag` on edge-of-breakup chord work.
- Boutique Amp: sweep `Touch` with pick dynamics and guitar-volume cleanup.

## Final P11A Status

Current implementation status after validation: PASS.

Validation summary:

- `NOVA_SharedCode` Debug: PASS, 0 warnings, 0 errors.
- `NOVA_SharedCode` Release: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin` Debug: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin` Release: PASS, 0 warnings, 0 errors.
- `NOVA_VST3` Release: PASS, 0 warnings, 0 errors.
- `git diff --check`: PASS; line-ending warnings only.
- `scripts/check-audio-thread-policy.ps1`: PASS.
- `scripts/run-base-audio-validation.ps1`: PASS twice, `results=259`, `passes=7428`, `failures=0`.
- `scripts/run-golden-audio-metrics.ps1`: PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`.
- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `total=16`, `pass=16`, `warn=0`, `fail=0`.
- `scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, `runs=3`, `passRuns=3`, `warnRuns=0`, `failRuns=0`.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
- `scripts/run-diagnostics-bundle.ps1`: PASS summary, policy `contractChecks=552`.

Remaining status:

- Manual listening is still required before judging final subjective amp feel.
- Cabinet professionalization is intentionally pending for the next phase.
