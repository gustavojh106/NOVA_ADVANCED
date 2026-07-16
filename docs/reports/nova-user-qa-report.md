# NOVA User QA Report

## Executive Summary

* Status: BLOCKED
* User QA approved: NO
* Release approved: NO
* Main blockers:
  * No commercial DAW was installed. The official JUCE AudioPluginHost 8.0.12 covered real VST3 scan/load, realtime graph routing, editor workflows, three instances, and component-state save/reload, but it does not provide the required DAW automation timeline or offline render/export workflow.
  * Practical PDC comparison was not available in the installed host. The reviewed pre-User-QA engineering harness is not being treated as User QA evidence.
  * No human listening pass was available for tonal quality, input/output level, severe-pop/click judgment, realtime-versus-offline comparison, or dangerous-gain judgment.
  * One non-reproducible slow host exit was observed and is logged as Medium defect `UQ-001`.

The objective User QA that could be executed passed without a NOVA crash, state corruption, or Critical/High defect. That partial evidence is not sufficient for release approval.

## QA Starting State

* QA start: 2026-07-14 19:59 -06:00 (America/Costa_Rica)
* QA end: 2026-07-14 20:46 -06:00
* Branch: `master`
* HEAD: `d2f043c24d8572105c7ec85c79af84ecd9881d85`
* Expected technical RC commit present: `68cb2db42307859b5da4c6e7fb965c9fdcabfc59` (contained by `master`)
* Expected final-report commit present: `d2f043c24d8572105c7ec85c79af84ecd9881d85` (current HEAD, contained by `master`)
* Working tree before report creation: clean
* VST3 bundle: `C:\Users\gusta\OneDrive\Desktop\DEV\ULTIMATE\NOVA\Builds\VisualStudio2022\x64\Release\VST3\NOVA.vst3`
* VST3 binary: `C:\Users\gusta\OneDrive\Desktop\DEV\ULTIMATE\NOVA\Builds\VisualStudio2022\x64\Release\VST3\NOVA.vst3\Contents\x86_64-win\NOVA.vst3`
* VST3 binary timestamp: 2026-07-14 12:52:21.577 -06:00 (2026-07-14T18:52:21.577Z)
* VST3 binary SHA-256: `BB1E00696DD1CB94EFB13298DA09C4EF47F06BA3FE16B8E9362886EF7C658567`
* Standalone path: `C:\Users\gusta\OneDrive\Desktop\DEV\ULTIMATE\NOVA\Builds\VisualStudio2022\x64\Release\Standalone Plugin\NOVA.exe`
* Standalone timestamp: 2026-07-14 12:53:17.392 -06:00 (2026-07-14T18:53:17.392Z)
* Standalone SHA-256: `742C84A1679DE45693FFB4E38020E225DADC27F799D076438A110BEEAF19420B`
* moduleinfo.json: `C:\Users\gusta\OneDrive\Desktop\DEV\ULTIMATE\NOVA\Builds\VisualStudio2022\x64\Release\VST3\NOVA.vst3\Contents\Resources\moduleinfo.json`
* moduleinfo.json timestamp: 2026-07-14 12:52:21.882 -06:00 (2026-07-14T18:52:21.882Z)
* moduleinfo.json SHA-256: `16BE1AD9756D9ED5B7F7616ED3B0EDEE95E2FB97F88CCA0AA5CE8D12DCB75298`
* Reports reviewed: `docs/reports/nova-final-pre-user-qa-validation-report.md`

## QA Matrix

The matrix was written before execution. Unavailable coverage remains explicit and is not inferred as passing.

### Applications and Hosts

| Application / host | Availability | Result |
|---|---:|---|
| NOVA Release Standalone | AVAILABLE | Objective workflows executed; PASS for exercised cases, but human listening unavailable |
| JUCE AudioPluginHost 8.0.12 | AVAILABLE after local Release build | Real VST3 scan/load, routed graph, editor, settings, three instances, state save/reload, and sample-rate/buffer matrix executed |
| REAPER | UNAVAILABLE (not installed) | Not run |
| Ableton Live | UNAVAILABLE (not installed) | Not run |
| FL Studio | UNAVAILABLE (not installed) | Not run |
| Cubase | UNAVAILABLE (not installed) | Not run |
| Studio One | UNAVAILABLE (not installed) | Not run |
| pluginval | UNAVAILABLE (not installed) | Not run; supplemental only |

