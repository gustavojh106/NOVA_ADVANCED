# P10F High-Gain Architecture Root Fix Results

## Fixes Applied

P10F keeps Fuzz as the reference and changes only the failing architecture around Distortion, Boost, and HighGainAmp.

Applied changes:
- Distortion Metal/Studio adaptive gate now uses lower thresholds, a higher non-zero floor, slower release, and gentler gain/tight threshold lift.
- Distortion high-gain stage 1 now uses static high-gain compatibility trim before the clip network, reducing dependence on final containment while preserving character.
- Boost now applies static amp-input compatibility trim at higher boost amounts before its local output containment.
- HighGainAmp now conditions hot preamp input with a static musical atan curve before sag, and sag depth is reduced so hot upstream drive does not become audible ducking.
- P10F tests measure Fuzz reference behavior and guard Distortion/Boost/HighGainAmp against active-input volume collapse.
- `scripts/check-audio-thread-policy.ps1` now checks P10F docs, scenarios, ducking metrics, Fuzz reference guard, no schema bump, no golden baseline update, no known failures, no OutputChain-only masking, no factory approval, pending manual listening, P7F/Reaper pending, and clean preservation notes.

## Before / After

Before:
- Fuzz -> ClassicAmp -> Cabinet was the positive reference.
- Distortion/Boost/HighGainAmp routes could feel like the volume dropped while playing.
- Existing P10D/P10E checks caught clipping, mute, fizz, helicopter modulation, and stuck-state risks, but did not directly model active-input gain collapse.

After:
- P10F adds direct active-input ducking metrics.
- Distortion integrated gate is less likely to close while input is active.
- Boost is less likely to overdrive HighGainAmp into non-musical dynamic sag.
- HighGainAmp handles hot input as preamp drive instead of defensive gain reduction.
- OutputChain remains unchanged and is not the fix.

## Impact

Fuzz impact:
- No Fuzz DSP code changed.
- Fuzz reference behavior is measured by `p10f_fuzz_reference_gain_behavior`.

Clean Impact:
- Clean Studio and Wide Ambient Clean are documented as preserved.
- Clean Amp DSP was not changed.
- Chorus/Delay/Reverb clean paths were not changed.

HighGainAmp -> Modern4x12 impact:
- The base high-gain amp/cab route remains explicitly guarded by `p10f_highgainamp_internal_ducking_guard`.
- HighGainAmp input conditioning is static and only affects hotter input above the preamp conditioning knee.

## P10F Scenarios

- `p10f_fuzz_reference_gain_behavior`
- `p10f_distortion_gain_ducking_guard`
- `p10f_distortion_highgain_ducking_guard`
- `p10f_boost_highgain_ducking_guard`
- `p10f_highgainamp_internal_ducking_guard`
- `p10f_noise_gate_low_setting_reference`
- `p10f_highgain_noise_floor_after_silence`
- `p10f_highgain_no_perceptible_volume_collapse`

## Current Validation

Status: PASS for required technical gates.

Executed validation:
- build NOVA_SharedCode Debug x64: PASS.
- build NOVA_SharedCode Release x64: PASS.
- build NOVA_StandalonePlugin Debug x64: PASS.
- build NOVA_StandalonePlugin Release x64: PASS.
- build NOVA_VST3 Release x64: PASS.
- `scripts/run-base-audio-validation.ps1`: PASS, `results=245 passes=7294 failures=0 failingResults=0`.
- second consecutive `scripts/run-base-audio-validation.ps1`: PASS, `results=245 passes=7294 failures=0 failingResults=0`.
- `scripts/run-golden-audio-metrics.ps1`: PASS against existing baseline, no baseline update.
- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release`: PASS, `total=16 pass=16 warn=0 fail=0`.
- `scripts/run-rt-profile-stability.ps1 -Configuration Release -CiMode -Runs 3`: PASS, every scenario `3/0/0`, blocking events `0`.
- `scripts/check-audio-thread-policy.ps1`: PASS, `contractChecks=497`, `failures=0`, `contractFailures=0`.
- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release`: PASS.
- `scripts/run-diagnostics-bundle.ps1`: PASS; bundle reports base validation `results=245 passes=7294 failures=0 failingResults=0`, RT profile `16/16/0/0`, RT stability `passRuns=3 warnRuns=0 failRuns=0 blockingEvents=0`, policy PASS.
- `scripts/generate-draft-factory-presets.ps1`: PASS, generated 6 draft presets.
- `scripts/validate-draft-factory-presets.ps1`: PASS, manifest unchanged.

## Remaining WARN

Manual listening is not marked PASS by P10F. The next listening pass should compare:
- Fuzz -> ClassicAmp -> Cabinet as the reference.
- Distortion -> CleanAmp -> Cabinet.
- Distortion -> HighGainAmp -> Modern4x12.
- Boost -> HighGainAmp -> Modern4x12.
- HighGainAmp -> Modern4x12.
- Each high-gain route with low Noise Gate around 4%.

Manual listening QA general remains pending.
Distortion/high-gain listening QA remains pending.
P7F/Reaper remains pending.
