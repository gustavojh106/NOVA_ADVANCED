# P13B — Stateful / Time-Based Effects Audit Report

- **Task ID:** P13B-stateful-timebased-effects-audit
- **Date:** 2026-06-12
- **Auditor:** Claude Fable (read-only audit; no code modified)
- **Baseline:** post-P12D (OutputChain transparent-safety limiter) + P13A-F001 (host PDC reporting via `AudioEngine::LatencyListener`)
- **Scope:** Delay, Reverb, Neural (primary); Chorus, Flanger, Octave (secondary stateful processors found in repo)
- **Assumptions per task brief:** global mix = 100% (F003), line gain not used as silence (F004), no tone conclusions from knob minimums (F012)

---

## 1. Inventory

### 1.1 DelayPedal — `Source/Effects/Pedals/Delay/DelayPedal.h`

| Aspect | Detail |
|---|---|
| Class | `DelayPedal final : ProcessorBase, TempoSyncable` |
| Parameters | mode (Analog/Tape/Digital/Reverse), time 35–2500 ms, sync + 12 divisions, feedback 0–0.97, tone 600–14000 Hz, lowCut 20–2200 Hz, spread, texture, modDepth, modRate 0.05–6 Hz, mix, duck, swell, reverse, freeze (bool) |
| Reported latency | None (correct — delay time is creative effect time, not PDC latency) |
| State buffers | 2× `DelayLine` (~4.5 s + block + 16 samples each), 2×2 diffusion all-pass (128 samples), 4 biquad pairs (toneLPF, fbHighPass, dcBlock, freezeLPF), 2 envelope followers (duck/swell), 4 LFO phases, 2 reverse-window phases, freeze hold states |
| Feedback paths | Main loop: wet → crossfeed mix → toneLPF → fbHighPass → diffusion APs → `softSaturate` (tanh) → ×feedback (≤0.97) ×feedbackTrim (≤~0.95) → written back with input. Freeze loop: hold state → freezeLPF → `softSaturate(×1.06 drive)` at effective gain ~0.9987–1.0+ — bounded by tanh, intentional infinite hold |
| SR-dependent coefficients | Biquads recomputed against live `sr`; diffusion AP delays scaled by `sr/48000`; all ms→samples conversions use live `sr` |
| Bypass | `ProcessorBase` crossfade (10 ms); wet engine not run while bypassed; `reset()` called on un-bypass → tails cleared (intentional, commented in base) |
| Reset | Clears delay lines, all filters, APs, envelopes, phases, freeze state, smoothers re-seeded from params |
| prepareToPlay | Guards `sampleRate <= 0`; allocates lines sized to SR + block; re-seeds 11 smoothers; resets filters; `prepareBypassSmoother`; `reset()`; sets `isPrepared` |
| processBlock | `isPrepared` + `beginBypassProcess` gate; input NaN/Inf scrub; per-sample loop with smoothed params; feedback-filter coefficients refreshed every 8 samples; telemetry capture; `endBypassProcess` |

### 1.2 ReverbPedal — `Source/Effects/Pedals/Reverb/ReverbPedal.h`

| Aspect | Detail |
|---|---|
| Class | `ReverbPedal final : ProcessorBase` wrapping `Nova::Reverb::Engine` (8-line FDN) |
| Parameters | mode (Spring/Plate/Hall/Room/Shimmer/Cloud), decay, tone, size, damping, bassCut, diffusion, width, mod, predelay 0–250 ms, mix, duck, swell, gate, reverse, freeze (bool), shimmerPitch (choice) |
| Reported latency | None (correct — predelay is creative, not PDC) |
| State buffers | 8 FDN `CircularDelay` lines (12000 samples @48k-ref, SR-scaled), 6 diffuser APs, 3 dispersion APs (Spring), mid/side predelay (~0.32 s), ER tap buffer (0.15 s), `GrainPitchShifter` (0.35 s, 4 grains), 3 `CharacterDelayVoice` (drip/sheen/bloom), 2 `ReverseBloomVoice` (0.52 s), per-line dampers/HP/LP/DC blockers, 4 performance envelopes |
| Feedback paths | FDN: damped line outputs → Hadamard-8 → per-line DC blocker → `tanh(fb·x·0.4)·2.5` soft limit → loop-input HP (30 Hz) → write. `fb` clamped ≤0.92 in `configure()`; freeze raises to ≥0.9992 with damping mostly bypassed (intentional hold), still tanh-bounded. Shimmer: pitch-shifted FDN mono sum reinjected at `shimmerMix` 0.44, LP-filtered |
| SR-dependent coefficients | All delays scaled by `ratioToRef = sr/48000`; all one-pole/damper coefficients computed against live `sr`; envelope coefficients exp-derived from `sr` |
| Bypass | Same base-class behavior — tails truncated, state reset on un-bypass |
| Reset | Clears every buffer, filter, envelope, phase; telemetry reset |
| prepareToPlay | Guards SR; sizes dry/wetInput scratch to prepared block; `engine.prepare`; invalidates `last*` parameter cache (forces sync `configure`); `reset()` |
| processBlock | Gate; `ScopedNoDenormals`; param-change-detected `configure()` (not per-block); larger-than-prepared block → dry passthrough (no alloc); buffer scrub (NaN/Inf/denormal/±8 clamp); wet-input peak trim; block engine render; per-sample wet sanitize + softCeiling(1.03); equal-power mix |

