# P11C Amp/Cab Open UI Redesign Audit

Date: 2026-05-19

## Scope

P11C audits and redesigns only the open editor UI for amplifiers and cabinets. It does not change DSP, parameter IDs, parameter ranges, state schema, preset serialization, automation, routing, OutputChain, or pedal editor design.

## Pedal Reference

The pedal reference remains the existing premium pedal language:

- Dark NOVA surface with restrained accent glow.
- Clear title hierarchy.
- Rotary controls with numeric readouts.
- Compact stompbox-style identity.
- Per-effect accent color.

Pedals are intentionally not redesigned in this phase.

## Amplifier UI Audit

Active amp processors:

- `Source/Effects/Amplifiers/CleanAmp.h`
- `Source/Effects/Amplifiers/ClassicAmp.h`
- `Source/Effects/Amplifiers/HighGainAmp.h`
- `Source/Effects/Amplifiers/ChimeAmp.h`
- `Source/Effects/Amplifiers/BoutiqueAmp.h`

Before P11C, all active amps used `PremiumPedalEditor`, so their open editor felt like a larger pedal rather than an amplifier head. The parameter bindings were already complete after P11A, but the visual hierarchy did not communicate channel, tone stack, voicing, or power-section identity strongly enough.

Existing amp parameter bindings remain unchanged:

- Clean Amp: Drive, Bass, Treble, Reverb, Headroom, Master.
- Classic Amp: Drive, Tone, Presence, Depth, Sag, Bright, Master.
- High Gain Amp: Drive, Tone, Presence, Tight, Resonance, Feel, Master.
- Chime Amp: Drive, Treble, Bass, Brilliance, Sag, Master.
- Boutique Amp: Drive, Warmth, Mid, Presence, Touch, Master.

## Cabinet UI Audit

Active cabinet processors:

- `Source/Effects/Cabinets/CabinetPedal.h`
- `Source/Effects/Cabinets/Vintage2x12Cabinet.h`
- `Source/Effects/Cabinets/Modern4x12Cabinet.h`

Before P11C, active cabinets also used `PremiumPedalEditor`. P11B added useful controls, but the open editor still did not visually read as a speaker/IR module. It lacked a cabinet grille/speaker identity and a clear final-voicing section.

Existing cabinet parameter bindings remain unchanged:

- Cabinet: Thump, Air, Resonance, Low Cut, High Cut, Distance, Mix, Level.
- Vintage 2x12: Warmth, Sparkle, Resonance, Low Cut, High Cut, Distance, Mix, Level.
- Modern 4x12: Low End, Presence, Resonance, Low Cut, High Cut, Distance, Mix, Level.

## Standard Direction

Amps:

- Use a wider amp-head panel.
- Strong header with amp name as the protagonist.
- Drive and Master get side positions like input/channel and output/power controls.
- Middle controls sit in a tone/voicing/power section.
- Visual language: dark channel strip, subtle meter rail, restrained accent glow.

Cabinets:

- Use a wider speaker/IR module.
- Header identifies the module as `SPEAKER / IR`.
- Central grille and speaker visual distinguish cabinets from amps and pedals.
- Controls sit in a final voicing/cuts/room section.
- Visual language: dark cabinet enclosure, grille lines, speaker geometry, restrained accent glow.

## Risks

- Larger editor sizes need enough modal overlay space, but existing overlay uses a viewport and can host larger editors.
- This phase changes visual presentation only; manual visual QA is still required.
- No manual listening approval is claimed.
