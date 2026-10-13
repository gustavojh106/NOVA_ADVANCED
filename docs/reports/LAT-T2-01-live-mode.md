# LAT-T2-01 — Modo Live (sin lookaheads): investigación

- **Fecha:** 2026-10-11
- **Estado:** decisiones tomadas, **Fase 0 terminada** (sección 11.1) y **B-11 arreglado** (sección 11.2), ambos el 2026-10-12. Siguiente: F1 (Standalone).
- **Roadmap:** `docs/latency-optimization-roadmap.md` → T2-01
- **Alcance:** Standalone y VST3. Solo latencia propia de NOVA (DSP); la del driver es T2-10.

## Resumen

- **Hoy NOVA agrega 3.67 ms** en una cadena típica (Gate + Overdrive + amp + cab + reverb) a 44.1 y 48 kHz, y 2.0 ms con la cadena vacía. El 95% viene de dos lookaheads: limiter de salida (2.0 ms) y Noise Gate (1.5 ms). El oversampling suma 4 muestras por procesador (0.08 ms).
- **Modo Live recomendado:** limiter con la ruta `processWithoutLookahead` que ya existe + Noise Gate con lookahead corto de 0.25 ms. Resultado: **−3.25 ms** (3.67 → 0.42 ms); con el gate en 0 ms, −3.5 ms.
- **Seguridad:** en la simulación, el limiter sin lookahead mantiene el techo de P12D (pico de muestra ≤ −0.26 dBFS, soft ceiling sin tocar, el preset de master +10.73 dB queda limpio). Con material post-cab, el true peak queda igual que en Studio (±0.1 dB). Un soft-clipper solo **no** sirve: con overs sostenidos distorsiona ~20–25% (residuo −14 dB contra −66 dB), es decir, el síntoma que P12D corrigió.
- **Lo que cambia en el sonido:** solo cuando el limiter actúa. En el primer flanco de un pico, el limiter recorta la subida en vez de bajar la ganancia 2 ms antes. Los presets nominales nunca llegan al limiter (P9F), así que para ellos Live suena idéntico.
- **DAW:** ningún host garantiza un cambio de latencia limpio con el transporte andando. En VST3, el modo es por instancia, no automatizable, y se aplica con el transporte detenido.

## 1. Objetivo y criterio de éxito

**Objetivo:** un modo Live que quite la latencia propia de NOVA sin perder las garantías de P12D (techo de salida) y sin clicks nuevos en el gate.

**Criterio de éxito (medible):**

| # | Criterio | Valor | Por qué ese valor |
|---|---|---|---|
| C1 | Latencia propia en Live (`latency.breakdown`) | `outputChain=0`; Noise Gate ≤ `round(0.00025·fs)` (12 @ 48k; 0 si se elige la variante máxima); **X = 4 muestras por procesador con oversampling (5 en CleanAmp) + gate**. Cadena típica ≤ 20 muestras @ 48k (0.42 ms); cadena vacía = 0 | Lo que queda es oversampling (≤ 0.1 ms por procesador). Bajarlo es T1-05 / T2-05: menos de 1 muestra de ganancia por procesador, no justifica más riesgo aquí |
| C2 | Pico de muestra a la salida | **Y = 0.0 dB** por encima del techo de 0.97 (−0.26 dBFS), en todas las señales de peor caso, y `softCeilingTouchedSamples = 0` en el escenario P12D | Es la garantía de P12D. Sin lookahead se cumple por construcción: la ganancia se calcula con la misma muestra que se atenúa |
| C3 | True peak (oversampling 8x) frente a Studio | ≤ Studio + 0.5 dB en material post-cab | 0.5 dB es del orden del umbral de diferencia de nivel; la simulación da +0.1 dB como máximo. El techo de muestra (C2) es lo que protege oídos y equipo |
| C4 | Distorsión con overs sostenidos (escenario P12D) | Residuo ≤ −60 dB (Studio: −66 dB) | Distingue un limiter (ganancia) de un clipper (−14 dB) |
| C5 | "Salpicadura" en los ataques (energía nueva > 9 kHz) | ≤ entrada + 1.5 dB | Proxy objetivo de click. Studio da +0.1 a +0.7 dB; un clipper, +8 a +14 dB |
| C6 | Ataque del gate (primer ms) | Pérdida ≤ 1 dB en notas ≥ 10 dB sobre el umbral; ≤ 2.5 dB cerca del umbral | Sin lookahead no puede ser 0. Valores de la variante recomendada en la simulación (sección 4) |
| C7 | Prueba ABX de clicks (gate y limiter) | ≤ 11/16 aciertos (p > 0.1) | Si nadie distingue con fiabilidad, no hay click audible |
| C8 | Con Live apagado | Bit-exacto 48/48 en Debug y Release, golden y base validation PASS | Studio no debe cambiar ni un bit |

## 2. Inventario de latencia y ataque percibido

### 2.1 Latencia real por nodo

| Nodo | 44.1 kHz | 48 kHz | 96 kHz | Dónde |
|---|---|---|---|---|
| Limiter de OutputChain (lookahead 2.0 ms, siempre armado) | 88 | 96 | 192 | `Source/Core/DSP/Global/OutputChain.h:263`, `OutputChain.h:153`, `OutputChain.cpp:184-188` |
| Noise Gate pedal (lookahead 1.5 ms, `jlimit(16, 192, …)`) | 66 | 72 | 144 | `Source/Effects/Pedals/Gate/NoiseGatePedal.h:219`, `:226` |
| Oversampling 8x IIR (11 procesadores) | 4 | 4 | 4 | Real 4.076 muestras a cualquier frecuencia (ver 2.2). Se reporta 4 con `(int)` o `std::round`; **CleanAmp reporta 5** (`std::ceil`, `CleanAmp.h:126`) |
| Cabinets (Cabinet, Modern 4x12, Vintage 2x12) | 0 | 0 | 0 | `juce::dsp::Convolution` por defecto = latencia cero (`juce_Convolution.h:94-97`, `:116`); el IR sintético arranca en la muestra 0 (`SyntheticIR.h:152`) |
| InputChain, ChannelStrip, Octave, Reverb, Delay, modulación, Compressor, EQ, Wah | 0 | 0 | 0 | No llaman a `setProcessingLatency` (Octave reporta 0: `OctavePedal.h:355`) |

**Oversampling por procesador:** Boost, Distortion, Fuzz, Metal, Neural, Overdrive y los 5 amps, todos con `Oversampling(2, 3, filterHalfBandPolyphaseIIR)` (`isMaxQuality = true`, latencia fraccional).

### 2.2 Mediciones

| Fuente | Resultado |
|---|---|
| Log de sesión de la app (`%APPDATA%/NOVA/Logs/session-log.txt`, 2026-10-09, corridas de validación con cadenas vacías, 48 kHz) | `latency.breakdown: graph=96 samples (2.00 ms) \| inputChain=0 \| lineA=0 \| lineB=0 \| outputChain=96` |
| Harness VST3 en JUCE AudioPluginHost 8.0.12 (`artifacts/nova-host-vst3-pdc-validation.log`, 2026-07-14) | Sin pedales: **88 / 96 / 192** muestras @ 44.1 / 48 / 96k. Con Overdrive: **92 / 100 / 196** (+4). El pico del impulso aparece 2–3 muestras después de la latencia reportada (dispersión de fase de los filtros del pedal + la fracción 0.076) |
| Réplica en Python del diseño halfband de JUCE (`juce_FilterDesign.cpp:619-700`, `juce_Oversampling.cpp:560-575`, `:647-665`) | 8x con `isMaxQuality = true`: **4.076** muestras base (etapas: 5.27, 4.18 y 3.15 muestras a su propia frecuencia). Con `isMaxQuality = false` (T1-05): 2.95. Coincide con el +4 medido en el host |

**Limitación:** ningún script existente renderiza `latency.breakdown` para una cadena con pedales (el log solo tiene cadenas vacías). La cadena típica se calcula desde el código y se valida con la medición del Overdrive en el host. La Fase 0 agrega un test que imprime `buildDiagnosticReport()` con esa cadena a 44.1, 48 y 96 kHz.

### 2.3 Cadena típica: Gate + Overdrive + HighGain amp + cab + reverb

