# Roadmap de optimización de latencia y CPU — NOVA

- **Creado:** 2026-10-06
- **Estado:** backlog. Nada implementado todavía.
- **Origen:** sesión de exploración "middleware de audio → latencia → DSP" (análisis del código + perfiles Release existentes).
- **Objetivo:** llevar la latencia ida y vuelta (guitarra → oído) al mínimo físico posible con **cualquier** combinación de dispositivos de entrada/salida, y bajar el uso de CPU para sostener buffers chicos sin cortes.

## Cómo usar este documento

- Cada mejora tiene un ID (`T0-xx`, `T1-xx`, `T2-xx`) y un estado: `Pendiente`, `En curso`, `Hecho` o `Descartado`.
- Al cerrar un ítem se anota en su fila: fecha, commit y resultado de la validación (CPU antes → después, golden metrics).
- Los ítems `T2` **no se implementan directo desde aquí**. Cada uno necesita primero su propio documento de investigación (`docs/reports/LAT-T2-xx-<nombre>.md`) con diseño, alternativas, riesgos, plan de pruebas y criterio de éxito.
- Todas las ganancias son **estimaciones** hasta medirlas con `T0-01` y `T0-02`.

---

## 1. Resumen

| | Hoy | Meta realista |
|---|---|---|
| Latencia ida y vuelta (interfaz USB + audífonos de cable, preset típico) | ~28–38 ms (sin ASIO, WASAPI shared) | ~3–5 ms |
| Latencia propia de NOVA (DSP) | ~3.5 ms | ~0–0.5 ms (modo Live) |
| CPU de una cadena típica (Release, 48 kHz) | ~10% (pico 23% con buffer de 32) | ~2–3% |

- **T0 (sin riesgo):** la ganancia grande en ms es activar ASIO. El resto son ahorros de CPU con salida **bit-idéntica** e información honesta para el usuario.
- **T1 (riesgo bajo):** ahorros grandes de CPU con cambios numéricos mínimos y verificables.
- **T2 (riesgoso):** aquí están la mayoría de los ms restantes (modo Live, línea muteada, 96 kHz, puente entre dispositivos) y la reestructuración profunda del motor.

> **Importante:** bajar CPU no baja latencia por sí solo. La CPU libre permite usar buffers más chicos (o 96 kHz) sin cortes. Ahorro por cada vez que el buffer se divide a la mitad, a 48 kHz (entrada + salida): 128→64 = −2.7 ms, 64→32 = −1.3 ms, 32→16 = −0.7 ms.

---

## 2. Línea base

### 2.1 Presupuesto de latencia (interfaz USB con ASIO, buffer 64 @ 48 kHz)

| Componente | ms aprox. | ¿Lo controla NOVA? |
|---|---|---|
| Conversores AD + DA (filtros internos del hardware) | 0.5–1.5 | Solo subiendo la frecuencia de muestreo (T2-11) |
| Buffers de entrada + salida | 2.7 | Sí, con CPU libre y el driver adecuado |
| Margen de seguridad del driver USB | 0.5–2 | No (depende del fabricante) |
| Limiter de salida (lookahead siempre activo desde P12D) | 2.0 | Sí (T2-01) |
| Noise Gate pedal (lookahead, si está en la cadena) | 1.5 | Sí (T2-01) |
| Oversampling IIR por pedal/amp | <0.1 cada uno | Sí (T2-05) |
| Latencia de la línea muteada (alineación del grafo) | 0–2 | Sí (T2-02) |
| Aire, si se usa monitor o altavoz | 2.9 por metro | Solo informando al usuario |

### 2.2 Latencia interna medida en el código

| Fuente | Valor | Dónde |
|---|---|---|
| Lookahead del limiter de OutputChain | 2.0 ms (96 muestras @ 48k), siempre activo desde P12D | `Source/Core/DSP/Global/OutputChain.h:263`, `Source/Core/DSP/Global/OutputChain.cpp:187` |
| Lookahead del NoiseGate pedal | 1.5 ms (72 muestras @ 48k) | `Source/Effects/Pedals/Gate/NoiseGatePedal.h:219` |
| Apertura del gate de InputChain | 4 ms. No es latencia, pero suaviza el ataque y la nota se siente más tarde | `Source/Core/DSP/Global/InputChain.h:361` |
| Oversampling 8x IIR (11 procesadores) | Pocas muestras cada uno | `oversampler(2, 3, filterHalfBandPolyphaseIIR)` |

### 2.3 CPU en Release (baseline `docs/rt-profile/p4c-rt-profile-release-baseline.json`)

| Escenario | Buffer | CPU promedio | CPU pico |
|---|---|---|---|
| `clean_empty_chain` | 128 | 0.6% | 1.5% |
| `overdrive_v2_nominal` | 128 | 5.6% | 8.8% |
| `high_gain_amp_nominal` | 128 | 6.6% | 9.3% |
| `cabinet_nominal` | 128 | 2.2% | 3.9% |
| `reverb_cloud_tail` | 128 | 1.9% | 3.0% |
| `overdrive_cleanamp_reverb_chain_nominal` | 128 | 9.9% | 13.9% |
| `stress_block_32` | 32 | 10.1% | 22.8% |
| `sample_rate_96000` | 128 | 19.5% | 27.1% |

Observación: un solo Overdrive cuesta casi lo mismo que el amp high-gain completo, y más que cab + reverb juntos.

---

## 3. Criterios de riesgo

| Tier | Definición | Validación mínima |
|---|---|---|
| **T0 — sin riesgo** | Salida de audio bit-idéntica, o cambios que no tocan el audio (diagnóstico, UI informativa, tests, un build flag que no cambia el comportamiento por defecto). Sin cambios de estado/presets, sin hilos nuevos, revert trivial. | Build Release + base validation + golden metrics sin cambios + checksum bit-exacto (`T0-01`) + rt-profile Release |
| **T1 — riesgo bajo** | Cambio numérico pequeño y medible (diferencia ≤ −60 dB contra la versión actual) o feature aislada y opcional. Localizado en un archivo o procesador. | Todo lo de T0 + null test contra la versión anterior + golden dentro de tolerancia + escucha A/B |
| **T2 — riesgoso** | Toca la arquitectura del motor, el audio thread, hilos nuevos, estado/presets, decisiones de seguridad previas (P12D), configuración del sistema operativo, o cambia el sonido de forma audible. | Documento de investigación propio **antes** de escribir código |

---

## 4. Protocolo de validación

```powershell
# Build Release
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin

# Suite de regresión base
powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -Configuration Release -Platform x64

# Golden metrics
powershell -ExecutionPolicy Bypass -File scripts/run-golden-audio-metrics.ps1 -Configuration Release -Platform x64

# CPU contra la baseline Release
powershell -ExecutionPolicy Bypass -File scripts/run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json

# Bit-exactitud (obligatorio para T0)
powershell -ExecutionPolicy Bypass -File scripts/run-bitexact-checksums.ps1 -Configuration Release

# Policy del audio thread
powershell -ExecutionPolicy Bypass -File scripts/check-audio-thread-policy.ps1
```

