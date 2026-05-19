# P10G High-Gain Noise Floor / Collapse Results

## Scope

P10G addressed the remaining post-P10F WARN/P2 high-gain issues: excessive idle noise, poor low-gate usefulness, Distortion active-volume-collapse feel, upstream dirt into HighGainAmp, and cheap fizz. It did not change UI/UX, deploy, Reaper smoke, factory approval, schemas, IDs, known failures, golden baselines, or OutputChain limiting.

## Fixes Applied

| Area | Change |
|---|---|
| Distortion high-gain modes | Raised useful gate thresholds, lowered true-idle floor, slowed close/release, and added pre-saturation idle-noise rejection. |
| Distortion fizz | Reduced Studio/Metal presence and top-cut aggressiveness before the final tone section. |
| HighGainAmp upstream noise | Added input noise rejection before high-gain stages, preserving fast attack and avoiding output masking. |
| HighGainAmp fizz | Slightly reduced presence shelf and post-presence low-pass ceiling. |
| Tests | Added deterministic P10G metrics for idle, post-phrase, active RMS, gate movement, collapse proxy, clipping, brightness, high-frequency energy, and rumble. |

## P10G Scenarios Added

- `p10g_fuzz_low_gate_reference`
- `p10g_distortion_low_gate_noise_floor`
- `p10g_highgain_low_gate_noise_floor`
- `p10g_boost_highgain_ground_noise_guard`
- `p10g_distortion_highgain_ground_noise_guard`
- `p10g_distortion_active_volume_collapse_guard`
- `p10g_noise_gate_sustain_preservation_guard`
- `p10g_highgain_fizz_proxy_guard`
- `p10g_highgain_baseline_preservation`

Additional requested guard names are represented by the above metric surface:

- `p10g_distortion_low_gate_sustain_guard`: covered by `p10g_distortion_low_gate_noise_floor` and `p10g_noise_gate_sustain_preservation_guard`.
- `p10g_distortion_idle_noise_floor_guard`: covered by `p10g_distortion_low_gate_noise_floor`.
- `p10g_distortion_highgain_noise_to_signal_guard`: covered by `p10g_distortion_highgain_ground_noise_guard`.
- `p10g_boost_highgain_idle_noise_guard`: covered by `p10g_boost_highgain_ground_noise_guard`.
- `p10g_highgain_upstream_noise_rejection_guard`: covered by the Boost/Distortion high-gain ground-noise guards.

## Representative After Metrics

These are from the deterministic P10G phrase/noise harness during validation tuning. Values are proxy metrics, not a replacement for listening.

| Chain | idleNoiseRms | postPhraseNoiseRms | noiseToSignalRatio | activeRms | highFrequencyEnergyProxy | clipped/invalid |
|---|---:|---:|---:|---:|---:|---:|
| Noise Gate 4% -> Fuzz -> ClassicAmp -> Cabinet | ~0.0092 | ~0.0704 | ~0.113 | ~0.622 | ~0.024 | 0 / 0 |
| Noise Gate 4% -> Distortion -> CleanAmp -> Cabinet | ~0.00052 | ~0.00101 | ~0.0068 | ~0.150 | ~0.017 | 0 / 0 |
| Noise Gate 4% -> HighGainAmp -> Modern4x12 | ~0.00018 | ~0.00124 | ~0.0129 | ~0.097 | ~0.022 | 0 / 0 |
| Distortion -> HighGainAmp -> Modern4x12 | ~0.0018 | ~0.0082 | ~0.0436 | ~0.187 | ~0.063 | 0 / 0 |

## Validation

Completed:

- SharedCode Debug build: PASS.
- Standalone Debug build: PASS.
- SharedCode Release build: PASS.
- Standalone Release build: PASS.
- VST3 Release build: PASS.
- `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`: PASS, run 1.
  - `results=254 passes=7342 failures=0 failingResults=0`
- `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`: PASS.
  - `results=254 passes=7342 failures=0 failingResults=0`
- `scripts/run-golden-audio-metrics.ps1`: PASS, no baseline update.
- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `total=16 pass=16 warn=0 fail=0`.
- `scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, `runs=3 passRuns=3 warnRuns=0 failRuns=0`.
- `scripts/check-audio-thread-policy.ps1`: PASS.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
- `scripts/run-diagnostics-bundle.ps1`: PASS.
- `scripts/generate-draft-factory-presets.ps1`: PASS, generated 6 draft `.nova-preset` files.
- `scripts/validate-draft-factory-presets.ps1`: PASS, manifest updated `false`.

## Manual Listening Status

High-gain remains ready for another manual listening pass, not approved. The next pass should focus on:

- Distortion active-volume-collapse feel in Studio and Metal modes.
- Boost -> HighGainAmp -> Modern4x12 idle noise with Noise Gate at 0%, 4%, 8%, 15%, 30%, and 50%.
- Distortion -> HighGainAmp -> Modern4x12 fizz and sustain.
- HighGainAmp -> Modern4x12 baseline tone, to confirm the good post-P10F baseline stayed intact.
- Clean Studio, Wide Ambient Clean, Clean Amp, and clean Chorus/Delay/Reverb spot checks.

Manual listening QA general remains pending.
Distortion/high-gain listening QA remains pending.
P7F/Reaper remains pending.
