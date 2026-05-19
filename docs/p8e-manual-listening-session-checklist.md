# P8E Manual Listening Session Checklist

Use this checklist for each future real-guitar listening session. Do not mark manual listening QA complete without attached evidence.

## Session Metadata

- Date:
- Tester:
- Branch:
- Build hash:
- Commit status:
- Build type:
- Sample rate:
- Buffer:
- Interface:
- Guitar / DI used:
- Input gain:
- Output gain:
- Limiter setting:
- Monitoring setup:
- Headphones / monitors:
- Room notes:

## Test Setup

- Preset/session used:
- Pedal or chain tested:
- Base chain:
- Input material: palm mutes / open chords / single notes / lead sustain / staccato / silence between notes / clean DI.
- Parameter states tested: default / nominal musical / extreme high / extreme low / mix=0 / mix=1 / bypass-unbypass / automation-manual movement.

## Result

- Result: PASS / WARN / FAIL
- Severity: P0 / P1 / P2 / P3
- Subjective notes:
- Expected sound:
- Actual sound:
- Reproducibility: always / often / intermittent / once / not reproduced.
- Workaround:

## Evidence

- `session-log.txt`:
- Screenshots:
- Audio render:
- Preset/session file:
- Diagnostics bundle:
- Additional notes:

## Severity Definitions

- P0: audio runaway, crash, or dangerous output.
- P1: pedal unusable, severe clipping, or corrupt state.
- P2: audible problem with a workaround.
- P3: taste, documentation, or minor preset/gain staging concern.

## Required Pending Markers

- Distortion manual listening QA remains pending unless this session specifically tests it and attaches evidence.
- P7F/Reaper remains pending unless a separate DAW smoke session is executed and documented.
- General manual listening QA remains pending until all planned sessions are complete with evidence.
- Factory presets are not final.
- UI/UX is not final.
