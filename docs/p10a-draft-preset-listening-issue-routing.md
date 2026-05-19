# P10A Draft Preset Listening Issue Routing

Status: NOT_RUN

## Existing Templates

Use `docs/p8e-listening-qa-issue-template.md` for subjective listening findings:

- Harshness.
- Dullness.
- Noise floor.
- Pumping.
- Stereo image concern.
- Tail feel.
- General taste or playability issue.

Use `docs/p9-preset-gain-staging-issue-template.md` for preset level and gain-staging findings:

- Level mismatch against Dry Reference or neighboring presets.
- Unsafe or excessive output.
- Too-low usable output.
- Bypass jump.
- Suspected limiter dependency.
- Near-clipping or clipping heard during normal playing.

## Targeted Surgery

Open targeted surgery only when evidence points to a repeatable technical defect in a processor or audio path. Examples include repeatable clicks, runaway feedback, broken bypass behavior, corrupt state, or a processor-specific regression. P10A should report this, not patch DSP.

## Gain-Staging Adjustment

Open a gain-staging adjustment when the preset sounds usable but needs input, output, drive, blend, compressor makeup, or wet/dry level changes. Do not silently edit generated presets in P10A.

## Taste Or UI Note

Leave a taste note when the behavior is subjective and not blocking: brightness preference, ambience length preference, preset naming, UI discoverability, or documentation clarity.

## Required Fields For Any Issue

- Preset.
- Result and severity.
- Setup details.
- Exact reproduction phrase.
- Evidence path.
- Recommended state.
- Whether it blocks listening candidate status.

Manual listening QA remains pending or NOT_RUN until the session is executed. Distortion manual listening QA remains pending. P7F/Reaper remains pending.