### Sample Rates and Buffer Sizes

| Context | 44.1 kHz / 64 | 48 kHz / 256 | 96 kHz / 1024 |
|---|---:|---:|---:|
| Standalone, DirectSound | PASS | PASS | PASS |
| VST3, JUCE AudioPluginHost with DirectSound | PASS | PASS | PASS |

Additional device observations:

* Standalone Windows Audio Exclusive Mode opened the Steinberg UR12 at 44.1 kHz / 576 samples and offered only 44.1 kHz in that mode.
* AudioPluginHost initially opened Windows Audio at 44.1 kHz / 448 samples and offered only 44.1 kHz with the selected NVIDIA output. DirectSound provided the required matrix.
* Unsupported Windows Audio Low Latency Mode changed safely to `No audio device is currently open`; selecting DirectSound recovered the device without a crash or hang.

### Required User Workflows

| ID | Workflow | Result |
|---|---|---:|
| UQ-01 | Application/plugin scan and load | PASS |
| UQ-02 | UI open/close/reopen | PASS |
| UQ-03 | Settings open/close and plugin/standalone clarity | PASS |
| UQ-04 | Preset browse/load | PASS |
| UQ-05 | Preset save/reload | PASS |
| UQ-06 | Session/component state save/reload | PASS in AudioPluginHost graph; no commercial DAW session |
| UQ-07 | Global bypass control | PASS for control/state responsiveness; listening not run |
| UQ-08 | Module bypass control | PASS for control/state responsiveness; listening not run |
| UQ-09 | Multiple instances | PASS with three instances and independent state |
| UQ-10 | Three representative parameter automations | NOT RUN; installed host has no DAW automation timeline |
| UQ-11 | Realtime playback/audio processing | PARTIAL; stereo graph and device active, but no source/listening validation |
| UQ-12 | Offline render/export | NOT RUN; unavailable in installed host |
| UQ-13 | Input/output level sanity | NOT RUN as a listening/measurement pass |
| UQ-14 | Practical latency/PDC alignment | NOT RUN; unavailable in installed host |
| UQ-15 | CPU usage sanity | PASS for displayed Standalone processing margins across matrix |
| UQ-16 | Startup preset behavior | PASS |
| UQ-17 | Settings Apply/Cancel/Reset | PASS |
| UQ-18 | Production diagnostics hidden/inert | PASS for user-facing UI inspection; pre-QA negative-hook evidence reviewed separately |
| UQ-19 | Short stability and repeated workflows | PASS with one Medium, non-reproducible host-exit observation |
| UQ-20 | No crashes, hangs, severe pops, or corrupted state | PARTIAL; no crash/corruption, one transient slow exit, listening unavailable |

* Hosts tested: NOVA Release Standalone; official JUCE AudioPluginHost 8.0.12
* Hosts unavailable: REAPER, Ableton Live, FL Studio, Cubase, Studio One, pluginval
* Sample rates tested: 44.1 kHz, 48 kHz, 96 kHz in Standalone and VST3 host
* Buffer sizes tested: 64, 256, 1024 samples in Standalone and VST3 host; initial device-specific 448 and 576 sample configurations also observed
* Presets tested: `aa`, `default`, all five `Factory - Orbit` presets, `TEST`, and temporary `Codex User QA 20260714`
* Sessions tested: one three-instance AudioPluginHost `.filtergraph` save/reload
* Automation tested: NO
* Offline render tested: NO

## Standalone QA Results

* Launch/lifecycle: PASS for launch, close, relaunch, startup-preset relaunch, and clear-startup relaunch. All launches were responsive.
* Audio device behavior: PASS for direct device UI, unsupported-mode safe fallback, recovery, and the 44.1/64, 48/256, 96/1024 DirectSound matrix. No fake host-controlled UI appeared.
* UI/settings:
  * Cancel: PASS. `Show performance stats` changed from On to Off in the draft, Cancel restored On, and `editor-settings.xml` remained at SHA-256 `629196E949D05BC39858B380A741C7D1A3E6F032B2FA5060107A2AF1080D3419`.
  * Apply: PASS. The draft Off value persisted after reopening and the file hash changed as expected.
  * Reset defaults: PASS. Confirmation appeared; `Show performance stats` returned On; the audio settings file stayed unchanged.
  * Placeholder/diagnostic UI: PASS. General, Audio, Library, and implemented wizard controls were visible; no production diagnostic/artifact controls were exposed.
