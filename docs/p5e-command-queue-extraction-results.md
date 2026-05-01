# P5E Command Queue Extraction Results

Date: 2026-04-29 local run

## Summary

P5E extracted the AudioEngine graph-command queue storage into a dedicated header-only module:

- `Source/Core/Audio/AudioEngineCommandQueue.h`

The extraction is deliberately narrow. `GraphCommand` remains private to `AudioEngine`, and `AudioEngine` still owns command interpretation, model mutation, graph rebuild decisions, graph build/swap, routing, dry/wet, runtime params, health monitoring, CPU metering, and diagnostics.

No DSP, tone, routing, dry/wet, latency, graph build/swap internals, parameter IDs, preset schema, golden baselines, or known-failure lists were changed.

## Files Modified

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/Audio/AudioEngineCommandQueue.h`
- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`
- `artifacts/p4-offline-qa-report.txt`
- `artifacts/rt-profile-debug-x64-report-p5e.json`
- `artifacts/rt-profile-release-x64-report-p5e.json`
- `docs/p5e-command-queue-extraction-results.md`

Existing P5B/P5C/P5D files remain in the working tree:

- `Source/Core/Audio/CpuMeter.h`
- `Source/Core/Audio/HealthMonitor.h`
- `Source/Core/Audio/RuntimeParameterSnapshot.h`
- `docs/p5b-cpumeter-extraction-results.md`
- `docs/p5c-healthmonitor-extraction-results.md`
- `docs/p5d-runtime-parameter-snapshot-results.md`

## Module Created

Created `Source/Core/Audio/AudioEngineCommandQueue.h`.

The module is header-only to avoid touching `NOVA.jucer` or generated Visual Studio project files.

Public surface:

- `Nova::Audio::AudioEngineCommandQueue<Command>`
- `enqueue(const Command&)`
- `hasPending() const noexcept`
- `drain()`

The class is templated so `AudioEngine::GraphCommand` can stay inside `AudioEngine`. This avoids widening includes and avoids moving command semantics into the helper.

## State Moved

Moved from `AudioEngine::ControlPlane` into `AudioEngineCommandQueue`:

- `juce::CriticalSection commandLock`
- `std::deque<GraphCommand> pendingCommands`
- `std::atomic<bool> commandsPending`

`ControlPlane` now stores:

- `Nova::Audio::AudioEngineCommandQueue<GraphCommand> commandQueue`

## Logic Moved

Moved out of `AudioEngine`:

- push command under `commandLock`
- set pending flag after enqueue with `std::memory_order_release`
- test pending flag with `std::memory_order_acquire`
- drain queued commands by swapping into a local `std::deque`
- clear pending flag after drain with `std::memory_order_release`

No logging, `juce::String`, DSP work, graph mutation, `ValueTree` access, host notifications, or session calls were added to the queue.

## Logic Kept In AudioEngine

Kept in `AudioEngine`:

- `GraphCommandType`
- `GraphCommand`
- `enqueueGraphCommand(...)` scheduling semantics
- `flushPendingGraphCommands()`
- `applyGraphCommandToModel(...)`
- model chain mutation
- active graph publish/retirement
- explicit graph reset handling
- runtime parameter application after command drain
- `requestControlGraphRebuild()`
- all public `AudioEngine` APIs

`enqueueGraphCommand(...)` still decides whether to flush immediately on the message thread or call `triggerAsyncUpdate()`.

## Command Order Preservation

Order is unchanged:

- commands are still appended with `push_back`
- `drain()` still uses `swap` into a local deque
- `flushPendingGraphCommands()` still iterates with `for (const auto& cmd : commands)`
- no sorting, filtering, coalescing, or deduplication was introduced

This preserves add/remove/move/clear/bypass ordering and the existing behavior for explicit rebuild commands.

## flushPendingGraphCommands Preservation

`flushPendingGraphCommands()` still:

- returns early when there are no pending commands and no graph reset request
- applies pending runtime params to the active graph on that early return path
- drains commands before taking `modelLock`
- applies command mutations under `modelLock`
- treats `GraphCommandType::RebuildGraph` as explicit rebuild
- consumes `graphResetRequested` with `exchange(false, std::memory_order_acq_rel)`
- rebuilds only when topology or explicit reset/rebuild requires it
- applies runtime params after command processing
- cleans retired graphs for params-only changes

