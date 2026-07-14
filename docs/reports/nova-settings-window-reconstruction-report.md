# NOVA Settings Window Reconstruction Report

## Executive Summary

* Status: CONDITIONAL PASS
* Can proceed to final Claude technical audit: YES
* Main blockers:
  * No automated GUI-level test coverage exists for the settings overlay itself (draft/commit/cancel is exercised through the service layer and a manual smoke checklist, not through automated UI driving). Documented, not release-blocking for a technical audit.
  * Host-level VST3/PDC smoke validation remains pending from the prior release-blocker phase (no pluginval / Steinberg validator / DAW on this machine). Unchanged by this phase; still required before user QA/release.

## Prior Release Blocker Context

* Release blocker report reviewed: `docs/reports/nova-release-blocker-fix-report.md` (CONDITIONAL PASS; settings reconstruction explicitly approved as next step).
* Conditions still pending: host-level VST3 scan / PDC smoke (`docs/reports/p13a-pdc-latency-host-smoke-procedure.md`); JUCE include-path portability (documented in `docs/reports/nova-build-reproducibility-setup.md`).
* Host-level VST3/PDC smoke status: STILL PENDING. No validator or DAW installed locally. Does not block this phase; must be executed before user QA/release.

## Phase 0 — Current State Review

* Files reviewed:
  * `docs/reports/nova-release-blocker-fix-report.md`
  * `docs/reports/p14b-p1-drive-dynamics-measurement-harness.md`
  * `Source/Core/PluginEditor.cpp` (settings overlay, `EditorPrefs`, settings pages)
  * `Source/Core/PluginProcessor.h/.cpp` (startup restore, session clear paths, diagnostic gating)
  * `Source/Core/SessionPersistence.h` (preset I/O, startup-preset pointer)
  * `Source/Core/NovaSettingsService.h`
  * `Source/Core/AudioEngineTests.cpp` (validation coverage)
* Existing settings behavior (before reconstruction):
  * All editor-preference persistence lived inside an anonymous-namespace `EditorPrefs` block in `PluginEditor.cpp`: private XML cache, immediate flush-to-disk on every setter, no schema version, no sanitization of stored values on load.
  * The settings overlay had six categories; Controllers, Profile and Cloud were `PlaceholderSettingsPage` instances describing unshipped features (fake-functional UI).
  * The overlay had a single CLOSE button; every control change persisted immediately — there was no Cancel, no Apply, no Restore Defaults.
* Existing persistence paths:
  * `%APPDATA%\NOVA\editor-settings.xml` — editor preferences (XML, `NOVA_EDITOR_SETTINGS` root, `SETTING name/value` children).
  * `%APPDATA%\NOVA\startup-preset.txt` — plain-text absolute path to the startup preset file.
  * Preset files themselves are unchanged by this phase.
* Existing startup preset behavior (the audit concern):
  * `SessionPersistence::savePresetToFile()` and `loadPresetFromFile()` both silently overwrote `startup-preset.txt` on every save/load.
  * `NOVAAudioProcessor` constructor unconditionally called `restoreStartupPresetIfAvailable()` regardless of the editor `startupMode` preference — `startupMode` in the UI was misleading.
  * Invalid host state in `setStateInformation()` called `clearSessionAndForgetStartupPreset()`, deleting the user's startup pointer on a host-state hiccup.
  * The editor applied `startupPointer` fallback even when `startupMode` was "Remember last context".
* Current risks identified:
  * Duplicated startup-pointer path logic in three places (editor, persistence, tests).
  * Unversioned, unsanitized settings file; corrupt XML silently produced an empty cache.
  * Placeholder pages presenting unshipped functionality.
  * Settings mutation coupled directly to disk I/O inside UI callbacks with no transactional boundary.

## Phase 1 — Settings Architecture Design

* Architecture summary: extract a header-only, message-thread-only service (`Nova::SettingsService`) owning keys, defaults, validation, XML schema, file locations, and startup-pointer helpers. Keep `EditorPrefs` in `PluginEditor.cpp` as a thin adapter over committed settings, while every `SettingsOverlay` owns an independent draft passed by reference to its pages. Incremental approach chosen over full rewrite: overlay structure, look-and-feel and page classes are preserved; only persistence, transaction semantics, startup ownership, and placeholder removal changed.
* Local editor preferences (service-owned, `editor-settings.xml`): show performance stats, open browser/presets on startup, startup mode, confirm-before-clear, tuner reference, latency tips, switcher shortcut key, and switcher mode mask. Legacy library-view/favorites keys remain accepted and round-tripped for compatibility, but are not exposed because no implemented preset-browser behavior consumes them.
* Host/plugin parameters: untouched. The settings UI reaches audio behavior only through existing engine APIs (`setTunerReferencePitch`, `setSwitcherMode`) on the message thread; no new parameters, no hidden parallel DSP state, no parameter mutation from settings persistence.
* Standalone-only settings: `AudioDeviceSelectorComponent` is created only under `JucePlugin_Build_Standalone` **and** a live `StandalonePluginHolder`. In plugin mode the Device tab shows an explicit read-only notice: "Audio device setup is controlled by the host in plugin formats." No fake-editable device UI.
* Startup preset ownership: the pointer file is written from exactly one code path — the explicit "set startup preset" action in the settings overlay (`PluginEditor.cpp`, via `SettingsService::writeStartupPresetPointer`). Clearing it is likewise explicit. Preset save/load never touches it.
* Diagnostics/measurement handling: no diagnostics controls were added to the settings UI. All diagnostic/measurement env hooks remain compile-time gated (`NOVA_DIAGNOSTIC_ENV_HOOKS_ENABLED` = Debug or explicit `NOVA_ENABLE_RELEASE_DIAGNOSTICS=1`); session logging additionally requires `NOVA_SESSION_LOGGING_ENABLED`. Verified by negative check (below).
* Placeholder/unfinished pages decision: Controllers, Profile and Cloud placeholder pages REMOVED for RC (buttons, page instances, categories all deleted). Remaining categories: General, Audio, Library, plus wizard launchers.
* Compatibility strategy: same file names, same locations, same XML root/child tags and key names as the legacy format. `schemaVersion` attribute added (v1); files without it load normally and are stamped on next save. Unknown keys are preserved through round-trip. `startup-preset.txt` format unchanged.

## Phase 2 — Settings Service / Model Layer

* Files added: `Source/Core/NovaSettingsService.h` (header-only; registered in `NOVA.jucer`).
* Files changed: `Source/Core/PluginEditor.cpp` (EditorPrefs now delegates to the service), `Source/Core/SessionPersistence.h` (uses service pointer helpers; local duplicates removed), `Source/Core/PluginProcessor.cpp` (reads `startupMode` through the service).
* Defaults: centralized in `makeDefaultEditorSettings()` — stats on, browser/presets-on-startup off, startup mode "Remember last context", confirm-before-clear on, A = 440 Hz, latency tips on, library "All presets", favorites-first off, switcher key = space, switcher modes = 7 (all three).
* Validation: `sanitize()` normalizes every known key — bools re-normalized or defaulted, enumerated strings (startup mode, tuner reference, library view) whitelisted, switcher key clamped to positive, switcher mode mask masked to 3 bits and forced to ≥2 enabled modes. Unknown keys pass through untouched.
* XML schema/versioning: root `NOVA_EDITOR_SETTINGS`, `schemaVersion="1"`, `SETTING name/value` children (unchanged element shape from legacy files).
* Migration/compatibility: legacy files (no schemaVersion) load and sanitize cleanly; nothing is rejected for missing version. No migration step needed.
* Corrupt/missing file handling: missing file → pure defaults (`loadedFromDisk=false`); unparseable XML or wrong root → defaults with `usedFallback=true`; both covered by unit test.
* RT-safety notes: the service is file/XML/string code with zero audio-thread involvement. All call sites are the processor constructor, editor constructor and UI callbacks (message thread). No locks, allocations or I/O were added to `processBlock` paths. The one incidental audio-path-adjacent change (`AudioEngine.cpp`, `PluginEditor.cpp` topology callbacks) only wraps existing debug asserts in `#if JUCE_DEBUG`, removing a needless Release-mode `MessageManager` query — no behavioral change.

