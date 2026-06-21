# P13C-FIX1 — Claude Post-Fix Review

**Task ID:** P13C-FIX1-claude-post-fix-review
**Scope:** Review-only audit of the Codex 5.5 amp/cabinet safety-hardening patch. No code modified.
**Reviewer:** Claude (Opus 4.8)
**Date:** 2026-06-16
**Verdict:** ✅ APPROVE — P13C-FIX1 may be closed; safe to proceed to drive/dynamics audit.

---

## 1. Patch Surface Reviewed

| File | Change |
|------|--------|
| `Source/Effects/Amplifiers/ClassicAmp.h` | preparedBlockSize capture; oversized guard; input/output scrub; sag-env NaN reset; ScopedNoDenormals |
| `Source/Effects/Amplifiers/HighGainAmp.h` | Same as above + inputNoiseReject (gate) env NaN reset/sanitize |
| `Source/Effects/Amplifiers/BoutiqueAmp.h` | Same pattern; touch/envelope NaN reset |
| `Source/Effects/Amplifiers/ChimeAmp.h` | Same pattern; cathodeBias + sagEnv NaN reset |
| `Source/Effects/Cabinets/CabinetPedal.h` | ScopedNoDenormals only |
| `Source/Effects/Cabinets/Modern4x12Cabinet.h` | ScopedNoDenormals only |
| `Source/Effects/Cabinets/Vintage2x12Cabinet.h` | ScopedNoDenormals only |
| `Source/Core/AudioEngineTests.cpp` | +161 lines: amp oversized + NaN/Inf recovery tests; cabinet near-silence/tail finite tests |

Diff totals: 8 files, +336 / −23.

---

## 2. Review Questions

### Q1 — Oversized-block guards correctly placed before JUCE Oversampling in all four amps?
**YES.** In each amp `processBlock`, ordering is:
`ScopedNoDenormals` → `beginBypassProcess` → `scrubInvalidSamples(buffer)` → **`if (numSamples > preparedBlockSize) { endBypassProcess; return; }`** → voicing/param update → `oversampler.processSamplesUp`.

Verified on ClassicAmp: guard at line 125, `processSamplesUp` at line 137. Identical structure in HighGain, Boutique, Chime. `preparedBlockSize` is captured in `prepareToPlay` as `jmax(1, samplesPerBlock)` immediately before `oversampler.initProcessing`, so the guard threshold always matches the capacity the oversampler was initialized for. The guard is strictly upstream of every oversampler entry point. **Correct.**

### Q2 — Fallback behavior safe, finite, non-allocating?
**YES.** Fallback path does only `endBypassProcess(buffer)` then `return` — no oversampler call, no buffer resize, no allocation. Input was already scrubbed to finite, so the dry passthrough is finite. Bypass-ramp bookkeeping stays balanced (`begin`/`end` paired on every exit). Non-allocating and real-time safe.

### Q3 — Avoids tone changes for normal finite in-capacity audio?
**YES.** For finite samples with `|x| ≥ 1e-30` (≈ −600 dBFS, far below any audible or float-meaningful level), `sanitizeAudioSample` returns the value unchanged. In-capacity blocks bypass the oversized branch entirely. The only added per-sample math is the finite/denormal predicate, which is value-preserving for real audio. Test `p13c_amp_oversized_blocks_stay_safe` asserts null-RMS ≤ 1e-7 against dry input in the fallback case, confirming no material alteration. **No tone change.**

### Q4 — NaN/Inf scrubs sufficient before filters, envelopes, gates, oversamplers, waveshapers?
**YES.** Two layers: (a) whole-buffer `scrubInvalidSamples` at block entry, before any DSP; (b) per-sample `sanitizeAudioSample(data[...])` at the top of the inner stage loop. Envelope/sag/bias/gate states each get an `isfinite` reset guard before update and (where they feed forward) a post-update `sanitizeAudioSample`. Outputs are sanitized before write-back. This covers the chain ahead of filters, oversampler, and waveshaper stages. **Sufficient.**

### Q5 — HighGainAmp gate/envelope protected from NaN latch and persistent silence?
**YES.** The "gate" is `computeInputNoiseRejectGain`'s `inputNoiseEnvelope`. It is now: input sanitized → `if (!isfinite(env)) env = 0` → updated → `env = sanitizeAudioSample(env)`. Because a latched NaN/huge env would drive reject-gain → 0 (permanent silence), the finite-reset breaks the latch and lets the env decay back. `sagEnvelope` gets the same treatment. Both arrays are also zeroed in `prepareToPlay`. Test `p13c_amp_nan_inf_recovery` asserts post-abuse `rms > 0.0010` specifically for High Gain Amp — guards against persistent silence. **Protected.**