| | 44.1 kHz | 48 kHz | 96 kHz |
|---|---|---|---|
| **Studio (hoy)** | 66 + 4 + 4 + 0 + 0 + 88 = 162 (**3.67 ms**) | 72 + 4 + 4 + 96 = 176 (**3.67 ms**) | 144 + 8 + 192 = 344 (**3.58 ms**) |
| **Live recomendado** (limiter 0, gate 0.25 ms) | 11 + 8 = 19 (**0.43 ms**) | 12 + 8 = 20 (**0.42 ms**) | 24 + 8 = 32 (**0.33 ms**) |
| **Live máximo** (limiter 0, gate 0) | 8 (0.18 ms) | 8 (0.17 ms) | 8 (0.08 ms) |
| Cadena vacía, Studio → Live | 2.00 → 0 ms | 2.00 → 0 ms | 2.00 → 0 ms |

Round trip con ASIO, buffer 64 @ 48 kHz: **~8.5 → ~5.2 ms** (Live recomendado) o ~5.0 ms (máximo), según la base del roadmap (§10).

### 2.4 Alineación: grafo, DryWetMixer y host

| Mecanismo | Cómo funciona | Efecto del modo Live |
|---|---|---|
| PDC interno del grafo | `AudioProcessorGraph` retrasa la línea más rápida para alinearla con la más lenta: `grafo = input + max(lineA, lineB) + output` (`DiagnosticsManager.h:211-215`). La latencia se captura al construir (`GraphBuilder.h:105`) | Baja junto con los lookaheads. Con una línea muteada que tiene Gate (B-02 / T2-02), la línea activa hoy espera 72 muestras; en Live, 12 o 0 |
| DryWetMixer | Retrasa el dry según la latencia del grafo; se actualiza al publicar un grafo (`AudioEngine.cpp:155-160`, `:527-530`; `DryWetMixer.h:152-157`) | Se realinea solo, porque el cambio de modo pasa por una reconstrucción del grafo |
| Endpoint Dry (`OUTPUT_MIX = 0`) | Devuelve la entrada **sin retraso** y sin tocar el grafo (`AudioEngine.cpp:639-644`), pero al host se le reporta la latencia del grafo | **Hallazgo B-12:** en un DAW, con Mix = 0 la pista suena adelantada 2 ms (Studio). En Live el desfase casi desaparece. Es previo a T2-01 y de baja prioridad |
| Bypass global del host | `processBlockBypassed` retrasa el dry con `getLatencySamples()` (`PluginProcessor.cpp:697-740`) | Sigue alineado si la latencia reportada se actualiza (P13A) |
| Reporte al host (P13A-F001) | `applyHostLatencyIfChanged` desde el message thread, con dedupe (`PluginProcessor.cpp:539-581`) | Reporta el nuevo valor sin tocar `processBlock` |
| Reconstrucción del grafo | Cambiar la latencia de un nodo exige reconstruir (`AudioEngine.cpp:344-371`). Después, el motor pasa 6 bloques por la rama de arranque (`AudioEngine.cpp:481-488`, `Constants.h:101`) | **Hallazgo B-11 (a verificar con un test en F0):** esa rama devuelve el buffer **de entrada sin procesar** (`AudioEngine.cpp:573-580`), no silencio. Tras cada reconstrucción suenan 6 bloques de DI crudo, sin OutputChain y sin alinear (8 ms con buffer 64; 64 ms con 512). Afecta al cambio de modo y hoy también al bypass de pedales con latencia y al cambio de preset (T2-15) |

### 2.5 Ataque percibido (no es latencia)

| Fuente | Qué hace | Medición (simulación, sección 3.2) |
|---|---|---|
| Gate de InputChain | Envolvente con ataque de 1 ms, abre al 1.5× del umbral, ganancia que abre en 4 ms (`InputChain.h:359-372`). **Apagado por defecto:** umbral −100 dB ≤ −95 → desactivado (`InputChain.cpp:19-26`, `PluginStateModel.h:303`); los presets draft lo dejan en −100 | Con el umbral en −50 dB y una púa fuerte: **−18.7 dB en el primer ms, −9.3 dB en 3 ms y −3.5 dB en 10 ms**. Es el mayor "ladrón de ataque" de NOVA cuando el usuario lo activa |
| Noise Gate pedal en Studio | El lookahead de 1.5 ms abre el gate antes de que llegue la nota | Pérdida 0.0 dB: el lookahead hace exactamente su trabajo |
| Limiter en Studio | Cuando hay un over, baja la ganancia 2 ms antes del pico (escalón de ganancia anticipado) | Solo actúa con overs. En los presets nominales, nunca (P9F) |
| Compressor y dinámicos | Sin lookahead. El attack (12 ms por defecto) es una elección musical | Fuera de alcance: no es latencia ni seguridad |

## 3. Limiter sin lookahead

### 3.1 Por qué P12D lo dejó siempre armado

- **P12A:** con `limiter = 0 dB` (el default) el lookahead no corría. La única protección era el soft ceiling (0.985 → 0.999), que hacía de limiter principal con master alto (hasta +12 dB) y line gain (hasta 2x): "clipping leve" con high gain.
- **P12D:** umbral transparente de 0.97 (−0.265 dBFS) siempre armado, latencia de lookahead siempre reportada, y un test que fija las garantías (`AudioEngineTests.cpp:5190`):
  - **G1:** pico de salida < 0.98 con master +10.73 dB.
  - **G2:** ninguna muestra ≥ 0.985 (no se toca la banda del soft ceiling).
  - **G3:** el limiter reduce ganancia (> 0.5 dB).
  - **G4:** `softCeilingTouchedSamples == 0`.
- **Base de comparación existente:**
  - Golden p4 `impulse_transparency_line_a`: pico **0.97000003** en el índice **96** (`docs/golden-metrics/p4-offline-qa-baseline.json`; `docs/reports/p12d-golden1-outputchain-rebaseline.md`).
  - P9F (`artifacts/p9f-draft-preset-limiter-telemetry-report.txt`): 6 presets draft, picos de 0.10 a 0.57, `limiterTouched = 0`, `limiterMaxReductionDb = 0`. **En uso nominal el limiter no actúa nunca.**
  - P14B no mide el limiter (solo pedales de drive y dinámica).

### 3.2 Método

No existe un escenario offline en C++ para el modo Live (escribirlo sería código nuevo). Se usó una **réplica en Python** (scratchpad, desechable) de OutputChain: DC blockers, ganancia master, `PeakLimiter` con sus dos rutas (estimador Catmull-Rom que solo ve dentro del bloque, hold de 6 ms, release dependiente del programa), soft ceiling, bloques de 64 @ 48 kHz, mono, float64.

**Validación de la réplica:**
- Tono a fs/4 con fase de 45°: da +0.81 dBTP, igual al cálculo analítico del estimador (estima 1.149 cuando el pico real es 1.30).
- El escenario del test "OutputChain limiter catches isolated pick transients with lookahead" (`AudioEngineTests.cpp:4138`) da pico 0.2512, dentro del rango esperado (0.18–0.31).
- Impulso de 1.0 → 0.97, como en el golden.

**Límites:** float64 contra float32, sin link estéreo y con señales sintéticas (Karplus-Strong). La Fase 0 porta estas mediciones a tests en C++.

**Señales:**
- Impulsos de 1.0 y 2.0.
- Senos de 1 kHz a 0 dBFS y +6 dB.
- Seno de 4321 Hz a +6 dB, para aliasing.
- Tono a fs/4 con picos entre muestras.
- Rampa de −12 a +12 dB.
- Escenario P12D (seno de 0.5 con master +10.73 dB).
- Púas con 4 notas (82–330 Hz) a +6 y +12 dB sobre el techo, en dos versiones: "post-cab" (paso bajo Butterworth de 4.º orden a 5.5 kHz) y "high-gain post-cab" (tanh(10x) + cab).
- Púas de banda completa (ruido blanco, irreal) como peor caso.

### 3.3 Alternativas evaluadas

