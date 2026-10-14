# E6 — Reporte de problemas y aviso de presets más nuevos

**Fecha:** 2026-10-13 · **Rama:** `perf/t0-latency-optimizations` · **Checklist:** `docs/latency-optimization-roadmap.md` §11b

## 1. Qué faltaba

- NOVA ya escribía un log de sesión (`%APPDATA%/NOVA/Logs/session-log.txt`) y un diagnóstico del motor (`AudioEngine::buildDiagnosticReport`), pero no había forma de sacarlos desde la app. Un usuario de la beta tenía que buscar carpetas ocultas.
- **Hallazgo B-18:** el log se **borraba** al iniciar cada sesión. Si NOVA se colgaba y el usuario la volvía a abrir para reportarlo, el log de la sesión que falló ya no existía.
- E2 detectaba los presets de una versión más nueva (`isFromNewerSchema`), pero solo lo dejaba en el log. Si el usuario guardaba encima, perdía sin saberlo lo que esta versión no entiende.

## 2. Exportar el reporte (Settings > HELP)

Nueva sección **HELP** en Settings (junto a GENERAL y LIBRARY), igual en Standalone y VST3:
- La versión: "NOVA 1.0.0 · Standalone · Release build".
- **Export problem report:** pregunta "What happened?" (opcional), guarda `NOVA-problem-report-<fecha>.zip` en `Documents\NOVA\Problem Reports\` y abre el Explorador con el archivo seleccionado.
- **Open logs folder:** abre la carpeta de logs, por si la exportación falla.

Contenido del zip (`Source/Core/ProblemReport.h`):

| Archivo | Qué tiene |
|---|---|
| `report.txt` | Lo que escribió el usuario; versión, build, formato (Standalone/VST3) y DAW; Windows, CPU, RAM y JUCE; preset actual; audio (driver, dispositivos, frecuencia, buffer, latencia total y del driver, canales activos, dropouts del driver, carga de CPU de audio, monitoreo, relojes distintos de E4, recomendación del Autopilot); diagnóstico del motor (cadenas, latencia por tramo, CPU, auto-heal) |
| `session-log.txt` | El log de esta sesión, vaciado de la cola justo antes de exportar |
| `session-log-previous.txt` | El log de la sesión anterior (después de un crash, el de la sesión que falló) |
| `current-rig.nova-preset` | El rig actual, que se puede cargar como preset |

**Privacidad:** la carpeta del usuario se reemplaza por `%USERPROFILE%` en todos los textos (`C:\Users\<nombre>\...`, con `\` o `/` y sin importar mayúsculas). Los nombres de los dispositivos de audio se quedan porque hacen falta. Nada se envía por internet: el usuario adjunta el archivo.

## 3. Log de la sesión anterior (B-18, corregido)

`SessionLogger` ya no borra el log al iniciar: lo mueve a `session-log-previous.txt` (`rotateLogFile`). Se guarda solo la última sesión anterior.

## 4. Aviso de presets de una versión más nueva

Cuando se carga un preset, el preset de inicio o una sesión de DAW guardados por un NOVA más nuevo, el procesador deja un aviso (`takeNewerStateNotice`), y el editor lo muestra **una vez** en un diálogo:

> "From The Future" was saved by NOVA 9.9.0; this is NOVA 1.0.0. It loaded as well as possible, but pedals or settings this version doesn't know are missing. Saving over it here would drop them: update NOVA to use it fully.

En un DAW, el aviso aparece al abrir el editor. El editor lo revisa dos veces por segundo, así que también cubre los presets que se cargan sin la ventana abierta.

## 5. Tests (`E6ProblemReportTests`)

- El texto del reporte tiene todas las secciones; los datos faltantes se marcan; la línea Host solo aparece en un DAW; el nombre del archivo.
- La anonimización: rutas con `\` y con `/`, mayúsculas, solo cambia rutas, ignora una carpeta raíz.
- La rotación del log con archivos temporales: sin log actual, después de un "crash", solo se guarda la última sesión.
- La exportación con un procesador real: el zip tiene el reporte, el log de esta sesión (con la entrada `report.export`, sin la carpeta del usuario) y un rig que se vuelve a cargar con el Overdrive.
- El aviso: un preset de esta versión no deja aviso; uno más nuevo sí (con nombre y versión) y solo una vez; una sesión de DAW más nueva sin versión también.

**Validación:** base PASS (349 resultados, 14274 checks, 0 fallos) en Debug y Release; bit-exacto 56/56 en Debug y Release; golden Release PASS; policy del audio thread PASS. **En la app** (hook `NOVA_UI_SNAPSHOT_DIR`, que ahora también captura HELP y exporta un reporte real junto a las imágenes): el zip de 14 KB trae los 4 archivos, con el equipo real del usuario (UR12 → BlackShark, 12 dropouts del driver, aviso de relojes de E4) y 0 apariciones del nombre de usuario. HELP revisada a 1920×1080 y 1280×760.

## 6. Prueba manual (con tu equipo)

| # | Prueba | Qué esperar |
|---|---|---|
| E6-M1 | Settings > HELP > Export problem report, escribe una frase y pulsa Export | Se abre el Explorador en `Documents\NOVA\Problem Reports` con el zip seleccionado. Adentro: los 4 archivos; `report.txt` con tu frase y tu equipo de audio, sin tu nombre de usuario en las rutas |
| E6-M2 | Abre NOVA dos veces seguidas y exporta | `session-log-previous.txt` tiene la sesión anterior |
| E6-M3 | En un DAW (VST3), exporta desde HELP | `report.txt` dice "VST3" y el nombre del DAW |

## 7. Fuera de alcance

- Envío automático (subir el reporte a un servidor): requiere servidor, consentimiento y política de privacidad.
- Volcados de memoria (minidumps) al colgarse: necesitan código nativo de Windows; el log de la sesión anterior cubre lo básico.
