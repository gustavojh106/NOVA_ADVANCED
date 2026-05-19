# P9F Draft Preset Direct Limiter Telemetry Results

Date: 2026-05-11

## Summary

P9F closes the P9E proxy-only warning for draft preset gain staging. Direct limiter telemetry is now present in the P9D draft builder report and consumed by `scripts/validate-draft-factory-presets.ps1`.

Status: `PASS`.

All six generated draft presets report:

- `limiterTouchedSamples=0`
- `limiterActiveBlocks=0`
- `sustainedClampBlocks=0`
- `limiterMaxReductionDb=0`
- `invalidSamples=0`
- `nearClipSamples=0`
- `clippedSamples=0`

All six remain technical `LISTENING_CANDIDATE` recommendations only. No draft is shipping-approved or final.

Manual listening QA general remains pending. Distortion manual listening QA remains pending. P7F/Reaper remains pending.

## Files Reviewed

- `Source/Core/DSP/Global/OutputChain.h`
- `Source/Core/DSP/Global/OutputChain.cpp`
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `scripts/generate-draft-factory-presets.ps1`
- `scripts/validate-draft-factory-presets.ps1`
- `artifacts/p9d-draft-preset-builder-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.json`
- `Resources/Presets/DraftFactory/factory-bank.draft.json`

## Files Modified

- `Source/Core/AudioEngineTests.cpp`
- `scripts/validate-draft-factory-presets.ps1`
- `scripts/check-audio-thread-policy.ps1`
- `docs/p9f-draft-preset-direct-limiter-telemetry-results.md`
- `artifacts/p9d-draft-preset-builder-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.txt`
- `artifacts/p9f-draft-preset-limiter-telemetry-report.json`
- `artifacts/p9f-draft-preset-limiter-telemetry-report.txt`

Manifest updated: no. P9F keeps recommendations in artifacts/docs and leaves `Resources/Presets/DraftFactory/factory-bank.draft.json` unchanged.

## Telemetry Audit

Existing telemetry found:

- `OutputChainProcessor::DebugSnapshot` already exposes `limiterTouchedSamples`, `limiterActiveBlocks`, `limiterMaxReductionDb`, `limiterDeltaPeak`, `softCeilingTouchedSamples`, and guard counters.
- `AudioEngine::getOutputChainDebugSnapshot()` already exposes the active output-chain snapshot.
- Existing tests already use this snapshot for routing and limiter assertions.

Telemetry added:

- `P9DProcessMetrics` now records `invalidSamples`, `limiterTouchedSamples`, `limiterActiveBlocks`, `sustainedClampBlocks`, `limiterMaxReductionDb`, `limiterDeltaPeak`, and `softCeilingTouchedSamples`.
- `p9dProcessDraft()` samples `AudioEngine::getOutputChainDebugSnapshot()` around deterministic processing blocks and accumulates limiter counters for each generated draft.
- `p9dBuildReportJson()` writes those fields into `artifacts/p9d-draft-preset-builder-report.json`.
- `scripts/validate-draft-factory-presets.ps1` consumes the direct limiter fields and emits P9F JSON/TXT reports.

No `OutputChain` DSP code was changed. No limiter threshold, limiter math, release behavior, gain behavior, soft ceiling behavior, or audio runtime behavior was changed.

## Generated Artifacts

- `artifacts/p9d-draft-preset-builder-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.json`
- `artifacts/p9e-draft-preset-gain-staging-report.txt`
- `artifacts/p9f-draft-preset-limiter-telemetry-report.json`
- `artifacts/p9f-draft-preset-limiter-telemetry-report.txt`

P9E warning status after P9F: resolved to `PASS` because direct limiter telemetry is now available and all six draft presets report zero limiter dependency.

## Per-Preset Metrics

| Draft | Peak | RMS | DC | nearClip | clipped | invalid | limiterTouched | limiterActiveBlocks | sustainedClampBlocks | maxReductionDb | Status | Readiness |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| Clean Studio | 0.16507283 | 0.06736890 | 0.00033277 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | PASS | LISTENING_CANDIDATE |
| Classic Crunch | 0.88079733 | 0.36019193 | 0.00296498 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | PASS | LISTENING_CANDIDATE |
| Tight Modern Rhythm | 0.60013866 | 0.11147278 | 0.00007852 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | PASS | LISTENING_CANDIDATE |
| Wide Ambient Clean | 0.18907842 | 0.04512435 | 0.00018605 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | PASS | LISTENING_CANDIDATE |
| Funk Comp Clean | 0.36104861 | 0.09426657 | 0.00034733 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | PASS | LISTENING_CANDIDATE |
| Dry Reference | 0.09965807 | 0.03168143 | 0.00019163 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | PASS | LISTENING_CANDIDATE |

## Gain-Staging Adjustment

No draft needs a gain-staging adjustment from P9F. None touched the global limiter in the deterministic draft validation render.

## Safety Confirmations

- No writes to the user preset directory were reported.
- No `startup-preset.txt` changes were reported.
- `STATE_SCHEMA_VERSION` remains `1`.
- No schema/ID changes.
- No golden baseline update.
- No known-failure list update.
- No `OutputChain` behavior changes.
- No `AudioEngine`, `DryWetMixer`, `RoutingMixer`, or `GraphBuilder` behavior changes.
- No `SessionPersistence::savePresetToFile()` or `SessionPersistence::loadPresetFromFile()` use was added to the generator.
- Draft presets remain non-shipping.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.

## Policy Scan Additions

`scripts/check-audio-thread-policy.ps1` now includes `p9f_*` checks for:

- P9F results doc presence.
- P9F limiter telemetry JSON/TXT artifacts.
- P9F report status `PASS`.
- Direct limiter telemetry present and zero for all six drafts.
- P9F readiness values limited to technical statuses.
- Validator script consumes direct limiter telemetry.
- No shipping approval marker.
- No schema bump.
- No golden baseline update.
- No known-failure ignore added.
- No user preset directory writes.
- No startup preset pointer writes.
- Generated draft files remain only under `Resources/Presets/DraftFactory/generated/`.
- Manual listening, Distortion listening, and P7F/Reaper remain pending.
- No OutputChain, AudioEngine, DryWetMixer, RoutingMixer, or GraphBuilder behavior files changed.

## Risks Remaining

- The gate is deterministic technical validation only, not listening QA.
- `Tight Modern Rhythm` still requires high-gain manual listening.
- Reaper/DAW behavior remains unverified.
- Draft factory presets are still not final production presets.

## Recommendation For P9G

Run a targeted manual listening candidate pass over the six technical candidates. If subjective listening finds level imbalance, harshness, weak tails, or high-gain feel issues, make those as explicit P9G gain-staging or voicing adjustments with separate validation.
