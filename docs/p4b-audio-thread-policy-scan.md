# P4B Audio Thread Policy Scan

Fecha: 2026-04-29

## Objetivo

Revisar rutas de audio thread y patrones peligrosos antes de aceptar la baseline de runtime profiling P4B. Esta revision no cambia DSP, tono, UI, wizards, IDs de parametros ni preset schema.

## Patrones buscados

- `SessionLogger::logEvent`
- `juce::String` usado para logging en audio thread
- `juce::SpinLock::ScopedLockType`
- `std::mutex` / `std::lock_guard`
- `juce::ScopedLock` / `juce::CriticalSection`
- `juce::WaitableEvent`
- `AudioBuffer::setSize` dentro de `processBlock`
- `std::vector::resize` / `push_back` dentro de `processBlock`
- `dynamic_cast` dentro de `processBlock`
- `IIR::Coefficients<float>::make*` dentro de `processBlock`
- `setValueNotifyingHost` dentro de `processBlock`
- graph rebuild directo dentro de `processBlock`

Comandos usados:

```powershell
rg -n "SessionLogger::logEvent|juce::String|SpinLock::ScopedLockType|std::mutex|std::lock_guard|WaitableEvent|setValueNotifyingHost|buildGraphFromModelLocked|requestControlGraphRebuild|dynamic_cast<|IIR::Coefficients<float>::make|\.setSize\(|\.resize\(|\.push_back\(" Source\Core Source\Effects
rg -n "void .*processBlock|processBlock\(|void process\(|process\(juce::AudioBuffer|AudioBuffer<float>& buffer" Source\Core\AudioEngine.cpp Source\Core\PluginProcessor.cpp Source\Effects
rg -n "std::mutex|std::lock_guard|juce::ScopedLock|CriticalSection|SpinLock::ScopedLockType|WaitableEvent" Source\Core Source\Effects
```

## Archivos revisados

