# E3 — Robustez de dispositivo para la beta

- **Fecha:** 2026-10-13
- **Estado:** parte automatizada **hecha**, incluida la vuelta automática a la interfaz (sección 2.1). Falta la prueba manual con hardware real (sección 4).
- **Roadmap:** `docs/latency-optimization-roadmap.md` → §11b, criterio E3.

## 1. Bug encontrado y corregido: las perillas se reiniciaban al reabrir el dispositivo

Cada vez que el Standalone reabre el dispositivo, llama a `prepareToPlay`, y `AudioEngine::prepare()` reconstruía todos los procesadores **sin copiar su estado**. Los pedales seguían en la cadena y el bypass se conservaba, pero **todas las perillas volvían a su valor por defecto**.

**Cuándo pasaba:**
- al cambiar el buffer o la frecuencia de muestreo;
- al cambiar de interfaz;
- al usar Aplicar o Deshacer del Autopilot y la calibración de buffer;
- al desconectar la interfaz (JUCE abre otro dispositivo).

**Medido antes del arreglo:** 40 de 40 parámetros del rig de prueba se perdían en cada re-preparación.

**Arreglo:** `prepare()` reconstruye el grafo copiando el estado vivo de cada pedal, el mismo mecanismo que ya usaba el bypass que cambia la latencia. Ahora: 0 parámetros perdidos.

## 2. Qué hace JUCE al desconectar un dispositivo

Fuente: `AudioDeviceManager::audioDeviceListChanged`, JUCE 8.0.12.

| Evento | Comportamiento |
|---|---|
| Se desconecta el dispositivo en uso | JUCE lo cierra y abre el dispositivo por defecto (en una laptop, normalmente el micrófono y los parlantes integrados). NOVA se re-prepara y conserva los ajustes (gracias al arreglo de la sección 1) |
| Riesgo de acople con el micrófono integrado | Lo cubre la política de mute del Autopilot (T2-10 F1): escucha los cambios de dispositivo y vuelve a silenciar la entrada si la ruta nueva tiene riesgo de acople. Además, tras cada re-preparación el motor saca unos bloques de silencio con fade-in (arreglo de B-11), lo que cubre el instante del cambio |
| Se vuelve a conectar la interfaz | JUCE no vuelve a ella por su cuenta. **NOVA sí, desde 2026-10-13** (sección 2.1) |

### 2.1 Vuelta automática a la interfaz (agregado el 2026-10-13)

El Autopilot recuerda el dispositivo elegido (driver, entrada, salida, frecuencia y buffer) y sigue los cambios de dispositivo:

| Situación | Qué hace NOVA |
|---|---|
| Se desconecta el dispositivo elegido | Avisa: *"‹dispositivo› disconnected. NOVA switches back to it as soon as you reconnect it."* La entrada queda protegida por la política de mute |
| Vuelve a aparecer | Lo reabre con la frecuencia y el buffer de antes, y avisa: *"‹dispositivo› reconnected. Your audio setup is back."* La política de mute decide de nuevo según la ruta |
| Mientras espera, el usuario elige otro dispositivo | Ese pasa a ser el elegido; no vuelve al anterior |
| Cambios deliberados (Audio Setup, Aplicar/Deshacer, calibración) | El dispositivo nuevo pasa a ser el elegido |
| Aparece pero no abre (driver ocupado o todavía iniciando) | Hasta 3 intentos; después avisa que hay que elegirlo en Audio Setup. Sin bucles |

- **Dónde:** la lógica de decisión es pura y tiene tests (`AudioAutopilot::updateReconnect`, `isRouteAvailable`; test "Reconnect: switch back to the chosen device…"). La integración con JUCE está en `StandaloneAudioAutopilot::updateReconnect` / `switchBackToPreferred`.
- **Solo en el Standalone:** en el VST3 el DAW maneja el dispositivo.
- **Límite con ASIO:** muchos drivers ASIO siguen apareciendo en la lista aunque la interfaz esté desconectada. Ahí JUCE no cambia de dispositivo; el driver deja de entregar audio y, al reconectar, suele pedir un reset que JUCE atiende reabriendo. Depende del fabricante: hay que comprobarlo en la prueba M3/M4.

## 3. Cobertura automática (suite `E3 device robustness`)

