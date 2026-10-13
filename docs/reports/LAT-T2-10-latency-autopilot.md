# LAT-T2-10 — Latency Autopilot: investigación

- **Fecha:** 2026-10-09
- **Estado:** Fases 1 y 2 implementadas (2026-10-09; secciones 10 y 11). Siguiente: Fase 3 (automático en la primera ejecución, re-detección y monitoreo).
- **Roadmap:** `docs/latency-optimization-roadmap.md` → T2-10
- **Alcance:** solo el Standalone. En el VST3 el DAW controla el dispositivo de audio.

## 1. Objetivo y criterio de éxito

Que el usuario conecte su equipo, abra NOVA y quede sonando con la mejor configuración posible para su hardware, sin abrir el panel del driver ni entender qué es un buffer.

**Criterio de éxito (Fase 3 completa):**
- Máquina limpia (sin configuración guardada) + interfaz USB con ASIO del fabricante → NOVA termina en ASIO, 48 kHz, el buffer estable más chico y la entrada sin mutear. Como máximo un clic y ~20 s.
- Sin ASIO → WASAPI Low Latency o Exclusive, nunca Shared si hay una alternativa que funcione.
- Salida Bluetooth → funciona y avisa de la latencia; no promete lo que no puede dar.
- Ningún cambio automático deja al usuario sin audio: si el dispositivo nuevo no abre o no entrega callbacks en 2 s, se revierte solo.

## 2. Cómo funciona hoy (verificado en el código)

| Tema | Comportamiento actual | Dónde |
|---|---|---|
| Arranque del dispositivo | `StandalonePluginHolder::reloadAudioDeviceState()` restaura el XML guardado en la clave `audioSetup`. Sin estado guardado, usa el **primer tipo de la lista** (WASAPI Shared, "Windows Audio") con el dispositivo por defecto de Windows | `juce_StandaloneFilterWindow.h:333-363` |
| Orden de tipos | WASAPI Shared → Exclusive → Low Latency → DirectSound → **ASIO al final** | `juce_AudioDeviceManager.cpp:327-331` |
| Buffer al elegir ASIO | El **preferido del driver**, es decir, lo que el usuario dejó en el panel del fabricante. Si `createBuffers()` falla con el tamaño pedido, JUCE vuelve en silencio al preferido | `juce_ASIO_windows.cpp:480-491`, `:841-879` |
| Tamaños de buffer ASIO | Derivados de `getBufferSize(min, max, preferred, granularity)`. Si min == max, el driver solo acepta un tamaño y se cambia desde su propio panel (`showControlPanel()`) | `juce_ASIO_windows.cpp:396`, `:685`, `:830` |
| WASAPI Low Latency | Requiere `IAudioClient3`. Si el driver no lo soporta, **no hay fallback**: el dispositivo no abre | `juce_WASAPI_windows.cpp:663-685`, `:905-912` |
| Detección de cortes | `AudioDeviceManager::getXRunCount()` = xruns del driver (ASIO solo si soporta `kAsioCanReportOverload`; si no, −1) **+ callbacks que tardaron más que el buffer** (`AudioProcessLoadMeasurer`). Hay señal útil en cualquier driver | `juce_AudioDeviceManager.cpp:1389-1393`, `juce_ASIO_windows.cpp:455-456`, `:1375` |
| Carga de CPU | `AudioDeviceManager::getCpuUsage()` y el `CpuMeter` propio de NOVA (promedio y pico por bloque) | `juce_AudioDeviceManager.h:325`, `Source/Core/Audio/CpuMeter.h` |
| Conectar/desconectar | `AudioIODeviceType::Listener::audioDeviceListChanged()` y el `ChangeBroadcaster` del `AudioDeviceManager` | `juce_AudioIODeviceType.h:137-143` |
| **Input muteado por defecto** | El Standalone de JUCE arranca con `shouldMuteInput = true` (protección contra feedback) y NOVA no lo maneja. **Un usuario nuevo puede abrir NOVA y no oír su guitarra** hasta encontrar el control | `juce_StandaloneFilterWindow.h:344`; sin referencias en `Source/` |
| Latencia mostrada | Ya es round trip real (driver + NOVA) desde T0-04 | `Source/GUI/Wizards/AudioSetupWizard.h` |

