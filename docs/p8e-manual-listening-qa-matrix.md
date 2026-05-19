# P8E Manual Listening QA Matrix

Date: 2026-05-08

Scope: readiness matrix for future manual listening QA with real guitar DI. This document does not record subjective results and does not mark manual listening QA as complete.

## Status Guardrails

- Manual listening QA general remains pending until executed with real evidence.
- Distortion manual listening QA remains pending and is not completed.
- P7F/Reaper remains pending and is not completed.
- Factory presets are not final.
- UI/UX is not final.
- No DSP/audio-path, revoicing, routing, schema/ID, golden baseline, or known-failure change is authorized by this matrix.

## Base Chains

| Chain | Routing |
| --- | --- |
| Clean baseline | Input -> Clean Amp -> Cabinet -> Output |
| Drive baseline | Input -> Overdrive/Distortion/Fuzz -> Clean Amp or Classic Amp -> Cabinet -> Output |
| High gain baseline | Input -> Distortion/Boost -> High Gain Amp -> Modern 4x12 -> Output |
| Ambient baseline | Input -> Clean Amp -> Chorus -> Delay -> Reverb -> Output |
| Modulation baseline | Input -> Clean Amp -> Modulation -> Cabinet/Output |
| Wah baseline | Input -> Wah -> Overdrive/Distortion optional -> Amp -> Cabinet -> Output |
| Time FX stress | Input -> Delay -> Reverb -> Chorus -> Output |

## Common Signal Set

Use repeatable DI passages where possible: palm mutes, open chords, single notes, lead sustain, staccato, silence between notes, and clean DI reference.

## Common Parameter Sweep

For every pedal, test default, nominal musical setting, extreme high, extreme low, bypass/unbypass, and manual or automation movement where applicable. Test `mix=0` and `mix=1` when the processor exposes a mix/wet/blend control.

## Common Listening Targets

Listen for clipping, pumping, dullness, harshness, rumble/DC, zipper noise, clicks, recovery/tails, volume jump, loss of character, noise floor, stereo weirdness, and phase weirdness.

## Severity Guidance

| Severity | Meaning |
| --- | --- |
| P0 | Audio runaway, crash, unsafe output, or monitor-threatening behavior. Stop the session and preserve evidence. |
| P1 | Pedal unusable, severe clipping, state corruption, or broken bypass/session behavior. |
| P2 | Audible issue with a practical workaround or narrow parameter range. |
| P3 | Taste, preset/gain staging, documentation, or minor subjective concern. |

## Dynamics / Gain Pedals

| Pedal | Objective | Recommended chain | Recommended signal | Parameters | What to hear | Expected result | Severity notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Compressor | Confirm controlled dynamics without obvious artifacts. | Clean baseline, then Input -> Compressor -> Clean Amp -> Cabinet -> Output. | Palm mutes, open chords, single notes, lead sustain, staccato, silence. | Default, low/high sustain or ratio, attack/release movement, level makeup, bypass/unbypass. | Pumping, dull attack, volume jump, noise floor lift, clicks. | Smoother sustain and attack control with stable level and no runaway noise. | P1 if it clips or jumps loudly; P2 for obvious pumping at nominal settings. |
| Noise Gate | Confirm gating threshold and release feel. | Input -> Noise Gate -> Clean Amp -> Cabinet -> Output, plus high gain baseline. | Silence between notes, staccato, palm mutes, lead sustain, clean DI. | Default, low/high threshold, fast/slow release if available, bypass/unbypass. | Chatter, clipped note tails, pumping, noise floor, clicks. | Noise reduces during silence while musical tails recover naturally. | P1 if gate locks closed/open; P2 if nominal threshold cuts normal notes. |
| EQ | Confirm tonal shaping remains musical and level safe. | Clean baseline with EQ before amp and after amp if supported by zone rules. | Open chords, single notes, palm mutes, clean DI. | Default, broad boosts/cuts, extreme high/low bands, output level, bypass/unbypass. | Harshness, dullness, rumble/DC, volume jump, phase weirdness. | Bands shape tone predictably without unexpected clipping or phase collapse. | P1 for severe clipping at nominal gain; P2 for large unintended level shifts. |
| Boost | Confirm clean/colored gain staging. | Input -> Boost -> Clean Amp -> Cabinet -> Output, plus Boost -> High Gain Amp -> Modern 4x12. | Palm mutes, open chords, lead sustain, silence. | Default, low/high boost, tone if available, bypass/unbypass. | Clipping, harshness, rumble, volume jump, noise floor. | Boost increases drive or level predictably and remains controllable. | P0/P1 for unsafe output; P2 for nominal harshness or noise lift. |
| Neural | Confirm neural amp/pedal model remains stable and characterful. | Input -> Neural -> Cabinet -> Output, then Neural -> Delay/Reverb stress. | Clean DI, single notes, open chords, palm mutes, sustain. | Default, gain/level/tone extremes, model selection if available, bypass/unbypass. | Loss of character, clipping, dullness, harshness, noise floor, zipper. | Model has recognizable character, stable gain, and no artifact bursts. | P1 for instability or unusable model; P2 for audible artifacts under normal play. |
| Overdrive | Confirm touch response and amp push. | Drive baseline with Clean Amp and Classic Amp. | Palm mutes, open chords, single notes, lead sustain. | Default, gain/tone/level low-high, mix=0/1 if available, bypass/unbypass, knob movement. | Clipping, harshness, dullness, volume jump, loss of character. | Musical breakup, distinct from Distortion/Fuzz, no unexpected limiter-like flattening. | P2 for nominal dullness/harshness; P1 if bypass or level is broken. |
| Distortion | Prepare focused manual verification without passing it. | Drive baseline, high gain baseline, Distortion -> Reverb -> Chorus. | Palm mutes, open chords, lead sustain, staccato, silence. | Default, mode variants, gain/tone/level extremes, mix=0/1, bypass/unbypass, movement. | Clipping, pumping, dullness, harshness, recovery/tails, volume jump, loss of Metal/Studio/Classic character. | Distortion remains aggressive and bounded, with modes still distinct. Manual listening QA remains pending until evidence is recorded. | P1 for severe clipping or broken bypass; P2 for containment audibly flattening normal settings. |
| Fuzz | Confirm fuzz character and cleanup behavior. | Drive baseline into Clean Amp/Classic Amp/Cabinet. | Single notes, open chords, lead sustain, guitar volume cleanup if available. | Default, fuzz/gain/tone/level extremes, mix=0/1 if available, bypass/unbypass. | Harsh fizz, dullness, DC/rumble, volume jump, loss of character. | Saturated fuzz remains intentional, stable, and distinct from Overdrive/Distortion. | P1 for runaway low end or severe clipping; P2 for unusable nominal tone. |
| Octave | Confirm tracking and blend behavior. | Input -> Octave -> Clean Amp -> Cabinet -> Output; also Octave -> Drive baseline. | Single notes, staccato, lead sustain, limited chords, silence. | Default, octave level/blend, tracking-sensitive extremes, mix=0/1, bypass/unbypass. | Tracking glitches, zipper, rumble, phase weirdness, volume jump. | Single-note octave tracks musically with expected limits on complex chords. | P2 for normal-note tracking artifacts; P3 for expected chord limitations. |

