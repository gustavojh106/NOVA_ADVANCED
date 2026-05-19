# P7I - Diagnostics / Visualizer / CPU Profiler Polish Results

Fecha: 2026-05-07

## Objetivo

Pulir la superficie de diagnostics, visualizer y CPU profiler ya extraida en P5L (`DiagnosticsManager`) y `CpuMeter` (P5+), cerrando el item P2 listado en `docs/p7a-product-hardening-audit-and-readiness-map.md` seccion 5.3 (`visualizer / per-sample atomic CPU profiling at block 32 / 64`) sin tocar DSP, schema, IDs, presets, routing ni golden baselines.

P7F/Reaper smoke continua pendiente por entorno y no se marca como PASS en esta fase.

## Alcance

- Cobertura unit-test directa de `Nova::Audio::DiagnosticsManager::formatProfilingLine`, `formatProfilingResults` y de `CpuMeter` con casos limite que antes solo se cubrian indirectamente desde `AudioEngine`.
- Aseveracion explicita de que `AudioEngine::runRealtimeProfilingSuite` cubre los block sizes 32 y 64 que P7A pedia auditar.
- Aseveracion de la salud de `AudioEngine::buildDiagnosticReport` (campos estables presentes en el reporte).
- Tooling de agregacion de diagnostics: `scripts/run-diagnostics-bundle.ps1`.
- Policy contract checks que codifican la superficie diagnostica para que no pueda regresar.
- Documento de cierre P7I.

Fuera de alcance:
- Reaper/DAW smoke real (P7F sigue pendiente).
- UI/UX visual final.
- Tonal QA, factory presets, brand work.
- DistortionPedal surgery, Reverb refactor, AudioEngine refactor.
- Release engineering (signing, installer, crash reporter).

## Archivos modificados

- `Source/Core/AudioEngineTests.cpp`
  - Anade includes a `Audio/CpuMeter.h` y `Audio/DiagnosticsManager.h`.
  - Anade la clase `P7IDiagnosticsProfilerTests` con 8 `beginTest` cases.
  - Registro estatico de la nueva suite y wiring en `touchAudioEngineValidationTests`.

- `scripts/check-audio-thread-policy.ps1`
  - Nueva seccion P7I con contract checks (no se eliminan checks previos).

- `scripts/run-diagnostics-bundle.ps1` (nuevo)
  - Read-only sobre artifacts existentes; escribe `artifacts/diagnostics-bundle.json`.
  - No toca DSP, schema, golden baselines ni audio thread.

- `docs/p7i-diagnostics-visualizer-cpu-profiler-polish-results.md` (este documento, nuevo).

No se tocan headers DSP, no se tocan amps, pedales, cabinets, AudioEngine.cpp ni AudioEngine.h.

## Tests agregados

En `Source/Core/AudioEngineTests.cpp`, suite `P7I Diagnostics / Profiler`:

- `P7I CpuMeter reset clears all counters`
- `P7I CpuMeter ignores invalid sample rate / block size without polluting state`
- `P7I CpuMeter peak decays toward floor across many empty blocks`
- `P7I DiagnosticsManager::formatProfilingLine produces deterministic shape`
- `P7I DiagnosticsManager::formatProfilingLine appends notes when present`
- `P7I DiagnosticsManager::formatProfilingResults joins lines and includes header`
- `P7I runRealtimeProfilingSuite covers blocks 32 and 64 explicitly`
- `P7I AudioEngine::buildDiagnosticReport is non-empty and contains stable fields`

Cobertura:

- `CpuMeter::reset()` deja todos los acumuladores en 0.
- `CpuMeter::endBlock` no contamina estado con sample rate o block size invalidos.
- El peak de `CpuMeter` no crece sin cota en estado estable.
- `formatProfilingLine` emite `block=`, `blocks=`, `avgMs=`, `peakMs=`, `avgCpu=`, `peakCpu=`, `passed=true` y omite `notes=` cuando estan vacias.
- `formatProfilingLine` anade `notes=...` cuando hay contenido y refleja `passed=false` para fallidos.
- `formatProfilingResults` emite la cabecera `AudioEngine realtime profiling results:` y una linea por resultado.
- `runRealtimeProfilingSuite` cubre block 32 y block 64 (P7A 5.3) con `processedBlocks > 0` y todas las metricas finitas.
- `buildDiagnosticReport` no esta vacio y contiene los campos estables (`engineOn=`, `sampleRate=`, `blockSize=`, `cpuLoad=`, `autoHealCount=`).

