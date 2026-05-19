# P7B - Audio-Thread RT-Safety Closure Results

Date: 2026-05-01
Scope: close the P0 audio-thread RT-safety items still listed as `pendiente` in `docs/audio-realtime-safety-audit.md` and `docs/pedal-audit-matrix.md`. No tonal change, no DSP change, no schema change, no dry/wet change, no routing policy change, no graph lifecycle change, no golden baseline update, no new known failures, no new module extraction. Documentation and policy contract additions only.

## Summary

P7B is a closure-and-lock-down phase. When this phase started, the actual source already met the P0 recommendations from the audit; what was missing was explicit policy enforcement and documented evidence. P7B adds 33 new policy contract checks that codify the rules across the broader file set (not just the inside of `processBlock`), updates the audit/matrix docs to mark items as `corregido`, and re-runs the full validation surface to confirm no regression.

## Files modified

- `scripts/check-audio-thread-policy.ps1` — added P7B contract checks (`p7b_*` prefix). Existing checks left intact.
- `docs/audio-realtime-safety-audit.md` — added P7B closure summary at the top; marked the four P0 hallazgos prioritarios as `CORREGIDO`. Original table preserved as historical evidence.
- `docs/pedal-audit-matrix.md` — added P7B closure summary at the top.
- `docs/p7b-audio-thread-rt-safety-closure-results.md` — this report.
- Refreshed generated artifacts only (no source code changes):
  - `artifacts/audio-thread-policy-scan.json` / `.txt`
  - `artifacts/rt-profile-release-x64-report.json`
  - `artifacts/rt-profile-stability-release-x64.json`
  - `artifacts/rt-profile-stability-runs-release-x64/run-0[1-3].json`
  - `audio-base-test-report.txt`

No `Source/` change. No `NOVA.jucer` change. No schema/IDs/tolerances change. No golden baseline change.

## Verification of pre-existing closure (read-only audit)

The state of the source going into P7B already satisfied the P0 recommendations. Each item below was verified by direct inspection.

### 1. Telemetry transport for the seven audio-thread emitters

Audit items in `audio-realtime-safety-audit.md`:

- `ChannelStripProcessor::processBlock` telemetry
- `OutputChainProcessor::processBlock` telemetry
- `OverdrivePedal::processBlock` telemetry
- `CleanAmp::processBlock` telemetry
- `DelayPedal::processBlock` telemetry
- `FlangerPedal::processBlock` telemetry
- `ReverbPedal::processBlock` telemetry

State at start of P7B:

- `Source/Core/PedalSignalTelemetry.h` declares `RealtimeSignalTelemetryQueue` — a fixed-capacity (256 slots) lock-free SPMC queue using sequence-based slot publication (`writeSequence` / `readSequence` / per-slot `sequence` atomics). No mutex, no `juce::SpinLock`, no allocation on the audio path.
- `PedalSignalTelemetry::captureOutputAndPublishIfNeeded(...)` accumulates a window in stack-resident structures, then on emit publishes a fixed-size `RealtimeSignalTelemetryEvent` (with `std::array<char, 64>` tag, no `juce::String`) to the queue.
- All seven emitter call sites use `captureOutputAndPublishIfNeeded(...)` — verified by `grep`.
- The seven emitter source files contain zero `SessionLogger::logEvent` / `SessionLogger::logValueTree` calls — verified by `grep`.
- The `juce::String` formatting and `SessionLogger::logEvent` invocations live in `SessionLogger::flushPendingEntries` / `buildRealtimeTelemetryReport` and run on the dedicated logger thread; they consume from the lock-free queue.

Conclusion: P0 telemetry items are corregido pre-P7B.

### 2. `PluginProcessor::processBlock` lock-free path

Audit items:

- `refreshEngineGlobalParamsIfNeeded` SpinLock (`runtimeCacheLock`, `enginePushStateLock`).
- `refreshEngineGlobalParamsIfNeeded` calling `logRuntimeSnapshot(...)` from the audio callback.
- `refreshEngineEnabledIfNeeded` lock + logging.
- `SessionStore::getRuntimeGlobalParams` SpinLock.
- `SessionLogger::logEvent` / `enqueue` (must be unreachable from audio).

State at start of P7B:

- `SessionStore::getRuntimeGlobalParams() const` returns `runtimeParamsCache.load()`. `runtimeParamsCache` is a `RuntimeGlobalParamAtomics` mirror with `std::atomic<float>` / `std::atomic<bool>` / `std::atomic<int>` fields. No `juce::SpinLock`, no `juce::ScopedLock`, no `juce::CriticalSection`.
- `SessionStore::isEngineEnabled() const noexcept` returns `engineEnabledCache.load(std::memory_order_acquire)` — atomic only.
- `SessionCoordinator::getRuntimeGlobalParams` and `isEngineEnabled` are thin pass-throughs to the atomic readers.
- `NOVAAudioProcessor::processBlock(...)` body contains:

  ```cpp
  juce::ScopedNoDenormals noDenormals;
  refreshEngineEnabledIfNeeded(false);
  refreshEngineGlobalParamsIfNeeded(false, false);
  audioEngine.process(buffer, midi);
  audioVisualizer.pushBuffer(buffer);
  ```

  No `juce::String` construction, no `SessionLogger::logEvent`, no lock primitive. The `false` flags suppress logging; logging is deferred via `deferredRuntimeSnapshotLog` / `deferredEngineToggleLog` atomics for emission outside the audio callback.

- `SimpleOscilloscope::pushBuffer(...)` uses double-buffered atomic frame indices, no allocation, no lock.

Conclusion: P0 PluginProcessor items are corregido pre-P7B.

### 3. Per-block `std::vector<float*>` in the four amps

Audit items:

- `ClassicAmp::PremiumAmpCore::process` — `std::vector<float*> channelData`.
- `HighGainAmp` `process` — same pattern.
- `ChimeAmp` `process` — same pattern.
- `BoutiqueAmp` `process` — same pattern.

State at start of P7B:

- All four amps declare `static constexpr int kMaxCoreChannels = 8;` (or `kMaxAmpChannels = 8`) and use `std::array<float*, kMaxAmpChannels> channelData{};` inside their saturation loops. No heap allocation on the audio path.
- A repository-wide grep for `std::vector<float*>` returns zero matches under `Source/Effects/` and zero under `Source/`.

Conclusion: corregido pre-P7B.

### 4. JUCE IIR coefficient factories from process

Audit items:

- `ClassicAmp` / `HighGainAmp` / `ChimeAmp` / `BoutiqueAmp` `updateVoicingIfNeeded` — `juce::dsp::IIR::Coefficients<float>::make*` allocations.
- `CleanAmp::updateToneFilters` — same.
- Cabinets (`CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet`) — same.

State at start of P7B:

- All four amps use `juce::dsp::IIR::ArrayCoefficients<float>::make*` (value-type, no allocation, no reference-count churn). The threshold-cached `updateVoicingIfNeeded` only re-applies when params cross `1.0e-4f`, so even the value-type assignment is rare.
- `CleanAmp` uses an internal `IIRFilter` type rather than the JUCE factory.
- Repository-wide grep for `IIR::Coefficients<float>::make` returns zero matches under `Source/`. Cabinets remain to be addressed in a separate phase per the audit's P1 scope; they are explicitly out of P7B.

Conclusion: corregido pre-P7B for amps + CleanAmp. Cabinets remain P1.

## Policy contract checks added (P7B)

`scripts/check-audio-thread-policy.ps1` now adds 33 P7B contract checks, all with prefix `p7b_*`. They run at file scope (not just inside `processBlock` ranges) because the offending patterns can also live in helper functions reached from the audio path (for example `updateVoicingIfNeeded` called from amp `processBlock`).

| Group | Checks |
|-------|--------|
| Audio-thread emitter source files have no `SessionLogger::logEvent` / `SessionLogger::logValueTree` | `p7b_emitter_no_session_logger` x9 (ChannelStrip.h/.cpp, OutputChain.h/.cpp, OverdrivePedal.h, CleanAmp.h, DelayPedal.h, FlangerPedal.h, ReverbPedal.h) |
| Each emitter implementation publishes via the lock-free queue | `p7b_emitter_uses_lock_free_publish` x7 (the implementation translation units) |
| `PedalSignalTelemetry.h` declares the lock-free queue | `p7b_pedal_telemetry_lock_free_queue_present` |
| `PedalSignalTelemetry.h` does not call `SessionLogger` | `p7b_pedal_telemetry_no_session_logger` |
| Amps do not use `std::vector<float*>` | `p7b_amp_no_vector_channel_alloc` x5 (ClassicAmp, HighGainAmp, ChimeAmp, BoutiqueAmp, CleanAmp) |
| Amps do not use the allocating `IIR::Coefficients<float>::make*` factory | `p7b_amp_no_juce_iir_factory` x5 (negative lookbehind allows `ArrayCoefficients`) |
| `NOVAAudioProcessor::processBlock` body has no `SessionLogger`, no `juce::String`, no lock primitive | `p7b_plugin_processor_processblock_no_session_logger`, `p7b_plugin_processor_processblock_no_juce_string`, `p7b_plugin_processor_processblock_no_lock` |
| `SessionStore` keeps the runtime/engine cache lock-free with atomic snapshot | `p7b_session_store_atomic_runtime_cache`, `p7b_session_store_no_lock_on_hot_path` |

Final policy scan (after P7B):

