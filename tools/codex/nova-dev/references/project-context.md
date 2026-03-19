# NOVA Project Context

## Overview

NOVA is a JUCE audio plugin for guitar multi-effects. The main runtime split is between:

- audio processing in `AudioEngine`
- state and session control in `SessionStore` and `SessionCoordinator`
- host/plugin entrypoints in `PluginProcessor`
- editor and drag-drop pedalboard behavior in `PluginEditor` and `Source/GUI/Widgets`

## Main Architecture

### Signal Flow

`Input -> InputChain -> Line A/B pedal chains -> ChannelStrip A/B -> DryWetMixer -> OutputChain -> Output`

The switcher controls whether Line A, Line B, or both lines are active.

### State Stack

`PluginProcessor -> SessionCoordinator -> SessionStore -> PluginStateModel`

Persistence and preset I/O are handled by `SessionPersistence`.

## Files To Read First

- `Source/Core/AudioEngine.h`
- `Source/Core/PluginProcessor.h`
- `Source/Core/PluginStateModel.h`
- `Source/Core/SessionStore.h`
- `Source/Core/SessionPersistence.h`
- `Source/Core/PluginEditor.h`

## Key Invariants

- State trees should be canonicalized through `PluginStateModel`.
- Unsupported pedal types should not survive canonicalization.
- Zone placement is constrained by `PedalCatalog::enforceZone()`.
- Only one amp and one cabinet are allowed per chain.
- Runtime globals flow through `AudioEngine::RuntimeGlobalParams`.
- Live pedal processor state can be serialized through `SessionPersistence::captureLivePedalStates()`.

## Build Facts

- Solution: `Builds/VisualStudio2022/NOVA.sln`
- Project definition: `NOVA.jucer`
- Primary validation target: `NOVA_StandalonePlugin`

## Sensitive Change Categories

- audio-thread behavior
- graph topology rebuilds
- preset schema changes
- drag-drop indexing and zone mapping
- host parameter bindings