- `Source/Core/PluginProcessor.cpp`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngine.h`
- `Source/Core/SessionStore.h`
- `Source/Core/SessionCoordinator.h`
- `Source/Core/SessionLogger.h`
- `Source/Core/Visualizer.h`
- `Source/Core/DSP/Global/InputChain.*`
- `Source/Core/DSP/Global/ChannelStrip.*`
- `Source/Core/DSP/Global/OutputChain.*`
- Pedales registrados por `Source/Core/PedalRegistry.h`
- Amplificadores registrados por `Source/Core/PedalRegistry.h`
- Cabinets registrados por `Source/Core/PedalRegistry.h`
- Headers legacy encontrados por busqueda global en `Source/Effects/Pedals`

## Rutas de audio revisadas

- `NOVAAudioProcessor::processBlock`
- `AudioEngine::process`
- `AudioEngine::processWithSampleAccurateDryWet`
- `OutputChainProcessor::processBlock`
- `InputChainProcessor::processBlock`
- `ChannelStripProcessor::processBlock`
- `processBlock` de pedales registrados
- `processBlock` de amplificadores registrados
- `processBlock` de cabinets registrados

## Coincidencias y clasificacion

| Patron | Coincidencias | Clasificacion |
| --- | --- | --- |
| `SessionLogger::logEvent` | Aparece en constructor, prepare/release, control plane, diagnosticos offline y `AudioEngine::run()` | No hay llamada directa desde `processBlock` o `AudioEngine::process`. |
| `juce::String` para logging | Aparece en logging/control/docs/test/offline diagnostics | `processBlock` llama `refreshEngine*IfNeeded(false, false)`, lo que evita construir logs desde audio thread. |
| `SpinLock::ScopedLockType` | `Source/Core/SessionLogger.h` | Peligroso si se llamara desde audio thread. Scan no encontro `logEvent` en audio path. |
| `WaitableEvent` | `Source/Core/SessionLogger.h` | Solo logger worker wake-up; no audio path directo. |
| `juce::ScopedLock` / `CriticalSection` | `AudioEngine` command/model/owner locks y `SessionLogger` write lock | Control plane, prepare, graph commands, diagnostics e info. No hay lock directo dentro de `AudioEngine::process`. |
| `std::mutex` / `std::lock_guard` | Sin coincidencias relevantes en `Source/Core` o `Source/Effects` | PASS. |
| `AudioBuffer::setSize` en registered `processBlock` | No encontrado en rutas registradas activas | PASS. `setSize` aparece en prepare paths y buffers prealocados. |
| `std::vector::resize` / `push_back` en registered `processBlock` | No encontrado en rutas registradas activas | PASS. `push_back` aparece en tests/offline diagnostics/control helpers. |
| `dynamic_cast` en `processBlock` | No encontrado en rutas activas de audio | PASS. `dynamic_cast` aparece en graph build/control/offline diagnostics. |
| `IIR::Coefficients<float>::make*` en `processBlock` | No encontrado | PASS. Coefficients se recalculan en prepare/update helpers, no en el audio callback. |
| `setValueNotifyingHost` en `processBlock` | No encontrado | PASS. Aparece en UI, recall, tests y setup offline. |
| graph rebuild en `processBlock` | No rebuild directo | PASS. Audio thread solo puede setear atomics de recovery; rebuild ocurre en control thread. |

## Hallazgos especificos

- `NOVAAudioProcessor::processBlock` contiene `refreshEngineEnabledIfNeeded(false)`, `refreshEngineGlobalParamsIfNeeded(false, false)`, `audioEngine.process(...)` y `audioVisualizer.pushBuffer(...)`. La ruta evita logging con `allowLogging=false`. `audioVisualizer.pushBuffer` usa arrays fijos y atomics; no se observaron locks ni allocation.
- `SessionStore::getRuntimeGlobalParams()` carga un snapshot atomico. `SessionStore::isEngineEnabled()` carga un bool atomico. Esa lectura es compatible con audio thread.
- `AudioEngine::process()` usa un medidor ligero con `juce::Time::getMillisecondCounterHiRes()` y atomics ya existentes. No construye strings, no usa locks y no hace rebuild directo.
- `AudioEngine::handleHealthAfterBlock()` puede marcar `graphResetRequested` y `pendingAutoHealLog` con atomics si detecta corrupcion sostenida. El rebuild y logging se ejecutan despues en `AudioEngine::run()`, no en el audio callback.
- `OutputChainProcessor::processBlock()` aplica scrub, DC blocker, gain, limiter y soft ceiling usando buffers ya preparados. `PeakLimiter::prepare()` hace `delayBuffer.setSize(...)`, pero `processBlock()` no cambia tamanos.

## Falsas alarmas

- `SessionLogger.h` contiene `SpinLock`, `CriticalSection` y `WaitableEvent`; son aceptables mientras `SessionLogger::logEvent` no se invoque desde audio thread.
- `AudioEngine` contiene `juce::ScopedLock` en prepare, graph command flushing, getters diagnosticos y build graph; no aparecen en `AudioEngine::process`.
- `setValueNotifyingHost` aparece en editores, restore/setup y offline QA. No aparece en audio callback.
- `dynamic_cast` aparece en graph build/control/offline diagnostics. No aparece en audio callback.
- `setSize` aparece en `prepareToPlay`/prepare helpers de pedales/cabinets y output limiter. No aparece en active registered `processBlock`.

## Riesgos restantes

- `Source/Effects/Pedals/ChorusPedal.h` y `Source/Effects/Pedals/CompressorPedal.h` son headers legacy no referenciados por `NOVA.jucer` ni `PedalRegistry`; contienen `setSize` dentro de `processBlock`. Riesgo bajo mientras sigan fuera del build/registry, pero deben eliminarse o migrarse en una fase de limpieza.
- `Source/Effects/Pedals/Wah/AutoWahPedal.h` y `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` estan en `NOVA.jucer`, pero no estan registrados como tipos activos; `Auto Wah` canonicaliza a `Wah` y `Metal Distortion` canonicaliza a `Distortion`. Ambos contienen `setSize` dentro de `processBlock`; no deben registrarse sin corregir esa allocation.
- El scan no demuestra ausencia total de heap allocations internas de JUCE, del host o del CRT. Para certeza fuerte se requiere ETW/WPA, Visual Studio allocation profiler o build diagnostica con hooks de heap.
- `applyHostTransportState()` consulta `getPlayHead()->getPosition()` durante el refresh de parametros en `processBlock`. No fue parte de los patrones prohibidos y no mostro locks locales, pero puede depender de comportamiento del host. Recomendado para auditoria P4C si se busca RT policy estricta.

## Correcciones aplicadas

- No se corrigio DSP ni rutas tonales.
- Se agrego lectura diagnostica de `OutputChainProcessor::DebugSnapshot` desde `AudioEngine` para reportar actividad de limiter en profiling offline.
- Se corrigio la lectura de fallback counters del runner P4B para usar `getRealtimeFallbackCount()`, que es el metodo expuesto por los procesadores P1.
- Se elimino un warning C4100 trivial dejando sin nombre el parametro no usado de `AudioEngine::processWithSampleAccurateDryWet`.

## Recomendacion

Mantener una regla simple para P4C/P5: ningun archivo legacy con `setSize` en `processBlock` debe entrar al registry sin pasar por el patron P1 de prepare-time allocation + realtime fallback.