## 3. Lo que JUCE ofrece y lo que falta

| Necesidad | JUCE 8.0.12 | Falta construir |
|---|---|---|
| Listar drivers y dispositivos | `getAvailableDeviceTypes()`, `getDeviceNames()` | — |
| Probar una configuración | `setAudioDeviceSetup()` + leer de vuelta `getCurrentBufferSizeSamples()` / `getCurrentSampleRate()` | Verificar que lleguen callbacks y revertir si no |
| Detectar cortes | `getXRunCount()` (siempre útil, por la parte de carga) | Medir también el jitter entre callbacks (cortes por DPC que no son de CPU) |
| ¿El ASIO es nativo o un wrapper? | Solo el nombre del driver | Lista de wrappers conocidos (ASIO4ALL, FlexASIO, ASIO2WASAPI, "Generic Low Latency ASIO Driver") |
| ¿El driver ASIO es multi-cliente? | Nada | Prueba: con ASIO activo, intentar abrir el endpoint WASAPI del mismo equipo en shared. `AUDCLNT_E_DEVICE_IN_USE` = no es multi-cliente. Requiere código nativo de Windows |
| ¿Bluetooth, USB o integrado? | Nada | Propiedades MMDevice: `PKEY_Device_EnumeratorName` (`BTHENUM`, `USB`, `HDAUDIO`…) y `PKEY_AudioEndpoint_FormFactor` (auriculares, parlantes, micrófono…). Helper COM solo para Windows |
| Relacionar driver ASIO ↔ endpoint WASAPI del mismo equipo | Nada | Heurística por tokens de fabricante en los nombres ("Steinberg", "Focusrite", "Scarlett", "UMC"…) |

## 4. Diseño propuesto

### 4.1 Componentes

| Componente | Responsabilidad | Hilo |
|---|---|---|
| `Source/Core/Audio/AudioAutopilot.h` (nuevo) | **Lógica pura:** recibe un inventario y un perfil, devuelve una lista ordenada de candidatos; evalúa los resultados de una prueba de estabilidad. Sin JUCE device APIs, testeable con unit tests | — |
| `Source/Core/Platform/WindowsAudioEndpointInfo.h/.cpp` (nuevo) | Bus (USB/Bluetooth/HDA), form factor y prueba multi-cliente vía COM | Message thread |
| Integración en el Standalone | Inventario real, aplicar o revertir, prueba de estabilidad con timer, escuchar conexiones | Message thread |
| UI | Paso "Configuración automática" en `AudioSetupWizard` + botón en Ajustes + avisos | Message thread |

El audio thread no se toca: solo se leen contadores que ya existen (`getXRunCount()`, `getCpuUsage()`, `CpuMeter`).

### 4.2 Inventario

Por cada par (tipo de driver, dispositivo):
- canales de entrada y salida;
- frecuencias y buffers disponibles;
- si es ASIO nativo o wrapper;
- bus (USB, Bluetooth, integrado) y form factor;
- si el dispositivo de entrada y el de salida son el mismo equipo (un solo reloj: siempre preferible).

### 4.3 Motor de decisión (perfil Estudio)

1. **Descartar:** dispositivos sin entrada o sin salida, y salidas Bluetooth si existe cualquier otra salida.
2. **Preferir** que entrada y salida sean el mismo equipo.
3. **Prioridad de driver:** ASIO nativo > WASAPI Exclusive > WASAPI Low Latency > ASIO wrapper > WASAPI Shared > DirectSound.
4. **Frecuencia:** 48 kHz si está disponible (la validada por las golden metrics); si no, 44.1 kHz; si no, la del dispositivo.
5. **Buffer:** lo decide la prueba de estabilidad (4.4).
6. **Probar en orden:** abrir el candidato; si no abre o no llegan callbacks en 2 s, pasar al siguiente. Esto cubre los drivers sin `IAudioClient3` y los ASIO que ignoran el buffer pedido.

