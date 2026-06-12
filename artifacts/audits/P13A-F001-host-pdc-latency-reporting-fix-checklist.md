# P13A-F001 Host PDC Latency Reporting Fix Checklist

Date: 2026-06-12

## Recovery

- [x] Restore `Source/Core/PluginProcessor.h` from git.
- [x] Compare the restored header with the current P13A-F001 implementation in `PluginProcessor.cpp`.
- [x] Add only the missing declarations and members required by the implementation.
- [x] Preserve the existing public/private processor API.
- [x] Leave `PluginProcessor.cpp` unchanged.
- [x] Leave AudioEngine, OutputChain, RuntimeGraphManager, DryWetMixer, RoutingMixer, PluginStateModel, pedal DSP, UI, schema, and tests untouched.

## Host PDC Requirements

- [x] Preserve async host latency refresh through `juce::AsyncUpdater`.
- [x] Preserve deduped host latency reporting with `lastReportedHostLatencySamples`.
- [x] Keep `setLatencySamples()` off the audio thread.
- [x] Keep constructor-time latency notifications gated until the engine is prepared.
- [x] Clamp reported latency to `Nova::Config::MAX_GRAPH_LATENCY_SAMPLES`.

## Regenerated Deliverables

- [x] `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-report.md`
- [x] `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-summary.md`
- [x] `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-findings.json`
- [x] `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-checklist.md`

## Validation

- [x] Run `scripts/build-nova.ps1`.
- [x] Force a real Standalone rebuild after the header repair.
- [x] Run `scripts/run-base-audio-validation.ps1`.
- [x] Run a null-byte scan over the repaired header and regenerated deliverables.

## Remaining

- [ ] Run Release VST3 build before release.
- [ ] Perform manual DAW PDC inspection before release.
- [ ] Move the repo outside OneDrive sync or exclude it from sync.