| ID | Alternativa | Latencia |
|---|---|---|
| Studio | Limiter actual, lookahead de 96 muestras | 2.0 ms |
| **(a)** | `processWithoutLookahead` existente (`OutputChain.h:352-398`), mismo umbral, hold y release; soft ceiling detrás | 0 |
| (b1) | Soft-clipper sin memoria: identidad hasta 0.85 y rodilla tanh hasta 0.97 | 0 |
| (b2) | (b1) con ADAA de 1.er orden, activo solo cerca de la rodilla (ver nota) | 0 (+½ muestra de retardo de grupo dentro del ADAA) |
| (b3) | (b1) a 2x con el halfband IIR de JUCE (`isMaxQuality = false`) | 1.84 muestras |
| (c) | (a) con el umbral en 0.94 como margen | 0 |
| (d) | Híbrido: limiter sin lookahead con attack de 0.5 ms (umbral 0.90) + clipper ADAA (0.90 → 0.97) | 0 |
| (e) | Limiter actual con lookahead de 0.25 ms (12 muestras) | 0.25 ms |

**Nota ADAA:** aplicado siempre, en la zona lineal equivale a `(x[n] + x[n−1]) / 2`: un filtro que baja 3 dB a 12 kHz y colorea el camino limpio. Conmutarlo solo cerca de la rodilla introduce saltos de media muestra, que en la medición **empeoran** el aliasing (−16.9 dB contra −31 dB sin ADAA). ADAA no encaja en un limitador de seguridad transparente.

### 3.4 Resultados

**Overs sostenidos (lo que P12D protege):**

| | P12D master +10.73 dB: pico muestra / true peak / residuo | Seno 1 kHz +6 dB: residuo | Seno 4321 Hz +6 dB: energía inarmónica |
|---|---|---|---|
| Studio | −0.20 dBFS / −0.20 dBTP / **−65.7 dB** | −72.2 dB | −43.2 dB |
| **(a)** | **−0.26 / −0.26 / −66.5 dB** | **−72.2 dB** | **−43.2 dB** |
| (c) | −0.54 / −0.54 / −66.4 | −72.0 | −43.2 |
| (e) | −0.26 / −0.26 / −68.4 | −71.9 | −43.2 |
| (d) | −0.27 / −0.26 / −70.7 | −67.8 | −33.7 |
| (b1) | −0.26 / −0.26 / **−14.0** | **−12.5** | −31.0 |
| (b2) | −0.26 / −0.26 / −14.0 | −12.2 | −16.9 |
| (b3) | −0.26 / −0.26 / −14.0 | −12.5 (2365 muestras en el soft ceiling) | −32.7 |

En todas las opciones de limiter, el soft ceiling queda sin tocar en el escenario P12D, con una reducción máxima de 4.98 dB (G2, G3 y G4).

**Transientes post-cab (lo que suena distinto en Live):**

| | Púa +6 dB: pico muestra / true peak | Púa +12 dB: pico muestra / true peak | High-gain +6 dB: true peak | Diferencia con Studio en los primeros 5 ms | Energía nueva > 9 kHz en el ataque (entrada: −28.0 dB) |
|---|---|---|---|---|---|
| Studio | −0.21 / −0.20 | −0.13 / −0.03 | −0.18 | — | −27.3 dB |
| **(a)** | **−0.26 / −0.14** | **−0.26 / −0.15** | **−0.17** | −11.0 / −10.4 / −13.6 dB | **−26.9 dB** |
| (e) | −0.26 / −0.19 | −0.25 / −0.19 | −0.23 | −12.8 / −12.8 / −14.4 | −27.5 |
| (c) | −0.54 / −0.42 | −0.54 / −0.42 | −0.43 | −11.6 / −10.8 / −14.3 | −26.9 |
| (d) | −0.26 / +0.31 | −0.26 / +1.08 | +0.08 | −5.8 / −1.5 / −6.4 | −21.2 |
| (b1) | −0.26 / +0.36 | −0.26 / +1.42 | +0.33 | −3.1 / +2.2 / −3.1 | **−19.9 (−13.7 a +12 dB)** |
| (b2) | −0.26 / +0.90 | −0.26 / +1.52 | +1.10 | −2.8 / +2.3 / −2.8 | −17.4 |
| (b3) | **−0.01** (341 muestras en el soft ceiling) / +0.39 | −0.01 (2900) / +1.55 | +0.52 | −2.8 / +2.3 / −2.6 | −20.1 |

**Camino limpio** (púa post-cab a 0.5, debajo del umbral): (a), (b1), (b2), (c), (d) y (e) son idénticos a Studio salvo por la latencia (null −240 dB). (b3) no: los filtros IIR cambian la fase (null −18 dB).

### 3.5 Lectura

1. **(a) cumple G1–G4** y el techo de muestra es incluso más exacto que en Studio. En Studio, el pico llega a −0.13 dBFS (0.985, el borde del soft ceiling): el hold no se renueva con picos algo menores y el release empieza a subir la ganancia antes de que salga la muestra retrasada (medido: ganancia aplicada 0.313 contra 0.308 necesaria). En (a), la ganancia se calcula con la misma muestra que se atenúa.
2. **Qué cambia con (a):** solo con overs. Sube el primer flanco hasta el techo y lo aplana hasta el pico (de unas pocas muestras a un cuarto de ciclo); después es la misma reducción limpia de Studio. La diferencia en el ataque es de −10 a −14 dB frente a Studio, pero casi no agrega agudos nuevos (+0.2 a +1.1 dB sobre la entrada; Studio +0.1 a +0.7). **Esto hay que escucharlo (A/B), pero no es un click.**
3. **Los soft-clippers (b) no sirven como protección principal:**
   - Con overs sostenidos distorsionan ~20–25% (residuo de −12 a −14 dB): vuelve el "clipping leve" de P12A.
   - En los ataques agregan de +8 a +14 dB de energía nueva en agudos.
   - El oversampleado (b3) además **pierde el techo** (el filtro de decimación sobrepasa 0.97) y cambia el camino limpio.
4. **El híbrido (d)** es el más limpio en overs sostenidos, pero recorta más los ataques (−1.5 a −6 dB de diferencia) y suma complejidad sin ganar seguridad.
5. **El lookahead corto (e)** se parece mucho a (a) y cuesta 0.25 ms. No vale la pena.
6. **Hallazgo lateral B-13 (no es de Live):** el estimador de true peak (Catmull-Rom de 4 puntos, `OutputChain.h:278-312`) subestima los picos entre muestras con contenido agudo:
   - +0.81 dBTP con un tono a fs/4.
   - +2.7 dBTP con púas sintéticas de banda completa (en Live, +3.7).
   - Con material post-cab real se mantiene ≤ 0 dBTP.

   Si algún día se quiere true peak garantizado, la referencia es un detector oversampleado 4x más un lookahead mínimo de ~0.1 ms (FabFilter Pro-L 2, BS.1770). Queda anotado, fuera de T2-01.

**CPU:**
- (a) hace lo mismo que Studio, menos la lectura y escritura del buffer de retardo: igual o menor costo.
- (b1)/(b2): un `tanh` o un log-cosh por muestra, solo por encima de la rodilla.
- (b3): unas 16 secciones allpass por muestra y canal.

Todo esto es chico frente a un pedal (el escenario `clean_empty_chain` completo cuesta 0.6% promedio) y no decide nada.

### 3.6 ¿Mantiene Live las garantías de P12D?

**Sí, con (a):**
- El mismo umbral de 0.97, el mismo hold y release, el mismo soft ceiling y el mismo `HARD_ABS_LIMIT`.
- La simulación cumple las 4 aserciones del test P12D.
- **Oídos y equipo:** el nivel máximo de muestra es idéntico y el sostenido queda controlado igual.
- **Masters calientes:** el limiter sigue haciendo el trabajo, no el soft ceiling.

Lo único que se pierde es la anticipación de 2 ms en el primer flanco de cada over.

**Recomendación:** (a), la ruta `processWithoutLookahead`, que ya existe y ya se usa como fallback (`OutputChain.h:193-196`).

## 4. Noise Gate sin lookahead

### 4.1 Cómo funciona hoy

- Detector híbrido peak/RMS sobre un sidechain filtrado: ataque del pico de 0.08 ms (`NoiseGatePedal.h:312`), hysteresis de 8 dB, hold de 70 ms, rodilla con smoothstep y release adaptativo.
- La ganancia abre con `attack × jmap(focus, 1, 0.72)` (0.21 ms con los valores por defecto; `:364`) y se aplica a la señal retrasada 1.5 ms (`:384-385`).
- El cuello de botella al abrir **no es el detector** (0.08 ms). Es la rampa de ganancia, que el lookahead esconde por completo.

