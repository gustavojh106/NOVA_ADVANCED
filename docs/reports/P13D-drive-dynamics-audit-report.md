# P13D — Drive & Dynamics Audit Report

**Task ID:** P13D-drive-dynamics-audit
**Mode:** Audit only — no code/tests/presets modified.
**Auditor:** Claude (Opus 4.8)
**Date:** 2026-06-16
**Context:** After P12D (OutputChain limiter), P13A-F001 (host PDC), P13B-FIX1 (time-based denormal/NaN), P13C-FIX1 (amps/cabs hardening).
**Assumptions per carry-forward caveats:** global mix = 100%, line gain = 1.0 (unity).

---

## 0. Verdict (short)

Drive and dynamics effects are **safe to keep enabled**. Overdrive is the strongest (full guard parity). **Boost is the riskiest active effect** — it is the only oversampled drive that still lacks the oversized-block capacity guard and input NaN/Inf scrub that P13C added to the amps, so it can read/write past the oversampler's prepared capacity and can latch NaN into its biquad states. No **hard blocker** prevents the full-chain/preset audit (normal hosts send block ≤ prepared), but Boost (F001/F002) and the Noise-Gate NaN scrub (F003) should be handed to Codex 5.5 before release. ScopedNoDenormals parity (F004) is a lower-priority performance hardening.

---

## 1. Inventory

Active drive/dynamics processors (registered in `PedalRegistry::getFactory()` + present in `PedalCatalog`):

| Effect | File | Class | Zone | Params | Oversampling | Latency reported |
|--------|------|-------|------|--------|--------------|------------------|
| Boost | `Source/Effects/Pedals/Boost/BoostPedal.h` | `BoostPedal` | Pre/FX | gain, tone, tight, character, mid, level | 8× HalfbandIIR | ✅ oversampler latency |
| Overdrive | `Source/Effects/Pedals/Overdrive/OverdrivePedal.h` | `OverdrivePedal` | Pre/FX | drive, tone, texture, mix, level | 8× HalfbandIIR | ✅ |
| Distortion | `Source/Effects/Pedals/Distortion/DistortionPedal.h` | `DistortionPedal` | Pre/FX | gain, tone, body, mix, level, tight, mode(5) | 8× HalfbandIIR | ✅ |
| Fuzz | `Source/Effects/Pedals/Fuzz/FuzzPedal.h` | `FuzzPedal` | Pre/FX | fuzz, tone, gate, mix, level, bias, mode(3) | 8× HalfbandIIR | ✅ |
| Neural | `Source/Effects/Pedals/Neural/NeuralPedal.h` | `NeuralPedal` | Pre/FX | drive, focus, detail, comp, mix, level | 8× HalfbandIIR | ✅ |
| Compressor | `Source/Effects/Pedals/Compressor/CompressorPedal.h` | `CompressorPedal` | Pre/FX | threshold, ratio, attack, release, knee, focus, blend, makeup | none | ✅ zero (correct) |
| Noise Gate | `Source/Effects/Pedals/Gate/NoiseGatePedal.h` | `NoiseGatePedal` | Pre/FX | threshold, attack, hold, release, range, hysteresis, focus | none | ✅ lookahead samples |

**Dead/legacy (NOT registered, NOT user-reachable):** `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` (`MetalDistortionPedal`). `PedalCatalog.h:40` maps legacy `"Metal Distortion"` → `"Distortion"`; the Metal voice now lives as Distortion mode index 3. See F005.

### Per-effect processBlock path / state

