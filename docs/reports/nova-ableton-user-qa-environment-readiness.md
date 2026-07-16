# NOVA Ableton User QA Environment Readiness

## Executive Summary

* Status: ENVIRONMENT BLOCKER
* Can start final human-assisted User QA: NO
* Main blockers: A targeted no-NOVA matrix reproduced an Ableton exit crash with an empty Set at the required 64-sample buffer. The dump is an Ableton-internal `0xc0000005` software-NX fault with no NOVA or JUCE module loaded. The two earlier crashes remain Ableton-main-thread `std::out_of_range` exceptions; the second also occurred with no NOVA module loaded. Higher-buffer checks were clean but were not expanded into the single- or multiple-NOVA matrices, so they are not yet a safe final-QA workaround. Human confirmation of audible UR12 output is also still pending.

## NOVA Pinned State

* Branch: `master`
* Expected NOVA commit: `d3dcc1d8b205829d17ca7f6067e1e0860ebdfd4b`
* Expected crash-isolation starting HEAD: `348c79d13a0e7c2167d73f2ea9a9291e7aa175a8`
* Current HEAD before this addendum: `348c79d13a0e7c2167d73f2ea9a9291e7aa175a8`
* Working tree: Clean before this report update.
* HEAD explanation: The targeted run started at the exact expected documentation HEAD. Excluding this report, the repository has no differences from the validated NOVA source content and contains no source changes.
* VST3 source path: `C:\Users\gusta\OneDrive\Desktop\DEV\ULTIMATE\NOVA\Builds\VisualStudio2022\x64\Release\VST3\NOVA.vst3`
* VST3 installed path: Ableton scans the source bundle in place through its dedicated custom VST3 folder. `C:\Program Files\Common Files\VST3\NOVA.vst3` is absent because Windows denied the unelevated copy.
* VST3 source hash: `BB1E00696DD1CB94EFB13298DA09C4EF47F06BA3FE16B8E9362886EF7C658567` for `Contents\x86_64-win\NOVA.vst3`
* VST3 installed hash: `BB1E00696DD1CB94EFB13298DA09C4EF47F06BA3FE16B8E9362886EF7C658567` for the Ableton-accessible in-place bundle
* Standalone hash: `742C84A1679DE45693FFB4E38020E225DADC27F799D076438A110BEEAF19420B`
* moduleinfo.json hash: `16BE1AD9756D9ED5B7F7616ED3B0EDEE95E2FB97F88CCA0AA5CE8D12DCB75298`
* Result: Binary and source-content identity PASS. Exact-HEAD identity differs only by the prior documentation-only report commit.

## Ableton Installation

* Product: Ableton Live 12 Trial
* Version: `12.4.3` (`2026-07-07_e3d8be4d07`)
* Executable: `C:\ProgramData\Ableton\Live 12 Trial\Program\Ableton Live 12 Trial.exe`
* Executable signature: Valid; signed by Ableton AG
* Preferences: `C:\Users\gusta\AppData\Roaming\Ableton\Live 12.4.3\Preferences`
* Trial authorization: Authorized. Live displayed that saving and exporting were successfully activated with 30 days remaining.
* Saving available: YES. A temporary Set was written successfully.
* Export available: YES. `Export Audio/Video` is enabled and the Trial activation dialog explicitly confirmed exporting.
* Launch result: The main application window launched and was initially responsive. The environment later failed stability validation with two matching crashes during Set transition/open.
* Risks: BLOCKING. Windows Application Error events occurred at `2026-07-16 13:43:32` and `2026-07-16 13:48:37`; both identify `Ableton Live 12 Trial.exe`, `KERNELBASE.dll`, and exception `0xe06d7363`. The second Live log contains `Licensing: Network timer callback failed` and `Python exception: invalid unordered_map<K, T> key`. The first crash occurred after NOVA had already been removed by loading Ableton's default Set, so the evidence does not isolate NOVA as the cause.

