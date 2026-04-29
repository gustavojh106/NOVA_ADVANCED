# P4B RT Profiling Results

Fecha: 2026-04-29

## Resumen

P4B implemento una verificacion runtime offline/test-only para perfilar escenarios criticos de audio sin cambiar DSP, tono, UI, wizards, IDs de parametros ni preset schema.

Resultado principal:

- Base validation: PASS.
- Golden metrics P4: PASS.
- RT profile baseline: 16 escenarios, 15 PASS, 1 WARN, 0 FAIL.
- No samples `NaN`/`Inf`.
- No hard clipping.
- No fallback blocks.
- No limiter activity en los escenarios perfilados.
- No denormal-like samples.

## Archivos modificados o agregados por P4B

- `Source/Core/OfflineQADiagnostics.h`
- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/PluginProcessor.cpp`
- `scripts/run-rt-profile-scenarios.ps1`
- `docs/p4b-rt-profiling-plan.md`
- `docs/p4b-audio-thread-policy-scan.md`
- `docs/p4b-rt-profiling-results.md`
- `docs/rt-profile/p4b-rt-profile-baseline.json`

Nota: el worktree ya contenia cambios de P0/P1/P2/P3/P4A antes de esta fase. P4B no los revierte.

## Implementacion

- Se agrego `OfflineQADiagnostics::runRtProfileAndWriteReport()`.
- El runner se activa solo con `NOVA_RUN_RT_PROFILE=1`.
- El path del reporte se controla con `NOVA_RT_PROFILE_REPORT_PATH`.
- La medicion se ejecuta fuera del audio callback real de usuario, con buffers prealocados por escenario.
- La serializacion JSON ocurre al terminar los escenarios, no dentro del loop medido.
- Se agrego `AudioEngine::getOutputChainDebugSnapshot()` para leer actividad de limiter ya expuesta por `OutputChainProcessor`.
- Se agrego `scripts/run-rt-profile-scenarios.ps1` para build, ejecucion, parseo, baseline update y comparacion basica contra baseline.
- Se elimino un warning trivial C4100 dejando sin nombre el parametro no usado de `AudioEngine::processWithSampleAccurateDryWet`; no cambia comportamiento ni DSP.

## Metricas capturadas

Cada escenario reporta:

- `status`
- `sampleRate`
- `blockSize`
- `processedBlocks`
- `avgProcessMs`
- `peakProcessMs`
- `cpuAvgPercent`
- `cpuPeakPercent`
- `maxBudgetRatio`
- `blocksOver50`
- `blocksOver75`
- `blocksOver90`
- `blocksOver100`
- `invalidSamples`
- `clippedSamples`
- `nearClipSamples`
- `denormalLikeSamples`
- `fallbackBlockCount`
- `limiterTouchedSamples`
- `limiterMaxReductionDb`
- `inputPeak`
- `outputPeak`
- `warnings`

## Criterios PASS/WARN/FAIL

- FAIL si hay `NaN`/`Inf`.
- FAIL si hay hard clipping.
- FAIL si hay bloques sostenidos sobre 100% del block budget.
- WARN si `maxBudgetRatio > 0.75`.
- WARN si algun bloque supera 90% del budget.
- WARN si `fallbackBlockCount > 0`.
- WARN si aparecen denormal-like samples.
- WARN si aparece limiter activity en escenarios clean/nominal donde se espera cero.

La baseline inicial no usa thresholds agresivos contra microvariaciones; el script solo alerta si el promedio de CPU sube de forma amplia frente a baseline.

## Escenarios perfilados

| Escenario | Status | SR | Block | Avg ms | Peak ms | CPU avg | CPU peak | Max budget | Notas |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `clean_empty_chain` | PASS | 48000 | 128 | 0.0726 | 0.0866 | 2.72% | 3.25% | 0.032 |  |
| `clean_line_a` | PASS | 48000 | 128 | 0.0751 | 0.0976 | 2.82% | 3.66% | 0.037 |  |
| `dual_parallel_clean` | PASS | 48000 | 128 | 0.0844 | 0.1020 | 3.16% | 3.83% | 0.038 |  |
| `overdrive_v2_nominal` | PASS | 48000 | 128 | 0.5374 | 0.5879 | 20.15% | 22.05% | 0.220 |  |
| `overdrive_cleanamp_reverb_chain_nominal` | PASS | 48000 | 128 | 1.1068 | 1.2034 | 41.51% | 45.13% | 0.451 |  |
| `high_gain_amp_nominal` | PASS | 48000 | 128 | 0.3593 | 0.3768 | 13.48% | 14.13% | 0.141 | `nearClipSamples=19168`, no hard clip, no limiter |
| `cabinet_nominal` | PASS | 48000 | 128 | 0.3289 | 0.5207 | 12.33% | 19.53% | 0.195 |  |
| `delay_feedback_nominal` | PASS | 48000 | 128 | 0.1853 | 0.2220 | 6.95% | 8.32% | 0.083 |  |
| `reverb_cloud_tail` | PASS | 48000 | 128 | 0.3039 | 0.3528 | 11.40% | 13.23% | 0.132 |  |
| `reverb_reverse_swell` | PASS | 48000 | 128 | 0.3488 | 0.3675 | 13.08% | 13.78% | 0.138 |  |
| `stress_block_32` | PASS | 48000 | 32 | 0.2930 | 0.3277 | 43.94% | 49.16% | 0.492 |  |
| `stress_block_64` | PASS | 48000 | 64 | 0.5668 | 0.6130 | 42.51% | 45.97% | 0.460 |  |
| `stress_block_512` | PASS | 48000 | 512 | 4.4505 | 4.5503 | 41.72% | 42.66% | 0.427 |  |
| `sample_rate_44100` | PASS | 44100 | 128 | 1.1027 | 1.1349 | 37.99% | 39.10% | 0.391 |  |
| `sample_rate_48000` | PASS | 48000 | 128 | 1.0918 | 1.1195 | 40.94% | 41.98% | 0.420 |  |
| `sample_rate_96000` | WARN | 96000 | 128 | 1.1066 | 1.1812 | 83.00% | 88.59% | 0.886 | Peak budget ratio > 75% |

Baseline guard totals:

- `invalidSamples=0`
- `clippedSamples=0`
- `fallbackBlockCount=0`
- `limiterTouchedSamples=0`
- `denormalLikeSamples=0`

## Policy scan

Creado: `docs/p4b-audio-thread-policy-scan.md`.

Resultado:

- No se encontro `SessionLogger::logEvent` directo en `processBlock` o `AudioEngine::process`.
- No se encontraron locks directos dentro de `AudioEngine::process`.
- No se encontro `setValueNotifyingHost` dentro de `processBlock`.
- No se encontro `dynamic_cast` dentro de rutas activas de audio.
- No se encontro graph rebuild directo desde `processBlock`.
- No se encontro `AudioBuffer::setSize` dentro de `processBlock` de procesadores registrados activos.

Riesgos documentados:

- Headers legacy no registrados con `setSize` en `processBlock`: `Source/Effects/Pedals/ChorusPedal.h`, `Source/Effects/Pedals/CompressorPedal.h`.
- Headers presentes en `NOVA.jucer` pero no registrados como tipos activos con `setSize` en `processBlock`: `Source/Effects/Pedals/Wah/AutoWahPedal.h`, `Source/Effects/Pedals/Metal/MetalDistortionPedal.h`.
- `applyHostTransportState()` consulta `getPlayHead()->getPosition()` durante el refresh de parametros en `processBlock`; no mostro locks locales, pero depende de host/JUCE y merece auditoria estricta en P4C si se busca una RT policy mas fuerte.

## Comandos ejecutados

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120
powershell -ExecutionPolicy Bypass -File scripts\run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -UpdateBaseline
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240
git diff --check
```

