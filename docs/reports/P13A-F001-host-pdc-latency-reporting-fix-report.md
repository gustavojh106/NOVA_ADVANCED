# P13A-F001 Host PDC Latency Reporting Fix Report

Date: 2026-06-12
Type: recovery and validation report

## Scope

`Source/Core/PluginProcessor.h` was restored from git after null-byte corruption, while `Source/Core/PluginProcessor.cpp` still contained the P13A-F001 host PDC implementation. This recovery updates only the header declarations and members required by the current implementation, then regenerates the missing deliverable documents.

## Files Repaired

- `Source/Core/PluginProcessor.h`
- `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-report.md`
- `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-summary.md`
- `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-findings.json`
- `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-checklist.md`

## Header Repair

The header now matches the existing P13A-F001 implementation in `PluginProcessor.cpp` by adding:

- private inheritance from `juce::AsyncUpdater`
- private inheritance from `AudioEngine::LatencyListener`
- `handleAsyncUpdate()` override
- `audioEngineLatencyChanged(int)` override
- `requestHostLatencyRefresh()`
- `updateHostLatencyFromEngineNow()`
- `applyHostLatencyIfChanged(int)`
- `audioEnginePreparedForHostLatency`
- `hostLatencyRefreshPending`
- `lastReportedHostLatencySamples`

No changes were made to `PluginProcessor.cpp`.

## Host PDC Behavior Preserved

The repaired header preserves the implementation policy already present in `PluginProcessor.cpp`:

- graph latency changes are coalesced through `hostLatencyRefreshPending`
- the async callback re-reads the latest engine latency before reporting
- duplicate latency values are filtered by `lastReportedHostLatencySamples`
- `setLatencySamples()` is called only from prepare/sync/message-thread paths, not from `processBlock`
- reported values are clamped to `Nova::Config::MAX_GRAPH_LATENCY_SAMPLES`

## Validation

- `scripts/build-nova.ps1`: PASS, Debug Standalone target, 0 warnings, 0 errors
- forced MSBuild `/t:Rebuild`: PASS, Debug Standalone target, 0 warnings, 0 errors
- `scripts/run-base-audio-validation.ps1`: PASS, 268 results, 7,543 passes, 0 failures
- null-byte scan: PASS after regeneration; no null bytes found in the repaired header or regenerated deliverables

## Verdict

PASS. The header surface has been restored to match the current P13A-F001 implementation, async/deduped host latency reporting is preserved, and the project rebuilds and passes base audio validation.

## Remaining Issues

- Release VST3 build and DAW-side PDC inspection were not run in this recovery.
- The repository still lives under OneDrive, which remains a corruption risk for uncommitted files.
