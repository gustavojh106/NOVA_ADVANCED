# P11C Settings System Rebuild Audit

Date: 2026-05-19

## Scope

This phase rebuilds NOVA's Settings surface as a modern configuration center. It does not change DSP, audio behavior, parameter IDs, state schema, preset serialization, automation, golden baselines, factory approval, Reaper/P7F status, or OutputChain behavior.

## Current Settings Found

Existing editor preferences live in `Source/Core/PluginEditor.cpp` under `EditorPrefs` and persist to:

- `%APPDATA%/NOVA/editor-settings.xml`

Existing editor preference keys:

- `editor.showPerformanceStats`
- `editor.openBrowserOnStartup`
- `editor.openPresetsOnStartup`
- `editor.startupMode`
- `editor.confirmBeforeClear`
- `editor.tunerReference`
- `editor.showLatencyTips`
- `editor.libraryView`
- `editor.libraryFavoritesFirst`
- `editor.switcherShortcut`
- `editor.switcherModes`

Existing global/state-backed settings found through `PluginStateModel` and global parameter wiring:

- Engine on/off
- Switcher mode
- Input gain
- Input gate
- Force mono
- Output volume
- Output limiter ceiling
- Output mix
- Line A/B gain, pan and width

Existing settings-related UI before this pass:

- A `SettingsOverlay` with General, Audio, Library, Controllers, Profile and Cloud.
- `GeneralSettingsPage` for interface/startup preferences.
- `AudioSettingsPage` with session/default/device tabs and an embedded JUCE device selector in standalone.
- `LibrarySettingsPage` for current preset/startup preset/preset folder.
- Placeholder pages for controllers/profile/cloud.
- Wizard launch buttons that could launch the existing full `AudioSetupWizard`.

## Gaps Found

- Settings had only six broad categories, not a complete configuration map.
- Most requested product areas had no visible place in Settings.
- Search, footer actions, reset-all confirmation, export/import entry points and config health were missing or incomplete.
- Audio Setup Wizard was available elsewhere, but not represented as a safe planned shell inside Settings.
- Many backend-unavailable settings were not visible as planned/disabled rows, making the roadmap unclear.

## Implementation Direction

The new surface is `SettingsOverlayV2`, added in `Source/Core/PluginEditor.cpp`. It keeps the old overlay code present but routes `openSettingsOverlay()` to V2.

Design choices:

- Single premium modal with left navigation.
- 19 requested sections.
- Search field that scans section names, setting names, descriptions and status text.
- Status badges: Active, Read-only, Planned, Shell, Protected, Danger.
- Footer actions: Apply/Save, Reset Section, Restore Defaults, Export Settings, Import Settings.
- Dangerous reset-all behavior uses confirmation.
- Audio Setup Wizard is a placeholder shell only; it does not invoke the full existing wizard from Settings in this phase.

## Sections Implemented

1. Overview
2. Audio
3. Audio Setup Wizard
4. Input & Output
5. Performance
6. Engine & Processing
7. Modules
8. Pedals
9. Amplifiers
10. Cabinets
11. Presets & Sessions
12. MIDI / Control
13. Interface
14. Workflow
15. Diagnostics
16. Files & Storage
17. Safety
18. Updates / About
19. Advanced

## Persistence Behavior

No schema change was made. Existing editor preferences continue to use `editor-settings.xml`.

New behavior:

- `Restore Defaults` clears editor preferences only after confirmation.
- `Reset Section` restores known safe editor preference defaults for Audio, Interface, Workflow and Presets & Sessions.
- `Export Settings` copies the editor settings XML to `Documents/NOVA-settings-export.xml`, or creates an empty settings XML if no settings file exists.
- `Import Settings` is visible but planned/disabled via an explanatory notice because safe validation and merge behavior need a dedicated pass.

## Intentionally Not Implemented

- Full Audio Setup Wizard logic.
- Online updater.
- MIDI mapping backend.
- Global performance mode backend.
- Full config JSON import/export.
- New audio routing, DSP or OutputChain behavior.
- DAW/Reaper smoke.
- Factory approval.
