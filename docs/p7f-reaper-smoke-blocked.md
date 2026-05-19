# P7F Reaper Smoke Blocked

Fecha: 2026-05-06

## Resultado

P7F no pudo ejecutar el smoke test real de Reaper porque `reaper.exe` no esta instalado o no es accesible desde el entorno actual. No se inventaron resultados de host.

Estado de escenarios Reaper:

| Escenario | Estado | Motivo |
| --- | --- | --- |
| RPR-01 Scan VST3 | NOT RUN | Reaper no disponible. |
| RPR-02 Instantiate on audio track | NOT RUN | Reaper no disponible. |
| RPR-03 Save/reopen project | NOT RUN | Reaper no disponible. |
| RPR-04 Host bypass + plugin bypass | NOT RUN | Reaper no disponible. |
| RPR-05 Automation sweep | NOT RUN | Reaper no disponible. |
| RPR-06 Sample-rate and buffer switch | NOT RUN | Reaper no disponible. |
| RPR-07 Offline render | NOT RUN | Reaper no disponible. |
| RPR-08 Multi-instance | NOT RUN | Reaper no disponible. |
| RPR-09 Remove plugin during playback | NOT RUN | Reaper no disponible. |

## Preflight

Ejecutado:

- `scripts/run-host-smoke-preflight.ps1`

Resultado:

- PASS.
- Carpeta generada: `artifacts/host-smoke-preflight/20260506-152321/`.
- Base validation dentro del preflight: `results=189 passes=6520 failures=0 failingResults=0`.
- RT Release single-run dentro del preflight: PASS `16/16/0/0`.
- Policy scan dentro del preflight: `failures=0`, `contractFailures=0`, `contractChecks=142`.

## Reaper Detection

Rutas revisadas:

- `C:\Program Files\REAPER (x64)\reaper.exe`
- `C:\Program Files\REAPER\reaper.exe`
- `C:\Program Files (x86)\REAPER\reaper.exe`
- `%LOCALAPPDATA%\Programs\REAPER (x64)\reaper.exe`
- `%LOCALAPPDATA%\Programs\REAPER\reaper.exe`
- `PATH` via `Get-Command reaper.exe`

Resultado:

- `reaper.exe` no encontrado.

## Evidencia Guardada

Carpeta:

- `artifacts/host-smoke/reaper/20260506-152321/`

Contenido preparado:

- `reaper-smoke-notes.txt`
- `reports/audio-base-test-report.txt`
- `reports/audio-thread-policy-scan.txt`
- `reports/audio-thread-policy-scan.json`
- `reports/rt-profile-release-x64-report.json`
- `reports/rt-profile-release-x64-report-gate.json`
- `reports/host-smoke-preflight-summary.json`
- `reports/host-smoke-preflight-summary.txt`
- `session-log.txt` si existia en `%APPDATA%\NOVA\Logs\`.

No hay `.rpp`, render output, screenshots ni crash dumps porque Reaper no se ejecuto.

## Que Falta

Para ejecutar P7F manual o en una sesion posterior:

1. Instalar Reaper x64.
2. Configurar el VST3 path de Reaper para incluir:
   - `Builds/VisualStudio2022/x64/Release/VST3/`
3. Confirmar que el artifact existe:
   - `Builds/VisualStudio2022/x64/Release/VST3/NOVA.vst3/Contents/x86_64-win/NOVA.vst3`
4. Seleccionar audio device en Reaper.
5. Ejecutar el checklist:
   - `docs/p7e-manual-host-smoke-checklist.md`
6. Completar reporte usando:
   - `docs/p7e-host-smoke-report-template.md`

## Issues

### P3 - Reaper No Disponible

- Severidad: P3 documentation/environment.
- Repro: `reaper.exe` no existe en rutas estandar ni en `PATH`.
- Impacto: bloquea ejecucion de RPR-01..RPR-09.
- Workaround: instalar Reaper y repetir P7F.

## Validacion Local

- `git diff --check`: PASS.
- `run-host-smoke-preflight.ps1`: PASS.

## Recomendacion P7G

Instalar Reaper y repetir P7F usando la matriz P7E. No avanzar a tonal QA/factory presets con Reaper marcado como PASS hasta capturar `.rpp`, render/logs y resultados RPR-01..RPR-09 reales.
