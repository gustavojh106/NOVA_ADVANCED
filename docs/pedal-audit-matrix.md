# Pedal Audit Matrix

Fecha: 2026-04-27
Baseline auditada: `baseline-audio-v2`
Commit auditado: `da11290f86fcbda228128143b89e2da0f4de97cc`
Alcance: auditoria estatica inicial de procesadores/pedales. No se reconstruyo ningun pedal y no se cambio codigo de audio.

## P7B closure summary (2026-05-01)

Los items P0 enlistados al final de esta matriz fueron verificados y cerrados en P7B (`docs/p7b-audio-thread-rt-safety-closure-results.md`). En particular:

- Telemetry desde process en `ChannelStrip`, `OutputChain`, `Overdrive`, `CleanAmp`, `Delay`, `Flanger`, `Reverb`: ahora se publica por la cola lock-free `RealtimeSignalTelemetryQueue`. El log/format vive en el thread de logger.
- Allocations por bloque en amps `ClassicAmp`, `HighGainAmp`, `ChimeAmp`, `BoutiqueAmp`: corregidas a `std::array<float*, kMaxAmpChannels>`.
- Coeficientes JUCE IIR desde process en amps y CleanAmp: corregidos a `juce::dsp::IIR::ArrayCoefficients` (CleanAmp usa filtros custom). Cabinets se mantienen como P1.

Las reglas estan codificadas en `scripts/check-audio-thread-policy.ps1` con prefijo `p7b_*`. La matriz original sigue como evidencia historica; consultar la P7B closure para el estado vigente.

## P7C closure summary (2026-05-06)

P7C cierra los P1 activos de allocation fallback, cabinet coefficient hygiene y feedback/DC stress (`docs/p7c-allocation-fallback-and-feedback-stress-results.md`):

- `Compressor`, `Distortion`, `Fuzz`, `Neural`, `Wah`, `Cabinet`, `Vintage 2x12 Cabinet`, `Modern 4x12 Cabinet`: no-allocation fallback ante bloques/canales mayores a `prepareToPlay`.
- `Neural`: helpers por canal fijos con `std::array`; sin `ensureChannels`/`resize` en el path auditado.
- Cabinets: `ArrayCoefficients<float>` value-type, sin factories alloc-prone `IIR::Coefficients<float>::make*`.
- `Phaser`: DC accumulation protegido por blockers y regression test bajo bias sostenido.
- `Delay`, `Flanger`, `Reverb`: stress tests deterministas para DC, NaN/Inf, feedback/runaway y picos altos.

La tabla original se conserva como snapshot de auditoria inicial. El estado vigente de los items P1 cerrados esta en esta seccion y en la closure P7C.

## Criterios evaluados

Para cada pedal/procesador se reviso:

1. Allocations en `processBlock`.
2. Smoothing de parametros.
3. Control de DC.
4. Riesgo de spikes.
5. Riesgo de near clipping.
6. Riesgo de alimentar mal al `OutputChain`.
7. Gain staging interno.
8. Bypass via `ProcessorBase`.
9. Tail handling.
10. Latencia estable.
11. Telemetry util.
12. Block sizes pequenos.
13. Sample rates 44.1k, 48k, 96k.
14. Cambios bruscos de coeficientes.
15. Zipper noise/modulacion.
16. Feedback/reverb/delay runaway.
17. Output por encima de niveles razonables.

Leyenda compacta:

- OK: no se vio riesgo relevante en esta auditoria inicial.
- Riesgo: requiere fix o prueba antes de considerar 10/10.
- Revisar: no hay evidencia suficiente; requiere medicion/test.
- N/A: no aplica directamente.

## Matriz