### Reglas de implementación para T0 (salida bit-idéntica)

- **Técnica canónica: memoización por valor exacto.** `if (x != lastX) { lastX = x; lastY = f(x); }`, con `lastX` inicializado en NaN para forzar el primer cálculo. Misma función y mismos argumentos dan el mismo resultado bit a bit: el proyecto compila con `/fp:precise` (no hay `/fp:fast` ni `/arch` en el `.vcxproj`).
- No reordenar operaciones de punto flotante dentro de una expresión (mantener `a * b * c` en el mismo orden).
- **Limitación:** la memoización baja el CPU **promedio**. El **peor caso** (girando perillas, con smoothing activo) queda igual que hoy. Bajar el peor caso es `T1-07`.

---

## 5. T0 — Sin riesgo

| ID | Mejora | Dónde | Ganancia estimada | Estado |
|---|---|---|---|---|
| T0-01 | **Test de checksum bit-exacto por procesador.** Render determinístico (notas punteadas en varias dinámicas, silencios y ruido con semilla fija) con automatización de parámetros, y hash de los bits de salida. | `Source/Core/OfflineQADiagnostics.h` (`runBitExactChecksums`), `scripts/run-bitexact-checksums.ps1`, baselines `docs/golden-metrics/t0-bitexact-{debug,release}-x64-baseline.json` | Habilitador: demuestra que los cambios T0 no tocan el sonido | **Hecho, 2026-10-07.** 24 procesadores × 2 configuraciones (48 kHz/64 mono, 44.1 kHz/128 estéreo). Determinístico entre corridas |
| T0-02 | **Escenarios de rt-profile por procesador:** `proc_*` (valores por defecto) y `proc_*_knobs` (todos los parámetros continuos rampeando: peor caso). | `Source/Core/OfflineQADiagnostics.h` | Habilitador: medir antes/después | **Hecho.** 48 escenarios nuevos (64 en total) |
| T0-03 | **Activar ASIO:** `JUCE_ASIO="1"` en `JUCEOPTIONS` de `NOVA.jucer` y re-exportar con Projucer. El default no cambia: JUCE agrega ASIO al final de la lista de tipos y el escaneo solo lee el registro. | `NOVA.jucer:1068` | **−15 a −30 ms** cuando el usuario elige ASIO | **Hecho (solo desarrollo), 2026-10-07.** Build Debug OK; base validation PASS (311 resultados, 13482 checks, 0 fallos). **Antes de publicar falta firmar el acuerdo ASIO** (ver nota) |
| T0-04 | **Latencia honesta en el wizard:** entrada + salida reportadas por el driver + latencia de NOVA, en vez de `buffer / sampleRate`. | `Source/GUI/Wizards/AudioSetupWizard.h` | 0 ms; números reales para el usuario | **Hecho.** Muestra "~X ms round trip (buffer Y ms)" en el escaneo y en la barra de estado |
| T0-05 | **Si no hay ASIO, el wizard recomienda "Windows Audio (Low Latency Mode)" o exclusive** en vez de shared, explicando que exclusive bloquea el dispositivo para otras apps. Solo guía; el usuario decide. | `Source/GUI/Wizards/AudioSetupWizard.h` | **−5 a −20 ms** para usuarios sin ASIO, según el driver | **Hecho.** Banner + recomendación cuando el driver es "Windows Audio" (shared) o DirectSound y no hay ASIO |
| T0-06 | **Desglose de latencia interna por nodo** en el reporte de diagnóstico. | `Source/Core/Audio/DiagnosticsManager.h`, `Source/Core/AudioEngine.cpp` | Visibilidad | **Hecho.** Línea `latency.breakdown` (grafo, inputChain, lineA, lineB, outputChain) y `latencySamples` por pedal |
| T0-07 | **Overdrive.** Memoizar `decibelsToGain` (un `pow` por sub-muestra y canal), saltar `updateToneModel()` (8 `std::exp` en double por muestra) cuando drive/tone/texture no cambian, memoizar el `cos`/`sin` de la mezcla. | `Source/Effects/Pedals/Overdrive/OverdrivePedal.h` | Estimado −30 a −45% | **Hecho. Medido: −8%** (5.26 → 4.82%). La estimación fue optimista: el costo lo dominan los `tanh` y los 8 filtros a 8x (ver T1-03 y T1-04) |
| T0-08 | **Distortion.** `decibelsToGain(...)` y `highGainCompatibilityTrim` fuera del loop por canal y memoizados; `modeParam->getIndex()` leído una vez por bloque. | `Source/Effects/Pedals/Distortion/DistortionPedal.h` | — | **Hecho. −12%** (11.64 → 10.23%; pico 17.0 → 14.2%) |
| T0-09 | **Boost.** `decibelsToGain(currentGain)` memoizado y derivados (`driveWeight`, `denseDrive`, `ampInputCompatibilityTrim`…) calculados por muestra base en lugar de por sub-muestra. | `Source/Effects/Pedals/Boost/BoostPedal.h` | — | **Hecho. −41%** (5.52 → 3.23%) |
| T0-10 | **CleanAmp.** `drive` y `compensation` memoizados. | `Source/Effects/Amplifiers/CleanAmp.h` | — | **Hecho.** Sin cambio medible (dentro del ruido) |
| T0-11 | **ClassicWah.** Coeficientes de envolvente (3 `std::exp`) y `std::log(minFreq/maxFreq)` memoizados. | `Source/Effects/Pedals/Wah/ClassicWahPedal.h` | — | **Hecho.** Sin cambio medible: domina la matemática que depende de la señal (`tan`, `pow` y `exp` del barrido) → T1-03 |
| T0-12 | **Compressor.** Memoizados: `decibelsToGain(makeup)`, 3 `exp` de coeficientes y el `cos`/`sin` por canal y muestra de `mixDryWetEqualPower`. | `Source/Effects/Pedals/Compressor/CompressorPedal.h` | — | **Hecho. −12%** |
| T0-13 | **Phaser.** Perfil del modo, `log` de los límites de frecuencia y `exp(spread)` por etapa calculados una vez por bloque; `cos`/`sin` de la mezcla memoizados. | `Source/Effects/Pedals/Phaser/PhaserPedal.h` | — | **Hecho. −15%** |
| T0-14 | **Barrido del mismo patrón en el resto.** Neural: tone model (7 `exp` + `pow` por muestra base) y `pow` por sub-muestra memoizados. `cos`/`sin` de mezcla memoizados en Fuzz, Reverb, Delay, Chorus, Flanger y Tremolo. Amps (Classic, HighGain, Chime, Boutique), Gate, EQ y etapas globales: solo tienen matemática que depende de la señal o que ya es por bloque. | Varios | — | **Hecho.** Neural −16%; el resto dentro del ruido |
| T0-15 | **Corregir CLAUDE.md:** el oversampling es 8x (`factor = 3`), no 4x. | `CLAUDE.md` | Documentación correcta | **Hecho.** También se documentó el script bit-exacto |
| T0-16 | **Octave: detector de pitch** (hallazgo de T0-02). Corre una autocorrelación en el audio thread (~1500 análisis/s × ~237 lags × 256 muestras) con `wrapIndex` (bucles `while`) y sanitize en cada acceso. Ahora recorre el ring buffer con índices decrecientes y calcula `sum(x²)` una vez por análisis, con el mismo orden de suma (bit-idéntico). | `Source/Effects/Pedals/Octave/OctavePedal.h` (`PeriodTracker`) | — | **Hecho. −72%** (43.5 → 12.1%; pico 70 → 13%) |

