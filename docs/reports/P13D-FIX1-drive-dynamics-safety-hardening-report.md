# P13D-FIX1 Drive/Dynamics Safety Hardening Report

## Verdict

P13D-FIX1 is implemented for the active drive/dynamics safety findings. Boost now rejects larger-than-prepared host blocks before the oversampler, Boost and Noise Gate scrub NaN/Inf input before stateful DSP, and Distortion, Fuzz, Compressor, and Noise Gate enter their process paths with `juce::ScopedNoDenormals`.

The change is intentionally safety-only: no preset schema, routing, output limiter, graph, UI, amp, cabinet, delay, reverb, modulation, or Neural behavior was intentionally changed.

## Code Changes

- `Source/Effects/Pedals/Boost/BoostPedal.h`
  - Tracks prepared block/channel capacity.
  - Scrubs non-finite input samples to `0.0f` before filters, oversampling, and clipping.
  - Uses a no-allocation dry fallback with an atomic fallback counter when the host block exceeds prepared capacity.
  - Resets Boost biquad state if non-finite state is detected.
- `Source/Effects/Pedals/Gate/NoiseGatePedal.h`
  - Adds `juce::ScopedNoDenormals` at process entry.
  - Scrubs non-finite input samples before sidechain filters, detector, and lookahead.
  - Guards biquad, detector, lookahead, and gate gain state against non-finite latch.
- `Source/Effects/Pedals/Distortion/DistortionPedal.h`
  - Adds `juce::ScopedNoDenormals` at process entry.
- `Source/Effects/Pedals/Fuzz/FuzzPedal.h`
  - Adds `juce::ScopedNoDenormals` at process entry.
- `Source/Effects/Pedals/Compressor/CompressorPedal.h`
  - Adds `juce::ScopedNoDenormals` at process entry.
- `Source/Core/AudioEngineTests.cpp`
  - Adds Boost to the generic prepared, oversized fallback, and bypass finite-safety matrix.
  - Adds P13D tests for Boost oversized fallback, Boost NaN/Inf recovery, Noise Gate NaN/Inf recovery, near-silence finite behavior, and nominal finite input behavior.

## MetalDistortion

`Source/Effects/Pedals/Metal/MetalDistortionPedal.h` was not changed. The existing policy scan classifies it as `LEGACY_ALIAS_ONLY`: it is not registered in `PedalRegistry`, and `PedalCatalog` maps the legacy "Metal Distortion" name to the active `Distortion` processor. Deleting it was deferred to avoid source-membership and migration risk.

## Validation

- PASS: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Target NOVA_SharedCode`
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1`
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/run-base-audio-validation.ps1 -TimeoutSeconds 180`
  - `results=281 passes=8103 failures=0 failingResults=0`
- PARTIAL: `powershell -ExecutionPolicy Bypass -File scripts/run-audio-quality-gates.ps1 -Full -TimeoutSeconds 240`
  - Base validation passed.
  - Golden metrics stopped the full gate on existing out-of-scope `output_chain_clean_path / impulse_transparency_line_a` drift.
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report-gate.json`
  - `total=16 pass=16 warn=0 fail=0`
- PASS: `powershell -ExecutionPolicy Bypass -File scripts/check-audio-thread-policy.ps1`
- PASS: JSON parse of `artifacts/audio-thread-policy-scan.json`
- PASS: null-byte scan over `Source`, `docs`, and `artifacts`
- PASS: `git diff --check`

## Notes

The full validation failure is outside this patch scope and points at OutputChain impulse latency/transparent pass-through behavior. P13D explicitly excluded OutputChain/P12D limiter logic, so no OutputChain change was made here.
