# P11A Amp Interface And Circuit Audit

Date: 2026-05-19

## Scope

P11A audited the active amplifier catalog only:

- Clean Amp
- Classic Amp
- High Gain Amp
- Chime Amp
- Boutique Amp

No cabinet work, global UI redesign, schema bump, OutputChain masking, golden baseline update, deploy, factory approval, or Reaper smoke was included.

## Active Files

All active amps are registered through `Source/Core/PedalRegistry.h` and cataloged in `Source/Core/PedalCatalog.h`.

Processor/editor ownership:

- `Source/Effects/Amplifiers/CleanAmp.h`: processor plus inline editor wiring through `PremiumPedalEditor`.
- `Source/Effects/Amplifiers/ClassicAmp.h`: processor plus inline editor wiring through `PremiumPedalEditor`.
- `Source/Effects/Amplifiers/HighGainAmp.h`: processor plus inline editor wiring through `PremiumPedalEditor`.
- `Source/Effects/Amplifiers/ChimeAmp.h`: processor plus inline editor wiring through `PremiumPedalEditor`.
- `Source/Effects/Amplifiers/BoutiqueAmp.h`: processor plus inline editor wiring through `PremiumPedalEditor`.
- `Source/Core/PluginEditor.cpp`: pedalboard slot/card rendering for placed modules.
- `Source/GUI/Widgets/DraggableButton.h`: quick-add thumbnail rendering.

There were no dedicated amp dashboard or thumbnail files before this phase. The amps shared the same generic pedal editor and generic slot/quick-add rendering.

## Public Parameters Before P11A

Clean Amp:

- `cleanDrive`
- `cleanBass`
- `cleanTreble`
- `cleanReverb`
- `cleanLevel`

Classic Amp:

- `ampDrive`
- `ampTone`
- `ampPresence`
- `ampDepth`
- `ampLevel`

High Gain Amp:

- `hgDrive`
- `hgTone`
- `hgPresence`
- `hgTight`
- `hgLevel`

Chime Amp:

- `chimeDrive`
- `chimeTrebleCut`
- `chimeBassCut`
- `chimeBrill`
- `chimeLevel`

Boutique Amp:

- `boutDrive`
- `boutWarmth`
- `boutMid`
- `boutPres`
- `boutLevel`

## Internal Controls Not Well Exposed

- Clean Amp had an internal clean triode/headroom behavior but no user-facing headroom control.
- Classic Amp had internal sag behavior fixed inside the amp core and no explicit brightness/pre-voice trim.
- High Gain Amp had internal input-noise rejection and sag/input feel but no user-facing feel control; low-end resonance was only indirectly affected by tone/tight.
- Chime Amp had cathode-bias sag fixed as a constant, despite being central to the model.
- Boutique Amp had envelope-responsive gain behavior fixed around one touch sensitivity.

## Inconsistencies

- Parameter naming mixed common amp terms with model-specific names. This is acceptable per model, but shared intent was not consistently exposed.
- All amps had five controls, even where the internal model already had more meaningful dimensions.
- Clean Amp was more mature technically than the other amps: bounded reverb, telemetry, DC protection, and explicit safety handling.
- Classic/HighGain/Chime/Boutique were mature enough to keep, but their useful internal behavior was less adjustable.
- Visual treatment did not communicate "amp head" strongly. Placed amp slots looked like generic pedal cards.

## Proposed Amp Standard

Front panel controls should remain compact and model-specific, but with a shared hierarchy:

- Gain section: `Drive`
- Tone section: model-appropriate tone controls (`Tone`, `Bass`, `Treble`, `Presence`, `Brilliance`, `Warmth`, `Mid`)
- Feel/power section: `Headroom`, `Sag`, `Feel`, or `Touch` depending on amp topology
- Output section: `Master`

Common rules:

- Preserve existing parameter IDs.
- Add new IDs only for processor-local parameters with defaults that keep old behavior close.
- Smooth gain/feel parameters on the DSP rate used by the amp.
- Keep dynamic controls local to amp stages; do not mask with OutputChain or final global limiters.
- Keep UI in the same NOVA family, but amp slots should read as amp heads rather than stompboxes.

## Phase Recommendation

P11A should be treated as the first amp professionalization pass:

- Add missing processor-local controls for internal amp behavior.
- Improve slot/thumbnail amp identity.
- Add deterministic coverage for parameter surface, state round-trip, tonal stability, clean preservation, and high-gain baseline preservation.
- Defer larger shared amp base-class refactors until the controls prove out in manual listening.