- **Boost:** scrub ❌ → 8× upsample → per-channel HP → gain → JFET `boostClip` + dense `atan` → mid/presence/air/DC biquads → downsample → `containBoostOutput` (finite + soft knee 0.72/0.94). State: 5×stereo biquads (`Biquad` with `z1/z2`), 6 SmoothedValues. **No `preparedBlockSize`, no oversized guard, no input scrub.** Has `ScopedNoDenormals`.
- **Overdrive (reference):** `ScopedNoDenormals` → input telemetry → **block-overflow guard (`numChannels>preparedChannels || numSamples>preparedBlockSize` → bypass)** → `scrubBuffer` (finite + denormal flush + ±8 clamp) → wet-feed trim → 8× → one-pole bank + `ResponsiveDriveStage` (env sag, asym bias, 2-stage tanh, DC trap, `softCeiling 1.05`) → downsample → `finalDcBlock` + `softCeiling` → equal-power mix + `sanitizeAudioSample` + `softCeiling`. Fully guarded.
- **Distortion:** `scrubInvalidSamples` (finite→0) → **`canProcessBlock` guard** (samples/channels ≤ prepared) → adaptive gate (`Gate` struct, hysteresis) → 8× → multi-stage asym/diode/LED clip + sag envelope + idle-reject env → tone stack biquads → downsample → equal-power mix + `containOutputSample` (`softCeiling 0.94`) + `finalDcBlock`. **No `ScopedNoDenormals`.**
- **Fuzz:** `scrubInvalidSamples` (finite→0) → **`canProcessBlock` guard** → 8× → input/sag envelopes + internal gate (per-channel `gateOpen`/`gateGain`) + drift/sputter LFOs + mode clip → tone biquads → downsample → equal-power mix + `containFuzzOutput` (finite + soft knee). **No `ScopedNoDenormals`.**
- **Neural:** `ScopedNoDenormals` → `scrubInvalidSamples` (finite→0) → **`canProcessBlock` guard** → 8× → **channel-capacity guard** (`hasChannels`, safe dry fallback) → `EnvelopeFollower` + waveshapers (`denseClip`/`silkyClip`) + tone/speaker one-pole bank + DC block → downsample → equal-power mix × wetTrim × level. Fully guarded; no explicit final soft ceiling (relies on wetTrim + OutputChain).
- **Compressor:** `scrubInvalidSamples` (finite→0) → **`canProcessBlock` guard** → per-sample hybrid peak/RMS detector + sidechain HP focus + program-dependent adaptive attack/release + soft knee + makeup + equal-power blend. State: 2×sidechain biquad, `HybridDetector`, `linkedGainReductionDb`. **No `ScopedNoDenormals`.** Latency 0 (correct — no lookahead).
- **Noise Gate:** **no scrub** → per-sample 3-stage sidechain (HP/presence-peak/LP) + hybrid detector + hysteresis open/close + hold counter + adaptive release + smoothstep range + **lookahead ring delay**. Reports `lookaheadSamples` latency. **No `ScopedNoDenormals`, no NaN/Inf scrub.**

### prepare / reset
All effects honour `sampleRate <= 0` early-out, reset filters/envelopes, re-seed SmoothedValues, and call `reset()` from `prepareToPlay`. Distortion/Fuzz/Neural/Compressor capture `preparedBlockSize`+`preparedChannels`; Boost does **not**. Sample-rate-dependent coefficients (`coefficientForTime`, `coeffForMs`, envelope/sag coeffs, biquad cutoffs vs `innerSr`) are all derived from the live sample rate — correct under SR changes.

---

## 2. Drive DSP safety

