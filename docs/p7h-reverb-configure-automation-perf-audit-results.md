# P7H - Reverb Configure / Automation Perf Audit Results

Fecha: 2026-05-06

## Objetivo

Auditar `ReverbPedal::engine.configure` bajo automatizacion intensa y endurecer la cobertura sin cambiar tono, parametros, IDs, schema, routing, presets ni golden baselines.

P7F/Reaper smoke permanece pendiente por entorno y no se marca como PASS en esta fase.

## Archivos revisados

- `Source/Effects/Pedals/Reverb/ReverbPedal.h`
- `Source/Core/AudioEngineTests.cpp`
- `scripts/check-audio-thread-policy.ps1`
- `docs/audio-realtime-safety-audit.md`
- `docs/pedal-audit-matrix.md`

## Diagnostico de `engine.configure`

`ReverbPedal::processBlock` llama `engine.configure(...)` desde el audio thread, pero solo cuando cambia alguno de los parametros relevantes cacheados:

- `mode`
- `decay`
- `tone`
- `size`
- `damping`
- `bassCut`
- `diffusion`
- `width`
- `mod`
- `predelay`
- `duck`
- `swell`
- `gate`
- `reverse`
- `freeze`

El path ya tenia thresholds para evitar jitter flotante: `1e-4f` para la mayoria de floats y `0.1ms` para predelay. En parametros estables no deberia reconfigurar cada bloque.

`Nova::Reverb::Engine::configure` no muestra patrones obvios de allocation (`assign`, `resize`, `reserve`, `setSize`, `new`, `make_unique`, `make_shared`) en la seccion auditada. El trabajo que si realiza es pesado para automation por bloque: ajuste de longitudes de lineas FDN, delays de diffusers/dispersion, filtros, early reflections, voces auxiliares de character/reverse y smoothers. Por eso se agrega regression coverage de conteo y bounded output.

No se encontraron `SessionLogger`, `juce::String`, `Logger` ni `DBG` dentro de `ReverbPedal::processBlock`.

## Cambios realizados

Se agrego instrumentacion diagnostica no tonal:

- `Nova::Reverb::Engine::getConfigureCallCount()`
- `Nova::Reverb::Engine::resetConfigureDiagnostics()`
- `ReverbPedal::getReverbConfigureCallCount()`
- `ReverbPedal::resetReverbConfigureDiagnostics()`

La instrumentacion solo cuenta llamadas a `configure`; no cambia coeficientes, curvas, rangos, modo, buffers, tails ni mezcla.

No se aplico fix DSP adicional porque la auditoria confirmo que ya existe gating por cambios y no se encontro allocation obvia dentro de `configure`. Cambiar la arquitectura para sacar `configure` del audio thread o separar fast/slow params podria alterar tails/modos y queda fuera de P7H.

## Tests agregados

En `Source/Core/AudioEngineTests.cpp`:

- `P7H ReverbPedal configure is not called every block under stable params`
- `P7H ReverbPedal aggressive automation sweep remains finite and bounded`
- `P7H ReverbPedal mode changes remain bounded`
- `P7H ReverbPedal freeze gate reverse swell automation remains bounded`

Cobertura:

- parametros estables no incrementan el contador despues del configure inicial;
- sweep agresivo de decay, size, tone, damping, diffusion, width, mod, predelay y mix;
- cambios de modo entre Spring/Plate/Hall/Room/Shimmer/Cloud;
- automation de duck/swell/gate/reverse/freeze;
- salida finita, sin NaN/Inf;
- picos y energia sostenida dentro de limites deterministas;
- DC tardio acotado bajo sweep agresivo.

## Policy checks agregados

`scripts/check-audio-thread-policy.ps1` agrega checks `p7h_*` para:

- documento P7H presente;
- instrumentacion de conteo de configure presente;
- tests P7H presentes por nombre;
- `ReverbPedal::processBlock` sin logging/strings;
- seccion `Engine::configure` presente;
- `Engine::configure` sin patrones obvios de allocation.

No se eliminaron checks previos. `contractChecks` debe subir respecto a P7G.

## Validacion

Validacion completa P7H ejecutada:

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings.
- `NOVA_SharedCode` Release x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings.
- `NOVA_VST3` Release x64: PASS, 0 warnings.
- `git diff --check`: PASS; solo avisos normales de CRLF en archivos ya modificados.
- `run-base-audio-validation.ps1` primera corrida final: PASS, `results=193`, `passes=6533`, `failures=0`, `failingResults=0`.
- `run-base-audio-validation.ps1` segunda corrida final: PASS, `results=193`, `passes=6533`, `failures=0`, `failingResults=0`.
- `run-golden-audio-metrics.ps1`: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`.
- `run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `16/16/0/0`.
- `run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`.
- `check-audio-thread-policy.ps1`: PASS, `failures=0`, `warnings=0`, `legacyWarnings=0`, `legacyQuarantined=4`, `contractFailures=0`, `contractChecks=155`.
- `run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS. Una primera corrida tuvo un WARN transitorio de RT single-run en `sample_rate_96000`; la repeticion final paso limpia con `16/16/0/0`, por lo que el artifact final queda PASS.

## Riesgos restantes

- `configure` sigue corriendo en el audio thread cuando hay automation real de parametros estructurales. P7H lo mide y lo acota, pero no redisenia la arquitectura.
- Separar fast params de structural configure podria mejorar CPU bajo automation extrema, pero puede cambiar tails o transiciones si no se disena con cuidado.
- Los cambios de modo siguen siendo reconfiguraciones pesadas por definicion; el test cubre bounded output, no coste temporal real.

## Recomendacion para P7I

Si P7I quiere optimizar mas, tratarlo como subfase dedicada de Reverb surgery con medicion offline de configure frequency por parametro. Separar parametros fast (`mix`, `duck`, `swell`, `gate`, `reverse`, `freeze`, quizas `width`) de parametros estructurales (`mode`, `size`, `diffusion`, `predelay`, tuning de voces) solo si se puede demostrar equivalencia tonal/golden sin baseline update.
