# P5B CpuMeter Extraction Results

Fecha: 2026-04-29

## Resumen

Se extrajo el metering de CPU/process-time de `AudioEngine` a un modulo dedicado sin tocar DSP, routing, dry/wet, graph build/swap, health monitor, tuner, runtime params, UI, IDs ni schema.

Resultado global:

- Builds Debug/Release: PASS.
- Base validation: PASS.
- Golden metrics: PASS.
- RT profile Release: PASS (`16/16/0/0`).
- RT profile Debug: PASS con WARN (`16/14/2/0`), dentro del mismo patron esperado de P4C.
- Policy scan: WARN no bloqueante, `0` FAIL en rutas activas.

## Archivos modificados

Codigo:

- `Source/Core/Audio/CpuMeter.h` (nuevo, header-only)
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`

Documentacion:

- `docs/p5b-cpumeter-extraction-results.md` (nuevo)

Generados por validacion:

- `artifacts/rt-profile-debug-x64-report-p5b.json` (nuevo)
- `artifacts/rt-profile-release-x64-report-p5b.json` (nuevo)
- `artifacts/audio-thread-policy-scan.txt` (actualizado)
- `artifacts/audio-thread-policy-scan.json` (actualizado)
- `artifacts/p4-offline-qa-report.txt` (actualizado por golden run)

## Modulo creado

Se creo `CpuMeter` en `Source/Core/Audio/CpuMeter.h` con API:

- `reset()`
- `beginBlock()`
- `endBlock(startMs, numSamples, sampleRate)`
- `getCpuLoad()`
- `getLastProcessTimeMs()`
- `getAverageProcessTimeMs()`
- `getPeakProcessTimeMs()`

Decision de implementacion:

- Header-only por riesgo minimo y reversibilidad: evita tocar `NOVA.jucer` y evita cambios de membresia de archivos/proyecto en esta fase.

## Estado movido desde AudioEngine

Movido:

- `cpuUsage`
- `lastProcessTimeMs`
- `averageProcessTimeMs`
- `peakProcessTimeMs`
- formula de `elapsedMs` clamp
- formula de EMA promedio (`90/10`)
- formula de peak decay (`0.995`)
- formula de CPU por bloque
- reset de estos meters cuando `resetMeters == true`

Integracion en `AudioEngine`:

- `process()` ahora usa `audioPlane.cpuMeter.beginBlock()` al inicio y `audioPlane.cpuMeter.endBlock(...)` en el mismo punto logico de cierre de bloque.
- Se removio `updateRealtimeTimingMeters(...)` de `AudioEngine`.
- Getters publicos siguen delegando los mismos valores semanticos:
  - `getCpuLoad()`
  - `getLastProcessTimeMs()`
  - `getAverageProcessTimeMs()`
  - `getPeakProcessTimeMs()`

## Que quedo igual

- Misma ubicacion logica de medicion (inicio de bloque y cierre previo a cada return).
- Misma semantica para `sampleRate <= 0` o `numSamples <= 0`.
- Mismo uso de atomics con `memory_order_relaxed` para lectura/escritura de meters.
- Sin cambios en graph build/swap, dry/wet, routing, health monitor, tuner o pedales.
- Sin logging/strings/locks/allocations agregadas en audio thread.

## Validacion ejecutada

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin
git diff --check
powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120
powershell -ExecutionPolicy Bypass -File scripts\run-golden-audio-metrics.ps1
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -ReportPath artifacts/rt-profile-debug-x64-report-p5b.json
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report-p5b.json
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1
```

Resultados:

- `NOVA_SharedCode Debug x64`: PASS, `0` warnings, `0` errors.
- `NOVA_StandalonePlugin Debug x64`: PASS, `0` warnings, `0` errors.
- `NOVA_SharedCode Release x64`: PASS, `0` warnings, `0` errors.
- `NOVA_StandalonePlugin Release x64`: PASS, `0` warnings, `0` errors.
- `git diff --check`: PASS (solo avisos LF/CRLF, sin whitespace errors).
- `run-base-audio-validation.ps1`: PASS (`results=136 passes=5758 failures=0 failingResults=0`).
- `run-golden-audio-metrics.ps1`: PASS contra baseline P4.
- `run-rt-profile-scenarios.ps1` Debug: PASS con WARN (`16/14/2/0`).
- `run-rt-profile-scenarios.ps1` Release: PASS (`16/16/0/0`).
- `check-audio-thread-policy.ps1`: WARN no bloqueante, `failures=0`.

## Comparacion RT profile antes/despues

Referencia P4C:

- Debug: `16/14/2/0`
- Release: `16/16/0/0`

P5B:

- Debug: `16/14/2/0`
- Release: `16/16/0/0`

Observacion:

- No hubo cambio de estado global en Debug/Release.
- `sample_rate_96000` sigue con comportamiento esperado:
  - Debug: WARN (ruido/overhead de Debug)
  - Release: PASS (`maxBudgetRatio=0.270`)

## Riesgos restantes

- Los WARN de Debug en `stress_block_32` y `sample_rate_96000` siguen siendo sensibles al ruido de entorno.
- Policy scan mantiene WARN legacy no activos (sin FAIL activo), ya documentado en P4C.
- La verificacion de allocs en runtime sigue siendo indirecta (tema de fases posteriores).

## Recomendacion para P5C

Continuar con P5C (extraer `HealthMonitor`) manteniendo el mismo enfoque:

- extraccion mecanica
- sin cambios de comportamiento
- validacion completa P4C por fase
- sin mezclar graph/dry-wet/runtime params en la misma entrega
