# NOVA P13E-P3 — Pedal Editor Overlay Lifetime Hardening

Date: 2026-06-18

## Problem Fixed

The pedal editor overlay previously obtained a raw `juce::AudioProcessor*` from `AudioEngine::getProcessorForPedal()`, created an editor for that processor, and retained only the editor component. A later graph replacement could retire and reclaim the graph that owned the processor while the overlay remained open. The JUCE processor editor could then access its freed processor.

P13E-P3 removes that long-lived raw-pointer path. Opening a pedal editor now acquires an `AudioEngine::PedalProcessorHandle` containing:

- the processor pointer;
- the generation of the graph containing it; and
- shared ownership of that graph runtime for the lifetime of the overlay.

## Files Changed

- `Source/Core/AudioEngine.h`
- `Source/Core/AudioEngine.cpp`
- `Source/Core/PluginEditor.h`
- `Source/Core/PluginEditor.cpp`
- `Source/Core/AudioEngineTests.cpp`
- `docs/reports/p13e-p3-pedal-editor-overlay-lifetime-hardening.md`

Validation also regenerated `audio-base-test-report.txt`, `artifacts/audio-thread-policy-scan.txt`, and `artifacts/audio-thread-policy-scan.json`.

## How Overlay Invalidation Works

Every successful `AudioEngine::publishGraph()` stores the new graph generation, marks a coalesced topology notification pending, and triggers the engine's existing `juce::AsyncUpdater`. `AudioEngine::handleAsyncUpdate()` delivers the latest generation to the registered topology listener on the JUCE message thread.

`NOVAAudioProcessorEditor` registers as that listener. If `currentOverlay` is a `PedalEditorOverlay` and its retained generation differs from the newly published generation, the editor resets the overlay immediately in the message-thread callback. Browser, settings, wizard, and tuner UI are unaffected.

All retiring graph paths converge on `publishGraph()`, including add/remove/move, clear and preset/session reconstruction, explicit/auto-heal rebuilds, prepare/replacement, engine re-enable rebuilds, and latency-affecting bypass replacement from P13E-P2.

When no graph replacement occurs, the generation remains unchanged and the pedal editor stays open normally. Parameter-only updates do not emit a topology invalidation.

## Why This Prevents Use-After-Free

The graph-runtime lease closes the asynchronous gap between graph publication and message-thread overlay invalidation. Even if the old graph is removed from the active runtime and its retirement grace period expires before the callback runs, the open overlay still owns that runtime and its processor cannot be freed.

On invalidation, `currentOverlay.reset()` destroys the processor editor first and releases the runtime lease afterward because of the overlay's member declaration order. The retired graph becomes reclaimable only after no editor can access its processor. The user must reopen the editor against the current graph generation.

The audio callback does no new UI work, allocation, logging, file I/O, or blocking synchronization. Graph publication performs only control-plane atomic notification bookkeeping; component destruction and overlay layout occur in the message-thread callback.

## Tests Added

`AudioEngineValidationTests` now includes `P13E-P3 pedal editor handle invalidates on graph generation change`. It:

1. Builds a graph with an Overdrive and acquires the same retained processor handle used by the overlay.
2. Verifies a parameter-only update preserves the handle and does not invalidate it.
3. Removes the edited pedal and verifies the active graph generation advances.
4. Verifies the retained graph stays alive until the topology callback runs.
5. Verifies invalidation is delivered on the JUCE message thread and releases the simulated overlay handle.
6. Forces retired-graph cleanup and verifies the old runtime can then be reclaimed.

This provides focused generation/token and retirement coverage without requiring fragile window automation in the startup unit-test harness.

## Validation Results

- PASS: SharedCode Debug x64 build — 0 warnings, 0 errors.
- PASS: Standalone Debug x64 build — 0 warnings, 0 errors.
- PASS: `scripts/run-base-audio-validation.ps1 -TimeoutSeconds 240`.
  - `results=284 passes=8128 failures=0 failingResults=0`
  - No known failures were ignored.
- PASS: `git diff --check`; output contains only the repository's existing LF-to-CRLF conversion notices.
- EXPECTED POLICY-GUARD FAIL: `scripts/check-audio-thread-policy.ps1`.
  - No FAIL findings in active audio routes.
  - 595 contract checks; the only two failures are the historical P9F/P9G scope guards that reject intentional changes to `AudioEngine`/`GraphBuilder` behavior files.
  - 0 warnings, 0 allowlisted warnings, 0 legacy warnings; four superseded/alias-only files remain quarantined.

## Remaining Risks and P13E Follow-ups

The retained handle is required for any future long-lived UI that edits a live graph processor; such UI must not fall back to the raw `getProcessorForPedal()` accessor. `AudioEngine` currently exposes one topology-listener slot, matching NOVA's single active plugin editor; multi-editor support would require a listener list.

Remaining planned work:

- P13E-P4 shared pedal contract scaffold
- P13E-P5 migration/inter-pedal corruption containment
- P12D-GOLDEN1 rebaseline after P13E

P13E-P1 and P13E-P2 remain closed and were not reopened or altered by this task.
