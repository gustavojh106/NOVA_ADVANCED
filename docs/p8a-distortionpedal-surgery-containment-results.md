# P8A DistortionPedal Surgery & Containment Results

Fecha: 2026-05-07

## Resumen

P8A investigo el fallo real observado en LineB cuando `Distortion -> Reverb -> Chorus` quedaba activo con `switchMode=LineB_Only`, `forceMono=true`, `gainB=2.0`, `widthB=2.0`, `outputVolumeDb=-2.81` y `outputLimiterDb=-12`.

La cadena Reverb + Chorus era estable antes de activar Distortion. Al activar Distortion en modo Metal (`modeIndex=3`) con `distGain=58`, `distMix=1.0` y `distLevel=0.64`, Distortion podia entregar energia > 1.0 hacia Reverb. Esa energia cargaba el feedback/tail de Reverb y mantenia el OutputChain limitando aun despues de bypassar Distortion.

## Cadena reproducida

Test deterministico agregado:

`InputChain -> Distortion -> Reverb -> Chorus -> ChannelStrip -> OutputChain`

Parametros principales:

- `inputGainDb=-11.28`
- `forceMono=true`
- `gainB=2.0`
- `panB=0.0`
- `widthB=2.0`
- `outputVolumeDb=-2.81`
- `outputLimiterDb=-12`
- Distortion Metal: `gain=58`, `tone=0.54`, `body=0.52`, `mix=1.0`, `level=0.64`, `tight=0.42`
- Reverb/Chorus activos con parametros nominales reconstruibles desde sus defaults y una cola Hall moderada.

## Medicion pre-fix

La primera corrida con los tests P8A, antes de tocar DSP, fallo como se esperaba:

- Base validation: `results=203 passes=6607 failures=4 failingResults=2`
- Distortion metal high-gain extremo:
  - `peak=2.3066`
  - `rms=1.0641`
  - `dc=0.00100`
  - `nearClipSamples=67496`
  - `clippedSamples=63424`
- Cadena P8A con `distGain=58`, `distLevel=0.64`:
  - salida Distortion / entrada Reverb: `peak=1.2957`, `rms=0.7014`, `dc=0.00466`, `nearClipSamples=2020`, `clippedSamples=1858`
  - ventana completa de entrada Reverb post-activacion: `peak=1.2957`, `rms=0.4230`, `dc=0.00084`, `nearClipSamples=2020`, `clippedSamples=1858`
  - OutputChain recovery: `activeBlocks=192`, `touchedSamples=24564`

Despues del primer soft ceiling, el pico quedo contenido (`peak=0.9950`, `clippedSamples=0`), pero el test extremo revelo DC post-clipping (`dc=0.05432`). Por eso el fix final incluye DC blocker subsonic post-mix.

## Causa raiz probable

`DistortionPedal` tenia clipping interno y un DC blocker dentro de la etapa oversampled, pero no tenia containment final post `mix/level`. El mapping de `distLevel` llega hasta `1.6x`; en modo Metal la combinacion de filtros post-clip, level y entrada transitoria podia producir salida sostenida por encima de unity antes de Reverb.

El problema no estaba en `AudioEngine`, `DryWetMixer`, `RoutingMixer`, `GraphBuilder`, Reverb ni OutputChain. OutputChain solo recibia energia ya excesiva y la limitaba al final.

## Fix aplicado

Archivo modificado: `Source/Effects/Pedals/Distortion/DistortionPedal.h`

- Se agrego `containOutputSample()` como soft ceiling final post-level/post-mix.
- Techo: `0.995f`.
- Rodilla: transparente por debajo de `0.985 * ceiling`.
- Se agrego `finalDcBlock` a 10 Hz despues del containment inicial y antes de la salida final.
- Se aplica containment otra vez despues del DC blocker para evitar overs residuales.

No se cambiaron parametros, schema, IDs, factory presets, routing, OutputChain, Reverb, Chorus ni AudioEngine.

## Preservacion tonal

El cambio esta al final de Distortion, despues de la voz existente. La ruta nominal por debajo de la rodilla no cambia. El ceiling solo actua en overs destructivos que antes alimentaban Reverb por encima de unity; el DC blocker es subsonic y no revocea el rango de guitarra.

Los tests existentes de Distortion siguen cubriendo:

- `mix=0` transparencia
- firmas distintas por modo
- tight/gate behavior
- automation stress
- round-trip state

## Tests agregados o endurecidos

- `P8A DistortionPedal metal high-gain output stays bounded before downstream ambience`
- `P8A DistortionPedal rejects DC accumulation under biased high-gain input`
- `P8A Distortion Reverb Chorus bypass recovery stays bounded`
- `DistortionPedal automation stress remains finite under aggressive changes` ahora verifica el ceiling P8A.

## Policy checks agregados

`scripts/check-audio-thread-policy.ps1` ahora codifica:

- doc P8A presente
- tests P8A presentes
- soft ceiling + `finalDcBlock` presentes en Distortion
- no `SessionLogger`, `juce::String`, `DBG` ni logging en `DistortionPedal::processBlock`
- no allocation obvia en `DistortionPedal::processBlock`
- no golden baseline update
- no known-failure ignore agregado a base validation

## Validacion final

Validacion ejecutada:

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings
- `NOVA_SharedCode` Release x64: PASS, 0 warnings
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings
- `NOVA_VST3` Release x64: PASS, 0 warnings
- `git diff --check`: PASS
- `run-base-audio-validation.ps1 -Configuration Debug -Platform x64`: PASS, dos corridas consecutivas
  - `results=204 passes=6614 failures=0 failingResults=0`
- `run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64`: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`, sin baseline update
- `run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64`: PASS `16/16/0/0`
- `run-rt-profile-stability.ps1 -Configuration Release -Platform x64 -CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`
- `check-audio-thread-policy.ps1`: PASS
  - `failures=0`
  - `warnings=0`
  - `legacyWarnings=0`
  - `legacyQuarantined=4`
  - `contractFailures=0`
  - `contractChecks=183`
- `run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS
- `run-diagnostics-bundle.ps1`: PASS

No hubo cambios de schema/IDs, AudioEngine, DryWetMixer, RoutingMixer, GraphBuilder, UI/UX, presets ni golden baselines.

## Riesgos restantes

- El ceiling final es intencionalmente conservador para proteger ambience/chorus. En ajustes extremos de `distLevel=1.0`, el pedal ya no puede usarse como boost digital por encima de unity hacia efectos posteriores.
- La validacion golden decidira si algun escenario existente detecta cambio audible. Si falla, P8A debe detenerse sin baseline update.

## Recomendacion P8B

P8B deberia hacer QA auditivo/DAW enfocado solo en Distortion despues de esta contencion: comparar modo Metal y Studio con amp/cabinet, Reverb post-Distortion y bypass recovery real. No mezclar con Reverb refactor ni smoke DAW general.
