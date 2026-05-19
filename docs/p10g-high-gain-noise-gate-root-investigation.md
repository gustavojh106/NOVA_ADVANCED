# P10G High-Gain Noise / Gate Root Investigation

## User Post-P10F Findings

- HighGainAmp -> Modern4x12 sounds good and must be preserved.
- Boost -> HighGainAmp -> Modern4x12 is much cleaner after P10F; palm mutes are tight and the helicopter artifact is mostly gone.
- Distortion can still feel like it turns off or collapses volume in some cases.
- Boost/Distortion into HighGainAmp can leave a ground-like noise floor.
- A low Noise Gate setting around 4% is useful with Fuzz, but high-gain routes needed far more gate for near silence.
- High-gain still had cheap fizz and a slightly choked feel as gain rose.

## Fuzz Reference Comparison

Fuzz remains the positive reference for gate feel, not for tone. The important behavior is that its internal gate and pre-clip filtering reduce idle noise while leaving the played body and sustain intact. P10G keeps Fuzz code unchanged and adds `p10g_fuzz_low_gate_reference` so later high-gain work cannot redefine the reference by accident.

Representative P10G reference render:

| Chain | idleNoiseRms | postPhraseNoiseRms | noiseToSignalRatio | activeRms | gateTransitions | clipped/invalid |
|---|---:|---:|---:|---:|---:|---:|
| Noise Gate 4% -> Fuzz -> ClassicAmp -> Cabinet | ~0.0092 | ~0.0704 | ~0.113 | ~0.622 | 0 | 0 / 0 |

The Fuzz route has a higher post-phrase musical tail than the cleaned high-gain routes, but it remains stable, non-chattering, and free of clipping.

## Root Cause

The remaining high-gain problem was not OutputChain level. It came from early-stage behavior:

- Distortion high-gain modes used an adaptive pre-saturation gate with very low thresholds and a high floor. That combination could leave low-level hiss open while still moving enough during phrases to feel like ducking.
- Distortion generated too much upper presence before the final tone shaping in Metal/Studio modes, so fizz remained even when the output was bounded.
- HighGainAmp treated very low idle hiss the same as valid guitar input before the high-gain stages, so upstream Boost/Distortion noise could be magnified into ground-like idle noise.
- HighGainAmp presence and post-presence filtering were slightly too permissive for the Modern4x12 pairing under upstream dirt.

## Fix Direction

P10G applies only local stage conditioning:

- Distortion: lower integrated-gate floor for true idle, raise high-gain gate thresholds into a useful range, slow close/release to avoid note chopping, and add envelope-based idle rejection before high-gain clipping.
- Distortion: reduce Studio/Metal presence and top-cut mappings enough to reduce cheap fizz without making the pedal dull.
- HighGainAmp: add input noise rejection before preamp saturation using a fast-open, slow-close envelope. This rejects idle hiss before gain multiplication while preserving active attack.
- HighGainAmp: slightly reduce presence shelf gain and post-presence low-pass ceiling.

No OutputChain masking, global output reduction, schema change, golden update, known-failure addition, UI/UX, deploy, Reaper smoke, or factory approval was used.

## Gate Sweep Interpretation

The P10G tests model the important low-setting behavior directly with `configureLowGate(..., 0.04f)` and a deterministic phrase containing idle hiss, hum, active notes, sustain, and post-phrase silence. The low gate is expected to improve idle noise and preserve active RMS; it is not required to hard-mute every post-phrase musical tail.

Manual listening should still sweep 0%, 4%, 8%, 15%, 30%, and 50%. The expected result after P10G is that 4% becomes meaningfully useful for idle noise, while higher settings remain available for tight staccato silence.

## Preservation Notes

- Fuzz sound/behavior: preserved by no code edits to Fuzz and by `p10g_fuzz_low_gate_reference`.
- HighGainAmp -> Modern4x12 baseline: preserved by `p10g_highgain_baseline_preservation`.
- Boost P10F improvement: guarded by `p10g_boost_highgain_ground_noise_guard`.
- Clean Studio, Wide Ambient Clean, Clean Amp, and clean Chorus/Delay/Reverb: not revoiced in P10G; clean preservation remains covered by existing validation.
- Manual listening QA general remains pending.
- Distortion/high-gain listening QA remains pending.
- P7F/Reaper remains pending.