## Phase 3 — Settings Window UI

* Files changed: `Source/Core/PluginEditor.cpp` (SettingsOverlay and pages).
* Sections implemented: left rail with GENERAL, AUDIO (Session / Defaults / Device tabs), LIBRARY, plus wizard launchers (Audio Setup, Start, Preset Finder). Bottom action row: RESTORE DEFAULTS, APPLY / SAVE; top-right CANCEL (also ESC).
* Apply/Save behavior: each overlay opens its own copy of committed settings (`makeDraft`); all page controls write into that instance-local draft; APPLY / SAVE sanitizes and persists before publishing the draft. A failed file write leaves committed state unchanged, keeps the overlay open and shows an explicit error. On success, `onSettingsApplied` re-reads preferences into the live editor (stats visibility, startup toggles, tuner reference pitch, switcher shortcut/mode mask with automatic fallback if the current mode was disabled).
* Cancel behavior: CANCEL button, outside click, wizard launch, or ESC destroys the overlay and its local draft without commit; committed settings and disk are untouched.
* Reset defaults behavior: RESTORE DEFAULTS shows a confirmation `AlertWindow` (explicitly scoped to "local editor preferences — audio parameters, presets and sessions are not changed"), then persists defaults before replacing committed state. A failed write is reported and leaves existing settings unchanged.
* Plugin vs standalone behavior: Device tab embeds the JUCE `AudioDeviceSelectorComponent` only in standalone; in plugin builds it displays host-controlled notices with no editable controls. Session tab shows a live read-only device summary in standalone and a host-controlled message in plugin mode.
* Disabled/removed placeholders: Controllers & MIDI, Player Profile, Cloud & Account pages fully removed (buttons, categories, page objects, layout slots). Overlay subtitle updated to describe only real functionality.
* Functional limitations:
  * Explicit startup-preset assignment (`Use Current on Start`) and pointer clearing are immediate explicit actions; they update committed startup ownership and the current overlay draft together so a subsequent Apply cannot undo the explicit action accidentally.
  * Standalone device-manager changes are owned and persisted by JUCE's standalone holder and are therefore immediate rather than part of the editor-preference transaction.

## Phase 4 — Startup Preset Behavior

* Files changed: `Source/Core/SessionPersistence.h`, `Source/Core/PluginProcessor.h/.cpp`, `Source/Core/PluginEditor.cpp`.
* Final startup behavior:
  * `startupMode = "Remember last context"` (default): processor performs NO startup-preset restore; host state (plugin) or standalone session restore governs. Editor does not apply the startup pointer.
  * `startupMode = "Open clean rig"`: editor starts on "No Preset"; no pointer restore.
  * `startupMode = "Open startup preset"`: processor restores from `startup-preset.txt` at construction; editor reflects the preset name. Invalid/missing pointer degrades silently to clean state and deletes the dangling pointer.
  * The General page disables "Open startup preset" until a valid preset is explicitly assigned and repairs an impossible draft (`Open startup preset` with no valid pointer) to "Remember last context".
* Explicit user actions (only pointer-mutation paths in the product):
  * Settings → set startup preset: writes the pointer and sets `startupMode` to "Open startup preset".
  * Settings → clear startup preset: deletes the pointer and returns `startupMode` to "Remember last context".
* Compatibility with startup-preset.txt: same file, same location, same plain-path format. Existing pointers keep working when the user selects "Open startup preset".
* Cases validated:
  * Preset SAVE no longer writes the pointer (removed from `SessionPersistence::savePresetToFile`; unit-tested with a sentinel pointer).
  * Preset LOAD no longer writes the pointer (removed from `loadPresetFromFile`; unit-tested).
  * Existing pointer restores; missing pointer returns false safely; invalid pointer is deleted and restore degrades cleanly (unit-tested).
  * Invalid host state in `setStateInformation()` now calls `clearSession()` (new method) instead of `clearSessionAndForgetStartupPreset()` — a corrupt host chunk no longer destroys the user's startup preference. Explicit "clear session and forget startup preset" remains available for the user-facing clear action that intends it.