## NOVA Plug-In Discovery

* VST3 system folder: `C:\Program Files\Common Files\VST3` enabled; NOVA is not copied there because elevation was unavailable.
* Custom VST3 folder: `C:\Users\gusta\OneDrive\Desktop\DEV\ULTIMATE\NOVA\Builds\VisualStudio2022\x64\Release\VST3` enabled; VST2 custom folder remains disabled and separate.
* Scan result: PASS. Ableton's plug-in database contains one enabled NOVA module with successful scan state and the exact validated bundle path.
* Displayed plugin name: `NOVA`
* Manufacturer: `NOVA Audio`
* Version: `1.0.0`
* Format: VST3 (`VST 3.8.0` SDK reported by Ableton)
* Load result: PASS on the `Vocal Main` audio track. Ableton logged successful processor creation and 15 exposed parameters.
* Editor open/close: PASS
* Settings open/close: PASS; the Settings overlay was opened and cancelled without changing NOVA preferences.
* Risks: The bundle is scanned in place rather than copied to the system VST3 folder. This is byte-identical but depends on the repository build path remaining present.

## Audio Environment

* Driver type: ASIO
* Input device: Yamaha Steinberg USB ASIO / Steinberg UR12
* Output device: Yamaha Steinberg USB ASIO / Steinberg UR12
* Input channels: 2
* Output channels: 2
* Default sample rate: 44.1 kHz
* Default buffer: 64 samples
* Input latency: 3.29 ms
* Output latency: 4.29 ms
* Overall latency: 7.57 ms
* Test tone audible: HUMAN CONFIRMATION PENDING. Ableton's 440 Hz test tone was successfully toggled on for five seconds at `-36 dB` and returned to Off.
* Imported audio playback: Ableton's installed demo Set played for ten seconds through the UR12 path; human audibility confirmation is pending.
* NOVA processing sanity: NOVA's Ableton device activator changed `On -> Off -> On` during playback. The routing state change passed; a human-audible processed/bypassed difference was not claimed.
* Available sample rates: 44.1, 48, 88.2, 96, 176.4, and 192 kHz. Switching to 48 kHz and 96 kHz succeeded, and the environment was restored to 44.1 kHz.
* Available buffer sizes: 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, and 2048 samples.
* Risks: Human audibility and dropout confirmation remain pending. Ableton instability blocks using this audio path for final QA.

## Required QA Capability Check

* Human listening path ready: CONDITIONAL; the UR12 ASIO path is active, but human audibility is not yet confirmed.
* Automation available: YES. Ableton exposed NOVA's parameters and successfully selected an automation lane for NOVA `Input Gain`.
* Save/reload available: BLOCKED. Save passed, but Set transition/open caused two Ableton crashes.
* Offline export available: YES. WAV/PCM execution was not run because this was environment preparation only.
* Delay Compensation enabled: YES
* NOVA reported latency visible: Not verified before the crash stop condition.
* Waveform alignment workflow available: YES. Ableton displayed imported/demo audio waveforms and provides arrangement zoom controls.
* Multiple instances available: Not verified because the crash stop condition was reached.
* Result: BLOCKED. Save/reload stability, NOVA latency display, multiple-instance sanity, and human audibility are not all verified.

## QA Workspace

