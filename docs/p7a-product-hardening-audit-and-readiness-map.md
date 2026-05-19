# P7A - Product Hardening Audit & Readiness Map

Date: 2026-05-01
Scope: documentation-only audit of NOVA's current hardening state at the close of P6, and a readiness map for what remains before the product can move into final tonal QA, artistic preset curation, and brand work. No code, no DSP behavior change, no schema change, no golden baseline update, no known-failure addition.

This document is a snapshot. It does not extract modules, change runtime behavior, change tests, or change validation tolerances. It defines the surface that P7B and the rest of the P7 stage will operate against.

## 1. Executive Summary

P6 closed in a stable, fully green state:

- `DryWetMixer` post-migration stable (`P6F`) and exposed through direct unit coverage.
- `RoutingMixer` extracted as policy-only header (`P6H`), with `GraphBuilder` retaining strip `setParams` ownership.
- `P6I` re-validated the entire surface: builds clean, base validation `results=169 passes=6146 failures=0 failingResults=0`, golden metrics match the P4 baseline, RT Release `16/16/0/0`, RT Release Stability `3/0/0` across all 16 scenarios, policy scan `failures=0 contractFailures=0 contractChecks=20`, wrapper Fast Release PASS.

Structurally, NOVA is in the strongest position it has been in since the audit baseline. The remaining product-hardening work is largely orthogonal to further refactor: it concerns RT-safety closure on the pedal/amp/cab tier, validation breadth, release engineering, and external confidence (DAW smoke). Subjective tonal adjustments, final artistic preset curation, and brand redesign are explicitly out of P7 scope and carried forward to P8/P9/P10.

The recommended P7B is a closure pass on the highest-severity items still open from `docs/audio-realtime-safety-audit.md` and `docs/pedal-audit-matrix.md` - specifically, P0 telemetry transport and P0 amp allocation/coefficient hygiene - because those items remain the most credible source of real-world glitches under stress and are still listed as `pendiente` in the audit matrix.

## 2. Confirmed Stable Baseline (entering P7)

The protective layer at the start of P7:

- Build matrix: `NOVA_StandalonePlugin` Debug+Release x64 PASS; `NOVA_VST3` Release x64 PASS; `NOVA_SharedCode` PASS in both configurations.
- Base validation: `results=169 passes=6146 failures=0 failingResults=0`, two consecutive runs (P6I).
- Golden audio metrics: PASS against `docs/golden-metrics/p4-offline-qa-baseline.json`.
- RT profile Release single-run: `total=16 pass=16 warn=0 fail=0`.
- RT profile Release stability prioritized (`-CiMode -Runs 3`): `3/0/0` for all 16 scenarios.
- Audio thread policy scan: `status=WARN` (legacy non-blocking only), `failures=0`, `contractFailures=0`, `contractChecks=20`.
- Wrapper Fast Release gate: PASS.

No new known failures introduced. No golden baseline update applied. No schema change since P3. No graph lifecycle change since P5J. No DSP code touched in P6F-P6I.

## 3. Architecture State at End of P6

### 3.1 Modules already extracted (P5+P6)

These are stable and have direct or contract-level coverage. They are not the focus of P7:

| Module | Header | Owner of |
|--------|--------|----------|
| `CpuMeter` | `Source/Core/Audio/CpuMeter.h` | per-block timing, smoothed CPU telemetry |
| `HealthMonitor` | `Source/Core/Audio/HealthMonitor.h` | output sanitization, recovery action emission |
| `RuntimeParameterSnapshot` | `Source/Core/Audio/RuntimeParameterSnapshot.h` | atomic snapshot, revision tracking, normalized output mix |
| `AudioEngineCommandQueue` | `Source/Core/Audio/AudioEngineCommandQueue.h` | enqueue/drain pending graph commands |
| `GraphCommandApplier` | `Source/Core/Audio/GraphCommandApplier.h` | command-to-model mutation, result flags |
| `GraphRetirementQueue` | `Source/Core/Audio/GraphRetirementQueue.h` | retired graph lifetime, bounded cleanup |
| `RuntimeGraphManager` | `Source/Core/Audio/RuntimeGraphManager.h` | active graph ownership, raw publication |
| `DiagnosticsManager` | `Source/Core/Audio/DiagnosticsManager.h` | diagnostic/profiling string formatting |
| `GraphBuilder` (+ `GraphRuntimeTypes`) | `Source/Core/Audio/GraphBuilder.h`, `Source/Core/Audio/GraphRuntimeTypes.h` | runtime graph construction, zone ordering, runtime param apply |
| `DryWetMixer` | `Source/Core/Audio/DryWetMixer.h` | mix/ramp, scratch, dry-delay storage, latency clamp |
| `RoutingMixer` | `Source/Core/Audio/RoutingMixer.h` | line target policy (gain/pan/width, mute, dual-parallel comp) |