* Risks: behavior change — users who relied on "last saved/loaded preset silently becomes startup preset" will no longer get that; this was exactly the audited defect and the new behavior is the explicit, documented one.

## Phase 5 — Tests and Validation

* Tests added/updated (in `Source/Core/AudioEngineTests.cpp`, run by the NOVA suite):
  * "NovaSettingsService editor settings round-trip and sanitize safely" — missing file → defaults (no corrupt flag); save/load round-trip; invalid bool/startup-mode/tuner/library/shortcut/mode-mask sanitization; unknown-key preservation; corrupt XML → safe fallback; invalid write target → explicit failure.
  * "NovaSettingsService startup preset pointer helpers are explicit and bounded" — explicit write to existing preset succeeds; resolution; invalid pointer fails resolution and is removed when requested.
  * "Editor settings drafts remain isolated until explicitly committed" — two simultaneous drafts mutate independently, committed state is unchanged before commit, and explicit commit publishes only the selected draft.
  * "SessionPersistence saves and restores a canonical preset" — extended with a sentinel startup pointer proving save and load do NOT mutate it, plus explicit assignment + startup restore preserving the pointer.
* Commands run:
  * `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240`
  * `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3`
  * `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin`
  * Production Release standalone launched with audio-test, P14B, offline-QA, RT-profile and P9D diagnostic environment hooks forced on, with dedicated output paths.
  * P14B Debug measurement generated to a temporary file, compared with the tracked final2 baseline, then deleted.
* Debug validation result: `results=310 passes=13477 failures=0 failingResults=0`, `status=PASS`. (Prior blocker-fix baseline was 307 results / 13447 passes; the +3 results are the settings service, pointer, and independent-draft tests.)
* Release build result: production VST3 — SharedCode, VST3ManifestHelper, VST3 all `Rebuild`, `0 Warning(s)` / `0 Error(s)`, fresh `moduleinfo.json` regenerated and verified by the build script. Production standalone — `0 Warning(s)` / `0 Error(s)`.
* Settings-specific validation: covered by the three tests above inside the passing suite.
* Diagnostic gating validation: production Release standalone launched with all diagnostic hooks forced → zero audio/P14B/QA/RT/P9D artifacts and no session-log timestamp change. Env hooks, P14B writer and session logging remain dead in production Release.
* P14B stability: baseline/current stable SHA256 both `7eb4a21bd59b214671a897cdb9013233229dd0545e54c6539a4264cfa4c04d6f`; comparison PASS.
* Manual GUI smoke checklist (GUI automation unavailable; execute in Standalone Debug before user QA):
  1. Open settings (gear) — overlay appears, GENERAL selected.
  2. Toggle "show performance stats", press CANCEL, reopen — toggle unchanged.
  3. Toggle it again, press APPLY / SAVE, reopen — change persisted; stats HUD visibility matches.
  4. Change tuner reference to 442, APPLY — tuner overlay uses 442 Hz.
  5. RESTORE DEFAULTS → confirm — settings back to defaults, overlay closes.
  6. Without a startup preset, verify General disables "Open startup preset". Set one in Library — `startup-preset.txt` is created and General immediately shows "Open startup preset"; restart app — preset loads.
  7. Clear startup preset — pointer file deleted, mode back to "Remember last context".
  8. Save and load presets — verify `startup-preset.txt` timestamp/content unchanged.
  9. AUDIO → Device tab in standalone — selector edits device; confirm audio continues.
  10. ESC closes overlay from any page; no stuck scrim.
  11. Verify rail shows only GENERAL / AUDIO / LIBRARY + wizards (no Controllers/Profile/Cloud and no inactive Favorites/Recent controls).
* Failures: none. No skipped failing checks.

## Audio/DSP Safety Statement

