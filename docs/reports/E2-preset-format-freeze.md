# E2 — Formato de presets congelado para la beta

- **Fecha:** 2026-10-13
- **Estado:** hecho. El formato queda congelado en el **esquema 1**.
- **Roadmap:** `docs/latency-optimization-roadmap.md` → §11b, criterio E2.
- **Objetivo:** que los presets y las sesiones de DAW que guarden los beta testers sigan cargando igual en cualquier versión futura de NOVA.

## 1. Formato

Los presets (`.nova-preset`) y las sesiones de DAW usan el mismo árbol: un `juce::ValueTree` binario (`writeToStream`).

| Nodo | Contenido |
|---|---|
| `NOVA_STATE` (raíz) | `schemaVersion` = 1; `savedByVersion` = versión de NOVA que lo escribió (nuevo, solo informativo) |
| `SETTINGS` | `engineOn`, `switchMode`, `inputGain`, `inputGate`, `forceMono`, `outputVol`, `outputLimiter`, `outputMix` |
| `LINE_A` / `LINE_B` | Mezclador de la línea (`gainA/B`, `panA/B`, `widthA/B`) y los pedales en orden |
| `PEDAL` | `type` (ID del catálogo), `zone`, `enabled`, `pedalID` y `state` (estado del procesador en Base64) |

**Estado de cada procesador** (dentro de `state`):
- **Genérico** (`ProcessorBase`, la mayoría de pedales, amps y cabs): XML `PLUGIN_STATE` con un `PARAM` por parámetro: `id`, `value` (normalizado 0..1) y `raw` (valor real en unidades del parámetro, nuevo).
- **Propios:** Distortion (`DISTORTION_STATE` v2) y Reverb (`NIMBUS_REVERB_STATE` v7) guardan valores reales por ID; Wah (`WAH_STATE`) guarda `value` y `raw` por ID.

**Al cargar:** se valida el tamaño (≤ 50 MB) y las secciones obligatorias. Se descartan tipos de pedal desconocidos, se aplican las reglas de zona (un amp y un cab por línea, máximo 12 por zona flexible), se recortan los valores a rango y los parámetros que falten quedan en su valor por defecto.

## 2. Qué se cambió

| Cambio | Para qué |
|---|---|
| **Valor real (`raw`) junto al normalizado** en el serializador genérico y en el Wah | Si en el futuro cambia el rango de una perilla, el preset conserva el valor real (los mismos dB, ms o Hz) en vez de reinterpretar el número 0..1. Mientras el rango no cambie, se usa el normalizado exacto: los presets cargan igual que hoy, bit a bit |
| **Punto de migración** `PluginStateModel::migrateStateTree()` | Corre antes de estampar la versión actual. Hoy no hace nada (esquema 1); ahí van los pasos de versiones futuras |
| **Detección de presets de una versión más nueva** (`isFromNewerSchema`) | Carga lo que puede y deja una entrada `preset.load.newerSchema` / `state.restore.newerSchema` en el log. Desde E6, además, un diálogo avisa al usuario (`docs/reports/E6-problem-report.md` §4) |
| **`savedByVersion`** en cada preset y sesión | Saber qué versión escribió un preset cuando un tester reporta un problema |
| Log corregido | `PluginProcessor::loadPresetFromFile` registraba la versión *después* de sobreescribirla (siempre decía 1) |

## 3. Tests nuevos (`E2 preset format freeze`)

| Test | Qué verifica | Resultado |
|---|---|---|
| Round-trip completo | Los 24 procesadores con **todas** sus perillas en valores no default, pasando por el archivo de preset y un motor nuevo. Interruptores y selectores se comparan por su valor efectivo | 189 parámetros, 0 diferencias |
| Cambio de rango | Con un `value` normalizado que ya no corresponde al `raw`, gana el valor real. Un estado viejo sin `raw` carga por el normalizado | PASS |
| **Presets de referencia v1** | 25 presets en `docs/golden-metrics/presets-v1/` (uno por procesador más un `Full-Rig` con dos líneas, todas las zonas, routing dual y globales no default) y sus valores esperados en `expected.json` (valores reales). Cada versión futura debe cargarlos con los mismos valores | 25/25, 0 diferencias (832 comprobaciones) |

Antes ya existían los tests P7D (topología, routing, bypass y canonicalización) y round-trips por pedal llamando directo al procesador. Lo que faltaba era comprobar **los valores de todas las perillas a través del archivo**.

**Validación:** base PASS (337 resultados, 13953 checks, 0 fallos) en Debug y Release; bit-exacto 56/56; golden PASS. La única métrica golden que cambió fue `state_size_bytes` del caso `overdrive_v2_musicalsafe_nominal` (318 → 429 bytes, por el `raw`). Se actualizó a mano solo ese valor; el mismo caso sigue con `null_rms = 0`, o sea que el Overdrive se recupera exacto.

## 4. Reglas desde la beta

**Prohibido (rompe presets de usuarios):**
1. Renombrar o quitar IDs del árbol (`Nova::IDs`), IDs de tipo de pedal (`typeID` del catálogo) o IDs de parámetro (`paramID`). Si un pedal cambia de nombre, se agrega un alias en `PedalRegistry::canonicalType`.
2. Reordenar o quitar opciones de un parámetro de selección: se guarda el índice. Solo se agregan opciones al final.
3. Regenerar `docs/golden-metrics/presets-v1/` para que un test pase. Si un fixture falla, el cambio rompió presets reales.

**Permitido sin migración:**
- Agregar parámetros: los presets viejos los cargan con su valor por defecto.
- Agregar pedales nuevos.
- Cambiar el rango de una perilla: se conserva el valor real.
- Agregar propiedades nuevas al árbol: las versiones viejas las ignoran.

**Con migración:** cualquier cambio de significado o de estructura (mover datos entre nodos, cambiar unidades sin cambiar el ID, reemplazar un parámetro por otro):
1. Subir `Config::STATE_SCHEMA_VERSION`.
2. Agregar el paso en `migrateStateTree()` (`if (fromVersion < 2) { ... }`).
3. Mantener pasando los fixtures v1 y agregar fixtures v2.

**Regenerar fixtures** (solo para una versión nueva del esquema, en una carpeta nueva): correr los tests con `NOVA_WRITE_PRESET_FIXTURES=1`.

## 5. Pendiente fuera de E2

- ~~Mostrar en la UI un aviso cuando un preset viene de una versión más nueva de NOVA~~. **Hecho en E6 (2026-10-13):** diálogo una sola vez, también para el preset de inicio y las sesiones de DAW.