### 4.2 Mediciones (réplica en Python con valores por defecto: umbral −46, attack 0.25, hold 70, release 115, hysteresis 8, focus 0.55)

Señales de DI con piso de ruido (hum −66 dBFS + hiss −72 dBFS), así el gate empieza cerrado. Pérdida de energía en el primer ms / en los primeros 3 ms (en dB):

| Caso | Studio (72) | 0 + attack 0.25 | 0 + attack 0.10 | 0 + attack 0.02 | **12 + attack 0.25** | 12 + attack 0.10 | 24 + attack 0.25 |
|---|---|---|---|---|---|---|---|
| Púa fuerte, −12 dBFS | 0.0 / 0.0 | −2.2 / −0.5 | −0.8 / −0.2 | −0.1 / −0.03 | **−0.7 / −0.2** | −0.05 / −0.01 | −0.2 / −0.06 |
| Fingerpicking suave, −36 dBFS | 0.0 / 0.0 | −2.3 / −0.8 | −1.2 / −0.5 | −0.7 / −0.3 | **−0.9 / −0.3** | −0.1 / −0.04 | −0.3 / −0.1 |
| Nota a 4 dB del umbral, −42 dBFS | 0.0 / 0.0 | −4.4 / −1.3 | −3.1 / −1.0 | −2.3 / −0.8 | **−2.3 / −0.8** | −1.2 / −0.5 | −0.8 / −0.3 |
| Palm mute en semicorcheas, −18 dBFS | 0.0 / 0.0 | −2.2 / −0.6 | −0.8 / −0.2 | −0.1 / −0.03 | **−0.7 / −0.2** | −0.05 / −0.01 | −0.2 / −0.06 |

Pico del error en los primeros 5 ms, relativo al pico de la nota (cuánto se aparta la forma del ataque):
- Studio: −41 a −65 dB.
- 0 + attack 0.25: −1 a −4 dB. Se pierde el inicio del ataque.
- 12 + attack 0.25: −12 a −14 dB (−2 dB cerca del umbral).
- 12 + attack 0.10: −26 a −33 dB (−3 dB cerca del umbral).

### 4.3 Opciones

| Opción | Ahorro | Riesgo |
|---|---|---|
| **G1 — Lookahead 0.25 ms (11/12/24 muestras @ 44.1/48/96k), attack del usuario sin cambios** | −1.25 ms | Pérdida ≤ 0.9 dB en el primer ms, salvo cerca del umbral (−2.3 dB). Requiere un piso propio para Live: hoy el clamp mínimo es de 16 muestras (`:219`) |
| G2 — G1 + attack efectivo `min(attack, 0.10 ms)` cuando el usuario tiene un attack rápido (< 1 ms) | −1.25 ms | Casi transparente (≤ 0.1 dB), pero altera el parámetro del usuario. Candidato para A/B |
| G3 — Sin lookahead, attack del usuario | −1.5 ms | −2.2 dB en el primer ms en cualquier nota y forma del ataque alterada (error de −1 a −4 dB). Es el riesgo de click y ataque blando en notas suaves y palm mutes |
| G4 — Sin lookahead + rango menos profundo en Live (p. ej., −30 dB) | −1.5 ms | Salto de apertura menor (menos click), pero deja pasar más ruido: cambia la función del pedal |

- **Hysteresis y peak/RMS:** el detector ya es híbrido y la hysteresis de 8 dB evita el chatter. Nada de eso depende del lookahead, así que no hay que tocarlo.
- **Palm mutes:** con semicorcheas a 120 bpm, el hold de 70 ms deja el gate abierto entre golpes. Solo pierde ataque la primera nota tras un silencio.

**Recomendación:** G1. Si en el A/B se nota el ataque, probar G2.

### 4.4 Gate de InputChain (4 ms)

Mismo método, umbral −50 dB. Pérdida en el primer ms / en 3 ms / en 10 ms:

| Caso | Apertura 4 ms (hoy) | 1 ms | 0.3 ms |
|---|---|---|---|
| Púa fuerte | −18.7 / −9.3 / −3.5 | −8.8 / −2.6 / −0.7 | −3.3 / −0.7 / −0.2 |
| Fingerpicking suave | −62 / −15.2 / −4.2 | −62 / −7.1 / −1.4 | −62 / −4.0 / −0.8 |
| Palm mute | −18.4 / −9.4 / −3.9 | −9.0 / −2.8 / −0.9 | −3.5 / −0.9 / −0.3 |

- **En notas suaves manda la detección**, no la apertura: la envolvente tiene 1 ms de ataque y el gate abre recién al 1.5× del umbral.
- **Está apagado por defecto**, así que solo afecta a quien lo activa. Para esa persona, el ataque se siente ~10 ms más tarde.
- **Propuesta para Live:** apertura de 1 ms. Opcional: envolvente con ataque de 0.3 ms.
- **Riesgo:** el gate va antes de los drives, así que una rampa más rápida sobre el DI se amplifica. Hay que probarlo con un preset high-gain.
- **Decisión 6:** aplicarlo solo en Live o también en Studio.

## 5. Diseño del modo

### 5.1 Alcance y defaults

| | Standalone | VST3 |
|---|---|---|
| Alcance | **Ajuste global de la app** (como el driver o el buffer) | **Por instancia**, guardado en la sesión del DAW |
| Default F1–F3 | Studio (opt-in) | Studio |
| Default después del A/B (F4) | **Live** (decisión 4) | Studio siempre |
| ¿Viaja con los presets? | No | No |

**Por qué no por preset:**
- La latencia es una propiedad del setup (tocar en vivo o mezclar), no del sonido.
- Cargar un preset no debe cambiar la latencia: en el DAW provocaría un cambio de PDC con cada preset.

**Por qué por instancia en VST3:** un proyecto abierto en otra máquina tiene que sonar y alinearse igual. Un ajuste global en `editor-settings.xml` lo rompería, porque ese archivo lo comparten el Standalone y el VST3 (`NovaSettingsService.h:37-50`).

### 5.2 UI

- **Standalone:**
  - Interruptor "Live / Studio" en el paso Scan de Audio Setup, junto al round trip, y en Ajustes.
  - Al cambiarlo, el round trip mostrado baja solo, porque T0-04 ya suma la latencia del grafo.
  - Texto sugerido: *"Live: −3.3 ms. El limitador de salida reacciona sin anticipación; el techo de salida es el mismo. En picos muy calientes el ataque puede sonar levemente distinto. Studio: máxima precisión para grabar y mezclar."*
- **VST3:** el mismo interruptor en la sección de salida. Si el transporte está andando: *"Se aplica al detener la reproducción"*.
- Indicador "LIVE" visible en la barra de estado, para que nadie olvide en qué modo está.

### 5.3 Persistencia y schema

| | Dónde | Schema |
|---|---|---|
| Standalone | `PropertySet` del Standalone (`%APPDATA%/NOVA/NOVA.settings`), el mismo que usa el Autopilot para el mute (`novaInputMutePinnedByUser`). Es solo del Standalone | Sin cambios |
| VST3 | Propiedad nueva en la raíz de `NOVA_STATE` (`getStateInformation`, `PluginProcessor.cpp:783-790`), **excluida al guardar un preset** y **ignorada al cargar uno**. Ausente = Studio | **Sin bump de `STATE_SCHEMA_VERSION`:** es aditiva y opcional. Una versión vieja de NOVA la ignora; una sesión vieja carga en Studio, que es el comportamiento de hoy |

No se usa un `AudioParameter`: sería automatizable, y una automatización cambiaría la latencia en plena reproducción. La excepción es F5 (sección 6.3).

### 5.4 Qué cambia por dentro

- **OutputChain:** un flag atómico `liveMode`.
  - `process()` elige la ruta.
  - `updateReportedLatencyForLimiter()` reporta 0 o el lookahead (`OutputChain.cpp:184-188`).
  - El buffer de retardo se sigue dimensionando para Studio: no hay arrays nuevos (B-09).
