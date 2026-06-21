# P13D-FIX1 — Claude Post-Fix Review

**Task ID:** P13D-FIX1-claude-post-fix-review
**Scope:** Review-only audit of the Codex 5.5 drive/dynamics safety-hardening patch. No code modified.
**Reviewer:** Claude (Opus 4.8)
**Date:** 2026-06-16
**Verdict:** ✅ APPROVE — P13D-FIX1 may be closed; safe to proceed to full-chain/preset interaction audit. Golden-metric drift is inherited from P12D OutputChain, unrelated to this patch.

---

## 1. Patch Surface Reviewed

| File | Change |
|------|--------|
| `Source/Effects/Pedals/Boost/BoostPedal.h` | `preparedBlockSize`/`preparedChannels` capture; `scrubInvalidSamples` + `canProcessBlock` oversized guard before oversampler; realtime fallback counter; **Biquad self-heal** (reset+return 0 on non-finite `y/z1/z2`) |
| `Source/Effects/Pedals/Gate/NoiseGatePedal.h` | `ScopedNoDenormals`; input scrub; Biquad self-heal; `LookaheadDelay` finite-guards in+out; `HybridDetector` reset on non-finite; `detectorDb` fallback (-120); `gateGain` finite fallback |
| `Source/Effects/Pedals/Distortion/DistortionPedal.h` | `ScopedNoDenormals` (first statement) |
| `Source/Effects/Pedals/Fuzz/FuzzPedal.h` | `ScopedNoDenormals` (first statement) |
| `Source/Effects/Pedals/Compressor/CompressorPedal.h` | `ScopedNoDenormals` (first statement) |
| `Source/Core/AudioEngineTests.cpp` | Boost added to P1 prepared/oversized/bypass matrices; new `P13DDriveDynamicsSafetyHardeningTests` (Boost+Gate NaN/Inf recovery, near-silence finite, nominal-audible-bounded) |

Maps cleanly to P13D findings F001 (Boost oversized), F002 (Boost scrub), F003 (Gate NaN), F004 (ScopedNoDenormals ×4), F006 (tests). F005 (Metal) intentionally untouched — confirmed legacy/unregistered.

> Note: the working tree still carries the uncommitted **P13C-FIX1** amp/cab changes (7 files) and its tests; the `AudioEngineTests.cpp` diff therefore also shows the prior-approved P13C blocks. This is expected carry-forward, not part of P13D, and does not affect this review.

---

## 2. Review Questions

### Q1 — Boost oversized guard correctly placed before `oversampler.processSamplesUp`?
**YES.** Order in `BoostPedal::processBlock`: `beginBypassProcess` → `ScopedNoDenormals` (line 199) → `scrubInvalidSamples` (201) → **`if (!canProcessBlock) { fallbackCount++; endBypassProcess; return; }` (203–207)** → param updates → `oversampler.processSamplesUp` (218). `canProcessBlock` checks `numSamples ≤ preparedBlockSize && numChannels ≤ preparedChannels && ≤ kMaxProcessingChannels`, and `preparedBlockSize`/`preparedChannels` are captured in `prepareToPlay` immediately before `initProcessing(preparedBlockSize)`. Guard threshold matches oversampler capacity and is strictly upstream of every upsample call. **Correct.**

### Q2 — Boost fallback realtime-safe, finite, non-allocating, bypass-balanced?
**YES.** Fallback executes only `fallbackBlockCount.fetch_add(relaxed)` + `endBypassProcess(buffer)` + `return`. No allocation, no oversampler call, no lock. Input was scrubbed to finite first, so the dry passthrough is finite. `beginBypassProcess`/`endBypassProcess` are paired on every exit path (early bypass-fail returns before scrub; fallback and normal paths both call `endBypassProcess`). Counter is atomic. **Realtime-safe and balanced.**

### Q3 — Boost NaN/Inf scrub prevents biquad/filter state latch?
**YES — double-layered.** (a) `scrubInvalidSamples` replaces every non-finite input sample with 0 *before* the upsampler and the per-sample biquad chain, so the filters never ingest NaN/Inf. (b) Belt-and-suspenders: the `Biquad::process` now self-heals — if `y/z1/z2` become non-finite for any reason it calls `reset()` and returns 0, so even a state that somehow went bad recovers on the next sample instead of latching. This closes F002 completely.

