# P10A Six Draft Preset Manual Listening Run Pack

Status: NOT_RUN

This folder is the evidence workspace for the first real manual listening QA session over the six generated draft presets. It is preparation only. No subjective listening result has been recorded yet, and no preset is approved for factory shipping.

## Source Presets

Original draft presets remain in:

- `Resources/Presets/DraftFactory/generated/Dry-Reference.nova-preset`
- `Resources/Presets/DraftFactory/generated/Clean-Studio.nova-preset`
- `Resources/Presets/DraftFactory/generated/Funk-Comp-Clean.nova-preset`
- `Resources/Presets/DraftFactory/generated/Classic-Crunch.nova-preset`
- `Resources/Presets/DraftFactory/generated/Tight-Modern-Rhythm.nova-preset`
- `Resources/Presets/DraftFactory/generated/Wide-Ambient-Clean.nova-preset`

Do not move the originals. The optional `presets/` folder is for small reference copies only if the listener wants to freeze the exact files used during a session.

## Listening Order

1. Dry Reference
2. Clean Studio
3. Funk Comp Clean
4. Classic Crunch
5. Tight Modern Rhythm
6. Wide Ambient Clean

## Evidence Folders

- `issues/`: one markdown issue note per WARN/FAIL item.
- `renders/`: optional short bounced audio examples.
- `screenshots/`: screenshots of preset state, meters, host settings, or plugin settings.
- `logs/`: session notes, host logs, audio interface settings, and environment notes.
- `presets/`: optional reference copies of the six `.nova-preset` files.

## Guardrails

- Manual listening QA remains NOT_RUN until a real listening session is executed.
- Distortion manual listening QA remains pending.
- P7F/Reaper remains pending.
- No preset may be marked factory-approved from this run pack.
- Do not update golden baselines, schema, DSP, audio path, or startup preset pointers.