### 3.2 What still lives in `AudioEngine`

Per `P5P` snapshot, still concentrated in `AudioEngine`:

- top-level `process` orchestration and decimation decisions
- sample-accurate dry/wet pipeline orchestration (`processWithSampleAccurateDryWet`, `mixWetDrySampleAccurate`, `copyDryThroughLatency`, dry-delay buffer lifecycle)
- in-place pedal bypass orchestration (`applyPedalBypassToActiveGraph`)
- queue-drain orchestration (`flushPendingGraphCommands`)
- `buildGraphFromModelLocked` thin wrapper over `GraphBuilder`
- `publishGraph` thin wrapper over `RuntimeGraphManager`
- tuner flow (`setTunerEnabled`, push/process/readback, reference pitch)
- async/run-thread orchestration and synchronize points
- public surface for the plugin/session bridge

These are deliberately not P7 targets: they are orchestration layers, and further extraction without behavior coverage would be risk-positive without a proportional product gain.

### 3.3 Protected invariants entering P7

Carried from `P5P` and reinforced in `P6F`/`P6H`/`P6I`:

- no graph build in audio thread
- audio thread reads active graph via atomic `getActiveRaw()` only
- no `shared_ptr` copy/owner lock in `AudioEngine::process`
- `GraphBuilder` not invoked from `process`
- sample-accurate dry/wet behavior unchanged across P6
- canonical zone order `Pre -> Amp -> FX -> Cabinet` enforced by `GraphBuilder` and tests
- latency capture point post-rebuild
- `ProcessorBase`/`TempoSyncable` caches preserved
- runtime param application order preserved
- graph publish/retire contract preserved (swap owner -> publish raw -> latency -> retire old, +8 block grace)
- `RoutingMixer` is policy-only and does not touch `GraphRuntime`, `processBlock`, `SessionLogger`, or `juce::String`
- `DryWetMixer` is header-only state owner; orchestration calls remain in `AudioEngine`
- `GraphBuilder::applyRuntimeParamsToGraph` remains the strip `setParams` call site

## 4. Validation Surface Inventory

The hardening map below assumes these gates exist and are blocking in the right places. This inventory is descriptive, not normative.

| Gate | Script / Doc | Acceptance |
|------|--------------|------------|
| Build (Debug+Release, Standalone+VST3) | `scripts/build-nova.ps1` | 0 warnings, 0 errors |
| Base validation | `scripts/run-base-audio-validation.ps1` | failures=0, failingResults=0 |
| Golden audio metrics | `scripts/run-golden-audio-metrics.ps1` vs `docs/golden-metrics/p4-offline-qa-baseline.json` | PASS, no baseline updates |
| RT profile Release single-run | `scripts/run-rt-profile-scenarios.ps1 -Configuration Release` | 16/16/0/0 |
| RT profile Release stability | `scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3` | 3/0/0 across scenarios |
| Audio thread policy scan | `scripts/check-audio-thread-policy.ps1` | failures=0, contractFailures=0 |
| Wrapper fast gate | `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release` | PASS |
| Diff hygiene | `git diff --check` | only legacy CRLF warnings tolerated |

Quality gate intent levels are codified in `docs/p5k-audio-quality-gates.md` (Fast / Pre-Commit / Nightly / Release Candidate). Stability methodology in `docs/rt-profile-stability-guide.md`.

What this surface intentionally does not yet cover:

- external manual DAW smoke (Reaper / Logic / Live / Cubase / Pro Tools)
- multi-host parameter automation determinism
- preset compatibility regression test (round-trip across schema versions)
- crash-only and power-loss recovery audit (state file integrity)
- localization / character set audit on preset names and paths
- accessibility audit on the editor (keyboard nav, contrast, screen reader hints)
- packaging/codesigning/notarization pipeline
- CPU envelope on low-spec target machines (laptops, ARM under emulation)

These are the candidate slots inside the P7 stage after the immediate audio-thread closure work.

## 5. Hardening Backlog (Audio Thread)

Source of truth: `docs/audio-realtime-safety-audit.md` (P0/P1/P2 list) and `docs/pedal-audit-matrix.md`. Items below are summarized; the audit docs remain authoritative.

### 5.1 P0 - audio-thread RT-safety, still listed `pendiente`

These remain the highest-leverage items: they are credible glitch sources under real DAW use even though the current synthetic suite is green.

1. Telemetry/logging from `processBlock` in:
   - `ChannelStripProcessor`
   - `OutputChainProcessor`
   - `OverdrivePedal`
   - `CleanAmp`
   - `DelayPedal`
   - `FlangerPedal`
   - `ReverbPedal`

   Risk: `juce::String` construction, `SessionLogger::logEvent`, `juce::SpinLock` indirection from the audio thread.
   Recommendation (audit): split RT-safe collect (atomic counters / preallocated ring buffer, no strings) from non-audio format/log.

2. Locks and logging on the `PluginProcessor::processBlock` path through:
   - `refreshEngineGlobalParamsIfNeeded` (uses `runtimeCacheLock`, `enginePushStateLock`, may call `logRuntimeSnapshot`)
   - `refreshEngineEnabledIfNeeded`
   - `SessionStore::getRuntimeGlobalParams` (`juce::SpinLock::ScopedLockType`)
   - `SessionLogger::logEvent` / `writeStructured` / `enqueue`

   Risk: priority inversion, audio-thread blocking under contention.
   Recommendation (audit): atomic snapshot / lock-free double-buffer for runtime globals; logger is never called from audio.

3. `std::vector<float*>` allocation per block in:
   - `ClassicAmp` / `PremiumAmpCore`
   - `HighGainAmp`
   - `ChimeAmp`
   - `BoutiqueAmp`

   Recommendation (audit): `std::array<float*, maxChannels>` or scratch preallocated in `prepareToPlay`.

4. JUCE IIR coefficient factories called from `processBlock`:
   - amps: `ClassicAmp`, `HighGainAmp`, `ChimeAmp`, `BoutiqueAmp` `updateVoicingIfNeeded`
   - `CleanAmp::updateToneFilters`
   - cabinets: `CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet` `updateVoicing` / `updateCabinetVoicing`

   Risk: allocation, ref-count churn, click/zipper on parameter change.
   Recommendation (audit): precomputed tables or custom value-type smoothed coefficients.

### 5.2 P1 - allocation under contract violation

`AudioBuffer::setSize` / `std::vector::resize` paths that only allocate if a host violates the prepared contract, but should still degrade gracefully:

- `CompressorPedal::dryBuffer.setSize`
- `DistortionPedal::scratchBuffer.setSize`
- `FuzzPedal::scratchBuffer.setSize`
- `NeuralPedal::scratchBuffer.setSize`
- `NeuralPedal` `OnePoleFilterBank` / `EnvelopeFollower` `ensureChannels` (`resize`)
- `ClassicWahPedal::scratchBuffer.setSize`
- `CabinetPedal` / `Vintage2x12Cabinet` / `Modern4x12Cabinet` `scratchBuffer.setSize`

Recommendation (audit): preallocate against `MaxChannels` x `MaxBlock` at prepare; use no-allocation fallback (process in slices) on overflow.

Behavioral P1 items also open from the audit:

- reproduce / lock the `PhaserPedal` DC accumulation under sustained bias.
- stress tests on `DelayPedal` / `ReverbPedal` / `FlangerPedal` for DC, NaN/Inf, large peaks, and feedback runaway.

### 5.3 P2 - quality of life and discipline

- zipper / discontinuity tests for EQ, Tremolo, Wah, modulations.
- visualizer / per-sample atomic CPU profiling at block 32 / 64.
- decision on legacy unregistered files: `AutoWahPedal`, `MetalDistortionPedal`, root `CompressorPedal`, root `ChorusPedal` - either modernize or remove from `NOVA.jucer`.

### 5.4 What is already corrected

Per the audit's `Estado` column:

- `AudioEngine::handleHealthAfterBlock` (atomic-flag pattern only; rebuild/log outside audio).
- `ProcessorBase::beginBypassProcess` no-allocation overflow fallback.

These should not regress: they belong on the protected-invariants list.

## 6. Hardening Backlog (Non-Audio)

These are real product-readiness items not covered by the current synthetic gates. They are not subjective tonal work and not artistic curation - they are engineering closure.

### 6.1 Preset / schema robustness

- Preset round-trip regression test: save -> load -> save -> diff for every catalog pedal in every zone, in both single-line and dual-parallel topologies.
- Schema upgrade matrix: load presets stamped with each historical `STATE_SCHEMA_VERSION` and confirm canonicalization without silent loss.
- Pedal-state Base64 robustness: corrupted / truncated payloads, oversized payloads, missing zones, duplicate amps, duplicate cabinets, and zones at >`MAX_PEDALS_PER_FLEX_ZONE`.
- Recovery test: corrupt user preset directory and confirm engine still boots with empty session, with a diagnostic record (no crash, no hang).

