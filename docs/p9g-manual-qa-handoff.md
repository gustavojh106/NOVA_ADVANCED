# P9G Manual QA Handoff

Date: 2026-05-11

## Purpose

This handoff moves NOVA from deterministic technical validation into manual listening and integration QA. It does not execute listening QA, does not approve factory presets, and does not close Reaper/P7F.

## Source Materials

- Listening matrix: `docs/p8e-manual-listening-qa-matrix.md`
- Listening checklist: `docs/p8e-manual-listening-session-checklist.md`
- Listening issue template: `docs/p8e-listening-qa-issue-template.md`
- Preset gain-staging issue template: `docs/p9-preset-gain-staging-issue-template.md`
- Draft preset technical evidence: `docs/p9f-draft-preset-direct-limiter-telemetry-results.md`

Use the P8E matrix to decide what must be auditioned, the P8E checklist to run each listening session consistently, and the issue templates to separate tone defects from preset/gain-staging adjustments.

## Recommended Listening Order

1. Dry Reference
2. Clean Studio
3. Funk Comp Clean
4. Classic Crunch
5. Tight Modern Rhythm
6. Wide Ambient Clean
7. Distortion focused chain
8. Pedal-by-pedal broader pass

Start with `Dry Reference` so monitoring level, input gain, output level, noise floor, and pickup behavior are calibrated before judging voiced presets.

## Artifacts To Capture

- Date, monitor path, headphones/speakers, interface, sample rate, block size.
- Guitar/pickup used and input level estimate.
- Preset name or pedal chain.
- PASS/WARN/FAIL result.
- Short reason for each WARN/FAIL.
- Any linked issue ID.
- Whether the issue is tonal, level/gain staging, bypass jump, noise/runaway, UX, or host integration.

## PASS / WARN / FAIL Criteria

PASS:

- Sounds intentional for its category.
- No unsafe level jump.
- No obvious clipping, runaway tail, DC-like thump, stuck gate, or limiter dependency.
- Bypass behavior is acceptable for the target use.

WARN:

- Usable, but needs taste review, small gain adjustment, or context-dependent note.
- Issue does not block continued listening.
- No evidence of unsafe output or technical instability.

FAIL:

- Unsafe output, sustained runaway, clear clipping, broken bypass behavior, dead signal, unusable noise, severe level mismatch, or repeatable host/session failure.
- Blocks promotion until targeted work is complete.

## Routing Issues

Open targeted surgery when the issue is processor behavior:

- repeatable DSP instability
- broken bypass smoothing
- unsafe peaks from a pedal independent of preset gain
- runaway feedback/tail
- parameter automation defect
- processor state recall defect

Open preset/gain-staging adjustment when the issue is preset-specific:

- output too loud or too quiet
- too much makeup gain
- high-gain preset needs level trim
- ambient tail needs wet/dry or output adjustment
- compressor preset causes perceived level jump

Leave as taste/UI note when:

- tonal preference is subjective and technically safe
- label/category expectation needs refinement
- control layout or wizard flow affects auditioning
- final UI copy or preset description needs improvement

## Do Not Close Yet

- Manual listening QA general remains pending until the listening matrix is actually executed.
- Distortion manual listening QA remains pending until high-gain focused listening is executed.
- P7F/Reaper remains pending until the DAW environment is available and smoke is run.
- Draft presets remain non-final until explicit factory approval.
