# PRO — Brechas técnicas del motor para un producto de estudio

**Fecha:** 2026-10-14 · **Rama:** `perf/t0-latency-optimizations` (base `09bfa81`) · **Roadmap:** `docs/latency-optimization-roadmap.md`

- **Alcance:** motor de audio, entrada, salida, integración con el DAW y dispositivos del Standalone. La interfaz queda fuera.
- **Método:** lectura del código y mediciones de B-20 (`artifacts/rt-profile-b20-final-{1..5}.json`).
- **Límites:**
  - Lo que se dice de amps y cabs sale de la estructura del código, no de pruebas de escucha.
  - Las ganancias de CPU son estimaciones, salvo las medidas.
  - Nada de esto entra antes de la beta: el motor está congelado (§11b del roadmap).

## 1. Resumen

El motor es robusto y está medido: seguridad, estado, dispositivos, regresión y CPU. Lo que separa a NOVA de un producto de estudio profesional no es la estabilidad, sino:

1. **Fidelidad del modelado:** amps y cabs (área A).
2. **Integración con el DAW:** automatización, MIDI, pistas mono y latencia fija (áreas C y F).
3. **Calidad objetiva medida:** aliasing y consistencia entre frecuencias de muestreo (área B).

CPU y latencia ya tienen sus ítems T1/T2 en el roadmap; aquí solo se referencian.

## 2. Lo que ya está sólido

| Área | Estado | Dónde |
|---|---|---|
| Arquitectura | Plano de control y plano de audio sin locks en el hilo de audio. Cambio de grafo con fades (B-11). Auto-heal y health monitor | `Source/Core/AudioEngine.cpp`, `Source/Core/Audio/` |
| Dispositivos | 44.1–192 kHz y buffers de 32 a 1024 probados. Reconexión automática. Aviso de relojes separados | E3, E4 |
| Estado | Formato de presets congelado en el esquema 1, con migración y 25 presets de referencia | E2, `docs/reports/E2-preset-format-freeze.md` |
| QA | Bit-exacto por procesador (56). Golden metrics. Base validation (349 resultados / 14274 checks). rt-profile estable. Policy scan del hilo de audio | T0-01, T0-02, B-20 |
| Latencia | ASIO (solo desarrollo, falta L1). Autopilot F1/F2. Latencia propia de la cadena típica: ~3.7 ms (limiter 2 ms + gate 1.5 ms + ~4 muestras por oversampler) | T0-03, T2-10, T2-01 |
| Salida | Limiter con lookahead siempre activo, soft ceiling, límite duro, dry/wet global alineado | P12D, `Source/Core/DSP/Global/OutputChain.*` |

**CPU** (Release, buffer 128 @ 48 kHz, mediana de 5 corridas en reposo):

| Escenario | Promedio | Pico |
|---|---|---|
| Cadena vacía | 0.67% | 0.84% |
| Overdrive + Clean Amp + Reverb | 8.75% | 11.1% |
| High-gain amp | 8.0% | 8.8% |
| Buffer 32 (misma cadena) | 8.7% | 13.8% |
| 96 kHz (misma cadena) | 16.9% | 18.4% |

Los procesadores más caros son Octave (11.8%), Distortion (9.9%) y HighGain (7.9%). Girando perillas, EQ y Boost cuestan 1.87× (T1-07).

## 3. Brechas por área

### A. Fidelidad del modelado (la brecha más grande)

