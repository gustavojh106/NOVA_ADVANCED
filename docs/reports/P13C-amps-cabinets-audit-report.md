# P13C — Amps & Cabinets Audit Report

- **Task ID:** P13C-amps-cabinets-audit
- **Date:** 2026-06-12
- **Auditor:** Claude Fable (read-only; no code modified)
- **Baseline:** post-P12D (always-on limiter), P13A-F001 (host PDC), P13B + P13B-FIX1 (stateful FX hardened)
- **Assumptions:** global mix = 100%, line gain = 1.0; Release VST3 / manual DAW PDC check still pending (carry-forward)

---

## 1. Inventory

Five amps (`Source/Effects/Amplifiers/*.h`, flat headers) and three cabinets (`Source/Effects/Cabinets/`). All inherit `ProcessorBase` (bypass crossfade, latency API). No other amp-like or cab-like processors found; the closest neighbors (NeuralPedal preamp-style waveshaper, Distortion/Metal pedals) were audited in P13B or belong to the upcoming drive audit.

### Amps (all: 8× oversampling via `juce::dsp::Oversampling(2, 3, polyphase IIR)`, saturation + IIR voicing at inner rate, latency reported from `oversampler.getLatencyInSamples()`)

| Amp | Class | Params | Saturation core | Stateful components | Notes |
|---|---|---|---|---|---|
| **Clean Amp** | `CleanAmp` (`CleanAmp.h`) | drive, bass, treble, reverb, headroom, level 0–2 | `triodeShape`: tanh + x² asym blended ≤34% with dry; compensation 1/√ | 4 IIR (HP/shelves), 2 DC blockers, `BoundedStudioRoom` (8×32k delay lines, damped, soft-limited), 6 smoothers | Studio rebuild: input scrub (NaN/±8/denormal), `ScopedNoDenormals`, oversize-block `emergencySafePass`, output `softLimit(1.25)`, full telemetry |
| **Classic Amp** | `ClassicAmp` (`ClassicAmp.h`) | drive 0.5–6, tone, presence, depth, sag, bright, level 0–2 | `PremiumAmpCore`: 3 cascaded tanh stages, weights 0.28/0.38/0.64 (sum ≤1.30), per-channel sag envelope (`AMP_SAG_DECAY` 0.9992) | 7 IIR filters, sag envelope ×8ch, 3 smoothers | No input scrub, no `ScopedNoDenormals`, no oversize-block guard, no output containment (master ×2 raw) |
| **High Gain Amp** | `HighGainAmp` (`HighGainAmp.h`) | drive 1–10, tone, presence, tight, resonance, feel, level 0–2 | 4 tanh stages, weights 0.12/0.25/0.35/0.52 (sum ≤1.24); built-in input noise-reject gate (envelope, floor 0.07–0.18, smoothstep); `conditionPreampInput` soft-knee (finite-checked) | 8 IIR, sag + noise envelopes ×8ch | Output `kProfessionalOutputTrim = 0.32`; no input scrub / denormal guard / oversize guard |
| **Boutique Amp** | `BoutiqueAmp` (`BoutiqueAmp.h`) | drive 0.2–3.5, warmth, mid, pres, touch, level 0–2 | Piecewise stage1 (x−0.12·x²·sign below threshold; tanh·1.12 above) + light tanh stage2; envelope-driven dynamic gain (touch response) | 6 IIR, envelope ×8ch (constexpr coefficients) | Header claims 4× but code is 8×; piecewise waveshaper is discontinuous at threshold; no scrub/denormal/oversize guard; no output containment |
| **Chime Amp** | `ChimeAmp` (`ChimeAmp.h`) | drive 0.3–4, treble-cut, bass-cut, brilliance, sag, level 0–2 | Asymmetric 2-stage tanh (pentode/EL84), cathode-bias envelope (`cathodeBiasCoeff` 0.0012) + sag env (0.9988) | 6 IIR, 2 envelopes ×8ch | Stage mix sum ≤1.0 (safest weights); same missing guards as Classic/HGA/Boutique; header claims 4×, code is 8× |