| # | Procesador | Archivo principal | Alloc en process | Smoothing | DC / denormals | Spikes / near clip / output | Bypass / tail / latencia | Telemetry | Block/SR/coefs/modulacion | Estado y recomendacion |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Classic Amp | `Source/Effects/Pedals/Amps/ClassicAmp.h` | Riesgo: `std::vector<float*>` por bloque | OK parcial | OK: DC blocker | Riesgo: amp high gain puede empujar OutputChain | Bypass OK; tail N/A; latencia estable por oversampling | No especifica telemetry dedicada | Riesgo: JUCE IIR `make*` en process al cambiar voicing | P0: quitar allocation y coeficientes alloc-prone antes de cambios tonales |
| 2 | High Gain Amp | `Source/Effects/Pedals/Amps/HighGainAmp.h` | Riesgo: `std::vector<float*>` por bloque | OK parcial | OK: DC blocker | Riesgo alto por high gain y presencia/depth | Bypass OK; tail N/A; latencia estable | No especifica telemetry dedicada | Riesgo: JUCE IIR `make*` en process | P0: mismo fix que Classic Amp; stress test con input alto |
| 3 | Clean Amp | `Source/Effects/Pedals/Amps/CleanAmp.h` | OK en bloque normal; fallback seguro si excede capacidad | OK | OK: pre/post DC, scrub, soft limits | Mucho mejor controlado; reverb interna acotada | Bypass OK; tail/reverb interna manejada; latencia estable | Riesgo: telemetry log desde process | Riesgo: JUCE IIR `make*` en `updateToneFilters` | P0 telemetry/coefs; audio design parece bien resuelto |
| 4 | Chime Amp | `Source/Effects/Pedals/Amps/ChimeAmp.h` | Riesgo: `std::vector<float*>` por bloque | OK parcial | OK: DC blocker | Riesgo medio por bright voicing | Bypass OK; tail N/A; latencia estable | No especifica telemetry dedicada | Riesgo: JUCE IIR `make*` en process | P0: quitar allocation/coefs; test de bright spikes |
| 5 | Boutique Amp | `Source/Effects/Pedals/Amps/BoutiqueAmp.h` | Riesgo: `std::vector<float*>` por bloque | OK parcial | OK: DC blocker | Riesgo medio por sag/gain voicing | Bypass OK; tail N/A; latencia estable | No especifica telemetry dedicada | Riesgo: JUCE IIR `make*` en process | P0: quitar allocation/coefs; validar gain staging |
| 6 | Distortion | `Source/Effects/Pedals/Distortion/DistortionPedal.h` | Riesgo: `scratchBuffer.setSize` si excede preparado | OK | OK: DC block | Riesgo: clipping musical y output high gain | Bypass OK; tail N/A; latencia estable | No dedicada | Coefs custom; revisar block mismatch | P1: fallback no-allocation; test output ceiling |
| 7 | Fuzz | `Source/Effects/Pedals/Fuzz/FuzzPedal.h` | Riesgo: `scratchBuffer.setSize` si excede preparado | OK | OK: DC block | Riesgo: velcro/bias/starve puede producir jumps | Bypass OK; tail N/A; latencia estable | No dedicada | Revisar zipper con bias/starve | P1: fallback no-allocation; stress DC/picos |
| 8 | Overdrive V2 MusicalSafe | `Source/Effects/Pedals/Overdrive/OverdrivePedal.h` | OK en bloque normal; overflow sin allocation visible | OK | OK: DC cleanup fuerte | OK mejorado; output trim y soft ceilings | Bypass OK; tail N/A; latencia estable | Riesgo: telemetry log desde process | Buen enfoque para block pequeno; revisar SR matrix completa | P0 telemetry; mantener tono y usar como patron de safety |
| 9 | Cabinet / Cab Sim | `Source/Effects/Pedals/Cabinet/CabinetPedal.h` | Riesgo: `scratchBuffer.setSize` si excede preparado | OK para mix/level | Revisar: no DC cleanup dedicado post cab | Riesgo medio: resonancias/voicing pueden sumar energia | Bypass OK; tail convolution; latencia revisar | No dedicada | Riesgo: JUCE IIR `make*` en process; convolution preparada | P1: fallback no-allocation y coefs RT-safe |
| 10 | Vintage 2x12 Cabinet | `Source/Effects/Pedals/Cabinet/Vintage2x12Cabinet.h` | Riesgo: `scratchBuffer.setSize` si excede preparado | OK | Revisar DC post cab | Riesgo medio por resonancias | Bypass OK; convolution tail; latencia revisar | No dedicada | Riesgo: JUCE IIR `make*` en process | P1: igual que Cabinet |
| 11 | Modern 4x12 Cabinet | `Source/Effects/Pedals/Cabinet/Modern4x12Cabinet.h` | Riesgo: `scratchBuffer.setSize` si excede preparado | OK | Revisar DC post cab | Riesgo medio/alto con high gain | Bypass OK; convolution tail; latencia revisar | No dedicada | Riesgo: JUCE IIR `make*` en process | P1: igual que Cabinet; test con high gain |
| 12 | IR Loader | N/A registrado | N/A | N/A | N/A | N/A | N/A | N/A | No se encontro loader externo registrado; cabinets usan IR sintetico | Documentar alcance; si se agrega IR loader, exigir carga fuera de audio thread |
| 13 | Delay | `Source/Effects/Pedals/Delay/DelayPedal.h` | OK: buffers preparados | OK | OK: DC/feedback safeguards | Riesgo: feedback largo/freeze/reverse pueden acumular energia | Bypass OK; tail importante; latencia estable | Riesgo: telemetry log desde process | Revisar automation y feedback filters | P0 telemetry; P1 stress feedback/DC/NaN |
| 14 | Reverb | `Source/Effects/Pedals/Reverb/ReverbPedal.h` | OK; fallback dry si excede capacidad | OK | OK: DC cleanup y limits | Mejorado; P7H cubre automation/freeze/gate/reverse/swell bounded | Bypass OK; tail importante; latencia estable | OK: policy cubre no logging/string en processBlock | `engine.configure` auditado: cacheado por parametros, sin allocation obvia, contador diagnostico | P7H: audited/test-locked; surgery opcional futura |
| 15 | Compressor | `Source/Effects/Pedals/Compressor/CompressorPedal.h` | Riesgo: `dryBuffer.setSize` si excede preparado | OK | N/A, no nonlinear fuerte | Riesgo: makeup/parallel puede subir nivel | Bypass OK; tail/lookahead revisar; latencia estable | No dedicada | Sidechain custom; revisar 32 samples | P1: fallback no-allocation; test pumping/threshold automation |
| 16 | EQ | `Source/Effects/Pedals/EQ/EQPedal.h` | OK | OK | Revisar: EQ puede desplazar energia baja pero no DC source | Riesgo: boosts pueden near-clip | Bypass OK; tail N/A; latencia estable | No dedicada | Coefs custom actualizados frecuentemente; zipper revisar | P2: tests de automation y output gain compensation |
| 17 | Chorus | `Source/Effects/Pedals/Chorus/ChorusPedal.h` | OK: delay buffers preparados | OK | OK: feedback/DC handling | Riesgo bajo/medio por wet mix y feedback | Bypass OK; tail/mod delay; latencia estable | No dedicada | Modulation smoothing; revisar block 32 | Buen estado; agregar stress tests |
| 18 | Flanger | `Source/Effects/Pedals/Flanger/FlangerPedal.h` | OK: delay buffers preparados | OK | OK: feedback DC blockers | Riesgo: feedback resonance/spikes | Bypass OK; tail/mod delay; latencia estable | Riesgo: telemetry log desde process | Modulation/feedback requiere stress | P0 telemetry; P1 feedback runaway tests |
| 19 | Phaser | `Source/Effects/Pedals/Phaser/PhaserPedal.h` | OK | OK | Riesgo: fallo reportado de DC accumulation bajo bias | Riesgo: feedback loop puede acumular | Bypass OK; tail/feedback; latencia estable | No dedicada | Modulation smoothing; revisar block/SR | P1: reproducir/fijar test DC antes de tocar tono |
| 20 | Tremolo | `Source/Effects/Pedals/Tremolo/TremoloPedal.h` | OK | OK | N/A, no source DC fuerte | Riesgo bajo: depth/level compensation puede subir picos | Bypass OK; tail N/A; latencia estable | No dedicada | Coefs custom periodicos; zipper revisar | P2: discontinuity tests en rate/depth |
| 21 | Boost | `Source/Effects/Pedals/Boost/BoostPedal.h` | OK | OK | OK: DC block | Riesgo: por diseno puede empujar amps/OutputChain | Bypass OK; tail N/A; latencia estable | No dedicada | Coefs custom; CPU revisar oversampling | Buen estado; test de headroom en cadenas |
| 22 | Noise Gate | `Source/Effects/Pedals/Gate/NoiseGatePedal.h` | OK: lookahead preparado | OK | N/A | Riesgo bajo; puede clickear si release mal configurado | Bypass OK; tail/lookahead; latencia estable | Atomics de detector/gain; no logger | Per-sample atomics pueden costar CPU | P2: medir CPU y clicks en block 32 |
| 23 | Neural | `Source/Effects/Pedals/Neural/NeuralPedal.h` | Riesgo: `scratchBuffer.setSize` y `ensureChannels.resize` si excede preparado | OK | OK parcial: highpass/DC-ish | Riesgo medio por nonlinear/model-style drive | Bypass OK; tail N/A; latencia estable | No dedicada | Revisar block/layout mismatch | P1: preasignar/fallback; stress picos y DC |
| 24 | Wah | `Source/Effects/Pedals/Wah/ClassicWahPedal.h` | Riesgo: `scratchBuffer.setSize` si excede preparado | OK | Revisar: filtro resonante sin DC guard dedicado | Riesgo medio: resonancia puede spike | Bypass OK; tail N/A; latencia estable | No dedicada | Zipper de sweep/auto revisar | P1: fallback no-allocation; test resonant peaks |
| 25 | Octave | `Source/Effects/Pedals/Octave/OctavePedal.h` | OK | OK | OK: DC block | Riesgo medio: tracking/sub voices pueden sumar energia | Bypass OK; tail/tracker; latencia estable | No dedicada | Tracking puede variar con block pequeno | P1: tests block 32/64 y signal guitar/sine |
| 26 | Tuner tap | `Source/Core/TunerService.h` | OK: buffers preparados en constructor | N/A | N/A para salida; modo tuner silencia output | Riesgo bajo: coste de copia/captura | No usa ProcessorBase; tail N/A; latencia N/A | No logger directo en push | Revisar CPU con block 32/64; analisis corre fuera de audio | P2: mantener analisis fuera de audio y testear mute/tap |
| 27 | Auto Wah legacy | `Source/Effects/Pedals/Wah/AutoWahPedal.h` | Quarantined: `scratchBuffer.setSize` permanece solo en header legacy alias-only | OK parcial | Revisar solo si se reactiva | Riesgo medio si se registra | No activo; aliases `Auto Wah`/`Autowah`/`AutoWah` canonicalizan a `Wah` | No dedicada | No registrado en `PedalRegistry`; reemplazo activo `ClassicWahPedal` | P7G: cuarentena contractual, modernizar antes de registrar |
| 28 | Metal Distortion legacy | `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` | Quarantined: `scratchBuffer.setSize` permanece solo en header legacy alias-only | OK parcial | Revisar solo si se reactiva | Riesgo alto si se registra | No activo; alias `Metal Distortion` canonicaliza a `Distortion` | No dedicada | No registrado en `PedalRegistry`; reemplazo activo `DistortionPedal` | P7G: cuarentena contractual, modernizar antes de registrar |
| 29 | Compressor legacy | `Source/Effects/Pedals/CompressorPedal.h` | Quarantined: `setSize` permanece solo en duplicado superseded | Revisar solo si se reactiva | Revisar solo si se reactiva | Revisar solo si se reactiva | No activo; no esta en `NOVA.jucer` ni `PedalRegistry` | No dedicada | Reemplazo activo `Source/Effects/Pedals/Compressor/CompressorPedal.h` | P7G: cuarentena contractual, no tocar DSP |
| 30 | Chorus legacy | `Source/Effects/Pedals/ChorusPedal.h` | Quarantined: `setSize` permanece solo en duplicado superseded | Revisar solo si se reactiva | Revisar solo si se reactiva | Revisar solo si se reactiva | No activo; no esta en `NOVA.jucer` ni `PedalRegistry` | No dedicada | Reemplazo activo `Source/Effects/Pedals/Chorus/ChorusPedal.h` | P7G: cuarentena contractual, no tocar DSP |