## Modulation

| Pedal | Objective | Recommended chain | Recommended signal | Parameters | What to hear | Expected result | Severity notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Chorus | Confirm width, movement, and mono compatibility. | Ambient baseline and modulation baseline. | Open chords, single notes, lead sustain, clean DI. | Default, low/high rate/depth, mix=0/1, stereo width if available, bypass/unbypass. | Stereo weirdness, phase weirdness, dullness, clicks, zipper. | Lush modulation with stable center image and transparent mix=0. | P2 for phase collapse or zipper at normal movement. |
| Phaser | Confirm sweep musicality and feedback stability. | Modulation baseline before/after Clean Amp as zone allows. | Open chords, single notes, staccato, lead sustain. | Default, rate/depth/feedback extremes, mix=0/1, bypass/unbypass. | Harsh resonance, volume jump, zipper, phase weirdness. | Smooth sweep without spikes or unstable resonance. | P1 for runaway resonance; P2 for nominal zipper/clicks. |
| Flanger | Confirm comb sweep and feedback containment. | Modulation baseline, plus Time FX stress. | Open chords, single notes, staccato, silence. | Default, rate/depth/manual/feedback extremes, mix=0/1, bypass/unbypass. | Metallic harshness, phase weirdness, clipping, zipper, volume jump. | Flange remains animated and bounded through feedback extremes. | P1 for runaway feedback; P2 for severe nominal phase/level artifacts. |
| Tremolo | Confirm rhythmic amplitude modulation and level consistency. | Modulation baseline after Clean Amp, with Cabinet/Output. | Open chords, lead sustain, staccato. | Default, rate/depth/waveform extremes, mix=0/1 if available, bypass/unbypass, movement. | Clicks, volume jump, pumping beyond expected modulation, zipper. | Tremolo depth/rate are obvious but level-safe and click-free. | P2 for clicks or large bypass level mismatch. |
| Wah | Confirm sweep feel, resonance, and optional drive interaction. | Wah baseline. | Single notes, lead sustain, palm mutes, open chords. | Default, sweep low/high, resonance/drive if available, mix=0/1 if available, bypass/unbypass, manual movement. | Harsh peak, dull sweep, zipper, volume jump, phase weirdness. | Sweep is expressive and bounded; movement is smooth without zipper. | P1 for unsafe resonance; P2 for nominal sweep artifacts. |

## Time / Ambience

