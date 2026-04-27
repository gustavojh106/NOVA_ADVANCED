# Active Work

This file is the live operational context for NOVA. Keep it short and current. Codex should read this file at the start of any substantial session.

## Current Focus

- Hardening the base no-pedal audio path for MVP quality.

## Latest Decisions

- Remove input transpose from the MVP instead of carrying a rarely used pitch-shift path in the base signal flow.
- Treat the current low-output symptom as limiter state first: logs showed the output limiter at `-20 dB`, clamping peaks to `0.100000`.
- Clamp limiter ceiling control to `-12..0 dB` so legacy or accidental `-20 dB` states no longer make the base path feel nearly muted.
- The base limiter now uses lookahead, linked stereo gain reduction, sub-sample true-peak estimation, and explicit latency reporting while active.
- Single-jack guitar input is auto-promoted to both channels at unity before input gain, gate, pedals, and output processing, so users should not need to overdrive input/master just to recover level.
- Validate the base no-pedal path with `scripts/run-base-audio-validation.ps1`, which fails on base regressions while ignoring the two known pedal-only failures.
- Validate DSP changes with a real Standalone rebuild plus audio validation, not compile-only checks.

## Known Constraints

- `NOVA.jucer` is the source of truth for source-file membership.
- Avoid direct edits to generated Visual Studio project files.
- Preserve user changes already present in the worktree unless the task explicitly requires reconciling them.

## Current Sensitive Files

- `Source/Core/PluginEditor.cpp`
- `Source/Core/PluginEditor.h`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/PluginStateModel.h`
- `Source/GUI/Widgets/ChainLane.cpp`
- `Source/GUI/Widgets/DropZone.cpp`
- `Source/GUI/Widgets/DropZone.h`

## Session Startup

1. Read `docs/codex/project-map.md`.
2. Run `scripts/context-bootstrap.ps1`.
3. Inspect `git status --short`.
4. Confirm whether the task touches DSP, state schema, or source-file membership.

## Next Useful Updates

- Record the exact feature or bug being worked on.
- Record the branch or milestone name.
- Record any temporary hacks or do-not-break behavior.
- Remove stale notes when they are no longer active.
