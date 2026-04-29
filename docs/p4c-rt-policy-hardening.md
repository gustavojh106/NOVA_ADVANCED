# P4C RT Policy Hardening

Fecha: 2026-04-29

## Objetivo

Endurecer la policy de audio thread para detectar regresiones de seguridad RT temprano, sin cambiar DSP, tono, UI, wizards, IDs de parametros ni preset schema.

## Script de policy scan

Archivo:

- `scripts/check-audio-thread-policy.ps1`

Comandos:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1 -FailOnWarn
```

Salidas:

- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`

Estado posible:

- `PASS`: sin bloqueos ni advertencias.
- `WARN`: sin bloqueos, pero con advertencias (por ejemplo legacy no activos).
- `FAIL`: patron peligroso detectado en ruta activa de audio.

## Que bloquea (FAIL)

En rutas activas (`processBlock`, `AudioEngine::process`, `AudioEngine::processWithSampleAccurateDryWet`):

- `SessionLogger::logEvent`
- `juce::String` para logging en audio path
- `SpinLock::ScopedLockType`
- `std::mutex` / `std::lock_guard`
- `juce::ScopedLock` / `CriticalSection`
- `WaitableEvent`
- `AudioBuffer::setSize` en process path
- `std::vector::resize` / `push_back` en process path
- `dynamic_cast` en process path
- `IIR::Coefficients<float>::make*` en process path
- `setValueNotifyingHost` en process path
- graph rebuild directo en process path

## Que advierte (WARN)

- Hallazgos en archivos clasificados legacy/no activos.
- Hallazgos allowlisted explicitamente (si aplican).
- Cualquier warning si se ejecuta sin `-FailOnWarn`.

`-FailOnWarn` convierte warnings en error de salida para uso CI estricto.

## Allowlist actual

El allowlist vive dentro de `scripts/check-audio-thread-policy.ps1` y es explicito por:

- `file`
- `patternId`
- `lineRegex`
- `reason`

Entradas actuales:

- `Source/Core/PluginProcessor.cpp` + `dynamic_cast` en `parameterValueChanged` (fuera de audio callback).
- `Source/Core/AudioEngine.cpp` + flag atomico `graphResetRequested.store(true, ...)` (no rebuild directo en audio thread).

Regla:

- No usar allowlist para ocultar patrones en rutas activas.
- Solo usar allowlist para falsos positivos verificables y con `reason` concreto.

## Legacy headers y guard

Clasificacion P4C:

- `Source/Effects/Pedals/ChorusPedal.h`: legacy no activo (`NOVA.jucer`: no, `PedalRegistry`: no).
- `Source/Effects/Pedals/CompressorPedal.h`: legacy no activo (`NOVA.jucer`: no, `PedalRegistry`: no).
- `Source/Effects/Pedals/Wah/AutoWahPedal.h`: en `NOVA.jucer`, no registrado en `PedalRegistry`.
- `Source/Effects/Pedals/Metal/MetalDistortionPedal.h`: en `NOVA.jucer`, no registrado en `PedalRegistry`.

Guard documental:

- Ningun pedal legacy puede entrar al `PedalRegistry` sin cumplir primero la policy P1 de no-allocation en tiempo real.
- Alias legacy en `PedalCatalog::resolveAlias` deben seguir apuntando a procesadores modernizados (`Auto Wah -> Wah`, `Metal Distortion -> Distortion`) hasta que exista modernizacion formal.

## Audit de getPlayHead()->getPosition()

### Estado previo

- `NOVAAudioProcessor::processBlock` llamaba cada bloque:
  - `refreshEngineEnabledIfNeeded(false)`
  - `refreshEngineGlobalParamsIfNeeded(false, false)`
- `refreshEngineGlobalParamsIfNeeded` llamaba siempre `applyHostTransportState`, que ejecutaba `getPlayHead()->getPosition()` por bloque.

### Riesgo real

- En muchos hosts la consulta es barata, pero no hay garantia fuerte de costo constante o lock-free desde este lado.
- La consulta por bloque agrega overhead y aumenta superficie RT en la ruta de audio.

### Solucion minima aplicada en P4C

- Se mantiene la consulta en audio thread (donde el host suele proveer estado valido), pero se reduce frecuencia:
  - polling de transport cada 8 bloques (`kHostTransportPollIntervalBlocks = 8`).
  - en bloques intermedios se reutiliza snapshot atomico previo (`lastRuntimeGlobalParams`).
- `force=true` sigue refrescando transport inmediatamente.
- Se resetea el contador de polling en `prepareToPlay`.

Impacto:

- Menor presion de llamadas host en audio callback.
- Sin refactor grande de `AudioEngine`.
- Sin cambios DSP/tono/IDs/schema.

### Recomendacion P5

- Si se busca policy RT mas estricta, evaluar diseno de snapshot host-transport aislado y/o ruta dedicada de control-thread con validacion host-especifica.

