# P7C Allocation Fallback and Feedback Stress Results

Fecha: 2026-05-06

## Resumen

P7C cierra los P1 de fallback sin allocation, hygiene de coeficientes de cabinets y regression locks de feedback/DC/NaN/picos sin cambios tonales intencionales.

No se tocaron presets, schema/IDs, routing, dry/wet global, lifecycle del graph ni golden baselines. No hubo revoicing.

## Archivos modificados

- `Source/Core/AudioEngineTests.cpp`
- `Source/Effects/Pedals/Delay/DelayPedal.h`
- `Source/Effects/Pedals/Flanger/FlangerPedal.h`
- `Source/Effects/Pedals/Neural/NeuralPedal.h`
- `scripts/check-audio-thread-policy.ps1`
- `docs/audio-realtime-safety-audit.md`
- `docs/pedal-audit-matrix.md`
- `docs/p7c-allocation-fallback-and-feedback-stress-results.md`

## P1 items cerrados

- `CompressorPedal`, `DistortionPedal`, `FuzzPedal`, `NeuralPedal`, `ClassicWahPedal`, `CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet`: `processBlock` tiene guard `canProcessBlock(buffer)` y fallback dry/no-op sin `setSize`, sin `resize`, sin allocation cuando el host excede el bloque/canales preparados.
- `NeuralPedal`: `OnePoleFilterBank` y `EnvelopeFollower` ya no dependen de `std::vector<float>` ni `ensureChannels` con resize; usan `std::array<float, kMaxHelperChannels>` y `hasChannels()` para degradar sin allocation si aparece un layout no preparado.
- Cabinets: `CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet` usan `juce::dsp::IIR::ArrayCoefficients<float>::make*` value-type; no quedan llamadas `IIR::Coefficients<float>::make*` en el path auditado.
- `PhaserPedal`: se conserva el fix minimo de DC con `feedbackDcBlock` y `outputDcBlock`; se fijo regression coverage para bias sostenido y NaN/Inf.
- `DelayPedal`, `FlangerPedal`, `ReverbPedal`: quedan stress locks deterministas para DC, NaN/Inf, feedback alto/runaway y picos altos.

## Fallbacks no-allocation

Patron validado:

- Buffers scratch/dry se dimensionan en `prepareToPlay`.
- `processBlock` valida `numSamples <= preparedBlockSize` y `numChannels <= preparedChannels`.
- Si el host rompe el contrato, el pedal incrementa `fallbackBlockCount`, no redimensiona, deja pasar dry/sanitized input y llama `endBypassProcess`.
- La matriz `P1 Pedal Safety` cubre 44.1k/48k/96k, bloques 32/64/128/256/480/512 y senales silence/impulse/sines/DC/strong peaks/NaN/Inf/noise.

## Cabinet coefficients

Se cerro sin revoicing: las frecuencias, Q y gains no se cambiaron. La migracion mantiene los mismos factories semanticos (`makeLowShelf`, `makeHighShelf`, `makeLowPass`, `makeHighPass`, `makePeakFilter`) pero usa `IIR::ArrayCoefficients<float>` para evitar ref-count allocation churn en el callback.

## Phaser DC

`PhaserPedal` ya contenia el fix minimo: DC blockers en feedback y output. P7C lo deja protegido con:

- `PhaserPedal feedback loop rejects DC accumulation under sustained bias`
- `PhaserPedal sanitizes NaN/Inf input under aggressive feedback`
- policy checks `p7c_phaser_feedback_dc_blocker`, `p7c_phaser_output_dc_blocker` y test-name detection.

## Stress tests agregados

Delay:

- `DelayPedal feedback loop rejects DC accumulation under sustained bias`
- `DelayPedal max feedback under sustained input stays bounded`
- `DelayPedal sanitizes NaN/Inf input under aggressive feedback`
- `DelayPedal high peak input under feedback stays bounded`

Flanger:

- `FlangerPedal feedback loop rejects DC accumulation under sustained bias`
- `FlangerPedal max feedback under sustained input stays bounded`
- `FlangerPedal sanitizes NaN/Inf input under aggressive feedback`
- `FlangerPedal high peak input under feedback stays bounded`

Reverb:

- `ReverbPedal loop rejects DC accumulation under sustained biased playing`
- `ReverbPedal max decay under sustained input stays bounded`
- `ReverbPedal sanitizes NaN/Inf input under aggressive feedback`
- `ReverbPedal high peak input under max decay stays bounded`

## Policy checks agregados

`scripts/check-audio-thread-policy.ps1` conserva los checks previos y agrega `p7c_*` para:

- no `.setSize()` en `processBlock` de los pedales P1 auditados.
- no `scratchBuffer.setSize` / `dryBuffer.setSize` en `processBlock`.
- no resize/assign/push_back en `processBlock` de esos pedales.
- guard `canProcessBlock(buffer)` y contador `fallbackBlockCount`.
- Neural helpers con `std::array`, sin `std::vector<float>`, sin `resize/assign`, sin `ensureChannels`.
- cabinets con `IIR::ArrayCoefficients<float>::make*` y sin `IIR::Coefficients<float>::make*`.
- Phaser DC blockers presentes.
- test names P7C presentes.

Resultado policy final: `failures=0`, `contractFailures=0`, `contractChecks=127`. El estado del scanner es `WARN` solo por 4 hallazgos legacy no contractuales ya clasificados.

## Validacion

- `NOVA_SharedCode` Debug x64: PASS, 0 warnings.
- `NOVA_SharedCode` Release x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Debug x64: PASS, 0 warnings.
- `NOVA_StandalonePlugin` Release x64: PASS, 0 warnings.
- `NOVA_VST3` Release x64: PASS, 0 warnings.
- Base validation Debug x64: PASS dos corridas consecutivas, `results=181 passes=6169 failures=0 failingResults=0`.
- Golden metrics: PASS contra `docs/golden-metrics/p4-offline-qa-baseline.json`; no baseline update.
- RT profile Release: PASS `16/16/0/0`.
- RT profile stability Release `-CiMode -Runs 3`: PASS, todos los escenarios `3/0/0`.
- Policy scan: `failures=0`, `contractFailures=0`, `contractChecks=127`.
- Wrapper Fast Release: PASS.

Nota: una primera corrida de base validation antes de repetir mostro un fallo transitorio en un test existente de routing; las dos corridas consecutivas requeridas y el wrapper Fast pasaron con `181/6169/0`.

## Pendientes reales

- `ReverbPedal::engine.configure` bajo automatizacion intensa sigue como P1/P2 de auditoria/perf; no se cambio en P7C para evitar revoicing o redisenar internals.
- Legacy/no registrados siguen fuera de este cierre: root `CompressorPedal.h`, root `ChorusPedal.h`, `AutoWahPedal.h`, `MetalDistortionPedal.h`. El policy los reporta como WARN legacy, no como contract failures.
- Visualizer/profiler CPU y tests de zipper/discontinuidad para otros pedales siguen P2.

## Riesgos restantes

- Los fallbacks seguros degradan a dry/no-op si el host excede `prepareToPlay`; eso protege RT safety, pero no intenta preservar el efecto bajo contrato roto.
- Los checks de policy son estaticos y por patrones; complementan, no reemplazan, validation runtime.
- Cabinets mantienen los mismos coeficientes nominales, pero cualquier ajuste futuro de curvas debe pasar por golden metrics sin update de baseline salvo aprobacion explicita.

## Recomendacion P7D

P7D deberia enfocarse en validacion de preset/session y DAW smoke tests, manteniendo fuera UI/UX y revoicing. Si se decide seguir hardening DSP, priorizar `ReverbPedal::engine.configure` automation/perf y la deuda legacy no registrada como una subfase separada.
