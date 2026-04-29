# Audio Baseline v2 Report

Fecha: 2026-04-27
Baseline auditada: `baseline-audio-v2`
Commit auditado: `da11290f86fcbda228128143b89e2da0f4de97cc`
Alcance: diagnostico de arquitectura, audio path, procesadores DSP, telemetry y pruebas existentes. No se modifico codigo de audio en esta fase.

## 1. Resumen ejecutivo

`baseline-audio-v2` es una base claramente mas estable que las versiones historicas: el motor ya usa intercambio de graph mas seguro, `ProcessorBase` centraliza bypass con crossfade y compensacion opcional de latencia, y las cadenas `InputChain`, `ChannelStrip`, `OutputChain`, `CleanAmp`, `ReverbPedal` y `OverdrivePedal` muestran trabajo especifico para scrubbers, DC, gain staging y limitacion de energia.

La base todavia no esta en nivel 10/10 profesional porque quedan riesgos importantes de real-time safety y consistencia: hay telemetry/logging directo desde `processBlock`, locks en el camino de `PluginProcessor::processBlock`, posibles allocations por `juce::AudioBuffer::setSize`, `std::vector` en varios amps, y recalculo de coeficientes JUCE IIR dentro del bloque de audio. Esos problemas no necesariamente explican el "helicoptero" por si solos, pero si pueden producir glitches, drops, picos, bombeo del limiter o comportamiento no determinista bajo automatizacion.

Estado de validacion observado:

- `audio-validation-report.txt`: PASS, 115 resultados, 1034 passes, 0 failures.
- `audio-validation-report-current.txt`: FAIL, 117 resultados, 1040 passes, 4 failures, 1 resultado fallido: `Processor switcher cycles through all three routing modes`.
- `audio-base-test-report.txt`: FAIL historico/relevante: `PhaserPedal feedback loop rejects DC accumulation under sustained bias`.

Conclusion: baseline razonable para auditar y endurecer, pero no debe tratarse como cerrada. La prioridad inmediata debe ser eliminar riesgos RT antes de nuevos features visuales.

## 2. Piezas actuales del sistema de audio

### AudioEngine