Safety layers unique to Reverb: `softCeiling` at 6+ points, `sanitizeAudioSample` (NaN→0, denormal→0, ±8 clamp), `wetInputTrimForPeak`, `finalWetTrimForPeak`, per-line and output DC blockers. This is the most defended wet path in the repo.

### 1.3 NeuralPedal — `Source/Effects/Pedals/Neural/NeuralPedal.h` (+ trivial `.cpp`)

| Aspect | Detail |
|---|---|
| Class | `NeuralPedal final : ProcessorBase` |
| Parameters | drive 0–100, focus, detail, comp, mix, level |
| Reported latency | **Yes** — `setProcessingLatency(round(oversampler.getLatencyInSamples()))` (8× polyphase IIR oversampler, 2ch/3 stages) |
| State buffers | `juce::dsp::Oversampling<float>` internal filters, 7 `OnePoleFilterBank` (2ch fixed arrays), `EnvelopeFollower`, scratch dry buffer |
| Feedback paths | None across samples — `feedbackBlend` is intra-sample dense→silky coupling, tanh/atan-bounded |
| SR-dependent coefficients | All one-pole cutoffs computed against `innerSampleRate = sr × 8` |
| Bypass | Base class; bypass toggles reported latency N↔0 → triggers engine latency rebuild → host PDC update (P13A-F001 path) |
| Reset | Oversampler + all filter banks + envelope reset; smoothers re-seeded |
| prepareToPlay | Guards SR; `initProcessing(blockSize)`; latency reported; scratch sized; preallocated channel guard (`kMaxHelperChannels = 2`) |
| processBlock | Gate; `ScopedNoDenormals`; NaN/Inf scrub; oversize-block fallback (dry, counted in `fallbackBlockCount` atomic); channel-capacity fallback restores dry from scratch; mix snap at 0/1 for exact transparency |

**Important factual note:** despite the name and CLAUDE.md ("`Neural/` uses RTNeural inference for neural amp modeling"), `NeuralPedal` contains **no RTNeural usage** — `grep RTNeural Source/Effects` returns nothing. It is an analytic two-stage waveshaper (denseClip/silkyClip) with envelope-driven sag at 8× oversampling. `Source/Lib/RTNeural` and `Lib/models/` are vendored but unused by this pedal. See F-P13B-007.

### 1.4 ChorusPedal — `Source/Effects/Pedals/Chorus/ChorusPedal.h`

- 2 ch × 3 voice `DelayLine` (~45 ms + 128), input HPF / tone LPF / DC-block biquads, `feedbackState[2]`, LFO + drift phases.
- Feedback small and mode-fixed: internal ≤0.10, cross ≤0.08, all writes through `softClip` (tanh). Feedback state derived from post-softClip wet ×(0.86−0.10·mix) → loop gain ≪1.
- No latency (correct). Bypass/reset/prepare follow base pattern; smoothers on all params; tone/mode filter updates cached and thresholded.
- **Gaps:** no NaN/Inf input scrub (unlike Delay/Flanger/Neural/Reverb) and no own `ScopedNoDenormals` (see F-P13B-001/002).

