# P4A Golden Metrics Coverage Completion

Fecha: 2026-04-29
Commit base: `da11290f86fcbda228128143b89e2da0f4de97cc`

## Objetivo

Cerrar los dos coverage gaps de P3 en golden metrics/offline QA sin tocar DSP, UI, wizards, IDs de parámetros ni schema de presets.

## Escenarios agregados

1. `output_chain_biased_dc_cleanup`
- Caso golden: `output_chain_biased_input_dc_cleanup`
- Implementación: escenario offline directo sobre `OutputChainProcessor` con señal seno + DC offset.
- Métricas capturadas:
  - `input_peak`
  - `input_dc`
  - `output_peak`
  - `output_dc`
  - `dc_reduction_ratio`
  - `output_rms`
  - `output_early_rms`
  - `output_late_rms`
  - `rms_stability_ratio`
  - `limiterTouchedSamples` (expuesto por snapshot de telemetría)
  - `limiterMaxReductionDb` (expuesto por snapshot de telemetría)
  - `finite`

2. `overdrive_cleanamp_reverb_chain_nominal`
- Caso golden: `overdrive_cleanamp_reverb_chain_nominal`
- Cadena nominal:
  - Input test signal
  - Overdrive V2 MusicalSafe
  - CleanAmp
  - Reverb
  - OutputChain (vía `AudioEngine`)
- Métricas capturadas:
  - `input_peak`
  - `output_peak`
  - `output_rms`
  - `output_dc`
  - `finite`
  - `near_clip_count`
  - `tail_rms`
  - `body_rms`
  - `late_tail_rms`
  - `tail_decay_ratio`

## Criterios de aceptación aplicados

### OutputChain biased DC cleanup
- salida finita
- reducción clara de DC (`output_dc < input_dc * 0.35`)
- pico en rango seguro (`0.15 < output_peak < 1.02`)
- sin silencio accidental (`output_rms > 0.08`)
- estabilidad RMS para evitar pumping excesivo (`0.70 < rms_stability_ratio < 1.30`)

### Overdrive + CleanAmp + Reverb nominal
- salida finita
- pico seguro (`0.10 < output_peak < 1.02`)
- sin silencio (`output_rms > 0.03`)
- DC controlado (`output_dc < 0.03`)
- near clip bajo (`near_clip_count < 96`)
- cola de reverb presente y decay estable (`tail_rms > 1e-4`, `tail_rms < body_rms * 0.90`, `tail_decay_ratio < 0.85`)

## Resultados obtenidos

Baseline nueva creada:
- `docs/golden-metrics/p4-offline-qa-baseline.json`

Valores destacados:
- `output_chain_biased_input_dc_cleanup`
  - `input_dc=0.18000486`
  - `output_dc=0.00003440`
  - `dc_reduction_ratio=0.00019112`
  - `output_peak=0.49830621`
  - `output_rms=0.23367227`
  - `limiterTouchedSamples=0`
  - `limiterMaxReductionDb=0`
  - `status=PASS`
- `overdrive_cleanamp_reverb_chain_nominal`
  - `input_peak=0.28328192`
  - `output_peak=0.50009149`
  - `output_rms=0.07948290`
  - `output_dc=0.00001187`
  - `near_clip_count=0`
  - `tail_rms=0.02352910`
  - `tail_decay_ratio=0.14836357`
  - `status=PASS`

## Baseline actualizada

- Se creó baseline nueva P4 (preferida para no mezclar historia de P3):
  - `docs/golden-metrics/p4-offline-qa-baseline.json`
- `coverageGaps` en baseline P4: vacío.

## Archivos tocados

- `Source/Core/OfflineQADiagnostics.h`
- `Source/Core/DSP/Global/OutputChain.h`
- `Source/Core/DSP/Global/OutputChain.cpp`
- `scripts/run-golden-audio-metrics.ps1`
- `docs/golden-metrics/p4-offline-qa-baseline.json`
- `docs/p4-golden-coverage-completion.md`

## Validación ejecutada

- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode`
- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin`
- `git diff --check`
- `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`
- `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240 -UpdateBaseline`
- `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240`

Resultado:
- `run-base-audio-validation.ps1`: PASS (`results=136 passes=5758 failures=0 failingResults=0`)
- `run-golden-audio-metrics.ps1`: PASS contra baseline P4

## Riesgos restantes

- `limiterTouchedSamples/limiterMaxReductionDb` en el escenario biased quedaron en `0` con los parámetros nominales elegidos; esto es válido para baseline actual (no hubo actividad sostenida del limiter), pero no cubre un caso de limiting intencional.
- Sigue warning residual histórico de build SharedCode (`C4100` en `AudioEngine.cpp`), fuera del alcance de P4A.

## Recomendaciones para P4B

1. Agregar un escenario adicional de limiter engagement controlado (sin clipping extremo) para cubrir actividad positiva de limiter en golden metrics.
2. Si se necesita gate más estricto de CI, correr golden metrics también en `Release x64` con baseline separada por configuración.
3. Mantener política de update controlado de baseline (`-UpdateBaseline`) solo con justificación técnica documentada.
