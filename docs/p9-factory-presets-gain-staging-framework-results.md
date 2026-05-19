# P9 Factory Presets / Gain Staging Framework Results

Date: 2026-05-08

## Summary

P9 created the technical and documentation framework for future NOVA factory presets and gain staging. It did not execute manual listening QA, create final presets, run real DAW smoke, change DSP/audio-path code, change schema/IDs, update golden baselines, or add known failures.

## Files Reviewed

- `Source/Core/SessionPersistence.h`
- `Source/Core/PluginStateModel.h`
- `Source/Core/SessionStore.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/PedalCatalog.h`
- `Source/Core/PedalRegistry.h`
- `Source/GUI/Wizards/PresetFinderWizard.h`
- `Source/Core/AudioEngineTests.cpp`
- `docs/p7d-preset-session-parameter-validation-results.md`
- `docs/p8e-manual-listening-qa-matrix.md`
- `docs/p8e-manual-listening-qa-readiness-results.md`

## Files Created

- `docs/p9-factory-presets-gain-staging-framework.md`
- `docs/p9-factory-preset-qa-checklist.md`
- `docs/p9-preset-gain-staging-issue-template.md`
- `docs/p9-draft-factory-preset-catalog.md`
- `docs/p9-factory-presets-gain-staging-framework-results.md`

## Files Modified

- `scripts/check-audio-thread-policy.ps1`

## Framework Created

- Factory preset categories.
- Naming convention and legal-safety naming rules.
- Proposed factory bank structure and metadata model.
- Gain staging rules and internal peak/limiter/near-clip targets.
- Chain templates for clean, drive, high gain, ambient, modulation, wah, and utility presets.
- Preset readiness levels: `DRAFT_TECHNICAL`, `LISTENING_CANDIDATE`, `FACTORY_CANDIDATE`, `FACTORY_APPROVED`, `REJECTED`.
- QA checklist for future preset leveling and round-trip validation.
- Issue template for preset/gain staging problems.

## Draft Catalog

Created `docs/p9-draft-factory-preset-catalog.md` with 17 NOT FINAL draft concepts. No `.nova-preset` files were generated because the current code seeds Delay factory-style presets directly into the user preset directory and there is no separate versioned factory bank manifest or metadata schema.

## Scripts / Tests

No C++ tests were added. `scripts/check-audio-thread-policy.ps1` was extended with non-fragile `p9_*` checks for required docs, draft approval guardrails, unique draft names, no schema bump, no golden baseline update, no known-failure marker, and pending markers for manual listening, Distortion manual listening, and P7F/Reaper.

## Relation To P8E

P8E prepared manual listening QA. P9 uses that readiness as the gate before any draft can move beyond technical draft/listening candidate status. P9 explicitly keeps manual listening QA pending.

## Why Presets Were Not Marked Final

Factory presets require real guitar DI listening evidence, level matching, bypass checks, tail/recovery checks, and product review. P9 only defines the framework and draft catalog, so no preset is factory approved.

## Pending Items

- Manual listening QA general remains pending and is not completed.
- Distortion manual listening QA remains pending and is not completed.
- P7F/Reaper remains pending and is not completed.
- Factory bank storage/versioning remains undecided.
- Preset metadata schema remains undocumented in runtime state and was not added.
- UI/UX is not final.

## Validation Executed

- `git diff --check`: PASS.
- `scripts/check-audio-thread-policy.ps1`: PASS, `failures=0 warnings=0 legacyWarnings=0 legacyQuarantined=4 contractFailures=0 contractChecks=237`.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
  - Base validation: PASS, `results=214 passes=6976 failures=0 failingResults=0`.
  - RT profile Release single-run: PASS, `16/16/0/0`.
  - Policy scan: PASS.
  - Golden metrics and RT stability were skipped by Fast mode.
- `scripts/run-diagnostics-bundle.ps1`: PASS, `results=214 passes=6976 failures=0 failingResults=0`, RT `16/16/0/0`, stability `3/0/0`, policy `contractChecks=237`.

## Risks Remaining

- Existing Delay factory-style seed behavior writes into the user preset directory; future factory bank work should decide whether that remains the shipping path.
- Draft catalog names and chains are not listening validated.
- Gain staging targets are approximate QA rules, not golden baselines.
- Adding rich preset metadata may require a future manifest or schema decision.

## Recommendation For P9B

Implement a factory preset manifest and optional generator only after the bank location and metadata model are approved. P9B should generate a small `DRAFT_TECHNICAL` bank, run save/load round-trip validation, and feed those drafts into the P8E manual listening process.
