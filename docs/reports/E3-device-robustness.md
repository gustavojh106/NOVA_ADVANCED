# E3 — Robustez de dispositivo para la beta

- **Fecha:** 2026-10-13
- **Estado:** parte automatizada **hecha**. Falta la prueba manual con hardware real (sección 4).
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
| Se vuelve a conectar la interfaz | **JUCE no vuelve a ella sola:** el usuario tiene que elegirla otra vez en Audio Setup. Volver solo es parte de T2-10 F3 (re-detección automática) |

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
| M3 | Desconecta el cable USB de la interfaz mientras tocas | NOVA no se cuelga. Pasa a otro dispositivo con la entrada silenciada y un aviso. Sin acople |
| M4 | Vuelve a conectar la interfaz y elígela otra vez en Audio Setup | Vuelve a sonar con los mismos ajustes |
| M5 | Usa WASAPI (Windows Audio) en lugar de ASIO; prueba shared y, si aparece, Low Latency o exclusive | Suena sin cortes con buffer ≥ 128; el wizard muestra la latencia real |
| M6 | Sin interfaz: entrada y salida de la laptop o de una tarjeta integrada | La entrada empieza silenciada (riesgo de acople) y el aviso lo explica |
| M7 | Salida por audífonos Bluetooth, con la entrada en la interfaz | Funciona, con la latencia alta propia de Bluetooth. Si el audio de los audífonos baja a calidad "telefónica", Windows abrió su micrófono (modo manos libres): anótalo, porque habría que evitar ese dispositivo como entrada |
| M8 | Aplicar y Deshacer del Autopilot, y CALIBRATE BUFFER, con perillas movidas | Las perillas no cambian (antes se reiniciaban) |
| M9 | Suspende la laptop con NOVA abierto y vuelve | NOVA sigue funcionando o se recupera al reabrir el dispositivo; sin crash |

Si algo falla, anota el paso, el driver, la frecuencia y el buffer, y guarda `%APPDATA%/NOVA/Logs/session-log.txt`.

## 5. Pendiente / decisiones

- **Volver sola a la interfaz al reconectarla** (T2-10 F3): hoy hay que elegirla a mano. Es una mejora de comodidad, no de seguridad. Se puede hacer antes de la beta (alcance chico: escuchar la lista de dispositivos y reabrir el dispositivo preferido cuando reaparece) o dejarla para después.
