# Audio Realtime Safety Audit

Fecha: 2026-04-27
Baseline auditada: `baseline-audio-v2`
Commit auditado: `da11290f86fcbda228128143b89e2da0f4de97cc`
Alcance: busqueda estatica de riesgos en audio thread. No se aplicaron fixes en esta fase.

Leyenda de severidad:

- Critica: puede bloquear, asignar memoria o hacer trabajo claramente no RT-safe desde audio thread.
- Alta: puede asignar o hacer trabajo pesado bajo condiciones reales de host/automatizacion.
- Media: riesgo acotado, dependiente de contrato de prepare o configuracion.
- Baja: coste o patron a vigilar, sin evidencia directa de fallo.

## Hallazgos

| Archivo | Clase | Metodo | Problema | Riesgo | Severidad | Recomendacion | Estado |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `Source/Core/PluginProcessor.cpp` | `NOVAAudioProcessor` | `processBlock` -> `refreshEngineGlobalParamsIfNeeded` | El audio callback consulta runtime globals y pasa por rutas con `juce::SpinLock` (`runtimeCacheLock`, `enginePushStateLock`). | Priority inversion, bloqueo del audio thread si otro thread sostiene el lock. | Critica | Reemplazar por snapshot/atomics lock-free actualizados fuera de audio thread. | pendiente |
| `Source/Core/PluginProcessor.cpp` | `NOVAAudioProcessor` | `processBlock` -> `refreshEngineGlobalParamsIfNeeded` | Si detecta cambios, puede llamar `logRuntimeSnapshot(...)` desde el audio callback. | Construccion de strings y logger desde audio thread. | Critica | Mover logging a control thread/async queue RT-safe basada en atomics/ring buffer preasignado. | pendiente |
| `Source/Core/PluginProcessor.cpp` | `NOVAAudioProcessor` | `processBlock` -> `refreshEngineEnabledIfNeeded` | Puede tomar lock y emitir logging cuando cambia enabled. | Bloqueo y trabajo no determinista en audio callback. | Alta | Leer enabled desde atomic simple; deferir logging a message/control thread. | pendiente |
| `Source/State/SessionStore.h` | `SessionStore` | `getRuntimeGlobalParams` | Usa `juce::SpinLock::ScopedLockType` para devolver snapshot. | Lock en audio path por llamada desde `PluginProcessor::processBlock`. | Critica | Mantener copia atomica o double-buffer lock-free para audio. | pendiente |
| `Source/Core/SessionLogger.h` | `SessionLogger` | `logEvent` / `writeStructured` / `enqueue` | Usa timestamps, `juce::String`, `juce::SpinLock` y `WaitableEvent`. | No RT-safe si se llama desde audio thread. | Critica | Prohibir uso directo desde process; exponer contador atomico o ring buffer preasignado sin strings. | pendiente |
| `Source/Core/Telemetry/PedalSignalTelemetry.h` | `PedalSignalTelemetry` | `captureOutputAndEmitIfNeeded` | Analiza buffer y al emitir construye `juce::String`, consulta logger y llama `SessionLogger::logEvent`. | Glitches y locks periodicos desde procesadores DSP. | Critica | Separar "collect" RT-safe de "format/log" no-audio. En audio solo escribir counters preasignados. | pendiente |
| `Source/Effects/ChannelStrip/ChannelStripProcessor.h` | `ChannelStripProcessor` | `processBlock` | Llama telemetry con posible emision de logs. | Logging/string/lock indirecto en audio thread. | Critica | Desactivar emision directa; usar counters lock-free. | pendiente |
| `Source/Effects/OutputChain/OutputChainProcessor.h` | `OutputChainProcessor` | `processBlock` | Llama telemetry con posible emision de logs. | Logging/string/lock indirecto en audio thread; justo en defensa final. | Critica | Mover reporte de limiter/DC/spikes a snapshot lock-free. | pendiente |
| `Source/Effects/Pedals/Overdrive/OverdrivePedal.h` | `OverdrivePedal` | `processBlock` | Llama telemetry desde process. | Puede reintroducir glitches aunque DSP sea estable. | Critica | Mantener metricas atomicas; formatear fuera del audio thread. | pendiente |
| `Source/Effects/Pedals/Amps/CleanAmp.h` | `CleanAmp` | `processBlock` | Llama telemetry desde process. | Logging/string/lock indirecto y coste por ventana. | Critica | Igual que telemetry global. | pendiente |
| `Source/Effects/Pedals/Delay/DelayPedal.h` | `DelayPedal` | `processBlock` | Llama telemetry desde process. | Logging/string/lock indirecto en efecto con feedback. | Critica | Igual que telemetry global. | pendiente |
| `Source/Effects/Pedals/Flanger/FlangerPedal.h` | `FlangerPedal` | `processBlock` | Llama telemetry desde process. | Logging/string/lock indirecto en efecto con feedback. | Critica | Igual que telemetry global. | pendiente |
| `Source/Effects/Pedals/Reverb/ReverbPedal.h` | `ReverbPedal` | `processBlock` | Llama telemetry desde process. | Logging/string/lock indirecto en efecto de cola larga. | Critica | Igual que telemetry global. | pendiente |
| `Source/Core/AudioEngine.cpp` | `AudioEngine` | `processWithSampleAccurateDryWet` | Llama `dryScratch.setSize(...)` y `delayedDryScratch.setSize(...)` dentro del audio path; usa `avoidReallocating=true` y hay guard de capacidad. | Probablemente no asigna si prepare fue correcto, pero sigue siendo patron fragil y dependiente del contrato. | Media | Usar buffers preasignados y `setDataToReferTo`/views o fallback sin `setSize` en process. | requiere revision humana |
| `Source/Core/AudioEngine.cpp` | `AudioEngine` | `process` | Mide tiempo con `juce::Time::getMillisecondCounterHiRes()` por bloque. | Coste bajo/medio; puede ser aceptable en diagnostics, pero no ideal para build produccion. | Baja | Gating compile/runtime para profiler o muestreo decimado. | requiere revision humana |
| `Source/Core/AudioEngine.cpp` | `AudioEngine` | `buildGraphFromModelLocked` | Usa allocations, `dynamic_cast`, registry y graph rebuild. Actualmente se llama desde control/engine thread, no desde process. | Correcto si nunca entra al audio thread; regresion seria si se invoca desde callback. | Alta | Mantener asserts de thread y tests que impidan rebuild desde audio. | requiere revision humana |
| `Source/Core/AudioEngine.cpp` | `AudioEngine` | `handleHealthAfterBlock` | Marca atomics para reset/logging; no reconstruye directamente desde audio. | Patron aceptable si se mantiene asi. | Baja | Mantener solo flags atomicos en audio; rebuild/log fuera. | corregido |
| `Source/Effects/ProcessorBase.h` | `ProcessorBase` | `setProcessingLatency` | Si se llama tras `prepareToPlay`, puede preparar delay lines. | Allocation/reconfiguracion si un procesador cambia latencia desde audio thread. | Alta | Documentar/assert no-audio; latencia solo cambia durante prepare/rebuild/control path. | requiere revision humana |
| `Source/Effects/ProcessorBase.h` | `ProcessorBase` | `beginBypassProcess` | Tiene fallback sin allocation si el bloque supera capacidad. | Bien resuelto; posible cambio abrupto a wet/hard bypass si se excede contrato. | Baja | Mantener contador diagnostico y test de overflow. | corregido |
| `Source/Effects/Pedals/Amps/ClassicAmp.h` | `ClassicAmp` / `PremiumAmpCore` | `process` | Crea `std::vector<float*> channelData` dentro de process. | Allocation por bloque. | Critica | Reemplazar por `std::array<float*, maxChannels>` o scratch preasignado en prepare. | pendiente |
| `Source/Effects/Pedals/Amps/HighGainAmp.h` | `HighGainAmp` / core | `process` | Crea `std::vector<float*> channelData` dentro de process. | Allocation por bloque. | Critica | Igual que Classic Amp. | pendiente |
| `Source/Effects/Pedals/Amps/ChimeAmp.h` | `ChimeAmp` / core | `process` | Crea `std::vector<float*> channelData` dentro de process. | Allocation por bloque. | Critica | Igual que Classic Amp. | pendiente |
| `Source/Effects/Pedals/Amps/BoutiqueAmp.h` | `BoutiqueAmp` / core | `process` | Crea `std::vector<float*> channelData` dentro de process. | Allocation por bloque. | Critica | Igual que Classic Amp. | pendiente |
| `Source/Effects/Pedals/Amps/ClassicAmp.h` | `PremiumAmpCore` | `updateVoicingIfNeeded` | Recalcula `juce::dsp::IIR::Coefficients<float>::make*` desde process cuando cambian parametros. | Allocation/ref-count churn y posible zipper/click. | Critica | Precalcular fuera del audio thread o usar coeficientes custom value-type suavizados. | pendiente |
| `Source/Effects/Pedals/Amps/HighGainAmp.h` | core | `updateVoicingIfNeeded` | Recalcula coeficientes JUCE IIR desde process. | Allocation/ref-count churn y clicks. | Critica | Igual que Classic Amp. | pendiente |
| `Source/Effects/Pedals/Amps/ChimeAmp.h` | core | `updateVoicingIfNeeded` | Recalcula coeficientes JUCE IIR desde process. | Allocation/ref-count churn y clicks. | Critica | Igual que Classic Amp. | pendiente |
| `Source/Effects/Pedals/Amps/BoutiqueAmp.h` | core | `updateVoicingIfNeeded` | Recalcula coeficientes JUCE IIR desde process. | Allocation/ref-count churn y clicks. | Critica | Igual que Classic Amp. | pendiente |
| `Source/Effects/Pedals/Amps/CleanAmp.h` | `CleanAmp` | `updateToneFilters` | Recalcula coeficientes JUCE IIR desde process cuando cambian tone params. | Allocation/ref-count churn en un amp otherwise estable. | Critica | Mover a coeficientes custom/preasignados o cola de update no-audio. | pendiente |
| `Source/Effects/Pedals/Cabinet/CabinetPedal.h` | `CabinetPedal` | `processBlock` | `scratchBuffer.setSize(...)` si el bloque/canales exceden preparado. | Allocation en audio path bajo host contract roto o layout change. | Alta | Fallback no-allocation o preparar capacidad maxima. | pendiente |
| `Source/Effects/Pedals/Cabinet/Vintage2x12Cabinet.h` | `Vintage2x12Cabinet` | `processBlock` | `scratchBuffer.setSize(...)` si excede preparado. | Allocation en audio path. | Alta | Igual que CabinetPedal. | pendiente |
| `Source/Effects/Pedals/Cabinet/Modern4x12Cabinet.h` | `Modern4x12Cabinet` | `processBlock` | `scratchBuffer.setSize(...)` si excede preparado. | Allocation en audio path. | Alta | Igual que CabinetPedal. | pendiente |
| `Source/Effects/Pedals/Cabinet/CabinetPedal.h` | `CabinetPedal` | `updateVoicing` | Recalcula coeficientes JUCE IIR desde process. | Allocation/ref-count churn y cambios bruscos. | Alta | Coeficientes custom/preasignados o update fuera del callback. | pendiente |
| `Source/Effects/Pedals/Cabinet/Vintage2x12Cabinet.h` | `Vintage2x12Cabinet` | `updateCabinetVoicing` | Recalcula coeficientes JUCE IIR desde process. | Allocation/ref-count churn. | Alta | Igual que CabinetPedal. | pendiente |
| `Source/Effects/Pedals/Cabinet/Modern4x12Cabinet.h` | `Modern4x12Cabinet` | `updateCabinetVoicing` | Recalcula coeficientes JUCE IIR desde process. | Allocation/ref-count churn. | Alta | Igual que CabinetPedal. | pendiente |
| `Source/Effects/Pedals/Compressor/CompressorPedal.h` | `CompressorPedal` | `processBlock` | `dryBuffer.setSize(...)` si el bloque/canales exceden preparado. | Allocation bajo host contract roto/layout change. | Alta | Fallback no-allocation o capacidad maxima. | pendiente |
| `Source/Effects/Pedals/Distortion/DistortionPedal.h` | `DistortionPedal` | `processBlock` | `scratchBuffer.setSize(...)` si excede preparado. | Allocation en audio path. | Alta | Fallback no-allocation. | pendiente |
| `Source/Effects/Pedals/Fuzz/FuzzPedal.h` | `FuzzPedal` | `processBlock` | `scratchBuffer.setSize(...)` si excede preparado. | Allocation en audio path. | Alta | Fallback no-allocation. | pendiente |
| `Source/Effects/Pedals/Neural/NeuralPedal.h` | `NeuralPedal` | `processBlock` | `scratchBuffer.setSize(...)` si excede preparado. | Allocation en audio path. | Alta | Fallback no-allocation. | pendiente |
| `Source/Effects/Pedals/Neural/NeuralPedal.h` | `OnePoleFilterBank` / `EnvelopeFollower` | `ensureChannels` | `resize` de vectores si canales exceden preparado. | Allocation en process bajo layout mismatch. | Alta | Preasignar max channels o fallback no-allocation. | pendiente |
| `Source/Effects/Pedals/Wah/ClassicWahPedal.h` | `ClassicWahPedal` | `processBlock` | `scratchBuffer.setSize(...)` si excede preparado. | Allocation en audio path. | Alta | Fallback no-allocation. | pendiente |
| `Source/Effects/Pedals/AutoWahPedal.h` | `AutoWahPedal` | `processBlock` | `scratchBuffer.setSize(...)`; archivo legacy/no registrado directamente. | Riesgo si se reactiva o se usa por alias futuro. | Media | Unificar con Classic Wah o actualizar antes de registrar. | pendiente |
| `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` | `MetalDistortionPedal` | `processBlock` | `scratchBuffer.setSize(...)`; archivo legacy/no registrado directamente. | Riesgo si se reactiva o se registra. | Media | Eliminar del build si no se usa o modernizar fallback. | pendiente |
| `Source/Effects/Pedals/CompressorPedal.h` | legacy `CompressorPedal` | `processBlock` | `AudioBuffer::setSize` en process; no parece registrado. | Riesgo de deuda tecnica/uso accidental. | Media | Marcar legacy, remover de proyecto o actualizar. | pendiente |
| `Source/Effects/Pedals/ChorusPedal.h` | legacy `ChorusPedal` | `processBlock` | `AudioBuffer::setSize` en process; no parece registrado. | Riesgo de deuda tecnica/uso accidental. | Media | Marcar legacy, remover de proyecto o actualizar. | pendiente |
| `Source/Effects/Pedals/Reverb/ReverbPedal.h` | `ReverbPedal` | `processBlock` / `engine.configure` | Configuracion de engine por bloque cuando cambian parametros. No se confirmo allocation, pero es trabajo pesado. | CPU spikes/zipper bajo automatizacion intensa. | Media | Auditar internals, decimar updates, suavizar targets y medir. | requiere revision humana |
| `Source/Effects/Pedals/Delay/DelayPedal.h` | `DelayPedal` | filter/feedback updates | Updates frecuentes de filtros/feedback; no allocation visible. | CPU y zipper si automatizacion extrema. | Media | Tests de discontinuidad y profiler por block size 32/64. | requiere revision humana |
| `Source/Effects/Pedals/EQ/EQPedal.h` | `EQPedal` | processing/filter updates | Coeficientes custom se actualizan con smoothing, potencialmente frecuente. | CPU y diferencias entre block sizes si smoothing no es consistente. | Media | Tests de automation sweep y equivalencia por block size. | requiere revision humana |
| `Source/Effects/Pedals/Tremolo/TremoloPedal.h` | `TremoloPedal` | crossover/filter updates | Updates periodicos de filtros custom. | Bajo/medio; posible zipper si depth/rate cambian fuerte. | Baja | Tests de discontinuidad al automatizar. | requiere revision humana |
| `Source/Core/AudioVisualizer.h` | `AudioVisualizer` | `pushBuffer` | Copia una ventana de visualizacion desde cada `processBlock`. No se vio allocation/lock. | Coste CPU innecesario en produccion o bloques chicos. | Media | Decimar, hacer opt-in, o limitar coste por bloque. | requiere revision humana |
| `Source/Core/TunerService.h` | `TunerService` | `pushBuffer` | Usa buffers preasignados y FIFO; no allocation visible en audio. | Riesgo bajo; copiar hacia tuner puede costar CPU. | Baja | Mantener analisis fuera de audio; medir en 32/64 samples. | requiere revision humana |
| `Source/Core/PluginProcessor.cpp` | `NOVAAudioProcessor` | `parameterValueChanged` | Usa `dynamic_cast` y notifica coordinator. Puede ser llamado por host en contexto sensible. | Riesgo si el host llama listeners desde audio thread. | Media | Mantener callback ligero; usar IDs pre-mapeados y cola no-audio si es necesario. | requiere revision humana |
| Varios `setStateInformation` | Pedales/APVTS | state restore | Llaman `setValueNotifyingHost` durante restore. No se observo en process. | Correcto si solo ocurre en restore/control thread; peligroso si se usa desde audio. | Media | Asegurar contrato de thread y test de restore fuera de audio. | requiere revision humana |

