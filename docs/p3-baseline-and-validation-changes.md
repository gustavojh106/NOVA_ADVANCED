# P3 Baseline And Validation Changes

Fecha: 2026-04-29
Commit referencia: `da11290f86fcbda228128143b89e2da0f4de97cc`

## Objetivo P3

Congelar baseline de audio validada de P2 y endurecer higiene de validación/regresión sin cambios DSP tonales, sin UI/wizards, sin refactor de AudioEngine, sin cambios de IDs/schemas.

## Archivos modificados

Código/scripts:
- `scripts/run-base-audio-validation.ps1`
- `scripts/run-golden-audio-metrics.ps1` (nuevo)
- `Source/Core/AudioEngine.h`
- `Source/Effects/Pedals/Base/ProcessorBase.h`

Documentación:
- `docs/p3-audio-baseline-lock.md` (nuevo)
- `docs/p3-golden-render-plan.md` (nuevo)
- `docs/p3-warning-cleanup-notes.md` (nuevo)
- `docs/p3-baseline-and-validation-changes.md` (nuevo)
- `docs/golden-metrics/p3-offline-qa-baseline.json` (nuevo baseline serializado)

## Scripts tocados

### `run-base-audio-validation.ps1`

- Eliminada lista de known failures ignorados (Phaser/reverse+swell).
- Nuevo comportamiento: cualquier línea `FAIL |` ahora falla la ejecución.
- Agregado breakdown de fallos por grupos:
  - Core
  - P1 Pedal Safety
  - Reverb
  - Routing
  - OutputChain
  - AudioEngine
  - Regression

### `run-golden-audio-metrics.ps1` (nuevo)

- Ejecuta `OfflineQADiagnostics` en Standalone Debug.
- Extrae métricas por escenario y las serializa/valida contra baseline JSON.
- Soporta:
  - `-UpdateBaseline` para regenerar baseline controladamente.
  - modo validación con tolerancias (`8%` relativo, piso `1e-4`, métricas discretas con tolerancia `0`).
- Reporta gaps de cobertura explícitos (no los oculta).

## Known failures eliminados o mantenidos

- Eliminados del script de validación base:
  - `PhaserPedal feedback loop rejects DC accumulation under sustained bias`
  - `ReverbPedal reverse and swell create a delayed cinematic bloom`
- Mantenidos: ninguno en `run-base-audio-validation.ps1`.

## Golden renders/metrics implementados o planificados

Implementado (métricas ligeras, sin WAVs pesados):
- baseline serializada: `docs/golden-metrics/p3-offline-qa-baseline.json`
- escenarios cubiertos:
  - OutputChain clean path
  - Clean dry LineA
  - Dual parallel clean
  - Reverb Cloud tail
  - Reverb swell
  - Reverb reverse
  - Reverb reverse + swell
  - Overdrive V2 MusicalSafe nominal (recall)

Planificado (gaps pendientes para P4):
- OutputChain biased input / DC cleanup
- Overdrive + CleanAmp + Reverb chain nominal

Detalles y procedimiento: `docs/p3-golden-render-plan.md`.

## Warnings revisados

- `C4099 TempoSyncable struct/class`: corregido (forward declaration coherente).
- `C4458 latencySamples oculta miembro JUCE`: corregido (renombre de parámetro local).
- Residual observado: `C4100` (`health` no usado en `AudioEngine.cpp`) documentado en `docs/p3-warning-cleanup-notes.md`.

## Comandos ejecutados

- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode`
- `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin`
- `git diff --check`
- `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`
- `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240 -UpdateBaseline`
- `powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240`

## Resultados

- `NOVA_SharedCode` Debug x64: PASS (1 warning `C4100` residual).
- `NOVA_StandalonePlugin` Debug x64: PASS.
- `git diff --check`: PASS (solo warnings LF/CRLF del worktree).
- `run-base-audio-validation.ps1`: PASS.
  - `results=136 passes=5758 failures=0 failingResults=0`
  - breakdown grupos: todo en `0`.
- `run-golden-audio-metrics.ps1`: PASS contra baseline recién creada.

## Riesgos restantes

- Dos casos mínimos aún sin escenario offline QA dedicado (documentados como gap explícito).
- Warning `C4100` residual en SharedCode.
- Baseline golden actual está fijada a entorno/configuración P3 (`Debug x64`); conviene fijar matriz Release en P4 si será gate de CI.

## Recomendación para P4

1. Agregar escenarios offline QA para los 2 gaps de cobertura restantes.
2. Decidir si `run-golden-audio-metrics.ps1` se vuelve gate opcional de CI (Debug y/o Release).
3. Resolver warning `C4100` residual con cambio local no funcional.
4. Mantener política: ningún cambio DSP sin ejecutar base validation + golden metrics.