* Presets:
  * PASS loading `aa`, `default`, two factory presets, then cycling through all remaining factory presets and `TEST` without a crash or corrupt UI.
  * PASS saving temporary preset `Codex User QA 20260714` (1,572 bytes, SHA-256 `7C0EF4D7BAF6E5755B75173B386F6CC4643A531D8DA14A3A82F6A912B0C179E3`) and reloading it after loading another preset.
* Startup preset:
  * Ordinary preset loads/saves left the startup preset as None.
  * `Use Current on Start` created the startup pointer; relaunch loaded `Codex User QA 20260714` with one module.
  * `Clear Startup` removed the pointer; the next relaunch showed `No Preset` and did not autoload the saved preset.
* Audio behavior:
  * Global power toggled Off -> On -> Off and remained responsive; the active state showed processing load.
  * Module bypass changed one of three module buttons On -> Off and restored all three to On.
  * Three representative visible controls changed to safe values and returned exactly to their original values.
  * A module editor opened and closed with Escape.
  * Listening judgment for pops/clicks, tone, gain, silence, and level was NOT RUN.
* Stability:
  * Ten consecutive preset selections/loads and five settings open/cancel cycles remained responsive.
  * Working set after the repeated cycle was approximately 135 MB.
  * Displayed matrix statistics were: 44.1/64 = 17.0% and 0.25 ms in a 1.5 ms buffer; 48/256 = 17.8% and 0.95 ms in a 5.3 ms buffer; 96/1024 = 34.0% and 3.62 ms in a 10.7 ms buffer.
* Result: BLOCKED/PARTIAL. All objective cases executed here passed, but Standalone User QA cannot pass without a human audio/listening pass.
* Defects: None specific to the Standalone.

## VST3 Host QA Results

* Host used: official JUCE AudioPluginHost 8.0.12, Release build at `C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE\extras\AudioPluginHost\Builds\VisualStudio2022\x64\Release\App\AudioPluginHost.exe`; SHA-256 `1B690B094C366163CDED168D3790E8B62B5D8B5449255A4C36FB1D3708FF6784`.
* Plugin discovery/load: PASS. The exact Release build path scanned as name `NOVA`, format `VST3`, manufacturer `NOVA Audio`, version `1.0.0`; no entry appeared in the host's recently-crashed list.
* Realtime playback: PARTIAL. A stereo graph was created and verified as Audio Input channels 0/1 -> NOVA inputs 0/1 -> Audio Output channels 0/1. The device remained open and the host responsive across the sample-rate/buffer matrix. No audio source/listening pass was available.
* Multiple instances: PASS. Three simultaneous NOVA nodes and three simultaneous editor windows opened. Their states remained independent: one instance held four active modules, one loaded `Factory - Orbit Crystal Grid` with one active module, and one remained empty with global power Off.
* Settings window: PASS. Plugin mode displayed `This instance is hosted by a DAW. Use NOVA Standalone for direct device configuration.` and exposed zero audio-device ComboBoxes. Cancel preserved the draft; Apply persisted it; Reset showed a confirmation and Cancel dismissed it without a crash.
* Automation: NOT RUN. JUCE AudioPluginHost has no DAW automation-lane workflow.
* Session save/reload: PASS for host component state. A three-instance graph saved with three NOVA VST3 nodes and four stereo connections. Relaunching the host with the graph restored three editor windows and the independent 4-module/On, 1-module/On, and empty/Off states.
* Offline render/export: NOT RUN. The installed host does not provide the required DAW render/export workflow.
* PDC practical check: NOT RUN. The installed host did not provide a practical parallel-track/PDC comparison. The reviewed 44.1/48/96 engineering PDC evidence is not substituted for User QA.
* Sample-rate/buffer matrix: PASS for host/device lifecycle and plugin stability at DirectSound 44.1/64, 48/256, and 96/1024.
* Host close/reopen: Graph reload passed. One first close took approximately 15-25 seconds and was temporarily non-responsive; a repeat after graph reload exited in 1.03 seconds.
* Result: BLOCKED/PARTIAL. Core host/component workflows passed, but DAW automation, offline render, practical PDC, and listening coverage are missing.
* Defects: `UQ-001`.

## Defect Log

### UQ-001