`prepare/reset` for all amps: SR-guarded, oversampler re-init at prepared block, inner-rate filter prep, smoother re-seed, voicing cache invalidated via NaN sentinels, latency reported, `prepareBypassSmoother`, full `reset()`. Coefficients (`updateVoicingIfNeeded`) recomputed only on >1e-4 param delta using `ArrayCoefficients` assignment (RT-safe, no allocation).

### Cabinets (all: synthetic IR convolution at base rate + 5–6 IIR voicing filters + DC blocker, zero added latency)

| Cabinet | Class | IR source | Output safety | Notes |
|---|---|---|---|---|
| **Cabinet ("Atlas 4x12")** | `CabinetPedal` (`CabinetPedal.h`) | `generateAtlas4x12` — G12T-75 closed-back model | `containCabinetOutput` soft knee 0.86→ceiling 0.97 | NaN scrub, oversize-block dry fallback (`canProcessBlock` + atomic counter) |
| **Modern 4x12** | `Modern4x12Cabinet` | `generateModern4x12` — V30 model (2 kHz bark, 600 Hz scoop) | wet × `kProfessionalCabinetTrim` 0.70, **no containment** | Same scrub/fallback pattern; extra `midScoop` filter; presence compressed to ≤+3.5 dB |
| **Vintage 2x12** | `Vintage2x12Cabinet` | `generateVintage2x12` — Alnico open-back model | **no trim, no containment** | Same scrub/fallback pattern |

**SyntheticIR** (`SyntheticIR.h`): unit impulse → 8–9 cascaded biquads (double-precision design, float state) → 2 comb reflections (delays scaled by SR) → exponential decay → normalize to 0.85 peak. IR length `max(256, SR×0.023)` — duration constant in seconds across SR (unit-tested). Generated only in `prepareToPlay`; `loadImpulseResponse(..., Normalise::no)`.

**Convolution latency:** default `juce::dsp::Convolution` engine is zero-latency (direct first partition); cabinets correctly report nothing. IR install is asynchronous — `getCurrentIRSize()==0` guard skips convolution for the first block(s) after prepare (momentary EQ-only tone, masked by engine startup-silence counters).

## 2. Gain Staging & Clipping

Expected input: guitar-level ≈0.1–0.5 peak from InputChain; up to ~1.2+ with Boost/Distortion in front (pedals bound their outputs; CleanAmp additionally clamps input at ±8).

| Stage | Clean | Classic | HighGain | Boutique | Chime |
|---|---|---|---|---|---|
| Max pre-shaper gain | ×3.75 (drive×headroom map) | ×11.7 (drive 6 → 1.2+6·1.75), sag-reduced | ×19.5 (drive 10 → 1.5+10·1.8), sag+gate-reduced | ×~5 dynamic (envelope reduces when hot) | ×7 (1+4·1.5), sag/bias-reduced |
| Shaper bound | tanh-blend ≤~1.16 | Σ tanh weights ≤1.30 | Σ ≤1.24, then ×0.32 trim | ≤~1.22 (but see discontinuity F-007) | Σ ≤1.00 |
| Post-shaper EQ max | shelves ±5.5 dB | presence+bright ≤+7.6 dB (HF) | presence ≤+4.7 dB | shelves ≤+6 dB | brilliance ≤+9 dB (HF) |
| Output safety | `softLimit(1.25)` after master | none (master ≤2 raw) | trim 0.32 → worst ≈1.3 | none | none (Σ≤1.0 helps) |
| Worst-case peak into next stage | ≤1.25 | ≈3–6 (HF, master 2, theoretical) | ≈1.3 | ≈3–5 (theoretical) | ≈2–4 (HF, master 2) |

