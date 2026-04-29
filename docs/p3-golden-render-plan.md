# P3 Golden Render Plan

Fecha: 2026-04-29

## Estado actual

Infraestructura disponible: `OfflineQADiagnostics` + script nuevo `scripts/run-golden-audio-metrics.ps1`.

Implementado en P3:
- Baseline de métricas serializada en `docs/golden-metrics/p3-offline-qa-baseline.json`.
- Validación de drift con tolerancias controladas (sin `.wav` pesados).
- Comando baseline inicial:
  - `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -UpdateBaseline`
- Comando validación:
  - `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64`

## Cobertura de casos mínimos

Cubiertos por métricas golden en P3:
- OutputChain clean path
- Reverb Cloud tail
- Reverb swell
- Reverb reverse
- Reverb reverse + swell
- Overdrive V2 MusicalSafe nominal (baseline recall)
- Clean dry LineA
- Dual parallel clean

Pendientes (gap explícito, no oculto):
- OutputChain biased input / DC cleanup
- Overdrive + CleanAmp + Reverb chain nominal

## Dónde guardar renders/métricas

- Métricas baseline: `docs/golden-metrics/p3-offline-qa-baseline.json`
- Reporte de ejecución: `artifacts/p3-offline-qa-report.txt`

## Formato recomendado

- P3: JSON de métricas escalares por escenario (ligero y versionable).
- P4 (opcional): `.wav` cortos solo para casos con revisión auditiva humana.

## Duración, sample rate, block size

- Fuente actual (`OfflineQADiagnostics`):
  - sample rate: `48000`
  - block size: `64`
  - duración: depende de escenario (entre ~0.5s y ~1.9s típicamente)

## Métricas a comparar

- `peak`
- `rms`
- `null_rms` (cuando aplica)
- `finite`
- métricas de cola por ventanas (`late_rms`, `tail_end_rms`)
- métricas de bloom early/late para reverse/swell

Nota: `spectral centroid` aún no está cableado en P3; puede añadirse en P4 si se requiere mayor sensibilidad tímbrica.

## Tolerancias y drift

Tolerancias P3 en script:
- Métricas continuas: `max(8% relativo, 1e-4 absoluto)`
- Métricas discretas (`peak_index_*`, `finite`): tolerancia `0`

## Cómo evitar falsos negativos por floating-point drift

- Comparar contra baseline del mismo target/config (`Debug x64` en P3).
- Usar métricas agregadas por ventana (RMS/colas) en vez de sample-by-sample hard match.
- Mantener tolerancia relativa + piso absoluto.
- Evitar bloquear por casos no cubiertos: los gaps se reportan explícitamente, no se silencian.

## Proceso controlado para actualizar baseline

1. Confirmar `run-base-audio-validation.ps1` en PASS (sin ignores).
2. Ejecutar `run-golden-audio-metrics.ps1` y revisar drift.
3. Si el cambio es intencional y aprobado, regenerar baseline con `-UpdateBaseline`.
4. Documentar en changelog técnico:
   - motivo del update
   - escenarios/metrics afectados
   - impacto esperado en audio
5. Re-ejecutar validación para confirmar estabilidad post-update.

## Recomendación P4

Agregar dos escenarios offline QA nuevos para cerrar los gaps restantes:
- `output_chain_biased_input_dc_cleanup`
- `overdrive_cleanamp_reverb_chain_nominal`
