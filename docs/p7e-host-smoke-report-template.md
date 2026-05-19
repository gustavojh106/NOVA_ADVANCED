# P7E Host Smoke Report Template

## Build

- Date:
- Tester:
- Build hash:
- Branch:
- Commit status / dirty files:
- `STATE_SCHEMA_VERSION`:
- Configuration: Debug / Release
- Plugin format: Standalone / VST3
- Artifact path:

## Environment

- OS:
- CPU:
- RAM:
- Audio interface:
- Driver/API:
- Sample rates tested:
- Buffer sizes tested:
- Host:
- Host version:
- Host project path:

## Validation Baseline

- Base validation:
- Golden metrics:
- RT profile:
- RT stability:
- Policy scan:
- Wrapper Fast:
- Preflight artifact folder:

## Scenario Summary

| Area | Passed | Failed | Warn | Not Run |
| --- | ---: | ---: | ---: | ---: |
| Standalone |  |  |  |  |
| VST3 scan/instantiate |  |  |  |  |
| Host project save/load |  |  |  |  |
| Host/plugin bypass |  |  |  |  |
| Automation |  |  |  |  |
| Sample-rate/buffer switching |  |  |  |  |
| Offline render |  |  |  |  |
| Multi-instance/remove while running |  |  |  |  |
| Corrupt/recovery cases |  |  |  |  |

## Issues

### Issue 1

- Severity: P0 / P1 / P2 / P3
- Scenario ID:
- Host:
- Repro rate:
- Expected:
- Actual:
- Steps to reproduce:
- Attached logs:
- Attached project/preset:
- Crash/minidump:
- Screenshot/render:
- Recommendation:

## Attached Evidence

- `session-log.txt`:
- `audio-base-test-report.txt`:
- `artifacts/audio-thread-policy-scan.txt`:
- `artifacts/rt-profile-release-x64-report.json`:
- Host logs:
- Host project:
- Rendered audio:
- Crash dumps:
- Screenshots:

## Final Recommendation

- Overall status: PASS / WARN / FAIL
- Ship blocker:
- Required follow-up:
- Recommended next phase:
