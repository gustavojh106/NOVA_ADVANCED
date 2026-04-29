# P1 Blocked By P0 RT Safety Report

Fecha: 2026-04-28
Baseline referenciada: `baseline-audio-v2`
Documento P0 encontrado: `docs/p0-rt-safety-changes.md`

## Resumen

La fase P1 no debe avanzar todavia. Existe al menos un riesgo P0 critico que sigue alcanzable desde el audio thread: construccion de coeficientes JUCE IIR con factories `IIR::Coefficients<float>::make*` desde rutas llamadas por `processBlock`.

No se modificaron pedales P1 ni tests P1 en este paso.

## P0 confirmado como corregido por el reporte existente

Segun `docs/p0-rt-safety-changes.md`, quedaron corregidos:

- `PedalSignalTelemetry` ya no llama `SessionLogger`, no construye `juce::String` y no despierta `WaitableEvent` desde audio thread.
- `SessionStore::getRuntimeGlobalParams()` ya no toma `runtimeCacheLock`; usa snapshot atomico.
- `PluginProcessor::processBlock()` llama `refreshEngineEnabledIfNeeded(false)` y `refreshEngineGlobalParamsIfNeeded(false, false)`, evitando logging y lock de push en audio thread.
- `ClassicAmp`, `HighGainAmp`, `ChimeAmp` y `BoutiqueAmp` ya no crean `std::vector<float*>` por bloque.
- Los cuatro amps premium migraron sus factories JUCE IIR a `ArrayCoefficients`.

## P0 que bloquea P1

### CleanAmp

Ruta alcanzable desde audio thread:

- `Source/Effects/Amplifiers/CleanAmp.h:163` define `CleanAmp::processBlock`.
- `processBlock` llama `processOversampledAmp(buffer)`.
- `Source/Effects/Amplifiers/CleanAmp.h:801` llama `updateToneFilters(false)` dentro del procesamiento del bloque.
- `Source/Effects/Amplifiers/CleanAmp.h:731`, `735`, `740` y `745` llaman `juce::dsp::IIR::Coefficients<float>::makeHighPass/makeLowShelf/makeHighShelf`.

Impacto:

- Estas factories devuelven objetos de coeficientes y pueden asignar memoria.
- La asignacion es condicional a cambios de parametros/cache, pero sigue ocurriendo desde `processBlock`.
- Esto contradice el criterio P0: "JUCE IIR make* desde process en amps/cabs/CleanAmp".

### Cabinets

Las rutas de cabinets reales en este checkout estan bajo `Source/Effects/Cabinets`, no bajo `Source/Effects/Pedals/Cabinet`.

Rutas alcanzables desde audio thread:

- `Source/Effects/Cabinets/CabinetPedal.h:102` define `processBlock`, llama `updateCabinetVoicing()`, y esta funcion llama `IIR::Coefficients<float>::make*` en `:182`, `:186`, `:190` y `:193`.
- `Source/Effects/Cabinets/Vintage2x12Cabinet.h:99` define `processBlock`, llama `updateVoicing()`, y esta funcion llama `IIR::Coefficients<float>::make*` en `:177`, `:179`, `:181` y `:182`.
- `Source/Effects/Cabinets/Modern4x12Cabinet.h:101` define `processBlock`, llama `updateVoicing()`, y esta funcion llama `IIR::Coefficients<float>::make*` en `:180`, `:182`, `:185`, `:187` y `:188`.

Impacto:

- La reconstruccion de coeficientes ocurre desde `processBlock` cuando cambian parametros de voicing.
- Esto tambien cae en el criterio P0 de JUCE IIR factories desde audio thread.
- Ademas, estos mismos archivos contienen `scratchBuffer.setSize(...)` en `processBlock`; eso es P1, pero no debe corregirse hasta cerrar el P0 anterior.

## No bloqueante observado

- `Source/Effects/Cabinets/SyntheticIR.h` usa `std::vector` durante generacion de IR, pero las llamadas observadas vienen desde `loadSyntheticIR()` durante `prepareToPlay`, no desde `processBlock`.

## Recomendacion antes de P1

Cerrar un micro-pass P0 antes de esta fase:

1. Migrar `CleanAmp` y cabinets a asignacion RT-safe de coeficientes, siguiendo el patron ya usado en `ClassicAmp`, `HighGainAmp`, `ChimeAmp` y `BoutiqueAmp`: `juce::dsp::IIR::ArrayCoefficients<float>::make*` asignado al estado ya preparado.
2. Mantener identicas frecuencias, Q y gains para no revoicing.
3. Confirmar con `rg` que no quedan `IIR::Coefficients<float>::make*` alcanzables desde `processBlock` en `Source/Effects/Amplifiers/CleanAmp.h` ni `Source/Effects/Cabinets/*.h`.
4. Solo despues retomar P1: eliminar `AudioBuffer::setSize`/`std::vector::resize` en `processBlock`, agregar fallbacks no-allocation y tests.

## Estado

P1 queda bloqueado hasta cerrar esos P0.
