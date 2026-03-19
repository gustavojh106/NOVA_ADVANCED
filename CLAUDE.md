# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NOVA is a **guitar multi-effects processor** built as a JUCE audio plugin (VST3 + Standalone). It features a dual-chain signal routing architecture with 25 effect units (17 pedals, 5 amplifiers, 3 cabinets), drag-and-drop pedalboard GUI, preset system, and built-in tuner.

## Build System

- **IDE**: Visual Studio 2022 (solution at `Builds/VisualStudio2022/NOVA.sln`)
- **Project manager**: Projucer (`NOVA.jucer` at repo root)
- **Build targets**: `NOVA_StandalonePlugin` (debug/test), `NOVA_VST3` (production)
- **Build**: Open `NOVA.sln` in VS2022, select configuration (Debug/Release x64), build
- **When adding/removing source files**: Edit `NOVA.jucer` in Projucer, re-export to regenerate VS projects. Do NOT edit `.vcxproj` files directly.

## Running Tests

Tests use JUCE's built-in `juce::UnitTest` framework (not a separate test runner). They are compiled into the Standalone target and run at startup when enabled:

- Test file: `Source/Core/AudioEngineTests.cpp`
- Test category: `"NOVA"`
- Tests are registered as a static global (`AudioEngineValidationTests`) and execute within the JUCE unit test runner

## Architecture

### Signal Flow (Audio Thread)

```
Input → InputChain → [Line A chain] → ChannelStrip A ─┐
                   → [Line B chain] → ChannelStrip B ─┤→ DryWetMixer → OutputChain → Output
                                                       │
                                              (SwitcherMode selects routing)
```

The `AudioEngine` owns a `juce::AudioProcessorGraph` and manages two parallel effect chains (Line A / Line B). Routing modes: `LineA_Only`, `LineB_Only`, `Dual_Parallel`.

### Control Plane / Audio Plane Separation

`AudioEngine` uses a strict two-plane architecture:
- **ControlPlane**: Queues graph topology commands (add/remove/move pedal) via a lock-protected deque. Safe to call from any thread.
- **AudioPlane**: Processes audio, applies queued commands, holds runtime state. Only touched by the audio thread (plus atomics for metering).

Global parameter updates use a revision-stamped snapshot (`RuntimeGlobalParams`) with a spinlock — the audio thread only reads when the revision changes.

### Session State Stack

```
PluginProcessor → SessionCoordinator → SessionStore → PluginStateModel (ValueTree)
                                     → SessionPersistence (file I/O)
```

- **PluginStateModel**: Pure functions operating on a `juce::ValueTree`. Enforces canonical zone ordering (Pre → Amp → FX → Cabinet), single-amp-per-chain rule, schema versioning.
- **SessionStore**: Command pattern layer over PluginStateModel. Binds host parameters, produces `RuntimeGlobalParams` snapshots.
- **SessionCoordinator**: Async bridge between message thread and store. Manages preset save/load/restore.

### Effect Zones (per chain)

Each chain has four ordered zones: `Pre` → `Amp` → `FX` → `Cabinet`. Zone placement is enforced by `PedalCatalog::enforceZone()`:
- Pedals → Pre or FX only
- Amplifiers → Amp zone only (max 1 per chain, replacement semantics)
- Cabinets → Cabinet zone only

### Adding a New Effect

1. Create processor class inheriting `ProcessorBase` (in `Source/Effects/Pedals/<Category>/`)
2. Add entry to `PedalCatalog::entries()` array (update array size template parameter)
3. Register factory in `PedalRegistry::getFactory()` map + add `#include`
4. Add source file to `NOVA.jucer` via Projucer and re-export
5. If the pedal has a custom editor, implement `createEditor()` override

### Key Files

| File | Role |
|------|------|
| `Source/Core/Constants.h` | All enums, ValueTree IDs, colors, config constants |
| `Source/Core/AudioEngine.h/cpp` | Graph management, audio processing, auto-heal |
| `Source/Core/PluginProcessor.h/cpp` | JUCE plugin entry point, host parameter bridge |
| `Source/Core/PluginEditor.h/cpp` | Main GUI: sidebar drawers, chain lanes, mixer, presets |
| `Source/Core/PedalCatalog.h` | Static catalog of all 25 effects with metadata |
| `Source/Core/PedalRegistry.h` | Factory map: type string → processor constructor |
| `Source/Core/PluginStateModel.h` | ValueTree state operations (pure functions) |
| `Source/Core/SessionStore.h` | Command-pattern state mutations + parameter bindings |
| `Source/Core/SessionCoordinator.h` | Async coordinator between GUI/host and store |
| `Source/Core/SessionPersistence.h` | Preset file I/O, engine rebuild from state |
| `Source/Effects/Pedals/Base/ProcessorBase.h` | Base class for all effects (bypass, latency) |
| `Source/Core/StyleSheet.h` | Knob geometry, LookAndFeel classes |

### DSP Conventions

- All effects process stereo (`AudioChannelSet::stereo()` in/out)
- Parameter smoothing uses `juce::SmoothedValue` with ramp times from `Nova::Config`
- Oversampled effects (Overdrive, Distortion, Fuzz, etc.) use `juce::dsp::Oversampling<float>` at 4x
- Bypass crossfade is handled in `ProcessorBase::processBlock()` with configurable ramp
- Latency reporting: call `setProcessingLatency()` (not `setLatencySamples()` directly) to respect bypass state
- Hard absolute limit: `Nova::Config::HARD_ABS_LIMIT_LINEAR` (24.0f ≈ +27.6 dBFS)
- Auto-heal: AudioEngine detects consecutive corrupt blocks and resets the graph

### GUI Conventions

- Colors: `Nova::Colors::*` namespace in `Constants.h`
- Knob rendering: `UI::ModernKnobLnF` and `UI::StudioTrimKnobLnF` in `StyleSheet.h`
- Pedal editors: created via `PedalUIFactory` or individual `createEditor()` overrides
- Chain visualization: `ChainLane` widget manages drop zones and pedal slot components
- Sidebar drawers: left (asset browser), right (output/mixer)

### Libraries (vendored in `Source/Lib/`)

- **RTNeural**: Neural network inference for amp modeling
- **Eigen**, **json**, **xsimd**: RTNeural dependencies
- **models/**: Pre-trained neural network weights (JSON resources)
