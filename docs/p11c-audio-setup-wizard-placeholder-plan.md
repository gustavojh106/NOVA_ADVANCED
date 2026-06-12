# P11C Audio Setup Wizard Placeholder Plan

Date: 2026-05-19

## Current Phase

P11C adds the Audio Setup Wizard as a first-class Settings section and a reachable placeholder shell. The Settings button `Start Audio Setup Wizard` jumps to the shell inside Settings and marks it as opened. It does not run the full calibration wizard logic in this phase.

## Placeholder Copy

The shell describes:

> Guided setup for audio device, input gain, output level, latency, monitoring and noise calibration.

## Planned Wizard Steps

1. Select audio device
2. Select input
3. Select output
4. Set sample rate and buffer
5. Input level calibration
6. Output level safety test
7. Noise floor detection
8. Latency check
9. Save recommended settings

## Next Phase Requirements

The next prompt should implement the full wizard flow with:

- Standalone audio device selection and validation.
- Host-managed explanation for plugin formats.
- Explicit output test-tone confirmation and level limiting.
- Input level measurement.
- Noise-floor detection.
- Latency risk reporting.
- Recommended setting review before applying.
- Safe persistence through existing editor preferences and global parameters only where already supported.

## Safety Rules

- Do not emit test tones without explicit confirmation.
- Do not change output level silently.
- Do not run heavy diagnostics automatically.
- Do not write settings from a wizard step until the user confirms.
- Do not change DSP, schema or preset serialization.

## Current Status

Placeholder shell: implemented.

Full wizard logic: planned.
