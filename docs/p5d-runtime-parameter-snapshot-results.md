# P5D RuntimeParameterSnapshot Results

Date: 2026-04-29

## Summary

P5D extracted the AudioEngine runtime-parameter atomics and output-mix normalization bridge into a dedicated header-only module:

- `Source/Core/Audio/RuntimeParameterSnapshot.h`

The extraction is intentionally small. `AudioEngine` still owns graph/runtime application, graph build/swap, dry/wet smoothing, routing, health monitoring, CPU metering, and diagnostics formatting. No parameter IDs, preset schema, host notification calls, graph routing, dry/wet behavior, DSP, tone, latency, golden baselines, or known-failure lists were changed.

The module is header-only to avoid touching `NOVA.jucer` or generated Visual Studio project files.

## Files Modified

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/RuntimeParameterSnapshot.h`
- `scripts/check-audio-thread-policy.ps1`
- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`
- `artifacts/p4-offline-qa-report.txt`
- `artifacts/rt-profile-debug-x64-report-p5d.json`
- `artifacts/rt-profile-release-x64-report-p5d.json`

Existing P5A/P5B/P5C files remain in the working tree:

- `Source/Core/Audio/CpuMeter.h`
- `Source/Core/Audio/HealthMonitor.h`
- `docs/p5a-audioengine-refactor-plan.md`
- `docs/p5b-cpumeter-extraction-results.md`
- `docs/p5c-healthmonitor-extraction-results.md`
- prior P5B/P5C RT profile artifacts

## Module Created

Created `Source/Core/Audio/RuntimeParameterSnapshot.h`.

Public surface:

- `Nova::Audio::RuntimeGlobalParamsSnapshot`
- `Nova::Audio::RuntimeParameterSnapshot`
- `reset() noexcept`
- `store(const RuntimeGlobalParamsSnapshot&) noexcept`
- `load() const noexcept`
- `getRevision() const noexcept`
- `getOutputMixNormalized() const noexcept`

`AudioEngine::RuntimeGlobalParams` is now a type alias to `Nova::Audio::RuntimeGlobalParamsSnapshot`, preserving the public API spelling used by `PluginProcessor`, `SessionStore`, and `SessionCoordinator`.

## State Moved

Moved out of `AudioEngine` into `RuntimeParameterSnapshot`:

- `inputGainDb`
- `gateThresholdDb`
- `forceMono`
- `hostTempoBpm`
- `hostTempoValid`
- `hostTransportPlaying`
- `outputVolumeDb`
- `outputLimiterDb`
- output mix normalized atomic storage
- `switchMode`
- `gainA`, `panA`, `widthA`
- `gainB`, `panB`, `widthB`
- runtime parameter `revision`

The engine-facing storage still keeps output mix as normalized `0..1` internally, matching the previous `AudioEngine::RuntimeParameterAtomics` behavior.

## Logic Moved

Moved out of `AudioEngine`:

- runtime parameter atomic `store(...)`
- runtime parameter atomic `load()`
- revision increment on store
- revision acquire-load helper
- output mix normalized getter
- `outputMixRaw -> outputMixNormalized` conversion helper

Preserved memory ordering:

- runtime parameter values use `std::memory_order_relaxed`
- `revision.fetch_add(1, std::memory_order_release)`
- `getRevision()` uses `std::memory_order_acquire`

No locks, allocations, `juce::String`, `SessionLogger`, host notifications, or `ValueTree` access were introduced in the new module.

## Logic Kept In AudioEngine

Kept in `AudioEngine`:

- `updateGlobalParams(...)` entrypoints
- settings/line `ValueTree` snapshot construction
- `applyRuntimeParamsToGraph(...)`
- graph revision comparison and application timing
- graph build/swap/retirement
- dry/wet ramp and sample-accurate mix
- routing and LineA/LineB/Dual behavior
- health monitor integration
- CPU meter integration
- diagnostic report formatting

`AudioEngine::loadRuntimeParams()` remains a thin wrapper over the new module to preserve the existing private API shape.

## PluginProcessor And SessionStore

Left unchanged by design:

- `NOVAAudioProcessor::RuntimeGlobalParamAtomics`
- `SessionStore::RuntimeGlobalParamAtomics`
- host parameter bindings
- `setValueNotifyingHost(...)` behavior during state restore
- parameter ID and schema ownership
- state-to-binding and binding-to-state sync

These caches store raw runtime values with their existing acquire/release memory ordering. They were not unified with the engine-side normalized snapshot because that would broaden the phase and could change raw `outputMix` values observed by logging/diff code for legacy `0..1` states, even if audio output stayed equivalent.

## Host Transport Polling

Host transport polling remains in `PluginProcessor`:

- `kHostTransportPollIntervalBlocks = 8`
- `hostTransportPollCounter` still advances in `refreshEngineGlobalParamsIfNeeded(false, false)`
- intermediate blocks reuse the last runtime snapshot transport values
- `force=true` still refreshes host transport immediately
- `prepareToPlay()` still resets the poll counter and calls `refreshEngineGlobalParamsIfNeeded(true)`