| Pedal | Objective | Recommended chain | Recommended signal | Parameters | What to hear | Expected result | Severity notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Delay | Confirm repeats, feedback, mix, and recovery. | Ambient baseline and Time FX stress. | Staccato, single notes, lead sustain, silence between notes. | Default, low/high time/feedback/mix, mix=0/1, tempo/manual movement, bypass/unbypass. | Clicks, zipper, runaway feedback, dullness, recovery/tails, volume jump. | Repeats are musical, tails recover predictably, and feedback remains bounded. | P0/P1 for runaway feedback; P2 for zipper/clicks during normal changes. |
| Reverb | Confirm tails, decay, mix, and downstream stability. | Ambient baseline, Distortion -> Reverb stress, Time FX stress. | Staccato, open chords, lead sustain, silence. | Default, room/decay/damping/mix extremes, mix=0/1, bypass/unbypass, movement. | Tail cut, tail contamination, rumble, harshness, recovery/tails, volume jump. | Reverb tail sounds natural, bounded, and recovers after bypass or upstream changes. | P1 for runaway tail or severe clipping; P2 for nominal tail artifacts. |

## Amps

| Pedal | Objective | Recommended chain | Recommended signal | Parameters | What to hear | Expected result | Severity notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Clean Amp | Confirm clean headroom and base tone. | Clean baseline. | Clean DI, open chords, single notes, lead sustain. | Default, low/high gain/EQ/master, bypass/unbypass, movement. | Clipping, dullness, harshness, volume jump, noise floor. | Clean tone remains clear, responsive, and level-safe. | P1 for nominal clipping; P2 for dull or harsh base tone. |
| Classic Amp | Confirm mid-gain British-style response. | Input -> Classic Amp -> Cabinet/Vintage 2x12 -> Output, plus Overdrive into Classic Amp. | Palm mutes, open chords, lead sustain. | Default, gain/EQ/presence/master extremes, bypass/unbypass. | Harshness, dullness, loss of character, volume jump, rumble. | Mid-gain crunch feels distinct from Clean and High Gain. | P2 for subjective voicing concerns; P1 for severe clipping. |
| High Gain Amp | Confirm tight high-gain feel. | High gain baseline. | Palm mutes, staccato, single notes, lead sustain, silence. | Default, gain/tight/presence/depth/master extremes, bypass/unbypass. | Clipping, low-end rumble, noise floor, harshness, volume jump. | High gain is aggressive, tight, and bounded. | P1 for unusable clipping/noise; P2 for normal-setting muddiness/harshness. |
| Chime Amp | Confirm bright/jangle response without ice-pick highs. | Input -> Chime Amp -> Cabinet/Vintage 2x12 -> Output. | Open chords, single notes, clean DI, lead sustain. | Default, treble/bass/brilliance/level extremes, bypass/unbypass. | Harshness, dullness, stereo/phase weirdness, volume jump. | Bright voice remains articulate and musical. | P2 for nominal harshness; P3 for taste-only brightness. |
| Boutique Amp | Confirm dynamic sag/warmth feel. | Input -> Boutique Amp -> Cabinet -> Output, plus Boost/Overdrive into Boutique Amp. | Single notes, open chords, lead sustain, palm mutes. | Default, drive/warmth/mid/presence/master extremes, bypass/unbypass. | Pumping, dullness, loss of character, volume jump, clipping. | Responsive boutique voice with stable level and musical compression. | P2 for unwanted pumping at nominal settings. |

## Cabinets

| Pedal | Objective | Recommended chain | Recommended signal | Parameters | What to hear | Expected result | Severity notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CabinetPedal | Confirm generic cabinet filtering and bypass. | Clean baseline, Drive baseline, High gain baseline. | Open chords, palm mutes, single notes, clean DI. | Default, cab selection/size/tone/mic if available, mix=0/1 if available, bypass/unbypass. | Dullness, harshness, rumble/DC, phase weirdness, volume jump. | Cabinet shapes amp tone naturally without unexpected level or phase problems. | P1 for severe phase/level break; P2 for normal-setting tonal imbalance. |
| Vintage 2x12 | Confirm vintage cabinet pairing. | Input -> Classic Amp -> Vintage 2x12 -> Output. | Open chords, single notes, lead sustain. | Default, available cab tone/mic controls, bypass/unbypass. | Dullness, harshness, rumble, loss of character, volume jump. | Vintage 2x12 supports Classic/Chime voices with musical midrange. | P2 for strong mismatch at nominal settings; P3 for taste. |
| Modern 4x12 | Confirm high-gain cabinet pairing. | Input -> Distortion/Boost -> High Gain Amp -> Modern 4x12 -> Output. | Palm mutes, staccato, lead sustain, silence. | Default, available cab tone/mic controls, bypass/unbypass. | Rumble, harsh fizz, phase weirdness, noise floor, volume jump. | Modern 4x12 keeps high gain focused, tight, and bounded. | P1 for unsafe low end/clipping; P2 for nominal fizz or mud. |

## Session Decision Rule

Record each case as `PASS`, `WARN`, or `FAIL` only after real listening evidence exists. A `WARN` or `FAIL` should be filed with the P8E issue template and routed to no action, more listening, technical investigation, targeted surgery, preset/gain staging adjustment, or UI/UX note.