### 4.4 Prueba de estabilidad del buffer (activa, ~10–20 s)

1. Recorrer los tamaños disponibles de menor a mayor, empezando en 32 (Estudio) o 64 (Canciones).
2. Por cada tamaño: aplicar, **leer el tamaño real** (el driver puede imponer otro), esperar 0.5 s y medir 4 s con la salida en silencio y el preset actual cargado.
3. **Pasa si:** cero xruns nuevos, carga pico < 70% del presupuesto del bloque e intervalo máximo entre callbacks < 2× el período (era 1.5×; ver 11), con al menos el 90% de los callbacks esperados.
4. Elegir el primer tamaño que pase. Si el driver tiene un buffer fijo (min == max), ofrecer "Abrir panel del driver".

### 4.5 Monitoreo pasivo (después de configurar)

- Si aparecen 2 o más xruns en 60 s: **sugerir** subir un nivel de buffer.
- Si pasan 10 minutos sin xruns, con carga pico < 25% y un buffer mayor al mínimo: **sugerir** bajar.
- Por defecto solo sugiere; cambiar el buffer reinicia el audio unos ~100 ms y no conviene hacerlo en medio de una canción.

### 4.6 Perfiles

| | Estudio / baja latencia (default) | Tocar con canciones |
|---|---|---|
| Objetivo | Mínima latencia de la guitarra | Que YouTube, Spotify o el DAW sigan sonando |
| Driver | Ranking de 4.3 | ASIO nativo **solo si es multi-cliente**; si no, WASAPI Low Latency. Nunca Exclusive ni ASIO4ALL |
| Frecuencia | 48 kHz | La que ya usa Windows en ese dispositivo (no molesta a las otras apps) |
| Buffer | El menor estable | Un nivel por encima del menor estable |

La latencia de la canción no importa (uno toca sobre lo que oye): el perfil Canciones cambia **compatibilidad**, no latencia.

### 4.7 Flujos

- **Primera ejecución** (sin configuración guardada): inventario → candidato → aplicar → prueba → aviso "Configurado: ASIO · 48 kHz · 64 muestras · ~5 ms" con botón **Deshacer**.
- **Al conectar o desconectar un equipo:** volver a inventariar. Si apareció un candidato mejor (p. ej., conectaste la interfaz), proponer cambiar. Si el dispositivo activo desapareció, usar el mejor disponible.
- **Desde Ajustes:** botón "Optimizar audio" y selector de perfil.
- **Input:** desmutear automáticamente si la entrada es una interfaz o la salida son auriculares. Mantener el mute solo si la entrada es el micrófono integrado y la salida son los parlantes integrados (riesgo de feedback), explicando por qué.

### 4.8 Persistencia

- JUCE sigue guardando la configuración activa en `audioSetup`.
- NOVA guarda en `Nova::SettingsService`, por firma de equipo (tipo de driver + nombres): perfil, frecuencia, buffer probado, fecha y resultado de la prueba. Al volver a conectar ese equipo, se restaura sin repetir la prueba.

## 5. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Un driver ASIO defectuoso se cuelga al abrirlo | Probar candidatos de a uno con timeout; recordar los que fallaron y no volver a probarlos |
| La prueba produce clicks en los auriculares | Salida en silencio durante la prueba |
| Cambiar la frecuencia del hardware desconfigura otras apps | Solo en Estudio; Canciones usa la frecuencia de Windows |
| Exclusive bloquea otras apps sin que el usuario entienda por qué | Solo en Estudio, con aviso; Canciones lo evita |
| El driver ignora el buffer pedido | Leer siempre el valor real; buffer fijo → "Abrir panel del driver" |
| Falsos xruns por cargar presets o GUI pesada | Medir con el preset actual; monitoreo pasivo con umbral (≥ 2 en 60 s) |
| Feedback al desmutear con micrófono y parlantes integrados | Política de mute de 4.7 |
| Código COM nativo de Windows | Aislado en un helper; si falla, el Autopilot sigue funcionando con heurísticas por nombre |

