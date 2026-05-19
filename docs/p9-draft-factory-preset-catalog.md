# P9 Draft Factory Preset Catalog

Date: 2026-05-08

Status: NOT FINAL. These are draft technical concepts for future listening and gain staging. No preset in this document is approved for shipping.

## Draft Presets

| Name | Category | Intended chain | Pedals used | Purpose | Readiness | Risk notes | Listening QA required | Gain staging notes | P9B serialization status | Reason | Next action |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Clean Studio | Clean | Input -> Clean Amp -> Cabinet -> Output | Clean Amp, Cabinet | Neutral clean baseline for DI checks and general playing. | DRAFT_TECHNICAL | Needs real pickup range check. | Clean chords, single notes, bypass level. | Keep output headroom; no limiter activity expected. | GENERATED_DRAFT | P9D generated draft; round-trip/process finite pass. | Manual listening before any promotion. |
| Glass Clean | Clean | Input -> Compressor -> Clean Amp -> Vintage 2x12 -> Output | Compressor, Clean Amp, Vintage 2x12 | Brighter articulate clean. | DRAFT_TECHNICAL | Could become harsh with single coils. | Open chords, staccato, lead sustain. | Compressor makeup must not raise noise floor too much. | NOT_GENERATED | Not in the small P9B subset. | Reconsider after core six drafts round-trip cleanly. |
| Edge Breakup | Edge of Breakup | Input -> Boost -> Boutique Amp -> Vintage 2x12 -> Output | Boost, Boutique Amp, Vintage 2x12 | Touch-sensitive almost-clean drive. | DRAFT_TECHNICAL | Pickup output strongly affects breakup. | Guitar volume cleanup, open chords. | Avoid relying on limiter for amp push. | NOT_GENERATED | Not in the small P9B subset. | Add after clean/crunch gain targets are measured. |
| Classic Crunch | Crunch | Input -> Overdrive -> Classic Amp -> Cabinet -> Output | Overdrive, Classic Amp, Cabinet | Mid-gain rhythm crunch. | DRAFT_TECHNICAL | Bypass jump between OD on/off. | Palm mutes, chords, bypass. | Match OD level to amp input. | GENERATED_DRAFT | P9D generated draft; round-trip/process finite pass. | Validate OD bypass level in listening flow. |
| Tight Modern Rhythm | High Gain | Input -> Noise Gate -> Boost -> High Gain Amp -> Modern 4x12 -> Output | Noise Gate, Boost, High Gain Amp, Modern 4x12 | Tight palm-muted rhythm. | DRAFT_TECHNICAL | Gate threshold and low-end buildup. | Palm mutes, staccato, silence. | Keep nearClipSamples rare; no sustained clamp. | GENERATED_DRAFT | P9D generated draft; round-trip/process finite pass; high-gain listening remains pending. | Distortion/high-gain manual listening before any promotion. |
| Lead Delay | Lead | Input -> Overdrive -> Classic Amp -> Cabinet -> Delay -> Output | Overdrive, Classic Amp, Cabinet, Delay | Focused lead with repeats. | DRAFT_TECHNICAL | Delay overload from lead gain. | Single notes, lead sustain, bypass/tails. | Delay input should stay controlled. | NOT_GENERATED | Deferred to keep P9B subset small. | Add after Classic Crunch and ambience tail metrics are stable. |
| Wide Ambient Clean | Ambient | Input -> Clean Amp -> Chorus -> Delay -> Reverb -> Cabinet -> Output | Clean Amp, Chorus, Delay, Reverb, Cabinet | Wide clean ambience. | DRAFT_TECHNICAL | Tail buildup and stereo phase. | Open chords, silence between notes, tails. | Reverb/delay mix must leave dry clarity. | GENERATED_DRAFT | P9D generated draft; round-trip/process finite pass. | Tail listening and silence recovery before promotion. |
| Worship Wide Clean | Worship / Wide Clean | Input -> Compressor -> Clean Amp -> Chorus -> Delay -> Reverb -> Output | Compressor, Clean Amp, Chorus, Delay, Reverb | Wide clean support tone. | DRAFT_TECHNICAL | Can become too wet or too loud on swells. | Swells, chords, tails. | Output should remain consistent with Clean Studio. | NOT_GENERATED | Overlaps with Wide Ambient Clean for P9B coverage. | Add after Wide Ambient Clean listening notes exist. |
| Funk Comp Clean | Funk / Compressor Clean | Input -> Compressor -> EQ -> Clean Amp -> Cabinet -> Output | Compressor, EQ, Clean Amp, Cabinet | Snappy clean rhythm. | DRAFT_TECHNICAL | Compressor pumping. | Staccato, muted chords, single notes. | Preserve attack; avoid makeup gain jump. | GENERATED_DRAFT | P9D generated draft; round-trip/process finite pass. | Validate pumping and transient level in listening flow. |
| Blues Drive | Blues | Input -> Overdrive -> Boutique Amp -> Vintage 2x12 -> Output | Overdrive, Boutique Amp, Vintage 2x12 | Warm expressive blues drive. | DRAFT_TECHNICAL | Taste-dependent mids and sag feel. | Single notes, lead sustain, dynamics. | Match bypassed OD level carefully. | NOT_GENERATED | Deferred to avoid expanding beyond representative six drafts. | Add after Classic Crunch drive staging is proven. |
| Chime Jangle | Modulated Clean | Input -> Clean Amp -> Chime Amp? -> Vintage 2x12 -> Output | Chime Amp, Vintage 2x12 | Bright pop/jangle clean concept. | DRAFT_TECHNICAL | Chain must use one amp only; final chain should be Chime Amp -> Vintage 2x12. | Open chords, single notes. | Watch upper-mid harshness. | BLOCKED | Catalog concept still carries correction note; do not generate until chain is canonical. | Rewrite final template as one amp plus one cabinet before generation. |
| Trem Clean Pulse | Modulated Clean | Input -> Clean Amp -> Tremolo -> Cabinet -> Output | Clean Amp, Tremolo, Cabinet | Rhythmic clean pulse. | DRAFT_TECHNICAL | Trem depth can drop perceived level. | Sustained chords, rate changes. | Match perceived level with trem on. | NOT_GENERATED | Deferred to keep P9B subset small. | Add after dry/clean reference level is measured. |
| Phase Crunch | Classic Rock | Input -> Overdrive -> Classic Amp -> Phaser -> Cabinet -> Output | Overdrive, Classic Amp, Phaser, Cabinet | Classic movement over crunch. | DRAFT_TECHNICAL | Phaser placement and phase weirdness. | Chords, single notes, bypass. | Avoid resonance volume spikes. | NOT_GENERATED | Deferred to avoid extra modulation coverage in P9B. | Add after Classic Crunch is serialized and measured. |
| Shoegaze Wall | Shoegaze / Wall | Input -> Fuzz -> Clean Amp -> Chorus -> Delay -> Reverb -> Cabinet/Output | Fuzz, Clean Amp, Chorus, Delay, Reverb | Dense wall texture. | DRAFT_TECHNICAL | High risk for tail buildup and limiter pumping. | Chords, sustain, silence recovery. | Start conservative; validate limiter inactivity. | BLOCKED | Higher risk and ambiguous cabinet/output placement. | Clarify canonical chain and gain target before generation. |
| Experimental Swell | Experimental | Input -> Volume-style automation if available -> Delay -> Reverb -> Chorus -> Output | Delay, Reverb, Chorus | Nontraditional ambient texture. | DRAFT_TECHNICAL | Needs tool support and may not map to current controls. | Swells, tails, bypass. | Keep marked experimental until proven. | BLOCKED | Depends on unsupported/undefined volume-style automation. | Define supported controls before serialization. |
| Dry Reference | Utility / Calibration | Input -> Output | None | Dry DI comparison and level reference. | DRAFT_TECHNICAL | Not a musical preset. | Clean DI reference only. | Establish reference peak window. | GENERATED_DRAFT | P9D generated draft; round-trip/process finite pass. | Keep as technical reference; not a musical approval. |
| Low CPU Clean | Utility / Calibration | Input -> Clean Amp -> Output | Clean Amp | Simple low CPU sanity preset. | DRAFT_TECHNICAL | Missing cabinet may sound direct. | Clean DI, open chords. | Track CPU and output headroom. | NOT_GENERATED | Deferred because Dry Reference covers utility baseline in P9B. | Add after CPU metric target is defined. |

