# NOVA Project Map

## Purpose

NOVA is a JUCE-based guitar multi-effects plugin with VST3 and Standalone targets. The project is organized around a dual-chain signal path, a drag-and-drop pedalboard UI, session persistence, and a catalog/registry system for effect instantiation.

This file is the fast rehydration map for Codex sessions. Read it before touching architecture-sensitive code.

## Build And Project Rules

- The source of truth for source-file membership is `NOVA.jucer`.
- The Visual Studio solution is `Builds/VisualStudio2022/NOVA.sln`.
- Do not edit generated `.vcxproj` files directly.
- When adding or removing source files, update `NOVA.jucer` in Projucer and then re-export the IDE project.
- Prefer validating with the Standalone target first because JUCE unit tests are compiled into that app.

## Runtime Architecture

### Audio Plane

- `Source/Core/AudioEngine.h`
  Owns the `juce::AudioProcessorGraph`, the two runtime chains, the tuner service, and runtime metering.
- `Source/Core/DSP/Global/InputChain.*`
  Input conditioning before the two signal lines.
- `Source/Core/DSP/Global/ChannelStrip.*`
  Per-line gain, pan, width, and strip processing.
- `Source/Core/DSP/Global/OutputChain.*`
  Output processing after the dry/wet stage.

### Control Plane

- `Source/Core/PluginProcessor.*`
  JUCE entry point, host parameter bridge, engine synchronization, preset entrypoints.
- `Source/Core/SessionCoordinator.h`
  Async bridge between message-thread actions and persistent state.
- `Source/Core/SessionStore.h`
  Command-pattern mutations plus runtime parameter cache.
- `Source/Core/SessionPersistence.h`
  Save/load preset files, rebuild engine from state, restore startup preset.
- `Source/Core/PluginStateModel.h`
  Canonical `ValueTree` rules and mutation helpers.

## Core Invariants

- The chain zones are always `Pre -> Amp -> FX -> Cabinet`.
- `PedalCatalog::enforceZone()` is the guardrail for valid zone placement.
- Each chain allows at most one amp and one cabinet.
- Unsupported or malformed pedal state should be canonicalized by `PluginStateModel`.
- Audio-thread work must remain real-time safe. Control-plane mutations should flow through queued commands or snapshots.
- Global runtime parameters are mirrored through `AudioEngine::RuntimeGlobalParams`.

## High-Risk Areas

- `Source/Core/AudioEngine.*`
  Graph rebuilds, command flushing, latency, auto-heal, and audio-thread safety.
- `Source/Core/PluginStateModel.h`
  Any state-schema or canonicalization change can break preset compatibility.
- `Source/Core/SessionPersistence.h`
  Preset serialization bugs can silently corrupt recoverability.
- `Source/Core/PluginProcessor.cpp`
  Host parameter wiring and engine synchronization affect every session.
- `Source/Core/PluginEditor.*` and `Source/GUI/Widgets/*`
  UI drag/drop and chain indexing must stay aligned with state-model ordering.

## Effect Integration Workflow

1. Create the processor/editor under `Source/Effects/...`.
2. Register metadata in `Source/Core/PedalCatalog.h`.
3. Register factory wiring in `Source/Core/PedalRegistry.h`.
4. Add the new files to `NOVA.jucer`.
5. Re-export the Visual Studio project from Projucer.
6. Validate that state serialization, bypass, and zone placement still behave correctly.

## Session Bootstrap Checklist

1. Read `docs/active-work.md`.
2. Run `scripts/context-bootstrap.ps1`.
3. Check `git status --short` before editing.
4. If the task touches source-file membership, plan a `NOVA.jucer` update.
5. If the task touches presets, state schema, or graph routing, test more than a compile.

## Current Open-Change Warning

The worktree may already contain user changes in these areas:

- `Source/Core/PluginEditor.*`
- `Source/Core/PluginProcessor.cpp`
- `Source/Core/PluginStateModel.h`
- `Source/GUI/Widgets/*`

Read diffs before editing around those files.