## 6. Plan de pruebas

- **Unit tests** del motor de decisión (sin hardware) con inventarios sintéticos: interfaz con ASIO, solo WASAPI, ASIO4ALL instalado, salida Bluetooth, buffer fijo, entrada y salida distintas, perfil Canciones con driver no multi-cliente.
- **Unit tests** del evaluador de la prueba de estabilidad con métricas sintéticas (xruns, carga y jitter en los bordes de los umbrales).
- **Matriz manual de hardware:** interfaz USB con ASIO (la de desarrollo); laptop sin interfaz (Realtek); ASIO4ALL instalado; auriculares Bluetooth; conectar y desconectar la interfaz con NOVA abierto; YouTube sonando en perfil Canciones.
- **Regresión:** base validation, golden metrics y checksum bit-exacto (el DSP no cambia).

## 7. Fases de implementación

| Fase | Contenido | Riesgo | Tamaño |
|---|---|---|---|
| **F1** | Inventario + motor de decisión + unit tests. En el wizard, "Configuración recomendada" con **Aplicar** y **Deshacer** (nada automático). Política de desmute | T1 | M |
| **F2** | Prueba de estabilidad del buffer iniciada por el usuario ("Calibrar") | T1/T2 | M |
| **F3** | Configuración automática en la primera ejecución, re-detección al conectar o desconectar, monitoreo pasivo con sugerencias | T2 | M |
| **F4** | Perfiles (Estudio / Canciones), helper COM (Bluetooth, USB, integrado) y prueba multi-cliente | T2 | L |

Cada fase se valida y se commitea por separado. Los archivos nuevos se agregan a `NOVA.jucer` y se re-exporta con `Projucer --resave`.

## 8. Decisiones del usuario (2026-10-09)

| # | Decisión | Respuesta |
|---|---|---|
| 1 | Primera ejecución: ¿automático con aviso y "Deshacer", o preguntar antes? | **Automático con aviso y Deshacer** (F3) |
| 2 | ¿Silenciar la salida 10–20 s durante la calibración del buffer? | **Sí** (F2) |
| 3 | ¿WASAPI Exclusive en el perfil Estudio? | **Sí** en Estudio, nunca en Canciones (aplicado en F1) |
| 4 | Monitoreo continuo: ¿solo sugerencias o ajuste automático? | **Solo sugerencias** (F3) |
| 5 | ¿Desmutear la entrada automáticamente cuando sea seguro? | **Sí** (aplicado en F1) |

## 9. Referencias en el código

- Estado del dispositivo en el Standalone: `juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h` (`reloadAudioDeviceState`, `saveAudioDeviceState`, `shouldMuteInput`)
- ASIO: `juce_audio_devices/native/juce_ASIO_windows.cpp` (`readBufferSizes`, `open`, `asioMessagesCallback`, `showControlPanel`)
- WASAPI: `juce_audio_devices/native/juce_WASAPI_windows.cpp` (`querySupportedBufferSizes`, `tryInitialisingWithBufferSize`)
- Cortes y carga: `juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp` (`getXRunCount`, `getCpuUsage`)
- NOVA: `Source/GUI/Wizards/AudioSetupWizard.h`, `Source/Core/Audio/CpuMeter.h`, `Source/Core/NovaSettingsService.h`

## 10. Fase 1 — implementación (2026-10-09)

**Riesgo:** T1. El DSP no cambia: checksums bit-exactos 48/48 en Debug y Release.

### Archivos

