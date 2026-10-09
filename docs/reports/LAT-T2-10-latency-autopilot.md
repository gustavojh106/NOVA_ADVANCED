# LAT-T2-10 — Latency Autopilot: investigación

- **Fecha:** 2026-10-09
- **Estado:** investigación terminada; sin código. Faltan las decisiones de la sección 8 para empezar la Fase 1.
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
3. **Pasa si:** cero xruns nuevos, carga pico < 70% del presupuesto del bloque e intervalo máximo entre callbacks < 1.5× el período.
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

## 8. Decisiones pendientes (del usuario)

1. **Primera ejecución:** ¿aplicar la configuración automáticamente con aviso y "Deshacer", o preguntar antes? *Recomendado: automático con aviso.*
2. **Prueba de estabilidad:** ¿está bien silenciar la salida durante 10–20 s mientras calibra? *Recomendado: sí.*
3. **Exclusive en Estudio:** ¿se permite, sabiendo que bloquea el audio de otras apps? *Recomendado: sí en Estudio, nunca en Canciones.*
4. **Monitoreo continuo:** ¿solo sugerencias o ajuste automático? *Recomendado: solo sugerencias.*
5. **Desmute automático de la entrada** según la política de 4.7. *Recomendado: sí.*

## 9. Referencias en el código

- Estado del dispositivo en el Standalone: `juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h` (`reloadAudioDeviceState`, `saveAudioDeviceState`, `shouldMuteInput`)
- ASIO: `juce_audio_devices/native/juce_ASIO_windows.cpp` (`readBufferSizes`, `open`, `asioMessagesCallback`, `showControlPanel`)
- WASAPI: `juce_audio_devices/native/juce_WASAPI_windows.cpp` (`querySupportedBufferSizes`, `tryInitialisingWithBufferSize`)
- Cortes y carga: `juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp` (`getXRunCount`, `getCpuUsage`)
- NOVA: `Source/GUI/Wizards/AudioSetupWizard.h`, `Source/Core/Audio/CpuMeter.h`, `Source/Core/NovaSettingsService.h`