- **NoiseGate:** retardo efectivo de Live (0.25 ms o 0) dentro de los buffers que ya existen. `setProcessingLatency` asigna memoria (`ProcessorBase.h:47-56`, `:343-360`), así que el modo se fija **al construir el grafo** (message thread), nunca en `processBlock`.
- **AudioEngine:** `setLatencyMode()` en el control plane. Pide una reconstrucción explícita (la misma ruta que un bypass que cambia la latencia: `AudioEngine.cpp:344-371`), y `GraphBuilder` aplica el modo a los procesadores nuevos antes de prepararlos.
- **Sin locks ni allocations en el audio thread:** solo flags leídos por bloque y buffers preasignados.

## 6. Cambio de latencia en caliente

### 6.1 Standalone

1. El usuario cambia el modo → message thread → reconstrucción del grafo con procesadores nuevos.
2. `publishGraph` → el DryWetMixer se realinea solo (`AudioEngine.cpp:155-160`) y se reporta la nueva latencia.
3. **Problema actual (B-11):** tras la publicación suenan 6 bloques de DI crudo, con cortes duros en los dos extremos y un salto de tiempo igual a la latencia.
4. **Propuesta:**
   - fade-out de ~5 ms con el grafo viejo;
   - swap;
   - silencio durante los bloques de arranque (en lugar del DI);
   - fade-in de ~5 ms.

   Un hueco de ~15–20 ms al tocar un interruptor de ajustes es aceptable y no hace click. Las colas de reverb y delay se cortan, igual que con cualquier reconstrucción (T2-15).
5. **Alternativa descartada: crossfade entre los dos modos.** Mezclar dos señales desfasadas 96 muestras durante el fundido produce un comb filter audible.

### 6.2 VST3: comportamiento del PDC en los DAW

| Host | Latencia que cambia con el transporte andando | Fuente | Confianza |
|---|---|---|---|
| Spec VST3 | `restartComponent(kLatencyChanged)`; el host recalcula el PDC y "puede interrumpir el audio". Para grabación en vivo, la latencia "debería ser cero o pequeña" | Docs del SDK (vía dplug), foro de Steinberg | Alta |
| JUCE 8.0.12 | Envía `kLatencyChanged` solo si el valor cambió (`juce_audio_plugin_client_VST3.cpp:1548-1561`), de forma asíncrona. NOVA ya reporta desde el message thread (P13A) | Código | Alta |
| Reaper | Refresca el PDC al correr el transporte. Un reporte describe la pista del plugin desfasada hasta reiniciar | KVR, foro JUCE | Media (anecdótico) |
| Ableton Live 11 | Requiere stop/start de la reproducción para corregir el PDC | Foro JUCE (un desarrollador) | Media |
| Cubase / Logic | Se corrige después de stop/start | Foro JUCE | Media |
| Studio One | Rechazaba `restartComponent` desde el callback de audio; desde un timer funciona | Foro JUCE | Media |
| FL Studio | PDC automático: "se actualiza cuando se detecta o cambia el retardo del plugin" | Manual de Image-Line | Media (no dice qué pasa mientras suena) |
| Ardour | Toma el cambio sin reactivar el plugin | Commits de Ardour | Alta |

**Conclusión:** ningún host documenta un cambio limpio con el transporte andando. Lo seguro es cambiar con el transporte detenido.

### 6.3 Opciones

| Opción | Pro | Contra | Veredicto |
|---|---|---|---|
| Modo fijo por instancia, cambiable en cualquier momento | Simple | Desfase o glitch si se cambia reproduciendo | Solo con aviso |
| **Aplicar solo con el transporte detenido** (NOVA ya lee `hostTransportPlaying`, `PluginProcessor.cpp:267`, `:373`) | El PDC se recalcula sin audio en curso | Si el host no informa transporte, se aplica al instante con aviso | **Recomendada** |
| Latencia constante con relleno (reportar siempre la de Studio y rellenar en Live) | El PDC nunca cambia | **No gana nada:** el retardo real sigue siendo el mismo | Descartada |
| Exponer `LowLatencyMode` para la Constrain Delay Compensation de Cubase (`IParameterFunctionName`, SDK 3.7.0; header en `VST3_SDK/pluginterfaces/vst/ivstparameterfunctionname.h:36`; JUCE permite agregarlo con `VST3ClientExtensions::queryIEditController`, `juce_VST3ClientExtensions.h:67`) | Cubase activa Live solo al grabar. **Sin esto, CDC apaga NOVA** cuando el plugin no tiene modo de baja latencia | Requiere un parámetro y código VST3 específico | Fase opcional (F5) |

**Nota para el usuario de DAW:** el "Reduced Latency When Monitoring" de Ableton quita la compensación de la pista monitoreada, pero no la latencia propia del plugin. El modo Live sí la quita.

## 7. Relación con otros ítems

| Ítem | Relación |
|---|---|
| **T2-10 (Autopilot)** | El round trip del wizard ya incluye la latencia de NOVA, así que el efecto de Live se ve solo. **Sugerencia** (nunca automática, como en la decisión 4 de T2-10): con ASIO o Exclusive y buffer ≤ 128, la tarjeta propone "Modo Live: −3.3 ms". Aplicar Live no requiere recalibrar el buffer: no sube la carga de CPU |
| **T2-02 (línea muteada)** | Después de T2-01, la parte de **latencia** de T2-02 casi desaparece: la línea muteada solo aporta su oversampling (4–8 muestras) y su gate en Live (12). T2-02 conserva su valor de **CPU** |
| **T2-05 (islas de oversampling)** | Ahorra 4 muestras (0.08 ms) por isla fusionada. Su valor es CPU, no latencia |
| **T1-05 (`isMaxQuality = false`)** | 4.08 → 2.95 muestras por procesador: −1 muestra (0.02 ms). Despreciable como latencia |
| **T2-15 (preset sin hueco)** | Comparte B-11. Arreglar la transición del swap beneficia a ambos |
| **T1-12 (loopback)** | Permite medir físicamente el −3.3 ms |
| **B-09 / T1-14 (stack)** | El diseño no agrega arrays a los procesadores |

## 8. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| El ataque suena distinto cuando el limiter actúa (flanco recortado) | Solo pasa con overs; los presets nominales no llegan (P9F). A/B y ABX con master alto. Default Studio hasta validar (F4) |
| True peak algo mayor con contenido muy agudo | El techo de muestra no cambia (C2). En post-cab, ±0.1 dB. Suite de peor caso en C++ (F0) |
| Pérdida de ataque o click del gate | Lookahead de 0.25 ms (G1). ABX con palm mutes, fingerpicking y notas cerca del umbral |
| Desfase del PDC en el DAW al cambiar el modo | Por instancia, no automatizable, se aplica con el transporte detenido, fuera de los presets |
| DI crudo durante el cambio (B-11) | Fade-out → silencio → fade-in. Test de transición en F0 y F1 |
| Tests que asumen lookahead | Los tests de Studio no se tocan; Live tiene tests gemelos (sección 9) |
| Que Studio cambie sin querer | Con Live apagado, bit-exacto 48/48. Agregar OutputChain e InputChain al checksum (hoy cubre solo los 24 pedales) |
| Usuario confundido ("¿Live es menos seguro?") | El texto de la UI dice que el techo de salida es el mismo |
| Stack de los tests en Debug (B-09) | Sin arrays nuevos; los buffers se reutilizan al tamaño de Studio |
| Gate de InputChain más rápido → click con high gain | Solo en Live y con A/B (decisión 6) |

## 9. Plan de pruebas

### 9.1 Con Live apagado (Studio debe quedar idéntico)

- Checksums bit-exactos 48/48 en Debug y Release (`scripts/run-bitexact-checksums.ps1`).
- **Nuevo en F0:** checksums de OutputChain e InputChain en Studio.
- Base validation y golden metrics PASS. El golden `impulse_transparency_line_a` sigue en 0.97 @ 96.
- Policy del audio thread: 0 hallazgos.

### 9.2 Con Live encendido

