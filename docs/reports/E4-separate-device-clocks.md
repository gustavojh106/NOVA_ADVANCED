# E4 — Entrada y salida en dispositivos distintos (B-05)

**Fecha:** 2026-10-13 · **Rama:** `perf/t0-latency-optimizations` · **Checklist:** `docs/latency-optimization-roadmap.md` §11b

## 1. Problema

Con WASAPI o DirectSound, JUCE puede grabar de un dispositivo y reproducir por otro, pero no corrige la deriva entre los dos relojes: cada tanto descarta o rellena un paquete y se oye un click. Arreglarlo de verdad (puente de relojes con resampler adaptativo) es **T2-09** y queda fuera de E4. **E4 solo detecta la ruta y explica cómo evitarla.**

Caso real del usuario: entrada `Line (Steinberg UR12 )` y salida `Speakers (BlackShark V3 X PS)` en Windows Audio (Shared).

## 2. Regla de detección

`AudioAutopilot::usesSeparateClocks()` (lógica pura, sin APIs de audio):

| Ruta | ¿Avisa? | Por qué |
|---|---|---|
| WASAPI (Shared, Exclusive, Low Latency) o DirectSound, con entrada y salida en aparatos distintos | **Sí** | Dos relojes, nadie corrige la deriva |
| Dos endpoints del mismo aparato: `Line (Steinberg UR12 )` + `Speakers (Steinberg UR12 )` | No | Comparten reloj. Se compara el nombre del aparato entre paréntesis, sin mayúsculas, sin espacios sobrantes y sin el prefijo `2- ` de Windows |
| ASIO (nativo o ASIO4ALL) | No | ASIO abre un solo dispositivo |
| Sin entrada, entrada sin canales activos o dispositivo cerrado | No | No hay nada que pueda derivar |
| `Primary Sound Driver` / `Primary Sound Capture Driver` (DirectSound) | No | Siguen al dispositivo por defecto de Windows: no se sabe cuál es |
| Nombres sin paréntesis (`USB Audio CODEC`) | Según el nombre | El nombre entero es el aparato |

**Límite conocido:** dos unidades idénticas del mismo modelo (`Steinberg UR12` y `2- Steinberg UR12`, las dos activas) cuentan como un aparato. Es lo más raro y se prefiere no dar falsos positivos.

## 3. Qué ve el usuario

- **Indicador del header:** se pone ámbar (antes podía quedar verde: Shared con buffer chico da < 25 ms).
- **Panel de Audio:** la línea de avisos, en ámbar, explica el problema y qué hacer. Tiene prioridad sobre el aviso de modo compartido (los clicks importan más que la latencia) y crece a 3–4 líneas si hace falta.
- **Sin banner** (decisión del usuario): el indicador y el panel alcanzan.
- **Log de sesión:** `autopilot.clock separate clocks route=...` al arrancar o al cambiar a una ruta así (sirve para los reportes de E6).

El texto (`AudioAutopilot::adviseClocks()`) empieza igual en todos los casos:

> Input and output are on different devices (Steinberg UR12, BlackShark V3 X PS). Their clocks drift apart, so expect a click every so often.

y termina según lo que se pueda hacer:

| Caso | Final del aviso |
|---|---|
| La recomendación del Autopilot usa un solo aparato (ASIO de la interfaz, o entrada y salida de la interfaz) | "Plug your headphones into the Steinberg UR12 and press Apply above." Usa el **Apply** que ya está en la recomendación, sin un botón nuevo |
| La recomendación no lo arregla, pero la interfaz tiene salida | "Plug your headphones into the Steinberg UR12 and pick it as the Output device." |
| La interfaz no tiene salida | "For click-free audio, record and play through one device, such as an interface with a headphone output." |

Los nombres largos se acortan a 28 caracteres para que el aviso entre en el panel.

## 4. ¿La recomendación ya prefiere un solo aparato?

Sí: el par entrada + salida del mismo aparato suma **+100** en `pairScore`, y ASIO (un solo dispositivo) está en el tier más alto. En el equipo del usuario recomienda el ASIO de Yamaha Steinberg.

**Hallazgo B-17 (corregido en E4):** si la interfaz de la guitarra no tenía salida ni ASIO, el par "micrófono interno + parlantes internos" (0 + 0 + 100) le ganaba al par "interfaz + audífonos USB" (40 + 30): el Autopilot recomendaba el micrófono de la laptop, sin guitarra. Ahora la entrada interna resta 100 y la webcam 140, así que una entrada externa siempre gana. Con solo la laptop, su micrófono sigue siendo la única opción.

## 5. Tests (`AudioAutopilotTests`)

- **"E4 input and output on different devices are flagged and explained (B-05)":** mismo aparato con nombres distintos, prefijo `2- `, mayúsculas y espacios; aparatos distintos en los 4 drivers; nombres sin paréntesis; ASIO; sin entrada, sin canales y cerrado; alias `Primary Sound`; los 3 finales del aviso (con el equipo del usuario y su ASIO); sin aviso con un aparato o con ASIO; nombres largos acortados.
- **"AudioAutopilot never trades the guitar input for a built-in mic on the same device":** B-17. Falla con el puntaje anterior.

**Validación:** base PASS (344 resultados, 14216 checks, 0 fallos) en Debug y Release; bit-exacto 56/56 en Debug y Release; golden Release PASS; policy del audio thread PASS. **UI:** capturas con el equipo real del usuario (UR12 → BlackShark, Shared, 44.1 kHz, 160 muestras): indicador ámbar con 9.5 ms (sin E4 habría quedado verde) y el aviso en 2 líneas, con "press Apply above" junto a la recomendación ASIO, a 1920×1080 y a 1280×760.

## 6. Prueba manual (con tu equipo)

| # | Prueba | Qué esperar |
|---|---|---|
| E4-M1 | Windows Audio, entrada en la UR12 y salida en los BlackShark | Indicador ámbar. En el panel: el aviso de dispositivos distintos con "press Apply above" y la recomendación ASIO con Apply |
| E4-M2 | Conecta los audífonos a la UR12 y pulsa Apply | Pasa a ASIO, sin aviso; el indicador vuelve a verde si la latencia es baja |
| E4-M3 | Windows Audio con entrada y salida en la UR12 (`Line` + `Speakers`) | Sin aviso de dispositivos distintos (solo el de modo compartido) |

## 7. Fuera de alcance

- **T2-09:** puente de relojes (FIFO + resampler adaptativo) para que dos dispositivos suenen sin clicks.
- Detectar dos unidades idénticas del mismo modelo (necesita propiedades MMDevice, fase 4 de T2-10).