**Nota T0-03 (licencia):** el header ASIO que incluye JUCE 8.0.12 (`juce_audio_devices/native/asio/LICENSE.txt`) tiene licencia dual:
- **Steinberg ASIO License:** exige un acuerdo firmado con Steinberg antes de publicar software propietario.
- **GPLv3**, como alternativa.

Técnicamente el cambio es T0; lo que falta es la decisión legal/comercial.

**Decisión (2026-10-07):** NOVA usa JUCE 8 **Starter** (gratis) y se planea vender como software cerrado, así que la opción GPLv3 no aplica. El camino es la **licencia propietaria de Steinberg**:
- **Costo:** históricamente sin costo; consiste en firmar el acuerdo.
- **Cuándo:** el `LICENSE.txt` del SDK lo exige *antes de publicar*, por lo que las builds locales de desarrollo pueden activar ASIO ya.
- **Pendiente antes del lanzamiento:**
  - Firmar el acuerdo de licencia ASIO de Steinberg (verificar el texto vigente en steinberg.net).
  - Agregar el aviso de marca ASIO en la documentación o en "Acerca de".
  - Confirmar en juce.com el tope de ingresos actual de JUCE Starter (según el anuncio de JUCE 8: $20k anuales; al superarlo se pasa a Indie).
- **Nota relacionada:** el VST3 SDK incluido en JUCE 8.0.12 es la versión 3.8, con licencia MIT; ya no requiere un acuerdo con Steinberg.

**Checklist para producción (ASIO SDK Licensing Agreement 2.0.5, SDK 2.3.4 del 15.10.2025).** Revisado desde una copia en un mirror de GitHub (`audiosdk/asio`); hay que bajar la copia oficial desde Steinberg antes de firmar.

1. **Desarrollo:** se puede usar ya. El SDK también se ofrece bajo GPLv3, y la GPL solo impone obligaciones al distribuir.
2. **Firmar:** completar los datos del Licensee ("Company or Organization", representante, dirección, país) y el contacto técnico. Enviar a `reception@steinberg.de` (o fax +49 40 21035 300).
3. **Esperar la contrafirma:** el acuerdo rige solo cuando ambas partes firmaron (§9.1). **No publicar antes.**
4. **Costo:** ninguno; el acuerdo no tiene regalías ni tarifas (§4). No requiere notario; ley aplicable: Alemania.
5. **Persona física vs. empresa:** el formulario está pensado para "Company or Organization", y la licencia es intransferible (§2.1).
   - Si se firma como persona física y luego se registra la empresa, hay que firmar de nuevo a nombre de la empresa (el acuerdo nuevo reemplaza al anterior).
   - Pendiente preguntarle a Steinberg si acepta personas físicas.
6. **Obligaciones dentro del producto (distribución por descarga, §3.1.b):**
   - Mostrar "ASIO" + el aviso, o el logo "ASIO compatible" + el aviso, en el About y/o el splash y/o la documentación.
   - Incluir al menos un documento con el aviso dentro del instalador.
   - Si el About menciona a terceros (p. ej. JUCE), el aviso ASIO debe aparecer ahí también.
   - Texto exacto del aviso: "ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries".
7. **Obligaciones en la web:**
   - Logo + aviso en la página del producto (puede ser en una subpágina de especificaciones) y en la página legal / imprint.
   - Usar el logo según las Usage Guidelines incluidas en el SDK.
8. **Prohibiciones:** no usar "ASIO" en el nombre del producto ni de la empresa, ni en merchandising.
9. **Registro de uso de la marca** por país, año y producto (§3.1.j): no aplica mientras NOVA tenga menos de 1000 usuarios.

---

## 5b. Resultados de T0 (2026-10-07)

- **Bit-exactitud:** 48/48 idénticos en Debug y en Release (`scripts/run-bitexact-checksums.ps1`). El sonido no cambió.
- **Regresión:** base validation PASS (311 resultados, 13482 checks, 0 fallos); golden metrics PASS.
- **Policy del audio thread:** 0 hallazgos en rutas de audio. Las guardas de alcance P9F/P9G fallan solo porque `AudioEngine.cpp` tiene cambios sin commitear (líneas del reporte de diagnóstico, message thread); pasan al commitear.
- **CPU** (Release, buffer 128, una corrida por escenario; el ruido entre corridas es de ±5–20% en escenarios chicos):

| Escenario | Antes | Después | Cambio |
|---|---|---|---|
| `proc_octave` | 43.54% (pico 70.0%) | 12.08% (pico 13.1%) | **−72%** |
| `proc_boost` | 5.52% | 3.23% | **−41%** |
| `proc_neural` | 5.75% | 4.85% | −16% |
| `proc_phaser` | 1.73% | 1.47% | −15% |
| `proc_distortion` | 11.64% (pico 17.0%) | 10.23% (pico 14.2%) | −12% |
| `proc_compressor` | 1.11% | 0.98% | −12% |
| `proc_overdrive` | 5.26% | 4.82% | −8% |
| `overdrive_cleanamp_reverb_chain_nominal` | 9.62% | 8.92% | −7% |
| `stress_block_64` | 10.75% | 9.12% | −15% |
| `sample_rate_96000` | 23.13% | 17.84% | −23% |

- **Lectura honesta:** el gran ganador es el Octave, que antes podía causar cortes por sí solo con buffers chicos. En cadenas típicas la baja es de ~5–15%. Lo que domina ahora (`tanh` y filtros a 8x, estéreo duplicado) es T1/T2.
- **Evidencia:** `docs/rt-profile/t0-rt-profile-release-before.json` y `docs/rt-profile/t0-rt-profile-release-after.json`.

---

## 6. T1 — Riesgo bajo

