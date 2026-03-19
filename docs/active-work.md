# Active Work

This file is the live operational context for NOVA. Keep it short and current. Codex should read this file at the start of any substantial session.

## Current Focus

- No active focus recorded yet.

## Latest Decisions

- No decisions recorded yet.

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