### Q6 — ScopedNoDenormals additions correct and harmless?
**YES.** Placed as the first statement of each `processBlock` (4 amps + 3 cabinets), so the FTZ/DAZ scope covers the whole render and unwinds on return. RAII, stack-only, no behavior change for normal signal — purely a denormal-performance guard. **Correct and harmless.**

### Q7 — Cabinet near-silence/tail protections adequate?
**YES.** Cabinets already had `scrubInvalidSamples` + a `canProcessBlock` oversized/capacity guard pre-patch; the patch adds only `ScopedNoDenormals`, which is exactly what was missing for sub-denormal IR/biquad tails. New test `p13c_cabinets_near_silence_tail_remains_finite` drives a 1e-20 impulse + 180 silent blocks and asserts finite output with peak < 1e-10 across Cabinet / Vintage 2x12 / Modern 4x12. **Adequate.**

### Q8 — Tests meaningful and not overfitted?
**YES, with one note.** Tests exercise real failure modes: oversized blocks (4× block), interleaved NaN/+Inf/−Inf injection across 24 abuse blocks then 88 recovery blocks, and cabinet denormal tails. They assert structural invariants (finite, non-silence-latch, near-silent on near-silent input) rather than hard-coded sample values, so they are not brittle to voicing. Thresholds (`rms > 0.00045`, HighGain `> 0.0010`, peak `< 1e-10`, null-RMS `≤ 1e-7`) are loose invariants, not curve-fits. Helpers (`bufferHasOnlyFiniteSamples`, `computeBufferNullRms`, `computeBufferPeak`, `P11AAmpRenderStats`, `configureUsableAmpVoice/CabinetVoice`) all pre-exist and are reused. **Meaningful, not overfitted.**

### Q9 — Avoided do-not-touch zones?
**YES.** Diff touches only the 4 amp headers, 3 cabinet headers, and the test file. No changes to OutputChain/P12D limiter, host PDC/P13A-F001, AudioEngine graph, RuntimeGraphManager, DryWetMixer, RoutingMixer, PluginStateModel schema, UI, drive/dynamics/delay/reverb/modulation pedals, or SyntheticIR. **Clean.**

### Q10 — Can P13C-FIX1 be closed?
**YES.** All recommended hardening implemented correctly, validated by build + 7593-pass audio suite, no regressions, no scope creep.

### Q11 — Safe to proceed to drive/dynamics audit?
**YES.** Amp/cab layer is now denormal-safe, NaN/Inf-resilient, and capacity-guarded. No blocker remains upstream of the drive/dynamics stage.

---

## 3. Findings

All findings are observational / non-blocking. No defects.

- **F1 (info):** Double scrub — whole-buffer `scrubInvalidSamples` plus per-sample input `sanitizeAudioSample`. Redundant but value-preserving; negligible CPU. Acceptable defense-in-depth.
- **F2 (info):** `sanitizeAudioSample` flushes `|x| < 1e-30` on both input and output. Below denormal-relevance and inaudible; no tone impact.
- **F3 (info):** Cabinets correctly rely on the pre-existing `canProcessBlock` oversized guard; the patch adds only the missing `ScopedNoDenormals`. No new cabinet capacity guard needed.
- **F4 (deferred, non-blocking):** Clean Amp was not hardened (out of scope, unmodified) yet is included in the oversized finite test; the null-RMS assertion is correctly skipped for it. A future parity pass could extend the same guards to Clean Amp for consistency.
- **F5 (info):** Each amp adds a full O(n) buffer scrub pass on the hot path. Validation shows 0 failures and the cost is one linear pass per block — acceptable for the safety gain.

---

## 4. Validation Cross-Check

Static review confirms the structure behind Codex's reported results:
- Guard placement, fallback safety, scrub coverage, envelope NaN resets, ScopedNoDenormals scope, and test helper existence all verified by direct source inspection.
- Reported runs (relied upon, not re-executed): SharedCode + Standalone build PASS 0/0; `run-base-audio-validation.ps1` results=276, passes=7593, failures=0; RT-policy scan PASS; null-byte scan PASS; `git diff --check` only CRLF.

No contradiction found between reported validation and source state.

---

## 5. Verdict

**APPROVE.** Patch is correct, safe, in-scope, and well-tested. **P13C-FIX1 can be closed.** **Proceed to the drive/dynamics audit.**