- **Hot input:** every shaper is tanh-bounded, so hot input compresses rather than explodes; sag/gate envelopes further reduce sustained drive. CleanAmp hard-clamps ±8 first. No unbounded path found.
- **Boost/Distortion → HighGainAmp → Modern4x12:** chain-tested (p10c/p10d/p10e/p10g); 0.32 amp trim + 0.70 cab trim keeps the professional chain ≈≤1.0 sustained.
- **Limiter interaction / 0.999 pinning after P12D:** HighGain chain trims make sustained pinning unlikely (verified by `p10c_high_gain_chain_outputchain_limiter_independence`). ClassicAmp and BoutiqueAmp at master ≈2 with hot HF content can sit on the limiter — bounded and safe, but worth a listening check (L-row in matrix). Cabinet LP contours (≤8.2k) shave the HF where shelf boosts live, reducing real-world peaks well below the theoretical worst case.
- The theoretical 3–6× peaks (Classic/Boutique, master 2, shelves maxed) are caught by the always-on OutputChain limiter (`HARD_ABS_LIMIT_LINEAR` 24); they cannot escape, but they will engage limiting audibly — gain-staging hygiene, not a safety hole.

## 3. Tone / DSP Safety

| Check | Clean | Classic | HighGain | Boutique | Chime | Cabs (×3) |
|---|---|---|---|---|---|---|
| Aliasing | 8× OS, polyphase IIR — low risk | same | same (4 stages benefit most) | same | same | linear (convolution+IIR) — none |
| Denormals | guard + flush ✅ | ❌ no guard **F-003** | ❌ | ❌ | ❌ | ❌ (conv/IIR tails) **F-003** |
| NaN/Inf in | scrub ✅ | ❌ **F-002** | partial (`conditionPreampInput` finite-checks mid-chain, but gate envelope latches NaN → **persistent silence**) **F-002** | ❌ | ❌ | scrub ✅ |
| DC buildup | in+out DC blockers; asym shapers DC-blocked ✅ | 18 Hz HP post-core ✅ | 18 Hz HP ✅ | 18 Hz HP ✅ | 18 Hz HP ✅ (bias shift +0.03/0.02 offsets blocked) | 18 Hz one-pole post-IR ✅ |
| Filter stability | fixed-freq shelves, bounded gains ✅ | bounded (Q ≤0.78, gains ≤+7.6 dB) ✅ | bounded ✅ | bounded ✅ | bounded; LP min 2.2 kHz ✅ | bounded ±12 dB; cut freqs clamped; IR biquads designed in double ✅ |
| Feedback/resonance | room FDN-lite: feedback ≤0.50, soft-limited writes ✅ | none | none | none | none | comb reflections gains ≤0.20, decaying — stable ✅ |
| Stereo/mono | per-channel state ≤8ch ✅ | ✅ | ✅ | ✅ | ✅ | mono IR duplicated per channel; per-channel filter state ✅ |
| Phase | min-phase IIR throughout | same | same | same | same | IR is min-phase-ish synthetic; no linear-phase latency |
| SR coefficient correctness | filters from inner rate ✅; room delays from SR ✅ | filters ✅; **sag decay constant per-sample → SR-dependent feel** F-004 | filters ✅; sag + gate envelopes fixed coeffs F-004 | filters ✅; **env attack/release constexpr, comment says 4×/176 kHz but runs 8×** F-004/F-008 | filters ✅; cathode/sag fixed coeffs F-004 | filter design from SR ✅; **IR decay rate per-sample → tail shape varies with SR** F-004 (window length itself unit-tested constant) |

## 4. Latency / PDC

- **All five amps** report oversampler latency through `setProcessingLatency()` — correct (8× polyphase IIR ≈ a few samples). Bypass toggles latency N↔0 via `ProcessorBase::setBypassed`; the P13A-F001 listener path (graph rebuild → async host `setLatencySamples`) covers add/bypass/remove — same verified pipeline as P13B (tests `:4514`, `:5710`).
- **Rounding inconsistency (F-005):** Classic/HighGain/Boutique/Chime truncate (`(int)`), CleanAmp ceils, Neural rounds. Sub-sample disagreement only; harmless but should standardize on rounding.
- **Cabinets** correctly report zero: default JUCE convolution is the zero-latency engine, IIR voicing adds none.
- No mismatch found between internal delay and reported latency anywhere in scope.

## 5. RT Safety

