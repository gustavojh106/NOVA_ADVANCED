# P2 Core Test Failures Changes

Fecha: 2026-04-28

## Archivos modificados

- `Source/Core/DSP/Global/InputChain.h`
- `Source/Core/DSP/Global/InputChain.cpp`
- `Source/Core/DSP/Global/OutputChain.h`
- `Source/Core/DSP/Global/OutputChain.cpp`
- `Source/Core/AudioEngine.cpp`
- `Source/Effects/Pedals/Base/ProcessorBase.h`
- `Source/Effects/Pedals/Reverb/ReverbPedal.h`
- `docs/p2-core-test-failures-diagnosis.md`
- `docs/p2-core-test-failures-changes.md`

## Cambios realizados

- `InputChain`: el detector single-jack ahora promociona inmediatamente desde `Stereo` a `LeftOnly`/`RightOnly` en el primer bloque detectado. `forceMono` conserva el nivel cuando solo un canal trae senal, en vez de promediar fijo a -6 dB.
- `OutputChain`: el limiter legacy extremo se limita a `[-12 dB, 0 dB]`; el lookahead/latency solo se reporta y procesa cuando el limiter esta activo; el release se hizo menos pegajoso ante transitorios con DC; el DC blocker de salida bajo de 20 Hz a 10 Hz para reducir el error de condicionamiento en caminos clean cortos manteniendo rechazo de DC.
- `AudioEngine`: al cambiar bypass de pedales se reconstruye la latency del graph y se actualiza el dry delay; el path dry/wet ya no redimensiona scratch buffers en proceso; el diagnostico vuelve a exponer `wetMix=`.
- `ProcessorBase`: los pedales con latency reportan `0` cuando quedan bypassed estable, pero mantienen dry delay interno durante transiciones para no romper crossfades.
- `ReverbPedal`: la primera configuracion despues de prepare/restore sincroniza los smoothers con los parametros ya cargados; los cambios posteriores siguen suavizados. Cloud conserva mas late body finito; swell reduce mejor el ataque wet; reverse y reverse+swell conservan bloom tardio util sin runaway.

## Tests corregidos

- `InputChain preserves single-jack guitar level when input arrives on one channel`
- `OutputChain protects limiter headroom from biased input`
- `OutputChain clamps legacy extreme limiter thresholds to an audible floor`
- `Global processors preserve active params after reset`
- `AudioEngine single-line mode preserves clean input within conditioning tolerance`
- `AudioEngine parallel routing keeps practical unity on identical clean lines`
- `AudioEngine dry-only mix bypasses wet-path gain changes`
- `AudioEngine recovers cleanly across engine disable and re-enable within conditioning tolerance`
- `AudioEngine rebuilds graph latency when bypass changes node latency`
- `ReverbPedal produces a long finite tail that decays cleanly`
- `ReverbPedal swell softens the wet attack and blooms afterward`
- `ReverbPedal reverse and swell create a delayed cinematic bloom`

## Tests todavia fallando

Ninguno en la validacion final:

- `results=136`
- `passes=5758`
- `failures=0`
- `failingResults=0`
- `status=PASS`

## Riesgos restantes

- `ReverbPedal` tuvo cambios audibles probables en Cloud/reverse/swell: mas late body y bloom tardio. No se cambio arquitectura, IDs ni preset schema, pero conviene capturar golden audio para esos modos.
- `scripts/run-base-audio-validation.ps1` todavia conserva una lista de known failures para Phaser y reverse+swell, aunque el reporte final ya no tiene fallos. Conviene limpiar esa lista en P3 para evitar falsos verdes futuros.
- Quedan warnings de build preexistentes en SharedCode (`TempoSyncable` struct/class y `latencySamples` que oculta miembro JUCE).

## Cambios con posible impacto tonal

- `OutputChain` DC blocker: cutoff sub-audio de salida de 20 Hz a 10 Hz. Esto reduce alteracion del path clean y mantiene DC cleanup medido.
- `ReverbPedal` Cloud/reverse/swell: mas sustain late y bloom reverse. El objetivo fue cumplir cola finita y bloom delayed sin runaway, no revoicing general.

## Comandos ejecutados

- `powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode`
- `powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin`
- `git diff --check`
- `powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`

## Resultados de build/tests

- `NOVA_SharedCode`: PASS. Warnings existentes: `C4099 TempoSyncable`, `C4458 latencySamples`.
- `NOVA_StandalonePlugin`: PASS, `0 Warning(s), 0 Error(s)`.
- `git diff --check`: PASS; solo advertencias de normalizacion LF/CRLF del worktree.
- `run-base-audio-validation.ps1`: PASS, `results=136 passes=5758 failures=0 failingResults=0`.
- No reaparecieron fallos P1.
- No reaparecio `PhaserPedal feedback loop rejects DC accumulation under sustained bias`.

## Recomendacion P3

- Congelar golden renders cortos para OutputChain clean/biased input y Reverb Cloud/reverse/swell.
- Limpiar known-failure ignores del script de validacion ahora que el reporte final esta verde.
- Atacar warnings estructurales de build y revisar legacy duplicate pedal paths fuera del alcance P2.