| Brecha | Evidencia | Qué falta | Tier |
|---|---|---|---|
| **Amps genéricos.** Los 5 amps usan la misma receta: 3–4 `tanh` sin memoria mezclados en paralelo, shelves al final y un sag que es un detector de pico con decay fijo | `HighGainAmp.h:171-192`, `ClassicAmp.h:215-223` | Tonestack pasivo real (con interacción entre graves, medios y agudos). Filtros de acople y de cátodo entre etapas (dan el voicing y el feel del ataque). Etapa de potencia con realimentación negativa e interacción con la impedancia del parlante. Sag con constantes RC de ataque y caída. Alternativa o complemento: capturas neuronales (formato NAM) con RTNeural, que ya está vendorizado y sin usar | T2, como modelos nuevos (los actuales quedan por compatibilidad de presets) |
| **Cabs sintéticos.** 3 IR generados con 8 biquads + 2 reflexiones + ventana de 23 ms. En la práctica son una curva de EQ, no un cab microfoneado | `SyntheticIR.h:9-25` | IRs medidos. Cargador de IR del usuario (WAV con resampleo, hasta ~0.5 s). Mezcla de 2 micrófonos con alineación de fase y posición/distancia. Hoy no hay ningún lector de archivos de audio en el código (`AudioFormatManager` no aparece) | T2 (feature nueva; los cabs actuales no cambian). T1-01 baja la CPU de los actuales |
| **"Neural" no es neural** | CLAUDE.md, `Pedals/Neural/` | Inferencia real o renombrarlo | T2 |
| **Antialiasing sin medir.** 11 procesadores con oversampling 8x IIR; sin ADAA | `Oversampling<float>` en Amplifiers/* y Boost, Distortion, Fuzz, Metal, Neural, Overdrive | Medir el aliasing primero (B-2). Después, ADAA + menos oversampling | T2-04 |
| **Delay con interpolación lineal** (Chorus y Flanger usan cúbica) | `DelayPedal.h:131-134` | Interpolación de mayor orden o allpass, para modular el tiempo sin perder agudos | T1 |

### B. Consistencia y medición objetiva

| Brecha | Evidencia | Qué falta | Tier |
|---|---|---|---|
| **B-1: sonido que cambia con la frecuencia de muestreo.** E1 solo corrigió el cab. Quedan constantes por muestra | `AMP_SAG_DECAY = 0.9992f` por sub-muestra (`Constants.h:136`, usada en `ClassicAmp.h:215` y `HighGainAmp.h:176`): a 96 kHz el sag se recupera el doble de rápido que a 48 kHz. `confidence *= 0.992f` por muestra en `OctavePedal.h:149` | Auditar todos los coeficientes por muestra (no todos fallan: el release del Octave y el feedback del freeze del Delay están bien). Extender a todos los procesadores el test de E1 ("suena igual a 44.1, 48 y 96 kHz") | T1 (corrección intencional a 44.1 y 96 kHz; 48 kHz bit-idéntico) |
| **B-2: sin métricas espectrales.** Las golden miden nivel, DC, colas y null en el dominio del tiempo. La prueba de armónicos que existe declara que no mide aliasing | `docs/golden-metrics/p4-offline-qa-baseline.json`, `AudioEngineTests.cpp:1538` (`no_aliasing_claim`) | Suite de calidad: aliasing con barrido senoidal alto (energía no armónica), THD/IMD por amp y nivel, respuesta en frecuencia por procesador, piso de ruido, null test A/B entre versiones | T0 (no toca el audio; es requisito previo para A) |

### C. Entrada

| Brecha | Evidencia | Qué falta | Tier |
|---|---|---|---|
| **Sin calibración de nivel.** Los amps responden al nivel en dBFS, y cada interfaz mapea distinto los dBu a dBFS | Solo existe `inGain` (`PluginProcessor.cpp:501`) | Calibración dBFS ↔ dBu por interfaz (o un nivel de referencia del instrumento) para que un preset suene igual en cualquier equipo | T1 |
| **Transpose inexistente.** CLAUDE.md lo describe, pero `setTuningOffset` guarda un valor que nadie lee | `AudioEngine.cpp:1144-1152`; ningún otro uso de `tuningOffset` | Implementarlo (pitch shift de buena calidad, con latencia propia) o corregir CLAUDE.md | T2 (si se implementa); T0 (si se corrige la doc) |
| **Sin MIDI** | `acceptsMidi() = false` (`PluginProcessor.cpp:756`) | Program change para presets, CC/expresión (wah, volumen), MIDI learn | T1/T2 |
| **Mono duplicado** | `GraphBuilder.h:75` | Ruta mono hasta el primer efecto estéreo | T2-03 |
| **Hum** | — | Cancelador adaptativo de 50/60 Hz | T2-16 |

**Ya existe:** ganancia, gate con histéresis y hold, subsónico de 24 Hz + DC, force mono y guardas (`InputChain.h`).

### D. Salida

| Brecha | Evidencia | Qué falta | Tier |
|---|---|---|---|
| **True peak subestimado:** +0.8 a +1.4 dBTP con contenido agudo | Catmull-Rom de 4 puntos, `OutputChain.h:301-303` (B-13) | Oversampling 4x del detector (estándar ITU-R BS.1770) | T1 |
| **Lookahead de 2 ms siempre** | P12D | Modo Live F1/F2 (−3.25 ms en la cadena típica) | T2-01 |
| **Standalone: solo el primer par de salidas** | CLAUDE.md (Audio panel) | Salida DI paralela (grabar el DI y hacer reamp), salidas separadas por línea A/B | T1/T2 |

### E. Motor

| Brecha | Evidencia | Qué falta | Tier |
|---|---|---|---|
| Cambio de preset con hueco de silencio y colas de delay/reverb cortadas | `STARTUP_COUNTER_GRAPH_CHANGE`, B-11 | Grafo nuevo preparado en background + crossfade + spillover | T2-15 |
| La línea muteada se procesa igual y suma su latencia | `RoutingMixer.h:37` | Sacarla del procesamiento | T2-02 |
| Overhead del `AudioProcessorGraph` y delays de alineación | — | Chain compilado (base de T2-02, T2-03, T2-05 y T2-15) | T2-07 |
| **Latencia variable por preset:** es la suma de oversamplers y gate, y el DAW vuelve a compensar en cada cambio | `PluginProcessor.cpp:570-579` (`setLatencySamples` en cada cambio) | Reportar una latencia fija (máximo) y rellenar internamente, o mantenerla constante por sesión | T2 (nuevo) |
| CPU pendiente | §6–7 del roadmap | Lote T1 (T1-03 a T1-09, T1-13); T2-04 ADAA, T2-06 SIMD, T2-11 96 kHz adaptativo | T1/T2 |

### F. Integración con el DAW (VST3)

| Brecha | Evidencia | Qué falta | Tier |
|---|---|---|---|
| **Automatización limitada:** solo 14 parámetros globales llegan al host. Ninguna perilla de pedal, amp o cab se puede automatizar | `PluginProcessor.cpp:492-526` | Slots asignables o macros con IDs estables (compatible con E2 si solo se agregan parámetros) | T2 (estado) |
| **Solo estéreo→estéreo** | `PluginProcessor.cpp:413-415`, `:771-777` | Mono→estéreo (y mono→mono) para pistas mono en Cubase, Logic o Pro Tools. Verificar en E5 | T1 |
| **Sync al tempo solo en el Delay** | `TempoSyncable` solo en `DelayPedal.h:257` | Sync para Chorus, Flanger, Phaser y Tremolo | T1 |
| **Exportación sin modo de calidad** | `isNonRealtime()` no se usa | Más oversampling o calidad al exportar | T1 |
| Cola fija de 16 s; 1 programa | `PluginProcessor.cpp:758-768` | Cola según el preset; presets como programas del host (opcional) | T1 |
| **Plataforma:** solo Windows VST3 | `NOVA.jucer` | AU/macOS, AAX, CLAP (fuera del motor, pero define "estudio profesional") | — |

### G. Dispositivos (Standalone)

| Brecha | Qué falta | Ítem |
|---|---|---|
| Entrada y salida en aparatos distintos: hoy se detecta y se avisa (E4) | Puente de relojes con resampler adaptativo | T2-09 |
| Latencia estimada, no medida | Medición con cable de loopback | T1-12 |
| Autopilot sin re-detección ni memoria por equipo | Autopilot F3 | T2-10 |
| Windows puede degradar el proceso en segundo plano | EcoQoS | T1-11 |
| Hilos de audio sin afinidad | Scheduling (P-cores, `VirtualLock`) | T2-14 |
| ASIO solo en desarrollo | Firmar el acuerdo de Steinberg | L1 |

## 4. Orden recomendado (después del congelamiento)

1. **B-2, suite de medición objetiva (T0).** Sin ella no se puede validar nada de A ni de B-1.
2. **B-1, consistencia entre frecuencias, y true peak (T1).**
3. **Integración profesional mínima:** mono→estéreo, MIDI (PC/CC), parámetros automatizables, sync de los LFOs al tempo y latencia fija.
4. **Cargador de IR** con IRs medidos y 2 micrófonos.
5. **Nueva generación de amps:** modelado por componentes o capturas NAM vía RTNeural, como modelos nuevos.
6. **Motor:** T2-07 y después T2-03, T2-02 y T2-15; modo Live F1/F2.
7. **Lote de CPU T1,** validado con bit-exacto y null tests.

## 5. Correcciones de documentación detectadas

- CLAUDE.md dice que InputChain incluye transpose por pitch shift. No existe: `tuningOffset` se guarda y nadie lo lee. Hay que corregir la doc o implementarlo (C).
- CLAUDE.md llama "Neural" a un pedal que no hace inferencia. Ya lo aclara en el párrafo de la estructura de pedales, pero el nombre puede confundir a los usuarios (A).