| Archivo | Contenido |
|---|---|
| `Source/Core/Audio/AudioAutopilot.h` (nuevo) | Lógica pura. Clasificadores por nombre (familia de driver; ASIO nativo, genérico o virtual; endpoint integrado, Bluetooth, webcam, HDMI, virtual, auriculares, parlantes o micrófono; "mismo equipo"), `rankCandidates()`, `recommend()` y `adviseInputMute()` |
| `Source/Core/Audio/StandaloneAudioAutopilot.h` (nuevo) | Message thread, solo Standalone. Inventario real, Aplicar con verificación y reversión automática, Deshacer, guardia de mute y banner de avisos (`AutopilotNoticeBanner`) |
| `Source/Core/AudioEngineTests.cpp` | `AudioAutopilotTests`: 13 tests (115 checks) sobre inventarios sintéticos |
| `Source/GUI/Wizards/AudioSetupWizard.h` | Paso Scan: tarjeta "RECOMMENDED SETUP" (APPLY / UNDO, con estado CHECKING / APPLIED / REVERTED / UNDONE) y fila "Input Monitor" con MUTE/UNMUTE |
| `Source/Core/PluginEditor.h/.cpp` | Crea el autopilot y el banner (solo Standalone) y se los pasa al wizard |
| `Source/Core/AudioEngine.h` | Getter `getAudioBlockCount()`: lee el contador atómico que ya existía (sin cambios en `.cpp` ni en el audio thread) |
| `NOVA.jucer` + `.vcxproj` | Los 2 headers nuevos (re-export con `Projucer --resave`; el diff solo agrega esos archivos) |

### Comportamiento

- **Inventario:** usa los nombres que JUCE ya escaneó. **Nunca abre un driver ASIO inactivo**; solo el dispositivo activo aporta frecuencias y buffers.
- **Hardware ASIO "presente":** los tokens de marca del driver aparecen en algún endpoint de Windows (p. ej., "Yamaha Steinberg USB ASIO" ↔ "Line (Steinberg UR22C)"). Sin coincidencia, el driver queda como "no verificado", salvo que ya esté corriendo.
- **Ranking (Estudio):** ASIO nativo verificado > WASAPI Exclusive > WASAPI Low Latency > ASIO nativo no verificado > ASIO genérico (ASIO4ALL, FlexASIO…) > WASAPI Shared > DirectSound. Los ASIO virtuales (Voicemeeter…) y los endpoints virtuales o de loopback (VB-Cable, Stereo Mix) se descartan.
- **Par de endpoints (WASAPI/DirectSound):** se prefiere el mismo equipo (+100), luego una entrada de línea externa y una salida cableada. Una salida Bluetooth solo se usa si no hay otra, y la recomendación explica la latencia. **Nunca** se usa un micrófono Bluetooth/manos libres.
- **Frecuencia y buffer:** se piden 48 kHz (JUCE cae a la más cercana disponible). Buffer: en ASIO, el del panel del driver; en Exclusive/Low Latency, 128 si existe; en Shared, el default. La calibración llega en F2.
- **Ya óptimo:** si el dispositivo activo es el mejor, la tarjeta muestra "ACTIVE". Si además admite 48 kHz y no está en 48 kHz, ofrece solo el cambio de frecuencia (conserva el buffer).
- **Aplicar:** guarda el estado actual (XML de JUCE) y abre **exactamente** el candidato vía `AudioDeviceManager::initialise(xml)`.
  - No usa `setCurrentAudioDeviceType()`, porque abre primero el dispositivo por defecto del tipo (en ASIO, un driver cualquiera).
  - Si cambia el tipo de driver, cierra el dispositivo y espera 1.5 s, como hace JUCE. Sin ese cierre, JUCE ignora el paso Shared → Exclusive cuando el nombre del endpoint es el mismo.
- **Verificación:** si el dispositivo no abre, o el contador de bloques del motor no avanza entre 0.5 s y 2 s, se restaura la configuración anterior (estado REVERTED) y el candidato se excluye por esta sesión: la tarjeta pasa al siguiente.
- **Deshacer:** restaura la configuración previa a la última aplicación. Sigue disponible al reabrir el wizard, porque el autopilot vive en el editor.

### Política de mute de la entrada (decisión 5)

