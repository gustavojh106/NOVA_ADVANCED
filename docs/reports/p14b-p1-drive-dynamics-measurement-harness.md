# P14B-P1 Drive & Dynamics Measurement Harness

## Scope

P14B-P1 adds objective measurement infrastructure and baseline tests for Drive & Dynamics only:

- Boost: `boostGain`, `boostLevel`
- Compressor: `compMakeup`, `compBlend`
- Noise Gate: `gateThreshold`, `gateRange`
- Overdrive: `drive`, `level`, `mix`
- Distortion: `distGain`, `distLevel`, `distMix`
- Fuzz: `fuzzAmount`, `fuzzLevel`, `fuzzMix`
- Neural: `neuralDrive`, `neuralLevel`, `neuralMix`

No pedal DSP algorithms, gain staging, coefficients, parameter ranges, latency, bypass behavior, OutputChain behavior, graph lifecycle code, presets, or shared realtime safety contracts were intentionally changed.

## Measurement Definitions

- Sample rate: 48000 Hz
- Block size: 64 samples
- Render length: 10240 samples
- Measurement window: samples 2048..10239
- dBFS: `20 * log10(linear)`, floored at -180 dBFS
- RMS: stereo window RMS across both channels
- Peak: maximum absolute sample in the measured window
- Null RMS: RMS of `engaged - bypass` in the measured window
- Unity delta: `engagedRmsDbfs - bypassRmsDbfs`
- Sweep jump: largest absolute adjacent-step output RMS dB change across 9 linear parameter steps
- Standard reference signal: deterministic guitar-band composite plus fixed-seed noise, seed `0x14B10`
- Sine stimuli: deterministic 1 kHz and 5 kHz stereo sine renders

Normal unit tests do not write files. A Debug-only, opt-in JSON report can be generated with `NOVA_P14B_MEASUREMENT_REPORT_PATH`. A Release diagnostic build may also generate the report only when compiled with `NOVA_ENABLE_RELEASE_DIAGNOSTICS=1`; production Release builds do not compile the P14B harness.

The generated JSON now includes metadata:

- commit hash from `NOVA_BUILD_COMMIT`
- branch from `NOVA_BUILD_BRANCH`
- build configuration from `NOVA_BUILD_CONFIGURATION`
- build platform from `NOVA_BUILD_PLATFORM`
- compiler/toolchain
- OS name
- generation timestamp
- sample rate, block size, render length, measurement start, fixed noise seed, and sweep-step count

The reviewed local artifact from this run is `artifacts/p14b-p1-drive-dynamics-measurement-baseline.json` and is intentionally untracked.

## Ownership And Repeatability

Tracked:

- `Source/Core/AudioEngineTests.cpp` P14B measurement harness and unit coverage.
- `Source/Core/PluginProcessor.cpp` opt-in measurement writer hook.
- `scripts/generate-p14b-drive-dynamics-baseline.ps1`.
- `scripts/compare-p14b-drive-dynamics-baseline.ps1`.
- This reviewed report under `docs/reports/`.

Generated and untracked:

- `artifacts/p14b-p1-drive-dynamics-measurement-baseline.json`.
- `artifacts/p14b-audit-measurement-report.json`.
- `artifacts/p14b-audit-audio-validation-report.txt`.
- Any `artifacts/p14b-*.stable.json` comparison byproducts.

Generation:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/generate-p14b-drive-dynamics-baseline.ps1 -Configuration Debug -Platform x64 -OutputPath artifacts/p14b-p1-drive-dynamics-measurement-baseline.json
```

Second-run generation for determinism evidence:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/generate-p14b-drive-dynamics-baseline.ps1 -Configuration Debug -Platform x64 -OutputPath artifacts/p14b-p1-drive-dynamics-measurement-baseline-run2.json
```

