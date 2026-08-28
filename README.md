# NOVA

NOVA is a modular real-time audio processing platform designed to provide musicians, producers, and audio professionals with a flexible environment for building and using pedal-based effect chains inside a digital audio workflow.

The platform combines a low-latency audio engine, configurable audio effects, persistent user settings, DAW integration, and a structured quality-assurance process into a single system focused on stability, predictable audio behavior, and extensibility.

---

## Overview

NOVA's main goal is to provide a reliable virtual pedalboard experience where audio effects can be combined and processed in real time without compromising audio quality or host stability.

The platform is designed around several core principles:

* Real-time safe audio processing
* Low and predictable latency
* Modular effect architecture
* Stable DAW integration
* Reliable bypass behavior
* Consistent parameter and settings persistence
* Safe oversampling and DSP processing
* Extensible architecture for new pedals and audio modules
* Production-oriented testing and release validation

NOVA is not just a collection of effects. It is the underlying platform responsible for managing the complete lifecycle of audio processing, from incoming audio and pedal configuration to host synchronization, latency compensation, settings persistence, and final processed output.

---

# Core Platform

## Real-Time Audio Engine

The audio engine is the foundation of NOVA.

It is responsible for processing incoming audio buffers and routing them through the configured effect chain while maintaining real-time performance requirements.

The engine includes protections and optimizations for common DSP problems such as:

* Denormal floating-point values
* Unsafe processing during configuration changes
* Excessive buffer allocation
* Oversampling state transitions
* Effect bypass transitions
* Latency changes
* Host synchronization

The engine is designed so that expensive initialization or state reconstruction does not occur unnecessarily inside the real-time audio processing path.

This helps minimize:

* Audio dropouts
* CPU spikes
* Clicks and pops
* Unstable host behavior
* Unexpected latency changes

---

## Pedal and Effect System

NOVA uses a modular pedal architecture.

Each pedal represents an independent audio-processing module that can implement its own DSP behavior while still participating in the common NOVA processing pipeline.

This architecture makes it possible to add or evolve effects without redesigning the core engine.

The platform currently includes a migrated and standardized collection of pedal modules using the same processing and lifecycle conventions.

The pedal architecture handles:

* Audio processing
* Parameter management
* Initialization
* Reset behavior
* Bypass behavior
* Sample-rate changes
* Buffer configuration
* Oversampling
* Internal DSP state

A major objective of the architecture is ensuring that every pedal behaves consistently from the perspective of both the engine and the host.

---

# Audio Processing Pipeline

A simplified representation of the NOVA processing flow is:

```text
DAW / Audio Host
       |
       v
Incoming Audio Buffer
       |
       v
NOVA Audio Engine
       |
       v
Pedal / Effect Chain
       |
       +--> Effect 1
       |
       +--> Effect 2
       |
       +--> Effect 3
       |
       +--> ...
       |
       v
Output Processing
       |
       v
Processed Audio
       |
       v
DAW / Audio Host
```

Each effect receives audio from the previous stage and produces the signal that will be processed by the next stage.

The processing pipeline is built to remain deterministic and safe during continuous real-time execution.

---

# Latency Management

Some DSP algorithms require additional processing time.

NOVA includes explicit latency handling so that effects that introduce processing delay can correctly communicate that latency to the audio host.

The platform currently supports mechanisms such as:

* Internal processing latency
* Lookahead processing
* Plugin Delay Compensation (PDC)
* Host latency synchronization

NOVA uses a controlled lookahead strategy where required by the DSP pipeline.

Current processing includes a **2 ms lookahead** where applicable.

When latency changes, NOVA ensures that the audio host receives the correct latency information so the DAW can compensate for it.

This is especially important when NOVA is used alongside other tracks or plugins where phase and timing alignment must remain accurate.

---

# Oversampling

NOVA supports oversampling for effects that benefit from processing audio at a higher internal sample rate.

Oversampling can improve DSP quality and reduce aliasing in nonlinear effects such as:

* Distortion
* Saturation
* Clipping
* Drive
* Other nonlinear processors

The platform includes safeguards around oversampling initialization and configuration changes.