| Ruta activa | Resultado |
|---|---|
| ASIO nativo | Desmutea |
| Salida de auriculares (no parlantes) | Desmutea |
| Entrada y salida en el mismo equipo externo | Desmutea |
| Entrada de línea externa (no "Microphone") | Desmutea |
| Micrófono integrado o webcam con parlantes/HDMI | **Mutea** (riesgo de feedback) |
| Micrófono Bluetooth | **Mutea** (baja el headset a calidad de llamada) |
| Entrada virtual o loopback | **Mutea** (puede realimentar la salida) |
| ASIO genérico, micrófono USB con parlantes, "Primary Sound Capture Driver" | Queda como está; si estaba muteada, sigue muteada |

- Se evalúa ~0.5 s después de abrir la app y cada vez que cambia la ruta (también tras Aplicar, Revertir o Deshacer). Cada cambio automático muestra un aviso con el botón inverso (MUTE o UNMUTE).
- **Se respeta al usuario:**
  - Un mute manual (desde el wizard, el aviso o el diálogo de JUCE) bloquea el desmute automático hasta que el usuario desmutee.
  - Si el usuario desmutea a mano en una ruta "riesgosa", esa ruta queda aprobada y no se vuelve a mutear sola.
  - Ambos estados se guardan en el `PropertySet` del Standalone (`novaInputMutePinnedByUser`, `novaInputUnmuteApprovedRoute`).
- **Desvío consciente de 4.7 (más estricto):** se desmutea solo con evidencia de que es seguro. Además, se vuelve a mutear al pasar a una ruta con riesgo de feedback, por ejemplo si se desconecta la interfaz y JUCE cae al micrófono y los parlantes del laptop.

### Hallazgo B-10 (corregido)

`AudioSetupWizard` posicionaba los controles de cada página con coordenadas del wizard en lugar de coordenadas de la página. Quedaban corridos hacia la derecha y abajo, y, según el cálculo del layout con el editor a 1920×1080, el botón "GOT IT" del paso Scan quedaba fuera de la página: invisible, sin poder avanzar. Ahora `placeOnPage()` traduce las coordenadas. `StartWizard` tiene el mismo patrón y queda pendiente.

### Validación

- Build Debug Standalone: 0 errores, 0 warnings.
- Base validation (Debug): **PASS. 324 resultados, 13597 checks, 0 fallos** (antes 311 / 13482; +13 tests del Autopilot).
- Golden metrics (Debug): PASS.
- Checksums bit-exactos: **48/48 idénticos en Debug y Release**.
- Policy del audio thread: 0 hallazgos en rutas de audio. Las guardas P9F/P9G fallan solo porque `AudioEngine.h` tiene cambios sin commitear; pasan al commitear.
- rt-profile: no aplica (sin cambios de DSP ni de CPU en el audio thread).

### Prueba manual (pendiente)

1. **Interfaz con ASIO ya activa:** abrir NOVA. No debería aparecer ningún aviso. En Audio Setup → Scan: la tarjeta dice "ACTIVE · You're already on the best setup" y la fila Input Monitor dice "On", con el botón MUTE.
2. **B-10:** recorrer los 6 pasos del wizard. "GOT IT", el selector de dispositivo, las tarjetas de uso, las del afinador y "APPLY ALL" deben verse completos y alineados con sus textos.
3. **Aplicar y deshacer:**
   - En Device, cambiar a "Windows Audio" con el micrófono y los parlantes del equipo. Esperado: aviso "Input muted…" con UNMUTE.
   - Volver a Scan: recomienda el ASIO de la interfaz. APPLY → "CHECKING..." (~2 s) → "APPLIED", y aviso "Input on…". Tocar.
   - UNDO → vuelve la configuración anterior.
4. **Mute manual respetado:** MUTE en la fila Input Monitor y reiniciar NOVA → sigue muteado, sin aviso. UNMUTE y reiniciar → activo.
5. **Desconexión en caliente:** desenchufar la interfaz con NOVA abierto. Si JUCE cae al micrófono y los parlantes del equipo, la entrada se mutea con aviso. Reconectar: la tarjeta vuelve a recomendar el ASIO.
6. **Sin interfaz** (laptop o interfaz desconectada): recomienda "Windows Audio (Exclusive) · 48 kHz · 128 samples". APPLY funciona. Mientras tanto, otras apps no suenan por ese dispositivo (lo explica la tarjeta).
7. **Opcional, reversión:** aplicar un candidato que no pueda abrirse (p. ej., un dispositivo tomado en exclusivo por otra app). Esperado: "REVERTED", vuelve la configuración previa y la tarjeta pasa al siguiente candidato.