No host transport polling semantics were changed by P5D.

## Output Mix Behavior

The existing bridge was preserved:

- raw values `<= 1.0f` are treated as normalized values and clamped to `0..1`
- raw values `> 1.0f` are clamped to `0..100` and divided by `100`
- `getOutputMixNormalized()` returns the normalized atomic directly
- `load()` reconstructs `outputMixRaw` as `outputMixNormalized * 100.0f`, matching the old `AudioEngine::RuntimeParameterAtomics::load()`

Dry/wet smoothing remains in `AudioEngine::SampleAccurateRamp`; only the source of the normalized target changed from direct atomic access to `params.getOutputMixNormalized()`.

## Revision And Apply Semantics

Revision behavior is unchanged:

- every `store(...)` increments the runtime parameter revision
- graph application still compares `runtime.appliedParamRevision` against the current revision
- `applyRuntimeParamsToGraph(...)` still returns early when the revision already matches
- active graphs are still updated from the control thread/async paths, not by rebuilding from the audio callback
- graph build still applies the current snapshot and revision before `prepareToPlay()`/`rebuild()`

No changes were made to when params are considered changed for graph application.

## Validation Commands

Executed:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_SharedCode
powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_StandalonePlugin
git diff --check
powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120
powershell -ExecutionPolicy Bypass -File scripts\run-golden-audio-metrics.ps1
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240 -ReportPath artifacts\rt-profile-debug-x64-report-p5d.json
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240 -BaselinePath docs\rt-profile\p4c-rt-profile-release-baseline.json -ReportPath artifacts\rt-profile-release-x64-report-p5d.json
powershell -ExecutionPolicy Bypass -File scripts\check-audio-thread-policy.ps1
```

## Validation Results

Builds:

- `NOVA_SharedCode Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Debug x64`: PASS, 0 warnings, 0 errors.
- `NOVA_SharedCode Release x64`: PASS, 0 warnings, 0 errors.
- `NOVA_StandalonePlugin Release x64`: PASS, 0 warnings, 0 errors.

Static checks:

- `git diff --check`: PASS. Git reported LF-to-CRLF working-copy warnings only.
- `check-audio-thread-policy.ps1`: WARN, non-blocking.
- Policy scan active routes: `activeFiles=32`, `activeRanges=38`, `failures=0`.
- Policy scan warnings: 4 legacy warnings, unchanged class of issue from P4C/P5C.

Base validation:

- `run-base-audio-validation.ps1`: PASS.
- `results=136 passes=5758 failures=0 failingResults=0`.
- Known failures ignored by script: none.

Golden metrics:

- `run-golden-audio-metrics.ps1`: PASS.
- Baseline: `docs/golden-metrics/p4-offline-qa-baseline.json`.
- Coverage gaps: none reported.

RT profile Debug:

- P5D: `total=16 pass=15 warn=1 fail=0`.
- Remaining WARN: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=1.344300`.
- Warning text: peak budget ratio exceeded 75%; one or more blocks exceeded 90% and 100% budget.

RT profile Release:

- P5D: `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.249825`.

## RT Profile Comparison

P5C Debug:

- `total=16 pass=15 warn=1 fail=0`.
- WARN scenario: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=0.970575`.
- `stress_block_32.maxBudgetRatio=0.640350`.

P5D Debug:

- `total=16 pass=15 warn=1 fail=0`.
- WARN scenario: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=1.344300`.
- `stress_block_32.maxBudgetRatio=0.718200`.

P5C Release:

- `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.225975`.
- `stress_block_32.maxBudgetRatio=0.139200`.

P5D Release:

- `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.249825`.
- `stress_block_32.maxBudgetRatio=0.112200`.

Interpretation: no broad RT regression was observed. Debug retained the same single known 96 kHz warning class and remains scheduler-sensitive. Release, the primary performance gate, remained clean at `16/16/0/0`.

## Behavior Preservation Notes

- No DSP processors were modified.
- No routing, LineA/LineB/Dual, graph build/swap, dry/wet smoothing, or latency logic was changed.
- No parameter IDs, preset schema, host notification semantics, or golden baselines were changed.
- `HealthMonitor` and `CpuMeter` behavior was not changed by P5D.
- Pedal code was not touched.

## Risks Remaining

- `PluginProcessor` and `SessionStore` still have separate raw runtime parameter atomics. This is intentional for P5D, but the duplication remains a maintenance cost.
- Debug RT profile remains noisy at 96 kHz and can swing between runs without corresponding Release regression.
- The output mix bridge still has legacy compatibility behavior for `0..1` raw values. Any future unification must preserve both audio behavior and raw-value observability.
- Header-only modules compile through includes but are not added to `NOVA.jucer`; this was chosen to keep P5D low risk.

## Recommendation For P5E

For P5E, avoid global parameter unification until explicit tests cover raw `outputMix` observability, host transport snapshot reuse, and revision edge cases. The safer next step is a small graph/control-plane extraction around graph command application or active graph lifetime, keeping DSP, dry/wet, routing, and parameter cache semantics untouched.