* Workspace path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA`
* Project path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\Project`
* Source audio path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\SourceAudio`
* Export path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\Exports`
* PDC evidence path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\PDC`
* Screenshot path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\Screenshots`
* Log path: `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\Logs`
* Temporary Set: Retained at `Project\NOVA-Environment-Sanity Project\NOVA-Environment-Sanity.als` as crash-reproduction evidence outside the repository.
* Ableton preferences backup: `Backups\Ableton-Live-12.4.3-Preferences-20260716-131333`
* Ableton backup hashes: `Preferences.cfg` `AE0AFB2003E4C089A911B02FE06FE1722249C16191C3CD57C011489BB00B1435`; `Library.cfg` `D61C65F38E64572694D0B9BEE9AE06D503EC84F9F6D8D80C8F5FF1C77B2D6ED1`; `AddOns.txt` `59A6D193CA2058D229A06C1C168FD22F71FF46CD3A0B9774E64CE8781ACE9944`; `CrashDetection.cfg` `63DA4BFA69460A80E4CB73D3DE6D9E4C9DD11FAB0A216EA8563FE18691BAD0EE`
* NOVA user settings backup: `Backups\NOVA-User-Settings-20260716-131351\editor-settings.xml`, SHA-256 `629196E949D05BC39858B380A741C7D1A3E6F032B2FA5060107A2AF1080D3419`; no `startup-preset.txt` existed.
* Crash evidence: `Logs\Ableton-Crash-Report-20260716-134441.zip`, SHA-256 `C0FC22866DA0C3613CE0C287B21642D76F1D28DE881ABFD3B2516F1AE623504D`; `Logs\Ableton-Live-12.4.3-Log-after-environment-check.txt`, SHA-256 `6F6FD84CAAFBF0FF951360779ED1CE72FEBF16D50AB48A57A7C06879B7F42958`.
* Restore plan: With Ableton closed, copy the backed-up Ableton preference files to the original `Preferences` directory if the custom VST3/audio changes need to be reverted. Copy the backed-up `editor-settings.xml` to `C:\Users\gusta\AppData\Roaming\NOVA` only if NOVA preferences require restoration. No NOVA setting was changed through the Settings overlay.

## Repository State

* Source changes: NO
* Documentation changes: Updated `docs/reports/nova-ableton-user-qa-environment-readiness.md`.
* Generated repository artifacts: None
* Commit created: YES (local documentation-only commit)
* Push performed: NO

## Remaining Actions Before Final QA

1. Resolve the repeatable Ableton Live 12.4.3 crash before using this installation for QA. Repair/reinstall the signed Trial build or provide the preserved crash package to Ableton Support, with special attention to the licensing callback exception.
2. Confirm Live can create, close, and reopen multiple Sets without an APPCRASH.
3. Re-run environment preparation from Phase 0 and verify NOVA's displayed device latency plus two simultaneous NOVA instances.
4. Have the human user confirm the UR12 test tone and playback are audible, safe, and free from immediate crackling/dropouts.
5. Do not start formal User QA until the environment report can truthfully be marked READY.

## Next Recommended Step

Environment remains blocked.

## Ableton Crash Isolation Addendum

### Existing Crash Evidence