| ID | Mejora | Dónde | Ganancia estimada | Riesgo concreto | Validación extra | Estado |
|---|---|---|---|---|---|---|
| T1-01 | **Cab como IIR exacto.** El IR sintético es: 8 biquads → 2 reflexiones → ventana exponencial rⁿ. Multiplicar una respuesta por rⁿ equivale a escalar polos y ceros por r (b1·r, b2·r², a1·r, a2·r²; cada reflexión g pasa a g·rᵈ). Resultado: el mismo cab sin convolución FFT. | `Source/Effects/Cabinets/SyntheticIR.h`, `CabinetPedal.h`, `Modern4x12Cabinet.h`, `Vintage2x12Cabinet.h` | Cab 2.2% → ~0.3%; costo constante con buffers chicos (la FFT cuesta más con 16–32 muestras) | Difiere solo por el truncado actual del IR (~−68 dB) | Null test < −60 dB contra el IR actual | Pendiente |
| T1-02 | **Bug: el decay del IR depende de la frecuencia de muestreo** (`0.993f` fijo por muestra: a 96 kHz decae el doble de rápido). Usar r = 0.993^(48000/fs). | `Source/Effects/Cabinets/SyntheticIR.h:180` | El cab suena igual a 44.1, 48 y 96 kHz | Cambia el sonido a 44.1 y 96 kHz (corrección intencional; a 48 kHz no cambia) | Escucha a 44.1k | **Hecho, 2026-10-12.** Además del decay apareció B-16 (nivel): la normalización por pico hacía el cab **~6 dB más fuerte a 96 kHz** y ~0.6 dB más bajo a 44.1. Ahora el decay escala con la frecuencia y el nivel se iguala al del IR de 48 kHz. Los 3 cabs quedan a ≤ 0.52 dB de 48 kHz (100 Hz–5 kHz). A 48 kHz, bit-idéntico |
| T1-03 | **Aproximaciones rápidas de `tanh`, `exp` y `pow`** (racional / `exp2`) en saturadores y detectores. | HighGainAmp (4 `tanh` por sub-muestra), Overdrive, Distortion, Fuzz, ChimeAmp, BoutiqueAmp, CleanAmp | −50 a −80% del costo de las curvas | Error ~1e-4 (≈ −80 dB) | Null test + golden | Pendiente |
| T1-04 | **Filtros lineales fuera de la zona a 8x.** Dentro de la banda útil, los filtros lineales conmutan con el cambio de frecuencia de muestreo; solo la curva no lineal necesita 8x. | Overdrive (3 pre + 5 post), HighGainAmp (4 post), CleanAmp (3 shelves), Distortion (pre) | −30 a −60% por pedal | Diferencias pequeñas en agudos por el warping de los filtros | Golden + A/B | Pendiente |
| T1-05 | **Oversampling con `isMaxQuality = false`:** rechazo de −70 dB en vez de −90 dB. El cab corta igual todo lo que está arriba de 5–8 kHz. | 11 procesadores | ~−20–25% del costo de los filtros halfband; latencia de oversampling algo menor | Algo más de aliasing (inaudible después del cab) | Golden + A/B en high-gain | Pendiente |
| T1-06 | **LFOs con oscilador por rotación** (2 multiplicaciones + 2 sumas) en vez de `std::sin`. | Fuzz (4 `sin` por sub-muestra), Chorus, Flanger, Phaser, Tremolo | CPU | Deriva numérica mínima (renormalizar en cada bloque) | Null test | Pendiente |
| T1-07 | **Coeficientes y curvas a tasa de control** (cada 16–32 muestras, interpolando) mientras hay smoothing. | Overdrive, Boost, Wah, Phaser, Compressor… | **Baja el peor caso** (girando perillas). Esto sí abre margen para buffers chicos | Rampas en escalones de 16–32 muestras (inaudibles si se interpola) | A/B girando perillas | Pendiente |
| T1-08 | **Guards y telemetría por pedal más baratos.** Detectar por bloque con SIMD (una suma de `x·0` da NaN si alguna muestra es NaN/Inf; `findMinAndMax` para picos) y hacer el scrub completo solo si hay anomalía. RMS en float con `FloatVectorOperations`. Capturas por muestra solo en modo diagnóstico. | `Source/Core/PedalSignalTelemetry.h`, pedales con `scrubBuffer` / `captureInput` | CPU | Los reportes de QA pierden detalle en producción; los tests de P1 Pedal Safety deben seguir pasando | Base validation | Pendiente |
| T1-09 | **ProcessorBase:** los pedales activos con latencia copian el dry y mueven delay lines en cada bloque aunque no haya transición de bypass. Mantener solo un ring buffer mínimo. | `Source/Effects/Pedals/Base/ProcessorBase.h:243` | CPU | Crossfade de bypass | Tests de bypass | Pendiente |
| T1-10 | **Tuner con autocorrelación por FFT** + diezmado a ~8 kHz (hoy ~12M operaciones por análisis). | `Source/Core/DSP/Services/TunerService.cpp:146` | CPU/batería (hilo de fondo, no el de audio) | Precisión del tuner | Tests de tuner | Pendiente |
| T1-11 | **Sacar el proceso del power throttling de Windows (EcoQoS)** con `SetProcessInformation(ProcessPowerThrottling)` al arrancar el Standalone. | Arranque del Standalone | Evita que Windows degrade NOVA cuando está en segundo plano | Algo más de consumo con NOVA abierto | Prueba con NOVA en segundo plano | Pendiente |
| T1-12 | **Medición de latencia real con cable loopback** (emite un pulso y mide el retorno). | Wizard / Ajustes | Mide la latencia real; habilita T2-10 | Emite un pulso por la salida: nivel bajo (−20 dBFS) y confirmación previa | Prueba manual | Pendiente |
| T1-13 | **Octave: detector de pitch aún más barato.** Opciones: recorrer el buffer en segmentos contiguos sin `if` por muestra; `sum(y²)` incremental entre lags (cambia el redondeo); autocorrelación por FFT; menos lags o análisis menos frecuente. | `Source/Effects/Pedals/Octave/OctavePedal.h` (`PeriodTracker`) | 12% → ~2–5% | Cambios de redondeo o de tracking | Tests de Octave + A/B | Pendiente |
| T1-14 | **Tests en Debug al límite del stack** (hallazgo B-09). Crear los pedales de los tests en el heap (`std::make_unique`) o correr la suite en un hilo con stack más grande. | `Source/Core/AudioEngineTests.cpp` | Robustez: permite que futuros cambios agranden los procesadores | Solo tests | Base validation | Pendiente |

---

## 7. T2 — Riesgosos (investigar uno a uno)

Orden sugerido de investigación (por ms ganados y valor): **T2-10 (prioridad del producto) → T2-01 → T2-02 → T2-11 → T2-09 → T2-03 → T2-04 → T2-07 → resto.**

