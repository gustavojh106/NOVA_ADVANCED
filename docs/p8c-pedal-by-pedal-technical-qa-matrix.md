# P8C Pedal-by-pedal Technical QA Matrix

Date: 2026-05-07

Scope: technical QA inventory and deterministic safety coverage for active NOVA processors. This phase did not revoice pedals, change DSP, change routing, change schema/IDs, update factory presets, update golden baselines, run DAW smoke, or complete the Distortion listening check.

## Classification Key

- `READY_TECHNICAL`: current technical coverage is enough for this phase; manual listening can still remain pending.
- `NEEDS_MORE_TESTS`: no critical regression found, but coverage is thinner than the mature pedals or needs more pedal-specific musical tests.
- `NEEDS_TARGETED_SURGERY`: a reproducible technical failure needs a scoped future phase before readiness.
- `LEGACY_QUARANTINED`: present in source but intentionally not active/registered.
- `NOT_ACTIVE`: not present in active catalog/registry routing.

## Active Inventory

| Processor | Active file | Zone | Registered | Existing coverage | P8C coverage | Risk | Classification | Recommendation |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Compressor | `Source/Effects/Pedals/Compressor/CompressorPedal.h` | Pre/FX | Yes | round-trip, legacy restore, blend=0 transparency, GR behavior, focus, linked stereo, automation, P7C fallback | catalog strong input, generic automation extremes, bypass transitions | makeup/parallel gain under edge settings | `READY_TECHNICAL` | Keep; add listening QA later for pumping feel. |
| Noise Gate | `Source/Effects/Pedals/Gate/NoiseGatePedal.h` | Pre/FX | Yes | render finite, state/automation coverage in base suite | catalog strong input, generic automation extremes, bypass transitions | click feel under extreme threshold/release still a listening topic | `READY_TECHNICAL` | Keep; manual click audit later. |
| EQ | `Source/Effects/Pedals/EQ/EQPedal.h` | Pre/FX | Yes | round-trip, legacy restore, default transparency, filters, automation | catalog strong input, generic automation extremes, bypass transitions | high boost can raise level by design | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Boost | `Source/Effects/Pedals/Boost/BoostPedal.h` | Pre/FX | Yes | round-trip, gain/character, tight control, automation | catalog strong input, generic automation extremes, bypass transitions | can intentionally push downstream processors | `READY_TECHNICAL` | Keep; chain-level listening later. |
| Neural | `Source/Effects/Pedals/Neural/NeuralPedal.h` | Pre/FX | Yes | round-trip, mix=0, focus, automation, P7C fallback/helpers | catalog strong input, generic automation extremes, bypass transitions | model-style nonlinear drive still deserves manual QA | `READY_TECHNICAL` | Keep; no targeted surgery found. |
| Overdrive | `Source/Effects/Pedals/Overdrive/OverdrivePedal.h` | Pre/FX | Yes | channel isolation, mix=0, round-trip, tone/texture, automation, RT policy | catalog strong input, generic automation extremes, bypass transitions | mature coverage | `READY_TECHNICAL` | Keep as reference safety pattern. |
| Distortion | `Source/Effects/Pedals/Distortion/DistortionPedal.h` | Pre/FX | Yes | round-trip, legacy migration, mix=0, modes, tight/gate, P8A containment, P8B real-use QA | catalog strong input, generic automation extremes, bypass transitions | manual listening QA still pending | `READY_TECHNICAL` | Technical PASS; do not mark manual listening PASS. |
| Fuzz | `Source/Effects/Pedals/Fuzz/FuzzPedal.h` | Pre/FX | Yes | round-trip, mix=0, modes, gate/bias decay, automation, P7C fallback | catalog strong input, generic automation extremes, bypass transitions | bias/starve musical edge cases | `READY_TECHNICAL` | Keep; manual velcro/decay QA later. |
| Wah | `Source/Effects/Pedals/Wah/ClassicWahPedal.h` | Pre/FX | Yes | unified modern/legacy state, P7C fallback | catalog strong input, generic automation extremes, bypass transitions | fewer pedal-specific signal tests than modulation peers | `NEEDS_MORE_TESTS` | Add a future targeted wah sweep/DC/resonance test if P8D focuses gaps. |
| Octave | `Source/Effects/Pedals/Octave/OctavePedal.h` | Pre/FX | Yes | round-trip, dry-only, sub/upper voice, tone, automation | catalog strong input, generic automation extremes, bypass transitions | tracking behavior across guitar material | `READY_TECHNICAL` | Keep; manual tracking QA later. |
| Chorus | `Source/Effects/Pedals/Chorus/ChorusPedal.h` | Pre/FX | Yes | round-trip, mix=0, modes, automation | catalog strong input, generic automation extremes, bypass transitions | high depth/width listening feel | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Phaser | `Source/Effects/Pedals/Phaser/PhaserPedal.h` | Pre/FX | Yes | round-trip, mix=0, depth/feedback, modes, automation, DC rejection, NaN scrub | catalog strong input, generic automation extremes, bypass transitions | prior DC risk closed in P7C | `READY_TECHNICAL` | Keep; no targeted surgery found. |
| Flanger | `Source/Effects/Pedals/Flanger/FlangerPedal.h` | Pre/FX | Yes | round-trip, mix=0, modes, automation, feedback/DC/NaN/high peak P7C tests | catalog strong input, generic automation extremes, bypass transitions | feedback resonance by design | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Tremolo | `Source/Effects/Pedals/Tremolo/TremoloPedal.h` | Pre/FX | Yes | round-trip, legacy restore, mix=0, stereo width, harmonic/bias, automation | catalog strong input, generic automation extremes, bypass transitions | discontinuity feel under extreme rate/depth is listening QA | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Delay | `Source/Effects/Pedals/Delay/DelayPedal.h` | Pre/FX | Yes | round-trip, tail, stereo field, modes, freeze/duck/reverse/swell, automation, feedback/DC/NaN/high peak | catalog strong input, generic automation extremes, bypass transitions | long-tail musical QA remains | `READY_TECHNICAL` | Keep; no duplicate P8C delay test needed. |
| Reverb | `Source/Effects/Pedals/Reverb/ReverbPedal.h` | Pre/FX | Yes | round-trip, tails, stereo field, modes, freeze/duck/swell/gate/reverse, P7H configure/automation policy | catalog strong input, generic automation extremes, bypass transitions | mature but manual ambience QA remains | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Classic Amp | `Source/Effects/Amplifiers/ClassicAmp.h` | Amp | Yes | P7B RT closure, P8C generic | catalog strong input, generic automation extremes, bypass transitions | lacks amp-specific round-trip/voicing tests | `NEEDS_MORE_TESTS` | Add amp-specific state and strong-input tests in a future QA phase. |
| High Gain Amp | `Source/Effects/Amplifiers/HighGainAmp.h` | Amp | Yes | P7B RT closure, RT profile scenario, P8C generic | catalog strong input, generic automation extremes, bypass transitions | high gain can push downstream by design | `NEEDS_MORE_TESTS` | Add targeted high-gain chain tests before tonal QA. |
| Clean Amp | `Source/Effects/Amplifiers/CleanAmp.h` | Amp | Yes | P7B RT closure, prior clean-path/chains, P8B chain use, P8C generic | catalog strong input, generic automation extremes, bypass transitions | internal ambience/level behavior needs listening | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Chime Amp | `Source/Effects/Amplifiers/ChimeAmp.h` | Amp | Yes | P7B RT closure, P8C generic | catalog strong input, generic automation extremes, bypass transitions | bright voicing spike tests are thin | `NEEDS_MORE_TESTS` | Add bright-input spike and state tests later. |
| Boutique Amp | `Source/Effects/Amplifiers/BoutiqueAmp.h` | Amp | Yes | P7B RT closure, P8C generic | catalog strong input, generic automation extremes, bypass transitions | sag/gain-specific tests are thin | `NEEDS_MORE_TESTS` | Add boutique-specific state/dynamic tests later. |
| Cabinet | `Source/Effects/Cabinets/CabinetPedal.h` | Cabinet | Yes | Synthetic IR window, P7C fallback/coefs, P8B chain use, P8C generic | catalog strong input, generic automation extremes, bypass transitions | convolution/level variants need state tests | `READY_TECHNICAL` | Keep; no P8C surgery. |
| Vintage 2x12 | `Source/Effects/Cabinets/Vintage2x12Cabinet.h` | Cabinet | Yes | P7C fallback/coefs, P8C generic | catalog strong input, generic automation extremes, bypass transitions | variant-specific round-trip not explicit | `NEEDS_MORE_TESTS` | Add cabinet-variant state tests later. |
| Modern 4x12 | `Source/Effects/Cabinets/Modern4x12Cabinet.h` | Cabinet | Yes | P7C fallback/coefs, P8C generic | catalog strong input, generic automation extremes, bypass transitions | high-gain cabinet chain specifics need more coverage | `NEEDS_MORE_TESTS` | Add high-gain -> Modern4x12 chain test later. |

