# P9E Draft Preset Technical Gain-Staging Results

Date: 2026-05-11

## Summary

P9E added a deterministic technical gain-staging gate for the six generated draft presets under `Resources/Presets/DraftFactory/generated/`.

Status: `WARN`.

All six drafts pass the available technical proxy gates and are recommended as `LISTENING_CANDIDATE` for the next subjective listening stage. The warning is intentional: the current P9D builder report does not emit direct limiter touched-sample or active-block telemetry, so P9E uses finite output, peak, RMS, DC, zero nearClip, and zero clipped samples as the sustained-clamp proxy.

These are technical recommendations only. No preset is factory-approved or final.

Manual listening QA general remains pending. Distortion manual listening QA remains pending. P7F/Reaper remains pending.

## Files Reviewed

- `Resources/Presets/DraftFactory/factory-bank.draft.json`
- `Resources/Presets/DraftFactory/generated/Clean-Studio.nova-preset`
- `Resources/Presets/DraftFactory/generated/Classic-Crunch.nova-preset`
- `Resources/Presets/DraftFactory/generated/Tight-Modern-Rhythm.nova-preset`
- `Resources/Presets/DraftFactory/generated/Wide-Ambient-Clean.nova-preset`
- `Resources/Presets/DraftFactory/generated/Funk-Comp-Clean.nova-preset`
- `Resources/Presets/DraftFactory/generated/Dry-Reference.nova-preset`
- `artifacts/p9d-draft-preset-builder-report.json`
- `docs/p9d-side-effect-free-draft-preset-builder-results.md`
- `scripts/generate-draft-factory-presets.ps1`
- `scripts/check-audio-thread-policy.ps1`

## Files Modified

- `scripts/validate-draft-factory-presets.ps1`
- `scripts/check-audio-thread-policy.ps1`
- `docs/p9e-draft-preset-technical-gain-staging-results.md`
- `artifacts/p9e-draft-preset-gain-staging-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.txt`

Manifest updated: no. `Resources/Presets/DraftFactory/factory-bank.draft.json` was left unchanged because P9D remains the side-effect-free generation writer and rewrites manifest readiness to `DRAFT_TECHNICAL`. P9E records recommendations in artifacts and docs only.

## Validator Approach

`scripts/validate-draft-factory-presets.ps1`:

- Reads `Resources/Presets/DraftFactory/factory-bank.draft.json`.
- Invokes `scripts/generate-draft-factory-presets.ps1` to refresh the P9D deterministic builder report.
- Validates that the generated files remain under `Resources/Presets/DraftFactory/generated/`.
- Verifies no shipping approval marker exists in the manifest.
- Verifies manual listening, Distortion listening, and Reaper smoke statuses remain pending or not applicable.
- Applies preset-specific peak/RMS/DC/nearClip/clipped thresholds to the P9D deterministic process metrics.
- Emits JSON and text artifacts under `artifacts/`.

The validator does not call `SessionPersistence::savePresetToFile()` or `SessionPersistence::loadPresetFromFile()`. It does not write to the user preset directory and does not update `startup-preset.txt`.

## Generated Artifacts

- `artifacts/p9e-draft-preset-gain-staging-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.txt`

## Metrics

| Draft | Scenario | Peak | RMS | DC | nearClip | clipped | invalid | Status | Readiness |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| Clean Studio | clean_nominal_headroom | 0.16507322 | 0.06892111 | 0.00033261 | 0 | 0 | 0 | WARN | LISTENING_CANDIDATE |
| Classic Crunch | crunch_nominal_bounded | 0.88080329 | 0.36194629 | 0.00296529 | 0 | 0 | 0 | WARN | LISTENING_CANDIDATE |
| Tight Modern Rhythm | high_gain_staccato_proxy | 0.60015476 | 0.11091146 | 0.00007863 | 0 | 0 | 0 | WARN | LISTENING_CANDIDATE |
| Wide Ambient Clean | ambient_tail_recovery_proxy | 0.18908252 | 0.04534781 | 0.00018603 | 0 | 0 | 0 | WARN | LISTENING_CANDIDATE |
| Funk Comp Clean | funk_transient_comp_proxy | 0.36105183 | 0.09480944 | 0.00034761 | 0 | 0 | 0 | WARN | LISTENING_CANDIDATE |
| Dry Reference | dry_reference_nominal | 0.09965807 | 0.03168143 | 0.00019163 | 0 | 0 | 0 | WARN | LISTENING_CANDIDATE |

## Per-Preset Gate Notes

- `Dry Reference`: finite, no nearClip, no clipped samples, low DC, dry-reference peak/RMS target passed.
- `Clean Studio`: finite, no nearClip, no clipped samples, conservative clean headroom target passed.
- `Classic Crunch`: finite, no nearClip, no clipped samples, bounded crunch target passed.
- `Tight Modern Rhythm`: finite high-gain staccato proxy, no nearClip, no clipped samples, low DC. Distortion manual listening remains pending.
- `Wide Ambient Clean`: finite ambient tail proxy, no nearClip, no clipped samples, low DC, no wet-runaway proxy failure.
- `Funk Comp Clean`: finite compressor-clean transient proxy, no nearClip, no clipped samples, conservative peak/RMS target passed.

All six carry the same documented warning: direct limiter touched-sample and active-block telemetry is not available from the current P9D report. P9E therefore cannot claim a direct limiter telemetry PASS; it can only pass the proxy gate.

## Safety Confirmations

- No writes to the user preset directory were reported.
- No `startup-preset.txt` changes were reported.
- `STATE_SCHEMA_VERSION` remains `1`.
- No schema/ID changes.
- No golden baseline update.
- No known-failure list update.
- No DSP/audio-path changes.
- No AudioEngine, DryWetMixer, RoutingMixer, GraphBuilder, or OutputChain changes.
- Draft presets remain non-shipping.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.

## Policy Scan Additions

`scripts/check-audio-thread-policy.ps1` now includes `p9e_*` checks for:

- P9E results doc presence.
- P9E validator script presence.
- P9E JSON/TXT report artifacts.
- P9E report status `PASS` or documented `WARN` with zero failures.
- Six per-preset technical readiness records using allowed statuses only.
- No shipping approval marker.
- No schema bump.
- No golden baseline update.
- No known-failure ignore added.
- No user preset directory target in the validator.
- No startup preset update logic in the validator.
- Generated draft files remain only under `Resources/Presets/DraftFactory/generated/`.
- Manual listening, Distortion listening, and P7F/Reaper remain pending.

## Risks Remaining

- Limiter touched-sample and limiter active-block counts are not directly emitted by the P9D builder report.
- Strong DI, staccato, and ambient recovery are represented by the existing P9D deterministic input shapes and P9E proxy thresholds, not by a new C++ telemetry surface.
- No subjective listening has been performed.
- High-gain listening for `Tight Modern Rhythm` remains pending.
- DAW/Reaper behavior remains unverified.

## Recommendation For P9F

Proceed to a focused P9F pass that either adds a narrowly scoped draft-preset telemetry helper for direct limiter active-block/touched-sample reporting, or starts manual listening candidate review from these six `LISTENING_CANDIDATE` recommendations. Keep factory approval, Reaper smoke, and Distortion listening closure separate.
