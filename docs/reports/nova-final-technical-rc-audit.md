# NOVA Final Technical RC Audit

Audit date: 2026-07-13. Auditor: Claude (read-only senior technical audit). Scope: full working tree relative to commit `17b4a3b` after the release-blocker-fix phase and the settings-window-reconstruction phase.

## Final Verdict

* Status: GO TO FINAL PRE-USER-QA VALIDATION
* Can proceed to user QA now: NO
* Reason: All code-level audit areas pass on evidence. Scope was controlled, Release build integrity is intact and re-verified on disk today (fresh binary, fresh valid moduleinfo.json), P14B stable SHA256 was independently recomputed by this audit and matches (`7eb4a21bd59b214671a897cdb9013233229dd0545e54c6539a4264cfa4c04d6f`), the settings architecture is a real service/draft/commit design with safe failure modes, startup-preset ownership is now explicit and unit-tested, no DSP/tonal changes were found in the diff, and no realtime-safety regressions were introduced. The only remaining items are the two known manual/tooling validations (host-level VST3/PDC smoke and the manual settings GUI smoke checklist), which by rule block user QA but do not block the final pre-user-QA validation pass.
* Main blockers:
  * Host-level VST3/PDC validation (pluginval / Steinberg validator / DAW) has never been executed — blocks user QA.
  * Manual engineering smoke of the reconstructed settings window has not been executed — blocks user QA.

## Scope Control

* New features outside settings reconstruction: NO
* Broad unrelated refactors: NO
* DSP/tonal changes detected: NO
* Evidence:
  * `git diff --stat` covers 13 modified files + 8 untracked files; every change maps to either the blocker-fix phase (build scripts, diagnostic gating, P14B harness, .gitignore) or the settings phase (NovaSettingsService.h, PluginEditor.cpp, PluginProcessor.h/.cpp, SessionPersistence.h, NOVA.jucer registration, tests).
  * `Source/Core/AudioEngine.cpp` diff is exactly 2 lines: wrapping an existing message-thread `jassert` (and its `MessageManager` query) in `#if JUCE_DEBUG` inside `notifyTopologyChangedOnMessageThread()`. Same pattern in `PluginEditor.cpp::closeStalePedalEditorOverlay()`. No audio-path code touched.
  * No effect, amp, cabinet, InputChain/OutputChain/ChannelStrip, SyntheticIR, PluginStateModel, SessionStore, or parameter-definition files were modified.
  * UI removals (Controllers/Profile/Cloud placeholder pages; inactive Library "Favorites first"/"Recent first" controls) remove non-functional surface only; the legacy `editor.libraryView`/`editor.libraryFavoritesFirst` keys still round-trip in the service for compatibility.
* Concerns:
  * The `#if JUCE_DEBUG` assert-gating in AudioEngine/PluginEditor is technically outside the settings scope, but it is 4 lines, removes only a Release-mode assert query on the message thread, and is documented in the settings report. Accepted.
  * Intentional, documented behavior change: preset save/load no longer silently promotes the preset to startup preset, and an existing `startup-preset.txt` is only honored when `startupMode` is "Open startup preset". This is the audited defect being fixed, not scope creep, but it is user-visible (see Startup Preset audit).

## Repository State