## Legacy / Not Active

| Processor | File | Registry state | Classification | Recommendation |
| --- | --- | --- | --- | --- |
| Auto Wah legacy | `Source/Effects/Pedals/Wah/AutoWahPedal.h` | Not registered; aliases canonicalize to `Wah` | `LEGACY_QUARANTINED` | Keep quarantine unless a separate replacement/removal phase is approved. |
| Metal Distortion legacy | `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` | Not registered; alias canonicalizes to `Distortion` | `LEGACY_QUARANTINED` | Keep quarantine; do not reactivate without dedicated DSP QA. |
| Compressor legacy root header | `Source/Effects/Pedals/CompressorPedal.h` | Not registered; superseded | `LEGACY_QUARANTINED` | Keep quarantine. |
| Chorus legacy root header | `Source/Effects/Pedals/ChorusPedal.h` | Not registered; superseded | `LEGACY_QUARANTINED` | Keep quarantine. |

## Tests Added In P8C

- `P8C active pedal catalog processors remain finite under strong input`
- `P8C active pedal catalog automation extremes remain finite`
- `P8C active pedal catalog bypass transitions remain bounded`

These tests exercise all 24 active catalog entries through the registry. They are not tonal tests and do not replace manual listening QA.

## Pedals Ready

Technical-ready active processors for this phase: Compressor, Noise Gate, EQ, Boost, Neural, Overdrive, Distortion, Fuzz, Octave, Chorus, Phaser, Flanger, Tremolo, Delay, Reverb, Clean Amp, Cabinet.

## Pedals Needing More Tests

Wah, Classic Amp, High Gain Amp, Chime Amp, Boutique Amp, Vintage 2x12, Modern 4x12.

No active processor was classified as `NEEDS_TARGETED_SURGERY` by P8C.

## Manual Listening QA Remaining

- Distortion manual listening QA remains pending and is not completed.
- Pedal-by-pedal subjective tone, feel, pumping, modulation smoothness, tracking, cabinet feel, and amp voicing QA remain outside P8C.
