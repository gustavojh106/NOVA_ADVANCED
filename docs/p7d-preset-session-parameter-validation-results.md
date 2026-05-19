# P7D Preset / Session / Parameter Validation Results

Fecha: 2026-05-06

## Resumen

P7D blinda persistencia de presets, restore de session state y payloads de pedales antes de DAW smoke tests, tonal QA, presets finales o UI/UX.

No se hicieron cambios DSP, tonales, dry/wet, routing, graph lifecycle, UI/UX, factory presets, golden baselines ni schema IDs. `STATE_SCHEMA_VERSION` sigue en `1`.

## Archivos modificados

- `Source/Core/PluginStateModel.h`
- `Source/Core/AudioEngineTests.cpp`
- `scripts/check-audio-thread-policy.ps1`
- `docs/p7d-preset-session-parameter-validation-results.md`

## Cambios realizados

- `PluginStateModel::applyDefaultValues` ahora canonicaliza valores persistidos inseguros, no solo campos faltantes:
  - bools invalidos vuelven a default seguro.
  - enums/indices se limitan al rango valido.
  - floats NaN/Inf vuelven a default.
  - floats fuera de rango se clampan al rango de parametro existente.
- `PluginStateModel::sanitizeLine` ahora limita restores directos a `MAX_PEDALS_PER_FLEX_ZONE` por zona flexible (`Pre` y `FX`), preservando la regla ya existente de `insertPedal`.
- `sanitizeLine` tambien normaliza `PEDAL_ENABLED` invalido a default seguro.

Estos cambios afectan solo estado corrupto, incompleto o fuera de contrato. El path nominal de presets validos conserva type, id, zone, bypass/enabled, orden y parametros.

## Tests agregados

- `P7D preset save-load-save remains canonical`
- `P7D catalog presets round-trip every registered pedal`
- `P7D chain/global preset round-trips routing modes and bypass state`
- `P7D schema canonicalization rejects unknowns and clamps topology`
- `P7D parameter boundary restore clamps unsafe values`
- `P7D pedal state payload restore rejects corrupt payloads safely`
- `P7D corrupt session recovery leaves engine processable`

## Cobertura round-trip

- `save -> load -> save -> canonical compare`.
- Preset individual para cada entrada de `PedalCatalog::entries()`.
- Cadenas con `LineA_Only`, `LineB_Only` y `Dual_Parallel`.
- Zonas `Pre`, `Amp`, `FX`, `Cabinet`.
- `PEDAL_ENABLED` true/false.
- Parametros globales y mixer en valores medios y extremos validos.
- Preservacion de type, id, zone, enabled/bypassed, chain order, switchMode, line gain/pan/width y global params.

## Schema y canonicalizacion

Cubierto:

- schema actual `1`.
- schema antiguo simulado `0` canonicalizado a `1`.
- campos faltantes con defaults seguros.
- campos extra desconocidos sin crash.
- child nodes no pedal eliminados de lineas.
- pedal type desconocido omitido de forma segura.
- aliases legacy soportados, por ejemplo `Auto Wah -> Wah`.
- zona invalida o no permitida canonicalizada por `PedalCatalog::enforceZone`.
- duplicate amp/cabinet reducido a uno por cadena.
- mas de `MAX_PEDALS_PER_FLEX_ZONE` clampado por zona flexible.
- estado vacio restaurado a engine usable.

Comportamiento esperado:

- Unknown pedal type: skip seguro.
- Unknown fields: no participan en runtime; no deben bloquear restore.
- Invalid zone: canonicalizacion a zona permitida por catalogo.
- Duplicate amp/cabinet: se conserva el primero valido en orden de restore y se descartan duplicados.
- Oversized Pre/FX restore: se conservan hasta `MAX_PEDALS_PER_FLEX_ZONE` y se descarta el excedente.

## Parameter boundaries

Cubierto:

- NaN/Inf.
- valores negativos/positivos fuera de rango.
- bools/string invalidos.
- enums/indices fuera de rango.
- floats extremos.
- pedal enabled invalido.

Validado:

- clamp/default seguro.
- runtime cache finito.
- restore seguido de prepare/process con salida finita.
- save posterior parte de estado canonico valido.

## Payload corrupto

Cubierto:

- Base64 corrupto.
- decoded payload demasiado pequeno.
- bytes que no forman XML valido.
- decoded payload mayor al limite de 2 MB.
- payload vacio.
- payload valido con parametros desconocidos.

Validado:

- no crash.
- no hang.
- no throw sin capturar.
- graph conserva el pedal valido.
- process posterior produce salida finita.

## Session recovery

Cubierto:

- partial state restore.
- preset file corrupto/truncado rechazado.
- startup preset pointer apuntando a preset corrupto.
- host state corrupto via `NOVAAudioProcessor::setStateInformation`.
- restore seguido de prepare/process.
- restore con engine off y luego restore con engine on.

No se agrego cobertura de directorio completo de user presets corrupto porque no hay helper de inyeccion de directorio; P7D cubre el startup pointer real preservando/restaurando el contenido previo durante el test.

## Policy checks agregados

`scripts/check-audio-thread-policy.ps1` conserva los checks previos y agrega:

- `p7d_audio_test_present` para los siete tests P7D.
- `p7d_schema_version_unchanged` para bloquear schema bump accidental.
- `p7d_results_doc_present` para exigir este reporte de cierre.

## Validacion

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings.
- `NOVA_SharedCode` Release x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings.
- `NOVA_VST3` Release x64: PASS, 0 warnings.
- `git diff --check`: PASS.
- Base validation Debug x64: PASS dos corridas consecutivas, `results=188 passes=6506 failures=0 failingResults=0`.
- Golden metrics: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`; no baseline update.
- RT profile Release: PASS `16/16/0/0`.
- RT profile stability Release `-CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`.
- Policy scan: `failures=0`, `contractFailures=0`, `contractChecks=136`.
- Wrapper Fast Release: PASS.

Nota: una corrida pre-final fallo por una comparacion exacta contra bytes de payload opaco de `ReverbPedal`; el valor restaurado era equivalente a nivel de parametro pero serializaba dos floats con representacion textual distinta. El test exacto save/load/save se dejo con payload estable y Reverb permanece cubierto por round-trip de catalogo y payload robustness. Las dos corridas finales consecutivas pasaron.

## Riesgos restantes

- Los tests ejercitan restore/persistence determinista, no DAW smoke tests reales.
- Policy scan es estatico y por nombres/patrones; complementa, no reemplaza, validation runtime.
- No se hizo factory preset curation ni tonal QA por alcance explicito de P7D.

## Recomendacion P7E

P7E deberia entrar a DAW smoke tests y host-state smoke con escenarios reales de plugin load/save, manteniendo fuera revoicing, UI/UX y factory preset curation hasta que persistence quede validada tambien en hosts.
