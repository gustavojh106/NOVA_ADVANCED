# P8D Targeted Pedal Gap Closure Matrix

Date: 2026-05-08

Scope: targeted technical gap closure for the P8C `NEEDS_MORE_TESTS` pedals. No DSP surgery, revoicing, routing change, schema/ID change, UI change, preset change, golden baseline update, DAW smoke, Reaper/P7F work, or Distortion manual listening completion was performed.

## Classification Update

| Processor | P8C status | P8D added coverage | P8D status | Remaining gap |
| --- | --- | --- | --- | --- |
| Wah / ClassicWahPedal | `NEEDS_MORE_TESTS` | full sweep, resonance extreme, DC bias, strong peaks, fast automation, bypass/unbypass, mix=0 settled transparency, legacy aliases | `READY_TECHNICAL` | manual expression/listening QA |
| Classic Amp | `NEEDS_MORE_TESTS` | strong input, drive/tone/presence/depth/level extremes via public parameter API, automation, DC, bypass/unbypass | `READY_TECHNICAL` | subjective British voicing QA |
| High Gain Amp | `NEEDS_MORE_TESTS` | high-gain palm-mute synthetic input, strong input, gain/tight/presence/level automation, near-clip dominance guard, DC, bypass/unbypass | `READY_TECHNICAL` | manual high-gain feel/listening QA |
| Chime Amp | `NEEDS_MORE_TESTS` | bright spike input, treble/bass/brilliance/level extremes, adjacent-delta proxy, DC, bypass/unbypass | `READY_TECHNICAL` | manual bright/jangle QA |
| Boutique Amp | `NEEDS_MORE_TESTS` | drive/warmth/mid/presence/master extremes, strong input, automation, DC, bypass/unbypass | `READY_TECHNICAL` | manual sag/dynamic feel QA |
| Vintage 2x12 | `NEEDS_MORE_TESTS` | Classic Amp -> Vintage 2x12 chain, strong input, DC bias, high peaks, parameter automation, bypass/unbypass | `READY_TECHNICAL` | manual cab tone/feel QA |
| Modern 4x12 | `NEEDS_MORE_TESTS` | High Gain Amp -> Modern 4x12 chain, strong input, DC bias, high peaks, parameter automation, bypass/unbypass | `READY_TECHNICAL` | manual high-gain cab QA |
| CabinetPedal wrapper | `READY_TECHNICAL` | wrapper automation and bypass coverage refreshed as part of cabinet test | `READY_TECHNICAL` | manual cab variant QA |

## Final Technical Classification

`READY_TECHNICAL`: Compressor, Noise Gate, EQ, Boost, Neural, Overdrive, Distortion, Fuzz, Octave, Chorus, Phaser, Flanger, Tremolo, Delay, Reverb, Clean Amp, Cabinet, Wah, Classic Amp, High Gain Amp, Chime Amp, Boutique Amp, Vintage 2x12, Modern 4x12.

`NEEDS_MORE_TESTS`: none from the P8C targeted gap list.

`NEEDS_TARGETED_SURGERY`: none found in P8D.

## Manual QA Remaining

- Distortion manual listening QA remains pending and is not completed.
- Wah expression feel, amp voicing feel, cabinet feel, and full pedal-by-pedal subjective listening QA remain outside P8D.
- P7F/Reaper remains pending and was not touched.