These safeguards prevent invalid states when:

* Sample rates change
* Audio buffers change
* Effects are enabled or disabled
* Processing modes are modified

Oversampling resources are prepared outside unsafe real-time operations whenever possible.

---

# Bypass Architecture

Bypass behavior is particularly important in an audio plugin because incorrectly implemented bypass logic can cause:

* Audio discontinuities
* CPU spikes
* Internal DSP resets
* Unexpected latency
* Clicks or pops

NOVA's bypass architecture avoids unnecessary reconstruction of DSP resources when an effect is bypassed.

Effects preserve the appropriate internal processing state while allowing the audio signal to pass according to the configured bypass behavior.

This improves both stability and responsiveness during live use.

---

# Denormal Protection

Very small floating-point values can cause certain processors to enter CPU-intensive denormal number states.

NOVA includes denormal protections in the real-time audio pipeline.

This prevents unnecessary CPU consumption when processing extremely low-level signals or silence.

The result is more predictable performance during long sessions.

---

# Settings System

NOVA includes a persistent editor settings system used to store application and plugin configuration.

Settings are stored through:

```text
editor-settings.xml
```

The settings system is version-aware and designed to maintain compatibility with previously saved configurations.

The settings interface follows explicit configuration semantics:

### Apply

Applies the selected settings to the active NOVA instance and persists the configuration.

### Cancel

Discards unapplied changes and restores the previously active configuration.

This prevents partially modified settings from accidentally affecting the processing environment.

---

# DAW Integration

NOVA is designed to operate inside professional digital audio workstation environments.

Integration testing has been performed using **Ableton Live** as one of the primary host environments.

The platform handles host-related behaviors including:

* Audio buffer processing
* Sample-rate changes
* Plugin lifecycle events
* Playback start and stop
* Project loading
* Project reopening
* Latency reporting
* Plugin Delay Compensation
* Effect bypass
* Parameter changes
* Configuration persistence

A key objective is that NOVA behaves predictably even when the host repeatedly creates, destroys, suspends, or reconfigures plugin instances.

---

# Stability and Real-Time Safety

Real-time audio software has different constraints from traditional application software.

The audio thread cannot safely tolerate operations that may block unpredictably.

For this reason, NOVA's architecture attempts to minimize or eliminate operations such as:

* Dynamic resource reconstruction
* File access
* Heavy initialization
* Unnecessary memory allocation
* Lock contention
* Expensive configuration operations

from critical real-time processing paths.

DSP resources are initialized or prepared during safe lifecycle stages whenever possible.

---

# Quality Assurance

NOVA uses a structured validation process that includes both automated and real-world DAW testing.

Testing covers areas such as:

* Plugin initialization
* Audio processing
* Effect activation
* Effect bypass
* Pedal combinations
* Parameter changes
* Sample-rate transitions
* Buffer-size transitions
* Oversampling
* Latency compensation
* Settings persistence
* DAW project reopening
* Debug builds
* Release builds

The platform has successfully completed extensive Ableton validation scenarios, including an **86/86 test execution without detected failures** during the latest validated test cycle.

Debug and Release configurations are also validated independently to detect behavior that may only appear under compiler optimization.

---

# Platform Architecture

At a high level, NOVA can be represented as several layers:

```text
+--------------------------------------------------+
|                   DAW / Host                     |
+--------------------------------------------------+
                        |
+--------------------------------------------------+
|               NOVA Host Integration              |
|                                                  |
|  Plugin Lifecycle | Latency | Buffers | PDC      |
+--------------------------------------------------+
                        |
+--------------------------------------------------+
|                 NOVA Audio Engine                |
|                                                  |
| Routing | Real-Time Safety | DSP Coordination    |
+--------------------------------------------------+
                        |
+--------------------------------------------------+
|                 Pedal / DSP Layer                |
|                                                  |
| Pedal 1 | Pedal 2 | Pedal 3 | ... | Pedal N     |
+--------------------------------------------------+
                        |
+--------------------------------------------------+
|             Audio Processing Utilities           |
|                                                  |
| Oversampling | Lookahead | Denormal Protection   |
+--------------------------------------------------+

+--------------------------------------------------+
|               Configuration Layer                |
|                                                  |
| Settings | Persistence | Version Compatibility   |
+--------------------------------------------------+
```

