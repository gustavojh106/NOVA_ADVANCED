# NOVA Ableton User QA Environment Readiness

## Executive Summary

* Status: BLOCKED
* Can start final human-assisted User QA: NO
* Main blockers: Ableton Live 12 Trial crashed twice while replacing or opening Live Sets. Windows recorded matching `APPCRASH` events in `KERNELBASE.dll` with exception `0xe06d7363`, and Ableton logged a licensing network callback exception. Human confirmation of audible UR12 output is also still pending.

## NOVA Pinned State

* Branch: `master`
* Expected NOVA commit: `d3dcc1d8b205829d17ca7f6067e1e0860ebdfd4b`
* Current HEAD before this report update: `699ad9b4bbf9e631d7ac81177151b2677b4f9c0f`
* Working tree: Clean before this report update.
* HEAD explanation: Current HEAD is the previous documentation-only environment report commit. Excluding this report, the repository has no differences from the expected NOVA commit and contains no source changes.
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