### T2-01 — Modo Live (sin lookaheads)
- **Estado:** **investigación terminada (2026-10-11):** `docs/reports/LAT-T2-01-live-mode.md`.
- **Decisiones tomadas (2026-10-12):** limiter (a); gate G1 (0.25 ms); global en el Standalone y fuera de los presets; Studio por defecto hasta el A/B; VST3 y gate de InputChain pospuestos; B-11 se arregla antes de F1, como ítem propio; nombre "Live / Studio".
- **F0 hecha (2026-10-12):**
  - Flags inertes `setLiveMode()` en OutputChain y Noise Gate, apagados por defecto y sin conectar.
  - Checksums de OutputChain e InputChain, más dos `@live`.
  - Suite C++ `T2-01 F0 Live mode measurements`.
  - Resultado: Live cumple C2–C6 en C++ (techo 0.97 en todos los casos, P12D −66.3 dB, gate ≤ 1.1 dB en el primer ms). En el A/B hay que escuchar el primer flanco de los overs (+0.9 a +2.2 dB de agudos frente a Studio, solo cuando el limiter actúa).
  - Validación: B-11 confirmado (6 bloques de DI crudo). Bit-exacto 48/48 previos sin cambios (56/56 con las entradas nuevas), base PASS (333/13744/0) en Debug y Release, golden PASS.
  - Detalle en §11.1 del documento.
- **Resultado:**
  - Cadena típica: 3.67 → 0.42 ms (**−3.25 ms**) con el limiter por `processWithoutLookahead` y el gate con un lookahead de 0.25 ms. Con el gate en 0 ms, −3.5 ms.
  - En simulación, el limiter sin lookahead cumple las 4 garantías de P12D.
  - Un soft-clipper solo no sirve: con overs sostenidos distorsiona ~20–25%.
  - Nuevos hallazgos B-11, B-12 y B-13.
- **Ganancia:** hasta **−3.5 ms** (−2 ms del limiter, −1.5 ms del gate) y un ataque que se siente más rápido.
- **Idea:** en modo Live, limiter tipo soft-clipper sin lookahead (ya existe `processWithoutLookahead` en `Source/Core/DSP/Global/OutputChain.h:353`) y gates de latencia cero que abren en 0.1–0.3 ms. El modo Studio conserva el comportamiento actual.
- **Por qué es riesgoso:**
  - Revierte en parte P12D (`docs/reports/P12D-outputchain-limiter-default-safety-fix-results.md`), que armó el limiter siempre por seguridad ante masters calientes.
  - Los tests de P12D y P14B asumen que hay lookahead.
  - Cambiar la latencia en caliente afecta el PDC dentro de un DAW (P13A-F001).
  - Requiere un ajuste nuevo (estado).
- **Preguntas a investigar:** ¿protege el soft-clipper lo suficiente (oídos y equipo)? ¿Ajuste global de la app o por preset? ¿Cómo se reporta la latencia al conmutar dentro de un DAW? ¿Hay clicks del gate rápido en notas suaves?

### T2-02 — Línea muteada fuera del procesamiento
- **Ganancia:** la CPU de una línea completa; 0 a ~2 ms si la línea muteada tiene más latencia que la activa.
- **Hallazgo:** en `LineA_Only` / `LineB_Only` la otra línea se procesa igual; solo se pone su ganancia en 0 (`Source/Core/Audio/RoutingMixer.h:37`, `Source/Core/Audio/GraphBuilder.h:78-85`). Además, el grafo de JUCE retrasa la línea activa para alinearla con la muteada (`DelayChannelOp` en `juce_AudioProcessorGraph.cpp`).
- **Por qué es riesgoso:** toca AudioEngine/GraphBuilder (zona de alto riesgo). Hoy el cambio A↔B es instantáneo justamente porque las dos líneas están siempre activas.
- **Preguntas a investigar:** ¿precalentar la línea inactiva unos bloques antes de conmutar? ¿Reconstruir el grafo o suspender nodos? ¿Cómo evitar el hueco de 6 bloques (`STARTUP_COUNTER_GRAPH_CHANGE`)?

### T2-03 — Ruta mono hasta el primer efecto estéreo
- **Ganancia:** −50% de CPU en la parte más pesada (drives, amp, cab).
- **Hallazgo:** con un solo canal de entrada, el grafo duplica la señal a L y R (`Source/Core/Audio/GraphBuilder.h:75`); con guitarra en un solo jack, InputChain la promueve a ambos canales. Todos los procesadores trabajan doble con datos idénticos.
- **Por qué es riesgoso:** toca los layouts de canales, las entradas estéreo reales, la definición de qué pedales "crean" estéreo y los tests de routing.
- **Preguntas a investigar:** ¿detección automática de mono vs. estéreo? ¿Dónde se separa la señal (primer chorus/delay/reverb, modo dual)?

### T2-04 — ADAA + oversampling reducido
- **Ganancia:** −70 a −90% de CPU en saturadores. ADAA con 2x da una calidad comparable a 4–8x normal.
- **Idea:** en vez de evaluar la curva en puntos sueltos, evaluar su promedio entre muestras usando la antiderivada F: `y[n] = (F(x[n]) − F(x[n−1])) / (x[n] − x[n−1])`. Para `tanh`, F(x) = ln(cosh x).
- **Por qué es riesgoso:**
  - Cambia el sonido y el aliasing.
  - Las curvas con memoria (envolventes de sag, clippers con capacitor) no se pueden tratar de forma directa.
  - Necesita una rama de respaldo cuando x[n] ≈ x[n−1].
- **Preguntas a investigar:** ¿qué pedales tienen curvas sin memoria? ¿ADAA de 1.er o 2.º orden? ¿Qué criterio objetivo de aliasing usar (medición con barrido)?

### T2-05 — Islas de oversampling fusionadas
- **Ganancia:** −50% de filtros halfband en drive → amp consecutivos (hoy: sube, baja, sube, baja), y se elimina la latencia intermedia.
- **Por qué es riesgoso:** cruza la frontera entre procesadores. Requiere T2-07 o un "contexto oversampleado" compartido.

### T2-06 — SIMD (AVX2)
- **Ganancia:** hasta 8 sub-muestras de la curva por instrucción.
- **Por qué es riesgoso:**
  - El build actual es SSE2 (sin `/arch`); AVX2 requiere despacho en runtime para no romper en CPUs viejas.
  - La envolvente de sag por sub-muestra impide vectorizar sin moverla a la tasa base, lo que cambia el sonido.

### T2-07 — Chain compilado (reemplazar `AudioProcessorGraph`)
- **Ganancia:**
  - Menos overhead fijo por callback, que importa con buffers de 16–32.
  - Sin delays de alineación innecesarios.
  - Es la base de T2-02, T2-03, T2-05 y T2-08.
- **Por qué es riesgoso:** es el cambio más riesgoso del documento. Toca el núcleo del motor: swap de grafos, retirement queue, PDC y tests de AudioEngine.

### T2-08 — Preset compiler
- **Idea:** al cargar un preset:
  - detectar la ruta mono;
  - fusionar islas de oversampling;
  - plegar en una sola cascada todo lo lineal posterior a la última distorsión (EQ post, cab, EQ pedal, strip);
  - eliminar nodos muertos.
- **Por qué es riesgoso:** depende de T2-07. Además hay que recalcular en background al mover perillas y hacer crossfade.

