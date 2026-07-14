# NOVA Release Blocker Fix Report

## Executive Summary

* Status: CONDITIONAL PASS
* Can proceed to settings window reconstruction: YES
* Internal release-engineering blockers cleared: VST3 manifest freshness, direct-target SharedCode rebuild safety, Release diagnostic gating, P14B ownership/determinism, internal PDC/latency regression coverage, and reproducibility documentation.
* Remaining external validation: host-level VST3 scan/PDC smoke remains pending because no local VST3 validator, pluginval, or DAW executable was available on this machine.
* Release/user-QA status: do not treat this as final ship approval until host-level VST3/PDC smoke is captured.

## Claude Audit Blockers Addressed

* Blocker: Release VST3 build succeeds, but VST3 `moduleinfo.json` is stale.
* Action taken: `scripts/build-nova.ps1` now builds the manifest helper before VST3, runs the helper after the VST3 build, normalizes the helper's JSON5 trailing commas to strict JSON, verifies parsing, and rejects stale manifest timestamps.
* Evidence: final production build `artifacts/release-vst3-production-final2.log`; binary timestamp `2026-07-08T22:58:19.6094364Z`; `moduleinfo.json` timestamp `2026-07-08T22:58:20.0379819Z`; parsed name/version `NOVA` / `1.0.0`.
* Status: PASS

* Blocker: Building `NOVA_VST3` directly can reuse stale `NOVA_SharedCode.lib`.
* Action taken: direct `NOVA_VST3` script builds now run `NOVA_SharedCode`, `NOVA_VST3ManifestHelper`, then `NOVA_VST3`; direct standalone builds run `NOVA_SharedCode` first. Release builds always use `Rebuild` to avoid stale production/diagnostic object reuse.
* Evidence: final log shows `NOVA_SharedCode -> NOVA_VST3ManifestHelper -> NOVA_VST3`, all `Action: Rebuild`, all `0 Warning(s)` and `0 Error(s)`.
* Status: PASS

* Blocker: Environment-triggered diagnostics remained callable in Release builds.
* Action taken: diagnostic env hooks are compiled for Debug or explicit `NOVA_ENABLE_RELEASE_DIAGNOSTICS=1` only. Session logging is also disabled in production Release unless an explicit diagnostic/session logging flag is set.
* Evidence: production Release standalone ignored `NOVA_RUN_AUDIO_TESTS` and emitted no report; explicit Release diagnostics rebuilt with `NOVA_ENABLE_RELEASE_DIAGNOSTICS=1` and passed validation.
* Status: PASS

* Blocker: P14B-P1 needs ownership policy, metadata, documented limits, and repeatable generation/comparison commands.
* Action taken: P14B harness is gated to Debug/explicit Release diagnostics, emits build/measurement metadata, documents tracked vs generated artifacts, and has generation/comparison scripts.
* Evidence: final two-run comparison stable SHA256 `7eb4a21bd59b214671a897cdb9013233229dd0545e54c6539a4264cfa4c04d6f`; `artifacts/p14b-compare-final2.log`.
* Status: PASS

* Blocker: PDC/latency lacks enough validation.
* Action taken: added internal latency tests for preset restore, bypass baseline restoration, and sample-rate reprepare; added host smoke procedure in `docs/reports/p13a-pdc-latency-host-smoke-procedure.md`.
* Evidence: Debug validation and explicit Release diagnostic validation both passed `results=307 passes=13447 failures=0 failingResults=0`.
* Status: CONDITIONAL PASS; external DAW/validator smoke still pending.

* Blocker: Project reproducibility is weakened by absolute local JUCE include paths.
* Action taken: documented current JUCE 8.0.12 local path requirement and deferred source-of-truth fix in `docs/reports/nova-build-reproducibility-setup.md`.
* Evidence: local required path remains `C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE\modules`.
* Status: DOCUMENTED / CONDITIONAL

## Notable Implementation Details