* Branch: `master`, ahead of `origin/master` by 5 commits (nothing from these two phases is committed yet).
* Git status: 13 modified tracked files, 8 untracked files, nothing staged. `git diff --check`: clean (no whitespace/conflict-marker issues).
* Modified files: `.gitignore`, `NOVA.jucer`, `Source/Core/AudioEngine.cpp`, `Source/Core/AudioEngineTests.cpp`, `Source/Core/PluginEditor.cpp`, `Source/Core/PluginProcessor.cpp`, `Source/Core/PluginProcessor.h`, `Source/Core/SessionPersistence.h`, `audio-base-test-report.txt`, `scripts/build-nova.ps1`, `scripts/run-base-audio-validation.ps1`, `scripts/run-golden-audio-metrics.ps1`, `scripts/run-rt-profile-scenarios.ps1`.
* Untracked files: `Source/Core/NovaSettingsService.h` (new production code — must be committed), 4 phase reports under `docs/reports/`, and `scripts/generate-p14b-drive-dynamics-baseline.ps1` + `scripts/compare-p14b-drive-dynamics-baseline.ps1` (must be committed).
* Generated artifacts: `artifacts/` contains blocker-phase build/validation logs and P14B JSON outputs, all now covered by the new `.gitignore` rules (`/artifacts/*.log`, `/artifacts/p14b-*.json`, `/artifacts/p14b-*.txt`, `/artifacts/release-vst3-*.json`) and therefore correctly excluded from version control. Historical tracked artifacts (audits, rt-profile reports) predate this work and are unchanged.
* Concerns:
  * The entire two-phase change set is uncommitted working-tree state. Before final pre-user-QA validation it should be committed so the validated state is pinned to a SHA. LF→CRLF conversion warnings on the modified files are a line-ending hygiene note only.

## Release Build Integrity

* Status: PASS
* Build flow reviewed: `scripts/build-nova.ps1`. Direct `NOVA_VST3` builds now run `NOVA_SharedCode` → `NOVA_VST3ManifestHelper` → `NOVA_VST3` in order; direct standalone builds run `NOVA_SharedCode` first; Release configuration always uses MSBuild `Rebuild` (also when `-EnableReleaseDiagnostics` is set), which prevents both stale SharedCode reuse and diagnostic-object contamination of production packages. MSBuild is resolved vswhere-first (amd64 preferred), with `PreferredToolArchitecture=x64`.
* moduleinfo.json status: FRESH AND VALID, re-verified by this audit on disk today: VST3 binary `2026-07-13T20:19:23.73Z`, moduleinfo.json `2026-07-13T20:19:24.07Z` (0.34 s after the binary), parses as strict JSON with Name=NOVA Version=1.0.0. The script normalizes the helper's JSON5 trailing commas, validates parseability, and hard-fails if the manifest is older than the binary.
* SharedCode stale-link protection: intact (ordered target list + forced Rebuild for Release).
* Release diagnostics gating: `NOVA_ENABLE_RELEASE_DIAGNOSTICS` is NOT defined anywhere in `NOVA.jucer`, `.vcxproj`, or props files (verified by search) — it is injected only via the `CL` environment variable when `-EnableReleaseDiagnostics` is explicitly passed, and the env var is restored afterwards. Production Release builds therefore compile all diagnostic hooks out.
* Reproducibility risks: the absolute local JUCE 8.0.12 path (`C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE\modules`) remains a machine-local assumption, documented in `docs/reports/nova-build-reproducibility-setup.md` with an explicit deferred fix. This blocks reproducible builds on other machines but not this RC gate.
* Evidence: build-nova.ps1 diff; on-disk timestamps of Release VST3, moduleinfo.json (today), Release Standalone `NOVA.exe` (today 14:20); grep for the diagnostics define across project files.
* Required fixes: none for this gate. JUCE path portability remains a dedicated future build-system pass.

## P14B Measurement Integrity