Stable comparison, ignoring approved volatile metadata fields:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/compare-p14b-drive-dynamics-baseline.ps1 -BaselinePath artifacts/p14b-p1-drive-dynamics-measurement-baseline.json -CurrentPath artifacts/p14b-p1-drive-dynamics-measurement-baseline-run2.json
```

Approved volatile metadata fields for stable comparison: `generatedAt`, `commitHash`, `branch`, `buildConfiguration`, `buildPlatform`, `compiler`, and `osName`. Measurement sections must match exactly.

## Reference Settings

| Pedal | Reference / Neutral Setting |
| --- | --- |
| Boost | `boostGain=0`, `boostTone=0.58`, `boostTight=0.24`, `boostChar=0`, `boostMid=0`, `boostLevel=1` |
| Compressor | `compThreshold=-20 dB`, `compRatio=4:1`, `compAttack=12 ms`, `compRelease=120 ms`, `compKnee=6 dB`, `compFocus=0.55`, `compBlend=0.82`, `compMakeup=2.5 dB` |
| Noise Gate | `gateThreshold=-46 dB`, `gateAttack=0.25 ms`, `gateHold=70 ms`, `gateRelease=115 ms`, `gateRange=-96 dB`, `gateHysteresis=8 dB`, `gateFocus=0.55` |
| Overdrive | `drive=30`, `tone=0.58`, `texture=0.42`, `mix=1`, `level=0.74` |
| Distortion | `distGain=58`, `distTone=0.54`, `distBody=0.52`, `distMix=1`, `distLevel=0.64`, `distTight=0.42`, `distMode=Vintage` |
| Fuzz | `fuzzAmount=67`, `fuzzTone=0.48`, `fuzzGate=0.28`, `fuzzMix=1`, `fuzzLevel=0.58`, `fuzzBias=0.56`, `fuzzMode=Vintage` |
| Neural | `neuralDrive=54`, `neuralFocus=0.58`, `neuralDetail=0.54`, `neuralComp=0.42`, `neuralMix=1`, `neuralLevel=0.75` |

## Unity / Gain-Staging Baseline

| Pedal | Input RMS dBFS | Bypass RMS dBFS | Engaged RMS dBFS | Engaged Peak dBFS | Engaged - Bypass dB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Boost | -23.8697 | -23.8697 | -23.8787 | -17.6070 | -0.0090 |
| Compressor | -23.8697 | -23.8697 | -20.0096 | -14.7894 | 3.8601 |
| Noise Gate | -23.8697 | -23.8697 | -23.8721 | -18.6494 | -0.0024 |
| Overdrive | -23.8697 | -23.8697 | -32.2170 | -24.0340 | -8.3473 |
| Distortion | -23.8697 | -23.8697 | -2.9253 | -0.5374 | 20.9444 |
| Fuzz | -23.8697 | -23.8697 | -2.8841 | -0.7504 | 20.9856 |
| Neural | -23.8697 | -23.8697 | -11.6346 | -6.2461 | 12.2351 |

These are baseline observations only. The tests do not fail for non-unity behavior. They fail only for non-finite output, unsafe peaks, impossible values, missing coverage, or gross discontinuity.

## Sweep Baseline

| Pedal | Control | Max Adjacent Jump dB | First-Step RMS dBFS | Loudest Step RMS dBFS |
| --- | --- | ---: | ---: | ---: |
| Boost | `boostGain` | 2.9020 | -23.8787 | -5.0025 |
| Boost | `boostLevel` | 2.7976 | -29.8993 | -17.8581 |
| Compressor | `compMakeup` | 2.1598 | -22.0063 | -5.9096 |
| Compressor | `compBlend` | 1.9357 | -23.8697 | -19.4488 |
| Noise Gate | `gateThreshold` | 8.1384 | -23.8721 | -23.8721 |
| Noise Gate | `gateRange` | 0.0000 | -23.8721 | -23.8721 |
| Overdrive | `drive` | 2.6065 | -38.0774 | -24.2704 |
| Overdrive | `level` | 2.9250 | -48.8670 | -26.3670 |
| Overdrive | `mix` | 1.6077 | -23.8697 | -23.8697 |
| Distortion | `distGain` | 4.5123 | -14.3664 | -2.7276 |
| Distortion | `distLevel` | 10.8066 | -23.8266 | -2.1443 |
| Distortion | `distMix` | 10.0641 | -23.8697 | -2.9253 |
| Fuzz | `fuzzAmount` | 7.3876 | -18.4206 | -2.3259 |
| Fuzz | `fuzzLevel` | 10.8066 | -23.9581 | -1.1929 |
| Fuzz | `fuzzMix` | 9.8811 | -23.8697 | -2.8841 |
| Neural | `neuralDrive` | 2.3009 | -18.4965 | -11.5182 |
| Neural | `neuralLevel` | 3.1200 | -29.6346 | -5.6346 |
| Neural | `neuralMix` | 2.9560 | -23.8697 | -11.6346 |

Largest observed sweep jumps are on level/mix controls for high-gain pedals. This is recorded as baseline data, not judged as a tonal failure.

## Drive Spectral Proxy

The harness uses the existing lightweight Goertzel-style frequency magnitude helper to track the sine fundamental and harmonics up to 10x when below Nyquist. There is no robust FFT path in P14B-P1, and this report makes no aliasing claim.

These spectral values are regression proxies only. They are not aliasing-proof measurements and must not be used as evidence that a drive algorithm is alias-free, fizz-free, or spectrally final.

| Pedal | Sine Hz | Output RMS dBFS | Fundamental dBFS | Tracked Harmonics dBFS | Residual Proxy dB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Overdrive | 1000 | -17.0212 | -20.0337 | -48.6450 | -3.0141 |
| Overdrive | 5000 | -19.4877 | -22.4974 | -69.2152 | -3.0109 |
| Distortion | 1000 | -1.4555 | -4.9562 | -14.2294 | -3.0052 |
| Distortion | 5000 | -2.6775 | -5.7147 | -28.3302 | -3.0071 |
| Fuzz | 1000 | -1.9380 | -5.2590 | -16.5581 | -3.0102 |
| Fuzz | 5000 | -8.4375 | -11.4520 | -41.9649 | -3.0099 |
| Neural | 1000 | -8.4019 | -11.7509 | -22.6701 | -3.0095 |
| Neural | 5000 | -10.8769 | -14.0184 | -29.1621 | -3.0100 |

## Compressor And Gate Probes

`CompressorPedal::computeGainReductionDb` now has analytic coverage for below-threshold, hard-knee/above-threshold, soft-knee midpoint, ratio 1:1, and high-ratio cases.

Noise Gate ramp/sustain probe:

- Observed lookahead: 72 samples
- Open attenuation: 0.0000 dB
- Closed attenuation after sustain: -160.9852 dB
- Output RMS: -22.8675 dBFS
- Output peak: -15.9176 dBFS

## Baseline Findings

- Boost and Noise Gate reference settings are effectively unity on the deterministic reference signal.
- Compressor reference setting adds 3.8601 dB RMS versus bypass due to makeup/blend behavior.
- Overdrive reference setting is 8.3473 dB below bypass on this reference signal.
- Distortion and Fuzz reference settings are roughly +21 dB RMS versus bypass and approach their internal containment ceilings without exceeding the hard absolute limit.
- Neural reference setting is +12.2351 dB RMS versus bypass.
- All target renders, reference baselines, sweeps, spectral proxy renders, compressor analytic cases, and gate probes stayed finite.

## Validation

- `NOVA_SharedCode` Debug x64 build: pass
- `NOVA_StandalonePlugin` Debug x64 build: pass
- `scripts/run-base-audio-validation.ps1 -TimeoutSeconds 240`: pass
- Validation report from this run: `results=305 passes=13431 failures=0 failingResults=0`

## Limitations And P14B-P2 Recommendation

- P14B-P1 does not render WAVs.
- P14B-P1 does not establish golden pass/fail tone targets.
- P14B-P1 does not perform robust FFT or aliasing analysis.
- P14B-P1 does not tune any thresholds, gain staging, dynamics timings, drive curves, or output behavior.

Recommended P14B-P2 scope:

- Add explicit offline artifact generation for reviewed JSON/CSV baselines.
- Add WAV rendering for reference and sweep excerpts.
- Add a real FFT/spectral analysis path before making any aliasing, fizz, or inharmonic-energy claims.
- Decide which measured baselines should become reviewed golden metrics.