### T2-09 — Puente de relojes entre dispositivos + WASAPI exclusive
- **Ganancia:** con dispositivos distintos para entrada y salida, de ~30–50 ms con clicks a ~8–12 ms sin clicks.
- **Hallazgo:** el WASAPI de JUCE con entrada y salida en dispositivos distintos no corrige la deriva de reloj: descarta paquetes o rellena con ceros, lo que produce clicks periódicos.
- **Idea:**
  - Un `AudioIODeviceType` virtual que abre los 2 dispositivos.
  - NOVA procesa al ritmo del reloj de entrada.
  - Hacia la salida: FIFO + resampler adaptativo con control PI.
  - Cada dispositivo se abre en exclusive a su frecuencia nativa.
- **Por qué es riesgoso:**
  - Requiere `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`.
  - Agrega hilos nuevos.
  - Hay que manejar la desconexión en caliente de cualquiera de los dos dispositivos.
  - Nunca hay que abrir el micrófono de un headset Bluetooth: Windows lo pasaría a HFP (16 kHz mono).

### T2-10 — Latency Autopilot (configuración automática de audio) — **prioridad del producto**
- **Estado:** **F1 hecha (2026-10-09):** inventario, motor de decisión puro con 13 unit tests, "Recommended setup" en el wizard con APPLY/UNDO y reversión automática si no llega audio en 2 s, y política de mute de la entrada. Validación: base PASS (324 resultados, 13597 checks, 0 fallos), golden PASS y bit-exacto 48/48 en Debug y Release. Detalle en `docs/reports/LAT-T2-10-latency-autopilot.md` §10.
- **F2 hecha (2026-10-09):** "CALIBRATE BUFFER" en el wizard. Prueba la escalera de buffers desde 32 con la salida en silencio y se queda con el primero sin xruns, con carga pico < 70% y gap entre callbacks < 2×. También: UNDO, CANCEL y DRIVER PANEL para buffers fijos. Validación: base PASS (326 resultados, 13617 checks, 0 fallos), golden PASS, bit-exacto 48/48 en Debug y Release. Siguiente: F3. Detalle en §11 del mismo documento.
- **Decisiones (2026-10-09):** primera ejecución automática con aviso y Deshacer; silenciar la salida durante la calibración; Exclusive permitido en Estudio (nunca en Canciones); monitoreo solo con sugerencias; desmute automático cuando es seguro.
- **Objetivo (pedido del usuario, 2026-10-07):** que el usuario solo "encienda" NOVA y obtenga la mejor configuración posible para su hardware, sin tocar el panel del driver.
- **Idea:**
  1. **Elegir driver:** ASIO nativo del fabricante si existe. Evitar wrappers genéricos (ASIO4ALL, FlexASIO), que suelen bloquear el audio de otras apps. Si no hay ASIO: WASAPI Low Latency → Exclusive → Shared.
  2. **Elegir frecuencia:** 48 kHz (la validada por las golden metrics); 44.1 kHz si el dispositivo no soporta 48 kHz.
  3. **Elegir buffer:** probar de menor a mayor (32 → 64 → 128…) midiendo el jitter de los callbacks y los cortes, y quedarse con el menor estable. Seguir monitoreando y subir un nivel si aparecen cortes.
  4. **Perfiles de uso:**
     - **Estudio / baja latencia** (default): el buffer estable más chico.
     - **Tocar con canciones:** prioriza que otras apps (YouTube, Spotify) sigan sonando. Evita exclusive y ASIO4ALL, y usa algo más de margen en el buffer.
  5. **Recordar la configuración por dispositivo** y re-detectar al conectar o desconectar periféricos.
- **Contexto medido (2026-10-07):** con ASIO activado, un driver multi-cliente (Yamaha Steinberg USB) permite tocar con una canción de YouTube/Spotify sonando por la misma interfaz sin problemas. La latencia de la canción no importa (uno toca sobre lo que oye); solo importa la latencia del camino de la guitarra.
- **Por qué es riesgoso:**
  - Cambia la configuración de audio de forma automática (hay que pedir confirmación y poder revertir).
  - Algunos drivers solo permiten cambiar el buffer desde su propio panel.
  - El conteo de cortes no está disponible en todos los drivers (hay que medir el timing de los callbacks nosotros).
  - Depende de T0-04 y T1-12.
- **Preguntas a investigar:** ¿cómo detectar si un driver ASIO es multi-cliente? ¿Cómo distinguir un ASIO nativo de un wrapper? ¿Cuánto debe durar la prueba de estabilidad del buffer? ¿Qué hacer con el VST3, donde el DAW controla el dispositivo?

### T2-11 — 96 kHz con oversampling adaptativo
- **Ganancia:** −1.5 a −2 ms (los buffers y los filtros AD/DA duran la mitad).
- **Idea:** el factor de oversampling depende de la frecuencia (8x @ 48k, 4x @ 96k, 2x @ 192k), así las curvas cuestan lo mismo en cualquier frecuencia.
- **Por qué es riesgoso:**
  - Consistencia del sonido entre frecuencias (las golden metrics se validan a 48 kHz).
  - Las etapas lineales cuestan 2x en CPU.
  - Depende de que el hardware soporte 96 kHz.

### T2-12 — Line A ∥ Line B en paralelo
- **Ganancia:** hasta ~2x en presets duales pesados.
- **Por qué es riesgoso:**
  - Necesita un hilo worker de tiempo real: P-core, espera activa y fallback (si el worker se atrasa, el hilo principal hace el trabajo).
  - Sube el consumo.

### T2-13 — Reverb/delay "wet" en un hilo worker
- **Idea:** calcular la cola con 1 bloque de retraso, escondido dentro del predelay.
- **Por qué es riesgoso:** hilos y sincronización; con un predelay menor a un bloque hay que compensar.

### T2-14 — Scheduling del sistema
- **Idea:**
  - Fijar los hilos de audio a P-cores (`SetThreadSelectedCpuSets`).
  - Plan de energía de audio opcional (opt-in).
  - `VirtualLock` de la memoria DSP.
- **Por qué es riesgoso:**
  - Son ajustes del sistema que requieren consentimiento.
  - El hilo ASIO pertenece al driver.
  - Puede afectar a otros programas.

### T2-15 — Cambio de preset sin hueco + spillover
- **Hallazgo:** después de reconstruir el grafo se saltan 6 bloques (`Source/Core/AudioEngine.cpp:487`) y se cortan las colas de delay/reverb.
- **Idea:** preparar el grafo nuevo en background y hacer crossfade, con spillover de delay/reverb.
- **Por qué es riesgoso:** toca el ciclo de vida del grafo y duplica la CPU durante el crossfade.

### T2-16 — Cancelador de hum adaptativo
- **Idea:** filtros notch IIR en 50/60 Hz y armónicos, con latencia cero, para depender menos del gate.
- **Por qué es riesgoso:** es una feature de DSP nueva y puede dar falsos positivos sobre notas graves.

---

## 8. Hallazgos y bugs detectados