### Pendiente

- La calibración del buffer quedó en la Fase 2 (sección 11).
- Guardar el resultado por firma de equipo (4.8) pasa a F3, que es la fase que lo usa al reconectar un equipo.

## 11. Fase 2 — calibración del buffer (2026-10-09)

**Riesgo:** T1. Toca el audio thread solo con contadores atómicos y un flag; el DSP no cambia (checksums bit-exactos 48/48 en Debug y Release).

### Archivos

| Archivo | Cambio |
|---|---|
| `Source/Core/Audio/AudioAutopilot.h` | Lógica pura: `CalibrationOptions`, `buildCalibrationLadder()` y `evaluateStability()` |
| `Source/Core/Audio/CpuMeter.h` | **Ventana de medición:** peor tiempo de bloque y mayor intervalo entre inicios de bloque desde el último reset. El reset lo pide cualquier hilo y lo aplica el audio thread en el bloque siguiente, así que el audio thread sigue siendo el único que escribe. Agrega 2 cargas atómicas y 2 comparaciones por bloque, sin locks ni allocations |
| `Source/Core/AudioEngine.h` | Getters inline de la ventana (`requestTimingWindowReset`, `getWindowPeakProcessTimeMs`, `getWindowMaxCallbackIntervalMs`). Sin cambios en `AudioEngine.cpp` |
| `Source/Core/PluginProcessor.h/.cpp` | `setOutputSilenced()`: `processBlock` limpia el buffer **después** del motor, así el DSP sigue corriendo y la carga medida es la real (decisión 2). En `false` por defecto: VST3 y presets no cambian |
| `Source/Core/Audio/StandaloneAudioAutopilot.h` | Máquina de estados de la calibración, cancelación, botón de panel del driver y estado unificado (`Activity`) para la tarjeta. El constructor ahora recibe el `NOVAAudioProcessor` |
| `Source/GUI/Wizards/AudioSetupWizard.h` | Tarjeta "RECOMMENDED SETUP": botones **CALIBRATE BUFFER**, **CANCEL** y **DRIVER PANEL**, barra de progreso y etiqueta CALIBRATING / CALIBRATED / NOT CALIBRATED |
| `Source/Core/AudioEngineTests.cpp` | 2 tests nuevos: escalera de tamaños (ASIO, WASAPI Exclusive, Low Latency, límites, perfil Canciones) y veredicto en los bordes de cada umbral |

### Cómo calibra

1. **Solo con drivers de baja latencia:** ASIO, WASAPI Exclusive y Low Latency. En Shared y DirectSound el buffer real es el del mezclador de Windows, así que la tarjeta pide cambiar de driver primero.
2. **Buffer fijo** (el driver ofrece un solo tamaño): no prueba nada y sugiere el botón **DRIVER PANEL**, que abre el panel con los mismos pasos que el selector de JUCE (bloqueo modal y reinicio del dispositivo si el driver lo pide).
3. **Escalera:** el menor tamaño disponible ≥ cada escalón de {32, 48, 64, 96, 128, 192, 256, 384, 512}, sin repetidos. Ejemplos:
   - ASIO: 32, 64, 128, 256, 512.
   - WASAPI Exclusive (múltiplos de 32 desde el mínimo del equipo): por ejemplo 144, 192, 256, 384, 512.
   - El límite de 512 (~10.7 ms por sentido) evita pruebas largas sin sentido para tocar.
4. **Por cada tamaño:**
   - Aplica el buffer y lee el tamaño real. Si el driver impone uno ya medido, lo salta.
   - Espera 0.5 s, reinicia la ventana y mide 4 s con la salida en silencio y el preset actual cargado.