- **Allocations:** none on the audio path. IR generation/`loadImpulseResponse` happen in `prepareToPlay` only. `ArrayCoefficients` assignment into existing `IIR::Coefficients` is the documented allocation-free pattern. CleanAmp's room uses a preallocated 8×32768 buffer.
- **Locks/logging/strings:** none in any amp/cab `processBlock`. CleanAmp telemetry uses the POD lock-free queue (strings built off-thread).
- **Dynamic resize:** none.
- **Oversize-block handling:** CleanAmp (`emergencySafePass`) and all three cabinets (`canProcessBlock` dry fallback + atomic counter) are safe. **Classic/HighGain/Boutique/Chime have no guard — `oversampler.processSamplesUp` on a block larger than `initProcessing` declared overruns the oversampler's preallocated internal buffers (assert in debug, memory corruption in release).** This is F-001, the most serious finding of this audit. The P1 oversize test covers the cabinets but none of the amps.
- **Per-sample cost:** 2–4 `tanh` per sample at 8× inner rate (≈16–32 tanh per base sample/channel) — heaviest CPU family in the plugin, consistent with the P13B F-006 profile note. Bounded and correct; profile before optimizing.
- **Parameter patterns:** raw atomic reads per block; voicing recompute thresholded at 1e-4 (fires per automation tick; coefficient jumps are unsmoothed — standard zipper risk on fast knob sweeps, no instability).

## 6. Tests

**Existing (strong):** finite/audible/no-clip guard for all five amps (`p11a_amp_tonal_stability_guard`); state round-trips (p11a/p11b); cabinet catalog + voicing stability (p11b); chain tests for Distortion→HGA→Modern4x12, Boost→HGA→Modern4x12, Fuzz→ClassicAmp→Cabinet, Distortion→CleanAmp→Cabinet (p10c nominal + bounded); palm-mute and extreme-gain HGA (p10c); limiter independence (p10c); fizz control (p10d); mute/helicopter/stuck-silence regression guards (p10e); ducking and noise-floor guards (p10f/p10g); sustain stability with and without cab; IR window constant across SR; engine-level finite/level-stable across SR/blocks; P1 bypass-transition + oversize-fallback for **cabinets**.

**Missing:**
1. **Oversize-block test for the five amps** (would expose F-001 immediately — extend `runP1OversizedFallback` to amps once guards exist).
2. **NaN/Inf injection for amps** (mirror P13B-FIX1's two-phase latch/recovery tests; HighGainAmp's gate-envelope latch → persistent silence is the key case).
3. **Boutique/Chime chain + hot-input + automation tests** — both amps are covered only by the generic p11a guard; no dedicated chain, palm-mute, or automation-stress coverage.
4. **SR-consistency tone test** for sag/envelope feel (would catch F-004 regressions; assert envelope time constants in ms are SR-invariant once fixed).
5. **Vintage2x12 hot-output bound test** (only cab without trim/containment).
6. No sustained-soft-ceiling-abuse test for ClassicAmp/Boutique at master 2 (limiter-independence test covers the HGA chain only).

## 7. Manual Listening Matrix

Executable version in `artifacts/audits/P13C-amps-cabinets-checklist.md`.

| # | Scenario | Listen for |
|---|---|---|
| L1 | CleanAmp → Cabinet (Atlas) | clean headroom, reverb return level, no pumping from softLimit |
| L2 | ClassicAmp → Cabinet | sag feel, bright cap interaction, master 2.0 limiter engagement |
| L3 | HighGainAmp → Modern 4x12 | chunk, fizz ceiling, gate floor audibility between notes |
| L4 | Boost → HighGainAmp → Modern 4x12 | tightness, no mush, no limiter pinning |
| L5 | Distortion → HighGainAmp → Modern 4x12 | stacked-gain noise floor, fizz proxy |
| L6 | Fuzz → ClassicAmp → Cabinet | fuzz cleanup with guitar volume, sag interaction |
| L7 | High master preset (Classic + Boutique at level ≈2) | sustained OutputChain limiting? 0.999 pinning? |
| L8 | Low input / high output vs high input / normal output | gain-staging symmetry, noise floor |
| L9 | Palm muting, high gain | low-end flub vs tight, sag pump |
| L10 | Sustained notes, high gain | bloom/decay smoothness, gate tail cut |
| L11 | Noise floor between notes (HGA gate floor 0.07–0.18) | hiss modulation/breathing |
| L12 | BoutiqueAmp edge-of-breakup picking dynamics | crackle when straddling cleanThreshold (F-007) |
| L13 | Same preset at 44.1k vs 96k | sag/touch feel difference (F-004) |

