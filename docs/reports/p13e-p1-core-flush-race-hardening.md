# P13E-P1 Core Flush Race Hardening

## Scope

P13E-P1 is implemented. No P13E-P2 through P13E-P5 work is included, and graph topology behavior is unchanged.

## Race Fixed

`AudioEngine::flushPendingGraphCommands()` could run concurrently from the message/async path and the engine control thread. Both callers could reach `GraphBuilder::applyRuntimeParamsToGraph()` on the active `GraphRuntime`, where `appliedParamRevision` was a plain `uint32_t`.

The patch:

- changes `GraphRuntime::appliedParamRevision` to `std::atomic<uint32_t>` with relaxed loads/stores;
- adds a control-plane `juce::CriticalSection` acquired through `juce::ScopedTryLock` at flush entry, returning immediately on contention;
- routes the control thread's idle parameter-apply path through the guarded flush, so active-runtime parameter application is serialized;
- leaves the audio callback lock-free and allocation-free.

## Focused Coverage

`AudioEngineTests.cpp` now runs concurrent parameter updates/synchronization against repeated graph add/remove flushes while the engine control thread is active. It verifies:

- the active graph's applied revision converges to the final runtime snapshot revision;
- queued graph command order remains intact;
- processed output remains finite after the stress run.

A friend test-access shim defined only in `AudioEngineTests.cpp` reads the snapshot and active-runtime revisions without adding production public API.

## Validation

- PASS: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Target NOVA_SharedCode`
  - Debug x64, 0 warnings, 0 errors.
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1`
  - Debug x64 Standalone, 0 warnings, 0 errors.
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -TimeoutSeconds 240`
  - `results=282 passes=8106 failures=0 failingResults=0`
- PASS: `git diff --check`
- PARTIAL: `powershell -ExecutionPolicy Bypass -File scripts/check-audio-thread-policy.ps1`
  - No FAIL findings in active audio routes.
  - The script's overall status is FAIL only because its historical P9F/P9G scope guards reject any intentional `AudioEngine`/`GraphBuilder` behavior-file change.

## Checklist

- [x] Atomic applied revision.
- [x] Non-blocking single-flusher guard.
- [x] No lock, allocation, file I/O, logging, or graph rebuild added to the audio callback.
- [x] Focused concurrent convergence coverage.
- [x] Standalone unit suite passes.
- [x] No limiter, output-chain, pedal, amp, cabinet, or golden-baseline changes in P13E-P1.