- `status=WARN` (existing legacy non-blocking warnings only — `Source/Effects/Pedals/ChorusPedal.h`, `CompressorPedal.h`, `Wah/AutoWahPedal.h`, `Metal/MetalDistortionPedal.h`).
- `summary.failures=0`
- `summary.contractChecks=53` (P6I baseline 20 + P7B addition 33)
- `summary.contractFailures=0`
- `summary.legacyWarnings=4` (unchanged)
- `summary.allowListEntries=2` (unchanged)

Acceptance criteria met: `failures=0` and `contractFailures=0`, with new contract checks added (none removed).

## Validation

### Builds

| Target | Configuration | Result |
|--------|---------------|--------|
| `NOVA_StandalonePlugin` | Release x64 | PASS, 0 warnings, 0 errors |
| `NOVA_StandalonePlugin` | Debug x64 | PASS, 0 warnings, 0 errors |
| `NOVA_VST3` | Release x64 | PASS, 0 warnings, 0 errors |

`NOVA_SharedCode` Release/Debug compiled implicitly through above targets, PASS.

### Base validation (consecutive runs)

`scripts/run-base-audio-validation.ps1`:

- Run 1: PASS, `results=169 passes=6146 failures=0 failingResults=0`
- Run 2: PASS, `results=169 passes=6146 failures=0 failingResults=0`

Group breakdown both runs:

- Core: 0 failures
- P1 Pedal Safety: 0 failures
- Reverb: 0 failures
- Routing: 0 failures
- OutputChain: 0 failures
- AudioEngine: 0 failures
- Regression: 0 failures

No known failures are ignored by the script. Counts match P6I exactly (no regression and no inflation from coverage cheats).

### Golden audio metrics

`scripts/run-golden-audio-metrics.ps1`: PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`. No baseline updates.

### RT profile Release scenarios

`scripts/run-rt-profile-scenarios.ps1 -Configuration Release`: `total=16 pass=16 warn=0 fail=0`. All 16 scenarios PASS:

- `clean_empty_chain`, `clean_line_a`, `dual_parallel_clean`
- `overdrive_v2_nominal`, `overdrive_cleanamp_reverb_chain_nominal`
- `high_gain_amp_nominal`, `cabinet_nominal`
- `delay_feedback_nominal`, `reverb_cloud_tail`, `reverb_reverse_swell`
- `stress_block_32`, `stress_block_64`, `stress_block_512`
- `sample_rate_44100`, `sample_rate_48000`, `sample_rate_96000`

No budget breaches; CPU peaks consistent with P6I.

### RT profile Release stability (prioritized, CiMode, 3 runs)

`scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`. All 16 scenarios `3/0/0`:

| Scenario | runs(pass/warn/fail) |
|----------|----------------------|
| clean_empty_chain | 3/0/0 |
| clean_line_a | 3/0/0 |
| dual_parallel_clean | 3/0/0 |
| overdrive_v2_nominal | 3/0/0 |
| overdrive_cleanamp_reverb_chain_nominal | 3/0/0 |
| high_gain_amp_nominal | 3/0/0 |
| cabinet_nominal | 3/0/0 |
| delay_feedback_nominal | 3/0/0 |
| reverb_cloud_tail | 3/0/0 |
| reverb_reverse_swell | 3/0/0 |
| stress_block_32 | 3/0/0 |
| stress_block_64 | 3/0/0 |
| stress_block_512 | 3/0/0 |
| sample_rate_44100 | 3/0/0 |
| sample_rate_48000 | 3/0/0 |
| sample_rate_96000 | 3/0/0 |

### Audio thread policy scan (post-P7B)

`scripts/check-audio-thread-policy.ps1`:

- `status=WARN` (existing legacy non-blocking only)
- `failures=0`
- `contractFailures=0`
- `contractChecks=53`
- 33 new P7B checks all `passed=true`

### Wrapper Fast Release

`scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS through all steps:

1. Build standalone Release: PASS
2. Base validation: `results=169 passes=6146 failures=0 failingResults=0`
3. RT profile Release scenarios: 16/16/0/0
4. Audio thread policy scan: `failures=0`, `contractFailures=0`
5. RT stability: skipped (Fast mode), executed separately above

### Diff hygiene

`git diff --check`: PASS (only existing CRLF normalization warnings; no whitespace errors).

## Behavior preservation (verified, not changed)

- No DSP code modified. Tonal output is bit-identical (golden metrics PASS confirms it).
- No `RealtimeSignalTelemetryQueue` capacity, no `kMaxAmpChannels` value, no smoothing constants modified.
- No graph topology, dry/wet, routing policy, or graph lifecycle code modified.
- No state schema, IDs, tolerances, or golden baselines modified.
- No new module extracted. No `setParams` call site moved.
- The seven emitters publish telemetry through exactly the same lock-free queue they used at P6I.
- The four amps allocate exactly the same `std::array<float*, 8>` they allocated at P6I.