### 1.5 FlangerPedal — `Source/Effects/Pedals/Flanger/FlangerPedal.h`

- 2 `DelayLine` (~12.5 ms + 128), regen LPF, wet LPF, DC block, **dedicated feedback-loop DC blocker**, `feedbackState[2]` clamped ±1.15.
- Feedback parameter is bipolar ±0.95; loop bounded by `softClip` on write, DC-blocked, hard-clamped seed. Telemetry instruments the loop (seed/state/inject/write peaks).
- Input NaN/Inf scrub present. No reported latency (correct). No own `ScopedNoDenormals` (F-P13B-001).

### 1.6 OctavePedal — `Source/Effects/Pedals/Octave/OctavePedal.h`

- Stateful but not time-based on the output path: oscillator synthesis driven by `PeriodTracker` (768-sample decimated analysis history, normalized autocorrelation every 8 decimated samples), 2 envelope followers, ~14 biquads.
- Explicitly reports `setProcessingLatency(0)` — correct; output is synthesized in-phase, no inherent delay.
- Has `ScopedNoDenormals`. **No NaN input scrub** — NaN would latch in biquad/envelope/tracker state until reset (F-P13B-003). Autocorrelation cost is bounded (~O(maxLag·window) per 32 input samples) and acceptable.

### 1.7 Other stateful processors noted (out of audit scope, not analyzed in depth)

- `InputChain` pitch-shift transpose (buffer-based delay line, varispeed) — global chain, covered by P13A.
- `CabinetPedal`/`SyntheticIR` (1024-tap convolution state) — deferred to the amps/cabinets audit phase.
- `PhaserPedal`, `TremoloPedal`, `AutoWahPedal`, `CompressorPedal`, `NoiseGatePedal` — stateful (allpass/LFO/envelope) but no delay-line/tail behavior; NoiseGate reports lookahead latency correctly via `setProcessingLatency(lookaheadSamples)`.
- Legacy flat `Pedals/ChorusPedal.h`, `Pedals/CompressorPedal.h` exist but are superseded by subfolder versions per CLAUDE.md; registry uses subfolder classes.

---

## 2. Audio Safety

| Check | Delay | Reverb | Neural | Chorus | Flanger | Octave |
|---|---|---|---|---|---|---|
| NaN/Inf input scrub | ✅ | ✅ (full sanitize) | ✅ | ❌ **F-002** | ✅ | ❌ **F-003** |
| Denormal protection in DSP scope | ❌ **F-001** | ✅ (`ScopedNoDenormals` + 1e-30 flush) | ✅ | ❌ **F-001** | ❌ **F-001** | ✅ |
| Feedback runaway | tanh-bounded loop, fb ≤0.97×trim<1 effective; freeze tanh-bounded | fb ≤0.92; loop `tanh(x·0.4)·2.5` limit; freeze intentional ≥0.9992, bounded | no feedback | loop gain ≪1, softClip | softClip + DC block + ±1.15 clamp | n/a |
| Gain explosion | output trims + duck/swell clamps; performanceGain ≤1.08 | softCeiling chain + wet trims | wetTrim compensation, atan/tanh bound | softClip wet | softClip + polarity-aware | wetTrim normalizer |
| Silence/stuck output | none found; freeze is intentional hold | gate floor ≥0.02, freeze intentional | mix-snap avoids stuck crossfade | none found | none found | dry-only fast path correct |
| DC buildup | dcBlock on wet out + fbHighPass in loop; dedicated test | per-line DCBlocker in loop + output DC; dedicated test | dcBlock one-pole 18 Hz | DC-block biquad on wet | loop + wet DC blockers; dedicated test | dcBlock per channel |
| Pre-OutputChain clipping | wet peaks bounded ~1.1; mix equal-power | wet ceiling 1.02–1.05 | bounded shapers | bounded | bounded | tanh-bounded voices |
| Stereo / mono input | `jmin(2, ch)`, mono mirrors L | mid/side, mono path runs L into both engine inputs | channel-capacity guarded, fallback to dry | mono mirrored | mono mirrored + mono-safe feedback | mono detector sum |
| Parameter sanitizer coverage | jlimit on every derived value | jlimit + configure clamps | jlimit + control normalization | jlimit + delay clamp 0.85–32 ms | delay clamp 0.35–12 ms | jlimit + snap targets |
| Smoothing of dangerous params | 11 smoothers, feedback/time 50 ms | 10 engine smoothers + configure-on-change | 7 smoothers; drive 12 ms | 5 smoothers | 6 smoothers incl. feedback | 5 smoothers |