### Q4 — Noise Gate scrub protects detector, gateGain, lookahead, related state?
**YES — every latch point named in F003 is covered.** Input `scrubInvalidSamples` at block entry; `HybridDetector::process` resets `peakEnvelope`/`rmsSquare` when input or either envelope is non-finite (recovers a previously latched detector once finite input resumes); `detectorDb` falls back to `-120` when `detectorLin` is non-finite (no NaN into threshold comparisons); `gateGain` falls back to `targetGain` (a finite `decibelsToGain`) when non-finite; `LookaheadDelay::process` guards both the stored input and the returned output to finite; sidechain `Biquad` self-heals. The scrub stops new NaN; the per-state resets recover old latched NaN. **Complete.**

### Q5 — ScopedNoDenormals additions correctly placed and harmless?
**YES.** Added as the first statement of `processBlock` in Distortion, Fuzz, Compressor, and Noise Gate (Boost/Overdrive/Neural already had it). FTZ/DAZ scope spans the whole render and unwinds on return (RAII, stack-only). No behavioural change for normal signal — pure denormal-performance guard on decay tails / silence (most valuable for Compressor and Gate, which operate on silence). **Correct and harmless.** Minor stylistic note: in Distortion/Fuzz/Compressor it sits before the `isPrepared/beginBypassProcess` early-return, so it briefly scopes the bypass-skip path too — harmless and consistent with the existing Boost/Overdrive ordering.

### Q6 — Tests meaningful and not overfitted?
**YES.** Boost is now exercised by the generic P1 matrices (prepared sizes, oversized fallback with counter==1 + dry-preserved assertion, bypass continuity). The new `P13DDriveDynamicsSafetyHardeningTests` injects interleaved NaN/+Inf/−Inf across 24 abuse blocks then asserts finite output during abuse, finite during 88/104 recovery blocks, and `recoveryRms > 0.0010` (no silence latch) for both Boost and Gate; near-silence finite for Distortion/Fuzz/Compressor/Gate (180 blocks of ±1e-24); and nominal-audible-bounded (`0.0010 < rms < 1.0`) for Boost+Gate. Assertions are structural invariants (finite / not-latched / audible-bounded), not hard-coded sample values — robust to voicing. Helpers (`computeWindowRms`, `setRangedParamById`, `bufferHasOnlyFiniteSamples`, `computeBufferPeak`) pre-exist and are reused. **Meaningful, not overfitted.**

### Q7 — Avoided tone changes for normal finite in-capacity audio?
**YES.** For finite, in-capacity blocks: the oversized guard is skipped; `scrubInvalidSamples` only rewrites non-finite samples (finite audio untouched); the Biquad self-heal branch only fires on non-finite state (never for normal audio); the Gate detector/gateGain/lookahead fallbacks only fire on non-finite; `ScopedNoDenormals` is value-neutral for audible signal. The new nominal-audible test (`rms` within bounds) and the existing chain/golden DSP tests confirm no material alteration. **No tone change.**

### Q8 — Avoided do-not-touch zones?
**YES.** Diff touches only the 5 listed pedal headers + the test file. No changes to OutputChain/P12D limiter, host PDC/P13A-F001, AudioEngine graph, RuntimeGraphManager, DryWetMixer, RoutingMixer, PluginStateModel schema, UI, amps/cabinets, delay/reverb/modulation, or Neural. Metal left untouched as instructed. **Clean.**