## Policy checks agregados

`scripts/check-audio-thread-policy.ps1` agrega checks `p7i_*`:

- `p7i_diagnostics_polish_doc_present` - doc P7I presente.
- `p7i_diagnostics_bundle_script_present` - bundle script presente.
- `p7i_diagnostics_manager_format_profiling_line_present` - `formatProfilingLine` presente en `DiagnosticsManager.h`.
- `p7i_diagnostics_manager_format_profiling_results_present` - `formatProfilingResults` presente.
- `p7i_diagnostics_manager_build_report_present` - `buildDiagnosticReport` presente.
- `p7i_runtime_profiling_covers_block_32` - el array `blockSizes` de `runRealtimeProfilingSuite` incluye 32.
- `p7i_runtime_profiling_covers_block_64` - el array `blockSizes` incluye 64.
- `p7i_diagnostics_test_present` (uno por nombre de test P7I, total 8).

Resultado actual del scan en cierre P7I: `contractChecks=170`, `contractFailures=0`. Sube respecto al baseline P7H (`contractChecks=155`).

## Tooling P7I

`scripts/run-diagnostics-bundle.ps1`:

- Read-only sobre `artifacts/rt-profile-release-x64-report.json`, `artifacts/rt-profile-stability-release-x64.json`, `artifacts/audio-thread-policy-scan.json` y `audio-base-test-report.txt`.
- Escribe `artifacts/diagnostics-bundle.json` con un resumen agregado: RT profile, RT stability, policy scan, base validation.
- No corre el audio engine, no muta DSP, no toca presets ni golden baselines.
- Bandera `-FailOnMissing` para CI estricto; por defecto se reporta lo presente y se lista lo que falta.

## Validacion

Validacion completa P7I ejecutada:

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings.
- `NOVA_SharedCode` Release x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings.
- `NOVA_VST3` Release x64: PASS, 0 warnings.
- `run-base-audio-validation.ps1` primera corrida: PASS, `results=201`, `passes=6600`, `failures=0`, `failingResults=0`.
- `run-base-audio-validation.ps1` segunda corrida: PASS, `results=201`, `passes=6600`, `failures=0`, `failingResults=0`.
- `run-golden-audio-metrics.ps1`: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`.
- `run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `16/16/0/0`.
- `run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`.
- `check-audio-thread-policy.ps1`: PASS, `failures=0`, `warnings=0`, `legacyWarnings=0`, `legacyQuarantined=4`, `contractFailures=0`, `contractChecks=170`.
- `run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
- `run-diagnostics-bundle.ps1`: PASS, escribe `artifacts/diagnostics-bundle.json` con todos los inputs presentes.

Delta vs P7H:

- Base validation: `results` 193 -> 201 (+8 = nuevos beginTest P7I); `passes` 6533 -> 6600 (+67 expectations).
- Policy scan: `contractChecks` 155 -> 170 (+15); `contractFailures` 0 -> 0.
- Sin cambios en golden metrics, RT profile budgets, RT stability gating ni schema.

## Riesgos restantes

- `runRealtimeProfilingSuite` corre desde test thread sobre el mismo `AudioEngine`; sigue siendo una superficie de profiling sintetica. P7I la fija en contract pero no introduce profiler en proceso vivo.
- `DiagnosticsManager::buildDiagnosticReport` sigue construyendo `juce::String` y solo se debe invocar fuera del audio thread; las policy ranges siguen prohibiendo `juce::String` en `processBlock`.
- El bundle script depende de artifacts producidos por otros gates; no los regenera. Es agregador, no orquestador.

## Recomendacion para P7J

Si se quiere extender la polish, las siguientes piezas siguen abiertas:
- Visualizer en GUI (meter UI, CPU graph) con polling 30 Hz desde message thread, leyendo solamente atomicos. P7I codifica los acumuladores; un widget consumidor seria P7J.
- Diagnostic export desde el editor (boton "Save diagnostics bundle") apoyado en `run-diagnostics-bundle.ps1`.
- Per-pedal CPU breakdown en `runRealtimeProfilingSuite` por nodo del graph; requiere tocar AudioEngine y queda fuera del scope diagnostics-only.

P7F/Reaper smoke sigue pendiente y debe cerrarse en su propia fase con entorno estable.