## Hallazgos prioritarios

P0:

1. Eliminar logging/strings/locks de telemetry en todos los `processBlock`.
2. Eliminar locks y logging del camino `PluginProcessor::processBlock`.
3. Quitar `std::vector` allocation por bloque en Classic/HighGain/Chime/Boutique Amp.
4. Quitar recalculo de coeficientes JUCE IIR desde process en amps/cabs/CleanAmp.

P1:

1. Reemplazar todos los `AudioBuffer::setSize` condicionales dentro de process por fallback no-allocation.
2. Preasignar max channels o fallback en `NeuralPedal` helpers.
3. Auditar `ReverbPedal::engine.configure` bajo automatizacion.
4. Convertir metricas de salud a snapshot estructurado y testable.

P2:

1. Reducir coste de visualizer/profiler en modo produccion.
2. Anadir tests de discontinuidad para filtros/modulaciones.
3. Marcar o retirar pedales legacy no registrados para evitar uso accidental.

## Regla de salida para audio thread

Hasta que los P0 esten cerrados, la regla operativa debe ser:

- Sin `new/delete`.
- Sin `malloc/free`.
- Sin `std::vector::resize`, `push_back` o creacion de vectores por bloque.
- Sin `AudioBuffer::setSize`.
- Sin strings.
- Sin `SessionLogger::logEvent`.
- Sin locks/spinlocks.
- Sin graph rebuild.
- Sin `setValueNotifyingHost`.
- Sin cambios de latencia.
- Sin coeficientes JUCE IIR creados por factories dentro de process.

