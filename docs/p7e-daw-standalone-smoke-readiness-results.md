# P7E DAW / Standalone Smoke Readiness Results

Fecha: 2026-05-06

## Resumen

P7E prepara la matriz de smoke tests para Standalone/VST3/DAW y agrega un puente minimo de host-state validation. No se hicieron cambios DSP, tonales, AudioEngine, dry/wet, routing, graph lifecycle, UI/UX, factory presets, schema/IDs ni golden baselines.

## Archivos creados/modificados

- `docs/p7e-daw-standalone-smoke-matrix.md`
- `docs/p7e-manual-host-smoke-checklist.md`
- `docs/p7e-host-smoke-report-template.md`
- `docs/p7e-daw-standalone-smoke-readiness-results.md`
- `scripts/run-host-smoke-preflight.ps1`
- `scripts/check-audio-thread-policy.ps1`
- `Source/Core/AudioEngineTests.cpp`

## Matriz creada

`docs/p7e-daw-standalone-smoke-matrix.md` cubre:

- Standalone launch, device select, sample rates 44.1/48/96 kHz, buffers 32/64/128/256/512.
- Engine on/off, add/remove/bypass, save/load session, corrupt state recovery.
- Multi-minute idle/play y visibilidad CPU/process-time.
- VST3/DAW scan, instantiate, host project save/load, close/reopen, host bypass, plugin bypass.
- Automation sweep, preset/state save/load, sample-rate switch, buffer switch, transport, offline render, multi-instance, remove while audio running y recovery.
- Priorizacion: Reaper primero; Ableton Live, Cubase y FL Studio si disponibles; Logic solo macOS.

## Checklist creada

`docs/p7e-manual-host-smoke-checklist.md` incluye:

- Preconditions.
- Campos de version/build/host/device/sample-rate/buffer.
- Pasos Standalone y DAW.
- Resultado esperado.
- Campos PASS/FAIL.
- Evidencia requerida: `session-log.txt`, reports, screenshots opcionales, RT artifacts, host project, render, crash/minidump.

## Template creado

`docs/p7e-host-smoke-report-template.md` permite registrar:

- build hash, branch, dirty files y schema version.
- OS, host, audio device, sample rates y buffers.
- scenarios passed/failed/warn/not run.
- issue reproduction, severity, evidencia adjunta y recomendacion.

## Script creado

`scripts/run-host-smoke-preflight.ps1` es deliberadamente seguro:

- Verifica artifact de Standalone.
- Verifica bundle/binario VST3.
- Ejecuta `run-audio-quality-gates.ps1 -Fast`.
- Copia reports/logs a `artifacts/host-smoke-preflight/<timestamp>/`.
- Genera summary `.json` y `.txt`.
- No abre DAWs.
- No usa UI automation.
- No modifica proyectos de host.

## Tests host-state agregados

Se agrego:

- `P7E host state get/set survives corrupt and repeated engine toggles`

Cobertura:

- `setStateInformation` corrupto no crashea.
- `getStateInformation` despues de corrupt restore produce `NOVA_STATE` valido con schema actual.
- `prepare/process` despues de setStateInformation host-like produce salida finita.
- `setStateInformation` repetido alternando engine off/on mantiene serializacion canonica y audio finito.

## Policy checks agregados

`scripts/check-audio-thread-policy.ps1` conserva checks previos y agrega:

- docs P7E presentes.
- preflight script presente.
- test host-state P7E presente.
- schema bump sigue bloqueado por el check P7D `p7d_schema_version_unchanged`.

## Validacion

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings.
- `NOVA_SharedCode` Release x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings.
- `NOVA_VST3` Release x64: PASS, 0 warnings.
- `git diff --check`: PASS.
- Base validation Debug x64: PASS dos corridas consecutivas, `results=189 passes=6520 failures=0 failingResults=0`.
- Golden metrics: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`; no baseline update.
- RT profile Release: PASS `16/16/0/0`.
- RT profile stability Release `-CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`.
- Policy scan: `failures=0`, `contractFailures=0`, `contractChecks=142`.
- Wrapper Fast Release: PASS.
- Host smoke preflight: PASS, artifact en `artifacts/host-smoke-preflight/20260506-122814`.

## Que queda manual

- Reaper scan/instantiate/save/reopen/render/multi-instance.
- Ableton Live, Cubase y FL Studio solo si estan instalados.
- Logic Pro no aplica al entorno Windows actual.
- Audio device selection real en Standalone.
- Evidencia de host project files, screenshots, host logs y crash dumps si aparecen.

## Riesgos restantes

- La automatizacion DAW por UI no se implemento porque seria fragil sin infraestructura confiable por host.
- El preflight valida artifacts y gates internos, pero no reemplaza pruebas reales en host.
- Las diferencias entre hosts pueden aparecer en scan, automation, offline render o project restore aunque Standalone y UnitTests pasen.

## Recomendacion P7F

P7F deberia ejecutar Reaper como primer host real, guardar project files de evidencia y clasificar cualquier fallo por severidad. Mantener fuera UI/UX, tonal QA, factory preset curation y release engineering profundo hasta cerrar al menos Reaper smoke.
