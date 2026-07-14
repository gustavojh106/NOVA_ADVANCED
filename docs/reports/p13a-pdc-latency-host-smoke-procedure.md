# P13A PDC / Latency Host Smoke Procedure

## Scope

This is an engineering smoke procedure for VST3 host PDC and impulse alignment. It is not user QA and does not approve tone, presets, UI, or manual listening.

## Local Tool Availability

Checked on 2026-07-08:

- `pluginval.exe`: not found on `PATH`
- `validator.exe`: not found on `PATH`
- `vst3validator.exe`: not found on `PATH`
- `reaper.exe` / `REAPER.exe`: not found on `PATH`
- `C:\Program Files\REAPER (x64)\reaper.exe`: not found
- `C:\Program Files\Steinberg\VST3PluginTestHost\validator.exe`: not found
- `C:\Program Files\Steinberg\VST3PluginTestHost\vst3validator.exe`: not found

Host-level validation is therefore pending a machine with a VST3 validator or DAW installed.

## Production Build Input

Run from repo root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3
```

Expected VST3:

```text
Builds/VisualStudio2022/x64/Release/VST3/NOVA.vst3
```

## Internal Automated Preflight

Run before host smoke:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3
```

The Debug validation covers OutputChain lookahead latency, host PDC mirroring, bypass latency replacement, preset/state restore latency, and sample-rate reprepare latency.
The Release VST3 build script uses `Rebuild` for Release targets by default.

## Host Smoke Matrix

Use a fresh DAW/session at 48 kHz and repeat at 96 kHz if the host supports it.

1. Insert `NOVA.vst3` on a stereo audio track.
2. Feed a single-sample impulse followed by silence.
3. Record/render the track with no pedals and Engine on.
4. Verify the rendered impulse is aligned by host PDC against a dry reference track.
5. Add an enabled Overdrive before the output path and confirm the host reports/compensates the increased latency.
6. Bypass the Overdrive and confirm the host PDC returns to the OutputChain baseline.
7. Save a session or preset with enabled Overdrive, reload it, and confirm the compensated impulse alignment remains stable.
8. Change sample rate to 96 kHz, reopen/reload if required by the host, and repeat the no-pedal and Overdrive checks.

## Pass Criteria

- VST3 scans successfully.
- Host-reported plugin latency is non-zero with no pedals because OutputChain lookahead is always armed.
- Host-reported plugin latency increases when a latency-bearing pedal is enabled.
- Host-reported plugin latency returns to the OutputChain baseline when that pedal is bypassed.
- Rendered impulse alignment nulls or sample-aligns against a dry reference after host PDC.
- No crash, scan failure, missing `moduleinfo.json`, or stale binary/manifest mismatch.

## Evidence To Capture

- Host and version.
- Sample rate and buffer size.
- VST3 bundle path and binary timestamp.
- `moduleinfo.json` timestamp.
- Host-reported latency for no-pedal, enabled Overdrive, bypassed Overdrive, and restored preset states.
- Rendered impulse files or screenshots showing alignment.
- Any scan/validation logs.

## Current Status

Internal engineering status can proceed to conditional assessment after automated builds/tests pass. External host-level PDC remains pending because no local VST3 validator or DAW executable is available in this environment.