Engine-level backstop confirmed: `AudioEngine` output health monitor sanitizes/meters every block and auto-heals after `CORRUPT_BLOCKS_BEFORE_HEAL = 2` corrupt blocks (graph reset). This mitigates — but does not excuse — the per-pedal scrub gaps, because a heal event is an audible dropout plus total state loss.

## 3. Latency / PDC Correctness

- **Who reports latency:** Neural (8× oversampler) among the audited set; elsewhere Overdrive/Distortion/Fuzz/Boost/Metal (4× OS), all five amps, NoiseGate (lookahead), OutputChain (limiter lookahead). All use `setProcessingLatency()` except OutputChain (graph-global, uses `setLatencySamples` directly by design — P12D).
- **Who correctly reports zero:** Delay, Reverb, Chorus, Flanger, Octave. Delay time, reverb predelay, and the Reverse-mode window are *creative* time, not PDC latency. No mismatch found between any internal delay and reported latency.
- **Bypass:** `ProcessorBase::setBypassed` swaps reported latency N↔0 and flags the change; `AudioEngine` rebuilds graph latency on bypass-latency change (test: *"AudioEngine rebuilds graph latency when bypass changes node latency"*, `AudioEngineTests.cpp:5710`), and P13A-F001 forwards it to the host async/deduped (`PluginProcessor.cpp:470–513`, clamped to `MAX_GRAPH_LATENCY_SAMPLES`, never called from `processBlock`). Test *"P13A host PDC reports live AudioEngine graph latency"* (`:4514`) covers the pipeline.
- **Bypass timing alignment:** during transitions the dry path is delayed through `bypassLatencyLines` so the 10 ms crossfade is sample-aligned; in steady bypass, latency 0 is reported and dry passes undelayed — consistent.
- **Add/remove:** graph publish → latency listener → host update. Covered by GraphBuilder latency tests (`:6170`, `:6314`).
- **Tails vs rebuild:** any graph rebuild (add/remove/preset/heal) destroys or resets wet state — tails are cut. Bypass also cuts tails (see §4). This is consistent, intentional behavior, not a PDC bug.
- **Residual risk (carried from P13A):** Release VST3 + manual DAW PDC validation still pending; Neural's float→int latency rounding leaves a sub-sample residual (standard, inaudible).

## 4. Tail Behavior

| Event | Behavior | Intentional? |
|---|---|---|
| Bypass engaged | Wet engine stops rendering; 10 ms equal-power crossfade to (latency-matched) dry; tail audibly truncated | Yes — by design; no "trails/spillover" mode exists |
| Un-bypass | `beginBypassProcess` calls `reset()` → all tails/state cleared so stale audio never jumps back in (commented in `ProcessorBase.h:232`) | Yes — explicitly commented |
| Remove/delete pedal | Node destroyed on graph publish; tail gone instantly; engine startup-silence counter (`STARTUP_COUNTER_GRAPH_CHANGE = 6`) mutes the transition | Yes (verify click-freedom by ear — checklist item) |
| Preset switch | Full engine rebuild from state; all tails cleared | Yes |
| Engine off/on, prepare cycle | `prepareToPlay` reallocates + `reset()` — clean | Yes |
| Sample-rate switch | All buffers re-sized from live SR, all coefficients recomputed, full reset; covered by *"p10e_sample_rate_reset_recovers_stuck_chain"* and P1 multi-rate tests | Yes |

Tail truncation on bypass is internally documented (code comment) but **not user-facing documented**. Guitarists often expect delay/reverb trails to ring out through bypass. This is a product decision, not a defect — recorded as F-P13B-005.

## 5. RT Safety