| Test | Qué cubre | Resultado |
|---|---|---|
| Re-preparar conserva los ajustes | Rig típico (Gate, Overdrive, High Gain Amp, Delay en bypass, Cabinet) con perillas no default, re-preparado en 96k/512 → 44.1k/32 → 48k/1024 → 88.2k/256 → 192k/128 → 48k/64 | 0 parámetros perdidos, bypass conservado, latencia correcta en cada paso |
| Matriz de frecuencias × buffers | 44.1 / 48 / 88.2 / 96 / 192 kHz × 16 a 2048 muestras (40 combinaciones) con el rig típico | 40/40: salida finita, bajo el techo, no silenciosa y con la latencia esperada (162 / 176 / 316 / 344 / 584 muestras) |
| Bloques irregulares del host | Preparado a 512, recibe bloques de 1, 7, 100, 333, 0 (vacío), 513, 1024 y 4096 (estos tres rompen el contrato del host) | Salida finita y bajo el techo; sin crash |
| Desconectar y reconectar | `NOVAAudioProcessor`: release → re-prepare a 44.1 kHz / 480 (buffer no potencia de 2) | Ajustes y topología conservados; la latencia reportada al host sigue a la nueva frecuencia |

**Validación:** base PASS (341 resultados, 14150 checks, 0 fallos) en Debug y Release; bit-exacto 56/56; golden PASS.

## 4. Prueba manual (con tu hardware)

Usa un preset con perillas movidas (no default): así se nota si algo se reinicia. En cada paso se espera:
- ningún ruido fuerte ni click;
- ningún acople;
- que el sonido y las perillas sigan iguales.

| # | Prueba | Qué esperar |
|---|---|---|
| M1 | Con ASIO, tocando, cambia el buffer en Audio Setup: 32 → 64 → 128 → 256 → 512 | Un hueco corto y limpio en cada cambio; las perillas no cambian |
| M2 | Cambia la frecuencia de muestreo (44.1 → 48 → 96 kHz, si tu interfaz lo permite) | Igual que M1. El cab suena igual en todas (arreglo E1) |
| M3 | Desconecta el cable USB de la interfaz mientras tocas (con WASAPI y también con ASIO) | NOVA no se cuelga. Con WASAPI pasa a otro dispositivo, con la entrada silenciada y el aviso "disconnected". Sin acople. Con ASIO puede quedar en silencio en el mismo driver |
| M4 | Vuelve a conectar la interfaz **sin tocar nada** | NOVA vuelve sola a la interfaz en unos segundos, con el aviso "reconnected", y suena con los mismos ajustes (frecuencia, buffer, perillas). Si no vuelve, anota el driver |
| M4b | Desconecta, elige a mano otro dispositivo y reconecta la interfaz | NOVA se queda en el dispositivo que elegiste (no vuelve a la interfaz) |
| M5 | Usa WASAPI (Windows Audio) en lugar de ASIO; prueba shared y, si aparece, Low Latency o exclusive | Suena sin cortes con buffer ≥ 128; el wizard muestra la latencia real |
| M6 | Sin interfaz: entrada y salida de la laptop o de una tarjeta integrada | La entrada empieza silenciada (riesgo de acople) y el aviso lo explica |
| M7 | Salida por audífonos Bluetooth, con la entrada en la interfaz | Funciona, con la latencia alta propia de Bluetooth. Si el audio de los audífonos baja a calidad "telefónica", Windows abrió su micrófono (modo manos libres): anótalo, porque habría que evitar ese dispositivo como entrada |
| M8 | Aplicar y Deshacer del Autopilot, y CALIBRATE BUFFER, con perillas movidas | Las perillas no cambian (antes se reiniciaban) |
| M9 | Suspende la laptop con NOVA abierto y vuelve | NOVA sigue funcionando o se recupera al reabrir el dispositivo; sin crash |

Si algo falla, anota el paso, el driver, la frecuencia y el buffer, y guarda `%APPDATA%/NOVA/Logs/session-log.txt`.

## 5. Decisiones

- **Volver sola a la interfaz al reconectarla:** el usuario pidió incluirla antes de la beta (2026-10-13). Hecho (sección 2.1). Validación: base PASS (342 resultados, 14174 checks, 0 fallos) en Debug y Release, bit-exacto 56/56. Smoke en el equipo de desarrollo: al abrir, el log registra `autopilot.reconnect chosen device=...`. La prueba de desconexión real es M3/M4.