Only the storage/drain mechanics changed location.

## Audio Thread Boundary

The new queue is not referenced by `AudioEngine::process`.

Queue drain and command interpretation remain in:

- message-thread flush via `enqueueGraphCommand(..., flushIfSafe=true)`
- `AsyncUpdater::handleAsyncUpdate()`
- `AudioEngine::run()`
- `synchronizeProcessingState()`, which keeps the existing guard that returns immediately when called from the engine audio thread

P5E did not add any new queue calls to the realtime processing function. The queue still uses a `juce::CriticalSection`, matching the previous control-plane lock semantics, and the policy scanner reports no FAIL in active audio routes.

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
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 240 -ReportPath artifacts\rt-profile-debug-x64-report-p5e.json
powershell -ExecutionPolicy Bypass -File scripts\run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240 -BaselinePath docs\rt-profile\p4c-rt-profile-release-baseline.json -ReportPath artifacts\rt-profile-release-x64-report-p5e.json
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
- Policy scan warnings: 4 legacy warnings, unchanged class of issue from P4C/P5D.

Base validation:

- `run-base-audio-validation.ps1`: PASS.
- `results=136 passes=5758 failures=0 failingResults=0`.
- Known failures ignored by script: none.

Golden metrics:

- `run-golden-audio-metrics.ps1`: PASS.
- Baseline: `docs/golden-metrics/p4-offline-qa-baseline.json`.
- Coverage gaps: none reported.

RT profile Debug:

- P5E: `total=16 pass=15 warn=1 fail=0`.
- Remaining WARN: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=1.392600`.
- `stress_block_32.maxBudgetRatio=0.732600`.

RT profile Release:

- P5E: `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.286950`.
- `stress_block_32.maxBudgetRatio=0.264900`.

## RT Profile Comparison

P5D Debug:

- `total=16 pass=15 warn=1 fail=0`.
- WARN scenario: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=1.344300`.
- `stress_block_32.maxBudgetRatio=0.718200`.

P5E Debug:

- `total=16 pass=15 warn=1 fail=0`.
- WARN scenario: `sample_rate_96000`.
- `sample_rate_96000.maxBudgetRatio=1.392600`.
- `stress_block_32.maxBudgetRatio=0.732600`.

P5D Release:

- `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.249825`.
- `stress_block_32.maxBudgetRatio=0.112200`.

P5E Release:

- `total=16 pass=16 warn=0 fail=0`.
- `sample_rate_96000.maxBudgetRatio=0.286950`.
- `stress_block_32.maxBudgetRatio=0.264900`.

Interpretation: no broad RT regression was observed. Debug kept the same single known 96 kHz warning class. Release remained clean at `16/16/0/0`.

## Behavior Preservation Notes

- No DSP processors were modified.
- No routing, LineA/LineB/Dual, graph build/swap internals, dry/wet smoothing, or latency logic was changed.
- No runtime parameter, HealthMonitor, or CpuMeter behavior was changed.
- No parameter IDs, preset schema, host notification semantics, golden baselines, or known failures were changed.
- Pedal registry and pedal processors were not touched.

## Risks Remaining

- `AudioEngineCommandQueue` is intentionally templated to keep `GraphCommand` private. This is safe for P5E, but a future non-templated command module would need a more deliberate command-type boundary.
- `flushPendingGraphCommands()` still mixes command interpretation, model mutation, graph rebuild decisions, and runtime-param application. P5E only isolated queue storage.
- Header-only modules compile through includes but are not added to `NOVA.jucer`; this was chosen to keep this phase low risk.
- Debug RT profile remains scheduler-sensitive at 96 kHz.

## Recommendation For P5F

For P5F, keep the next extraction focused on either graph command interpretation or active graph lifetime, not both. Do not start `RuntimeGraphManager`, `GraphBuilder`, `RoutingMixer`, or `DryWetMixer` until command semantics and graph ownership have separate, documented boundaries.