| Prueba | Detalle | Criterio |
|---|---|---|
| Latencia reportada | OutputChain 0 a 44.1/48/96k; NoiseGate 11/12/24 (G1) o 0; grafo = suma; `latency.breakdown` de la cadena típica; host PDC (gemelos de P13A, `AudioEngineTests.cpp:5374`, `:5414`, `:5479`, `:5534`) | C1 |
| Techo de picos | Impulsos (1.0, 2.0), senos a 0 dBFS / +6 / +12 dB, rampa de −12 a +12 dB, fs/4 a 45°, escenario P12D, púas post-cab y high-gain, master +12 dB con line gain 2x, estéreo enlazado | C2, C3, C4 |
| Salpicadura en ataques | Energía > 9 kHz en ventanas de 10 ms desde el ataque, comparada con la entrada | C5 |
| Gate | Pérdida en 1 / 3 / 10 ms y pico del error en púa fuerte, fingerpicking, nota a 4 dB del umbral y palm mutes; y sin chatter (gemelo de `high_gain_noise_gate_chatter_guard`, `:15389`) | C6 |
| Transición | Cambiar de modo con señal: sin DI crudo, sin muestras > techo, sin discontinuidad > umbral del `HealthMonitor` | Sin clicks |
| rt-profile Release | Escenarios de cadena en Live frente a Studio | CPU igual o menor (dentro del ruido) |
| A/B + ABX | Presets nominales (deben ser idénticos), master alto, high gain, palm mutes, fingerpicking | C7 |
| DAW | `docs/reports/p13a-pdc-latency-host-smoke-procedure.md` + cambio de modo con el transporte detenido y andando, en AudioPluginHost (harness actual), Reaper, Ableton y Cubase si están disponibles | Alineación del impulso, sin crash |

### 9.3 Tests existentes que asumen lookahead

| Test | Línea | Qué pasa |
|---|---|---|
| "OutputChain limiter catches isolated pick transients with lookahead" | `AudioEngineTests.cpp:4138` | Studio sin cambios. Gemelo Live: misma aserción (la simulación da 0.2512, dentro de 0.18–0.31) |
| "OutputChain clamps legacy extreme limiter thresholds…" | `:4105` | Studio (latencia > 0). Gemelo Live: latencia == 0 |
| "P12D OutputChain transparent safety…" | `:5190` | Studio sin cambios. Gemelo Live con las 4 aserciones (la simulación las cumple) |
| "P12D … stays transparent on quiet signal" | `:5274` | Gemelo Live: idéntico salvo latencia |
| "P12D … reports lookahead latency at every limiter setting" | `:5350` | Studio. Gemelo Live: 0 en los tres umbrales |
| P13A (host PDC, bypass, restore, reprepare) | `:5374–5534` | Studio. Gemelos Live, más un test nuevo: cambio de modo → latencia del host actualizada |
| "AudioEngine publishes replacement graph when bypass changes node latency" y "rapid latency-affecting bypass toggles" | `:6740`, `:7280` | Modelo para el test "cambio de modo publica un grafo nuevo" |
| P14B gate probe (`observedLookahead` entre 16 y 192) | `:14957` | Studio. Gemelo Live: 0 o 12 según la decisión 2 |
| NoiseGate "clamps late noise…", focus y linked stereo | `:10847`, `:10897`, `:10946` | Studio. Gemelos Live con tolerancias de ataque (C6) |
| Helpers con latencia derivada (`expectStereoSamplesMatchAfterLatency`, golden p12d) | — | Funcionan en los dos modos: leen la latencia del motor |

## 10. Fases de implementación

| Fase | Contenido | Riesgo | Tamaño |
|---|---|---|---|
| **F0** | Solo tests y diagnóstico: checksums de OutputChain e InputChain; suite C++ de peor caso para el limiter (métricas de 3.2) y el gate (métricas de 4.2) en Studio, como línea base; test que imprime `latency.breakdown` de la cadena típica a 44.1/48/96k; test que confirma o descarta B-11. **Sin cambios de sonido** | T0 | S |
| **F1** | Live en el motor: conectar los flags de OutputChain (ruta a) y del gate (0.25 ms), que ya existen desde F0, mediante `AudioEngine::setLatencyMode` con reconstrucción y transición con fades (sin DI crudo, ya resuelto por el arreglo de B-11). Ajuste en el `PropertySet` del Standalone + interruptor simple en Ajustes. **Default Studio.** Tests gemelos Live | T1 (apagado por defecto) | M |
| **F2** | UI: tarjeta en Audio Setup, indicador LIVE, texto explicativo, sugerencia del Autopilot (T2-10). Gate de InputChain a 1 ms en Live (si se aprueba). Protocolo A/B + ABX | T1 | S |
| **F3** | VST3: estado por instancia (fuera de los presets), aplicar con el transporte detenido, smoke de PDC en DAW | T2 | M |
| **F4** | Si el A/B pasa: Live por defecto en el Standalone | T2 (cambia el default) | S |
| **F5** (opcional) | `LowLatencyMode` para la Constrain Delay Compensation de Cubase | T2 | S/M |

Cada fase se valida y se commitea por separado. Los archivos nuevos se agregan a `NOVA.jucer` y se re-exporta con `Projucer --resave`.

## 11. Decisiones (tomadas el 2026-10-12)

| # | Decisión | Opciones | Recomendación del documento | **Decidido** |
|---|---|---|---|---|
| 1 | Limiter en Live | (a) `processWithoutLookahead`, (e) lookahead de 0.25 ms, (d) híbrido | **(a)**: misma seguridad, 0 ms, sin código DSP nuevo | **(a)**, condicionado a F0. **F0 lo confirmó en C++** (sección 11.1) |
| 2 | Noise Gate en Live | G1 (0.25 ms), G2 (G1 + attack ≤ 0.10 ms), G3 (0 ms) | **G1** (−1.25 ms, casi transparente). G2 solo si el A/B lo pide | **G1** |
| 3 | Alcance | Global (Standalone) + por instancia (VST3), fuera de los presets | **Sí**, como en 5.1 | **Sí** |
| 4 | Default del Standalone | Studio siempre / Live desde F1 / Live después del A/B | **Studio en F1–F3; Live en F4** si el A/B y el ABX pasan | **Sí** |
| 5 | VST3 | Sin Live / Live aplicado con el transporte detenido / Live inmediato | **Live con el transporte detenido**, no automatizable. CDC de Cubase en F5 | **Pospuesto:** primero Live solo en el Standalone; F3 y F5 después |
| 6 | Gate de InputChain (4 ms) | Sin cambios / 1 ms solo en Live / 1 ms siempre | **1 ms solo en Live**, con A/B high-gain | **Pospuesto:** viene apagado y va antes de los drives (riesgo de click alto, poco beneficio) |
| 7 | B-11 (DI crudo tras reconstruir) | Arreglarlo dentro de T2-01 / dejarlo para T2-15 | **Dentro de T2-01 (F0 + F1)** | **Antes de F1, como ítem propio:** afecta también al cambio de preset, al bypass con latencia y al Aplicar/Deshacer del Autopilot |
| 8 | Nombre en la UI | "Live / Studio", "Baja latencia", "Modo escenario" | **"Live / Studio"**: corto y coherente con el roadmap | **"Live / Studio"** |

**Orden resultante:** F0 (hecha) → arreglo de B-11 → F1 + F2 solo en el Standalone, con Studio por defecto → A/B del usuario → F4. F3 y F5 (VST3) quedan para después.

### 11.1 Resultados de la Fase 0 (2026-10-12)

**Qué se agregó (sin cambio de sonido):**
- **Flags inertes de Live, apagados por defecto:** `OutputChainProcessor::setLiveMode()` (limiter por `processWithoutLookahead`, latencia reportada 0) y `NoiseGatePedal::setLiveMode()` (lookahead de 0.25 ms). Hacían falta para medir Live en C++. Todavía no están conectados al motor, al estado ni a la UI: eso es F1.
- **Checksums bit-exactos** de `OutputChain`, `InputChain`, `OutputChain@live` y `Noise Gate@live` en las dos configuraciones (`OfflineQADiagnostics.h`). Los dos `@live` fijan la salida de Live para detectar derivas en F1.
- **Suite `T2-01 F0 Live mode measurements`** (`AudioEngineTests.cpp`, clase `T201LiveModeF0Tests`). Mide, compara Studio contra Live y escribe las cifras en el log con el prefijo `T2-01 F0`.

**Validación:**
- Bit-exacto: los 48 checksums previos, idénticos en Debug y en Release; con las 8 entradas nuevas, 56/56 determinísticos.
- Base validation PASS en Debug y Release (333 resultados, 13744 checks, 0 fallos); golden PASS.
- rt-profile Release 64/64 PASS. Frente a post-T0, los escenarios de motor (pasan por OutputChain) quedan dentro del ruido: mediana −3.3% (`artifacts/rt-profile-release-t2-01-f0.json`).

**Limiter, 48 kHz, bloque 64 (C++):**

| Caso | Pico de muestra Studio / Live | True peak Studio / Live | Residuo Studio / Live |
|---|---|---|---|
| Impulso 2.0 | −0.26 / −0.26 dBFS | −0.26 / −0.26 dBTP | — |
| Seno 1 kHz +6 dB | −0.25 / −0.27 | −0.25 / −0.26 | −71.0 / −71.0 dB |
| Seno 4321 Hz +6 dB | −0.24 / −0.26 | −0.23 / −0.20 | −46.8 / −46.8 dB |
| Tono fs/4 a 45° | −2.18 / −1.75 | +0.91 / +0.93 (B-13) | −86.6 / −93.4 dB |
| Rampa −12 → +12 dB | −0.27 / −0.27 | −0.27 / −0.26 | — |
| **P12D** (seno 0.5, master +10.73 dB) | −0.21 / −0.26 | −0.21 / −0.26 | **−65.7 / −66.3 dB** (Python: −65.7 / −66.5) |
| P12D dos tonos (test unitario) | −0.26 / −0.26 | −0.26 / −0.26 | −34.2 / −33.2 dB (intermodulación del batido, igual en ambos) |
| Púas post-cab +6 dB | −0.26 / −0.26 | −0.26 / −0.23 | — |
| Púas post-cab +12 dB | −0.19 / −0.26 | −0.19 / −0.23 | — |
| Púas high-gain post-cab +6 dB | −0.21 / −0.26 | −0.20 / −0.18 | — |
| **Púas high-gain sin cab +6 dB** (nuevo) | −0.23 / −0.26 | **+0.86 / +1.42** | — |
| Púas de banda completa sin cab +6 dB | −0.26 / −0.26 | +0.14 / +0.36 | — |

| Criterio | Resultado |
|---|---|
| C2 techo de muestra | **Cumple.** Live nunca pasa de 0.97 (−0.26 dBFS) y nunca toca el soft ceiling, en todos los casos. Studio llega a −0.19 dBFS en púas de +12 dB |
| G1–G4 de P12D | Cumplen en Studio y en Live |
| C3 true peak post-cab | **Cumple:** Live queda a +0.03 a +0.07 dB de Studio (criterio ≤ +0.5) |
| C3 sin cab (criterio nuevo: ≤ Studio + 1 dB) | Cumple: +0.56 dB. Los dos modos pasan de 0 dBTP sin cab: es B-13 (estimador Catmull-Rom), previo a Live |
| C4 residuo P12D | **Cumple:** −66.3 dB (≤ −60) |
| C5 splatter en ataques | Cumple con la métrica del documento (agudos de salida contra agudos de entrada): Live −0.65 / −5.16 / −4.07 dB (≤ +1.5) |
| Camino limpio | Live es Studio desplazado 96 muestras, **diferencia exacta 0** |

**A escuchar en el A/B:** si se descuenta la reducción de ganancia, en los ataques limitados Live agrega entre +0.9 y +2.2 dB más agudos que Studio (Studio +1.2 a +1.4 dB, Live +2.1 a +3.7 dB). Studio también agrega agudos porque baja la ganancia de golpe, solo que 2 ms antes. Es muy poco frente a un clipper (+8 a +14 dB), pero confirma que **el primer flanco de un over suena distinto**. Solo pasa cuando el limiter actúa; los presets nominales no llegan a él (P9F).

**Noise Gate, 48 kHz (pérdida en el primer ms / en 3 ms):**

| Caso | Studio (72 muestras) | Live (12 muestras) | Python (12) |
|---|---|---|---|
| Púa fuerte −12 dBFS | 0.00 / 0.00 dB | −0.27 / −0.14 | −0.7 / −0.2 |
| Fingerpicking −36 dBFS | 0.00 / 0.00 | −0.82 / −0.39 | −0.9 / −0.3 |
| Nota más suave que abre el gate (−37 dBFS) | 0.00 / 0.00 | **−1.09 / −0.51** | −2.3 / −0.8 |
| Palm mute en semicorcheas −18 dBFS | 0.00 / 0.00 | −0.30 / −0.16 | −0.7 / −0.2 |

- **C6 cumple:** ≤ 1 dB en notas fuertes, ≤ 2.5 dB cerca del umbral. C++ da pérdidas algo menores que Python.
- **Nota de método:** el detector escucha un sidechain filtrado (con el focus por defecto, paso alto en ~200 Hz), así que el nivel en dBFS de una nota no se compara directo con el umbral de −46 dB. Una nota de 110 Hz a −42 dBFS no abre el gate ni en Studio. El caso "cerca del umbral" se define ahora como la nota más suave que abre el gate en Studio.
- **Latencia del gate:** Live 11 / 12 / 24 muestras a 44.1 / 48 / 96 kHz; Studio 66 / 72 / 144.

**Latencia de la cadena típica (Studio, `latency.breakdown`):** 162 / 176 / 344 muestras a 44.1 / 48 / 96 kHz, idéntico a lo calculado en 2.3 (gate 66/72/144, Overdrive 4, High Gain Amp 4, cab 0, reverb 0, OutputChain 88/96/192). Con Live, por cálculo: 19 / 20 / 32 muestras.

**B-11 confirmado:** tras reconstruir el grafo, salen exactamente **6 bloques** con la entrada sin procesar. Con el master a −20 dB, la salida salta de −29.6 a −10.5 dBFS (+19 dB) durante 8 ms con bloque 64. El test queda como caracterización; el arreglo de B-11 lo invierte (0 bloques crudos).

### 11.2 Arreglo de B-11 (2026-10-12)

**Antes:** al reconstruir el grafo, el motor dejaba de procesar durante 6 bloques y sacaba la entrada sin procesar. En el arranque en frío eran 5 bloques.

**Ahora** (`AudioEngine::process`):
1. **Fade-out con el grafo saliente:** cuando el audio thread detecta un grafo nuevo, procesa el viejo unos bloques más con una rampa de 1 a 0 de 5 ms.
   - Usa como máximo 4 bloques, así que con buffer 32 la rampa es de 2.7 ms.
   - El grafo viejo sigue vivo porque `RuntimeGraphManager` lo retiene 8 bloques.
   - Solo se usa en bloques consecutivos, para no tocar nunca un grafo ya liberado.
2. **Silencio en los bloques de arranque**, nunca la entrada. Durante esos bloques el grafo nuevo **se alimenta** con la señal y su salida se descarta, así sus delay lines y filtros ya están llenos al entrar.
3. **Fade-in de 5 ms** con el grafo nuevo. Si no alcanzó a llenarse su latencia durante el arranque, la rampa espera a que salga señal.
4. **Endpoint Dry** (mix 0): no pasa por el grafo, así que sigue sacando la entrada sin hueco.

**Medición** (seno de 220 Hz, master −20 dB; test "B-11 graph transition"):

| Caso | Bloques crudos | Bloques en silencio | Pico en la transición vs. normal | Paso máximo transición / normal |
|---|---|---|---|---|
| Agregar pedal, buffer 64 | 0 (antes 6) | 7 | −31.3 / −29.6 dBFS | 0.00078 / 0.00090 |
| Agregar pedal, buffer 32 | 0 | 9 | −31.2 / −29.6 | 0.00078 / 0.00090 |
| Agregar pedal, buffer 512 | 0 | 6 | −30.3 / −30.4 | 0.00094 / 0.00087 |
| Bypass que cambia la latencia (gate), buffer 64 | 0 | 8 | −30.2 / −29.6 | 0.00086 / 0.00087 |
| Arranque en frío, buffer 64 | 0 (antes 5) | 6 | −30.4 / −30.4 | 0.00086 / 0.00087 |