Archivos principales:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngineTests.cpp`

Responsabilidades actuales:

- Mantiene el `juce::AudioProcessorGraph`.
- Construye runtime graph desde el modelo de cadena.
- Gestiona Line A/B, modos `LineA_Only`, `LineB_Only`, `Dual_Parallel`.
- Intercambia graphs mediante puntero atomico y retiro diferido.
- Aplica global dry/wet con ramp sample-accurate.
- Mantiene metering, profiling CPU/process time, health checks y sanitation.
- Recibe comandos de graph desde control plane.
- Gestiona tuner tap y modo tuner.
- Reporta latencia del runtime graph.
- Ejecuta un thread propio para flushing de comandos, cleanup de graphs retirados, logging pendiente y tuner analysis.

Estado general: arquitectura avanzada y con separacion parcial control/audio, pero `AudioEngine` concentra demasiadas responsabilidades. Hay areas ya protegidas contra rebuild en audio thread, pero quedan riesgos en scratch buffers y diagnosticos.

### ProcessorBase

Archivo principal:

- `Source/Effects/ProcessorBase.h`

Responsabilidades actuales:

- Interface comun para procesadores NOVA.
- Bypass crossfade.
- Bypass latency compensation opcional.
- Fallback no-allocation si el bloque supera la capacidad preparada.
- Estado XML generico para parametros.

Estado general: una base solida para bypass y estado. Riesgo: `setProcessingLatency()` puede preparar delay lines si se llama despues de `prepareToPlay`; debe mantenerse fuera de audio thread y auditarse con asserts/documentacion.

### InputChain

Archivo principal:

- `Source/Effects/InputChain/InputChainProcessor.h`

Funciones:

- Scrub de NaN/Inf/denormals/clamps mediante `SignalGuard`.
- Auto routing de una entrada mono hacia estereo si corresponde.
- Force mono con smoothing.
- DC/subsonic conditioning.
- Input gain smoothing.
- Input gate smoothing.
- Contadores atomicos de invalid/clipped/denormal.

Estado general: bien encaminado. No se observaron allocations ni locks directos en `processBlock`.

### ChannelStrip

Archivo principal:

- `Source/Effects/ChannelStrip/ChannelStripProcessor.h`

Funciones:

- Gain/pan/width con smoothing.
- Mute y control por linea.
- Scrub de salida.
- Telemetry por ventana.

Estado general: DSP util y simple. Riesgo principal: telemetry puede construir strings y llamar logger desde audio thread.

### OutputChain

Archivo principal:

- `Source/Effects/OutputChain/OutputChainProcessor.h`

Funciones:

- DC blockers.
- Master gain smoothing.
- Limiter lookahead estable.
- Soft ceiling final.
- Telemetry de output, limiter, DC, spikes y near clip.
- Latencia de limiter preparada.

Estado general: es la defensa final mas madura del sistema. Riesgo: no debe ser la unica defensa contra pedales peligrosos; la telemetry actual no es RT-safe si se emite desde audio thread.

### Pedales

Registrados en `Source/Effects/PedalRegistry.h`:

- Compressor
- Boost
- Noise Gate
- EQ
- Neural
- Overdrive
- Distortion
- Fuzz
- Wah
- Octave
- Chorus
- Phaser
- Flanger
- Tremolo
- Delay
- Reverb
- Classic Amp
- High Gain Amp
- Clean Amp
- Chime Amp
- Boutique Amp
- Cabinet
- Vintage 2x12
- Modern 4x12

Alias/catalogo en `Source/Effects/PedalCatalog.h`:

- `Auto Wah` apunta al tipo `Wah`.
- `Metal Distortion` apunta al tipo `Distortion`.

Archivos legacy o no registrados directamente encontrados:

- `Source/Effects/Pedals/AutoWahPedal.h`
- `Source/Effects/Pedals/Metal/MetalDistortionPedal.h`
- `Source/Effects/Pedals/CompressorPedal.h`
- `Source/Effects/Pedals/ChorusPedal.h`

Estado general: suite amplia y musicalmente ambiciosa. Hay diferencias fuertes de madurez entre procesadores: `OverdrivePedal` y `CleanAmp` tienen defensas modernas, mientras varios amps/cabs y pedales legacy todavia tienen patrones peligrosos en `processBlock`.

### Amps

Archivos:

- `Source/Effects/Pedals/Amps/ClassicAmp.h`
- `Source/Effects/Pedals/Amps/HighGainAmp.h`
- `Source/Effects/Pedals/Amps/CleanAmp.h`
- `Source/Effects/Pedals/Amps/ChimeAmp.h`
- `Source/Effects/Pedals/Amps/BoutiqueAmp.h`

Estado general: `CleanAmp` fue reconstruido con defensas fuertes de gain staging, DC y reverb interna. Los otros amps usan oversampling y DC blocking, pero hacen allocation con `std::vector<float*>` dentro de process y recalculan coeficientes JUCE IIR dentro del audio block cuando cambian parametros.

### Cabinets / Cab Sim / IR

Archivos:

- `Source/Effects/Pedals/Cabinet/CabinetPedal.h`
- `Source/Effects/Pedals/Cabinet/Vintage2x12Cabinet.h`
- `Source/Effects/Pedals/Cabinet/Modern4x12Cabinet.h`
- `Source/Effects/Pedals/Cabinet/SyntheticIR.h`

Estado general: no se encontro un IR Loader externo registrado; los cabinets usan IR sintetico/convolucion interna. Riesgos: `setSize` de scratch buffer si cambia el tamano de bloque/layout en process y recalculo de coeficientes JUCE IIR en process.

### Reverbs

Archivo:

- `Source/Effects/Pedals/Reverb/ReverbPedal.h`

Estado general: mucho mas estable que el historico: trim interno, DC cleanup, soft limits, bounded feedback/freeze, fallback dry si el bloque supera capacidad preparada. Riesgo principal: telemetry desde process y configuracion pesada por bloque si cambian parametros.

### Delays / Modulations

Archivos:

- `Source/Effects/Pedals/Delay/DelayPedal.h`
- `Source/Effects/Pedals/Chorus/ChorusPedal.h`
- `Source/Effects/Pedals/Flanger/FlangerPedal.h`
- `Source/Effects/Pedals/Phaser/PhaserPedal.h`
- `Source/Effects/Pedals/Tremolo/TremoloPedal.h`

Estado general: delay y modulations tienen smoothing y feedback control. `Delay` y `Flanger` emiten telemetry desde audio thread. `Phaser` requiere atencion por el fallo historico de DC accumulation bajo bias sostenido.

### Tuner

Archivos:

- `Source/Core/TunerService.h`
- uso desde `AudioEngine`

Estado general: `TunerService` preasigna buffers en constructor, `pushBuffer()` desde audio thread usa FIFO y copia sin allocation visible, y `process()` corre en thread de engine. El modo tuner silencia salida despues de alimentar el servicio. Riesgo bajo, pero conviene medir CPU y comportamiento con bloques pequenos.

### Telemetry / Logger

Archivos:

- `Source/Core/Telemetry/PedalSignalTelemetry.h`
- `Source/Core/SessionLogger.h`
- `Source/Core/DSP/SignalGuard.h`

Estado general: la cobertura diagnostica es buena y captura las metricas correctas para el problema historico. El problema es el lugar de ejecucion: `PedalSignalTelemetry::captureOutputAndEmitIfNeeded()` puede construir `juce::String` y llamar `SessionLogger::logEvent()` desde `processBlock`. `SessionLogger` usa `juce::SpinLock`, timestamps, strings y `WaitableEvent`. Esto debe salir del audio thread.

### Preset / State Restore

Archivos principales:

- `Source/State/SessionStore.h`
- `Source/State/SessionCoordinator.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/PluginStateBridge.h`
- `Source/Model/ChainModel.h`

Estado general: existe modelo de cadena, parametros globales runtime y persistencia de estado. Riesgo: `PluginProcessor::processBlock()` consulta runtime globals por rutas con spinlocks y puede loggear snapshots cuando cambian parametros. Eso debe migrar a atomics/snapshots lock-free para audio thread.

## 3. Evaluacion del estado actual de la base de audio

Fortalezas:

- Graph swapping con publicacion atomica y retiro diferido.
- Separacion parcial entre control thread y audio thread.
- `ProcessorBase` da bypass coherente.
- `InputChain` hace scrub y condicionamiento temprano.
- `OutputChain` tiene limiter/lookahead y telemetry de salud.
- `OverdrivePedal V2` ataca DC, spikes y gain staging con enfoque musical.
- `CleanAmp` y `ReverbPedal` tienen mejores defensas contra acumulacion de energia.
- Hay una suite grande de `AudioEngineTests.cpp`, con tests por pedales, sample rates y escenarios de estabilidad.

Debilidades:

- No hay garantia RT-safe end-to-end.
- Telemetry actual puede romper audio thread bajo ventanas/alertas.
- Algunos procesadores allocation-prone permanecen registrados.
- Recalculo de coeficientes con JUCE IIR dentro de process puede asignar memoria.
- La suite de tests no cubre aun toda la matriz solicitada de block sizes, sample rates, senales y cadenas combinadas.
- Las metricas existen dispersas en logs/telemetry, pero no como API de captura estructurada para tests automatizados.

## 4. Riesgos principales todavia presentes

1. Telemetry/logging desde audio thread.
   Riesgo: glitches, priority inversion, CPU spikes, drops y comportamiento no determinista.

2. Locks en el camino de `PluginProcessor::processBlock()`.
   Riesgo: bloqueo del audio callback si otro thread sostiene `enginePushStateLock` o `runtimeCacheLock`.

3. Allocations por `std::vector<float*>` en amps.
   Riesgo: allocation por bloque y latencia no determinista.

4. `juce::dsp::IIR::Coefficients<float>::make*` en process cuando cambian parametros.
   Riesgo: allocation/ref-count churn y clicks si los coeficientes cambian bruscamente.

5. `juce::AudioBuffer::setSize` en process como fallback de capacidad.
   Riesgo: allocation si host entrega bloque/layout mayor que lo preparado.

6. Feedback/reverb/delay todavia necesitan stress tests con DC offset, picos, NaN/Inf y cambios de parametros.
   Riesgo: acumulacion de energia y activacion excesiva del limiter.

7. Falla actual en test de switcher de routing.
   Riesgo: discrepancia de estado/UI/engine en modos Line A/B/Dual.

8. Phaser con antecedente de fallo DC.
   Riesgo: DC acumulada en feedback bajo bias sostenido.

## 5. Cosas que ya parecen bien resueltas

- El rebuild de graph no se ejecuta directamente desde audio thread.
- `AudioEngine::synchronizeProcessingState()` evita sincronizar desde audio thread.
- El graph publicado se lee por puntero atomico en audio path.
- `InputChain` tiene scrub temprano de NaN/Inf/denormals.
- `OutputChain` contiene defensa final con limiter y soft ceiling.
- `ProcessorBase` evita bypass hard-click en la ruta normal.
- `OverdrivePedal` redujo el problema historico de "helicoptero" sin destruir completamente el tono.
- `CleanAmp` maneja reverb interna y return levels con mas limites que versiones previas.
- `ReverbPedal` tiene gain staging interno mas conservador.
- `TunerService` separa captura en audio thread y analisis en thread no-audio.

## 6. Areas que impiden llegar a 10/10

- RT safety end-to-end.
- Telemetry diagnostica lock-free y sin strings en audio thread.
- Snapshot de parametros globales sin locks ni logging en `processBlock`.
- Filtros con coeficientes preasignados o custom structs sin allocation.
- Politica uniforme para bloque/layout mayor que `prepareToPlay`: no allocation, fallback seguro, contador diagnostico.
- Test matrix completa por block size, sample rate, senal, routing y cadena.
- Captura estructurada de metricas para comparar builds.
- Reglas consistentes de output ceiling por pedal, no solo en `OutputChain`.
- Validacion de bypass/null para cada procesador.
- Refactor incremental de `AudioEngine` para separar graph, parametros, routing, latency, diagnostics y health.

## 7. Archivos criticos

Motor y plugin:

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/PluginProcessor.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `Source/Model/ChainModel.h`
- `Source/State/SessionStore.h`
- `Source/State/SessionCoordinator.h`

Base DSP:

- `Source/Effects/ProcessorBase.h`
- `Source/Core/DSP/SignalGuard.h`
- `Source/Core/Telemetry/PedalSignalTelemetry.h`
- `Source/Core/SessionLogger.h`

Cadenas globales:

- `Source/Effects/InputChain/InputChainProcessor.h`
- `Source/Effects/ChannelStrip/ChannelStripProcessor.h`
- `Source/Effects/OutputChain/OutputChainProcessor.h`

Pedal registry/catalog:

- `Source/Effects/PedalRegistry.h`
- `Source/Effects/PedalCatalog.h`

Procesadores de alto riesgo:

- `Source/Effects/Pedals/Amps/ClassicAmp.h`
- `Source/Effects/Pedals/Amps/HighGainAmp.h`
- `Source/Effects/Pedals/Amps/CleanAmp.h`
- `Source/Effects/Pedals/Amps/ChimeAmp.h`
- `Source/Effects/Pedals/Amps/BoutiqueAmp.h`
- `Source/Effects/Pedals/Cabinet/CabinetPedal.h`
- `Source/Effects/Pedals/Cabinet/Vintage2x12Cabinet.h`
- `Source/Effects/Pedals/Cabinet/Modern4x12Cabinet.h`
- `Source/Effects/Pedals/Delay/DelayPedal.h`
- `Source/Effects/Pedals/Reverb/ReverbPedal.h`
- `Source/Effects/Pedals/Overdrive/OverdrivePedal.h`
- `Source/Effects/Pedals/Distortion/DistortionPedal.h`
- `Source/Effects/Pedals/Fuzz/FuzzPedal.h`
- `Source/Effects/Pedals/Neural/NeuralPedal.h`
- `Source/Effects/Pedals/Phaser/PhaserPedal.h`
- `Source/Effects/Pedals/Flanger/FlangerPedal.h`

## 8. Pruebas minimas necesarias

Matriz base:

- Block sizes: 32, 64, 128, 256, 480, 512.
- Sample rates: 44100, 48000, 96000.
- Canales: mono input, stereo input, single-jack L, single-jack R.

Senales:

- Silencio.
- Impulso.
- Sine 100 Hz.
- Sine 1 kHz.
- Sine 5 kHz.
- Ruido blanco bajo.
- Ruido rosa si existe helper.
- Guitarra real si existe asset.
- Sweep de frecuencia.
- DC offset sostenido.
- Picos fuertes.
- NaN/Inf intercalados.

Casos funcionales:

- Engine off/on.
- Bypass on/off por pedal.
- Cambio de parametros durante audio.
- Agregar/quitar pedal si el sistema lo permite.
- Cambio `LineA_Only`, `LineB_Only`, `Dual_Parallel`.
- Global mix 0/50/100.
- Input gain alto.
- Output limiter off, -6, -12.
- Chain vacia.
- Chain con amp.
- Chain con amp + reverb.
- Chain con overdrive + amp + reverb.
- Chain con delay/reverb largos.

Asserts minimos:

- Cero NaN/Inf al output.
- Cero denormals persistentes.
- DC output bajo umbral.
- Output peak no supera umbral peligroso sostenido.
- Limiter no toca muestras en casos nominales donde no debe.
- Bypass null dentro de tolerancia.
- Sin discontinuidades grandes al automatizar parametros.
- Diferencias entre block sizes dentro de tolerancia para cadenas deterministas.
- Latencia reportada estable por configuracion.

## 9. Metricas de referencia para comparar futuras versiones

| Metrica | Fuente actual/propuesta | Criterio de baseline sano |
| --- | --- | --- |
| CPU avg | `AudioEngine` profiler | Registrar por preset/cadena; sin spikes visibles en cadenas nominales |
| CPU peak | `AudioEngine` profiler | Debe mantenerse con margen en 32/64 samples |
| Process time avg | `AudioEngine` profiler | Menor que duracion de bloque con margen amplio |
| Process time peak | `AudioEngine` profiler | Sin picos al automatizar parametros |
| limiterTouchedSamples | `OutputChain` telemetry | 0 o bajo en cadenas nominales; no sostenido |
| limiterMaxReductionDb | `OutputChain` telemetry | 0 dB en nominal; bajo y no sostenido en high gain |
| dcAlertBlocks | telemetry/health | 0 en nominal; fallar test si sostenido |
| spikeBlocks | telemetry/health | 0 en nominal; investigar cualquier repeticion |
| nearClipSamples | telemetry | 0 en nominal; bajo en casos extremos |
| invalidSamples | `SignalGuard`/InputChain | 0 despues de scrub; entrada NaN/Inf debe limpiarse |
| clippedSamples | `SignalGuard`/InputChain | 0 en nominal; detectar entradas peligrosas |
| denormalSamples | `SignalGuard`/InputChain | 0 sostenido |
| safetyTouchedSamples | pedales/OutputChain | Bajo; no debe ocultar runaway interno |
| output peak | `AudioEngine`/meter | <= 1.0 despues de OutputChain; margen nominal recomendado |
| output rms | test helper/telemetry | Razonable por cadena; detectar pumping |
| latencia reportada | `AudioEngine::getLatencySamples()` | Estable por cadena; cambia solo en rebuild/control path |

Notas:

- Hoy varias metricas existen en logs/telemetry, pero no todas estan centralizadas para tests.
- La siguiente fase de test deberia exponer un snapshot de metricas sin strings ni locks desde audio thread.
- Para comparar futuras versiones, guardar fixtures por cadena y reportes por build.

## 10. Checklist de baseline-audio-v2 sana

- [ ] Worktree en commit `da11290f86fcbda228128143b89e2da0f4de97cc` o equivalente.
- [ ] `NOVA.jucer` no modificado manualmente fuera del flujo normal.
- [ ] Tests actuales corren sin fallas nuevas.
- [ ] Revisar y resolver/fijar expectativa del fallo `Processor switcher cycles through all three routing modes`.
- [ ] Re-ejecutar test de Phaser DC accumulation y confirmar estado actual.
- [ ] Abrir chain vacia y verificar output silencioso/estable.
- [ ] Probar input mono L/R y auto routing.
- [ ] Probar global mix 0/50/100 sin clicks.
- [ ] Probar bypass por pedal sin jumps de nivel.
- [ ] Probar overdrive + amp + reverb con limiter observando `limiterTouchedSamples`.
- [ ] Probar delay/reverb largo con feedback alto y DC offset.
- [ ] Confirmar que no hay NaN/Inf al output ante senal contaminada.
- [ ] Confirmar latencia estable despues de rebuild de graph.
- [ ] Confirmar que telemetry diagnostica no causa glitches visibles.
- [ ] No avanzar a features UI hasta cerrar P0 RT safety.

