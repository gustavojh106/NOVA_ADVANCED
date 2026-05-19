# P11B Cabinet Interface And Voicing Audit

Date: 2026-05-19

## Scope

P11B audited the active cabinet catalog:

- Cabinet
- Vintage 2x12
- Modern 4x12

No global UI redesign, OutputChain masking, schema bump, golden baseline update, deploy, factory approval, or Reaper smoke was included.

## Active Files

Active cabinet processors:

- `Source/Effects/Cabinets/CabinetPedal.h`
- `Source/Effects/Cabinets/Vintage2x12Cabinet.h`
- `Source/Effects/Cabinets/Modern4x12Cabinet.h`

Shared cabinet helper:

- `Source/Effects/Cabinets/SyntheticIR.h`

Integration points:

- `Source/Core/PedalCatalog.h`
- `Source/Core/PedalRegistry.h`
- `Source/Core/PluginEditor.cpp`
- `Source/GUI/Widgets/DraggableButton.h`
- `Source/GUI/Widgets/AssetItem.cpp`

Legacy/simple editor:

- `Source/Effects/Cabinets/CabinetEditor.h`

The active cabinets do not use `CabinetEditor.h`; they create `PremiumPedalEditor` instances inline.

## Parameters Before P11B

Cabinet:

- `cabThump`
- `cabAir`
- `cabDistance`
- `cabMix`
- `cabLevel`

Vintage 2x12:

- `v2x12Warmth`
- `v2x12Sparkle`
- `v2x12Distance`
- `v2x12Mix`
- `v2x12Level`

Modern 4x12:

- `m4x12Low`
- `m4x12Presence`
- `m4x12Distance`
- `m4x12Mix`
- `m4x12Level`

## Internal Behavior Not Exposed Well

- Each cabinet already had a low-pass and high-pass contour driven indirectly by `Distance`.
- Each cabinet had speaker/body resonance in the synthetic IR, but no user-facing resonance trim after convolution.
- High-cut/fizz control was indirect. Users had to alter `Distance` or presence/air, which also changed perceived mic distance.
- Low-cut/rumble control was indirect. Users had to use `Distance`, which also changed top-end behavior.
- The generic Cabinet had local output containment; Vintage 2x12 and Modern 4x12 relied on gain staging and the Modern trim.

## UI Gaps

- Placed cabinet slots used the generic card style and did not communicate speaker cabinet identity.
- Quick-add cabinet thumbnails had a basic icon only; they did not visually match the polish level of pedals or the P11A amp slots.
- Editors used the shared premium control layout, which is consistent but lacked enough cabinet-specific controls.

## Proposed Cabinet Standard

Shared front panel:

- Body / low voice: `Thump`, `Warmth`, or `Low End`
- Top voice: `Air`, `Sparkle`, or `Presence`
- Resonance: cabinet-body or speaker-resonance trim
- Low Cut: explicit rumble control
- High Cut: explicit fizz/top cutoff control
- Distance: mic/room distance proxy
- Mix
- Level

Defaults should preserve current behavior as closely as possible:

- Low Cut defaults should not raise existing distance-driven high-pass unless the user turns them up.
- High Cut defaults should sit above the existing distance-driven low-pass where possible.
- Resonance defaults to `0 dB` so old presets recall close to the old tone.

## Implementation Direction

The safe first phase is to add processor-local parameters and post-IR filters, then validate stability and known good paths. Larger IR-generation changes are deferred because they would affect all tonal baselines more broadly.