- **Expected input level:** guitar-level (~‑18 to ‑6 dBFS nominal). All drives normalise internally; Overdrive additionally measures input peak and trims wet feed above 1.25 (`wetFeedTrimForPeak`). Distortion/Neural apply `highGainCompatibilityTrim`/`wetTrim` so they don't slam HighGainAmp.
- **Internal gain:** Boost up to +24 dB pre-clip; Overdrive 4–20 dB stage1; Distortion `stage1BaseDb`+gain×perPct (up to ~+34 dB in Metal mode) across 3 clip stages; Fuzz 1.4–9.2× stage1; Neural +7 to +29 dB. All terminal stages are bounded by `tanh`/`atan`/exp clips, so per-sample magnitude is intrinsically bounded ≈ ≤1.0–1.3 before output containment.
- **Aliasing:** every drive runs **8× HalfbandPolyphaseIIR oversampling** around the nonlinearity — adequate for the saturation orders used. Pre/inter/top-cut low-passes at inner rate further limit harmonic spray.
- **Output containment before OutputChain:** Boost `containBoostOutput` (ceil 0.94), Overdrive `softCeiling` chain (≤1.20 emergency), Distortion `containOutputSample` (ceil 0.94), Fuzz `containFuzzOutput` (ceil 0.92), Neural wetTrim only, Compressor/Gate no boost beyond makeup/unity. All keep nominal output ≤ ~0.94–1.0, so they do **not** chronically pin the P12D limiter at 0.999 — the limiter sees transient overs only, consistent with the P12D "emergency-only soft ceiling" design.
- **Hot input:** Overdrive (input trim), Distortion/Neural (compat trim) tame hot feeds. **Boost has no input scrub/trim**, but its `tanh`/`atan` stages + `containBoostOutput` still bound output; the risk is non-finite latch (F002), not level runaway.
- **Into HighGainAmp / cabinets:** the compat trims are specifically tuned so drives don't over-saturate HighGainAmp's input gate (now NaN-safe post P13C). Existing chain tests confirm bounded output (see §6).
- **Sustain/noise:** Distortion (modes 3/4) and Fuzz embed gates; Metal-voice idle-reject env keeps noise floor controlled. Standalone Noise Gate handles the rest.

---

## 3. Dynamics DSP safety

- **Compressor detector:** hybrid peak/RMS with `jmax(rmsSquare,0)` floor and `gainToDecibels(jmax(.,1e-6))` floor — no `log(0)`/`sqrt(neg)`. Linked-stereo, program-dependent adaptive attack/release derived from crest/depth, all `jmax`-clamped to sane minimums. Ratio `jmax(1,·)`, knee `jmax(0.25,·)`. **Stable; no stuck state.** Denormal risk on decay tails (F004). Input scrubbed finite before detection.
- **Noise Gate detector:** same hybrid detector + 3-stage sidechain, hysteresis (open/close split by `hysteresisDb`), hold counter, adaptive release, smoothstep range, lookahead. Coefficients SR-correct. Logic prevents chatter (hysteresis + hold). **Gaps:** (a) **no NaN/Inf input scrub** — a non-finite input poisons the detector envelope → `detectorDb`=NaN → comparisons fail → `gateGain` can go NaN and the lookahead ring stores NaN with **no recovery path** (F003); (b) **no denormal protection** while sitting on its primary operating condition (near-silence) (F004).
- **Attack/release correctness:** both use `exp(-1/(t·sr))` one-pole coefficients with clamped minimum times — correct and SR-independent in behaviour.
- **Silence / near-silence:** Compressor returns ~unity (detector→threshold floor, GR→0). Gate closes to `range` (default ‑96 dB ⇒ effectively muted) smoothly. Both fine functionally; denormal CPU is the only concern.
- **Recovery after invalid input:** Compressor recovers (input scrubbed before detector each block, IIR stable). **Gate does not guarantee recovery** (F003).
- **Pumping/click:** Compressor focus HP avoids low-end pumping; equal-power blend avoids clicks. Gate hold + adaptive release + smoothstep avoid chatter/clicks.

---

## 4. RT safety