Resultados:

- `NOVA_SharedCode Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Debug x64`: PASS, 0 warnings, 0 errors.
- `run-base-audio-validation.ps1`: PASS, `results=136 passes=5758 failures=0 failingResults=0`.
- Validation groups: Core=0, P1 Pedal Safety=0, Reverb=0, Routing=0, OutputChain=0, AudioEngine=0, Regression=0.
- `run-golden-audio-metrics.ps1`: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`; coverage gaps tracked vacio.
- `run-rt-profile-scenarios.ps1`: PASS de script, summary `total=16 pass=15 warn=1 fail=0`.
- `git diff --check`: PASS. Git reporto solo avisos de normalizacion LF/CRLF, sin whitespace errors.

## Cambios que no se hicieron

- No se tocaron algoritmos DSP ni constantes tonales.
- No se tocaron UI/wizards.
- No se cambiaron IDs de parametros.
- No se cambio preset schema.
- No se cambio golden metrics baseline P4.
- No se agregaron known failures.
- No se corrigieron riesgos legacy que requieren limpieza/refactor fuera de P4B.

## Riesgos pendientes

- La verificacion de allocations es indirecta. No hay hook global de heap ni prueba ETW/CRT en P4B.
- Los tiempos son Debug x64 y pueden variar por scheduler, antivirus, OneDrive y frecuencia de CPU.
- La baseline de `sample_rate_96000` queda en WARN por pico de budget >75%; no hay FAIL ni overrun sostenido.
- Los headers legacy con allocation en `processBlock` deben permanecer fuera del registry hasta corregirse.
- La consulta de playhead desde audio thread debe revisarse si P4C quiere cerrar una policy RT estricta frente a hosts problematicos.

## Recomendacion para P4C

- Convertir el policy scan en script CI con allowlist explicita.
- Ejecutar ETW/WPA o Visual Studio allocation profiler en los escenarios P4B para confirmar heap allocations reales.
- Auditar `getPlayHead()->getPosition()` y decidir si conviene snapshot control-plane/atomic para host transport.
- Limpiar o corregir headers legacy antes de permitir nuevos registros de pedales.
- Agregar baseline Release x64 si se quiere separar rendimiento real de ruido Debug.