## 8. Scorecard (1–10)

| Model | DSP safety | Gain staging | Tone arch | High-gain stab | Cab stab | Latency/PDC | RT safety | Param safety | Tests | Release |
|---|---|---|---|---|---|---|---|---|---|---|
| **Clean Amp** | 9 | 9 | 8 | 9 | — | 9 | 9 | 9 | 8 | **9** |
| **Classic Amp** | 6 | 7 | 8 | 8 | — | 9 | 8 | 8 | 8 | **7** |
| **High Gain Amp** | 6 | 8 | 8 | 9 | — | 9 | 8 | 8 | 9 | **8** |
| **Boutique Amp** | 6 | 7 | 7 | 7 | — | 9 | 8 | 8 | 6 | **7** |
| **Chime Amp** | 6 | 8 | 8 | 8 | — | 9 | 8 | 8 | 6 | **7** |
| **Cabinet (Atlas)** | 8 | 9 | 8 | — | 9 | 10 | 9 | 9 | 9 | **9** |
| **Modern 4x12** | 7 | 8 | 8 | — | 9 | 10 | 9 | 9 | 9 | **9** |
| **Vintage 2x12** | 7 | 7 | 8 | — | 8 | 10 | 9 | 9 | 7 | **8** |

Deduction drivers: missing oversize-block guards + NaN scrub + denormal guards (Classic/HGA/Boutique/Chime → DSP 6); Boutique/Chime test gap; Vintage2x12 lacks output trim/containment; Boutique waveshaper discontinuity.

## 9. Risk Table

