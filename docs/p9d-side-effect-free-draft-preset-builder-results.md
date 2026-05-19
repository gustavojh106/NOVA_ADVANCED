# P9D Side-effect-free Draft Preset Builder Results

Date: 2026-05-10

## Summary

P9D added an internal C++ draft preset builder and generated six draft `.nova-preset` files under `Resources/Presets/DraftFactory/generated/`.

Status: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`.

These are not final factory presets. Manual listening QA general remains pending, Distortion manual listening QA remains pending, and P7F/Reaper remains pending.

## Files Reviewed

- `Source/Core/AudioEngineTests.cpp`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/PluginStateModel.h`
- `Source/Core/SessionStore.h`
- `Source/Core/SessionPersistence.h`
- `Source/Core/PedalCatalog.h`
- `Source/Core/PedalRegistry.h`
- `Resources/Presets/DraftFactory/factory-bank.draft.json`
- `scripts/generate-draft-factory-presets.ps1`
- `scripts/check-audio-thread-policy.ps1`

## Builder Approach

- Helper lives in `Source/Core/AudioEngineTests.cpp` and is invoked only with `NOVA_RUN_P9D_DRAFT_BUILDER=1`.
- `Source/Core/PluginProcessor.cpp` checks that tooling env var before attaching `SessionLogger`, seeding user presets, or restoring startup preset state.
- Draft state is built through `SessionStore::Command::makeAddPedal`, `PluginStateModel` canonicalization, `PedalCatalog`, and `PedalRegistry`.
- Serialization writes `juce::ValueTree::writeToStream` directly to explicit draft output files.
- The builder does not call `SessionPersistence::savePresetToFile()` or `SessionPersistence::loadPresetFromFile()`.

## Generated Files

Output folder: `Resources/Presets/DraftFactory/generated/`

- `Clean-Studio.nova-preset`
- `Classic-Crunch.nova-preset`
- `Tight-Modern-Rhythm.nova-preset`
- `Wide-Ambient-Clean.nova-preset`
- `Funk-Comp-Clean.nova-preset`
- `Dry-Reference.nova-preset`

Manifest updated: `Resources/Presets/DraftFactory/factory-bank.draft.json`.

## Safety Confirmations

- No writes to `%APPDATA%/NOVA/Presets` were detected by the launcher.
- No `startup-preset.txt` changes were detected by the launcher.
- `STATE_SCHEMA_VERSION` remains `1`.
- No schema/ID changes.
- No `NOVA.jucer` changes.
- No DSP/audio-path changes.
- No AudioEngine, DryWetMixer, RoutingMixer, GraphBuilder, or OutputChain changes.
- No golden baseline updates.
- No preset is marked as shipping-approved.

## Round-trip Results

All six generated files parse as `NOVA_STATE`, restore through the canonical model, and pass canonical semantic save/load/save comparison.

| Draft | Generation | Round-trip | Process |
| --- | --- | --- | --- |
| Clean Studio | GENERATED_DRAFT | ROUND_TRIP_PASS | PROCESS_FINITE_PASS |
| Classic Crunch | GENERATED_DRAFT | ROUND_TRIP_PASS | PROCESS_FINITE_PASS |
| Tight Modern Rhythm | GENERATED_DRAFT | ROUND_TRIP_PASS | PROCESS_FINITE_PASS |
| Wide Ambient Clean | GENERATED_DRAFT | ROUND_TRIP_PASS | PROCESS_FINITE_PASS |
| Funk Comp Clean | GENERATED_DRAFT | ROUND_TRIP_PASS | PROCESS_FINITE_PASS |
| Dry Reference | GENERATED_DRAFT | ROUND_TRIP_PASS | PROCESS_FINITE_PASS |

## Process Finite / Gain Staging

Metrics from `artifacts/p9d-draft-preset-builder-report.json`:

| Draft | Peak | RMS | DC | nearClip | clipped |
| --- | ---: | ---: | ---: | ---: | ---: |
| Clean Studio | 0.16507147 | 0.06814992 | 0.00033269 | 0 | 0 |
| Classic Crunch | 0.88078070 | 0.35830422 | 0.00296493 | 0 | 0 |
| Tight Modern Rhythm | 0.60013866 | 0.11147278 | 0.00007852 | 0 | 0 |
| Wide Ambient Clean | 0.18907842 | 0.04512435 | 0.00018605 | 0 | 0 |
| Funk Comp Clean | 0.36105093 | 0.09371597 | 0.00034750 | 0 | 0 |
| Dry Reference | 0.09965807 | 0.03168143 | 0.00019163 | 0 | 0 |

These are process-safety measurements only, not golden metrics and not listening approval.

## Policy Checks Added

P9D policy checks cover:

- P9D results doc presence.
- Generated folder and generated files under `Resources/Presets/DraftFactory/generated/`.
- Manifest file paths restricted to the generated draft folder.
- No shipping-approved readiness marker.
- No schema bump.
- No golden baseline update.
- No startup-preset update logic in the generator.
- Generator does not call `SessionPersistence::savePresetToFile()` or `loadPresetFromFile()`.
- Manual listening, Distortion listening, and P7F/Reaper remain pending.

## Validation

Final validation executed:

- `git diff --check`: PASS. Git reported existing LF/CRLF working-copy warnings, but no whitespace errors.
- `scripts/generate-draft-factory-presets.ps1 -Configuration Debug -Platform x64`: PASS, generatedPresetCount=6.
- `scripts/check-audio-thread-policy.ps1`: PASS, failures=0, warnings=0, contractFailures=0, contractChecks=311.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
  - Base validation: `results=214 passes=6976 failures=0 failingResults=0`.
  - RT Release: `total=16 pass=16 warn=0 fail=0`.
  - Policy scan: PASS.
- `scripts/run-diagnostics-bundle.ps1`: PASS.
  - Base validation: `results=214 passes=6976 failures=0 failingResults=0`.
  - RT profile: `16/16/0/0`.
  - RT stability: `runs=3 passRuns=3 warnRuns=0 failRuns=0 blockingEvents=0`.
  - Policy: `status=PASS failures=0 contractFailures=0 contractChecks=311`.
- `build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode`: PASS.
- `build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_SharedCode`: PASS.
- `build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin`: PASS.
- `build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin`: PASS.
- `build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3`: PASS.
- `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`: PASS.
- second consecutive `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`: PASS.
- `scripts/run-golden-audio-metrics.ps1`: PASS against existing baseline; no baseline update.
- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `16/16/0/0`.
- `scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, every scenario `3/0/0`.

## Risks Remaining

- Presets are technical drafts only and need manual listening.
- Distortion/high-gain listening remains pending for `Tight Modern Rhythm`.
- Reaper/DAW smoke remains pending.
- No factory preset should be promoted from these files until P9E listening/gain review is complete.

## Recommendation For P9E

Run a focused manual listening and gain-staging pass over the six generated drafts, starting with Dry Reference, Clean Studio, Classic Crunch, and Tight Modern Rhythm. Keep Reaper/P7F separate until the DAW environment is available.