- **Sin click:** el paso máximo entre muestras en la transición queda igual al de la señal normal. Un corte duro de este seno daría unas 35 veces más.
- **Hueco audible:** fade-out + 6 bloques de silencio + fade-in, es decir ~18 ms con buffer 64 y ~74 ms con buffer 512. Antes, en ese mismo tiempo sonaba DI crudo a +19 dB. Achicar el hueco o hacer crossfade con spillover es T2-15.
- **Costo:** durante los bloques de arranque el grafo nuevo se procesa (antes no se procesaba). Es el mismo costo que un bloque normal.
- **rt-profile Release:** 63 PASS, 1 WARN, 0 FAIL (`artifacts/rt-profile-release-b11.json`).
  - Promedios: mediana +4.2% en los escenarios de motor, frente a +8.2% en los `proc_*`, que no tocan el motor. Es ruido de la máquina.
  - El WARN es un pico aislado en `stress_block_32` (1 bloque de 675, 79% del presupuesto; en una segunda corrida, 61% y PASS).
  - En esas corridas los picos subieron unas 2 veces en casi todos los escenarios, también en los `proc_*`, así que no se atribuye al arreglo. Conviene repetir el rt-profile con la máquina en reposo antes de mergear.
- **Falla del harness (aparte):** dos corridas del script de rt-profile dejaron el reporte vacío (0 bytes) un segundo después de lanzar NOVA. Al repetirlas, funcionaron.

**Hallazgos laterales:**
- **B-14:** `LookaheadDelay::process()` del Noise Gate trata un retardo de 0 como un retardo de buffer completo: lee la muestra más vieja antes de escribir. Live usa 12 muestras, así que no lo afecta, pero G3 (0 ms) necesitaría arreglarlo.
- **B-15:** el test "AudioEngine mixed-zone insertion keeps zone-canonical effective routing" fallaba de forma intermitente en Release, también en `HEAD` sin F0 (3 de 3). Causa: no esperaba la carga del IR del Cabinet, que corre en un hilo de fondo. Arreglado con una espera de 500 ms (Release 3/3 PASS).

## 12. Referencias

**Código (NOVA):**
- Limiter: `Source/Core/DSP/Global/OutputChain.h` (`PeakLimiter`, `processWithoutLookahead`, `estimateLinkedTruePeak`), `OutputChain.cpp` (`limiterDbToLinear`, `applySoftCeiling`, `updateReportedLatencyForLimiter`).
- Gates: `Source/Effects/Pedals/Gate/NoiseGatePedal.h`, `Source/Core/DSP/Global/InputChain.h/.cpp`.
- Latencia, bypass y grafo: `Source/Effects/Pedals/Base/ProcessorBase.h`, `Source/Core/AudioEngine.cpp`, `Source/Core/Audio/GraphBuilder.h`, `Source/Core/Audio/DryWetMixer.h`, `Source/Core/Audio/DiagnosticsManager.h`.
- Host: `Source/Core/PluginProcessor.cpp` (`applyHostLatencyIfChanged`, `processHostBypassDelay`, transporte).
- Estado: `Source/Core/PluginStateModel.h`, `Source/Core/NovaSettingsService.h`, `Source/Core/Constants.h`.

**JUCE 8.0.12:**
- `juce_dsp/processors/juce_Oversampling.cpp`, `juce_dsp/filter_design/juce_FilterDesign.cpp`, `juce_dsp/frequency/juce_Convolution.h`.
- `juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp`, `juce_audio_processors_headless/utilities/juce_VST3ClientExtensions.h`.
- `VST3_SDK/pluginterfaces/vst/ivstparameterfunctionname.h`.

**Informes internos:**
- `docs/reports/P12A-outputchain-limiter-headroom-design-report.md`
- `docs/reports/P12D-outputchain-limiter-default-safety-fix-results.md`
- `docs/reports/p12d-golden1-outputchain-rebaseline.md`
- `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-report.md`
- `docs/reports/p13a-pdc-latency-host-smoke-procedure.md`
- `docs/reports/p14b-p1-drive-dynamics-measurement-harness.md`
- `artifacts/nova-host-vst3-pdc-validation.log`
- `artifacts/p9f-draft-preset-limiter-telemetry-report.txt`
- `docs/golden-metrics/p4-offline-qa-baseline.json`

**Literatura:**
- Giannoulis, Massberg, Reiss, *Digital Dynamic Range Compressor Design — A Tutorial and Analysis*, JAES 60(6), 2012, pp. 399–408 — https://aes2.org/publications/elibrary-page/?id=16354
- Parker, Zavalishin, Le Bivic, *Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution*, DAFx-16 (2016).
- Bilbao, Esqueda, Parker, Välimäki, *Antiderivative Antialiasing for Memoryless Nonlinearities*, IEEE SPL 24(7) (2017).
- True peak (BS.1770, oversampling 4x, subestimación de hasta ~0.69 dB): https://www.forasoft.com/learn/audio-for-video/articles-audio/true-peak-dbtp-inter-sample (fuente secundaria)
- FabFilter Pro-L 2, true peak con 4x y lookahead mínimo de 0.1 ms: https://www.fabfilter.com/help/pro-l/using/truepeaklimiting

**DAW y VST3:**
- Docs del SDK VST3 sobre `getLatencySamples` y `kLatencyChanged`: https://dplug.dpldocs.dlang.org/dplug.vst3.ivstaudioprocessor.IAudioProcessor.getLatencySamples.html
- Steinberg, "Reporting latency change?": https://forums.steinberg.net/t/reporting-latency-change/201601
- Ardour, latencia VST3 sin reactivar: https://git.ardour.org/ardour/livetrax/commit/b8b232cfab5ae7f247ffa61c7a38238d41a9455a
- JUCE, latencia en Cubase y Logic tras stop/start: https://forum.juce.com/t/issue-with-latency-reporting-edit-now-fixed/15339
- JUCE, Ableton requiere stop/start: https://forum.juce.com/t/playback-record-stop-start-pdc-ableton/67582
- KVR, Reaper refresca el PDC con el transporte: https://www.kvraudio.com/forum/viewtopic.php?p=7633753
- JUCE, clicks al cambiar la latencia: https://forum.juce.com/t/prevent-click-when-changing-latency-with-setlatencysamples/29846
- JUCE, Studio One y `restartComponent`: https://forum.juce.com/t/vst3-latency-reporting-in-presonus-studio-one-fails/28249
- FL Studio, PDC automático: https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/mixer_mixermenu.htm
- Cubase/Nuendo, Constrain Delay Compensation: https://archive.steinberg.help/nuendo/v12/en/cubase_nuendo/topics/vst_instruments/vst_instruments_constrain_delay_compensation_c.html
- Cubase/Nuendo, Plug-in Delay Compensation (botón Live en dinámicos con lookahead): https://archive.steinberg.help/nuendo/v12/en/cubase_nuendo/topics/audio_effects/audio_effects_plugin_delay_compensation_c.html
- Ableton, Reduced Latency When Monitoring: https://help.ableton.com/hc/en-us/articles/209072249
- Ableton, Delay Compensation FAQ: https://help.ableton.com/hc/en-us/articles/209072409

**Procesadores comerciales (referencia; cifras de foros, confianza baja):**
- Line 6 Helix ~1.8 ms entrada → salida (cita de soporte de Line 6 en el foro): https://line6.com/support/topic/14264-helix-faq/page/21/
- Fractal Axe-Fx III ~2.2 ms en modo calidad, FM3 2.9 ms y FM9 4.6 ms (medición con osciloscopio de soporte de Fractal), con un modo de oversampling "Min Latency" (fase mínima), análogo a este modo Live: https://forum.fractalaudio.com/threads/fm9-latency-around-5ms.200203/post-2515095, https://forum.fractalaudio.com/threads/latency-of-amp-modelers.184918/post-2279853
- Contexto: estas cifras incluyen conversores AD/DA. NOVA en Live (0.4 ms de DSP) más una interfaz con ASIO queda en el mismo orden que el hardware dedicado; en Studio, el DSP solo ya ocupa 3.7 ms.

## Anexo: análisis desechable

Scripts en el scratchpad de la sesión (no versionados; F0 los reemplazó con la suite `T2-01 F0 Live mode measurements` en C++):
- `os_latency.py`: réplica del diseño halfband IIR de JUCE.
- `limiter_sim.py` y `splatter.py`: OutputChain y las alternativas de la sección 3.
- `gate_sim.py`: NoiseGatePedal e InputChain.

Las cifras de las secciones 3 y 4 salen de ahí, con semillas fijas.