* ID: UQ-001
* Title: First three-instance AudioPluginHost exit was temporarily non-responsive
* Severity: Medium
* Environment: Windows; NOVA Release VST3; JUCE AudioPluginHost 8.0.12 Release; DirectSound
* Host: JUCE AudioPluginHost 8.0.12
* Sample rate: 96 kHz
* Buffer size: 1024 samples
* Steps:
  1. Scan and insert three NOVA VST3 instances.
  2. Route one instance in stereo from host Audio Input to Audio Output.
  3. Keep independent states across the three instances and save the graph.
  4. Close the three editor windows and close the host.
* Expected: Host exits promptly and remains responsive during teardown.
* Actual: Initial close attempts left the host open; Alt+F4 made it non-responsive for at least 15 seconds. The process then exited without external termination. After graph reload, the same three-editor close and host exit completed in 1.03 seconds.
* Frequency: 1 of 2 observed shutdowns
* Evidence: Process responsiveness polling; saved/reloaded graph with three NOVA nodes and restored independent states; second shutdown stopwatch result 1.03 seconds
* Suspected area: Host/audio-device/plugin teardown interaction; not isolated to NOVA and not reproduced
* Blocks user QA pass: NO by itself; requires confirmation in the intended commercial DAW
* Blocks release: NO by itself
* Recommended next action: Repeat three-instance close/reopen in the intended commercial DAW at 96 kHz / 1024 and collect host diagnostics if the delay recurs.

No Critical or High defect was found in the executed coverage.

## Audio/DSP Observations

* Tonal concerns: NOT ASSESSED; human listening unavailable
* Noise/click/pop concerns: NOT ASSESSED aurally; no UI/device crash or corrupt processing state observed
* Gain concerns: NOT ASSESSED aurally; safe control changes restored exactly
* Latency/PDC concerns: Practical User QA NOT RUN; pre-QA engineering evidence reviewed but not substituted
* CPU concerns: No objective Standalone margin blocker observed in the exercised matrix; 96 kHz / 1024 displayed 34.0% and 3.62 ms processing time within a 10.7 ms buffer

## Settings/User Workflow Observations

* Apply/Cancel: PASS in Standalone and hosted VST3
* Reset defaults: PASS in Standalone; confirmation/cancel lifecycle PASS in hosted VST3
* Startup preset: PASS for explicit set, relaunch restore, clear, and non-mutation by ordinary save/load
* Plugin vs Standalone clarity: PASS; hosted VST3 showed DAW guidance and no editable standalone device selector
* Persistence: PASS for local preference persistence, temporary preset round-trip, startup pointer behavior, and three-instance component state save/reload

## Final Gate Decision

* User QA result: BLOCKED
* Release gate: BLOCKED
* Reason: Required User QA remains incomplete. The objective Standalone and real VST3 host workflows that were available passed without a Critical/High NOVA defect, but there is no human listening pass, commercial DAW automation, offline render/export, or practical PDC result.
* Required fixes before release: No code fix is prescribed by this run. Complete the missing manual/commercial-DAW matrix and retest `UQ-001` during three-instance shutdown.

## Final Repository State

* Working tree: Clean except for new report `docs/reports/nova-user-qa-report.md`
* New/modified reports: `docs/reports/nova-user-qa-report.md`
* Source-code changes: NONE
* User state restoration:
  * `editor-settings.xml` restored to SHA-256 `629196E949D05BC39858B380A741C7D1A3E6F032B2FA5060107A2AF1080D3419`
  * `NOVA.settings` restored to SHA-256 `347DC8FBC2B4C700A4921D1E6C64FC877422F5035AE51580A24FAB554D7B78F2`
  * Temporary QA preset removed
  * Startup pointer restored to missing
  * AudioPluginHost settings restored byte-for-byte; QA crash-list file removed
  * Temporary saved graph removed
* Commit created: NO
* Commit hash if created: N/A
* Push performed: NO

## Next Recommended Step

Blocked by missing manual/user validation.

---

# Missing QA Completion Addendum (2026-07-16)