| Concern | Finding |
|---|---|
| Allocations in processBlock | None in steady state. Scratch/dry buffers preallocated in `prepareToPlay`. **Metal** (dead) resizes scratch inside processBlock — not reachable. |
| Locks | None. Metering via `std::atomic`. |
| Logging/string work | None on the audio path (telemetry uses numeric scalars; Overdrive's `buildReport` is not called from processBlock). |
| Dynamic resize | None for active effects. |
| Parameter lookup | Raw `AudioParameterFloat*` reads + SmoothedValue — RT-safe. |
| Expensive per-sample ops | `tanh/atan/exp/sin` at 8× — acceptable, matches established cost; gates compute per-sample peak over channels. |
| Denormal risk | **Boost ✅, Overdrive ✅, Metal ✅, Neural ✅** have `ScopedNoDenormals`. **Distortion, Fuzz, Compressor, Noise Gate ❌** (F004). |
| Invalid-sample / state latch | Overdrive/Distortion/Fuzz/Neural/Compressor scrub input → finite before stateful filters, so no latch. **Boost ❌** (filters see raw input → NaN latch, F002). **Noise Gate ❌** (detector/lookahead latch, F003). |
| Oversized block | Overdrive/Distortion/Fuzz/Neural/Compressor guard (bypass + counter). **Boost ❌** — no capacity guard before `oversampler.processSamplesUp` (F001). Gate/Compressor have no oversampler; Gate ring is self-bounded (circular), Compressor guarded. |

---

## 5. Latency / PDC

| Effect | Reports | Correct? |
|--------|---------|----------|
| Boost | `oversampler.getLatencyInSamples()` | ✅ |
| Overdrive | oversampler latency | ✅ |
| Distortion | `round(oversampler latency)` | ✅ |
| Fuzz | `round(oversampler latency)` | ✅ |
| Neural | oversampler latency | ✅ |
| Compressor | 0 (no lookahead) | ✅ |
| Noise Gate | `lookaheadSamples` (~1.5 ms) | ✅ |

- Latency is set via `setProcessingLatency()` in `prepareToPlay` and is **constant across parameter automation** (oversampling factor and lookahead are fixed once prepared — no automated path changes latency). Mode changes (Distortion/Fuzz) do not change oversampling, so latency is automation-stable.
- Bypass latency consistency: `ProcessorBase` bypass crossfade preserves reported latency; no effect drops/changes latency on bypass.
- P13A-F001 host listener covers add/bypass/remove because reported latency is fixed per instance and the engine re-reports on graph change; no drive/dynamics effect mutates its latency at runtime, so no uncovered dynamic-latency case is introduced here.

---

## 6. Tests — existing & gaps

**Existing coverage (`Source/Core/AudioEngineTests.cpp`):**
- State round-trips: Boost, Overdrive, Neural, Compressor, Noise Gate, Distortion, Fuzz.
- Mix-zero transparency: Overdrive, Neural, Distortion, Fuzz.
- Automation-stress finite: Boost, Overdrive, Neural, Distortion, Fuzz, Compressor, Noise Gate.
- **Required chain tests all present:** `p10c_boost_highgainamp_modern4x12_nominal`, `p10c_distortion_highgainamp_modern4x12_nominal`, `p10c_fuzz_classicamp_cabinet_nominal`, `p10c_distortion_cleanamp_cabinet_nominal`; plus `distortion_highgain_modern4x12_professional_bounds`, `p10e_boost_highgain_noise_clipping_guard`, `high_gain_noise_gate_chatter_guard`, `p8b_distortion_into_amp_and_cabinet`.
- Dynamics behaviour: Compressor GR/focus/linked-stereo; Noise-Gate clamp/focus/linked-stereo/legacy.
- **Oversized-block fallback:** `runP1OversizedFallback<…>` runs for **Compressor, Distortion, Fuzz, Neural** (asserts finite + fallback-counter==1 + dry preserved).

**Gaps (recommend, once code lands — do not add now):**
- **G1:** No oversized-block test for **Boost** (it has no fallback counter/guard — see F001). Overdrive's overflow path (`debugTelemetry.blockOverflowCount`) is also not asserted by a dedicated test.
- **G2:** No NaN/Inf-input recovery test for any drive (P13C added `injectNanInfSamples` for amps; the analogous drive test is missing — would have surfaced F002/F003).
- **G3:** No denormal-CPU/near-silence finite test for Compressor / Noise Gate decay tails (F004).
- **G4:** No sample-rate-change re-prepare test specifically for drive/dynamics (44.1k↔96k) verifying coefficient correctness.

---

## 7. Manual listening matrix

| # | Chain / condition | What to listen for |
|---|-------------------|--------------------|
| 1 | Boost → HighGainAmp → Modern4x12 | Tightness, no fizz/aliasing, no dropouts/clicks (watch F001/F002 under odd buffer sizes) |
| 2 | Distortion (Metal) → HighGainAmp → Modern4x12 | Palm-mute clamp, gate behaviour, no runaway/limiter pinning |
| 3 | Fuzz (Vintage) → ClassicAmp → Cabinet | Cleanup with guitar volume, sag, gate sputter musicality |
| 4 | Overdrive → CleanAmp → Cabinet | Transparent breakup, touch sensitivity, mix transparency at 0% |
| 5 | Compressor → CleanAmp | Sustain without pumping; silence stays quiet (no denormal CPU/whine) |
| 6 | Gate → HighGainAmp | Clean idle silence, no chatter, attack doesn't clip note onset |
| 7 | Drive + Gate + HighGainAmp | Gate tracks drive output; no stuck-closed on sustained notes |
| 8 | Palm muting, high gain | Tight clamp, no flutter, no aliasing on chugs |
| 9 | Sustained notes, high gain | No gate ducking mid-note; smooth sustain |
| 10 | Silence between notes | True silence, no denormal hiss, no NaN dropout |
| 11 | Low input / high drive | Idle-reject/gate floor behaves; no roaring noise floor |
| 12 | High input / normal output | Output containment holds; limiter not chronically engaged |
| 13 | Bypass on/off under sustain | Click-free crossfade, level match, latency-stable |

---

## 8. Scorecard (1–10)

| Effect | DSP safety | Gain staging | Aliasing/OS | Dyn/env stability | Latency/PDC | RT safety | Param safety | Test coverage | Release readiness |
|--------|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **Overdrive** | 9 | 9 | 9 | 9 | 9 | 9 | 9 | 9 | 9 |
| **Neural** | 8 | 8 | 9 | 8 | 9 | 9 | 9 | 8 | 8 |
| **Distortion** | 8 | 8 | 9 | 8 | 9 | 7 | 9 | 9 | 8 |
| **Compressor** | 8 | 8 | 9 | 8 | 9 | 7 | 9 | 9 | 8 |
| **Fuzz** | 7 | 8 | 9 | 7 | 9 | 7 | 8 | 8 | 7 |
| **Noise Gate** | 6 | 8 | 9 | 7 | 9 | 6 | 8 | 8 | 7 |
| **Boost** | 5 | 8 | 8 | 8 | 9 | 5 | 9 | 6 | 6 |

(RT-safety deductions: Distortion/Fuzz/Compressor = missing `ScopedNoDenormals`; Noise Gate = + NaN latch; Boost = + missing oversized guard & input scrub.)

---

## 9. Risk table

| ID | Effect | Severity | Probability | Evidence | Files | Recommended fix | Implementer | Blocks full-chain/preset audit? |
|----|--------|----------|-------------|----------|-------|-----------------|-------------|----------------------------------|
| P13D-F001 | Boost | Medium-High | Medium | `processBlock` calls `oversampler.processSamplesUp(block)` (BoostPedal.h:199) with **no `numSamples > preparedBlockSize` guard**; no `preparedBlockSize` member. Oversampler `initProcessing` sized to prepared block (line 142). Larger host block ⇒ OOB/UB/assert. Parity gap vs P13C amps & Overdrive. | `Source/Effects/Pedals/Boost/BoostPedal.h` | Capture `preparedBlockSize` in prepare; add oversized-block bypass guard + realtime fallback counter before upsampling (mirror Overdrive/amps). | Codex 5.5 | **No** (normal hosts send block ≤ prepared) — fix before release |
| P13D-F002 | Boost | Medium | Medium | No input scrub before stateful biquads (BoostPedal.h:236+). NaN/Inf input poisons `inputHP/midPeak/presShelf/airLP/dcBlock` `z1/z2` permanently; `containBoostOutput` zeroes only the current output sample, not filter state ⇒ persistent silence, no recovery. | `Source/Effects/Pedals/Boost/BoostPedal.h` | Add `scrubInvalidSamples` (finite→0) at block entry, like Overdrive/Distortion/Fuzz/Neural. | Codex 5.5 | No — robustness hardening |
| P13D-F003 | Noise Gate | Medium | Low-Medium | `processBlock` has **no NaN/Inf scrub** (NoiseGatePedal.h:253+); non-finite input ⇒ detector envelope NaN ⇒ `detectorDb` NaN ⇒ `gateGain` NaN + lookahead ring stores NaN, no reset/recovery path. | `Source/Effects/Pedals/Gate/NoiseGatePedal.h` | Add input scrub + `isfinite` reset for detector/`gateGain`/lookahead on non-finite. | Codex 5.5 | No |
| P13D-F004 | Distortion, Fuzz, Compressor, Noise Gate | Low-Medium | Medium | No `juce::ScopedNoDenormals` in these four `processBlock`s (grep). Biquad/envelope decay tails on silence ⇒ denormal CPU spikes; Compressor & Gate operate on silence by design. | `DistortionPedal.h`, `FuzzPedal.h`, `CompressorPedal.h`, `NoiseGatePedal.h` | Add `juce::ScopedNoDenormals` as first statement of each `processBlock` (matches P13B/P13C pattern). | Codex 5.5 | No |
| P13D-F005 | Metal (dead) | Low | N/A (unreachable) | `MetalDistortionPedal` not in `PedalRegistry`; `PedalCatalog.h:40` maps `Metal Distortion`→`Distortion`. Lacks oversized guard, input scrub, output containment; resizes scratch in processBlock. Latent only. | `Source/Effects/Pedals/Metal/MetalDistortionPedal.h` | Delete legacy file, or guard it for parity, to prevent accidental re-registration. | Codex 5.5 (cleanup) | No |
| P13D-F006 | Tests | Low | N/A | `runP1OversizedFallback` covers Compressor/Distortion/Fuzz/Neural but not Boost; no NaN/Inf-input or denormal-tail drive tests (§6 G1–G4). | `Source/Core/AudioEngineTests.cpp` | After F001–F004 land, add Boost oversized + drive NaN/Inf recovery + Compressor/Gate near-silence finite tests. | Codex 5.5 | No |

---

## 10. Verdict (full)

- **Safe to keep enabled?** **Yes.** All seven active drive/dynamics effects are functionally correct, oversampled where needed, latency-honest, output-contained, and RT-safe in steady state. Their nonlinear stages are intrinsically bounded, so they do not chronically pin the P12D limiter.
- **Strongest:** **Overdrive** — full guard parity (overflow guard + scrub + denormals + multi-stage soft ceiling + telemetry). The template the others should match.
- **Riskiest:** **Boost** — the only oversampled drive without the P13C-style oversized-block guard *and* without input NaN/Inf scrub (F001 + F002). Second: **Noise Gate** (NaN latch + denormal, F003/F004), notable because it lives on silence.
- **Blocker before full-chain/preset audit?** **No hard blocker.** Under normal host behaviour (block ≤ prepared, finite audio) every effect is safe, and all required interaction chains already pass. F001/F002/F003 are robustness-parity gaps, not functional failures. Proceed to the full-chain/preset interaction audit; schedule the Boost fix before the release build.
- **What should Codex 5.5 fix next (priority order):**
  1. **F001 + F002 — Boost** oversized-block guard + input scrub (bring Boost to amp/Overdrive parity).
  2. **F003 — Noise Gate** NaN/Inf scrub + detector/lookahead reset.
  3. **F004 — ScopedNoDenormals** in Distortion, Fuzz, Compressor, Noise Gate.
  4. **F005 — Metal** legacy cleanup; **F006 — tests** after fixes land.

**Code modified during this audit:** No.
