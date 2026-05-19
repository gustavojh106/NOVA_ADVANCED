# P9G Release Readiness Checklist

Date: 2026-05-11

Status values: `PASS`, `PENDING`, `BLOCKED`, `NOT_STARTED`.

## Technical Gates

| Gate | Status | Evidence path | Owner | Notes |
| --- | --- | --- | --- | --- |
| Base validation | PASS | `audio-base-test-report.txt` | Engineering | `results=214 passes=6976 failures=0 failingResults=0` |
| Golden metrics | PASS | `artifacts/p4-offline-qa-report.txt` | Engineering | Passed against existing baseline; no baseline update |
| RT profile | PASS | `artifacts/rt-profile-release-x64-report.json` | Engineering | Release `16/16/0/0` |
| RT stability | PASS | `artifacts/rt-profile-stability-release-x64.json` | Engineering | All scenarios `3/0/0` |
| Policy scan | PASS | `artifacts/audio-thread-policy-scan.json` | Engineering | failures=0, warnings=0, contractFailures=0 |
| Diagnostics bundle | PASS | `artifacts/diagnostics-bundle.json` | Engineering | Aggregates current technical evidence |
| Generated drafts | PASS | `artifacts/p9d-draft-preset-builder-report.json` | Engineering | Six drafts generated under draft folder |
| Direct limiter telemetry | PASS | `artifacts/p9f-draft-preset-limiter-telemetry-report.json` | Engineering | Zero limiter dependency for all six drafts |

## Manual Gates

| Gate | Status | Evidence path | Owner | Notes |
| --- | --- | --- | --- | --- |
| Manual listening QA | PENDING | `docs/p8e-manual-listening-qa-matrix.md` | QA / Product | Not executed |
| Distortion listening | PENDING | `docs/p8b-distortion-focused-manual-qa-checklist.md` | QA / Product | High-gain listening not executed |
| Preset listening | PENDING | `docs/p9g-manual-qa-handoff.md` | QA / Product | Six draft presets ready for listening |
| Pedal listening | PENDING | `docs/p8e-manual-listening-session-checklist.md` | QA / Product | Broader pass follows draft preset listening |

## Integration Gates

| Gate | Status | Evidence path | Owner | Notes |
| --- | --- | --- | --- | --- |
| Reaper smoke | BLOCKED | `docs/p7f-reaper-smoke-blocked.md` | QA / Integration | Environment pending; not PASS |
| DAW smoke matrix | PENDING | `docs/p7e-daw-standalone-smoke-matrix.md` | QA / Integration | Run after candidates stabilize |
| VST3 load/save | PENDING | `Builds/VisualStudio2022/x64/Release/VST3/` | QA / Integration | VST3 builds; host smoke pending |
| Automation | PENDING | `docs/p7e-manual-host-smoke-checklist.md` | QA / Integration | Needs host validation |
| Offline render | PENDING | `docs/p7e-manual-host-smoke-checklist.md` | QA / Integration | Needs DAW render validation |
| Multi-instance | PENDING | `docs/p7e-manual-host-smoke-checklist.md` | QA / Integration | Needs host validation |

## Product Gates

| Gate | Status | Evidence path | Owner | Notes |
| --- | --- | --- | --- | --- |
| Factory presets approved | PENDING | `Resources/Presets/DraftFactory/factory-bank.draft.json` | Product / Audio | Drafts are technical candidates only |
| UI/UX final | PENDING | `Source/Core/PluginEditor.cpp` | Product / Design | Final polish not started |
| Wizards | PENDING | `Source/GUI/Wizards/` | Product / Design | Final wizard flow pending |
| Documentation | PENDING | `docs/` | Product / Engineering | Technical docs exist; user docs pending |
| Installer/package | NOT_STARTED | build/release packaging artifact TBD | Release | Packaging not started |
| Release notes | NOT_STARTED | release notes artifact TBD | Release | Not started |

## Release Candidate Rule

Release candidate work should not start until technical gates remain PASS, manual gates are closed, integration gates are closed, product gates are closed, and no draft preset remains only technically recommended.
