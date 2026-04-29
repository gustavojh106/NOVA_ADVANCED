# P4B RT Profiling Plan

Fecha: 2026-04-29

## Objetivo

Agregar una verificacion runtime offline de seguridad real-time para escenarios criticos de NOVA, sin cambiar DSP, tono, UI, wizards, IDs de parametros ni preset schema.

## Que queremos medir

- Tiempo promedio de `AudioEngine::process()` por bloque.
- Tiempo pico de `AudioEngine::process()` por bloque.
- CPU estimado por bloque: `elapsedMs / blockBudgetMs`.
- Max block budget ratio.
- Conteo de bloques sobre 50%, 75%, 90% y 100% del budget.
- Samples invalidos (`NaN`/`Inf`) y clipped.
- Near-clip samples cuando el helper exista.
- Denormal risk de salida por aproximacion numerica.
- Actividad de limiter cuando este expuesta de forma barata.
- Fallback counts cuando los procesadores los expongan de forma barata.

## Que no podemos medir con certeza desde C++ estandar/JUCE

- Todas las allocations del heap sin reemplazar `operator new`, usar hooks del CRT, ETW, Instruments, Tracy, VerySleepy, Visual Studio Profiler u otra herramienta externa.
- Locks internos de JUCE o del host que no esten visibles en el codigo del proyecto.
- Page faults, cambios de prioridad del scheduler, DPC latency, interrupciones del driver o preemption del SO.
- Coste exacto de llamadas SIMD/denormal handling en hardware distinto.
- Logging externo que ocurra fuera del proceso o dentro de APIs de terceros.

## Estrategia para detectar allocations runtime

- P4B implementa una aproximacion runtime sin hooks globales: escenarios offline repiten `AudioEngine::process()` con buffers prealocados y miden spikes/budget overruns.
- P4B mantiene un scan estatico de patrones peligrosos dentro de `processBlock()` y rutas de audio.
- La verificacion fuerte de heap allocations queda para herramienta externa/manual:
  - Visual Studio Diagnostic Tools / Performance Profiler.
  - ETW/WPA heap allocation provider.
  - CRT debug heap hooks en build diagnostica dedicada.
  - Tracy/Remotery si se acepta instrumentacion externa en P5.

## Estrategia para detectar locks/logging desde audio thread

- Scan estatico de patrones:
  - `SessionLogger::logEvent`
  - `juce::String` construido para logging
  - `juce::SpinLock::ScopedLockType`
  - `std::mutex`, `std::lock_guard`
  - `WaitableEvent`
  - `AudioBuffer::setSize` en `processBlock`
  - `std::vector::resize` / `push_back` en `processBlock`
  - `dynamic_cast` en `processBlock`
  - `IIR::Coefficients<float>::make*` en `processBlock`
  - `setValueNotifyingHost` en `processBlock`
  - graph rebuild en `processBlock`
- Clasificar coincidencias como peligro real, falsa alarma o requiere revision.
- No corregir refactors grandes en P4B; documentar para P4C/P5.

## Estrategia para CPU/process-time spikes

- Nuevo runner offline/test-only activado por variable de entorno.
- El runner prepara escenarios con `AudioEngine`, prealoca buffers y mide alrededor de `engine.process(...)`.
- Las mediciones y serializacion ocurren fuera de la ruta de audio real.
- Criterios por niveles:
  - `FAIL`: invalid samples, clipped hard limit, o bloques sostenidos por encima de 100% del budget.
  - `WARN`: peak budget ratio > 75%, cualquier bloque > 90%, near-clip alto, denormal risk sostenido, limiter activity inesperada en clean scenarios.
  - `PASS`: sin fallos y sin warnings.

## Escenarios a perfilar

- `clean_empty_chain`
- `clean_line_a`
- `dual_parallel_clean`
- `overdrive_v2_nominal`
- `overdrive_cleanamp_reverb_chain_nominal`
- `high_gain_amp_nominal`
- `cabinet_nominal`
- `delay_feedback_nominal`
- `reverb_cloud_tail`
- `reverb_reverse_swell`
- `stress_block_32`
- `stress_block_64`
- `stress_block_512`
- `sample_rate_44100`
- `sample_rate_48000`
- `sample_rate_96000`

## Riesgos de falsos positivos

- Debug x64 puede exagerar tiempos respecto a Release.
- Antivirus, OneDrive, scheduler y carga de sistema pueden generar picos no reproducibles.
- Primeros bloques tras prepare/rebuild pueden incluir warm-up/cache effects; deben excluirse con warm-up.
- Medir con `Time::getMillisecondCounterHiRes()` tiene overhead y jitter propio.
- Cambios de CPU frequency scaling pueden mover ratios sin cambio de codigo.

## Riesgos de overhead de instrumentacion

- Medicion por bloque agrega llamadas de reloj; solo debe correr offline/test diagnostics.
- Reportes JSON/string deben construirse fuera del loop medido o despues del escenario.
- No se debe introducir logging ni asignaciones dentro de `AudioEngine::process()` o `processBlock()`.
- No se debe publicar telemetry pesada desde la ruta medida salvo contadores ya existentes.

## Implementacion P4B

- Agregar runner offline activado por `NOVA_RUN_RT_PROFILE=1`.
- Escribir reporte JSON a `NOVA_RT_PROFILE_REPORT_PATH`.
- Crear script `scripts/run-rt-profile-scenarios.ps1`.
- Crear baseline `docs/rt-profile/p4b-rt-profile-baseline.json`.
- Mantener `run-base-audio-validation.ps1` y golden metrics sin cambios de baseline salvo que la validacion lo requiera.

## Herramientas externas/manuales para fases posteriores

- ETW/WPA para heap allocations, thread scheduling y DPC latency.
- Visual Studio Profiler para CPU sampling e allocation stacks.
- Hook global de allocation en build diagnostica separada.
- Audio callback profiler con prioridad real de driver/host si se valida en DAW.