* `scripts/build-nova.ps1` now prefers amd64 MSBuild through `vswhere`, sets x64 architecture environment for generated VST3 post-build logic, and passes `PreferredToolArchitecture=x64`.
* Release builds now rebuild by default. This avoids stale `NOVA.lib` and avoids mixing explicit diagnostic Release objects into later production Release packages.
* Release diagnostics are opt-in through `-EnableReleaseDiagnostics`; diagnostic runner scripts pass this flag for Release and use hidden process windows.
* The VST3 helper's JSON5-compatible output is normalized to strict JSON only in the wrapper-generated artifact.
* A Release-only unit-test harness assumption was fixed: routing-parameter tests now drain message-thread work through `AudioEngineTestAccess` after params are updated from a simulated audio thread.
* No intentional DSP, tonal, gain staging, coefficient, bypass, preset-format, or realtime audio-path behavior changes were made.

## Validation Evidence

* Production Release VST3:
  * Command: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3`
  * Result: SharedCode, VST3ManifestHelper, and VST3 all rebuilt with `0 Warning(s)` and `0 Error(s)`.
  * Manifest: `Builds/VisualStudio2022/x64/Release/VST3/NOVA.vst3/Contents/Resources/moduleinfo.json`
  * VST3 binary: `Builds/VisualStudio2022/x64/Release/VST3/NOVA.vst3/Contents/x86_64-win/NOVA.vst3`

* Production Release env-hook negative:
  * Command: normal Release standalone build, then launch with `NOVA_RUN_AUDIO_TESTS=1`.
  * Result: no validation report emitted.

* Explicit Release diagnostics:
  * Command: `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240`
  * Result: `results=307 passes=13447 failures=0 failingResults=0`, `status=PASS`.

* Debug validation:
  * Command: `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240`
  * Result: `results=307 passes=13447 failures=0 failingResults=0`, `status=PASS`.

* P14B determinism:
  * Generate run 1: `scripts/generate-p14b-drive-dynamics-baseline.ps1 -Configuration Debug -Platform x64 -OutputPath artifacts/p14b-p1-drive-dynamics-measurement-baseline-final2.json`
  * Generate run 2: `scripts/generate-p14b-drive-dynamics-baseline.ps1 -Configuration Debug -Platform x64 -OutputPath artifacts/p14b-p1-drive-dynamics-measurement-baseline-final2-run2.json`
  * Compare: `scripts/compare-p14b-drive-dynamics-baseline.ps1 -BaselinePath artifacts/p14b-p1-drive-dynamics-measurement-baseline-final2.json -CurrentPath artifacts/p14b-p1-drive-dynamics-measurement-baseline-final2-run2.json -WriteNormalizedArtifacts`
  * Result: stable SHA256 matched and comparison passed.

## Tool Availability

Checked locally on 2026-07-08:

* `pluginval.exe`: not found
* `validator.exe`: not found
* `vst3validator.exe`: not found
* `reaper.exe` / `REAPER.exe`: not found
* `C:\Program Files\REAPER (x64)\reaper.exe`: not found
* Steinberg validator default install paths: not found

## Files Changed

* `.gitignore`
* `Source/Core/AudioEngine.cpp`
* `Source/Core/AudioEngineTests.cpp`
* `Source/Core/PluginEditor.cpp`
* `Source/Core/PluginProcessor.cpp`
* `scripts/build-nova.ps1`
* `scripts/run-base-audio-validation.ps1`
* `scripts/run-golden-audio-metrics.ps1`
* `scripts/run-rt-profile-scenarios.ps1`
* `scripts/generate-p14b-drive-dynamics-baseline.ps1`
* `scripts/compare-p14b-drive-dynamics-baseline.ps1`
* `docs/reports/nova-release-blocker-fix-report.md`
* `docs/reports/nova-build-reproducibility-setup.md`
* `docs/reports/p13a-pdc-latency-host-smoke-procedure.md`
* `docs/reports/p14b-p1-drive-dynamics-measurement-harness.md`

## Remaining Blockers

* External host-level VST3/PDC smoke: pending a machine with pluginval, Steinberg validator, REAPER, or another DAW.
* JUCE path portability: documented but not structurally fixed; should be a dedicated Projucer/build-system pass.

## Next Recommended Step

Proceed to settings window reconstruction. Before release/user QA, install a VST3 validator or DAW and execute `docs/reports/p13a-pdc-latency-host-smoke-procedure.md`.