* Status: PASS
* Stable SHA: `7eb4a21bd59b214671a897cdb9013233229dd0545e54c6539a4264cfa4c04d6f` — independently recomputed by this audit today by re-running `scripts/compare-p14b-drive-dynamics-baseline.ps1` against `artifacts/p14b-p1-drive-dynamics-measurement-baseline-final2.json` and `...-final2-run2.json`; baseline and current hashes match and the comparison passes.
* Baseline/golden status: baseline JSONs are intentionally untracked local artifacts per the documented ownership policy in `docs/reports/p14b-p1-drive-dynamics-measurement-harness.md`; the new `.gitignore` rules enforce this. Note: the settings reconstruction report once calls the final2 baseline "tracked" — that wording is wrong; the documented and enforced policy is untracked/generated. Minor doc inconsistency only.
* Metadata/artifact policy: generated reports embed commit/branch/configuration/platform/compiler/OS/timestamp metadata; the compare script masks exactly those volatile fields (`generatedAt`, `commitHash`, `branch`, `buildConfiguration`, `buildPlatform`, `compiler`, `osName`) and hashes the remainder — measurement sections must match exactly. Deterministic and correctly scoped.
* Production Release inertness: the harness is compiled only under `NOVA_P14B_MEASUREMENT_HARNESS_ENABLED` (`JUCE_DEBUG || NOVA_ENABLE_RELEASE_DIAGNOSTICS`) in `AudioEngineTests.cpp`; the report-writer hook in `PluginProcessor.cpp` (`maybeWriteP14BMeasurementReportOnce`) sits inside `NOVA_DIAGNOSTIC_ENV_HOOKS_ENABLED`; the define is absent from project files. The blocker-phase and settings-phase negative checks (production Release launched with all diagnostic env vars forced → zero artifacts) corroborate.
* Evidence: fresh comparison run output captured in this audit; `artifacts/p14b-compare-final2.log`; compare/generate script review; gating review in source.
* Required fixes: none. Optional: fix the "tracked baseline" wording in the settings report when it is committed.

## Settings Architecture Audit

* Status: PASS
* Files reviewed: `Source/Core/NovaSettingsService.h` (full read), `Source/Core/PluginEditor.cpp` (full diff), `Source/Core/SessionPersistence.h` (full diff), `Source/Core/PluginProcessor.h/.cpp` (full diff), settings tests in `Source/Core/AudioEngineTests.cpp`.
* Service/model separation: real. `Nova::SettingsService` (header-only, message-thread) owns keys, defaults, validation, XML schema, file locations, and startup-pointer helpers. `EditorPrefs` in PluginEditor.cpp is a thin adapter over a per-process committed-settings singleton. Pages edit an overlay-local draft passed by reference. SessionPersistence and PluginProcessor consume the service; duplicated pointer-path logic was removed from three places.
* Persistence design: transactional. `commitDraft()` sanitizes, writes to disk first, and only replaces committed state if the write succeeded. A failed write shows an explicit async AlertWindow ("changes remain open and have not been committed") and keeps the overlay open. Confirmed by code and by the "invalid write target → explicit failure" unit test.
* Defaults/validation: centralized in `makeDefaultEditorSettings()` and `sanitize()` — bools re-normalized or defaulted, enumerated strings whitelisted (startup mode, tuner reference, library view), switcher key clamped positive, mode mask masked to 3 bits with a ≥2-modes floor. Unknown keys pass through untouched (unit-tested with `user.displayName`).
* XML schema/versioning: same root (`NOVA_EDITOR_SETTINGS`) and child shape (`SETTING name/value`) as legacy files; additive `schemaVersion="1"` attribute; files without the attribute load normally and are stamped on next save. Safe.
* Multi-instance draft behavior: each `SettingsOverlay` owns an independent `draftSettings` copy; unit test proves two drafts mutate independently and committed state is untouched until an explicit commit. Committed state is a per-process static touched only on the JUCE message thread — safe within a host process. Cross-process (two DAWs) is last-write-wins on the XML file, which is standard settings-file behavior, not a defect.
* Apply/Cancel/Save behavior: APPLY / SAVE = sanitize → persist → publish → `onSettingsApplied` re-reads live editor state (stats HUD, startup toggles, tuner pitch via existing `setTunerReferencePitch`, switcher shortcut/mask with fallback if the current mode was disabled). CANCEL / ESC / outside dismissal destroys the overlay and draft with no persistence.
* Reset defaults behavior: confirmation AlertWindow explicitly scoped to local editor preferences; persists defaults to disk before replacing committed state; failed write reported and existing settings left unchanged. Note: restoring defaults resets `startupMode` to "Remember last context" but does not delete `startup-preset.txt` — the pointer survives and can be re-enabled; consistent and predictable.
* Placeholder page/control handling: `PlaceholderSettingsPage` class and Controllers/Profile/Cloud pages, buttons, categories, and layout slots fully deleted; inactive Library view/favorites controls removed; overlay subtitle rewritten to describe only real functionality.
* Evidence: full diffs and service source cited above; tests "NovaSettingsService editor settings round-trip and sanitize safely", "…startup preset pointer helpers are explicit and bounded", "Editor settings drafts remain isolated until explicitly committed" — all present in source and included in the passing 310-result suite. Settings tests write only to the temp directory, not the user's real `%APPDATA%\NOVA` settings file.
* Required fixes: none.