* Crash timestamps: `2026-07-16 13:43:32` (PID `28208`, `0x6E30`), `2026-07-16 13:48:37` (PID `4628`, `0x1214`), and the targeted no-NOVA baseline crash at `2026-07-16 14:42:42` (PID `17972`, `0x4634`). The corresponding dumps were written at `13:43:36`, `13:48:40`, and `14:42:45`.
* Faulting modules: Windows named `KERNELBASE.dll` for the first two events. Local dump analysis resolves both to `msvcp140!std::_Xout_of_range` called from Ableton main/UI-thread frames. Windows named an unknown module/`StackHash_ac46` for the no-NOVA event; local dump analysis resolves it to an attempted execute at address zero on an Ableton worker thread, with `Ableton Live 12 Trial.exe` frames immediately below it.
* Exception codes: The first two crashes are C++ exceptions `0xe06d7363` at offset `0x00000000000c1ada`, with dump bucket `CPP_EXCEPTION_e06d7363_msvcp140.dll!std::_Xout_of_range`. The no-NOVA baseline crash is `0xc0000005`, exception data `0x8`, with dump bucket `SOFTWARE_NX_FAULT_c0000005_Ableton Live 12 Trial.exe!Unknown`.
* Dumps/logs: Full local dumps remain uncommitted at `%LOCALAPPDATA%\CrashDumps\Ableton Live 12 Trial.exe.28208.dmp` SHA-256 `A1580874F68E511DB52F575E905160B81AA711429EE73C547FEA43295DFC6A8D`, `...4628.dmp` SHA-256 `D7635ADCAEB3629EE5C188381C28DCCE2DB545AC8E8F08D7A98DA2445F7B8570`, and `...17972.dmp` SHA-256 `BDDAB0DB36EF3048BA6022C93C7A680D63CACF0C8C4C5E53B79F9A8F3B065A17`. Small debugger and WER text evidence is preserved under `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\Crash-Isolation-20260716\Evidence`.
* NOVA/JUCE evidence: NOVA was loaded in the first crash process but is absent from the faulting stack. NOVA is not loaded at all in the second or third crash process. No JUCE frame or separately loaded JUCE module appears in any faulting stack. The third crash was reproduced with the saved empty baseline Set and no plug-in device.
* Ableton evidence: The first two stacks are identical through Ableton main/UI-thread frames and `std::_Xout_of_range`. The second Live log also records `Licensing: Network timer callback failed` and `Python exception: invalid unordered_map<K, T> key` immediately before termination. The third stack is an Ableton worker-thread software-NX fault after a successful empty-Set reopen and application-exit request.
* ASIO/driver evidence: `ysusb_asio64.dll` was loaded in all three processes but is absent from every faulting stack. The first dump also contains Ableton's own ASIO support module outside the fault stack. This establishes driver presence, not driver causality.
* Assessment: The crashes are not attributable to NOVA on the available evidence. Ableton Live 12.4.3 Trial is demonstrably unstable without NOVA at the original 44.1 kHz/64-sample configuration. The three crashes span two Ableton-internal failure signatures, so the environment is not sufficiently isolated for final QA.

### Baseline Without NOVA

* Empty Set cycles: Five complete save/new-Set/reopen/application-exit cycles at 44.1 kHz/64 samples. Four passed; cycle 3 crashed on exit after the reopen succeeded. The two earlier preparation crashes are not counted in these five cycles.
* Stock-device cycles: Five complete cycles at 44.1 kHz/64 samples with one Ableton Utility device. All five passed.
* Buffers tested: 64, 256, and 1024 samples with Yamaha Steinberg USB ASIO. Three complete empty-Set cycles passed at 256 and three passed at 1024. One extra 1024 procedural attempt was excluded because the reopen action did not occur.
* Crashes: One crash in 5 empty-Set cycles at 64 samples; 0 in 5 stock-device cycles at 64; 0 in 3 empty-Set cycles at 256; 0 in 3 empty-Set cycles at 1024. Total counted matrix: 16 complete cycles, 1 crash.
* Exit timing: Empty/64 clean and crash exits ranged from `0.90` to `3.81` seconds; stock/64 from `0.90` to `1.27` seconds; empty/256 from `1.05` to `1.32` seconds; empty/1024 from `1.22` to `1.73` seconds. Transition and reopen actions normally completed below two seconds after the application was ready.
* Result: FAIL at the required original configuration. The no-NOVA baseline reproduces an Ableton crash, which blocks NOVA-specific attribution and final User QA.

### Single NOVA Instance

* Scenarios: The controlled Phase 4 scenario matrix was not started after the no-NOVA baseline produced a new dump-backed crash. The prior environment-preparation sanity check remains valid only as a single successful load/editor/settings/automation observation, not as a repeated stability matrix.
* Total cycles: 0 targeted Phase 4 cycles.
* Crashes: Not tested in this matrix.
* Reproducible scenario: None established for NOVA.
* Editor/settings relationship: Unknown; neither original faulting stack contains NOVA/JUCE editor code, and the second and third crash processes did not load NOVA.
* Automation relationship: Unknown; the earlier one-lane Input Gain check passed, but it was not repeated here.
* Result: BLOCKED BY BASELINE. No NOVA-specific crash is established.

### Multiple NOVA Instances

* Two-instance result: Not run because the Ableton-only baseline failed first.
* Three-instance result: Not run because the Ableton-only baseline failed first.
* Open-editor result: Not run.
* Total cycles: 0 targeted Phase 5 cycles.
* Crashes: Not tested.
* Result: BLOCKED BY BASELINE. Multiple-instance readiness remains unproven.

### Driver/Buffer Isolation

* Driver configurations: Yamaha Steinberg USB ASIO with Steinberg UR12 throughout; 2 inputs/2 outputs at 44.1 kHz. No new driver was installed and no alternate driver was required by the Phase 6 trigger.
* Buffer configurations: 64 samples for five empty and five stock-device cycles; 256 samples for three empty cycles; 1024 samples for three counted empty cycles.
* Crash frequency: Empty/64 `1/5`; stock/64 `0/5`; empty/256 `0/3`; empty/1024 `0/3`. Both earlier preparation crashes also occurred at 64 samples.
* Faulting modules: Ableton/`msvcp140` for the two earlier failures and Ableton executable frames for the no-NOVA software-NX failure. `ysusb_asio64.dll` is loaded but not present on any faulting stack.
* Stable configuration: 256 and 1024 samples are clean candidates in this limited baseline matrix. Neither is accepted as a safe final-QA configuration because the single- and multiple-NOVA matrices and human audio-path confirmation were not run there.
* Result: There is material 64-sample sensitivity evidence, but not enough stack evidence to classify the Yamaha driver as the cause or to downgrade the gate to CONDITIONAL.

### Human Audio Path Sanity

* Human present: NO response was available during this run.
* Test tone audible: PENDING; the earlier preparation run toggled Ableton's test tone successfully but received no human confirmation.
* Imported audio audible: PENDING; the earlier demo playback ran but received no human confirmation.
* NOVA audio audible: PENDING; the earlier NOVA activator toggle passed as a routing action but received no human-audibility confirmation.
* Immediate crackling/dropouts: PENDING.
* Stable audio configuration: Automation observed clean host operation at 256 and 1024 samples, but no human-audible configuration is claimed.
* Result: PENDING; no listening judgment was inferred.

### State Restoration

* Ableton settings restored: YES. The pre-test Ableton Preferences and Unlock snapshot at `C:\Users\Public\Documents\NOVA-QA\Ableton-Final-QA\Crash-Isolation-20260716\Backups\BeforeTesting-20260716-142811` was restored after Live closed.
* NOVA settings restored: NOT REQUIRED; all NOVA user-state hashes remained identical to baseline throughout the run.
* Hash verification: PASS. The pre-test manifest covers 42 Ableton/NOVA files; post-restoration mismatches are `0`. Yamaha `PeriodFrames` was restored from the tested 256/1024 values to the original `64`.
* Temporary files removed: Test Sets, cycle logs, and summaries are intentionally retained only in the external `Crash-Isolation-20260716` QA workspace. Full dumps remain in the Windows crash-dump directory and are not copied into the repository.
* Repository contamination: NONE before this documentation update. No source, binary, database, preference, dump, or machine-local test artifact was added to the repository.

### Updated Environment Gate

* Status: ENVIRONMENT BLOCKER
* Can start final human-assisted User QA: NO
* NOVA-specific crash established: NO
* Main evidence: A fresh empty-Set/no-NOVA crash at 64 samples produced a full dump with an Ableton-only software-NX stack. The second original crash also had no NOVA module loaded, and both original faulting stacks resolve to Ableton-main-thread `std::out_of_range` exceptions.
* Remaining blockers: Ableton does not pass the required no-NOVA 64-sample baseline; the cleaner 256/1024 candidates have not passed the single- and multiple-NOVA matrices; human audio-path confirmation is pending.
* Required next step: Repair or replace the Ableton Live 12.4.3 Trial environment, then pass five no-NOVA save/new/reopen/exit cycles at 256 samples before running the single- and multiple-NOVA matrices and requesting human audio-path confirmation.
