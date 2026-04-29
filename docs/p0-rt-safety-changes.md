# P0 RT Safety Changes

Fecha: 2026-04-27
Baseline: `baseline-audio-v2`
Alcance: solo P0 real-time safety. No cambios intencionales de tono, UI, IDs de parametros, preset schema ni refactor grande de `AudioEngine`.

## Archivos modificados

- `Source/Core/PedalSignalTelemetry.h`
- `Source/Core/SessionLogger.h`
- `Source/Core/SessionStore.h`
- `Source/Core/PluginProcessor.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/DSP/Global/ChannelStrip.h`
- `Source/Core/DSP/Global/ChannelStrip.cpp`
- `Source/Core/DSP/Global/OutputChain.cpp`
- `Source/Effects/Pedals/Overdrive/OverdrivePedal.h`
- `Source/Effects/Amplifiers/CleanAmp.h`
- `Source/Effects/Pedals/Delay/DelayPedal.h`
- `Source/Effects/Pedals/Flanger/FlangerPedal.h`
- `Source/Effects/Pedals/Reverb/ReverbPedal.h`
- `Source/Effects/Amplifiers/ClassicAmp.h`
- `Source/Effects/Amplifiers/HighGainAmp.h`
- `Source/Effects/Amplifiers/ChimeAmp.h`
- `Source/Effects/Amplifiers/BoutiqueAmp.h`

Generated/updated by validation:

- `audio-base-test-report.txt`

## Riesgos P0 corregidos

### Telemetry / logging

- `PedalSignalTelemetry` no llama `SessionLogger`, no construye reportes `juce::String`, no consulta timestamps de logger y no despierta `WaitableEvent` desde audio thread.
- Audio thread ahora solo acumula metricas numericas y publica eventos POD en una cola fija lock-free/preasignada.
- `SessionLogger` drena esa cola desde su propio thread y alli construye strings, categorias, timestamps y escribe archivo.
- `ChannelStrip`, `OutputChain`, `Overdrive`, `CleanAmp`, `Delay`, `Flanger` y `Reverb` ya no pasan lambdas de formateo desde `processBlock`.
- Se elimino la API antigua `captureOutputAndEmitIfNeeded` para evitar reintroducir logging directo por accidente.

Metricas conservadas en el snapshot RT-safe:

- peak/rms por input/output L/R
- DC max por input/output L/R
- sample delta max
- spikeBlocks
- dcAlertBlocks
- nearClipSamples
- invalidSamples
- clippedSamples
- inputActiveBlocks
- blocks / totalSamples / windowMs aproximado por sample count

### PluginProcessor / SessionStore

- `SessionStore::getRuntimeGlobalParams()` ya no toma `runtimeCacheLock`; lee un snapshot atomico.
- `SessionStore::noteParameterValueChanged()` actualiza atomics en vez de escribir cache protegido por spinlock.
- `PluginProcessor::processBlock()` llama `refreshEngineEnabledIfNeeded(false)` y `refreshEngineGlobalParamsIfNeeded(false, false)`: no toma `enginePushStateLock` y no loggea cambios.
- El cache de "ultimo push al engine" en `PluginProcessor` usa atomics en vez de `juce::SpinLock`.

### Amps premium

- `ClassicAmp`, `HighGainAmp`, `ChimeAmp` y `BoutiqueAmp` ya no crean `std::vector<float*>` por bloque.
- Esos amps usan `std::array<float*, 8>` fija para punteros de canal.
- Las factories alloc-prone `juce::dsp::IIR::Coefficients<float>::make*` fueron reemplazadas por `juce::dsp::IIR::ArrayCoefficients<float>::make*` asignadas al estado ya preparado.
- Las formulas, frecuencias, Q y gains no se cambiaron intencionalmente.

## Riesgos pendientes

- Los reportes extra por pedal (`inputTrim`, `wetTrim`, limiter internals detallados, reverb engine internals) ya no se formatean desde audio. Queda pendiente migrarlos a campos POD extra y formatearlos en `SessionLogger`.
- `CleanAmp` todavia usa factories JUCE IIR en `updateToneFilters`; no estaba dentro del bloque de amps premium pedido para esta fase.
- Aun existen `AudioBuffer::setSize` condicionales en otros pedales/cabs fuera del alcance P0 exacto.
- `AudioVisualizer::pushBuffer` sigue copiando desde `processBlock`; no se toco en esta fase.
- `parameterValueChanged()` aun usa `dynamic_cast` y comparaciones de `juce::String`; queda para una fase posterior de host callback safety.

## Cambios que pueden afectar comportamiento

- Audio: no hay cambios intencionales de tono, gain staging, latencia musical ni parametros.
- Diagnostics: los logs de telemetry ahora contienen el snapshot comun de senal. Los detalles especificos por pedal siguen capturandose en acumuladores internos, pero se descartan al reset de ventana hasta migrarlos a campos POD serializados fuera del audio thread.
- Timing de telemetry: las ventanas usan conteo de samples en vez de `juce::Time::getMillisecondCounter()` desde audio thread.

## Como probar manualmente

1. Abrir Standalone Debug.
2. Probar chain vacia con engine on/off y verificar que el output no se silencia inesperadamente.
3. Probar `Overdrive -> Classic/HighGain/Chime/Boutique Amp -> Reverb -> Output`.
4. Automatizar global mix, input gain, output level y parametros de amp mientras suena.
5. Revisar `%APPDATA%/NOVA/Logs/session-log.txt` y confirmar eventos `pedal.private.*.window/alert` sin glitches audibles.
6. Probar block sizes bajos desde host si esta disponible: 32 y 64 samples.

## Tests ejecutados

- `MSBuild NOVA_SharedCode.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m`
  - Resultado: PASS build, 0 errors, warnings existentes de `ProcessorBase`/`TempoSyncable`/parametro no usado.
- `MSBuild NOVA_StandalonePlugin.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m`
  - Resultado: PASS build, 0 warnings, 0 errors en wrapper Standalone.
- Static checks con `rg`:
  - Sin `SessionLogger::logEvent`, `SessionLogger::getQueueStats`, `WaitableEvent`, `SpinLock::ScopedLockType` ni `captureOutputAndEmitIfNeeded` en las rutas de process auditadas.
  - Sin `std::vector<float*>` ni `IIR::Coefficients<float>::make*` en los cuatro amps premium corregidos.
- `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`
  - Resultado: FAIL, `results=133 passes=1292 failures=47 failingResults=13`.
  - Fallos reportados: InputChain single-jack level, OutputChain limiter/headroom/legacy latency expectations, global processor reset params, AudioEngine clean routing zeros, dry-only mix, disable/re-enable, latency rebuild, Phaser DC, Reverb tail/swell/reverse.

Nota: esos fallos son mas amplios que los archivos modificados en P0 y varios coinciden con expectativas ya senaladas como conflictivas en la baseline. Aun asi, la validacion automatizada no esta verde y debe tratarse como riesgo abierto antes de declarar baseline sana.

## Recomendaciones siguiente fase

1. Migrar telemetry extra por pedal a campos POD nombrables para recuperar `inputTrim`, `wetTrim`, limiter internals y reverb internals fuera del audio thread.
2. Resolver/actualizar la suite base para que distinga fallos historicos de regresiones reales.
3. Atacar `AudioBuffer::setSize` condicionales en pedales/cabs.
4. Revisar callbacks de host (`parameterValueChanged`) para eliminar trabajo no determinista.
5. Auditar `CleanAmp::updateToneFilters` y cabinets con el mismo patron `ArrayCoefficients`.