| ID | Affected | Severity | Probability | Evidence | Files | Recommended fix | Implementer | Blocks drive/dynamics audit? |
|---|---|---|---|---|---|---|---|---|
| F-P13C-001 | ClassicAmp, HighGainAmp, BoutiqueAmp, ChimeAmp | **Medium-High** | Low | No oversize-block guard before `oversampler.processSamplesUp`; JUCE Oversampling buffers are sized by `initProcessing` — a larger host block overruns them (assert/debug, heap corruption/release). CleanAmp guards (`CleanAmp.h:192` comment names the exact scenario); cabinets + Neural guard; P1 oversize test covers cabinets but no amp | `ClassicAmp.h:115`, `HighGainAmp.h:120`, `BoutiqueAmp.h:128`, `ChimeAmp.h:124` | Add `canProcessBlock`-style dry fallback (or CleanAmp's `emergencySafePass`) before upsampling; extend `runP1OversizedFallback` to all five amps | Codex 5.5 | No (low probability; flag as P13C-FIX1 priority 1) |
| F-P13C-002 | Same four amps | Medium | Low-Med | No NaN/Inf input scrub (CleanAmp scrubs; all cabs scrub). NaN latches IIR states and envelopes. **HighGainAmp worst case:** `inputNoiseEnvelope` latches NaN → gate gain NaN → `conditionPreampInput` zeroes → persistent output silence until reset/heal | same files (+`HighGainAmp.h:217-233`) | Add input scrub + finite guards on sag/cathode/gate envelopes (P13B-FIX1 pattern); add two-phase NaN latch/recovery tests | Codex 5.5 | No |
| F-P13C-003 | Same four amps + all three cabinets | Medium | Medium | No `ScopedNoDenormals` in `processBlock` (CleanAmp only); IIR tails, sag/cathode envelopes, convolution tails decay through denormal range — CPU spikes on non-FTZ hosts. Exact P13B F-001 pattern | 4 amp + 3 cab headers | 7 one-line additions | Codex 5.5 | No |
| F-P13C-004 | Classic, HGA, Boutique, Chime, SyntheticIR | Low-Med | Certain (tone consistency, not safety) | Per-sample envelope constants not derived from rate: `AMP_SAG_DECAY` 0.9992 (`Constants.h:135`), Chime `cathodeBiasCoeff` 0.0012 / sag 0.9988, Boutique `envAttack/envRelease` constexpr (comment claims 4×/176.4 kHz, code runs 8×); IR `decayRate` per-sample → tail decay time halves at 96k | amp headers, `SyntheticIR.h:139` | Derive coefficients from inner rate in `prepare`; convert IR decay to per-second constant | Codex 5.5 (after FIX1 items) | No |
| F-P13C-005 | Classic, HGA, Boutique, Chime | Low | Certain | Latency truncated `(int)getLatencyInSamples()` vs CleanAmp `ceil` vs Neural `round` — sub-sample under-report, inconsistent convention | 4 amp headers | Standardize on `std::round` | Codex 5.5 | No |
| F-P13C-006 | Vintage2x12 (and ClassicAmp/Boutique output stages) | Low | Low-Med | Output-safety inconsistency: Atlas has soft-knee containment (0.97), Modern has 0.70 trim, Vintage has neither; Classic/Boutique amps have no output limit at master ≤2 (+shelves) — bounded by tanh sums but can push 2–6× peaks into OutputChain | `Vintage2x12Cabinet.h:154-166`, `ClassicAmp.h:138-143`, `BoutiqueAmp.h:214-219` | Harmonize: add trim or containment to Vintage2x12; consider CleanAmp-style `softLimit` on Classic/Boutique masters. Listen first (L7) | Codex 5.5 (conditional on listening) | No |
| F-P13C-007 | BoutiqueAmp | Low (quality) | Medium at edge-of-breakup settings | Piecewise stage1 is discontinuous at `cleanThreshold`: e.g. t=0.6 → poly 0.557 vs tanh branch 0.526 (≈6% step). Signal straddling the threshold steps per-sample → crackle/extra harmonics | `BoutiqueAmp.h:181-194` | Match branch values at threshold (solve gain/offset) or crossfade region | Codex 5.5 | No |
| F-P13C-008 | BoutiqueAmp, ChimeAmp | Info | Certain | Header comments claim "4x oversampled rate"; code constructs `Oversampling(2,3)` = 8× and sets `innerRate = sr*8` | both headers | Fix comments when touching F-004 | Codex 5.5 | No |
| F-P13C-009 | All cabinets | Info | Certain (first blocks after prepare) | Async IR install: `getCurrentIRSize()==0` → convolution skipped, EQ-only tone momentarily; masked by engine startup-silence counters | cab headers | None required | — | No |

No Critical findings. Nothing blocks the drive/dynamics audit.

## 10. Verdict

**Amps and cabinets are safe to keep enabled.** Every saturation path is tanh-bounded, every output is caught by the always-on P12D OutputChain limiter, filters are stable with clamped gains, the audio paths are allocation- and lock-free, and the heavily-used high-gain chain (Boost/Distortion → HighGainAmp → Modern 4x12) is the best-tested signal path in the product.

- **Strongest:** **CleanAmp** among amps — it is the reference implementation (scrub, denormal flush, oversize fallback, bounded room, output soft-limit, telemetry); **Cabinet/Atlas 4x12** among cabs (scrub + fallback + output containment + best test coverage).
- **Riskiest:** **BoutiqueAmp** — least tested (generic p11a guard only), discontinuous waveshaper at its signature edge-of-breakup operating point, missing all three safety guards, and mis-documented oversampling. **HighGainAmp** has the single worst failure mode (NaN → persistent silence via the gate envelope) but the best test coverage.
- **Blockers:** none before the drive/dynamics audit.
- **Codex 5.5 next (P13C-FIX1, in order):**
  1. **F-001:** oversize-block dry fallback in the four unguarded amps + extend the P1 oversize test to all five amps.
  2. **F-002:** NaN/Inf input scrub + envelope finite guards in the same four amps + latch/recovery tests (HighGainAmp gate case explicitly).
  3. **F-003:** `ScopedNoDenormals` in 4 amps + 3 cabinets (7 one-liners).
  4. **F-005/F-008:** latency rounding + comment corrections (trivial, same pass).
  5. **F-004/F-006/F-007:** schedule after listening matrix results — these change tone/feel and deserve A/B confirmation, unlike the pure safety items above.

---
*Audit was strictly read-only. No source, test, preset, or project files were modified. Output limited to `docs/reports/` and `artifacts/audits/`.*
