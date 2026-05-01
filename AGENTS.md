# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

NOVA is a **guitar multi-effects processor** built as a JUCE audio plugin (VST3 + Standalone). It features a dual-chain signal routing architecture with 25 effect units (17 pedals, 5 amplifiers, 3 cabinets), drag-and-drop pedalboard GUI, preset system, and built-in tuner.

## Build Commands

Build scripts are PowerShell in `scripts/`. All commands run from the repo root.

```powershell
# Build Standalone (debug, default — use this for dev/test)
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1

# Build VST3 (release)
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Target NOVA_VST3

# Build entire solution
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Target Solution

# Quick validate (context bootstrap + standalone build)
powershell -ExecutionPolicy Bypass -File scripts/quick-validate.ps1

# Context bootstrap (prints active-work, git status, core paths)
powershell -ExecutionPolicy Bypass -File scripts/context-bootstrap.ps1
```

- **IDE**: Visual Studio 2022 (solution at `Builds/VisualStudio2022/NOVA.sln`)
- **Project manager**: Projucer (`NOVA.jucer` at repo root)
- **Build targets**: `NOVA_StandalonePlugin` (debug/test), `NOVA_VST3` (production)
- **When adding/removing source files**: Edit `NOVA.jucer` in Projucer, re-export to regenerate VS projects. Do NOT edit `.vcxproj` files directly.

## Running Tests

Tests use JUCE's built-in `juce::UnitTest` framework. They are compiled into the Standalone target and run at startup when enabled:

- Test file: `Source/Core/AudioEngineTests.cpp`
- Test category: `"NOVA"`
- Tests are registered as a static global (`AudioEngineValidationTests`)
- Test helpers use `kSampleRate = 48000.0`, `kBlockSize = 64`, `kTolerance = 1.0e-4f`
- `warmUpEngine(engine, blockSize, blocks=10)` skips startup silence blocks before assertions
- To run: build and launch the Standalone app — tests execute within the JUCE unit test runner

## Architecture

### Signal Flow (Audio Thread)

```
Input → InputChain → [Line A chain] → ChannelStrip A ─┐
                   → [Line B chain] → ChannelStrip B ─┤→ DryWetMixer → OutputChain → Output
                                                       │
                                              (SwitcherMode selects routing)
```

The `AudioEngine` owns a `juce::AudioProcessorGraph` and manages two parallel effect chains (Line A / Line B). Routing modes: `LineA_Only`, `LineB_Only`, `Dual_Parallel`. Input routing: `Stereo`, `Left`, `Right`, `Sum` (mono summing).

### Control Plane / Audio Plane Separation

`AudioEngine` uses a strict two-plane architecture:
- **ControlPlane**: Queues graph topology commands (add/remove/move pedal) via a lock-protected deque. Safe to call from any thread.
- **AudioPlane**: Processes audio, applies queued commands, holds runtime state. Only touched by the audio thread (plus atomics for metering).

Global parameter updates use a revision-stamped snapshot (`RuntimeGlobalParams`) with a spinlock — the audio thread only reads when the revision changes. No locks on audio path — only atomics + revision checks.

### Session State Stack

```
PluginProcessor → SessionCoordinator → SessionStore → PluginStateModel (ValueTree)
                                     → SessionPersistence (file I/O)
```

- **PluginStateModel**: Pure functions operating on a `juce::ValueTree`. Enforces canonical zone ordering (Pre → Amp → FX → Cabinet), single-amp-per-chain rule, schema versioning (`STATE_SCHEMA_VERSION`). `sanitizeLine()` enforces single Amp + single Cabinet per chain on insert/move.
- **SessionStore**: Command pattern layer over PluginStateModel. Command types: `Reset`, `RestoreStateTree`, `AddPedal`, `RemovePedal`, `MovePedal`, `SetPedalEnabled`. Binds host parameters, produces `RuntimeGlobalParams` snapshots.
- **SessionCoordinator**: Async bridge (via `AsyncUpdater`) between message thread and store. Manages preset save/load/restore.
- **SessionPersistence**: Captures live pedal states as Base64-encoded XML, serializes ValueTree for presets, rebuilds engine from state on load.

### Effect Zones (per chain)

Each chain has four ordered zones: `Pre` → `Amp` → `FX` → `Cabinet`. Zone placement is enforced by `PedalCatalog::enforceZone()`:
- Pedals → Pre or FX only
- Amplifiers → Amp zone only (max 1 per chain, replacement semantics)
- Cabinets → Cabinet zone only

Zone sort rank: 0=Pre, 1=Amp, 2=FX, 3=Cabinet. Max 12 pedals per flex zone (`MAX_PEDALS_PER_FLEX_ZONE`).

### Adding a New Effect

