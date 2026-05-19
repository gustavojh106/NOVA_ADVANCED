# P10A Session Checklist

Status: NOT_RUN

## Before Playback

- [ ] Confirm six source presets exist under `Resources/Presets/DraftFactory/generated/`.
- [ ] Confirm host or standalone build/version.
- [ ] Record sample rate.
- [ ] Record buffer size.
- [ ] Record audio interface.
- [ ] Record guitar or DI source.
- [ ] Record input gain setting.
- [ ] Record output gain and monitoring level.
- [ ] Confirm no preset is marked factory-approved.
- [ ] Confirm manual listening QA is still NOT_RUN before starting.

## Per Preset

- [ ] Load preset from generated draft source.
- [ ] Capture screenshot of preset and meters if practical.
- [ ] Play clean DI reference.
- [ ] Play open chords.
- [ ] Play single notes.
- [ ] Play lead sustain.
- [ ] Play staccato notes.
- [ ] Play palm mutes where relevant.
- [ ] Leave silence between phrases for recovery and tail checks.
- [ ] Record PASS/WARN/FAIL only after evidence is captured.
- [ ] Create issue note for each WARN/FAIL.

## After Playback

- [ ] Fill `results-template.csv`.
- [ ] Update linked issue notes in `issues/`.
- [ ] Place renders in `renders/` only when they support a finding.
- [ ] Place screenshots in `screenshots/`.
- [ ] Place session logs or environment notes in `logs/`.
- [ ] Keep Distortion focused listening pending unless it was explicitly executed as a separate run.
- [ ] Keep P7F/Reaper pending unless a real Reaper smoke was executed.
- [ ] Do not approve factory presets.