## Observaciones por categoria

### Allocations en processBlock

Confirmadas o altamente probables en la auditoria inicial:

- `ClassicAmp`, `HighGainAmp`, `ChimeAmp`, `BoutiqueAmp`: `std::vector<float*>` por bloque.
- `CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet`: `scratchBuffer.setSize` si excede prepare.
- `CompressorPedal` nuevo: `dryBuffer.setSize` si excede prepare.
- `DistortionPedal`, `FuzzPedal`, `NeuralPedal`, `ClassicWahPedal`: `scratchBuffer.setSize` si excede prepare.
- `NeuralPedal` helpers: `resize` de vectores si canales exceden prepare.
- Legacy/unregistered: `AutoWahPedal`, `MetalDistortionPedal`, root `CompressorPedal`, root `ChorusPedal`.

Estado vigente despues de P7C:

- Los items activos de cabinets/compressor/distortion/fuzz/neural/wah quedaron corregidos con fallback no-allocation.
- `NeuralPedal` helpers quedaron preasignados con `std::array`.
- P7G reclasifica los hallazgos legacy/no registrados como cuarentena contractual: no son rutas de audio activas, no cuentan como WARN, y el policy falla si se registran o cambian sus alias sin actualizar la decision.

### Smoothing

La mayoria de procesadores modernos tienen smoothing para parametros principales. La deuda no es ausencia total de smoothing, sino:

- Coeficientes recalculados abruptamente o con factories alloc-prone.
- Falta de tests de discontinuidad bajo automatizacion rapida.
- Potenciales diferencias audibles entre block sizes.

### DC, spikes y runaway

Mejor cubiertos:

- `InputChain`
- `OutputChain`
- `OverdrivePedal`
- `CleanAmp`
- `ReverbPedal`
- `DelayPedal`
- `FlangerPedal`
- `BoostPedal`
- `OctavePedal`

Requieren atencion:

- `PhaserPedal`: antecedente de fallo DC bajo bias sostenido.
- Cabinets: revisar DC post-convolution/voicing.
- Wah: resonancia y ausencia de DC guard dedicado.
- Fuzz/Distortion/HighGain chains: salida puede empujar demasiado a `OutputChain`.

### Telemetry

Telemetry util pero no RT-safe en su forma actual:

- `ChannelStrip`
- `OutputChain`
- `OverdrivePedal`
- `CleanAmp`
- `DelayPedal`
- `FlangerPedal`
- `ReverbPedal`

La recomendacion es conservar el contenido diagnostico, pero cambiar el transporte:

- Audio thread: counters atomicos/ring buffer preasignado sin strings.
- Control/logger thread: formateo JSON/string y file I/O.

### Bypass, tails y latencia