* Addendum run start: 2026-07-16 10:05 -06:00 (America/Costa_Rica)
* Addendum run end: 2026-07-16 10:25 -06:00
* Branch: `master`
* HEAD: `d2f043c24d8572105c7ec85c79af84ecd9881d85` (unchanged from the 2026-07-14 run)
* Working tree at start: clean except the untracked `docs/reports/nova-user-qa-report.md`
* Source-code changes made by this run: NONE
* Binaries re-verified byte-identical to the 2026-07-14 QA run:
  * VST3 binary SHA-256: `BB1E00696DD1CB94EFB13298DA09C4EF47F06BA3FE16B8E9362886EF7C658567`
  * Standalone SHA-256: `742C84A1679DE45693FFB4E38020E225DADC27F799D076438A110BEEAF19420B`
  * moduleinfo.json SHA-256: `16BE1AD9756D9ED5B7F7616ED3B0EDEE95E2FB97F88CCA0AA5CE8D12DCB75298`
* Environment re-scan (2026-07-16): no commercial DAW found in the Windows uninstall registry (HKLM 64-bit, HKLM WOW6432Node, HKCU), Program Files, or Start Menu shortcuts. Checked for: REAPER, Ableton Live, FL Studio, Cubase, Studio One, Pro Tools, Cakewalk, Waveform, Bitwig, Mixcraft, Audacity, LMMS, Ardour, Renoise, Reason. pluginval not on PATH, not in Downloads/Desktop/Program Files. The only available real VST3 host remains JUCE AudioPluginHost 8.0.12 (Release build used in the 2026-07-14 run).
* This addendum run was executed autonomously by tooling with no human operator present at the machine. Per the QA rules, no automated result below is claimed as human listening evidence.

## Human Listening and Gain/Pop Assessment

* Environment: NOT AVAILABLE. No human listener was present for this run; the run was executed by autonomous tooling only.
* Audio material: N/A (no listening pass performed)
* Presets/settings: N/A
* Actions performed: None. Faking or inferring listening results from automated tests is prohibited by the QA rules, so no listening checklist item was attempted.
* Observations: None recorded.
* Result: BLOCKED
* Defects: None (nothing was assessed)
* Exact missing evidence: A human must play (1) a clean instrument-like source, (2) a full-range music loop, and (3) transient-heavy material through NOVA Standalone and/or the VST3 host at normal monitoring level, exercise global/module bypass, preset loads, gain/drive/output moves, and settings open/close during playback, and judge pops, clicks, zipper noise, silence, runaway gain, tonal corruption, and level sanity.

## Commercial DAW Automation

* DAW/host: NONE AVAILABLE. No commercial DAW (and no REAPER) is installed on this machine as of 2026-07-16.
* DAW version if available: N/A
* Parameters automated: None
* Save/reload result: N/A
* Render/export with automation: N/A
* Result: BLOCKED
* Defects: None (nothing was tested)
* Commercial DAW coverage achieved: NO
* Exact missing evidence: Insert NOVA VST3 in a commercial DAW, write/draw automation on at least one gain/level, one tone/filter, and one drive/dynamics/modulation parameter, play back, save/close/reopen the session, confirm automation integrity, and render with automation active.

## Offline Render / Export Comparison

* Host: NONE AVAILABLE. JUCE AudioPluginHost 8.0.12 provides no offline render/export workflow, and no DAW is installed.
* Export settings: N/A
* Presets/settings: N/A
* Comparison method: N/A
* Observations: None
* Result: BLOCKED
* Defects: None (nothing was tested)
* Exact missing evidence: In a render-capable host, process known material through at least one clean preset, one drive/dynamics-heavy preset, and one time/modulation preset; export offline; re-import and compare against realtime playback for silence, tonal mismatch, timing shift, missing processing, and clicks/pops.

## Practical Host PDC Verification

* Host: NONE AVAILABLE for practical parallel-track PDC comparison. The installed AudioPluginHost has no track/render/alignment workflow.
* Method: Not executed. The pre-User-QA engineering impulse/PDC harness evidence (44.1/48/96 kHz, reported-latency-equals-arrival, global-bypass alignment at sample 100) remains reviewed but is explicitly NOT substituted for practical host PDC User QA.
* Sample rates: N/A for this addendum
* Buffer sizes: N/A
* Measured offsets: N/A
* Observations: None
* Result: BLOCKED
* Defects: None (nothing was tested)
* Exact missing evidence: In a real DAW with PDC enabled, duplicate a transient source onto a dry reference track and a NOVA track, render both, and measure transient alignment for NOVA active, global bypass, module bypass, and a latency-changing preset, at 44.1/48 kHz (96 kHz if practical).

