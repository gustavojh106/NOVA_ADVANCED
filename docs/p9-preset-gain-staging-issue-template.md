# P9 Preset / Gain Staging Issue Template

## Summary

- Preset name:
- Category:
- Chain:
- Severity: P0 / P1 / P2 / P3
- Reproducibility: always / often / intermittent / once / not reproduced

## Problem Type

Select all that apply:

- Too loud
- Too quiet
- Clips
- Pumps limiter
- Reverb/delay overload
- Bypass jump
- Noise floor
- Dull/harsh
- Subjective taste
- State restore issue

## Severity Guide

- P0: unsafe output or crash.
- P1: unusable preset or corrupt state.
- P2: audible issue with workaround.
- P3: taste, leveling, or documentation.

## Environment

- Date:
- Tester:
- Branch:
- Build hash:
- Commit status:
- Sample rate:
- Buffer:
- Interface:
- Guitar / DI:
- Input gain:
- Output gain:
- Limiter setting:
- Monitoring setup:

## Reproduction

1. Load preset/session:
2. Confirm chain:
3. Play input material:
4. Toggle bypass:
5. Save/load if state restore is involved:
6. Observe:

## Expected Result


## Actual Result


## Evidence

- Diagnostics bundle:
- Session log:
- Audio render:
- Preset file:
- Screenshots:
- Additional notes:

## Recommended Action

Select one:

- Gain staging adjustment
- More listening
- Technical investigation
- Targeted surgery
- Reject preset
- UI/UX note

## Guardrails

- Do not request revoicing or DSP surgery for taste-only issues.
- Do not mark manual listening QA complete from this issue alone.
- Distortion manual listening QA remains pending unless focused evidence is attached.
- P7F/Reaper remains pending unless a separate DAW smoke result is attached.
