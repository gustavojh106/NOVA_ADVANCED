---
name: nova-dev
description: Use this skill when working inside the NOVA JUCE plugin repository to follow the project's architecture, state-model rules, build flow, DSP constraints, and session bootstrap files. Trigger it for feature work, bug fixing, refactors, preset/state changes, GUI drag-drop changes, or when you need fast project-specific context.
---

# Nova Dev

## Overview

This skill specializes Codex for the NOVA project. It provides the shortest path to recover project context, avoid JUCE/Projucer mistakes, and work safely around the audio engine, state model, and drag-and-drop pedalboard UI.

## Session Workflow

1. Read `docs/active-work.md` from the repo root.
2. Read `docs/codex/project-map.md` from the repo root.
3. Check `git status --short` before editing.
4. If the task adds or removes source files, update `NOVA.jucer` instead of editing generated IDE files.
5. If the task touches `AudioEngine`, `PluginStateModel`, or `SessionPersistence`, validate more than a compile whenever practical.

## Hard Project Rules

- Treat `NOVA.jucer` as the source of truth for file membership.
- Do not edit generated `.vcxproj` files directly.
- Preserve unrelated user changes already in the worktree.
- Respect real-time safety on the audio path.
- Keep chain ordering canonical: `Pre -> Amp -> FX -> Cabinet`.
- Assume preset compatibility matters unless the user explicitly accepts schema changes.

## Decision Guide

### If the task touches DSP or audio routing

- Read `references/project-context.md`.
- Inspect `Source/Core/AudioEngine.*` and the relevant processor classes.
- Watch for thread-affinity, latency, smoothing, bypass, and graph rebuild side effects.

### If the task touches presets or state

- Read `references/project-context.md`.
- Inspect `Source/Core/PluginStateModel.h`, `Source/Core/SessionStore.h`, and `Source/Core/SessionPersistence.h`.
- Preserve canonicalization rules and serialization compatibility where possible.

### If the task touches the GUI pedalboard

- Read `references/session-rules.md`.
- Inspect `Source/Core/PluginEditor.*` plus `Source/GUI/Widgets/*`.
- Keep visual ordering, drop-zone indexing, and state-model ordering aligned.

## Fast Commands

- Bootstrap context: `powershell -ExecutionPolicy Bypass -File scripts/context-bootstrap.ps1`
- Build Standalone: `powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1`
- Quick validate: `powershell -ExecutionPolicy Bypass -File scripts/quick-validate.ps1`

## References

- Read `references/project-context.md` for architecture and file ownership.
- Read `references/session-rules.md` for working conventions and task intake.