5. **Pasa si:**
   - Cero xruns: los que reporta el driver más los callbacks que excedieron el bloque (`AudioDeviceManager::getXRunCount()`).
   - Llegó al menos el 90% de los callbacks esperados.
   - Carga pico < 70% del bloque.
   - Mayor intervalo entre callbacks < 2× el bloque.
6. **Resultado:**
   - Se queda con el primer tamaño que pasa y avisa "Stable at N samples (…) ~X ms round trip" con **UNDO**.
   - Si ninguno pasa, si se cancela o si el dispositivo cambia a mitad de la prueba, vuelve el buffer anterior.
   - La salida se desmutea siempre, incluso si se cierra la app a mitad de la calibración.
7. **Duración:** ~4.6 s por tamaño probado. Si pasa en 64: ~10 s. Peor caso (5 tamaños): ~25 s.
8. **Registro:** cada paso queda en el log de sesión (`autopilot.calibration`: buffer, xruns, carga pico, gap y % de callbacks), para ajustar los umbrales con hardware real.

### Decisiones de diseño

- **Gap máximo 2× en vez de 1.5× (cambio sobre 4.4):** muchos drivers USB entregan callbacks en ráfagas alineadas a los paquetes USB de 1 ms. Con buffers chicos eso da intervalos de hasta ~1.5× sin cortes audibles, y con 1.5× se descartarían buffers que funcionan. 2× sigue detectando los cortes por DPC (un callback que llega un bloque entero tarde). Es un parámetro de `CalibrationOptions`, a revisar con el log de pruebas reales.
- **La carga pico se mide sobre `AudioEngine::process`** (el `CpuMeter` existente), no sobre el callback completo. El overhead de JUCE es chico frente al DSP y el margen del 30% lo absorbe.
- **Sin persistencia por equipo todavía:** JUCE guarda el buffer elegido en `audioSetup`. La memoria por firma de equipo (4.8) llega con F3.

### Validación

- Build Debug Standalone: 0 errores, 0 warnings.
- Base validation (Debug): **PASS. 326 resultados, 13617 checks, 0 fallos** (+2 tests de calibración).
- Golden metrics (Debug): PASS.
- Checksums bit-exactos: **48/48 idénticos en Debug y Release**.
- Policy del audio thread: 0 hallazgos en rutas de audio. Fallan P9F/P9G (cambios sin commitear en `AudioEngine.h`).
  - En esta corrida también fallaron los chequeos de presets "fuera de DraftFactory". Lo causa el worktree `.claude/worktrees/…` de otra sesión, que vive dentro del repo y tiene copias de los presets. No viene de este cambio y desaparece al borrar el worktree.
- rt-profile Release: 64/64 PASS. Mediana del CPU promedio frente a `t0-rt-profile-release-after.json`: escenarios de motor (pasan por `CpuMeter`) +2.9%; escenarios `proc_*` (no pasan por `CpuMeter`; sirven de control) +9.6%. El costo de la ventana de medición queda dentro del ruido entre corridas.

### Prueba manual (pendiente)

1. Con la interfaz en ASIO, en Audio Setup → Scan: **CALIBRATE BUFFER**. La salida se silencia, la barra avanza y la etiqueta muestra CALIBRATING x%. Al terminar: "Stable at N samples…", un aviso con UNDO, y la fila Buffer Size muestra N.
2. Tocar con el buffer calibrado: no debería haber clicks. Si los hay, anotar N y el log `autopilot.calibration` (umbrales a ajustar).
3. UNDO → vuelve el buffer anterior.
4. CANCEL a mitad de la calibración → vuelve el buffer anterior y el audio suena.
5. Con un preset pesado (p. ej., Octave + amp high-gain + reverb), el buffer elegido debería ser igual o mayor que con un preset liviano.
6. En Windows Audio (Shared), CALIBRATE BUFFER no aparece y la tarjeta recomienda cambiar de driver.
7. **DRIVER PANEL** abre el panel del driver ASIO; al cerrarlo, el audio vuelve.
