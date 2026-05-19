# P10A Six Draft Preset Listening Run Pack

Status: NOT_RUN

## Purpose

Prepare the first real manual listening QA session for the six generated draft presets. This pack defines setup, capture, scoring, severity, evidence, and issue routing. It does not execute listening QA, does not invent subjective results, and does not approve factory presets.

## Presets Under Test

Use the original generated drafts under `Resources/Presets/DraftFactory/generated/`:

1. `Dry-Reference.nova-preset`
2. `Clean-Studio.nova-preset`
3. `Funk-Comp-Clean.nova-preset`
4. `Classic-Crunch.nova-preset`
5. `Tight-Modern-Rhythm.nova-preset`
6. `Wide-Ambient-Clean.nova-preset`

## Listening Order

1. Dry Reference
2. Clean Studio
3. Funk Comp Clean
4. Classic Crunch
5. Tight Modern Rhythm
6. Wide Ambient Clean

## Setup To Record

Record these values before judging any preset:

- NOVA build or executable path.
- Host or standalone mode.
- Sample rate.
- Buffer size.
- Audio interface.
- Guitar, pickup position, DI source, or reamp source.
- Input gain setting.
- Output gain setting.
- Monitoring chain and playback level.
- Operating system and driver mode.
- Any external processing, which should normally be disabled.

Recommended start point: unity host fader, conservative interface input gain, NOVA input and output unchanged from the draft preset unless the test explicitly records a calibration change. If the Dry Reference preset clips or feels unusably low, stop and document the setup before changing gain.

## Loading Each Preset

Load each `.nova-preset` from `Resources/Presets/DraftFactory/generated/`. Do not seed it into `%APPDATA%/NOVA/Presets`, do not change `startup-preset.txt`, and do not overwrite the generated source file during listening.

## What To Play

Use repeatable short phrases:

- Clean DI reference.
- Open chords.
- Single notes across low, middle, and high strings.
- Lead sustain.
- Staccato notes.
- Palm mutes where relevant.
- Muted rhythmic chords.
- Silence between phrases for recovery and tail behavior.

## What To Listen For

- Clipping or crackle.
- Pumping or unstable gain recovery.
- Excessive dullness.
- Excessive harshness or fizz.
- Rumble, DC-like offset, or low-end runaway.
- Zipper noise during control movement if controls are touched.
- Clicks, pops, or discontinuities.
- Tail buildup, runaway, or poor recovery after silence.
- Large volume jump compared with Dry Reference or neighboring presets.
- Noise floor problems.
- Stereo image or phase weirdness.
- Any audible dependence on limiter-like clamping.

## Result Criteria

`PASS`: No material listening issue found in the tested setup, with evidence recorded.

`WARN`: Usable but has a documented concern, taste risk, moderate gain mismatch, or issue needing another listening pass.

`FAIL`: Audible issue blocks use as a draft listening candidate, or suggests a technical fault, unsafe output, severe noise, severe level jump, runaway tail, or repeatable clipping.

`NOT_RUN`: Default state until real playback happens and evidence is captured.

## Severity

`P0`: Unsafe output, runaway, severe clipping, or issue that can damage ears/equipment. Stop playback.

`P1`: Blocks listening candidate status or suggests technical investigation.

`P2`: Significant quality issue or gain mismatch that requires issue tracking before promotion.

`P3`: Taste, wording, or minor preset polish note.

## Evidence To Capture

- Filled `artifacts/manual-listening/p10a-six-draft-presets/results-template.csv`.
- Session notes in `artifacts/manual-listening/p10a-six-draft-presets/logs/`.
- Screenshots of preset, meters, host setup, or audio settings in `screenshots/`.
- Short renders in `renders/` only when useful for a WARN/FAIL.
- Issue notes in `issues/` for each WARN/FAIL.

## Guardrails

Manual listening QA remains pending or NOT_RUN until a real session is executed. Distortion manual listening QA remains pending. P7F/Reaper remains pending. No preset may be marked factory-approved, release-ready, or final in P10A.