## Startup Preset Behavior Audit

* Status: PASS
* startup-preset.txt compatibility: same file, same location, same plain-path format; `SettingsService::readStartupPresetPointer/writeStartupPresetPointer/clearStartupPresetPointer` reproduce legacy semantics including dangling-pointer deletion.
* Explicit user action behavior: the pointer is written from exactly one product path — the Library page "Use Current on Start" action (`SettingsService::writeStartupPresetPointer` + persist `startupMode=Open startup preset`); clearing is likewise explicit and returns the mode to "Remember last context". Both actions also update the open overlay's draft so a subsequent Apply cannot undo them, and the General page is refreshed via `onStartupAssignmentChanged`.
* Preset save/load mutation risk: ELIMINATED. `SessionPersistence::savePresetToFile()` and `loadPresetFromFile()` no longer touch the pointer (verified in diff); unit test writes a sentinel to the pointer file and asserts it is byte-identical after save and after load (guarded by `StartupPresetPointerGuard`, which backs up and restores the user's real pointer file).
* Invalid/missing startup preset handling: missing pointer → `restoreStartupPresetIfAvailable()` returns false; dangling pointer → deleted and restore degrades to clean state; failed restore also clears the pointer. The General page disables "Open startup preset" when no valid pointer exists and repairs an impossible draft to "Remember last context". Invalid host state in `setStateInformation()` now calls the new `clearSession()` instead of `clearSessionAndForgetStartupPreset()` — a corrupt host chunk no longer destroys the user's startup preference. The editor's "clear session" button also uses `clearSession()`.
* startupMode truthfulness: NOW TRUTHFUL. The processor constructor restores the startup preset only when `startupMode == "Open startup preset"` (read through the service); the editor applies the pointer label only in that mode; "Open clean rig" starts on "No Preset"; "Remember last context" defers to host/standalone state.
* Evidence: SessionPersistence.h diff, PluginProcessor.cpp diff (constructor + setStateInformation), PluginEditor.cpp diff (constructor gating + explicit actions), sentinel unit test in AudioEngineTests.cpp.
* Required fixes: none. Behavioral note for release notes: users upgrading with an existing `startup-preset.txt` but default `startupMode` will no longer get automatic startup-preset loading until they select "Open startup preset" — this is the documented, intended fix of the audited defect, but it should be stated in user-facing changelog material.

## Standalone vs Plugin Mode Audit

* Status: PASS
* Plugin mode behavior: the Device tab shows an explicit read-only host-controlled notice; no fake-editable device UI. Session tab shows a host-controlled message.
* Standalone device settings behavior: `AudioDeviceSelectorComponent` is created only under `JucePlugin_Build_Standalone` with a live `StandalonePluginHolder`; device changes are owned/persisted by JUCE's standalone holder and are immediate rather than part of the editor-preference transaction — documented as a functional limitation in the settings report, which is the honest description of JUCE's ownership model.
* Host-controlled settings clarity: plugin parameters are untouched by the settings system; the settings UI reaches audio behavior only through existing message-thread engine APIs (`setTunerReferencePitch`, `setSwitcherMode`).
* Evidence: PluginEditor.cpp diff and settings report Phase 3; no new parameter definitions in the diff.
* Required fixes: none. The immediate (non-transactional) nature of standalone device changes should be confirmed as acceptable UX during the manual settings smoke.

## RT-Safety Audit

* Status: PASS
* Audio-thread file I/O risk: NONE ADDED. All `Nova::SettingsService` call sites are the processor constructor, editor constructor, and UI callbacks (verified by grepping every `SettingsService::` usage: AudioEngineTests.cpp, PluginEditor.cpp, SessionPersistence.h, PluginProcessor.cpp — none in `processBlock` paths).
* Audio-thread locks risk: none added; the two-plane engine architecture is untouched.
* Audio-thread allocation risk: none added.
* UI/audio thread coupling risk: settings apply publishes through existing message-thread APIs only. The removed Release-mode asserts in topology callbacks reduce (not add) message-thread work.
* Parameter mutation safety: host parameters unchanged; tuner reference and switcher mode go through pre-existing engine setters on the message thread; no parameter mutation from settings persistence.
* Hidden DSP state risk: none found; no parallel DSP state introduced.
* SessionLogger note: some `SessionLogger::logEvent` call sites remain compiled in production Release (e.g. in `setStateInformation`), but the logger is runtime-inert unless `attachOwner()` runs, and `attachOwner()` is compiled out in production Release — calls early-return on `sessionActive=false` and no log file is created. Corroborated by the negative gating checks. The only residual cost is transient string construction on message-thread error paths; negligible.
* Evidence: call-site grep, AudioEngine.cpp diff, SessionLogger.h review, PluginProcessor.cpp gating diff.
* Required fixes: none.

## Compatibility Audit

* Status: PASS
* Existing presets: preset file format and Base64 pedal-state encoding untouched; save/load round-trip covered by the extended canonical-preset test in the passing suite.
* Existing sessions: plugin state chunk format untouched; `setStateInformation`'s failure path became strictly safer (preserves startup pointer).
* Existing editor-settings.xml: same root/child tags and key names; legacy files without `schemaVersion` load normally and are stamped on next save; corrupt XML → safe defaults with `usedFallback=true`; unknown keys preserved (all unit-tested).
* Existing startup-preset.txt: same format and location; honored when the user selects "Open startup preset"; dangling pointers cleaned exactly as before.
* Migration behavior: additive only; no rejection or destructive migration paths exist.
* Evidence: NovaSettingsService.h source; settings round-trip/corruption unit tests; SessionPersistence diff.
* Required fixes: none beyond the startup-mode changelog note above.

## Test Evidence

* Commands reviewed: `scripts/run-base-audio-validation.ps1` (Debug), `scripts/build-nova.ps1 -Configuration Release -Target NOVA_VST3` and `-Target NOVA_StandalonePlugin`, production-Release env-hook negative launch, P14B generate/compare scripts.
* Debug validation: `audio-base-test-report.txt` = `results=310 passes=13477 failures=0 failingResults=0`, `status=PASS`; file written today 2026-07-13 14:18, matching the settings report and consistent with +3 results over the blocker-phase 307.
* Release VST3 build: binary and moduleinfo.json rebuilt today (14:19 local / 20:19 UTC), moduleinfo fresh and valid (re-verified by this audit). Blocker/settings reports record 0 warnings / 0 errors; the corresponding logs exist in `artifacts/` (now gitignored).
* Release Standalone build: `NOVA.exe` rebuilt today 14:20; reports record 0 warnings / 0 errors.
* Settings-specific tests: three new service-level tests (round-trip/sanitize, pointer helpers, draft isolation) plus the sentinel-pointer extension of the canonical-preset test — all present in source and in the passing suite. Tests use the temp directory; the one test touching the real pointer file backs it up and restores it via RAII guard.
* Diagnostic gating checks: production Release negative checks (all diagnostic env hooks forced → zero artifacts, no session-log write) recorded in both phase reports; compile-out verified in source and project files by this audit.
* P14B checks: stable SHA comparison re-executed in this audit — PASS, hash matches the recorded value.
* git diff --check: clean.
* Missing tests: no automated GUI-driving test of the settings overlay (draft semantics are covered at service level only); no automated host-level VST3 test.
* Concerns: this audit did not itself re-run the unit suite or rebuild (read-only mandate); it relied on today's on-disk evidence files plus the independently re-executed P14B comparison. The evidence chain is fresh and internally consistent.

## Pending Before User QA

* Item: Manual settings window engineering smoke test (11-step checklist in the settings reconstruction report, §Phase 5).
  * Severity: MEDIUM
  * Blocks user QA: YES
  * Blocks release: YES
  * Required evidence: executed checklist with per-step pass/fail recorded (Standalone Debug), including Apply/Cancel/Restore Defaults, startup-preset set/clear/restart, and device-tab behavior.

* Item: Host-level VST3/PDC validation (pluginval, Steinberg validator, REAPER, or another DAW) per `docs/reports/p13a-pdc-latency-host-smoke-procedure.md`.
  * Severity: HIGH
  * Blocks user QA: YES
  * Blocks release: YES
  * Required evidence: validator log and/or DAW impulse-alignment results at 48 kHz (and 96 kHz where supported), covering the no-pedal baseline, added-latency compensation, bypass return, and session-reload stability.

* Item: Commit the current working tree (including `Source/Core/NovaSettingsService.h`, the P14B scripts, and the phase reports) so the validated state is pinned to a SHA.
  * Severity: MEDIUM
  * Blocks user QA: YES (user QA must run against a pinned, reproducible state)
  * Blocks release: YES
  * Required evidence: commit(s) on `master` containing the full change set; `git status` clean apart from intentionally ignored artifacts.

* Item: JUCE include-path portability (documented local-machine dependency).
  * Severity: LOW (for user QA on this machine) / MEDIUM (for any other build machine)
  * Blocks user QA: NO
  * Blocks release: NO (documented), but required before any multi-machine build or CI claim.
  * Required evidence: dedicated Projucer/build-system pass per `docs/reports/nova-build-reproducibility-setup.md`.

* Item: Settings compatibility spot-check with a real pre-existing `editor-settings.xml` and `startup-preset.txt` from an older install (fold into the manual settings smoke).
  * Severity: LOW
  * Blocks user QA: NO (unit coverage exists; this is belt-and-braces)
  * Blocks release: NO
  * Required evidence: note in the smoke-test record that legacy files loaded and were stamped with `schemaVersion="1"` on next save.

## Release Risks

* Critical: none identified in code.
* High:
  * NOVA has never been loaded by a real VST3 host or validator. Host-scan, PDC, and lifecycle behavior in-host are unverified. This is the single largest outstanding risk and gates user QA.
* Medium:
  * Reconstructed settings overlay has no automated GUI test and no executed manual smoke yet; a UI-level regression (focus, modal scrim, draft wiring) would not have been caught by the service-level tests.
  * Entire change set is uncommitted; accidental working-tree loss or partial commit would desynchronize the validated evidence from the shipped code.
  * Startup-preset behavior change (no more silent promotion; mode-gated restore) will surprise users who relied on the old implicit behavior if not communicated.
* Low:
  * JUCE absolute path blocks reproduction on other machines (documented).
  * P14B baselines are local-only by documented policy; a lost `artifacts/` directory requires regeneration (SHA anchor is recorded in reports).
  * Settings report's "tracked final2 baseline" wording contradicts the documented untracked policy (doc-only).
  * Sentinel-pointer unit test transiently rewrites the user's real `startup-preset.txt` (RAII-restored; a crash mid-test could leave a sentinel string, which the product would then treat as a dangling pointer and delete safely).

## Required Fixes Before Final Pre-User-QA Validation

None.

## Required Fixes Before User QA

1. Execute the host-level VST3/PDC smoke per `docs/reports/p13a-pdc-latency-host-smoke-procedure.md` (install pluginval / Steinberg validator / a DAW) and record the evidence.
2. Execute the 11-step manual settings window smoke checklist (settings reconstruction report §Phase 5) in Standalone Debug and record per-step results; include a legacy `editor-settings.xml` / `startup-preset.txt` compatibility spot-check.
3. Commit the full working-tree change set (source, scripts, reports, `NOVA.jucer`) so user QA runs against a pinned SHA; re-run `scripts/run-base-audio-validation.ps1` once post-commit to confirm the pinned state still reports 310/13477/0.
4. Add the startup-preset behavior change to user-facing release notes.

## Next Recommended Step

Ready for final pre-user-QA validation prompt.

User QA is NOT approved yet: it remains blocked until the host-level VST3/PDC smoke and the manual settings window smoke are executed and pass.