## UQ-001 Retest

* Reproduced: NO
* Attempts: 4 measured close/exit cycles (plus 1 additional successful load cycle used to validate the harness)
* Environment: Same host binary as the original observation — JUCE AudioPluginHost 8.0.12 Release (`AudioPluginHost.exe`), same Release NOVA VST3 (SHA-256 `BB1E0069...`), Windows 11 Pro, DirectSound requested at 96 kHz / 1024 samples via seeded host device state (output: Primary Sound Driver), three NOVA VST3 instances with three open editor windows, one instance routed stereo Audio Input -> NOVA -> Audio Output.
* Method: A three-instance `.filtergraph` was reconstructed to match the original topology (three NOVA VST3 nodes with open editors, four stereo connections) and loaded via the host command line. Each cycle: launch, wait for the main window plus three NOVA editors, 8 s steady-state, close the three editor windows, close the host main window, then measure time-to-exit at 100 ms resolution with a responsiveness probe (`SendMessageTimeout` with abort-if-hung) on every poll.
* Observations:
  * Cycle 1: load 1.11 s; exit 0.13 s; no non-responsive period
  * Cycle 2: load 1.03 s; exit 0.11 s; no non-responsive period
  * Cycle 3: load 1.03 s; exit 0.11 s; no non-responsive period
  * Cycle 4: load 1.03 s; exit 0.12 s; no non-responsive period
  * All three editors and the main window appeared in every cycle; editors closed cleanly in every cycle; no save prompt, no crash dialog, no zombie process.
* Deviations from the original observation: no capture/input device was opened (input intentionally left unassigned to avoid an unattended feedback loop); NOVA instances used default state rather than the original 4-module/preset/empty states; every cycle was a load-from-file exit, whereas the original slow exit occurred on the first close after interactive graph construction. Original repro also occurred after an extended interactive session.
* Evidence: `artifacts/nova-uq001-retest-20260716.log` (ignored; SHA-256 `3F90F6741F4DE9211F0E2FB198C3DB5BC7CFC00594B5C53AD40FC9B75750F5FD`), reconstructed graph `artifacts/nova-uq001-retest-20260716.filtergraph.log` (ignored; SHA-256 of original `.filtergraph`: `24E4268B530C72E9312B84B2C2A86BBC25B37BB37CAA2835AA02206C7075F63A`). Host settings and NOVA user files were restored byte-for-byte after the retest (host settings SHA-256 `2BD4F9EF...`; `editor-settings.xml` SHA-256 `629196E9...`; `NOVA.settings` SHA-256 `347DC8FB...` all verified unchanged).
* Final severity: Medium, unchanged — not reproduced in 4 of 4 scripted attempts in the same host, but the original interactive first-close condition was not fully replicated, so the historical observation is retained rather than closed.
* Blocks release: NO by itself. Confirmation in the intended commercial DAW remains recommended once one is available.

## Updated Final Gate Decision (2026-07-16)

* User QA result: BLOCKED
* User QA approved: NO
* Release approved: NO
* Reason: All four required missing evidence items remain unexecutable in this environment. Human listening requires a human listener (none present in this autonomous run); commercial DAW automation, offline render/export comparison, and practical host PDC verification all require a DAW that is not installed (re-verified 2026-07-16). Per the QA rules, User QA remains BLOCKED whenever any of these four items is missing. The UQ-001 retest was the only executable item and passed cleanly (not reproduced, severity stays Medium, non-blocking by itself).
* Required fixes before release: No code fix is prescribed. Required actions are environmental/manual:
  1. A human listening and gain/pop assessment (Standalone and/or hosted VST3).
  2. Automation validation in a commercial DAW (install one of: Ableton Live, FL Studio, Cubase, Studio One, Pro Tools; REAPER acceptable only as a stopgap with commercial coverage still pending).
  3. Offline render/export comparison in that host.
  4. Practical host PDC verification in that host.
  5. Optional: repeat the three-instance exit cycle in the commercial DAW to close out UQ-001.

## Addendum Repository State

* Source-code changes: NONE
* Files changed: `docs/reports/nova-user-qa-report.md` (this addendum) only; retest evidence stored under ignored `artifacts/` paths
* Host/user state: AudioPluginHost settings, NOVA `editor-settings.xml`, and `NOVA.settings` verified byte-identical to their pre-retest baselines
* Push performed: NO
