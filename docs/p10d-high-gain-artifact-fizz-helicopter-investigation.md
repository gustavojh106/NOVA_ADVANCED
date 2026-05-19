# P10D High-Gain Artifact, Fizz & Helicopter Investigation

Date: 2026-05-18

## Manual WARN

Manual listening after P10C reported that clean and clean-effect tones are stable and excellent, while high-gain remains WARN: excessive fizz, audible artifacts, occasional clipping, and intermittent helicopter/tremolo-like modulation. Manual listening QA general remains pending. Distortion/high-gain listening QA remains pending. P7F/Reaper remains pending.

## Scope

P10D investigated high-gain technical proxies only. It did not touch UI/UX, DAW/Reaper smoke, deploy, factory approval, schema/IDs, golden baselines, known failures, or OutputChain limiting.

Chains covered by deterministic P10D guards:

| Chain | Signal/proxy focus |
|---|---|
| HighGainAmp solo | sustained input helicopter/modulation guard |
| HighGainAmp -> Modern4x12 | sustain, fizz, strong-input bounds |
| Boost -> HighGainAmp -> Modern4x12 | sustain, strong palm mute/chord bounds |
| Distortion -> HighGainAmp -> Modern4x12 | integrated gate chatter, fizz, clipping bounds |
| Tight Modern Rhythm | Noise Gate -> Boost -> HighGainAmp -> Modern4x12 phrase/recovery guard |
| Distortion -> CleanAmp -> Cabinet | retained from P10C coverage |
| Fuzz -> ClassicAmp -> Cabinet | retained from P10C coverage |

Signals covered: repeated palm mute, strong staccato, long sustain, silence/recovery, low-E burst, moderate input, and strong input.

## Metrics Added

P10D extends the P10C window metrics with:

- `modulationDepth3To20`: low-frequency amplitude-modulation proxy on the rectified envelope.
- `blockRmsVariance` and `peakVariance`: block-to-block instability proxies.
- `highFrequencyEnergyProxy`: post-cab fizz proxy focused above the upper guitar-cab band.
- `gateTransitions` and `gateDeltaPeak`: gate open/close chatter and hard gain-step proxies.

Existing P10C metrics remain in use: peak, RMS, DC, nearClip, clipped, invalid, adjacentDeltaPeak, brightnessProxy, rumbleProxy, limiterTouched, limiterActiveBlocks, and sustainedClampBlocks.

## Findings

Probable root causes:

- Fizz/harshness: HighGainAmp allowed a wide post-saturation presence/top-end path, and Modern4x12 still had a bright effective presence shelf plus a high top cut for dense high-gain material.
- Helicopter/tremolo risk: deterministic sustained HighGainAmp scenarios did not indicate runaway instability, so the likely source is gate/envelope interaction in high-gain phrase recovery, especially Distortion's integrated high-gain gate and external Noise Gate phrase transitions.
- Clipping: P10C fixed the gross high-gain level overs. P10D strong-input scenarios now guard against final nearClip/clipped samples without relying on OutputChain.

Representative P10D diagnostic readings during calibration:

| Scenario | Key measurement |
|---|---|
| HighGainAmp -> Modern4x12 strong chord | post-cab peak `0.5625`, nearClip `0`, clipped `0`, highFrequencyEnergyProxy `0.0313` |
| Modern4x12 low-E high-gain burst | post-cab peak `0.3676`, nearClip `0`, clipped `0`, highFrequencyEnergyProxy `0.0236` |
| Tight Modern Rhythm phrase/recovery | Noise Gate transitions `2`; no repeated chatter detected |

## Fixes Applied

- `HighGainAmp`: added a post-presence low-pass stage and reduced internal high-shelf/presence extremes. This targets fizz locally inside the amp, not OutputChain.
- `Modern4x12Cabinet`: lowered the top-cut range and compensated the effective presence shelf so the cab remains focused under high-gain material.
- `DistortionPedal`: softened adaptive gate behavior for Metal/Studio high-gain modes by raising gate floor, increasing hysteresis, and lengthening close release.
- `AudioEngineTests`: added P10D deterministic guards for helicopter modulation, fizz, strong-input clipping, Tight Modern Rhythm artifacts, Distortion -> HighGainAmp -> Modern4x12 bounds, Noise Gate chatter, and Modern4x12 fizz control.

## Status

Technical status: WARN-to-candidate. P10D passes deterministic safety/artifact proxies, but subjective high-gain listening must remain WARN until P10E manual listening confirms feel, fizz, attack, and professional voicing.

Clean impact: clean amps and clean ambient processors were not changed. Clean Studio, Wide Ambient Clean, Clean Amp, chorus/delay/reverb clean paths, draft preset validation, and golden metrics must remain separately validated.

Recommendation for P10E: run focused manual listening on the P10D chains with real DI material and compare palm mutes, staccato recovery, lead sustain, fizz, and perceived output against P10C.
