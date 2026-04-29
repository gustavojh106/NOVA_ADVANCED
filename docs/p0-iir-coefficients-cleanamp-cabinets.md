# P0 IIR Coefficients CleanAmp/Cabinets

Fecha: 2026-04-28
Baseline referenciada: `baseline-audio-v2`
Motivo: cerrar el bloqueo documentado en `docs/p1-blocked-by-p0-rt-safety-report.md`.

## Resumen

Se elimino el uso de `juce::dsp::IIR::Coefficients<float>::make*` desde rutas alcanzables por `processBlock` en `CleanAmp` y cabinets.

El cambio es mecanico y conservador: se mantiene `ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>`, pero las asignaciones a `*state` ahora usan `juce::dsp::IIR::ArrayCoefficients<float>::make*`, igual que en `ClassicAmp`, `HighGainAmp`, `ChimeAmp` y `BoutiqueAmp`.

No se tocaron UI, wizards, IDs de parametros, `AudioEngine`, P1, buffers, fallbacks, tests de pedales ni voicing intencional.

## Archivos modificados

- `Source/Effects/Amplifiers/CleanAmp.h`
- `Source/Effects/Cabinets/CabinetPedal.h`
- `Source/Effects/Cabinets/Vintage2x12Cabinet.h`
- `Source/Effects/Cabinets/Modern4x12Cabinet.h`

## Factories eliminadas

### `CleanAmp`

Reemplazadas dentro de `updateToneFilters`, llamada desde `processOversampledAmp`:

- `IIR::Coefficients<float>::makeHighPass(currentInnerRate, 38.0f, 0.70710678f)`
- `IIR::Coefficients<float>::makeLowShelf(currentInnerRate, 250.0f, 0.64f, decibelsToGain(bassGainDb))`
- `IIR::Coefficients<float>::makeHighShelf(currentInnerRate, 3100.0f, 0.58f, decibelsToGain(trebleGainDb))`
- `IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f, 0.70710678f)`

### `CabinetPedal`

Reemplazadas dentro de `updateCabinetVoicing`, llamada desde `processBlock`:

- `IIR::Coefficients<float>::makeLowShelf(currentSampleRate, 130.0f, 0.8f, decibelsToGain(thump))`
- `IIR::Coefficients<float>::makeHighShelf(currentSampleRate, 3400.0f, 0.72f, decibelsToGain(air))`
- `IIR::Coefficients<float>::makeLowPass(currentSampleRate, lowPass, 0.68f)`
- `IIR::Coefficients<float>::makeHighPass(currentSampleRate, highPass, 0.7f)`

### `Vintage2x12Cabinet`

Reemplazadas dentro de `updateVoicing`, llamada desde `processBlock`:

- `IIR::Coefficients<float>::makeLowShelf(currentSampleRate, 180.0f, 0.75f, decibelsToGain(warmth))`
- `IIR::Coefficients<float>::makeHighShelf(currentSampleRate, 4200.0f, 0.65f, decibelsToGain(sparkle))`
- `IIR::Coefficients<float>::makeLowPass(currentSampleRate, lpFreq, 0.62f)`
- `IIR::Coefficients<float>::makeHighPass(currentSampleRate, hpFreq, 0.65f)`

### `Modern4x12Cabinet`

Reemplazadas dentro de `updateVoicing`, llamada desde `processBlock`:

- `IIR::Coefficients<float>::makeLowShelf(currentSampleRate, 100.0f, 0.85f, decibelsToGain(lowEnd))`
- `IIR::Coefficients<float>::makeHighShelf(currentSampleRate, 3000.0f, 0.78f, decibelsToGain(presence))`
- `IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 650.0f, 1.2f, decibelsToGain(-2.0f))`
- `IIR::Coefficients<float>::makeLowPass(currentSampleRate, lpFreq, 0.72f)`
- `IIR::Coefficients<float>::makeHighPass(currentSampleRate, hpFreq, 0.72f)`

## Patron usado

Antes:

```cpp
*filter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, freq, q);
```

Ahora:

```cpp
*filter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(sampleRate, freq, q);
```

Esto evita crear el objeto heap/ref-counted que devuelven las factories de `Coefficients::make*` en rutas alcanzables por el audio thread.

## Confirmacion tonal

No hubo cambio tonal intencional.

Se conservaron los mismos valores numericos de frecuencia, Q y gain:

- `CleanAmp`: `38.0f`, `0.70710678f`, `250.0f`, `0.64f`, `3100.0f`, `0.58f`, `18.0f`.
- `CabinetPedal`: `130.0f`, `0.8f`, `3400.0f`, `0.72f`, `0.68f`, `0.7f`.
- `Vintage2x12Cabinet`: `180.0f`, `0.75f`, `4200.0f`, `0.65f`, `0.62f`.
- `Modern4x12Cabinet`: `100.0f`, `0.85f`, `3000.0f`, `0.78f`, `650.0f`, `1.2f`, `-2.0f`, `0.72f`.

Las expresiones dependientes de parametros tambien se conservaron:

- `bassGainDb`, `trebleGainDb`, `thump`, `air`, `warmth`, `sparkle`, `lowEnd`, `presence`.
- `lowPass`, `highPass`, `lpFreq`, `hpFreq`.

## Verificacion

Comandos/busquedas ejecutados:

```powershell
rg -n "IIR::Coefficients<float>::make" Source/Effects/Amplifiers/CleanAmp.h Source/Effects/Cabinets/CabinetPedal.h Source/Effects/Cabinets/Vintage2x12Cabinet.h Source/Effects/Cabinets/Modern4x12Cabinet.h
```

Resultado: sin matches.

```powershell
rg -n "IIR::ArrayCoefficients<float>::make" Source/Effects/Amplifiers/CleanAmp.h Source/Effects/Cabinets/CabinetPedal.h Source/Effects/Cabinets/Vintage2x12Cabinet.h Source/Effects/Cabinets/Modern4x12Cabinet.h
```

Resultado: confirma 17 asignaciones `ArrayCoefficients` en los cuatro archivos objetivo.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode
```

Resultado: PASS, 0 errors, 16 warnings existentes (`ProcessorBase::latencySamples`, `TempoSyncable`, parametro `health` no usado).

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
```

Resultado: PASS, 0 warnings, 0 errors.

## Riesgos restantes

- P1 todavia no esta hecho: los cabinets aun tienen `scratchBuffer.setSize(...)` dentro de `processBlock`.
- Esta fase no reviso ni corrigio buffers, fallbacks no-allocation, Neural helpers, Phaser DC accumulation ni tests P1.
- El build conserva warnings preexistentes fuera del alcance de este micro-pass.
- `SyntheticIR` sigue usando `std::vector` durante generacion de IR, pero la ruta observada es `prepareToPlay`/`loadSyntheticIR`, no `processBlock`.

## Estado para P1

El bloqueo P0 documentado para JUCE IIR coefficient factories en `CleanAmp` y cabinets queda cerrado en el alcance de este micro-pass.

P1 Pedal Safety & Stability Pass puede retomarse para atacar `setSize`/allocations en `processBlock`, fallbacks no-allocation y tests, sin mezclarlo con este cambio.