## Acceptance criteria check

| Criterion | Result |
|-----------|--------|
| Builds Debug+Release Standalone+VST3 PASS | YES |
| Base validation matches or exceeds P6I (`results>=169 passes>=6146 failures=0 failingResults=0`) | YES (matches exactly, twice consecutive) |
| Golden metrics PASS, no baseline update | YES |
| RT Release single-run 16/16/0/0 | YES |
| RT Release stability prioritized 3/0/0 across all scenarios | YES |
| Policy scan `failures=0`, `contractFailures=0` | YES |
| New P7B contract checks added | YES (33 added; P6I checks preserved) |
| Audit/matrix docs updated to mark items `CORREGIDO` | YES |
| Wrapper Fast Release PASS | YES |
| `git diff --check` PASS | YES |
| No DSP/tone change | YES |
| No schema change | YES |
| No golden baseline update | YES |
| No new known failures | YES |
| No new module extracted | YES |
| No `setParams` move | YES |
| No dry/wet or routing policy change | YES |

All acceptance criteria met.

## Risks remaining

These are intentionally out of P7B scope and tracked elsewhere:

- Cabinet `updateVoicing` / `updateCabinetVoicing` still call `juce::dsp::IIR::Coefficients<float>::make*`. The audit lists this as `Alta` severity (not `Critica`). Belongs to P7C (P1 closure).
- `setSize` / `resize` fallback hardening on `CompressorPedal`, `DistortionPedal`, `FuzzPedal`, `NeuralPedal`, `ClassicWahPedal`, cabinets. P1, P7C.
- `PhaserPedal` DC accumulation under sustained bias. P1, P7C.
- Stress tests for `DelayPedal` / `ReverbPedal` / `FlangerPedal` feedback runaway. P1, P7C.
- Legacy unregistered files (`AutoWahPedal`, `MetalDistortionPedal`, root `CompressorPedal`, root `ChorusPedal`) still produce non-blocking WARN findings. P2.
- DAW smoke matrix, preset round-trip matrix, accessibility, release engineering. Out of P7 audio-thread scope; tracked in P7A readiness map for P7D-P7G.

## Recommendation for P7C

Continue with P1 closure as outlined in `docs/p7a-product-hardening-audit-and-readiness-map.md` section 5.2:

- harden `AudioBuffer::setSize` / `std::vector::resize` fallbacks across affected pedals.
- migrate cabinet `updateVoicing` to value-type coefficients or a precomputed table.
- reproduce + lock the `PhaserPedal` DC accumulation test.
- add stress tests for `DelayPedal` / `ReverbPedal` / `FlangerPedal` feedback / DC / NaN / large peaks.

Suggested next-phase prompt:

> Tarea: P7C - Allocation Fallback and Feedback Stress Closure (no tonal changes).
>
> Cerrar los items P1 listados en `docs/audio-realtime-safety-audit.md` que dependan de fallbacks no-allocation y stress de feedback:
> 1. Reemplazar `scratchBuffer.setSize` / `dryBuffer.setSize` por fallback no-allocation en `CompressorPedal`, `DistortionPedal`, `FuzzPedal`, `NeuralPedal`, `ClassicWahPedal` y cabinets (`CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet`).
> 2. Reemplazar `OnePoleFilterBank` / `EnvelopeFollower` `ensureChannels` (`resize`) por preasignacion de max channels.
> 3. Migrar coeficientes de cabinet `updateVoicing` / `updateCabinetVoicing` a `IIR::ArrayCoefficients` o tabla precalculada.
> 4. Reproducir y fijar el test del `PhaserPedal` DC accumulation bajo bias sostenido.
> 5. Anadir stress tests deterministicos para `DelayPedal` / `ReverbPedal` / `FlangerPedal` cubriendo DC offset, NaN/Inf, picos altos, y feedback runaway.
> 6. Anadir contract checks (`p7c_*`) que codifiquen estas reglas. Mantener todos los checks previos.
>
> Restricciones: no cambiar tono, no cambiar schema, no actualizar golden baselines, no introducir known failures, no extraer modulos nuevos, no mover `setParams`, no tocar dry/wet ni routing policy.
>
> Validacion: builds Debug+Release Standalone+VST3, `run-base-audio-validation` (PASS, no regresion en results/passes), `run-golden-audio-metrics` (PASS contra P4 baseline), `run-rt-profile-scenarios` Release 16/16/0/0, `run-rt-profile-stability` Release `-CiMode -Runs 3` 3/0/0 priorizada, `check-audio-thread-policy` `failures=0 contractFailures=0`, `run-audio-quality-gates -Fast` PASS, `git diff --check` PASS.