- **Allocation in processBlock:** none found in audited pedals. All buffers sized in `prepareToPlay`; oversize blocks handled by explicit no-alloc fallbacks (Reverb → dry passthrough; Neural → dry restore + atomic counter; ProcessorBase → overflow flag).
- **Locks:** none on any audited audio path.
- **Logging/strings:** none on the audio thread. `PedalSignalTelemetry` publishes POD events with fixed `char` tags into `RealtimeSignalTelemetryQueue`; report strings are built off-thread. Debug telemetry accumulators are plain floats.
- **Dynamic resize on audio thread:** none. Reverb `setLength`/`configure` only adjusts read lengths within preallocated buffers.
- **Parameter lookup:** raw pointer `->get()` (atomic relaxed) — fine.
- **Graph mutation interactions:** pedals are passive nodes; mutation handled by ControlPlane/AudioPlane (out of scope, untouched).
- **CPU hotspots (bounded, correctness-safe, but real):**
  1. **Neural:** `updateToneModel()` runs once per *base-rate* sample inside the 8× loop — 7 `OnePoleFilterBank::setCutoff` calls each containing `std::exp`, plus ~15 `jmap`s. ≈ 7 exp + heavy scalar work × 48 k/s.
  2. **Reverb:** `outputToneL/R.setCutoff` per sample (2× exp/sample) inside `processBlock`; plus an extra full-buffer `measurePeak` + scrub pass.
  3. **Delay:** `getModeStyle()` struct build per sample, several `std::pow` per sample (swell path), feedback biquad coefficient rebuild (sin/cos) every 8 samples.
  These are the main reasons to profile before adding more per-instance load; none threaten correctness.

## 6. Test Review

Existing coverage in `Source/Core/AudioEngineTests.cpp` (compiled into Standalone, JUCE UnitTest "NOVA"):

- **Delay:** state round-trip, 5 s finite decaying tail, stereo decorrelation, mode signature differentiation, freeze stability, duck, reverse timing, swell, reverse+swell, automation stress, **feedback DC rejection, max-feedback boundedness, NaN/Inf sanitization under feedback, high-peak input under feedback** (`:11858–11982`). Excellent.
- **Reverb:** state round-trip + legacy mapping, finite decaying tail, loop DC rejection, stereo decorrelation, mode signatures, freeze/duck/swell/gate/reverse (+combos), configure-not-per-block (P7H), aggressive automation bounded, mode changes bounded, **max decay bounded, NaN/Inf sanitization, high-peak under max decay** (`:12105–12164`). Best-covered effect in the repo.
- **Neural:** state round-trip, mix-zero transparency, focus behavior, automation stress finite (`:3485–3608`).
- **Chorus/Flanger:** round-trip, mix-zero transparency, mode stereo signatures, automation stress; Flanger additionally has the full feedback-abuse quartet (DC, max feedback, NaN/Inf, high peak) (`:11982–12071`). Chorus does **not** have the feedback-abuse quartet (its loop gain is small, so lower priority).
- **Cross-cutting:** P1 pedals finite across prepared block sizes and sample rates + bypass transitions bounded (`:12239`, `:12265`); P8A/P8C bypass recovery incl. Reverb/Chorus; P10E distortion+reverb recovery guards; sample-rate reset recovery; DryWetMixer latency state machine; GraphBuilder latency capture; P13A host PDC test; bypass-latency rebuild test.