1. Create a subfolder `Source/Effects/Pedals/<Name>/` with processor class inheriting `ProcessorBase` (`<Name>Pedal.h`)
2. Create custom editor in the same subfolder (`<Name>Editor.h`) — see `OverdriveEditor.h` as reference
3. Add entry to `PedalCatalog::entries()` array (update array size template parameter)
4. Register factory in `PedalRegistry::getFactory()` map + add `#include`
5. Add source files to `NOVA.jucer` via Projucer and re-export
6. Optionally add thumbnail/dashboard paint in `PedalUIFactory` (or as separate `<Name>Thumbnail.h`/`<Name>Dashboard.h`)
7. Validate: state serialization, bypass crossfade, zone placement, and latency reporting

### Source Directory Layout

```
Source/
├── Core/                       # Plugin entry points, engine, state, constants
│   ├── DSP/Global/             # InputChain, ChannelStrip, OutputChain
│   └── DSP/Services/           # TunerService
├── Effects/
│   ├── Pedals/Base/            # ProcessorBase, PedalUIFactory, PremiumPedalUI
│   ├── Pedals/<Name>/          # All 17 pedals: <Name>Pedal.h + <Name>Editor.h (+ optional Thumbnail/Dashboard)
│   ├── Pedals/*.h              # Legacy flat files (fully superseded by subfolder versions)
│   ├── Amplifiers/             # 5 amp processors (flat .h files)
│   └── Cabinets/               # CabinetPedal, SyntheticIR, cabinet variants
├── GUI/
│   ├── Widgets/                # ChainLane, DropZone, AssetBrowserOverlay, AssetItem, DraggableButton
│   ├── Overlays/               # TunerOverlay
│   └── Wizards/                # WizardBase, StartWizard, AudioSetupWizard, PresetFinderWizard
├── Lib/                        # Vendored: RTNeural, Eigen, json, xsimd, models/
└── Resources/Textures/         # Knob filmstrips, surface materials
```

**Pedal file structure**: All 17 pedals are in their own subfolder (e.g., `Pedals/Overdrive/OverdrivePedal.h` + `OverdriveEditor.h`). Notable exceptions: `Wah/` contains two pedals (ClassicWahPedal + AutoWahPedal), and `Neural/` uses RTNeural inference for neural amp modeling (distinct from other pedal patterns). A few legacy flat files (e.g., `Pedals/ChorusPedal.h`, `Pedals/CompressorPedal.h`) still exist but are fully superseded — always use the subfolder version.

### Key Files

These files have non-obvious roles or are central coordination points — most other files are self-explanatory from the directory layout above.

| File | Role |
|------|------|
| `Source/Core/Constants.h` | Single source of truth: all enums, ValueTree IDs, colors, config constants |
| `Source/Core/AudioEngine.h/cpp` | Graph management, two-plane architecture, auto-heal, audio processing |
| `Source/Core/PluginStateModel.h` | Pure-function ValueTree ops: zone ordering, sanitization, schema versioning |
| `Source/Core/SessionStore.h` | Command-pattern state mutations, host parameter bindings, `RuntimeGlobalParams` snapshots |
| `Source/Core/SessionPersistence.h` | Preset file I/O with Base64-encoded pedal states, engine rebuild from state |
| `Source/Core/PedalCatalog.h` | Static catalog of all 25 effects: metadata, zone rules, accent colors |
| `Source/Core/PedalRegistry.h` | Factory map: type string → processor constructor (add `#include` + entry here for new effects) |
| `Source/Effects/Pedals/Base/ProcessorBase.h` | Base class for all effects: bypass crossfade, latency reporting |
| `Source/Effects/Pedals/Base/PremiumPedalUI.h` | Premium editor template with filmstrip knobs, hero knob, response curves |
| `Source/Effects/Cabinets/SyntheticIR.h` | Biquad cascade → comb-filter → exponential decay → 1024-sample IR per cabinet type |
| `Source/Core/StyleSheet.h` | Knob geometry constants, `ModernKnobLnF`, `StudioTrimKnobLnF` LookAndFeel classes |

**Legacy placeholders** (empty, safe to ignore): `Common.h`, `GlobalProcessors.h`.

### DSP Conventions

