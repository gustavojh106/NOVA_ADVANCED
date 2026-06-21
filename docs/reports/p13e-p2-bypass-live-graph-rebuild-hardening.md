# P13E-P2 Bypass Live-Graph Rebuild Hardening

## Scope

P13E-P2 is implemented. P13E-P1 was preserved, and no P13E-P3/P4/P5, limiter, output-chain, amp/cab DSP, or golden-baseline work is included.

## Unsafe Path Removed

`AudioEngine::applyPedalBypassToActiveGraph()` no longer:

- calls `AudioProcessorGraph::rebuild()` on the active runtime;
- writes `GraphRuntimeSlot::bypassed` in the active runtime;
- writes `GraphRuntime::latencySamples` in the active runtime;
- updates dry-delay latency or notifies host PDC from an in-place rebuild.

The only graph rebuild remains in `GraphBuilder`, where it runs while constructing an unpublished graph on the control plane.

## Safe Bypass and PDC Behavior

- The active pedal still receives `ProcessorBase::setBypassed()` so its atomic bypass state and internal bypass crossfade remain the immediate audible control path.
- `applyPedalBypassToActiveGraph()` compares the processor's reported latency before and after the bypass state change.
- A zero-latency bypass remains processor-local and does not rebuild or publish a graph.
- A latency-changing bypass sets a control-plane refresh flag. `flushPendingGraphCommands()` then builds from the updated model and publishes the replacement through the existing `RuntimeGraphManager` build/publish/swap/retire path.
- `ProcessorBase::setProcessingLatency()` now respects an already-bypassed state during preparation, so the unpublished replacement graph captures zero pedal latency when appropriate.
- Bypass-driven replacement builds capture live pedal parameter state from the active runtime and restore it before the replacement graph is prepared. This prevents latency/PDC refresh from resetting pedal controls.

No locks, allocations, logging, file I/O, graph construction, or graph publication were added to `AudioEngine::process()` or any pedal `processBlock()` path.

## Focused Coverage

Updated `AudioEngine` coverage now proves that latency-affecting bypass and unbypass:

- publish a different processor/runtime rather than rebuilding the active graph in place;
- advance the active graph generation;
- converge to the expected graph latency;
- preserve a non-default live Overdrive parameter across both replacement builds.

Added `AudioEngine rapid latency-affecting bypass toggles stay finite and converge`, which:

- prepares the engine and adds the oversampled/latency-affecting Overdrive;
- processes at least 512 audio blocks on an audio thread;
- issues 48 alternating bypass requests from the control thread, followed by a final bypass request;
- checks finite output throughout processing;
- checks the final `ProcessorBase` bypass state;
- checks graph generation advancement and final latency/PDC convergence.

## Validation

- PASS: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Target NOVA_SharedCode`
  - Debug x64, 0 warnings, 0 errors.
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1`
  - Debug x64 Standalone, 0 warnings, 0 errors.
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -TimeoutSeconds 240`
  - `results=283 passes=8117 failures=0 failingResults=0`.
- PASS: `git diff --check`.
- EXPECTED POLICY-GUARD FAIL: `powershell -ExecutionPolicy Bypass -File scripts/check-audio-thread-policy.ps1`
  - No FAIL findings in active audio routes.
  - 595 contract checks; the only two failures are the historical P9F/P9G scope guards, which reject any intentional change to `AudioEngine`/`GraphBuilder` behavior files.
  - 0 warnings, 0 allowlisted warnings, and 0 legacy warnings; four superseded/alias-only files remain quarantined.

## Files Changed for P13E-P2

- `Source/Core/AudioEngine.cpp`
- `Source/Core/AudioEngine.h`
- `Source/Core/Audio/GraphBuilder.h`
- `Source/Core/Audio/GraphRuntimeTypes.h`
- `Source/Effects/Pedals/Base/ProcessorBase.h`
- `Source/Core/AudioEngineTests.cpp`
- `docs/reports/p13e-p2-bypass-live-graph-rebuild-hardening.md`

Validation also regenerated `audio-base-test-report.txt` and the audio-thread policy scan artifacts.

## Remaining Risks / Follow-ups

- A latency-affecting bypass intentionally performs a full control-plane replacement build. Queue draining batches commands, but there is no additional debounce for extremely high-rate host automation.
- Automated coverage verifies finite output and state/PDC convergence; a manual DAW smoke test is still useful for host-specific PDC notification timing and subjective transition-click assessment.
