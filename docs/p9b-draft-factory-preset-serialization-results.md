# P9B Draft Factory Preset Serialization Results

Date: 2026-05-10

## Summary

P9B created a source-controlled draft factory bank manifest and validation guardrails without touching runtime schema, DSP, audio routing, user preset seeding, or startup preset selection. No final factory presets were created.

## Files Reviewed

- `docs/p9-factory-presets-gain-staging-framework.md`
- `docs/p9-draft-factory-preset-catalog.md`
- `docs/p9-factory-preset-qa-checklist.md`
- `docs/p9-factory-presets-gain-staging-framework-results.md`
- `docs/p7d-preset-session-parameter-validation-results.md`
- `docs/p8e-manual-listening-qa-matrix.md`
- `docs/p8e-manual-listening-qa-readiness-results.md`
- `Source/Core/PedalCatalog.h`
- `Source/Core/PedalRegistry.h`
- `Source/Core/Constants.h`
- `NOVA.jucer`
- `scripts/check-audio-thread-policy.ps1`

## Files Created

- `docs/p9b-draft-factory-bank-manifest.md`
- `docs/p9b-draft-factory-preset-serialization-results.md`
- `Resources/Presets/DraftFactory/factory-bank.draft.json`

## Files Modified

- `docs/p9-draft-factory-preset-catalog.md`
- `scripts/check-audio-thread-policy.ps1`

## Draft Bank Location

The draft bank lives at `Resources/Presets/DraftFactory/`. This keeps it in source control near other resources while avoiding automatic user preset seeding. `NOVA.jucer` was not changed, so the JSON manifest is not embedded as a JUCE binary resource and is not copied into `%APPDATA%/NOVA/Presets`.

## Preset File Decision

P9B did not generate `.nova-preset` files. The safe decision is manifest-only because the current repo has preset persistence and user seeding paths, but no clean standalone draft-bank builder was identified that can serialize external draft presets without risking hand-built ValueTree/XML drift or writes to the user preset directory.

## Selected Drafts

| Name | Category | Readiness | Serialization status | Reason |
| --- | --- | --- | --- | --- |
| Clean Studio | Clean | DRAFT_TECHNICAL | MANIFEST_ONLY | Safe representative clean amp/cab draft; generation deferred to P9C builder. |
| Classic Crunch | Crunch | DRAFT_TECHNICAL | MANIFEST_ONLY | Uses registered OD, amp, and cab; needs later round-trip and bypass-level validation. |
| Tight Modern Rhythm | High Gain | DRAFT_TECHNICAL | MANIFEST_ONLY | High-gain draft with gate/boost/amp/cab; Distortion listening remains pending. |
| Wide Ambient Clean | Ambient | DRAFT_TECHNICAL | MANIFEST_ONLY | Exercises FX-zone ambience without exceeding zone limits; tail validation deferred. |
| Funk Comp Clean | Funk / Compressor Clean | DRAFT_TECHNICAL | MANIFEST_ONLY | Exercises compressor/EQ clean flow; pumping/listening validation deferred. |
| Dry Reference | Utility / Calibration | DRAFT_TECHNICAL | MANIFEST_ONLY | Defines dry reference expectations without writing runtime preset state. |

## Validation

- Manifest JSON exists and parses.
- Manifest contains six draft records.
- Draft names are unique.
- Readiness is limited to `DRAFT_TECHNICAL`.
- No draft is marked as shipping approved.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.
- All referenced type IDs are registered in `PedalCatalog` and `PedalRegistry`.
- Zone assignments are valid, with at most one amp and one cabinet per line.
- No draft exceeds `MAX_PEDALS_PER_FLEX_ZONE`.
- `STATE_SCHEMA_VERSION` remains `1`.
- No golden baseline files were updated.
- No known failures were added.
- No `.nova-preset` files were generated.
- No writes were made to `%APPDATA%/NOVA/Presets`.
- `startup-preset.txt` was not created or changed.

## Validation Executed

- `git diff --check`: PASS. Git reported existing LF/CRLF working-copy warnings, but no whitespace errors.
- JSON parse check for `Resources/Presets/DraftFactory/factory-bank.draft.json`: PASS, `PresetCount=6`.
- `scripts/check-audio-thread-policy.ps1`: PASS, `failures=0 warnings=0 legacyWarnings=0 legacyQuarantined=4 contractFailures=0 contractChecks=255`.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
  - Base validation: PASS, `results=214 passes=6976 failures=0 failingResults=0`.
  - RT profile Release single-run: PASS, `16/16/0/0`.
  - Policy scan: PASS.
  - Golden metrics and RT stability were skipped by Fast mode.
- `scripts/run-diagnostics-bundle.ps1`: PASS, `results=214 passes=6976 failures=0 failingResults=0`, RT `16/16/0/0`, stability `3/0/0`, policy `contractChecks=255`.

## Gain Staging Targets

Because no real preset files were generated, P9B records expected targets rather than measured audio metrics:

| Draft | Expected target |
| --- | --- |
| Clean Studio | Unity-ish perceived clean baseline with headroom and no expected limiter activity. |
| Classic Crunch | Conservative rhythm peak level, with later OD bypass-level matching. |
| Tight Modern Rhythm | Controlled palm-muted peaks, rare nearClip samples, no sustained clamp. |
| Wide Ambient Clean | Finite ambience tails and no sustained wet-effect clamp. |
| Funk Comp Clean | Preserved attack and conservative compressor makeup. |
| Dry Reference | Finite unity reference with no nearClip or limiter activity expected. |

These are not golden metrics and must not be promoted into golden baselines.

## Pending

- Generate `.nova-preset` files only after P9C adds or identifies a safe draft preset builder that uses the runtime canonicalization path.
- Validate save/load/save canonical round-trip.
- Restore drafts into an engine, prepare/process deterministic input, and assert finite output.
- Measure conservative peak, nearClip, limiter activity, and clamp behavior.
- Confirm diagnostics bundle compatibility for generated drafts.
- Run manual listening QA general.
- Run Distortion/high-gain listening QA.
- Run Reaper/P7F smoke when the environment is ready.

## P9C Recommendation

Add a non-shipping draft preset generator that writes only under `Resources/Presets/DraftFactory/`, uses `PluginStateModel`/`SessionPersistence` canonical paths rather than hand-authored XML, and has explicit tests proving no writes to `%APPDATA%/NOVA/Presets` and no `startup-preset.txt` update.
