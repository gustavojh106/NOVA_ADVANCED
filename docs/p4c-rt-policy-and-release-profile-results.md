# P4C RT Policy And Release Profile Results

Fecha: 2026-04-29

## Resumen

P4C dejo endurecida la policy RT con un scanner reutilizable, agrego baseline de profiling Release x64 y cerro la auditoria del riesgo `getPlayHead()->getPosition()` con una mitigacion minima de bajo riesgo.

Resultado general:

- Base validation: PASS.
- Golden metrics P4: PASS.
- RT profile Debug: PASS con WARN (`16 total / 14 pass / 2 warn / 0 fail`).
- RT profile Release: PASS (`16 total / 16 pass / 0 warn / 0 fail`).
- Policy scan: WARN no bloqueante (solo legacy no activos), sin FAIL en rutas activas.

## Archivos modificados en P4C

- `scripts/check-audio-thread-policy.ps1` (nuevo)
- `scripts/run-rt-profile-scenarios.ps1`
- `Source/Core/PluginProcessor.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/OfflineQADiagnostics.h`
- `Source/Core/PedalCatalog.h`
- `docs/p4c-rt-policy-hardening.md`
- `docs/rt-profile/p4c-rt-profile-release-baseline.json`
- `docs/p4c-rt-policy-and-release-profile-results.md`

## Policy scan script creado

Script:

- `scripts/check-audio-thread-policy.ps1`

Salida:

- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`

Comportamiento:

- `FAIL` si aparece patron peligroso en rutas activas (`processBlock`, `AudioEngine::process`, `AudioEngine::processWithSampleAccurateDryWet`) de archivos activos.
- `WARN` para hallazgos allowlist o legacy clasificados como no activos.
- allowlist explicita por `file + patternId + lineRegex + reason`.

## Resultado del policy scan

Ejecucion:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1
```

Resultado:

- `status=WARN`
- `activeFiles=29`
- `activeRanges=30`
- `failures=0`
- `legacyWarnings=4`
- `allowlistedWarnings=0`

Interpretacion:

- No se detectaron patrones bloqueantes en rutas activas.
- El WARN viene unicamente de headers legacy fuera de ruta activa registrada.

## Legacy headers clasificados

- `Source/Effects/Pedals/ChorusPedal.h`: `legacy-no-active-reference` (`inJucer=false`, `inRegistry=false`)
- `Source/Effects/Pedals/CompressorPedal.h`: `legacy-no-active-reference` (`inJucer=false`, `inRegistry=false`)
- `Source/Effects/Pedals/Wah/AutoWahPedal.h`: `legacy-in-jucer` (`inJucer=true`, `inRegistry=false`)
- `Source/Effects/Pedals/Metal/MetalDistortionPedal.h`: `legacy-in-jucer` (`inJucer=true`, `inRegistry=false`)

Guard aplicado:

- Se mantuvo nota explicita en `PedalCatalog` para impedir registro directo de clases legacy sin modernizacion P1.

## Estado de getPlayHead()->getPosition()

Estado auditado:

- La ruta `processBlock -> refreshEngineGlobalParamsIfNeeded(false, false)` podia consultar playhead cada bloque en P4B.

Mitigacion minima aplicada en P4C:

- Poll de host transport cada 8 bloques (`kHostTransportPollIntervalBlocks = 8`).
- En bloques intermedios se reutiliza snapshot atomico de estado previo.
- `force=true` sigue forzando refresh inmediato.
- Contador reseteado en `prepareToPlay`.

Resultado:

- Menor frecuencia de consulta host en audio callback sin refactor grande de `AudioEngine`.
- No hubo cambios DSP/tono/IDs/schema.

## Profiling Debug

Ejecucion:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -ReportPath artifacts/rt-profile-debug-x64-report.json
```

Resultado:

- `total=16 pass=14 warn=2 fail=0`
- WARN en:
  - `stress_block_32`: `maxBudgetRatio=0.767` (pico >75%)
  - `sample_rate_96000`: `maxBudgetRatio=1.422`, `cpuAvg=90.71%`, `cpuPeak=142.18%`
- Sin `NaN/Inf`, sin hard clipping, sin fallback blocks, sin limiter touched samples.

## Profiling Release

Ejecucion:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report.json -UpdateBaseline
```

Resultado:

- `total=16 pass=16 warn=0 fail=0`
- `sample_rate_96000`: `status=PASS`, `maxBudgetRatio=0.270`, `cpuAvg=19.49%`, `cpuPeak=27.05%`
- Baseline Release actualizada en:
  - `docs/rt-profile/p4c-rt-profile-release-baseline.json`

## Comparacion 96 kHz Debug vs Release

- Debug 96 kHz: WARN con picos por encima de presupuesto (`maxBudgetRatio=1.422`).
- Release 96 kHz: PASS con margen amplio (`maxBudgetRatio=0.270`).
- Conclusion: el WARN de 96 kHz queda atribuido a ruido/overhead de Debug, no a degradacion estructural del runtime Release.

## Validacion final ejecutada

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin
git diff --check
powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120
powershell -ExecutionPolicy Bypass -File scripts\run-golden-audio-metrics.ps1
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -ReportPath artifacts/rt-profile-debug-x64-report.json
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report.json -UpdateBaseline
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1
```

Resultados:

- `NOVA_SharedCode Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_SharedCode Release x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Release x64`: PASS, 0 warnings, 0 errors.
- `run-base-audio-validation.ps1`: PASS, `results=136 passes=5758 failures=0 failingResults=0`.
- `run-golden-audio-metrics.ps1`: PASS contra baseline P4.
- `run-rt-profile-scenarios.ps1` Debug: PASS con WARN.
- `run-rt-profile-scenarios.ps1` Release: PASS sin WARN.
- `check-audio-thread-policy.ps1`: WARN no bloqueante, sin FAIL.
- `git diff --check`: PASS (solo avisos LF/CRLF, sin whitespace errors).

## Riesgos restantes

- Persisten headers legacy con patrones no-RT-safe, aunque no estan en rutas activas registradas.
- `getPlayHead()->getPosition()` sigue en audio path, aunque ahora con polling amortiguado; hosts especificos podrian requerir arquitectura de snapshot mas estricta.
- Debug profiling sigue sensible a scheduler/noise local, por diseno no se usa como puerta de bloqueo estricta.

## Recomendacion para P5

- Llevar `check-audio-thread-policy.ps1` a gating CI (con opcion `-FailOnWarn` para ramas endurecidas).
- Disenar en P5 un snapshot de transport host completamente desacoplado de audio callback si se busca hard RT policy.
- Mantener baseline Release como referencia principal de performance y usar Debug solo para diagnostico.
- No permitir entrada de pedales legacy al registry hasta pasar checklist P1 de no-allocation/lock/logging en `processBlock`.

