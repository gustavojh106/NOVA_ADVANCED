# P7G - Legacy Warning Cleanup / Quarantine Results

Fecha: 2026-05-06

## Objetivo

Cerrar los 4 warnings legacy restantes del policy scan sin tocar DSP activo, tono, rutas, schema, presets, UI ni golden baselines.

P7F/Reaper smoke permanece pendiente por entorno y no se marca como PASS en esta fase.

## Archivos modificados

- `scripts/check-audio-thread-policy.ps1`
- `docs/audio-realtime-safety-audit.md`
- `docs/pedal-audit-matrix.md`
- `docs/p7g-legacy-warning-cleanup-results.md`
- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`

No se modificaron processors activos ni headers legacy.

## Clasificacion legacy

| Archivo | Clasificacion P7G | Evidencia de uso/no uso | Decision |
| --- | --- | --- | --- |
| `Source/Effects/Pedals/CompressorPedal.h` | `DUPLICATE_SUPERSEDED` | No aparece en `NOVA.jucer`; no aparece en `PedalRegistry`; no hay entrada dedicada en `PedalCatalog`; el registro activo usa `Source/Effects/Pedals/Compressor/CompressorPedal.h`. | Mantener intacto y poner en cuarentena contractual contra el reemplazo moderno. |
| `Source/Effects/Pedals/ChorusPedal.h` | `DUPLICATE_SUPERSEDED` | No aparece en `NOVA.jucer`; no aparece en `PedalRegistry`; no hay entrada dedicada en `PedalCatalog`; el registro activo usa `Source/Effects/Pedals/Chorus/ChorusPedal.h`. | Mantener intacto y poner en cuarentena contractual contra el reemplazo moderno. |
| `Source/Effects/Pedals/Wah/AutoWahPedal.h` | `LEGACY_ALIAS_ONLY` | Aparece en `NOVA.jucer` como header `compile="0"`; no aparece en `PedalRegistry`; `PedalCatalog` canonicaliza `Auto Wah`/`Autowah`/`AutoWah` a `Wah`; runtime activo usa `ClassicWahPedal`. | Mantener intacto y poner en cuarentena contractual de alias. |
| `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` | `LEGACY_ALIAS_ONLY` | Aparece en `NOVA.jucer` como header `compile="0"`; no aparece en `PedalRegistry`; `PedalCatalog` canonicaliza `Metal Distortion` a `Distortion`; runtime activo usa `DistortionPedal`. | Mantener intacto y poner en cuarentena contractual de alias. |

## Cambios al policy scan

Se agregaron checks `p7g_legacy_file_quarantined` para cada archivo legacy. Cada check exige:

- el archivo legacy existe;
- el reemplazo activo existe;
- el archivo legacy no esta registrado en `PedalRegistry`;
- su estado en `NOVA.jucer` coincide con la clasificacion esperada;
- para alias legacy, `PedalCatalog` mantiene la canonicalizacion esperada.

Los hallazgos internos legacy se reportan como `legacyQuarantine`, no como `legacyWarnings`, porque no forman parte de rutas de audio activas. Si cualquiera de las suposiciones cambia, el policy scan produce `contractFailures`.

Resultado inicial del policy scan P7G:

- `status=PASS`
- `failures=0`
- `warnings=0`
- `legacyWarnings=0`
- `legacyQuarantined=4`
- `contractFailures=0`
- `contractChecks=146`

## Auditoria actualizada

`docs/audio-realtime-safety-audit.md` y `docs/pedal-audit-matrix.md` ahora distinguen entre:

- processors activos corregidos en fases previas;
- headers legacy duplicados o alias-only que no se deben considerar audio path activo;
- riesgo futuro si alguien los registra sin modernizarlos.

No se marca ningun header legacy como DSP corregido. La decision es cuarentena, no revoicing ni refactor.

## Validacion

Validacion completa P7G ejecutada:

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings.
- `NOVA_SharedCode` Release x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings.
- `NOVA_VST3` Release x64: PASS, 0 warnings.
- `git diff --check`: PASS; solo avisos normales de CRLF en archivos ya modificados.
- `run-base-audio-validation.ps1` primera corrida: PASS, `results=189`, `passes=6520`, `failures=0`, `failingResults=0`.
- `run-base-audio-validation.ps1` segunda corrida: PASS, `results=189`, `passes=6520`, `failures=0`, `failingResults=0`.
- `run-golden-audio-metrics.ps1`: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`.
- `run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `16/16/0/0`.
- `run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`.
- `check-audio-thread-policy.ps1`: PASS, `failures=0`, `warnings=0`, `legacyWarnings=0`, `legacyQuarantined=4`, `contractFailures=0`, `contractChecks=146`.
- `run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.

## Riesgos restantes

- Los headers legacy siguen existiendo. Esto es intencional para evitar churn de source membership y cambios accidentales de include/build.
- Si un header legacy se registra en el futuro, P7G debe dejar de tratarlo como cuarentena y debe modernizarse antes de entrar al audio path.
- `AutoWahEditor.h` aun incluye `AutoWahPedal.h`, pero ese editor no esta registrado como ruta runtime activa. Si se reactiva AutoWah, processor y editor deben auditarse juntos.

## Recomendacion para P7H

Entrar a una fase separada de legacy source membership solo si el proyecto quiere retirar fisicamente estos headers. Esa fase deberia tratar `NOVA.jucer`, Projucer/export, includes de editores legacy y build artifacts como alcance explicito. No mezclarlo con DSP activo, DAW smoke ni tonal QA.
