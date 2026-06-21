# P13E-P4 Shared Pedal RT-Safety Contract Scaffold

## Problem addressed

NOVA's hardened pedals repeated realtime-safety behavior independently: denormal suppression, invalid-input scrubbing, prepared-block limits, fallback counters, parameter checks, and dry/wet math. P13E-P4 adds an opt-in shared contract for future pedal work without reopening P13E-P1/P2/P3 or mass-migrating the catalog.

## Files changed

- `Source/Effects/Pedals/Base/NovaPedalProcessor.h` — new opt-in scaffold derived from `ProcessorBase`.
- `Source/Effects/Pedals/Boost/BoostPedal.h` — migrated the existing hardened wrapper to the scaffold.
- `Source/Effects/Pedals/Compressor/CompressorPedal.h` — migrated the existing hardened wrapper and retained gain-reduction fallback behavior.
- `Source/Core/AudioEngineTests.cpp` — added reusable scaffold tests and focused Boost/Compressor safety coverage.
- `NOVA.jucer` — registered the new base header as project source-of-truth membership.
- `scripts/check-audio-thread-policy.ps1` — taught the scanner to inspect `processPedalBlock()` audio ranges and to recognize the shared block/fallback guard for migrated P1 pedals.
- `artifacts/audio-thread-policy-scan.txt` / `.json` — refreshed by validation; both were already modified when this task started.
- `audio-base-test-report.txt` — refreshed by the passing app-driven validation run.
- `docs/reports/p13e-p4-shared-pedal-contract-scaffold.md` — this implementation and validation report.

No golden baseline, amp, cabinet, output-chain, limiter, state-schema, or preset file was changed for P13E-P4.

## New scaffold contract

`NovaPedalProcessor` owns a final `processBlock()` wrapper. Opted-in pedals:

1. call `preparePedalProcessing(sampleRate, maximumBlockSize, maximumChannels)` from `prepareToPlay()` after allocating their own scratch/DSP resources;
2. call `releasePedalProcessing()` from `releaseResources()`;
3. implement `processPedalBlock(buffer, midi)` for wet DSP only;
4. optionally override `handleRealtimeFallback()` for pedal-specific diagnostics/state cleanup.

The final wrapper provides:

- one `juce::ScopedNoDenormals` covering bypass integration, safety checks, wet rendering, and bypass crossfade completion;
- input sanitization through `Nova::DSP::scrub()` for NaN, Inf, denormal, and over-limit samples before `ProcessorBase` captures bypass dry scratch;
- a prepared block/channel boundary checked before derived wet DSP;
- finite-input dry fallback without resizing or entering derived wet DSP when a host block exceeds preparation;
- shared atomic fallback diagnostics through `getRealtimeFallbackCount()` and `clearRealtimeFallbackCount()`;
- `readParameter()` for finite, range-clamped `AudioParameterFloat` reads;
- `mixDryWetEqualPower()` for finite equal-power dry/wet interpolation.

The bypass/crossfade/latency implementation remains owned by `ProcessorBase`: the scaffold calls `beginBypassProcess()` and `endBypassProcess()` in the same order as the hardened pedal wrappers it replaces.

## Pedals migrated

### Boost

Boost was selected because its P13D hardening already had the target pattern: denormal guard, invalid-input scrub, prepared-size boundary, dry fallback, and fallback counter. Its oversampling, filters, smoothing, latency reporting, output containment, and tone equations remain in the pedal unchanged. Parameter targets now use the shared finite/range helper.

### Compressor

Compressor was selected because it already had prepared dry storage, an oversized dry fallback, and equal-power blend math. The shared fallback hook preserves its existing behavior of resetting the exposed gain-reduction meter to `0 dB` on fallback. The helper uses the same cosine/sine equal-power equation as the prior inline code.

No other pedal was migrated.

## Safety behavior covered

- normal prepared blocks dispatch wet DSP and do not increment fallback diagnostics;
- NaN and both Inf signs are replaced before derived processing;
- denormal-like values are flushed;
- hot values are bounded by the existing NOVA signal guard before derived processing;
- oversized blocks do not enter derived wet DSP and preserve finite dry input exactly;
- bypass transitions cannot reintroduce corrupt input through `ProcessorBase` dry scratch;
- the optional fallback hook is invoked once per fallback block;
- invalid operands in the shared mix helper remain finite;
- existing `ProcessorBase` bypass transitions and Boost latency reporting continue through the unchanged base lifecycle.

## Tests added

The `P13E-P4 Shared Pedal RT-Safety Contract` suite adds:

- a small reusable `P13EPedalContractProbe` proving shared scrub, wet dispatch, fallback dispatch, and helper behavior;
- normal finite/bounded/audible checks for Boost and Compressor;
- NaN/Inf/denormal/hot-input checks for Boost and Compressor;
- oversized dry-fallback checks for Boost and Compressor using the existing P1 helper;
- a bypass-transition regression check proving the shared scrub runs before dry-scratch capture.

Existing P1 prepared-matrix, oversized-fallback, bypass-transition, and P13D Boost abuse tests continue to run unchanged.

## Validation results

- SharedCode Debug x64 build: **PASS**, 0 warnings, 0 errors.
- Standalone Debug x64 build: **PASS**, 0 warnings, 0 errors. The first incremental link encountered a stale `AssetBrowserOverlay.obj` COMDAT error; a clean SharedCode rebuild regenerated intermediates and the retry passed.
- `scripts/run-base-audio-validation.ps1 -TimeoutSeconds 240`: **PASS** — 291 results, 8,171 assertions, 0 failures.
- `git diff --check`: **PASS**.
- `NOVA.jucer` XML parse and new-header membership check: **PASS**.
- Projucer 8.0.12 CLI re-save: completed, but generated projects resolved the existing relative JUCE module path one directory too high for this OneDrive workspace. That invalid generated path churn was not retained; `NOVA.jucer` remains updated and the known-good generated projects produced both passing builds.
- Audio-thread policy scan active-route analysis: **PASS** — 32 active files / 38 audio ranges scanned, no active audio-route findings, no warnings, and all shared pedal guard checks pass. The scanner's overall status remains **FAIL** only because two historical P9F/P9G scope guards detect pre-existing closed-phase changes in `AudioEngine` / `GraphBuilder`; P13E-P4 did not modify those files. Four pre-existing superseded/alias pedal guards remain quarantined, not active warnings.

## Risks and remaining follow-ups

- The scaffold is deliberately opt-in; unmigrated pedals retain their current local safety wrappers.
- The shared input guard can shape values above `HARD_ABS_LIMIT_LINEAR`; finite nominal guitar-level input is unchanged.
- P13E-P5 should migrate the remaining stateful/feedback pedals and add inter-pedal corruption containment. That work is intentionally not included here.
- P12D-GOLDEN1 should rebaseline after P13E is complete. No golden data was changed here.