### 6.2 Host integration confidence

- DAW smoke matrix: at minimum Reaper, Ableton Live, Logic Pro, Cubase. Each should drive load -> instantiate -> automation sweep on every catalog parameter -> save host project -> reopen.
- Sample rate / block size matrix: 44.1k, 48k, 88.2k, 96k against blocks 32, 64, 128, 256, 480, 512.
- Bus layout matrix: stereo in/out (canonical), mono-in/stereo-out, host bypass on/off, latency reporting accuracy across rebuilds.
- Multi-instance: confirm two NOVA instances on parallel tracks do not contend on shared state.

### 6.3 Editor / UX hardening

- Drag-and-drop indexing fuzz: verify chain ordering matches `PluginStateModel` zone ordering after rapid drag sequences (intra-chain move, inter-chain move, drop on disabled targets, drop during rebuild).
- Modal close discipline: pedal editor overlay close paths (button, ESC, scrim click) must always release focus and not leak `LookAndFeel` references.
- Asset browser overlay: confirm search + drag still respects zone rules (`PedalCatalog::enforceZone`).
- Wizard flows: `StartWizard`, `AudioSetupWizard`, `PresetFinderWizard` cover skip/back/next without state corruption.
- Accessibility pass: focus order, contrast, large text, keyboard-only navigation.

### 6.4 Release engineering

- Codesign + notarization pipeline (macOS) and signing (Windows) - placeholder, not in repo.
- Installer / package manifests (VST3 path, Standalone path) - not yet defined.
- Crash reporting hook (out of process minidump) - not yet defined.
- Telemetry consent and privacy posture - not yet defined.
- Versioning / changelog discipline tied to `STATE_SCHEMA_VERSION`.

### 6.5 Documentation and operational confidence

- Runbook for diagnostics: how to interpret `DiagnosticsManager` output, how to capture an `RT-profile` artifact for a user-reported glitch.
- Triage matrix: which gate caught a regression, what to roll back, which files are sensitive.
- `docs/active-work.md` should reflect the P7 cursor, not the MVP base-validation cursor it currently holds.

## 7. Out of Scope for P7

These are explicitly deferred and must not be entered during the P7 stage:

- Subjective tonal final adjustments per pedal/amp/cab. They depend on hardening being closed first (otherwise tonal A/B is unreliable). Owned by P8.
- Final artistic preset curation (factory bank, signature presets). Owned by P9.
- Brand redesign (logo, color system, marketing site, packaging assets, store creatives). Owned by P10.

A P7 task that proposes touching one of those areas should be rejected and reopened under the corresponding later phase.

## 8. Risk Map

| Area | Likelihood of latent issue | Blast radius | Notes |
|------|----------------------------|--------------|-------|
| Telemetry from audio thread | High | Medium-High (audible glitches under load, hard to repro) | P0, audit-listed |
| Per-block `std::vector` in amps | Medium | Medium (DAW dependent) | P0, audit-listed |
| JUCE IIR factories in amp/cab process | Medium | Medium | P0, audit-listed |
| `setSize` fallbacks across pedals | Low under correct prepare | High if a host violates contract | P1, audit-listed |
| Phaser DC accumulation | Confirmed report | Medium | P1, audit-listed |
| Preset round-trip across catalog | Unknown - no automated coverage today | High (silent corruption) | non-audio backlog |
| DAW host integration | Unknown - no external smoke today | High (release blocker class) | non-audio backlog |
| Drag-and-drop indexing under stress | Low - paths are tested - but no fuzz coverage | Medium (visible UX issue) | non-audio backlog |
| Release engineering (signing, installer, crash reporter) | Unknown - not in repo | High at ship time | non-audio backlog |

The High x Medium-High intersection is dominated by audio-thread telemetry and amp allocation, which is why P7B targets them first.

## 9. Recommendation - First Implementable Phase (P7B)

P7B should be a closure pass on the most credible glitch source still listed as `pendiente` in `docs/audio-realtime-safety-audit.md` without changing tone:

- Lock `processBlock` audio-thread paths against `juce::String` construction, `SessionLogger::logEvent`, `juce::SpinLock` acquisition, and `dynamic_cast`.
- Land an RT-safe telemetry transport (atomic counters / preallocated ring buffer) and re-route the seven existing emitters (`ChannelStripProcessor`, `OutputChainProcessor`, `OverdrivePedal`, `CleanAmp`, `DelayPedal`, `FlangerPedal`, `ReverbPedal`) onto it. The non-audio side keeps full diagnostic content (string formatting, file I/O) in the control thread.
- Replace per-block `std::vector<float*> channelData` allocations in `ClassicAmp`, `HighGainAmp`, `ChimeAmp`, `BoutiqueAmp` with `std::array` or scratch preallocated in `prepareToPlay`.
- Reinforce policy scan with new contract checks codifying these rules so they cannot regress.

Acceptance for P7B (proposed):

- builds clean Debug + Release, Standalone + VST3.
- base validation matches or exceeds P6I (`results>=169 passes>=6146 failures=0 failingResults=0`).
- golden metrics PASS against the P4 baseline; no baseline update.
- RT Release single-run 16/16/0/0; RT Release stability 3/0/0 prioritized.
- policy scan `failures=0`, `contractFailures=0`, with new contract checks added (no removals).
- audit-pendiente count for the items above moves from `pendiente` to `corregido` with a documented date.
- no DSP tone change. No schema change. No golden baseline update. No new known failures.

### 9.1 Suggested next-phase prompt

> Tarea: P7B - Audio-Thread RT-Safety Closure (no tonal changes).
>
> Cerrar los items P0 todavia listados como `pendiente` en `docs/audio-realtime-safety-audit.md`:
> 1. Eliminar `juce::String` / `SessionLogger::logEvent` / `juce::SpinLock` / `dynamic_cast` desde rutas de `processBlock` y desde `PluginProcessor::processBlock`.
> 2. Introducir transporte RT-safe de telemetry (counters atomicos / ring buffer preasignado) y mover a el las emisiones de `ChannelStripProcessor`, `OutputChainProcessor`, `OverdrivePedal`, `CleanAmp`, `DelayPedal`, `FlangerPedal`, `ReverbPedal`. El formateo y file I/O queda fuera del audio thread.
> 3. Reemplazar `std::vector<float*>` por bloque en `ClassicAmp`, `HighGainAmp`, `ChimeAmp`, `BoutiqueAmp` por `std::array` o scratch preasignado en `prepareToPlay`.
> 4. Anadir contract checks en `scripts/check-audio-thread-policy.ps1` que codifiquen las reglas anteriores. Mantener todos los checks previos.
>
> Restricciones: no cambiar tono, no cambiar schema, no actualizar golden baselines, no introducir known failures, no extraer modulos nuevos, no mover `setParams`, no tocar dry/wet ni routing policy.
>
> Validacion: builds Debug+Release Standalone+VST3, `run-base-audio-validation` (PASS, no regresion en results/passes), `run-golden-audio-metrics` (PASS contra P4 baseline), `run-rt-profile-scenarios` Release 16/16/0/0, `run-rt-profile-stability` Release `-CiMode -Runs 3` 3/0/0 priorizada, `check-audio-thread-policy` `failures=0 contractFailures=0`, `run-audio-quality-gates -Fast` PASS, `git diff --check` PASS.

### 9.2 Phases after P7B (sketch only, not committed)

- P7C: P1 closure - `setSize` fallbacks across pedals; `PhaserPedal` DC fix; stress tests for `DelayPedal` / `ReverbPedal` / `FlangerPedal`.
- P7D: preset round-trip and schema upgrade matrix.
- P7E: DAW smoke matrix and host integration confidence.
- P7F: drag-and-drop indexing fuzz, modal close discipline, accessibility pass.
- P7G: release engineering scaffolding (signing, installer, crash reporter, versioning).

P8 enters subjective tonal QA only after P7B-P7C are closed. P9 enters factory preset curation only after the preset round-trip matrix is green. P10 (brand) is independent of engineering closure but must not block on anything earlier.

## 10. Acceptance Criteria for P7A

| Criterion | Result |
|-----------|--------|
| Documentation only | YES |
| No source code change | YES |
| No `Source/` change | YES |
| No `scripts/` change | YES |
| No `NOVA.jucer` change | YES |
| No schema / IDs / tolerances change | YES |
| No golden baseline update | YES |
| No known failures introduced | YES |
| Audit honors P5P+P6I as the entering snapshot | YES |
| First implementable next phase (P7B) recommended | YES |
| Suggested next-phase prompt provided | YES |
| `git diff --check` PASS | to be confirmed at commit time |

P7A is a planning artifact only. Its value is fully realized when P7B starts from this readiness map without rediscovering scope.