Each layer has a clear responsibility, allowing the platform to evolve without tightly coupling individual effects to host-specific or configuration-specific logic.

---

# Extensibility

NOVA is designed so new audio processors can be integrated into the existing platform without requiring major modifications to the core engine.

A new pedal can primarily focus on its own DSP implementation while NOVA manages the surrounding infrastructure.

The platform provides common mechanisms for:

* Audio buffers
* Sample-rate configuration
* Lifecycle management
* Parameter handling
* Bypass
* Oversampling
* State management
* Host integration

This reduces duplicated infrastructure across effects and helps new pedals follow the same stability requirements as existing modules.

---

# Development Philosophy

NOVA follows several engineering principles intended specifically for production audio software.

### Stability First

Audio continuity is more important than unnecessary architectural complexity.

### Real-Time Safety

Operations performed on the audio thread must remain predictable and lightweight.

### Backward Compatibility

Changes to settings, processing behavior, or effect architecture should avoid breaking existing configurations whenever possible.

### Incremental Migration

Core DSP components are migrated and validated individually instead of introducing large unverified architectural changes.

### Evidence-Based QA

Technical changes are validated through builds, automated tests, and actual DAW scenarios.

---

# Current Platform Maturity

NOVA's core audio architecture has completed several major stabilization stages.

Recent engineering work has included:

* Standardization of the pedal processing architecture
* Migration of 14 pedal modules
* Improved bypass handling
* Removal of unnecessary DSP reconstruction during bypass
* Oversampling safeguards
* Denormal protection
* Lookahead stabilization
* Plugin Delay Compensation support
* Settings persistence improvements
* Apply/Cancel configuration semantics
* Debug and Release validation
* Extensive Ableton integration testing

The platform is currently focused on final product-level validation and UAT before being considered ready for a stable commercial MVP release.

---

# Commercial MVP Objectives

For NOVA to operate as a production-ready commercial product, the platform must consistently guarantee:

* Stable audio processing during long sessions
* No unexpected crashes
* No audio dropouts caused by internal processing
* Reliable project save and reload behavior
* Correct parameter persistence
* Correct latency reporting
* Predictable CPU usage
* Reliable effect bypass
* Compatibility across supported sample rates
* Compatibility across supported buffer sizes
* Stable DAW lifecycle behavior
* Consistent Debug and Release behavior
* Reproducible installation and deployment

Product-level UAT is used as the final validation layer for these requirements.

---

# Testing Strategy

NOVA validation can be divided into four main levels.

```text
Unit / Component Validation
            |
            v
Audio Engine Validation
            |
            v
DAW Integration Testing
            |
            v
User Acceptance Testing
```

### Component Validation

Verifies individual DSP modules and platform components.

### Audio Engine Validation

Verifies the complete processing pipeline and real-time behavior.

### DAW Integration Testing

Verifies NOVA under realistic host conditions.

### User Acceptance Testing

Validates the product from the perspective of an actual musician or producer rather than only from an engineering perspective.

---

# Future Development

The platform architecture allows NOVA to continue expanding with additional capabilities such as:

* New pedal and effect modules
* Additional DSP algorithms
* More advanced routing
* Preset management
* Improved pedalboard workflows
* Additional DAW compatibility validation
* Performance monitoring
* Expanded automated testing
* Additional user configuration options
* Commercial distribution and release infrastructure

Future functionality can be added while continuing to use the existing audio engine and effect architecture as the foundation.

---

# Summary

NOVA is a real-time audio processing platform built around a modular virtual pedal architecture.

Its core responsibilities extend beyond individual audio effects and include the complete infrastructure required to run those effects reliably inside a professional audio environment:

**DSP processing, pedal management, real-time safety, oversampling, latency compensation, bypass handling, configuration persistence, DAW integration, and production-level validation.**

The result is a foundation designed not only to support the current collection of NOVA effects, but also to serve as the base for an extensible commercial audio platform.
