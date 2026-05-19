# P7E DAW / Standalone Smoke Matrix

Fecha: 2026-05-06

## Alcance

Esta matriz prepara smoke tests manuales y parcialmente automatizables para Standalone, VST3 y host-state. No cubre tonal QA, factory preset curation, UI/UX polish ni release engineering profundo.

Build minimo recomendado:

- `NOVA_StandalonePlugin` Release x64.
- `NOVA_VST3` Release x64.
- Base validation PASS dos corridas consecutivas.
- Golden metrics PASS contra baseline P4.
- RT Release PASS `16/16/0/0`.
- Policy scan `failures=0`, `contractFailures=0`.

## Standalone Smoke

| ID | Escenario | Pasos | Resultado esperado | Evidencia |
| --- | --- | --- | --- | --- |
| ST-01 | Launch | Abrir `Builds/VisualStudio2022/x64/Release/Standalone Plugin/NOVA.exe`. | App abre sin crash/hang. | `session-log.txt`, screenshot opcional. |
| ST-02 | Audio device select | Seleccionar driver/dispositivo disponible, input y output. | Dispositivo aplica sin crash; medidores responden con senal. | Screenshot opcional, log. |
| ST-03 | Sample rate 44.1k | Cambiar dispositivo a 44.1 kHz. | Audio sigue finito; no pop/hang persistente. | Log, notas. |
| ST-04 | Sample rate 48k | Cambiar a 48 kHz. | Audio sigue finito; UI responde. | Log, notas. |
| ST-05 | Sample rate 96k | Cambiar a 96 kHz si el dispositivo lo soporta. | Audio sigue finito; CPU dentro de rango practico. | Log, notas. |
| ST-06 | Buffer 32 | Cambiar buffer a 32 samples si soportado. | Sin crash; CPU visible; audio sin NaN/ruido runaway. | Log, screenshot opcional. |
| ST-07 | Buffer 64 | Cambiar buffer a 64 samples. | Sin crash; audio estable. | Log. |
| ST-08 | Buffer 128 | Cambiar buffer a 128 samples. | Sin crash; audio estable. | Log. |
| ST-09 | Buffer 256 | Cambiar buffer a 256 samples. | Sin crash; audio estable. | Log. |
| ST-10 | Buffer 512 | Cambiar buffer a 512 samples. | Sin crash; audio estable. | Log. |
| ST-11 | Engine on/off | Alternar engine off/on durante entrada de audio. | Off preserva salida segura; on recupera procesamiento. | Log, notas. |
| ST-12 | Add/remove pedals | Agregar y remover pedales Pre/Amp/FX/Cabinet. | Orden canonico y sin crash. | Screenshot, log. |
| ST-13 | Bypass pedals | Bypassear varios pedales y alternar rapido. | Sin clicks extremos, NaN, hang ni graph invalido. | Log, notas. |
| ST-14 | Save/load session | Guardar preset/session, cerrar, reabrir y cargar. | Type/id/zone/enabled/params preservados. | Preset file, log. |
| ST-15 | Corrupt state recovery | Intentar cargar preset truncado/corrupto de prueba. | Rechazo limpio o fallback a estado seguro; app usable. | Corrupt preset, log. |
| ST-16 | Multi-minute idle/play | 5 min idle y 5 min con entrada o loop. | CPU estable; sin memoria runaway visible; sin audio corrupto. | Log, notas de CPU. |
| ST-17 | CPU/process-time visibility | Observar metering/diagnostics durante cadena nominal y cadena pesada. | Indicadores visibles/razonables; no bloqueo UI. | Screenshot opcional. |

## VST3 / DAW Smoke

| ID | Escenario | Pasos | Resultado esperado | Evidencia |
| --- | --- | --- | --- | --- |
| DAW-01 | Plugin scan | Escanear `NOVA.vst3` en host. | Host detecta plugin sin blacklist. | Host scan log. |
| DAW-02 | Instantiate plugin | Insertar NOVA en track audio. | Instancia abre y procesa silencio/audio sin crash. | Host project, log. |
| DAW-03 | Load/save host project | Guardar proyecto con NOVA y reabrir. | Estado de plugin preservado. | Project file, log. |
| DAW-04 | Close/reopen project | Cerrar host project y reabrir. | No crash; estado restaurado. | Project file, log. |
| DAW-05 | Host bypass on/off | Usar bypass del host. | Host bypass no rompe estado ni audio. | Notes/log. |
| DAW-06 | Plugin bypass on/off | Usar bypass/enable dentro de NOVA. | Estado preservado; audio estable. | Notes/log. |
| DAW-07 | Parameter automation sweep | Automatizar globals y varios parametros de pedal. | Sin zipper extremo, NaN, crash ni parameter corruption. | Automation lane screenshot/project. |
| DAW-08 | Preset/state save/load | Guardar estado en host y/o preset NOVA; recargar. | Estado canonico restaurado. | Project/preset/log. |
| DAW-09 | Sample-rate switch | Cambiar proyecto entre 44.1/48/96 kHz. | Plugin reprepare limpio; audio estable. | Host project/log. |
| DAW-10 | Buffer-size switch | Cambiar 32/64/128/256/512 si host permite. | Sin crash; process estable. | Notes/log. |
| DAW-11 | Transport play/stop | Alternar play/stop varias veces con audio. | No hang; tails/modulaciones estables. | Notes/log. |
| DAW-12 | Offline render | Render offline/bounce si host soporta. | Render completa sin crash y archivo finito. | Render file, log. |
| DAW-13 | Multi-instance | Insertar 4 instancias con cadenas distintas. | Proyecto guarda/reabre; CPU razonable. | Project/log. |
| DAW-14 | Remove while running | Remover instancia durante playback. | Host no crashea; audio engine se libera limpio. | Host log. |
| DAW-15 | Reopen after crash/corrupt state | Si hay crash o state corrupto reproducible, reabrir proyecto/copia. | NOVA no bloquea host; recovery limpio o issue registrado. | Project, dump/log. |

## Prioridad De Hosts

1. Reaper: primer host objetivo por rapidez de instalacion, logging y scripts de smoke reproducibles.
2. Ableton Live: ejecutar si esta instalado/disponible.
3. Cubase: ejecutar si esta instalado/disponible.
4. FL Studio: ejecutar si esta instalado/disponible.
5. Logic Pro: solo macOS; no aplica al entorno Windows actual.

Si un host no esta disponible, dejarlo como `NOT RUN - host unavailable` y conservar el checklist para ejecucion manual posterior.

## Reaper Priority Checklist

| ID | Escenario | Prioridad |
| --- | --- | --- |
| RPR-01 | Scan VST3 | P0 |
| RPR-02 | Instantiate on audio track | P0 |
| RPR-03 | Save/reopen project | P0 |
| RPR-04 | Host bypass + plugin bypass | P0 |
| RPR-05 | Automation sweep | P1 |
| RPR-06 | Sample-rate and buffer switch | P1 |
| RPR-07 | Offline render | P1 |
| RPR-08 | Multi-instance | P1 |
| RPR-09 | Remove plugin during playback | P1 |

## Pass / Fail Policy

- PASS: escenario completo, sin crash/hang, sin NaN/Inf audible/diagnostic, estado preservado cuando aplica.
- FAIL: crash, hang, plugin blacklist, project no reabre, audio corrupto sostenido, estado irrecuperable, o regression reproducible.
- WARN: host limitation, device unavailable, o issue menor no bloqueante con workaround claro.
- NOT RUN: host o hardware no disponible.
