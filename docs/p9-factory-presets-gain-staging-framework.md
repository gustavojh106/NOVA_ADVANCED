# P9 Factory Presets / Gain Staging Framework

Date: 2026-05-08

Scope: technical and documentation framework for future NOVA factory presets and gain staging. P9 does not approve factory presets, execute manual listening QA, run DAW smoke, change schema/IDs, or create final preset files.

## Current Preset Support Audit

| Area | Current state | P9 decision |
| --- | --- | --- |
| User preset file format | `SessionPersistence::savePresetToFile` writes canonical `ValueTree` data to `.nova-preset`. | Reuse for future draft/user presets; no schema change in P9. |
| User preset directory | `PluginProcessor.cpp` uses `%APPDATA%/NOVA/Presets`. | Current safe location for user-visible presets. Factory bank needs a clearer product decision before adding more generated files. |
| Startup restore | `SessionPersistence` writes `startup-preset.txt` pointing to the last saved/loaded preset. | Avoid generating new presets in P9 because seeding can affect user startup/library behavior. |
| Existing bundled/factory seed | `PluginProcessor.cpp` seeds Delay flagship presets named `Factory - Orbit ...` into the user preset directory if missing. | Document as existing infrastructure, not final factory bank policy. |
| State canonicalization | `PluginStateModel` canonicalizes root structure, schema, globals, chain zones, aliases, amp/cab uniqueness, and invalid values. | Safe foundation for future draft presets without schema bump. |
| Round-trip tests | P7D covers save-load-save canonical compare, every catalog pedal, routing modes, bypass state, corrupt payload rejection, and schema clamp. | Future preset files should reuse P7D patterns or add non-fragile catalog-level tests in P9B. |
| Metadata | Current saved state has engine/settings/line/pedal state. No formal preset `name`, `category`, `description`, `author`, `tags`, `version`, `input target`, or `output target` schema IDs were found. | Keep metadata in docs/catalog for now. Do not add schema IDs in P9. |
| Factory bank structure | No standalone versioned factory bank directory or manifest was found. | Propose a bank structure below, but do not implement it in P9. |

## Proposed Factory Bank Structure

Use this structure when product is ready to ship presets:

```text
Factory Bank: NOVA Factory Drafts
Version: 0.x while under listening QA
Author: NOVA
Preset records:
  - name
  - category
  - description
  - tags
  - readiness
  - intended input
  - output target
  - chain
  - file path, once generated
```

Potential future storage options:

- Keep a manifest in `Resources/Presets/factory-bank.json` and generated `.nova-preset` files beside it.
- Seed into `%APPDATA%/NOVA/Presets` only after naming, duplicate handling, and version migration are defined.
- Store rich metadata outside the current `NOVA_STATE` tree until a schema bump is justified.

## Categories

- Clean
- Edge of Breakup
- Crunch
- Classic Rock
- High Gain
- Lead
- Ambient
- Modulated Clean
- Worship / Wide Clean
- Funk / Compressor Clean
- Blues
- Shoegaze / Wall
- Experimental
- Utility / Calibration

## Naming Convention

Example names:

- Clean Studio
- Glass Clean
- Edge Breakup
- Classic Crunch
- Tight Modern Rhythm
- Wide Ambient Clean
- Lead Delay
- Funk Comp Clean
- Blues Drive
- Shoegaze Wall

Rules:

- Use short names that fit preset browsers.
- Do not use names like `Preset 1`, `New Preset`, or `Factory Test`.
- Do not promise artist, song, brand, or trademark likeness.
- Avoid real amp/cab brand names in preset titles.
- Prefer descriptive tone/use names: clean, crunch, lead, rhythm, ambient, wide, tight, glass, wall, calibration.
- Keep any internal draft prefix outside the final preset name where possible, for example in a manifest readiness field.

## Gain Staging Rules

Internal targets for future factory candidates:

- Recommended input nominal: clean DI peaks around `-12 dBFS` to `-6 dBFS` before NOVA input gain.
- Preset input gain should normally stay near `0 dB`; use negative input gain only for hot pickup/high-gain templates.
- Drives and amps may saturate musically, but a preset must not rely on the global limiter to sound controlled.
- Delay, Reverb, and Chorus should not receive destructive input from upstream drives.
- Bypassing the main drive/amp/modulation pedal should not cause an extreme perceived volume jump.
- Output should leave headroom for real playing variation.
- Nominal final peak target: roughly `-9 dBFS` to `-3 dBFS` on repeatable DI material.
- Strong-play final peak guard: avoid sustained peaks above roughly `-1 dBFS`.
- RMS/LUFS relative target, if tooling is available later: presets in the same category should sit within a small perceived loudness window after matching input DI.
- `nearClipSamples` should be zero or rare for nominal factory candidates.
- Limiter activity should be zero or very low in nominal presets.
- No sustained OutputChain clamp is acceptable for a factory candidate.

These are review targets, not golden baselines. They should guide QA and leveling without forcing baseline updates.

## Chain Templates

| Template | Chain |
| --- | --- |
| Clean | Input -> Clean Amp -> Cabinet -> Output |
| Drive | Input -> Overdrive/Distortion/Fuzz -> Clean/Classic Amp -> Cabinet -> Output |
| High Gain | Input -> Boost/Distortion -> High Gain Amp -> Modern 4x12 -> Output |
| Ambient | Input -> Clean Amp -> Chorus -> Delay -> Reverb -> Output |
| Modulation | Input -> Clean Amp -> Chorus/Phaser/Flanger/Tremolo -> Cabinet/Output |
| Wah | Input -> Wah -> Overdrive/Distortion -> Amp -> Cabinet -> Output |
| Utility | Input calibration / dry reference / low CPU test if applicable |

## Preset Readiness Levels

| Level | Meaning | Allowed in P9 |
| --- | --- | --- |
| DRAFT_TECHNICAL | Chain idea is technically valid and serializable, but not leveled or listening-approved. | Yes |
| LISTENING_CANDIDATE | Draft is ready for real guitar DI listening and level checks. | Yes |
| FACTORY_CANDIDATE | Passed initial listening and technical level review; needs final product review. | No |
| FACTORY_APPROVED | Approved for shipping factory bank. Requires real listening evidence and product signoff. | No |
| REJECTED | Removed from consideration. | Yes, if a draft is clearly unsuitable |

P9 may only create `DRAFT_TECHNICAL` or `LISTENING_CANDIDATE` records. No preset is factory approved until manual listening QA and final review are complete.

## Validation Rules For Future Presets

- Preset name is unique in the target bank.
- Category is one of the approved categories or explicitly marked experimental.
- Chain uses registered `PedalCatalog` / `PedalRegistry` type IDs.
- State saves, loads, and saves again canonically.
- No schema bump is required for draft content.
- No unknown pedal types, invalid zones, duplicate amp/cab in a single line, or excess flex-zone count.
- Output stays finite under nominal DI and strong DI.
- Limiter activity is documented.
- Bypass level of the main gain/amp/modulation stage is checked.
- Manual listening status remains pending until evidence exists.
- Distortion manual listening QA remains pending until focused evidence exists.
- P7F/Reaper remains pending until a real DAW smoke session is completed.
