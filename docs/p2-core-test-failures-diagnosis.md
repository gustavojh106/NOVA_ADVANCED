# P2 Core Test Failures Diagnosis

Fecha: 2026-04-28

Reporte base: `audio-base-test-report.txt`

Resumen observado:

- `results=136`
- `passes=5712`
- `failures=46`
- `failingResults=12`

## Diagnostico por test

### InputChain preserves single-jack guitar level when input arrives on one channel

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `1095`
- Expectativa: una senal en un solo jack debe aparecer en L/R sin perdida de -6 dB; `forceMono` tambien debe conservar nivel.
- Valor actual observado: finite pasa; las 3 aserciones de nivel fallan. El reporter no imprime RMS, pero por la ruta actual el primer bloque right-only queda con L cerca de `0.0`, R cerca de `0.127 RMS`; `forceMono` promedia L/R y queda cerca de `0.064 RMS`, bajo el umbral `0.10`.
- Causa probable: `AutoRoutingDetector` exige 3 bloques de confirmacion antes de copiar un canal activo al otro. El test y el caso de uso single-jack esperan recuperacion en el primer bloque. `applyForceMonoBlend()` usa promedio `(L + R) * 0.5`, lo que baja -6 dB cuando solo un canal tiene senal.
- Archivo/clase responsable: `Source/Core/DSP/Global/InputChain.*`, `AutoRoutingDetector`, `InputChainProcessor::applyForceMonoBlend`.
- Propuesta de fix: promocionar inmediatamente cuando el modo actual es `Stereo` y se detecta `LeftOnly`/`RightOnly`; para `forceMono`, usar suma mono normalizada por canales activos, no promedio fijo.
- Riesgo tonal/arquitectonico: bajo. Solo afecta entrada mono/single-jack y `forceMono`; no cambia IDs ni arquitectura.

### OutputChain protects limiter headroom from biased input

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `1331`
- Expectativa: una senal con DC offset debe quedar centrada y conservar al menos `82%` de la magnitud de 1 kHz respecto al caso limpio.
- Valor actual observado: falla la asercion de magnitud; la asercion DC pasa. El reporter no imprime el escalar, pero la magnitud biased queda por debajo de `clean * 0.82`.
- Causa probable: el DC blocker centra la senal, pero el transitorio inicial de offset alcanza el limiter antes de estabilizarse; el release actual del limiter es muy largo y mantiene reduccion de ganancia durante la ventana medida.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`, `OutputChainProcessor::PeakLimiter`.
- Propuesta de fix: mantener DC cleanup antes del limiter y acortar/rebalancear el release program-dependent para que una correccion por transitorio DC no deprima el tono sostenido.
- Riesgo tonal/arquitectonico: medio-bajo. Cambia recuperacion del limiter, no voicing base.

### OutputChain clamps legacy extreme limiter thresholds to an audible floor

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `1440`
- Expectativa: `setParams(0, -20)` debe tratarse como ceiling seguro cercano a `-12 dB`, con peak entre `0.18` y `0.32`; al volver a `0 dB`, latency debe ser `0`.
- Valor actual observado: peak queda fuera de rango por clamp actual hasta `-24 dB`; `getLatencySamples()` reporta `96` cuando el limiter esta en `0 dB`.
- Causa probable: `sanitizeLimiterDb()` permite `-24 dB`; `prepareToPlay()`/`reset()` siempre reportan el lookahead aunque el limiter este bypassed.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`.
- Propuesta de fix: clamp de limiter a `[-12, 0]`; reportar/aplicar lookahead solo cuando el threshold esta activo.
- Riesgo tonal/arquitectonico: bajo para presets legacy extremos; devuelve comportamiento esperado por baseline.

### Global processors preserve active params after reset

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `1803`
- Expectativa: `InputChain`, `ChannelStrip` y `OutputChain` deben conservar parametros activos despues de `reset()`.
- Valor actual observado: Input y ChannelStrip pasan; Output falla en ambos canales. Se espera `~0.5` con `-6 dB`; salida actual es `0.0` por delay de lookahead vacio.
- Causa probable: `OutputChain` aplica delay/lookahead incluso con limiter bypassed (`0 dB`), asi que un bloque de 4 samples sale desde el delay buffer vacio.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`.
- Propuesta de fix: bypass real del limiter/delay cuando el threshold esta inactivo.
- Riesgo tonal/arquitectonico: bajo.

### AudioEngine single-line mode preserves clean input within conditioning tolerance

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `1849`
- Expectativa: Line A sin pedales debe devolver las muestras limpias con tolerancia pequena.
- Valor actual observado: primeras 4 muestras L/R son `0.0`; esperado: `{0.25, -0.5, 0.75, -1.0}` y `{-0.2, 0.4, -0.6, 0.8}`.
- Causa probable: `OutputChain` aporta latency/lookahead aun con limiter `0 dB`, por lo que el graph devuelve silencio inicial.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`, efecto visible en `AudioEngine`.
- Propuesta de fix: misma correccion de latency/bypass en `OutputChain`.
- Riesgo tonal/arquitectonico: bajo.