### Q9 — Is the full-gate golden drift caused by P13D-FIX1, or inherited?
**Inherited / out-of-scope — NOT caused by this patch.** The two drifted cases are `output_chain_clean_path` / `impulse_transparency_line_a` and `output_chain_*` — these are **OutputChain impulse/clean-path scenarios with no drive/dynamics pedals in the signal path** (verified in `docs/golden-metrics/p4-offline-qa-baseline.json`). The baseline was generated at commit `da11290` ("stable version base"), **before P12D**. P12D deliberately made the limiter run unconditionally (`limiterDb=0` no longer bypasses, `limiter.process` always runs, soft ceiling emergency-only) — that change alters the clean-path impulse transparency/peak and is the root cause of the golden drift. P13D-FIX1 modifies only Boost/Gate/Distortion/Fuzz/Compressor internals, none of which participate in a clean/empty-chain impulse, so it is structurally impossible for this patch to move those two metrics. The full gate simply exits early on that pre-existing golden failure, which is why downstream stability steps didn't run.

### Q10 — Should the golden metrics be refreshed, investigated, or ignored for this patch?
**For this patch: ignore (it is out of scope and a do-not-touch zone).** Separately: the goldens should be **re-baselined** against current post-P12D OutputChain behaviour — but only after a P12D-owned task confirms the limiter-always-on transparency delta is the intended behaviour (it is, per P12D scope) and re-runs the full stability gate to completion. Refreshing OutputChain goldens is **not** something P13D-FIX1 should do (OutputChain is a do-not-touch zone here). Recommend a follow-up task "P12D-GOLDEN-REFRESH" owning that re-baseline.

### Q11 — Can P13D-FIX1 be closed?
**YES.** All recommended hardening (F001–F004) implemented correctly with strong belt-and-suspenders coverage, validated by build + 8103-pass base suite + 16/16 RT scenarios + RT policy scan, no regressions, no scope creep. The only gate stop is the inherited P12D golden drift, which is outside this patch's scope.

### Q12 — Safe to proceed to full-chain/preset interaction audit?
**YES.** Drive/dynamics layer is now denormal-safe, NaN/Inf-resilient, and capacity-guarded across all active effects, matching the amp/cab parity achieved in P13C-FIX1. No blocker remains.

---

## 3. Findings (observational / non-blocking)

- **F1 (info):** Boost/Gate Biquad self-heal (`reset()` + return 0 on non-finite state) is a strong addition beyond the minimum scrub — fully tone-neutral (only fires on non-finite). Good defense-in-depth.
- **F2 (info):** `ScopedNoDenormals` precedes the `isPrepared/beginBypassProcess` early-return in Distortion/Fuzz/Compressor; harmless and consistent with existing Boost/Overdrive style. No action.
- **F3 (info, out-of-scope):** Golden drift on `output_chain_clean_path` / `impulse_transparency_line_a` is inherited from P12D (baseline at pre-P12D commit `da11290`). Track via a separate P12D golden-refresh task; do not address in P13D.
- **F4 (info):** Working tree still carries uncommitted P13C-FIX1 amp/cab changes; recommend committing P13C-FIX1 and P13D-FIX1 so future diffs/goldens have a clean reference point.
- **F5 (info, deferred):** Compressor/Distortion/Fuzz `scrubInvalidSamples` replaces non-finite with 0 but (unlike Boost/Gate now) their internal biquads lack the self-heal branch. Input scrub makes this sufficient today; consider parity self-heal later as optional hardening. Non-blocking.

No defects.

---

## 4. Validation Cross-Check

Static review confirms the structure behind Codex's reported results:
- Guard placement, fallback safety, scrub coverage, biquad/detector/lookahead self-heal, ScopedNoDenormals scope, and test-helper existence all verified by direct source inspection.
- Golden-drift root cause independently confirmed: drifted cases are OutputChain-only scenarios; baseline commit `da11290` predates P12D.
- Reported runs (relied upon, not re-executed): SharedCode + Standalone build PASS; base validation 281 results / 8103 passes / 0 failures; RT 16/16; audio-thread policy PASS; JSON/null-byte/`git diff --check` PASS. Full gate PARTIAL — stopped at inherited OutputChain golden drift (expected, out of scope).

No contradiction between reported validation and source state.

---

## 5. Verdict

**APPROVE.** Patch is correct, safe, in-scope, well-tested, and tone-neutral. **P13D-FIX1 can be closed.** The full-gate golden drift is **inherited from P12D OutputChain**, not introduced here. **Proceed to the full-chain/preset interaction audit**, and open a separate P12D-owned golden re-baseline task.