* Intentional DSP changes: NO
* Details if any: none. Tuner reference pitch application moved from per-keystroke to on-apply (same engine API, message thread); switcher-mode fallback uses the existing `setSwitcherMode` path. No coefficient, gain, routing, oversampling, latency or bypass changes.
* Preset/session compatibility: preset file format, plugin state chunk format, `editor-settings.xml` shape and `startup-preset.txt` format all unchanged. Existing presets, sessions and settings files load. Schema version attribute added additively.
* RT-safety assessment: all new code is message-thread file/string/UI work. No file I/O, locks, allocations or UI calls added to the audio thread. Two Release-mode debug asserts were compiled out (`#if JUCE_DEBUG`) on message-thread callbacks — no audio-thread impact.

## Final Build/Test Evidence

* Commands:
  * Debug validation: `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240`
  * Release VST3: `scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3`
  * Release standalone: `scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin`
  * Gating negative: production Release `NOVA.exe` with `NOVA_RUN_AUDIO_TESTS=1` + report-path env vars.
  * P14B compare: `generate-p14b-drive-dynamics-baseline.ps1` to a temporary file, then `compare-p14b-drive-dynamics-baseline.ps1` against `artifacts/p14b-p1-drive-dynamics-measurement-baseline-final2.json`.
* Results:
  * Debug: `results=310 passes=13477 failures=0 failingResults=0`, PASS, all failure groups zero.
  * Release VST3: build succeeded, manifest regenerated (`Regenerated VST3 moduleinfo.json`), helper reported "Successfully generated a manifest for NOVA".
  * Release standalone: build succeeded.
  * Gating negative: zero audio/P14B/QA/RT/P9D outputs and no session-log change.
  * P14B stable comparison: PASS; baseline/current stable SHA256 `7eb4a21bd59b214671a897cdb9013233229dd0545e54c6539a4264cfa4c04d6f`.
* Warnings: 0 across all builds.
* Errors: 0 across all builds.

## Final Git Diff Summary

Working tree relative to `17b4a3b` (includes the already-reported release-blocker changes plus this settings phase):

* Files changed:
  * `Source/Core/PluginEditor.cpp` — EditorPrefs → service delegation, independent per-overlay draft/commit/cancel, Apply/Cancel/Restore Defaults, placeholder and inactive-library-control removal, startup-mode-gated editor startup, explicit startup-preset actions.
  * `Source/Core/PluginProcessor.h/.cpp` — `clearSession()` added; startup restore gated by `startupMode`; diagnostic/session-logging gating (blocker phase).
  * `Source/Core/SessionPersistence.h` — pointer writes removed from save/load; delegates pointer helpers to service.
  * `Source/Core/AudioEngineTests.cpp` — settings-service and startup-pointer tests added (plus blocker-phase P13A/P14B tests).
  * `NOVA.jucer` — registered `NovaSettingsService.h`.
  * `Source/Core/AudioEngine.cpp`, `.gitignore`, `audio-base-test-report.txt`, `scripts/*.ps1` — blocker-phase changes, unchanged here.
* Files added: `Source/Core/NovaSettingsService.h`; this report.
* Files removed: none.
* Untracked artifacts: prior-phase reports (`docs/reports/nova-release-blocker-fix-report.md`, `nova-build-reproducibility-setup.md`, `p13a-pdc-latency-host-smoke-procedure.md`, `p14b-p1-drive-dynamics-measurement-harness.md`) and P14B scripts (`scripts/generate-/compare-p14b-drive-dynamics-baseline.ps1`) — pending commit alongside this work.

## Remaining Blockers

* Blocker: No automated GUI-driving test of the settings overlay (draft semantics covered at service level + manual checklist only).
  * Severity: LOW
  * Required fix: execute the manual GUI smoke checklist during engineering QA; optionally add a JUCE UI test harness later.
  * Blocks final technical audit: NO
  * Blocks user QA: NO (checklist must be executed as part of user-QA prep)

* Blocker: Host-level VST3/PDC smoke validation pending (carried from release-blocker phase).
  * Severity: MEDIUM
  * Required fix: install pluginval / Steinberg validator / a DAW and execute `docs/reports/p13a-pdc-latency-host-smoke-procedure.md`.
  * Blocks final technical audit: NO
  * Blocks user QA: YES

## Next Recommended Step

Ready for final Claude technical audit. Before user QA/release: execute the manual GUI smoke checklist and the pending host-level VST3/PDC smoke procedure.
