# P13E-P5 — Pedal Contract Migration & Inter-Pedal Corruption Containment

**Date:** 2026-06-20
**Scope:** Migrate the remaining registered pedal processors to the shared
`NovaPedalProcessor` realtime-safety contract and close the inter-pedal
NaN/Inf corruption gap. Amps, cabinets, global output DSP, routing, and the
limiter are explicitly out of scope and were not touched.

P13E-P1 … P13E-P4 are closed and were not reopened. `NovaPedalProcessor` was
already proven with `BoostPedal` and `CompressorPedal`; this phase migrates the
remaining 14 registered pedals onto the same contract.

---

## 1. Pedal registry migration matrix

Source of truth: `PedalRegistry::getFactory()`. Every registered **pedal** type
(zones Pre / FX) now derives from `NovaPedalProcessor`. Amps and cabinets are
out of scope and remain on `ProcessorBase`.

| Registry type | Class | Before | After | Status |
|---|---|---|---|---|
| Compressor | `CompressorPedal` | `NovaPedalProcessor` | `NovaPedalProcessor` | Already migrated (P13E-P4) |
| Boost | `BoostPedal` | `NovaPedalProcessor` | `NovaPedalProcessor` | Already migrated (P13E-P4) |
| Noise Gate | `NoiseGatePedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| EQ | `EQPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Neural | `NeuralPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Overdrive | `OverdrivePedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Distortion | `DistortionPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Fuzz | `FuzzPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Wah | `ClassicWahPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Octave | `OctavePedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Chorus | `ChorusPedal` (subfolder) | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Phaser | `PhaserPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Flanger | `FlangerPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Tremolo | `TremoloPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |
| Delay | `DelayPedal` | `ProcessorBase` (+`TempoSyncable`) | `NovaPedalProcessor` (+`TempoSyncable`) | **Migrated** |
| Reverb | `ReverbPedal` | `ProcessorBase` | `NovaPedalProcessor` | **Migrated** |

**Result: 16 / 16 registered pedals on the shared contract (14 newly migrated this phase).**

### Documented exceptions (not migrated — with rationale)

These are **not** registered in `PedalRegistry` and therefore cannot be
instantiated from the registry or the test harness. They are intentionally
deferred to keep the patch focused on registered, reachable pedals:

| File | Reason for exception |
|---|---|
| `Source/Effects/Pedals/Wah/AutoWahPedal.h` | Not registered in `PedalRegistry`; unreachable via registry/harness. Derives directly from `ProcessorBase` (not from `ClassicWahPedal`), so the Wah migration does not implicitly migrate it. Migrating an unregistered pedal would add risk without registry/golden coverage. |
| `Source/Effects/Pedals/ChorusPedal.h` (flat legacy) | Legacy flat file fully superseded by `Pedals/Chorus/ChorusPedal.h`; not registered. |
| `Source/Effects/Pedals/CompressorPedal.h` (flat legacy) | Legacy flat file fully superseded by `Pedals/Compressor/CompressorPedal.h`; not registered. |
| `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` | Not registered in `PedalRegistry`; unreachable via registry/harness. |
| Amps (`ClassicAmp`, `HighGainAmp`, `CleanAmp`, `ChimeAmp`, `BoutiqueAmp`) | Out of scope per task constraints. |
| Cabinets (`CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet`) | Out of scope per task constraints. |

No registered pedal was silently deferred.

---

## 2. Shared contract guarantees (documented in `NovaPedalProcessor.h`)

`NovaPedalProcessor::processBlock()` is `final` and owns the audio-thread policy
so every derived pedal inherits it identically:

1. **Denormal suppression** for the whole render (`juce::ScopedNoDenormals`).
2. **Input scrub before any state capture** — `Nova::DSP::scrub(buffer)` runs
   *before* `ProcessorBase` captures its bypass dry scratch and *before* any
   derived feedback / detector / dry-capture path. NaN/Inf → 0, denormals
   flushed, values above the hard limit shaped (tanh) toward ~1.5× the limit.
   This is the core containment guarantee: corrupt input can never reach a
   feedback tail, a dry mix, or a bypass crossfade.
3. **Bypass integration** — `beginBypassProcess` / `endBypassProcess` crossfade
   with latency-matched dry, unchanged from `ProcessorBase`.
4. **No-allocation dry fallback** — oversized / over-wide blocks (relative to
   `preparePedalProcessing`) increment a shared `getRealtimeFallbackCount()`
   diagnostic, invoke `handleRealtimeFallback()`, and leave the (scrubbed) dry
   input intact instead of touching derived DSP.
5. **Shared helpers** — `readParameter()` (finite-clamped parameter reads) and
   `mixDryWetEqualPower()` (finite equal-power mix).

Derived pedals now only implement `processPedalBlock()` plus their own
`prepareToPlay()` / `releaseResources()` / `reset()`. Per-pedal duplicates of
the scrub loop, `canProcessBlock`, `isPrepared`, `fallbackBlockCount`,
`getRealtimeFallbackCount`, and `clearRealtimeFallbackCount` were removed in
favour of the shared implementation.

New base additions this phase:
- `isPedalPrepared()` — lets `DistortionPedal::setStateInformation` keep its
  "only `reset()` once prepared" behaviour without a private `isPrepared` flag.
- `noteRealtimeFallback()` — lets a pedal that detects an unsupported realtime
  condition *mid-render* (e.g. `NeuralPedal`'s prepared-channel guard) bump the
  shared fallback counter + hook while restoring dry, allocation-free.

Algorithm-specific protections were preserved verbatim: feedback guards,
hybrid detectors, lookahead delays, biquad NaN self-heal, oversampler guards,
DC blockers, soft-ceilings, `OverdrivePedal`/`DelayPedal`/`ReverbPedal`/
`FlangerPedal` signal/debug telemetry (now capturing the already-scrubbed
input), bypass/crossfade, latency reporting, and reset behaviour. No
intentional tone, gain, mix-law, latency, preset, or bypass behaviour changes.
`ProcessorBase` latency behaviour was not modified.

`OverdrivePedal` preserved its legacy oversized-block telemetry counter by
overriding `handleRealtimeFallback()` to bump `debugTelemetry.blockOverflowCount`.

---

## 3. Reset realtime-safety rule

Documented in `NovaPedalProcessor.h`:

> Derived `reset()` MUST be allocation-free and realtime-safe. `ProcessorBase`
> calls `reset()` from the audio thread on every bypass → un-bypass transition
> (see `beginBypassProcess`) so stale tails do not jump back in. `reset()` must
> not resize buffers, allocate, take locks, log, or perform I/O; it may only
> zero state, reseat smoother targets, and recompute cached coefficients. All
> buffer/voice allocation must happen in `prepareToPlay()`.

Every migrated stateful pedal was verified against this rule: `reset()` only
clears filter/delay state (`std::fill` over already-sized buffers, `.clear()`
that zeroes without reallocating), reseats `SmoothedValue` targets, resets
detectors/envelopes, and recomputes cached coefficients. No `setSize`,
`assign`, `allocate`, `new`, lock, log, or I/O on any `reset()` path.

This is exercised at runtime by the registry-wide invariant test (bypass
on/off cycles, which drive `reset()` on the audio thread) and by the existing
`runP1BypassToggleContinuity` matrix.

---

## 4. Inter-pedal corruption containment — test results

New test: **`P13E-P5 inter-pedal corruption containment: feedback chain stays
finite, bounded and decays`** (in `P13EPedalContractMigrationTests`).

- A corrupting source (NaN / +Inf / −Inf / denormal interspersed with a
  normal-level tone) is fed for 16 blocks through a live chain of real,
  registry-instantiated **stateful/feedback pedals**:
  `Delay → Chorus → Flanger → Reverb`.
- Then ~2.7 s of silence is fed through the chain.

Asserted and **passing**:
- Output stays **finite** at every stage for every block (no NaN/Inf
  propagation into or out of any feedback tail).
- Output stays **bounded** (`< 4.0` through the silence tail; the corrupting
  block stays `< 4·HARD_ABS_LIMIT`).
- The feedback tails **decay toward silence** after corruption
  (`latePeak < 0.05` after 2.7 s) — i.e. corruption is contained and the loops
  recover instead of latching into self-oscillation or a poisoned tail.

Containment mechanism: the shared scrub runs on each pedal's input before its
feedback write / dry capture, so a NaN/Inf from upstream is neutralised to a
finite, bounded sample before it can ever enter a delay line, all-pass loop,
FDN, or dry-mix path.

---

## 5. Registry-wide safety invariant — coverage

New test: **`P13E-P5 registry-wide invariant`** iterates **every registered
pedal type** (zones Pre ∪ FX, enumerated from `PedalRegistry`) and, per type,
instantiates via the registry and asserts:

1. Derives from `NovaPedalProcessor` (`dynamic_cast` succeeds) — the contract is
   universal across the registry.
2. **Normal finite signal** → finite, bounded, no fallback.
3. **NaN / Inf / denormal / hot input** (prepared block) → finite, bounded,
   scrubbed (no fallback).
4. **Recovery** → after corruption, a normal signal returns finite, bounded,
   audible output with no stale corrupt state, no fallback.
5. **reset() RT-safety proxy** → bypass on/off cycles stay finite, no fallback.
6. **Mono-content / prepared-channel** → identical L/R content on the prepared
   stereo bus stays finite, bounded, no fallback.

New test: **`P13E-P5 registry-wide oversized blocks take a no-allocation dry
fallback`** — for every registered pedal, a block larger than prepared
increments the fallback counter exactly once and preserves the dry input
(`nullRms < 1e-7`).

The existing `P1 Pedal Safety` matrices (`runP1PreparedMatrix`,
`runP1OversizedFallback`, `runP1BypassToggleContinuity`) were extended from the
7 previously-covered pedals to **all 16** registered pedals across
3 sample rates × 6 block sizes × 9 signal classes.

### P13E-P4 reviewer follow-ups (included)

- **`clearRealtimeFallbackCount()` behaviour** — dedicated test: an oversized
  block raises the shared counter, `clearRealtimeFallbackCount()` resets it to 0.
- **Compressor fallback resets the gain-reduction meter** — dedicated test:
  drive gain reduction with a hot signal, then an oversized block; the realtime
  fallback (`handleRealtimeFallback` override) resets
  `getCurrentGainReductionDb()` to 0.
- **mono / prepared-channel safety** — covered by invariant step 6 above.

---

## 6. Validation results

| Step | Command | Result |
|---|---|---|
| SharedCode build | `build-nova.ps1 -Target NOVA_SharedCode` | **0 warnings, 0 errors** |
| Standalone build | `build-nova.ps1 -Target NOVA_StandalonePlugin` | **0 warnings, 0 errors** |
| Base audio validation | `run-base-audio-validation.ps1 -TimeoutSeconds 240` | **PASS — results=297, passes=12925, failures=0** |
| `git diff --check` | — | **Clean** (only informational LF→CRLF notices) |
| Audio-thread policy scan | `check-audio-thread-policy.ps1` | See note below |

### Audio-thread policy scan

- Migrating to `processPedalBlock` initially produced two stale name-based
  contract failures (`p7h_reverb_processblock_missing`,
  `p8a_distortion_processblock_missing`) because those legacy checks hardcoded
  the old `ReverbPedal::processBlock` / `DistortionPedal::processBlock` name.
  The migration intentionally renames the audio entry to `processPedalBlock`
  (which the scanner already tracks and scans for forbidden patterns), so the
  three pedal-specific range filters (`p7h`, `p8a`, `p8b`) were updated to
  recognise `processPedalBlock` as well as `processBlock` — matching the
  migration-aware fallback the generic P7C pedal loop already used. After this,
  those checks pass (no logging / no allocation found in the migrated bodies).
- The migration **reduced** the scan's blocking failures and introduced **none**
  attributable to pedal code.
- **Two remaining failures, `p9f` / `p9g`, are pre-existing and out of scope.**
  They fire because `Source/Core/Audio/GraphBuilder.h`,
  `Source/Core/AudioEngine.cpp`, and `Source/Core/AudioEngine.h` are present in
  the working-tree diff. These files were modified **before** this session and
  were **not** touched by P13E-P5 (constraints forbid AudioEngine changes). The
  committed baseline scan was already `status=FAIL` with `p9f`/`p9g` present.

There are **no P13E-P5-attributable audio-thread policy failures.**

---

## 7. Remaining risks

- The audio-thread policy scan remains `status=FAIL` solely due to the
  pre-existing `p9f`/`p9g` scope guards on `AudioEngine.*` / `GraphBuilder.h`
  (unrelated to this phase). Resolving them belongs to whichever phase owns
  those files.
- Oversized-block handling for `Delay`/`Reverb`/other newly-migrated pedals now
  takes the shared dry fallback instead of processing (previously they had no
  size guard). Hosts respect the prepared max block size, so this only changes
  behaviour in a previously-unprotected pathological case, and is strictly safer.
- Telemetry capture (`Overdrive`/`Delay`/`Reverb`/`Flanger`) now observes the
  already-scrubbed input rather than raw input. This is diagnostic-only and does
  not affect audio.

## 8. Remaining follow-up

- **`P12D-GOLDEN1` rebaseline** — no golden baseline files were changed in this
  phase (verified by the policy scan's `*_no_golden_baseline_update` checks, all
  passing). A golden rebaseline remains a separate, deferred follow-up.
