# P3 Audio Baseline Lock

Fecha: 2026-04-29
Commit baseline: `da11290f86fcbda228128143b89e2da0f4de97cc`

## Resumen P0/P1/P2

- P0: limpieza de factories IIR y hardening RT base documentados en `docs/p0-iir-coefficients-cleanamp-cabinets.md` y `docs/p0-rt-safety-changes.md`.
- P1: pedal safety (no-allocation fallbacks, estabilidad finita, fix de Phaser DC) documentado en `docs/p1-pedal-safety-changes.md`.
- P2: cierre de fallos core (InputChain/OutputChain/AudioEngine/Reverb) y validación completa en verde documentado en `docs/p2-core-test-failures-diagnosis.md` y `docs/p2-core-test-failures-changes.md`.

## Estado de builds (P3)

- `NOVA_SharedCode` Debug x64: PASS.
- `NOVA_StandalonePlugin` Debug x64: PASS.

## Estado de `run-base-audio-validation.ps1` (P3)

- Resultado: PASS.
- Resumen: `results=136 passes=5758 failures=0 failingResults=0`.
- Known failures ignorados por script: ninguno.
- Group breakdown (fallos):
  - Core: 0
  - P1 Pedal Safety: 0
  - Reverb: 0
  - Routing: 0
  - OutputChain: 0
  - AudioEngine: 0
  - Regression: 0

## Pedales/procesadores considerados estables para esta baseline

Procesadores globales:
- InputChain
- ChannelStrip
- OutputChain
- AudioEngine (rutas clean/dry/parallel, bypass, latency rebuild)

Pedales y bloques validados en suite actual:
- Compressor
- Noise Gate
- EQ
- Boost
- Neural
- Overdrive
- Distortion
- Fuzz
- Wah
- Octave
- Chorus
- Phaser
- Flanger
- Tremolo
- Reverb
- Delay

Bloques P1 auditados con fallback no-allocation:
- Cabinet
- Vintage 2x12
- Modern 4x12
- Compressor
- Distortion
- Fuzz
- Neural
- Wah
- Phaser

## Riesgos restantes

- Cobertura golden aún incompleta para:
  - `OutputChain biased input / DC cleanup` (sin escenario dedicado offline QA).
  - `Overdrive + CleanAmp + Reverb chain nominal` (sin escenario dedicado offline QA).
- Advertencia de compilación residual observada en SharedCode: `C4100` (parámetro no usado en `AudioEngine.cpp`).
- Cambios de línea (`LF`/`CRLF`) siguen reportándose como warnings de git, sin errores de formato.

## Cambios que NO deben tocarse sin update de golden render/metrics

- Cualquier cambio DSP en:
  - OutputChain (DC cleanup/limiter behavior)
  - Reverb (Cloud/swell/reverse/reverse+swell)
  - Overdrive musical-safe behavior
  - Routing clean/dry/parallel base
- Cambios de smoothing/envelopes, ganancias internas, filtros y dinámicas aunque sean sub-audio.
- Ajustes de thresholds/tolerancias de tests de audio.

## Checklist antes de volver a tocar DSP

- [ ] Ejecutar `run-base-audio-validation.ps1` y confirmar `failures=0`.
- [ ] Ejecutar `run-golden-audio-metrics.ps1` y confirmar sin drift.
- [ ] Si el cambio afecta tono/comportamiento, actualizar baseline con `-UpdateBaseline` solo tras aprobación humana.
- [ ] Registrar motivación técnica y alcance exacto del cambio DSP.
- [ ] Confirmar explícitamente: sin cambios de IDs de parámetros ni schema de presets.
- [ ] Confirmar explícitamente: sin cambios UI/wizards dentro del mismo patch DSP.