- All effects process stereo (`AudioChannelSet::stereo()` in/out)
- Parameter smoothing uses `juce::SmoothedValue` with ramp times from `Nova::Config` (`SMOOTH_DEFAULT_SECONDS = 0.02`, `SMOOTH_DRIVE_SECONDS = 0.012`)
- Oversampled effects (Overdrive, Distortion, Fuzz, etc.) use `juce::dsp::Oversampling<float>` at 4x
- Bypass crossfade is handled in `ProcessorBase::processBlock()` via `beginBypassProcess()`/`endBypassProcess()` with 10ms ramp (`SMOOTH_BYPASS_SECONDS = 0.01`)
- Latency reporting: call `setProcessingLatency()` (not `setLatencySamples()` directly) to respect bypass state
- Hard absolute limit: `Nova::Config::HARD_ABS_LIMIT_LINEAR` (24.0f ≈ +27.6 dBFS)
- Auto-heal: AudioEngine detects consecutive corrupt blocks (`CORRUPT_BLOCKS_BEFORE_HEAL = 2`) and resets the graph
- Startup silence: engine skips initial blocks after prepare/rebuild (`STARTUP_COUNTER_INIT = 5`, `STARTUP_COUNTER_GRAPH_CHANGE = 6`)
- Cabinet simulation uses `SyntheticIR`: cascaded biquad filters → comb-filter reflections → exponential decay → 1024-sample IR per cabinet type
- InputChain includes pitch-shift transpose via buffer-based delay line with varispeed interpolation

### GUI Architecture

The editor (`PluginEditor.h/cpp`) uses a multi-tier responsive layout with three pedal card size tiers, sidebar drawers, and a modal pedal editor overlay. Key subsystems:

- **Responsive layout**: `FlexLayoutResult` in `PluginEditor.cpp` — auto-selects card size tier based on available space, adapts single vs dual lane
- **Sidebar drawers**: `SidebarDrawer` struct — left drawer has `AssetBrowserOverlay` (search + drag grid), right drawer has output/mixer controls
- **Modal editor overlay**: `PedalEditorOverlay` — dark scrim + viewport-wrapped editor panel, close via button or ESC
- **Pedal UI factory**: `PedalUIFactory` — registry of per-pedal `ThumbnailPaintFn` + `DashboardPaintFn` lambdas; fallback to generic if unregistered
- **Drop zones**: `DropZone` widget — drag state visualization (None/Valid/Invalid), shake animation on invalid drag, insertion markers
- **Knob LookAndFeels**: `UI::ModernKnobLnF` (main knobs), `UI::StudioTrimKnobLnF` (trim/gain), `PremiumKnobLnF` (filmstrip-based in premium editors)
- **Textures**: `Resources/Textures/` has knob filmstrips (101 frames each), hardware elements, surface materials. Helpers in `Nova::Textures` namespace
- **Async refresh**: `uiRefreshPending` atomic + `AsyncUpdater` batches repaints; `StatsTimer` polls at 30 Hz for metering
- **Drag-and-drop formats**: `"TypeID:DisplayName"` (asset browser → chain), `"MOVE:LineA/LineB:PedalIndex"` (intra-chain move)
- **Colors**: All defined in `Nova::Colors::*` in `Constants.h`. Each pedal has its own `accentHex`/`accentBrightHex` in `PedalCatalog`

- **Wizards**: `GUI/Wizards/WizardBase.h` — multi-step overlay with progress dots, back/next/skip navigation. Subclasses (`StartWizard`, `AudioSetupWizard`, `PresetFinderWizard`) override `buildStepContent()` and `onStepChanged()`. Shown as full-screen modal overlays.

**Premium editor reference**: `OverdriveEditor.h` — hero knob + control grid + response curve + texture overlays. Matching `OverdriveThumbnail.h` and `OverdriveDashboard.h` for asset browser and compact views.

### Validation Guidelines

Match validation depth to the subsystem being changed:
- **DSP changes**: Assume bypass, smoothing, and latency can regress even if compile passes. Test with audio.
- **State/preset changes**: Sanity-check serialization round-trips (save → load → compare).
- **GUI drag/drop changes**: Verify chain ordering and zone assignment stay aligned with `PluginStateModel` zone ordering.
- **Source file membership changes**: Update `NOVA.jucer` and re-export. Build the Standalone target first (it includes unit tests).
- **Minimum for any code change**: Compile the Standalone target.

### High-Risk Areas

Changes to these files can cascade broadly — test beyond compile:
- **AudioEngine**: graph rebuilds, command flushing, latency, auto-heal, audio-thread safety
- **PluginStateModel**: state schema or canonicalization changes can break preset compatibility
- **SessionPersistence**: preset serialization bugs can silently corrupt recoverability
- **PluginProcessor**: host parameter wiring and engine synchronization affect every session
- **PluginEditor + GUI/Widgets**: drag/drop indexing and chain ordering must stay aligned with state-model zone ordering
- **SyntheticIR**: cabinet IR generation — changes affect all cabinet pedal sounds

### Libraries (vendored in `Source/Lib/`)

- **RTNeural**: Neural network inference for amp modeling
- **Eigen**, **json**, **xsimd**: RTNeural dependencies
- **models/**: Pre-trained neural network weights (JSON resources)