| ID | Hallazgo | Ítem |
|---|---|---|
| B-01 | El decay del IR del cab depende de la frecuencia de muestreo | T1-02 |
| B-02 | La línea muteada consume CPU y le suma su latencia a la línea activa | T2-02 |
| B-03 | ASIO está desactivado; el wizard siempre muestra "ASIO: Not detected" | T0-03 |
| B-04 | El wizard calcula la latencia como `buffer / sampleRate` (la subestima) | T0-04 |
| B-05 | El WASAPI de JUCE con dispositivos distintos no corrige la deriva de reloj → clicks periódicos | T2-09 |
| B-06 | CLAUDE.md dice oversampling 4x; el código usa 8x | T0-15 |
| B-07 | Los `.vcxproj` versionados tenían rutas de módulos JUCE viejas (6 niveles `..`, apuntaban a una carpeta inexistente) y les faltaban 28 headers que sí estaban en `NOVA.jucer`. El build funcionaba porque esos archivos están `ExcludedFromBuild` o son headers | Corregido por el re-export de T0-03 |
| B-08 | El Octave costaba 43.5% de CPU (pico 70%) por su detector de pitch: con buffers chicos podía causar cortes por sí solo | T0-16 (−72%); más margen en T1-13 |
| B-09 | La suite de tests en Debug está al límite del stack de 1 MB: agregar 3 KB a `OctavePedal` provocó un stack overflow (`0xC00000FD`) en `AudioEngineTests.cpp`, que crea varios pedales como variables locales en una misma función | T1-14 |
| B-10 | `AudioSetupWizard` posicionaba los controles de sus páginas en coordenadas del wizard (no de la página): quedaban corridos y "GOT IT" podía quedar fuera de la vista. `StartWizard` repite el patrón | Corregido en `AudioSetupWizard` (T2-10 F1); `StartWizard` pendiente |
| B-11 | Tras reconstruir el grafo, los 6 bloques de arranque devuelven la **entrada sin procesar** (no silencio): DI crudo, sin OutputChain y sin alinear (`AudioEngine.cpp:573-580`). Pasa con el bypass de pedales con latencia, el cambio de preset, el Aplicar/Deshacer del Autopilot y el futuro cambio de modo Live. **Confirmado con un test en T2-01 F0:** 6 bloques crudos, +19 dB con el master a −20 dB. **Corregido (2026-10-12):** fade-out del grafo saliente, silencio con el grafo nuevo alimentándose y fade-in; 0 bloques crudos y sin click (T2-01 §11.2) | Hecho; el hueco restante es T2-15 |
| B-12 | Con `OUTPUT_MIX = 0`, el endpoint Dry devuelve la entrada sin retraso, pero al host se le reporta la latencia del grafo: en un DAW esa pista suena 2 ms adelantada (`AudioEngine.cpp:639-644`). Baja prioridad | T2-01 (Live lo reduce a ~0) |
| B-13 | El estimador de true peak del limiter (Catmull-Rom de 4 puntos) subestima los picos entre muestras con contenido agudo: +0.8 dBTP con un tono a fs/4. Con material post-cab queda ≤ 0 dBTP. **Medido en C++ (T2-01 F0):** high-gain sin cab, +0.86 dBTP en Studio y +1.42 en Live | Anotado en T2-01 §3.5; sin ítem todavía |
| B-14 | `LookaheadDelay::process()` del Noise Gate trata un retardo de 0 como un retardo de buffer completo. No afecta a Studio (72 muestras) ni a Live G1 (12) | Solo si algún día se elige G3 (T2-01) |
| B-15 | El test "AudioEngine mixed-zone insertion…" fallaba de forma intermitente en Release (también en `HEAD`): no esperaba la carga en segundo plano del IR del Cabinet | Corregido en T2-01 F0 (espera de 500 ms) |
| B-16 | El IR sintético se normalizaba por pico: a 96 kHz la misma respuesta se reparte en el doble de muestras, el pico baja a la mitad y el cab sonaba **~6 dB más fuerte** (a 44.1 kHz, ~0.6 dB más bajo) | Corregido con T1-02 |

## 9. Límites físicos (no se pueden optimizar por software)

- **Bluetooth clásico (A2DP):** ~150–300 ms. LE Audio lo reduce, pero sigue lejos del cable.
- **Filtros AD/DA del hardware:** solo se acortan subiendo la frecuencia de muestreo (T2-11).
- **Aire:** 2.9 ms por metro entre altavoz y oído.
- **Causalidad:** ningún software puede procesar una nota antes de que llegue.

## 10. Escenarios objetivo

| Escenario | Hoy | Tras T0 | Tras T0 + T1 | Tras T2 |
|---|---|---|---|---|
| Interfaz con ASIO + audífonos en la interfaz | ~28–38 ms | ~8.5 ms (ASIO, buffer 64) | ~7 ms (buffer 32 viable en más equipos) | ~3–3.5 ms |
| Interfaz + audífonos de cable en otra salida | ~30–50 ms + clicks | igual | igual | ~8–12 ms sin clicks |
| Salida Bluetooth | ~150–300 ms | igual | igual | igual |

## 11. Referencias

- Microsoft (2024): driver USB Audio Class 2 con ASIO incluido en Windows — https://devblogs.microsoft.com/windows-music-dev/making-music-on-windows/
- Steinberg: dropouts en CPUs Intel híbridas — https://www.steinberg.help/r/windows-dropouts-on-intel-hybrid-architecture-cpus-with-cubase/nuendo-running-in-background
- Latencia medida de AirPods (referencia de Bluetooth) — https://stephencoyle.net/airpods
- ADAA: Parker, Zavalishin, Le Bivic, *Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution*, DAFx-16 (2016).
- ADAA: Bilbao, Esqueda, Parker, Välimäki, *Antiderivative Antialiasing for Memoryless Nonlinearities*, IEEE Signal Processing Letters 24(7) (2017).
- Filtros TPT/SVF: Zavalishin, *The Art of VA Filter Design*.

## 11b. Checklist del motor para la beta (decidido 2026-10-12)

**Decisión del usuario:** primero llevar el motor a un estado estable y de calidad para una beta con usuarios reales. Las mejoras (modo Live, CPU, T2) siguen después. Cuando se cumpla todo, el motor queda **congelado**: solo entran arreglos de bugs reportados en la beta, y las optimizaciones siguen en otra rama, protegidas por los checksums bit-exactos.

| # | Criterio | Tipo | Estado |
|---|---|---|---|
| E1 | El cab suena igual a 44.1, 48 y 96 kHz (T1-02 + B-16) | Código | **Hecho (2026-10-12)** |
| E2 | Formato de presets congelado: revisar `STATE_SCHEMA_VERSION`, la migración de versiones viejas y el round-trip guardar → cargar | Revisión + tests | Pendiente |
| E3 | Robustez de dispositivo: desconexión y reconexión en caliente, 44.1/48/96 kHz, buffers de 32 a 1024, ASIO, WASAPI y equipos sin ASIO | Tests + prueba manual | Pendiente |
| E4 | Entrada y salida en dispositivos distintos (B-05): el wizard lo detecta y lo explica | Código (wizard) | Pendiente |
| E5 | VST3 en 2–3 DAWs (procedimiento P13A) | Prueba manual | Pendiente |
| E6 | Reporte de problemas: exportar el log de sesión y el diagnóstico desde la app | Código | Pendiente |
| E7 | Cierre: prueba de B-11 en la app, rt-profile con la máquina en reposo, merge a `master` | Prueba + merge | Pendiente |
| L1 | Acuerdo ASIO de Steinberg firmado por las dos partes (T0-03) | Legal (usuario, en paralelo) | Pendiente |

