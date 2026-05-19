# P7E Manual Host Smoke Checklist

Fecha:
Tester:
Build hash:
OS:
Audio device:
Driver/API:
Host:
Host version:
Plugin format: Standalone / VST3
Sample rate:
Buffer size:

## Preconditions

- Confirmar `git diff --check` PASS.
- Confirmar build Release x64 usado para smoke.
- Confirmar `Builds/VisualStudio2022/x64/Release/Standalone Plugin/NOVA.exe` existe.
- Confirmar `Builds/VisualStudio2022/x64/Release/VST3/NOVA.vst3/Contents/x86_64-win/NOVA.vst3` existe para DAW.
- Ejecutar `scripts/run-host-smoke-preflight.ps1` y adjuntar carpeta `artifacts/host-smoke-preflight/<timestamp>`.
- Cerrar instancias previas de NOVA/host que puedan bloquear binarios.

## Evidence To Capture

- `session-log.txt` si existe.
- `audio-base-test-report.txt`.
- `artifacts/audio-thread-policy-scan.txt`.
- `artifacts/rt-profile-release-x64-report.json` o `artifacts/rt-profile-release-x64-report-gate.json`.
- Screenshots opcionales para device settings, plugin scan, plugin loaded, automation, CPU.
- Host project file si aplica.
- Render output si aplica.
- Crash dump/minidump y host log si aplica.

## Standalone Steps

| Step | Action | Expected Result | PASS/FAIL | Notes/Evidence |
| --- | --- | --- | --- | --- |
| ST-01 | Launch Standalone Release. | App opens without crash/hang. |  |  |
| ST-02 | Select audio input/output device. | Device applies and app remains responsive. |  |  |
| ST-03 | Set 44.1 kHz. | Audio remains stable. |  |  |
| ST-04 | Set 48 kHz. | Audio remains stable. |  |  |
| ST-05 | Set 96 kHz if supported. | Audio remains stable or unsupported is reported cleanly. |  |  |
| ST-06 | Set buffer 32. | Stable or unsupported is reported cleanly. |  |  |
| ST-07 | Set buffer 64. | Stable. |  |  |
| ST-08 | Set buffer 128. | Stable. |  |  |
| ST-09 | Set buffer 256. | Stable. |  |  |
| ST-10 | Set buffer 512. | Stable. |  |  |
| ST-11 | Toggle engine off/on during signal. | No crash; processing resumes. |  |  |
| ST-12 | Add Pre, Amp, FX, Cabinet pedals. | Order is canonical and graph remains usable. |  |  |
| ST-13 | Bypass/unbypass individual pedals. | No sustained corrupt output. |  |  |
| ST-14 | Save session/preset. | File writes successfully. |  |  |
| ST-15 | Load saved session/preset. | State restores correctly. |  |  |
| ST-16 | Try corrupt/truncated preset copy. | Rejected or recovered cleanly; app remains usable. |  |  |
| ST-17 | Leave idle 5 minutes. | No hang or runaway CPU. |  |  |
| ST-18 | Play/process 5 minutes. | Audio remains stable; CPU/process-time visibility remains sane. |  |  |

## DAW / VST3 Steps

| Step | Action | Expected Result | PASS/FAIL | Notes/Evidence |
| --- | --- | --- | --- | --- |
| DAW-01 | Scan VST3 path in host. | NOVA appears and is not blacklisted. |  |  |
| DAW-02 | Instantiate NOVA on an audio track. | Plugin loads without crash. |  |  |
| DAW-03 | Feed audio or guitar input. | Output is finite and controllable. |  |  |
| DAW-04 | Save host project. | Save completes. |  |  |
| DAW-05 | Close and reopen project. | NOVA state is restored. |  |  |
| DAW-06 | Toggle host bypass. | No crash; state preserved. |  |  |
| DAW-07 | Toggle NOVA engine/plugin bypass. | No crash; processing resumes. |  |  |
| DAW-08 | Add/remove/bypass pedals inside NOVA. | Project remains stable and state saves. |  |  |
| DAW-09 | Automate parameters across valid ranges. | Automation plays without crash/NaN/state corruption. |  |  |
| DAW-10 | Save/reload plugin state or preset. | State remains canonical. |  |  |
| DAW-11 | Switch sample rate 44.1/48/96 kHz. | Reprepare is clean. |  |  |
| DAW-12 | Switch buffer 32/64/128/256/512. | Stable or unsupported cleanly. |  |  |
| DAW-13 | Play/stop transport repeatedly. | No hang; tails remain stable. |  |  |
| DAW-14 | Offline render/bounce. | Render completes and file is valid. |  |  |
| DAW-15 | Add multiple NOVA instances. | Host remains stable; project reopens. |  |  |
| DAW-16 | Remove NOVA while audio runs. | Host remains stable. |  |  |
| DAW-17 | Reopen after corrupt/crash scenario if applicable. | Host/project recovery is clean or issue is captured. |  |  |

## Failure Capture

For every FAIL:

- Record exact step ID.
- Save host project before and after if possible.
- Attach `session-log.txt`, host log, crash dump/minidump, screenshots, and render file if relevant.
- Mark severity:
  - P0: crash, blacklist, project cannot reopen, audio runaway.
  - P1: state loss, repeatable corrupt restore, automation crash.
  - P2: cosmetic host behavior, missing diagnostic evidence, non-blocking workaround.

## Final Result

Standalone: PASS / WARN / FAIL / NOT RUN
Reaper: PASS / WARN / FAIL / NOT RUN
Ableton Live: PASS / WARN / FAIL / NOT RUN
Cubase: PASS / WARN / FAIL / NOT RUN
FL Studio: PASS / WARN / FAIL / NOT RUN
Logic Pro: NOT APPLICABLE / PASS / WARN / FAIL / NOT RUN

Overall recommendation:
