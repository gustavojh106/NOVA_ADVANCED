# P3 Warning Cleanup Notes

Fecha: 2026-04-29

## Warnings revisados

### C4099 (`TempoSyncable` struct/class mismatch)

- Estado previo: `AudioEngine.h` forward declaration como `class TempoSyncable;` mientras la definición real está como `struct TempoSyncable` en `Source/Effects/Pedals/Base/ProcessorBase.h`.
- Acción P3: corrección trivial y segura.
  - Cambio aplicado: `struct TempoSyncable;` en `Source/Core/AudioEngine.h`.
- Impacto DSP/tone: ninguno.

### C4458 (`latencySamples` oculta miembro JUCE)

- Estado previo: parámetro `int latencySamples` en `ProcessorBase::prepareBypassLatencyLines(...)` podía ocultar miembro homónimo heredado de JUCE.
- Acción P3: corrección trivial y segura.
  - Cambio aplicado: renombre a `requestedLatencySamples` en `Source/Effects/Pedals/Base/ProcessorBase.h`.
- Impacto DSP/tone: ninguno (solo naming/local variable hygiene).

## Warnings residuales observados en P3

- `C4100` en `Source/Core/AudioEngine.cpp` (`health` no usado) durante build de `NOVA_SharedCode` Debug x64.
- No se corrigió en esta fase para mantener P3 enfocado en baseline lock/validation hygiene.

## Evaluación de riesgo

- Los dos warnings objetivo (C4099/C4458) eran triviales y se corrigieron sin cambios funcionales.
- No se introdujeron cambios de arquitectura, DSP, UI, IDs de parámetros ni preset schema.