**Criterio de congelamiento:** E1–E7 cerrados; base, golden y bit-exacto en PASS en Release; rt-profile limpio; matriz de hardware y DAWs sin crashes ni cortes con buffer ≥ 128. L1 es requisito para distribuir.

## 12. Siguiente paso recomendado

**Dónde retomar (2026-10-12):** todo commiteado en la rama `perf/t0-latency-optimizations` (sin mergear a `master`): T0, T2-10 F1 + F2 (probadas por el usuario en la app, caja negra OK), T2-01 F0 y el arreglo de B-11.

**Prioridad actual: checklist del motor para la beta (§11b).** E1 hecho; sigue E2 (formato de presets).

1. **Prueba rápida en la app del arreglo de B-11:** agregar y quitar pedales, cambiar de preset y usar Aplicar/Deshacer del Autopilot tocando. Debe oírse un hueco corto y limpio, sin DI crudo ni clicks.
2. **Después del congelamiento — T2-01 F1 + F2 solo en el Standalone:** `AudioEngine::setLatencyMode` + `GraphBuilder` conectan los flags de F0; ajuste en el `PropertySet` del Standalone; interruptor "Live / Studio"; Studio por defecto. Después, el A/B del usuario.
3. **Después, a elegir:**
   - T2-10 F3: automático en la primera ejecución, re-detección al conectar o desconectar, monitoreo con sugerencias y memoria por equipo.
   - T1 de CPU: T1-03, T1-04, T1-13 y T1-01.
4. **Antes de publicar:** firmar el acuerdo ASIO (T0-03) y mergear la rama a `master`.

## 13. Registro

| Fecha | Cambio |
|---|---|
| 2026-10-06 | Documento creado a partir de la sesión de exploración |
| 2026-10-07 | T0-03: licencia ASIO aclarada (camino propietario sin costo; firmar antes de publicar). VST3 SDK 3.8 es MIT |
| 2026-10-07 | T0-03: checklist de producción según el acuerdo ASIO 2.0.5 (contrafirma, avisos de marca, persona física vs. empresa) |
| 2026-10-07 | T0-03 implementado para desarrollo: `JUCE_ASIO="1"` en `NOVA.jucer` + re-export con `Projucer --resave`. El re-export también corrigió B-07. Drivers ASIO detectados en la máquina de desarrollo: Yamaha Steinberg USB ASIO, ASIO4ALL v2, Ableton Push, Ableton Move |
| 2026-10-07 | T0-01, T0-02 y T0-04…T0-16 implementados. 48/48 bit-idénticos (Debug y Release), base validation y golden PASS. Octave −72% de CPU. Nuevos hallazgos B-08 y B-09; nuevos ítems T1-13 y T1-14 |
| 2026-10-09 | T0 commiteado en la rama `perf/t0-latency-optimizations`. Probado en la app: ASIO con latencia "casi cero" y tocando sobre canciones sin problemas. Investigación de T2-10 terminada (`docs/reports/LAT-T2-10-latency-autopilot.md`) |
| 2026-10-09 | Decisiones de T2-10 tomadas. T2-10 F1 implementada: `AudioAutopilot.h` (motor puro + 13 tests), `StandaloneAudioAutopilot.h` (aplicar/deshacer con reversión automática + política de mute) y tarjeta "Recommended setup" en el wizard. Base PASS (324/13597/0), golden PASS, bit-exacto 48/48 (Debug y Release). Nuevo hallazgo B-10 (layout de páginas del wizard), corregido en `AudioSetupWizard` |
| 2026-10-09 | T2-10 F2 implementada: calibración del buffer (escalera desde 32 con la salida en silencio; criterios: 0 xruns, 90% de los callbacks, carga pico < 70% y gap < 2×). Ventana de medición en `CpuMeter`, flag de silencio en `processBlock`, CANCEL, UNDO y DRIVER PANEL. Base PASS (326/13617/0), golden PASS, bit-exacto 48/48 (Debug y Release). rt-profile Release 64/64 PASS. La ventana de `CpuMeter` queda dentro del ruido: los escenarios de motor (que la usan) +2.9% de mediana, frente a +9.6% de los `proc_*`, que no pasan por `CpuMeter` (ruido de la máquina). Evidencia: `artifacts/rt-profile-release-t2-10-f2.json` |
| 2026-10-11 | Investigación de T2-01 terminada (`docs/reports/LAT-T2-01-live-mode.md`). Latencia por nodo: código + harness VST3 (88/96/192 muestras de OutputChain; +4 por procesador con oversampling, 4.076 reales). Simulación offline del limiter (7 alternativas) y de los gates. Recomendación: `processWithoutLookahead` + gate con lookahead de 0.25 ms → −3.25 ms en la cadena típica, con las garantías de P12D intactas. Nuevos hallazgos B-11, B-12 y B-13. Sin cambios en `Source/` |
| 2026-10-12 | T2-10 F1 + F2, documentos de T2-01 y reportes de QA commiteados por tema. El usuario probó T2-10 en la app (caja negra OK). Decisiones de T2-01 tomadas (§11 del documento) |
| 2026-10-12 | T2-01 F0: flags inertes `setLiveMode()` (OutputChain, Noise Gate), checksums de OutputChain/InputChain (+2 `@live`) y suite C++ de medición. Live cumple C2–C6; B-11 confirmado; nuevos hallazgos B-14 y B-15 (B-15 corregido). Bit-exacto 48/48 previos sin cambios, base PASS (333/13744/0) en Debug y Release, golden PASS |
| 2026-10-12 | B-11 corregido en `AudioEngine::process`: fade-out de 5 ms con el grafo saliente (máx. 4 bloques), silencio con el grafo nuevo alimentándose y fade-in de 5 ms. Test "B-11 graph transition" (5 escenarios): 0 bloques crudos y sin click. Base PASS (333/13767/0) en Debug y Release, golden PASS, bit-exacto 56/56 |
| 2026-10-12 | Checklist del motor para la beta (§11b). E1 / T1-02 hecho: decay del IR escalado con la frecuencia + nivel igualado al de 48 kHz (B-16: el cab sonaba ~6 dB más fuerte a 96 kHz). Test nuevo: los 3 cabs a ≤ 0.52 dB de 48 kHz. Bit-exacto: solo cambian las 3 entradas de cab a 44.1 kHz (baselines actualizados a propósito). Base PASS (334/13773/0) en Debug y Release, golden PASS |
