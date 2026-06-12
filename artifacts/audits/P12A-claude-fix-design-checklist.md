# P12A — Fix Design Checklist

Use during implementation and before merge. Each item is binary; do not mark
PASS without observable evidence.

## Code changes (must all be true)

- [ ] `OutputChain.cpp:278` gate removed. `limiter.process()` always called.
- [ ] `prepareToPlay` calls `setLatencySamples(limiter.getLookaheadSamples())`
      once and never again.
- [ ] `updateReportedLatencyForLimiter` no longer branches on threshold; either
      deleted or reduced to a constant.
- [ ] `targetLimiterDb` initial value = `-0.3f` (was `0.0f`).
- [ ] `sanitizeLimiterDb` range = `[-12.0f, -0.1f]` to avoid collision with
      internal `0.9885531` per-sample active-gate.
- [ ] `applySoftCeiling` threshold = `0.997f`, ceiling = `0.9995f`. tanh shape
      unchanged.
- [ ] `sanitizeOutputVolumeDb` range = `[-36.0f, +6.0f]`.
- [ ] No new allocations in `processBlock`. No new locks. No `juce::String`,
      `SessionLogger`, `setSize`, `resize` on hot path.

## Files NOT touched (must all be true)

- [ ] `Source/Core/Audio/RuntimeGraphManager.h` unchanged.
- [ ] `Source/Core/AudioEngine.cpp` graph-swap path unchanged.
- [ ] `Source/Core/PluginStateModel.h` unchanged. `STATE_SCHEMA_VERSION` = 1.
- [ ] No pedal `processBlock` body modified.
- [ ] `Source/Core/AudioEngineTests.cpp` structure unchanged
      (new tests appended, no refactor).
- [ ] `kProfessionalOutputTrim` and every pedal `outputTrim` unchanged.

## Static / contract checks

- [ ] `scripts/check-audio-thread-policy.ps1`: status PASS, failures 0,
      warnings 0.
- [ ] `audio-thread-policy-scan.json` contract checks: 638+ pass,
      0 fail.

## Latency behaviour

- [ ] Reported latency at `prepareToPlay(48000, 64)` = lookaheadSamples = 96
      (or platform equivalent). Constant.
- [ ] Sweep limiter knob across full range; reported latency delta = 0.
- [ ] DAW re-PDC count across knob sweep = 0.

## Null tests (clean path preservation)

- [ ] Sine 440 Hz at −20 dBFS through Dry Reference preset:
      sample-equal to a pre-fix build with limiter forced bypass.
- [ ] White noise at −18 dBFS through Clean Studio preset:
      RMS delta < 1.0e-5; peak delta < 1.0e-4.
- [ ] `limiterMaxReductionDb < 0.01` on clean material.

## High-gain scenario (the actual bug)

- [ ] New scenario: HighGainAmp drive=10, Modern4x12 cabinet, master = 0 dB,
      input = pink noise + chord transient at −6 dBFS, 5 s duration.
- [ ] `limiterActiveBlocks > 0`.
- [ ] `limiterMaxReductionDb <= 6.0`.
- [ ] `softCeilingTouchedSamples == 0`.
- [ ] `guardClippedSamples == 0`.
- [ ] Output peak ≤ −0.2 dBFS (i.e. threshold honoured by lookahead).
- [ ] No DC drift; `postDcStage` mean magnitude < 1.0e-4.

## Fuzz reference preservation

- [ ] Fuzz-only preset, `Level = 1.0`, sustained sine at −12 dBFS.
- [ ] Sustain region RMS delta vs pre-fix < 1.0e-3.
- [ ] Spectral centroid delta < 50 Hz on sustain.
- [ ] Limiter only touches transients: `limiterTouchedSamples` localized
      to first ~10 ms of each transient.

## Preset compatibility

- [ ] All 6 draft presets in `Resources/Presets/DraftFactory/generated`
      load without warning.
- [ ] Round-trip save→load→save preserves canonical bytes for every draft.
- [ ] Legacy preset with `outputDb = +12` loads, clamps silently to +6 via
      `sanitizeOutputVolumeDb`, does not corrupt other state.
- [ ] Legacy preset with `limiterDb = 0` loads, clamps silently to `-0.1`.

## Telemetry / debug

- [ ] `DebugSnapshot::limiterActiveBlocks` now non-zero on high-gain runs
      (was zero by design at default).
- [ ] `DebugSnapshot::softCeilingTouchedSamples` ≈ 0 on all draft presets.
- [ ] Test assertions that used to expect zeros for limiter metrics on the
      transparent path are updated to expect threshold-relative behaviour.

## Manual listening (gates ship)

- [ ] All 6 drafts move from `NOT_RUN` to PASS or `ISSUE_LOGGED` in
      `artifacts/manual-listening/p10a-six-draft-presets/results-template.csv`.
- [ ] High-gain WARN listening verdict = PASS at amp drive ≥ 8 and master 0 dB.
- [ ] No new ducking complaints on clean and crunch presets.

## Sign-off

- [ ] No FACTORY_APPROVED markers added in this pass.
- [ ] `STATE_SCHEMA_VERSION` confirmed unchanged.
- [ ] `scripts/run-base-audio-validation.ps1` PASS.
- [ ] `scripts/quick-validate.ps1` PASS.