## Notes

- `Chime Jangle` intentionally records a correction note: final generated state must use one amp only.
- No `.nova-preset` files are generated in P9 because the factory bank location and metadata format are not final.
- P9B adds `Resources/Presets/DraftFactory/factory-bank.draft.json` as a manifest-only draft bank and still generates no `.nova-preset` files.
- Names are unique in this draft catalog.
- Manual listening QA general remains pending.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.

## P9C Round-trip Gate Update

P9C validation script result: `BLOCKED_SAFE_NO_GENERATION`. No draft `.nova-preset` files were created.

- Clean Studio: P9C serialization status `BLOCKED`; round-trip `NOT_GENERATED`; process finite `NOT_GENERATED`. Next action: add safe non-shipping builder before generation.
- Classic Crunch: P9C serialization status `BLOCKED`; round-trip `NOT_GENERATED`; process finite `NOT_GENERATED`. Next action: add safe non-shipping builder before generation.
- Tight Modern Rhythm: P9C serialization status `BLOCKED`; round-trip `NOT_GENERATED`; process finite `NOT_GENERATED`. Distortion listening remains pending; keep high-gain listening separate.
- Wide Ambient Clean: P9C serialization status `BLOCKED`; round-trip `NOT_GENERATED`; process finite `NOT_GENERATED`. Manifest order was corrected to `Amp -> FX -> Cabinet`; generate only after safe builder exists, then measure tails.
- Funk Comp Clean: P9C serialization status `BLOCKED`; round-trip `NOT_GENERATED`; process finite `NOT_GENERATED`. Next action: add safe non-shipping builder before generation.
- Dry Reference: P9C serialization status `BLOCKED`; round-trip `NOT_GENERATED`; process finite `NOT_GENERATED`. Next action: use as first P9D builder smoke case.

## P9D Side-effect-free Builder Update

P9D generated the six core draft files under `Resources/Presets/DraftFactory/generated/`:

- Clean Studio: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`.
- Classic Crunch: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`.
- Tight Modern Rhythm: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`; Distortion/high-gain listening remains pending.
- Wide Ambient Clean: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`.
- Funk Comp Clean: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`.
- Dry Reference: `GENERATED_DRAFT`, `ROUND_TRIP_PASS`, `PROCESS_FINITE_PASS`.

These are still technical drafts only. Manual listening QA general remains pending, Distortion manual listening QA remains pending, and P7F/Reaper remains pending.
