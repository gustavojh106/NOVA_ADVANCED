# P10C High-Gain Professionalization Audit Results

Date: 2026-05-17

## Scope

P10C focused only on high-gain gain staging and deterministic voicing proxies. It did not touch UI/UX, DAW/Reaper smoke, factory approval, schema/IDs, golden baselines, or manual-listening status.

Manual listening QA general remains pending. Distortion/high-gain listening QA remains pending. P7F/Reaper remains pending.

## Diagnostic Coverage Added

The JUCE validation suite now includes deterministic P10C scenarios:

| Scenario | Chain focus |
|---|---|
| `p10c_high_gain_amp_nominal_palm_mute` | HighGainAmp nominal palm-mute response |
| `p10c_high_gain_amp_extreme_gain_bounded` | HighGainAmp extreme gain/presence bound |
| `p10c_distortion_highgainamp_modern4x12_nominal` | Distortion -> HighGainAmp -> Modern4x12 |
| `p10c_boost_highgainamp_modern4x12_nominal` | Boost -> HighGainAmp -> Modern4x12 |
| `p10c_fuzz_classicamp_cabinet_nominal` | Fuzz -> ClassicAmp -> Cabinet |
| `p10c_distortion_cleanamp_cabinet_nominal` | Distortion -> CleanAmp -> Cabinet |
| `p10c_high_gain_chain_bypass_recovery` | High-gain boost bypass/unbypass recovery |
| `p10c_high_gain_chain_outputchain_limiter_independence` | High-gain chain before OutputChain limiting |

Metrics captured by the harness: peak, RMS, DC, nearClipSamples, clippedSamples, invalidSamples, adjacentDeltaPeak, brightnessProxy, rumbleProxy, limiterTouchedSamples, limiterActiveBlocks, and sustainedClampBlocks.

## Before-Fix Findings

Initial P10C run after compiling the new diagnostics failed two high-gain scenarios:

| Scenario | Failing stage/window | Peak | RMS | DC | nearClip | clipped | invalid | adjacentDeltaPeak | brightnessProxy | rumbleProxy |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `p10c_high_gain_amp_extreme_gain_bounded` | HighGainAmp extreme output | 2.6788 | 1.2294 | 0.04825 | 17615 | 17222 | 0 | 3.0885 | 0.1163 | 0.6546 |
| `p10c_distortion_highgainamp_modern4x12_nominal` | Final Modern4x12 cabinet output | 3.2895 | 0.8111 | 0.00060 | 5765 | 5518 | 0 | 4.1528 | 0.1988 | 0.3471 |

The signal stayed finite and low-DC, so this was not an RT/stability issue. The failure was local gain staging: HighGainAmp output was too hot, and Modern4x12 wet IR compensation was too high for sustained high-gain material.

## Root Cause

Probable root cause:

- HighGainAmp sums four saturation stages and exposes a wide `hgLevel` range without enough internal make-down gain.
- Modern4x12 uses a synthetic IR normalized by peak, not by sustained-energy behavior, so dense high-gain material can be amplified well above the expected cabinet output range.
- OutputChain was not the cause and was not changed.

## Fix Applied

Two local gain-stage trims were applied:

- `Source/Effects/Amplifiers/HighGainAmp.h`: post-amp professional output trim `0.34`.
- `Source/Effects/Cabinets/Modern4x12Cabinet.h`: Modern4x12 wet-only cabinet trim `0.70`.

These are stage-specific trims, not a global output-volume reduction and not OutputChain limiter masking.