**Gaps:**
1. No test feeds **NaN into Chorus or Octave** (the two pedals without scrubs — the tests would fail today, which is exactly why they're worth writing after the fix).
2. No **denormal-load CPU test** (hard in unit tests; acceptable to leave to profiling).
3. No assertion that **Neural's reported latency equals oversampler latency** after prepare, nor a host-PDC assertion for *bypassing Neural specifically* (the generic bypass-latency rebuild test uses other latency-bearing nodes).
4. No **tail-truncation characterization test** (e.g., assert bypass kills tail within crossfade window) — behavior is intentional but untested, so a regression to "stale tail jumps back in" (the bug the reset prevents) is only caught indirectly.
5. Tests are synthetic (sine bursts/impulses) but well-designed — levels, automation sweeps and abuse cases are realistic enough for this stage. Realism gap is covered by the listening matrix.

## 7. Manual Listening Matrix

See `artifacts/audits/P13B-stateful-timebased-effects-checklist.md` for the executable version. Summary:

| # | Scenario | Setup | Listen for |
|---|---|---|---|
| L1 | Clean guitar → Delay | Analog mode, time 480 ms, fb 0.46, mix 0.32 | repeat clarity, no zipper on time knob, stereo spread |
| L2 | High gain → Delay | Distortion/HighGainAmp before Delay, fb 0.75+ | loop saturation character, no runaway, duck behavior |
| L3 | Clean → Reverb | Hall + Plate, decay 0.7 | smooth tail, no metallic ring, no DC thump |
| L4 | High gain → Reverb | Shimmer + Cloud, decay 0.9, freeze on/off | freeze stability over 60 s, shimmer not screeching |
| L5 | Bypass tails | Engage bypass mid-tail on Delay and Reverb | confirm truncation is the 10 ms fade, no click; judge whether trails-mode is needed (F-005) |
| L6 | Preset switch tails | Switch presets while tail rings | gap acceptable? clicks? |
| L7 | Rapid automation | Sweep Delay time + Reverb size/mode continuously | F-008: zipper/clicks on Reverb size; Delay pitch-slew is intentional |
| L8 | Remove pedal mid-tail | Drag Delay out of chain while repeating | click vs. masked-by-startup-silence gap (F-009) |
| L9 | SR/block change | 44.1k↔48k↔96k, 64↔1024 block, Standalone audio settings | clean recovery, no stuck state |
| L10 | DAW PDC check | Release VST3 in REAPER/Live: Neural enabled vs bypassed on a doubled track | null/phase test confirms PDC follows bypass (carries P13A pending item) |
| L11 | NaN robustness (destructive) | Debug-only: inject NaN upstream of Chorus | confirm engine auto-heal recovers; validates F-002 priority |

## 8. Scorecard (1–10)

| Effect | DSP safety | Latency/PDC | Bypass/Tails | RT safety | Param safety | Test coverage | Release readiness |
|---|---|---|---|---|---|---|---|
| **Delay** | 9 | 10 | 8 | 8 | 9 | 9 | **9** |
| **Reverb** | 9 | 10 | 8 | 8 | 9 | 10 | **9** |
| **Neural** | 8 | 9 | 9 | 7 | 9 | 7 | **8** |
| **Chorus** | 7 | 10 | 9 | 8 | 9 | 8 | **8** |
| **Flanger** | 8 | 10 | 9 | 8 | 9 | 9 | **9** |
| **Octave** | 8 | 10 | 9 | 8 | 9 | 8 | **8** |

Deductions explained: Delay/Chorus/Flanger −denormal guard; Chorus/Octave −NaN scrub; Neural −per-sample tone-model cost and thinner latency/PDC test coverage; Bypass/Tails capped at 8–9 everywhere because trails are truncated by design without user-facing documentation.

## 9. Risk Table

| ID | Effect | Severity | Probability | Evidence | Files | Recommended fix | Implementer | Blocks next family? |
|---|---|---|---|---|---|---|---|---|
| F-P13B-001 | Delay, Chorus, Flanger | Medium | Medium | `grep ScopedNoDenormals Source/Effects/Pedals` — absent from these three `processBlock` bodies; base-class guard is scoped to `beginBypassProcess`/`endBypassProcess` only (`ProcessorBase.h:200` comment mandates per-pedal guard). Decaying feedback tails are textbook denormal generators → CPU spikes on hosts without global FTZ/DAZ | `DelayPedal.h:562`, `ChorusPedal.h:340`, `FlangerPedal.h:358` | Add `juce::ScopedNoDenormals noDenormals;` at top of each `processBlock` after the bypass gate | Codex 5.5 | No |
| F-P13B-002 | Chorus | Medium | Low–Med | `ChorusPedal::processBlock` has no NaN/Inf scrub (Delay/Flanger/Neural do, Reverb fully sanitizes). NaN input latches permanently in biquad `z1/z2` and `feedbackState` (tanh(NaN)=NaN). Engine auto-heal recovers after 2 corrupt blocks but at the cost of an audible dropout + full graph state loss | `ChorusPedal.h:340` | Add the same `scrubInvalidSamples` used by `FlangerPedal.h:533` | Codex 5.5 | No |
| F-P13B-003 | Octave | Low–Med | Low | Same NaN-latch pattern: no input scrub; biquads, envelopes, `PeriodTracker.history` hold NaN until reset/heal | `OctavePedal.h:321` | Same scrub helper | Codex 5.5 | No |
| F-P13B-004 | Reverb | Low | Low | `Engine::processSample` (`ReverbPedal.h:1089`) duplicates `Engine::processBlock` (`:1255`) but lacks the per-line `softCeiling(…, 1.15f)` clamp present at `:1349`; pedal only calls `processBlock`, so the unused path is a divergence trap | `ReverbPedal.h:1089–1250` | Delete `processSample` or align it with `processBlock` | Codex 5.5 | No |
| F-P13B-005 | Delay, Reverb (all tailed FX) | Info / UX decision | Certain | `ProcessorBase.h:230–236` — un-bypass resets wet state; wet not rendered while bypassed → tails truncate at bypass. Intentional and commented, but invisible to users; no trails/spillover option | `ProcessorBase.h` | Document the behavior; optionally design a "trails" bypass mode later (NOT in this phase — touches bypass architecture) | Product decision; impl Codex 5.5 if approved | No |
| F-P13B-006 | Neural, Reverb, Delay | Low | High (constant cost) | Neural: `updateToneModel` per base-rate sample = 7 `exp` calls/sample (`NeuralPedal.h:305–313`); Reverb: `outputTone.setCutoff` 2×exp/sample (`:1277`); Delay: per-sample `getModeStyle` + `std::pow` + filter rebuild every 8 samples | `NeuralPedal.h`, `ReverbPedal.h`, `DelayPedal.h` | Rate-limit coefficient updates to every N samples / block edges with smoothing; profile first | Codex 5.5 (after profiling) | No |
| F-P13B-007 | Neural | Low (docs) / Med (product claim) | Certain | No RTNeural symbol in `Source/Effects` (grep empty); CLAUDE.md claims "Neural/ uses RTNeural inference". Pedal is an analytic waveshaper. Vendored RTNeural/Eigen/xsimd/models unused by it | `NeuralPedal.h`, `CLAUDE.md` | Correct CLAUDE.md (and any marketing naming), or actually wire an RTNeural model — separate feature task | Doc fix: Codex 5.5; feature: separate phase | No |
| F-P13B-008 | Reverb | Low | Medium | `configure()` applies new FDN line lengths instantly via `setLength` on any size/mode change (threshold 1e-4 → fires throughout a knob sweep); read-pointer jumps can zipper. Automation tests assert boundedness, not click-freedom | `ReverbPedal.h:877–897` | Only if listening test L7 confirms audible artifacts: crossfade or slew line-length changes | Codex 5.5 (conditional) | No |
| F-P13B-009 | All tailed FX | Info | Medium | Removing a pedal mid-tail destroys the node; startup-silence counter masks the swap but the gap/click behavior is unverified by ear | `AudioEngine.cpp` (not to be touched) | Listening item L8 only; no code change proposed | — | No |

No Critical or High findings. Nothing here blocks the amps/cabinets audit.

## 10. Verdict

**Delay, Reverb, and Neural are safe to keep enabled.** All audited stateful/time-based effects have bounded feedback paths (tanh/softClip/softCeiling at every loop), correct zero-or-reported latency, allocation-free and lock-free audio paths, sample-rate-aware coefficients, and clean prepare/reset lifecycles. Reverb is the most heavily defended processor in the codebase; Delay's feedback-abuse test suite is exemplary.

**Nothing needs to be hidden or demoted to beta-only.** Chorus and Octave carry the only real robustness gaps (missing NaN scrub), and the engine-level sanitizer + auto-heal backstop keeps even those from being release blockers.

**No blocker exists before moving to the amps/cabinets family.** The pending items that matter (Release VST3 build + manual DAW PDC null test) carry over from P13A-F001 and are validation tasks, not code defects.

**Recommended next work for Codex 5.5,** in order, all small and low-risk:
1. F-P13B-001 — add `ScopedNoDenormals` to Delay/Chorus/Flanger `processBlock` (3 one-line changes).
2. F-P13B-002/003 — add NaN/Inf input scrub to Chorus and Octave (copy Flanger's helper), plus matching abuse tests.
3. F-P13B-004 — remove or align `Reverb::Engine::processSample`.
4. F-P13B-007 — correct the RTNeural claim in CLAUDE.md.
5. F-P13B-006 — defer until a profiling pass; do not optimize blind.

---
*Audit was strictly read-only. No source, test, preset, or project files were modified. Output limited to `docs/reports/` and `artifacts/audits/`.*
