# P9B Draft Factory Bank Manifest

Date: 2026-05-10

Status: internal draft. This is not a shipping factory bank and contains no approved factory presets.

## Bank

| Field | Value |
| --- | --- |
| bankName | NOVA Draft Factory Bank |
| bankVersion | p9b-draft-001 |
| readiness | DRAFT_TECHNICAL |
| generatedBy | P9D side-effect-free draft preset builder |
| schemaVersionTarget | 1 |
| manifestJson | `Resources/Presets/DraftFactory/factory-bank.draft.json` |
| generatedPresetFiles | 6 under `Resources/Presets/DraftFactory/generated/` |
| automaticSeed | disabled |
| userPresetDirectoryWrites | none |
| startupPresetPointerUpdate | none |

## Rules

- Maximum readiness in P9B is `DRAFT_TECHNICAL` or `LISTENING_CANDIDATE`.
- All selected records remain `DRAFT_TECHNICAL`.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.
- P9D generated draft `.nova-preset` files only under `Resources/Presets/DraftFactory/generated/`.
- No files are written to `%APPDATA%/NOVA/Presets`.
- `startup-preset.txt` is not created or changed.
- `STATE_SCHEMA_VERSION` remains `1`.

## Draft Records

| Name | Category | Tags | Readiness | Intended input | Output target | Chain template | File path | Manual listening | Distortion listening | Reaper smoke | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Clean Studio | Clean | clean, baseline, studio, cabinet | DRAFT_TECHNICAL | Guitar DI, humbucker or single-coil, conservative nominal input. | Unity-ish perceived level; finite output; no expected limiter activity. | A:Amp=Clean Amp; A:Cabinet=Cabinet | `Resources/Presets/DraftFactory/generated/Clean-Studio.nova-preset` | pending | not_applicable | pending | Generated draft; not listening-approved. |
| Classic Crunch | Crunch | crunch, rhythm, overdrive, classic | DRAFT_TECHNICAL | Guitar DI, bridge pickup rhythm level. | Conservative rhythm level; no sustained OutputChain clamp. | A:Pre=Overdrive; A:Amp=Classic Amp; A:Cabinet=Cabinet | `Resources/Presets/DraftFactory/generated/Classic-Crunch.nova-preset` | pending | not_applicable | pending | Requires later OD bypass-level check. |
| Tight Modern Rhythm | High Gain | high-gain, rhythm, gate, boost, modern | DRAFT_TECHNICAL | Guitar DI, bridge humbucker expected. | Controlled peak level; rare nearClip samples; no sustained clamp. | A:Pre=Noise Gate; A:Pre=Boost; A:Amp=High Gain Amp; A:Cabinet=Modern 4x12 | `Resources/Presets/DraftFactory/generated/Tight-Modern-Rhythm.nova-preset` | pending | pending | pending | High-gain draft; Distortion listening stays pending. |
| Wide Ambient Clean | Ambient | clean, ambient, chorus, delay, reverb, wide | DRAFT_TECHNICAL | Guitar DI, clean chords and swells. | Tail buildup must stay finite; wet effects should not drive clamp. | A:Amp=Clean Amp; A:FX=Chorus; A:FX=Delay; A:FX=Reverb; A:Cabinet=Cabinet | `Resources/Presets/DraftFactory/generated/Wide-Ambient-Clean.nova-preset` | pending | not_applicable | pending | Needs tail/recovery listening before promotion. |
| Funk Comp Clean | Funk / Compressor Clean | funk, clean, compressor, eq, rhythm | DRAFT_TECHNICAL | Guitar DI, single-coil or split-coil muted rhythm. | Preserve attack with conservative compressor makeup. | A:Pre=Compressor; A:Pre=EQ; A:Amp=Clean Amp; A:Cabinet=Cabinet | `Resources/Presets/DraftFactory/generated/Funk-Comp-Clean.nova-preset` | pending | not_applicable | pending | Requires later compressor pumping check. |
| Dry Reference | Utility / Calibration | utility, dry, reference, calibration | DRAFT_TECHNICAL | Guitar DI or deterministic validation input. | Finite unity reference path; no nearClip or limiter activity expected. | none | `Resources/Presets/DraftFactory/generated/Dry-Reference.nova-preset` | pending | not_applicable | pending | Generated draft reference; not a final factory preset. |

## Validation Notes

- Names are unique.
- All processor type IDs are present in `PedalCatalog` and `PedalRegistry`: `Boost`, `Cabinet`, `Chorus`, `Classic Amp`, `Clean Amp`, `Compressor`, `Delay`, `EQ`, `High Gain Amp`, `Modern 4x12`, `Noise Gate`, `Overdrive`, and `Reverb`.
- Zone assignments follow `Pre -> Amp -> FX -> Cabinet` and do not duplicate amps or cabinets on a line.
- Pre and FX zone counts are below `MAX_PEDALS_PER_FLEX_ZONE`.
- P9D builder report: `artifacts/p9d-draft-preset-builder-report.json`.
- All six generated drafts report `ROUND_TRIP_PASS` and `PROCESS_FINITE_PASS`.
- Measured process metrics are technical gates only, not golden baselines and not listening approval.

## P9C Generator Audit

- `scripts/generate-draft-factory-presets.ps1` validates this manifest but does not generate `.nova-preset` files.
- P9C status is `BLOCKED_SAFE_NO_GENERATION`.
- `filePath` values remain `none`/`null`.
- Blocking reason: the available preset save/load APIs update `startup-preset.txt`, and the Standalone processor construction path can seed user presets. P9C does not use those paths for draft generation.

## P9D Builder Update

- `scripts/generate-draft-factory-presets.ps1` now invokes the P9D C++ helper via `NOVA_RUN_P9D_DRAFT_BUILDER=1`.
- P9D status: `PASS`, generatedPresetCount=6.
- Builder output is restricted to `Resources/Presets/DraftFactory/generated/`.
- No `%APPDATA%/NOVA/Presets` changes were detected.
- No `startup-preset.txt` changes were detected.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.