`ProcessorBase` da una base consistente para bypass. Las areas a probar:

- Null test de bypass por pedal.
- Tails de delay/reverb/flanger/chorus al activar bypass.
- Latencia estable en cabinets/convolution, oversampling y limiter.
- No llamar `setProcessingLatency()` desde audio thread.

## Prioridad de fixes por pedal/procesador

P0:

1. Telemetry/logging desde process: `ChannelStrip`, `OutputChain`, `Overdrive`, `CleanAmp`, `Delay`, `Flanger`, `Reverb`.
2. Allocations por `std::vector` en `ClassicAmp`, `HighGainAmp`, `ChimeAmp`, `BoutiqueAmp`.
3. JUCE IIR coefficient factories desde process en amps/cabs/CleanAmp.

P1:

1. `AudioBuffer::setSize` fallback en cabinets, compressor, distortion, fuzz, neural, wah. **CERRADO en P7C**.
2. `NeuralPedal::ensureChannels` con resize en process. **CERRADO en P7C**.
3. Reproducir/fijar `PhaserPedal` DC accumulation. **CERRADO en P7C**.
4. Stress tests de delay/reverb/feedback con DC, NaN/Inf y picos. **CERRADO en P7C**.
5. `ReverbPedal::engine.configure` bajo automatizacion/perf. **AUDITADO en P7H** con contador diagnostico, tests deterministas y policy checks; rediseno estructural queda como opcion futura.

P2:

1. Tests de zipper/discontinuidad para EQ, Tremolo, Wah, modulations.
2. Medir CPU de visualizer, per-sample atomics y profiler en block 32.
3. Mantener la cuarentena P7G de pedales legacy/no registrados; si P7H decide reactivarlos o eliminarlos, hacerlo como cambio separado con validacion de build/source membership.

## Pruebas minimas por pedal

Cada pedal registrado debe pasar:

- Silence in/out: no NaN/Inf, no DC nuevo, no output inesperado.
- Impulse: decay/tail razonable y sin picos peligrosos.
- Sine 100 Hz, 1 kHz, 5 kHz: peak/rms razonable.
- DC offset: no acumulacion sostenida.
- Strong peaks: sin runaway ni invalid samples.
- NaN/Inf input: salida finita por scrubber propio o upstream.
- Block sizes: 32, 64, 128, 256, 480, 512.
- Sample rates: 44100, 48000, 96000.
- Parameter sweep: sin discontinuidad grande.
- Bypass null: dentro de tolerancia cuando bypassed.
- Chain stress: pedal antes/despues de amp, reverb y OutputChain.

## Recomendacion de orden

1. Arreglar transporte de telemetry para que ningun procesador loggee desde audio thread.
2. Arreglar locks/logging en `PluginProcessor::processBlock`.
3. Convertir amps/cabs a coeficientes y scratch RT-safe.
4. Convertir todos los `setSize` condicionales en process a fallback no-allocation.
5. Correr matriz automatizada por pedal.
6. Solo despues considerar cambios tonales o rebuild de procesadores.
