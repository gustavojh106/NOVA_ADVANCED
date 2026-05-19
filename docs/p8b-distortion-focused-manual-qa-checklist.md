# P8B Distortion Focused Manual QA Checklist

Scope: focused listening verification for the P8A DistortionPedal containment. Do not change factory presets, UI, routing, Reverb, Chorus, OutputChain, or golden baselines from this checklist.

## Setup

- Use a clean guitar DI with repeatable palm mutes, open chords, and lead sustain.
- Start with LineB only, input gain around -11 dB, gainB around 2.0, output volume around -3 dB, and limiter around -12 dB.
- Keep A/B level matching practical; judge character after matching perceived loudness.

## Cases

- Metal mode, high gain, mix=1, level medium-high.
- Studio mode, nominal gain, mix=1, level unity-like.
- Classic mode, nominal gain, mix=1, level unity-like.
- Distortion -> Amp.
- Distortion -> Cabinet.
- Distortion -> Reverb.
- Distortion -> Reverb -> Chorus.
- Distortion bypass/unbypass while sustaining a note.
- Distortion bypass/unbypass during palm mutes.
- mix=0 transparency check.
- mix=1 full wet check.
- high level check near the top of the musical range.

## Listen For

- Loss of Distortion character after P8A containment.
- Pumping or level breathing that follows the soft ceiling rather than the playing.
- Dullness or flattened attack that suggests excessive containment in nominal cases.
- DC or low-frequency rumble after high-gain activation.
- Reverb tail contamination after Distortion bypass.
- Slow recovery after bypassing Distortion.
- Perceived volume too low compared with pre-P8A expectations.
- Perceived volume too high into Reverb/Chorus/OutputChain.
- Extreme clicks on bypass/unbypass.
- Metal mode still feeling distinct from Studio/Classic modes.

## Pass Criteria

- Nominal Studio/Classic/Metal sounds remain distinct.
- Metal high gain is aggressive but does not destroy downstream ambience.
- Reverb/Chorus tails recover after bypass in a bounded window.
- mix=0 sounds dry and does not add audible coloration.
- No obvious rumble, sustained limiter clamp, or click spike appears in normal playing.