### AudioEngine parallel routing keeps practical unity on identical clean lines

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `1900`
- Expectativa: Dual parallel sin pedales debe mantener unidad practica con compensacion de lineas.
- Valor actual observado: primeras 4 muestras L/R son `0.0`; esperado: `{0.1, 0.2, -0.3, 0.4}` y `{-0.4, 0.3, -0.2, 0.1}`.
- Causa probable: misma latency de `OutputChain` bypassed.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`.
- Propuesta de fix: misma correccion de latency/bypass en `OutputChain`.
- Riesgo tonal/arquitectonico: bajo.

### AudioEngine dry-only mix bypasses wet-path gain changes

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `2050`
- Expectativa: con `outputMixRaw=0`, el audio dry queda intacto y el diagnostico refleja `wetMix=0`.
- Valor actual observado: audio pasa; falla solo el diagnostico. Reporte actual contiene `wetMixTarget=0, wetMixCurrent=0`, pero no el substring `wetMix=0`.
- Causa probable: cambio de etiqueta del diagnostico, no fallo de DSP.
- Archivo/clase responsable: `Source/Core/AudioEngine.cpp`, `AudioEngine::buildDiagnosticReport`.
- Propuesta de fix: incluir alias `wetMix=<current>` manteniendo campos existentes.
- Riesgo tonal/arquitectonico: nulo.

### AudioEngine recovers cleanly across engine disable and re-enable within conditioning tolerance

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `2067`
- Expectativa: activo, disabled y re-enabled deben preservar input limpio.
- Valor actual observado: en los bloques activo y re-enabled, primeras 4 muestras L/R son `0.0`; expected `{0.22, -0.11, 0.33, -0.44}` y `{-0.15, 0.25, -0.35, 0.45}`.
- Causa probable: mismo lookahead de `OutputChain` bypassed en graph; disabled pasa seco fuera del graph.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`, visible via `AudioEngine`.
- Propuesta de fix: bypass real del limiter en `OutputChain` cuando `limiterDb == 0`.
- Riesgo tonal/arquitectonico: bajo.

### AudioEngine rebuilds graph latency when bypass changes node latency

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `2308`
- Expectativa: Overdrive activo aporta latency; al bypass, latency total debe ser `0`; al reactivar, debe volver.
- Valor actual observado: al bypass el valor actual es `100`; esperado `0`.
- Causa probable: `OutputChain` agrega ~`96` samples siempre, aun cuando el limiter esta bypassed, y se suma a la latency del pedal.
- Archivo/clase responsable: `Source/Core/DSP/Global/OutputChain.*`, `AudioEngine::buildGraphFromModelLocked`.
- Propuesta de fix: remover latency fija del `OutputChain` cuando limiter esta en `0 dB`.
- Riesgo tonal/arquitectonico: bajo-medio; cambia reporte de latency a lo que ya espera la suite.

### ReverbPedal produces a long finite tail that decays cleanly

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `3200`
- Expectativa: Cloud impulse tail finita, peak < `1.25`, late RMS > `1.0e-4`, end RMS < `late * 0.45`.
- Valor actual observado: finite y peak pasan; falla `lateRms > 1.0e-4`.
- Causa probable: Cloud tail queda demasiado atenuada despues del bloom inicial por trims/safety/envelope en la ruta wet/FDN.
- Archivo/clase responsable: `Source/Effects/Pedals/Reverb/ReverbPedal.h`, `Nova::Reverb::Engine`.
- Propuesta de fix: ajuste conservador de energia de late body en Cloud, sin subir runaway ni cambiar arquitectura.
- Riesgo tonal/arquitectonico: medio; cualquier ajuste de reverb puede sentirse tonal. Debe ser pequeno y medido por tests.

### ReverbPedal swell softens the wet attack and blooms afterward

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `3396`
- Expectativa: swell alto debe reducir early RMS a `< baselineEarly * 0.72`, luego bloom > early * `1.45`, y no borrar late body.
- Valor actual observado: falla la reduccion del early wet attack.
- Causa probable: `swellGain`/`comboAttackDamp` no atenúan suficiente ER/late early window para Cloud cuando `swellAmount` es alto.
- Archivo/clase responsable: `Source/Effects/Pedals/Reverb/ReverbPedal.h`, `computePerformanceGains()`.
- Propuesta de fix: bajar floor inicial de swell y/o hacerlo abrir mas lento, preservando late body.
- Riesgo tonal/arquitectonico: medio.

### ReverbPedal reverse and swell create a delayed cinematic bloom

- Archivo del test: `Source/Core/AudioEngineTests.cpp`
- Linea aproximada: `3516`
- Expectativa: reverse+swell debe suavizar onset, bloom late > early * `2.0`, y late body > baselineLate * `0.48`.
- Valor actual observado: early soften pasa; fallan late bloom ratio y late body.
- Causa probable: el combo reverse+swell reduce demasiado `reverseSendScale`/wet body o no alimenta suficiente delayed reverse bloom tras atenuar el ataque.
- Archivo/clase responsable: `Source/Effects/Pedals/Reverb/ReverbPedal.h`, `computePerformanceGains()` y reverse blend.
- Propuesta de fix: aumentar ligeramente reverse/swell late body y/o reducir atenuacion wetTrim del combo, sin tocar freeze/gate.
- Riesgo tonal/arquitectonico: medio.

## Orden de correccion propuesto

1. `InputChain`: single-jack y forceMono unity.
2. `OutputChain`: limiter clamp `[-12, 0]`, bypass real y latency 0 cuando inactivo, release menos pegado por transient DC.
3. `AudioEngine`: solo alias diagnostico `wetMix=` si siguen fallos despues de OutputChain.
4. `ReverbPedal`: ajustes pequenos de envelope/late body, medidos con la suite.
