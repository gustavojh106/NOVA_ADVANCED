# P9C Draft Preset Generator / Round-trip Gate Results

Date: 2026-05-10

## Summary

P9C audited the available preset generation routes and added a non-shipping validator script, but did not generate `.nova-preset` files. Generation is blocked safely because the currently exposed C++ preset save/load APIs update `startup-preset.txt`, and the Standalone processor construction path can seed presets into the user preset directory.

No DSP, audio-path, schema, IDs, UI, factory shipping bank, golden baseline, manual listening status, Distortion listening status, or Reaper/P7F status was changed.

## Files Reviewed

- `Source/Core/SessionPersistence.h`
- `Source/Core/PluginStateModel.h`
- `Source/Core/SessionStore.h`
- `Source/Core/PedalCatalog.h`
- `Source/Core/PedalRegistry.h`
- `Source/Core/Constants.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `Resources/Presets/DraftFactory/factory-bank.draft.json`
- `docs/p9b-draft-factory-bank-manifest.md`
- `docs/p9-draft-factory-preset-catalog.md`
- `docs/p7d-preset-session-parameter-validation-results.md`
- `docs/p8e-manual-listening-qa-matrix.md`
- `docs/p8e-manual-listening-qa-readiness-results.md`

## Files Created

- `scripts/generate-draft-factory-presets.ps1`
- `docs/p9c-draft-preset-generator-roundtrip-results.md`
- `artifacts/p9c-draft-preset-generator-report.json`

## Files Modified

- `Resources/Presets/DraftFactory/factory-bank.draft.json`
- `docs/p9b-draft-factory-bank-manifest.md`
- `docs/p9-draft-factory-preset-catalog.md`
- `scripts/check-audio-thread-policy.ps1`

## Generator Approach

`scripts/generate-draft-factory-presets.ps1` reads `Resources/Presets/DraftFactory/factory-bank.draft.json`, validates the six P9B draft records, computes intended output paths under `Resources/Presets/DraftFactory/generated/`, and writes an audit report. It intentionally does not generate `.nova-preset` files until a side-effect-free C++ builder exists.

The script validates:

- unique draft names.
- allowed readiness only: `DRAFT_TECHNICAL` or `LISTENING_CANDIDATE`.
- manual listening remains pending.
- Distortion listening remains pending or not applicable.
- Reaper smoke remains pending.
- registered type IDs only.
- legal zones.
- canonical zone order: `Pre -> Amp -> FX -> Cabinet`.
- no duplicate amp/cab per line.
- Pre/FX capacity under `MAX_PEDALS_PER_FLEX_ZONE`.

## Generation Decision

Generated `.nova-preset` files: none.

Exact output folder that would be used: `Resources/Presets/DraftFactory/generated/`.

Blocking reasons:

- `SessionPersistence::savePresetToFile()` writes `startup-preset.txt` after saving.
- `SessionPersistence::loadPresetFromFile()` writes `startup-preset.txt` after loading.
- The Standalone processor construction path currently calls the existing Delay preset user-seed path.
- No non-shipping CLI/helper exposes `PluginStateModel` canonicalization plus `ValueTree::writeToStream` without those side effects.
- Reimplementing JUCE `ValueTree` binary serialization in PowerShell would be fragile and was not done.

## Draft Results

| Name | Manifest validation | Generation | Intended file |
| --- | --- | --- | --- |
| Clean Studio | PASS | BLOCKED | `Resources/Presets/DraftFactory/generated/Clean-Studio.nova-preset` |
| Classic Crunch | PASS | BLOCKED | `Resources/Presets/DraftFactory/generated/Classic-Crunch.nova-preset` |
| Tight Modern Rhythm | PASS | BLOCKED | `Resources/Presets/DraftFactory/generated/Tight-Modern-Rhythm.nova-preset` |
| Wide Ambient Clean | PASS | BLOCKED | `Resources/Presets/DraftFactory/generated/Wide-Ambient-Clean.nova-preset` |
| Funk Comp Clean | PASS | BLOCKED | `Resources/Presets/DraftFactory/generated/Funk-Comp-Clean.nova-preset` |
| Dry Reference | PASS | BLOCKED | `Resources/Presets/DraftFactory/generated/Dry-Reference.nova-preset` |

P9C corrected the manifest order for `Wide Ambient Clean` to keep the template canonical: `Clean Amp -> Chorus -> Delay -> Reverb -> Cabinet`.

## Round-trip Results

Round-trip validation was not executed because no `.nova-preset` files were generated. This is intentional; P9C does not create weak raw-byte or hand-authored XML tests.

Pending round-trip gate for P9D:

- generate draft files via a side-effect-free C++ helper.
- parse/load succeeds.
- save -> load -> save canonical semantic compare.
- restore into model/engine succeeds.
- prepare/process deterministic input produces finite output.
- chain remains canonical.
- schema remains `1`.

## Gain Staging Results

No measured gain staging metrics were produced because no generated preset files exist. The P9B manifest target notes remain expectations only, not golden baselines.

Pending measurements for P9D:

- peak.
- RMS if helper support exists.
- nearClip samples.
- invalid/clipped samples.
- limiter/clamp activity if exposed.
- DC.
- finite output.
- bypass jump where practical.

## Safety Confirmations

- No writes to `%APPDATA%/NOVA/Presets`.
- No `startup-preset.txt` changes.
- No `.nova-preset` files generated.
- No `Resources/Presets/DraftFactory/generated/` folder was required.
- `STATE_SCHEMA_VERSION` remains `1`.
- No schema/ID changes.
- No `NOVA.jucer` changes.
- No golden baseline updates.
- No known failures added.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.

## Validation Executed

- `scripts/generate-draft-factory-presets.ps1`: PASS with status `BLOCKED_SAFE_NO_GENERATION`, `generatedPresetCount=0`, `wouldGeneratePresetCount=6`.
- `git diff --check`: PASS. Git reported existing LF/CRLF working-copy warnings, but no whitespace errors.
- `scripts/check-audio-thread-policy.ps1`: PASS, `failures=0 warnings=0 legacyWarnings=0 legacyQuarantined=4 contractFailures=0 contractChecks=273`.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
  - Base validation: PASS, `results=214 passes=6976 failures=0 failingResults=0`.
  - RT profile Release single-run: PASS, `16/16/0/0`.
  - Policy scan: PASS.
  - Golden metrics and RT stability were skipped by Fast mode.
- `scripts/run-diagnostics-bundle.ps1`: PASS, `results=214 passes=6976 failures=0 failingResults=0`, RT `16/16/0/0`, stability `3/0/0`, policy `contractChecks=273`.

## Recommendation For P9D

Add a small side-effect-free C++ draft preset builder entrypoint that:

- constructs `SessionStore` state through `PluginStateModel::insertPedal`.
- writes `ValueTree::writeToStream` directly to an explicit draft output path.
- does not call `SessionPersistence::savePresetToFile()` or `loadPresetFromFile()`.
- does not instantiate `NOVAAudioProcessor` unless the user preset seed path is explicitly disabled for tooling.
- validates restore/process finite in the same tooling pass.
