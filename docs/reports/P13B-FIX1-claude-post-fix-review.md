# P13B-FIX1 — Claude Post-Fix Review

- **Task ID:** P13B-FIX1-claude-post-fix-review
- **Date:** 2026-06-12
- **Reviewer:** Claude Fable (read-only; no code modified)
- **Patch under review:** Codex 5.5 stateful/time-based effect safety patch (uncommitted working tree on `master`)
- **Findings addressed:** F-P13B-001 (denormal guards), F-P13B-002 (Chorus NaN scrub), F-P13B-003 (Octave NaN scrub), F-P13B-007 (CLAUDE.md RTNeural correction)

## Verdict: **APPROVE**

The patch does exactly what the P13B audit prescribed, stays inside the allowed file set, is bit-exact for finite audio, adds no realtime hazards, and is backed by five new tests (273 results, 7553 passes, 0 failures). Safe to commit. Minor follow-ups noted below are non-blocking and mostly pre-existing.

---

## 1. ScopedNoDenormals placement (Q1) — ✅ Correct and harmless

Diff verified in all four pedals:

- `DelayPedal.h` and `FlangerPedal.h`: `juce::ScopedNoDenormals noDenormals;` added as the first statement of `processBlock`, before the `isPrepared || beginBypassProcess` gate. This covers the entire DSP scope including `endBypassProcess` — strictly wider than required by `ProcessorBase.h:200`'s contract.
- `ChorusPedal.h`: same placement.
- `OctavePedal.h`: the guard previously sat *after* `beginBypassProcess`; the patch moves it to the top. Equivalent or better.

Nesting note: `beginBypassProcess`/`endBypassProcess` create their own scoped guards inside an already-FTZ scope. Re-setting MXCSR/FPCR is a few cycles and fully harmless. RAII restore order is correct (inner restores to FTZ-on, outer restores caller state on exit).

Denormal flush alters only sub-1e-38 values — inaudible by definition and standard industry practice; this is the entire point of the fix.

## 2. Chorus NaN/Inf hardening (Q2) — ✅ Prevents latch, transparent for finite audio

Layers added:

1. **Input scrub** — `scrubInvalidSamples` after the bypass gate, non-finite→0, identity otherwise. Matches the Delay/Flanger pattern exactly.
2. **`ChorusDSP::Biquad::process`** — sanitizes input, resets `z1/z2` if non-finite on entry, bails with reset if `y` goes non-finite, re-checks state after update. NaN can no longer persist across samples.
3. **`ChorusDSP::DelayLine`** — `write()` sanitizes the stored sample; `readCubic()` sanitizes all four taps. The 45 ms line can no longer launder a NaN back into the loop.
4. **Feedback state** — `previousLocal/CrossFeedback` reads sanitized; `wetState`/`feedbackState` writes sanitized; final buffer write sanitized.

Transparency argument: `sanitizeAudioSample(x) = std::isfinite(x) ? x : 0.0f` is the identity for every finite value (including denormals and large values — no clamping). All other additions are read-only finiteness branches. **Normal finite audio is bit-identical**, confirmed empirically by the new test *"ChorusPedal dry finite input is unchanged by safety scrub"* (null RMS ≤ 1e-6 at mix = 0).

Cost: ~20–30 extra `isfinite` checks per sample (6 biquads + up to 6 line reads/writes). `isfinite` on float is a cheap integer-domain test; negligible against the existing per-sample LFO/trig work. No tonal or CPU concern.

## 3. Octave NaN/Inf hardening (Q3) — ✅ Prevents latch, transparent for finite audio

1. **Input scrub** added after the bypass gate.
2. **`OctaveDSP::Biquad::process`** hardened identically to Chorus.
3. **`EnvelopeFollower::process`** — sanitizes input, resets non-finite state, sanitizes output.
4. **`PeriodTracker`** — entry check resets the tracker if any of `trackedPeriod / smoothedFreq / confidence / bestCorrelation / decimateAccumulator` went non-finite; `history` writes and reads (`pushDecimatedSample`, `historyAt`) sanitized; `smoothedFreq` sanitized post-update.

All ~14 biquads, both envelopes, and the 768-sample analysis history are now latch-proof. Same identity-for-finite property — no behavior change on clean audio. The `historyAt` sanitize adds one check per correlation access inside `analysePitch`; that loop runs once per 32 input samples and the check is trivial — bounded, acceptable.

Cosmetic nit: `sanitizeAudioSample(smoothedFreq)` applies an "audio sample" helper to a frequency value. Semantically it is just a finiteness guard — harmless, slightly misnamed (OBS-3).

## 4. Added tests (Q4) — ✅ Meaningful, not overfitted

Five tests, matching the +5 result count (268→273):

| Test | Value |
|---|---|
| *OctavePedal sanitizes NaN/Inf input and recovers to finite output* | **Would fail pre-patch** (NaN latched in biquads/tracker → recovery render stays NaN). Two-phase design (dirty render, then clean render) directly proves the no-latch property — exactly the F-P13B-003 failure mode. Generic finite assertions, no magic numbers tied to implementation. |
| *ChorusPedal sanitizes NaN/Inf input and recovers to finite output* | Same two-phase structure for F-P13B-002; **would fail pre-patch**. |
| *ChorusPedal dry finite input is unchanged by safety scrub* | Regression guard for transparency (review Q2). Null-RMS against input at mix = 0. Directly prevents future "sanitizer that clamps audio" regressions. |
| *DelayPedal near-silence feedback tail remains finite* | Weakest of the five: a 1e-20 impulse with feedback < 1 mathematically cannot exceed 1e-12, so it likely passes pre-patch too. It does not measure denormal CPU cost (not feasible in a unit test). Still useful as a guard against future silence-region gain/NaN bugs. Not overfitted — just low discrimination. (OBS-1) |
| *FlangerPedal near-silence feedback tail remains finite* | Same as above. |

NaN injection uses modulo patterns (every 11th/13th/17th/19th/23rd/31st sample mixed with clean sine) — realistic corruption shape, no overfitting to the scrub implementation.

## 5. Tone preservation (Q5) — ✅

No coefficient, gain, filter cutoff, mix law, feedback amount, or saturation curve was touched. The only value-altering operations are non-finite→0 (previously garbage) and FTZ (sub-audible). The Chorus dry-transparency test pins this empirically. Tone intentionally unchanged: confirmed.

## 6. RT-safety of the patch itself (Q6) — ✅

- No allocation: helpers are `inline float` / `static void` over existing buffers.
- No locks, no logging, no `juce::String`, no dynamic resize.
- All loops bounded by `getNumChannels()/getNumSamples()`.
- Codex's RT/audio-thread policy scan reported 0 failures/warnings, consistent with this reading.

## 7. CLAUDE.md correction (Q7) — ✅ Accurate

- Pedal description now reads "analytic/model-inspired waveshaping pedal rather than an RTNeural inference path" — matches the P13B verification (`grep RTNeural Source/Effects` → empty).
- Library section honestly marks RTNeural/Eigen/json/xsimd/models as vendored-but-unused by the current Neural pedal.
- Matches F-P13B-007 exactly. No overcorrection (libraries are still documented as present).

## 8. Anything required before commit? (Q8) — Nothing blocking

- **OBS-2 (pre-existing, not a regression):** in every pedal (Delay/Flanger/Neural and now Chorus/Octave), the input scrub runs *after* `beginBypassProcess`, which copies the still-unsanitized input into the dry buffer. During a bypass crossfade, NaN input can therefore reach the output via the dry leg until the engine-level health monitor scrubs it (and auto-heal fires after 2 corrupt blocks). FIX1 follows the established codebase pattern; changing it would touch `ProcessorBase` ordering semantics — out of FIX1 scope. Candidate for a future hardening pass.
- **OBS-4:** `audio-base-test-report.txt` (generated artifact) is in the diff with the new counts — fine to commit together, just noting it's machine-generated.
- **OBS-5:** F-P13B-004 (unused, divergent `Reverb::Engine::processSample`) from the P13B recommendation list was *not* included in FIX1. Acceptable scope cut; carry as a follow-up item.
- Asymmetry note: Chorus/Octave received deep state hardening while Delay/Flanger only have input scrubs + already-bounded loops. This asymmetry is justified (Delay/Flanger loops were already tanh/clip-bounded with DC blockers and passed abuse tests) — no action needed.

## 9. Can P13B-FIX1 be closed? (Q9) — **Yes**

All four targeted findings are correctly implemented, validated (build clean, 273/7553/0, RT scan clean), and scope-respecting (no do-not-touch zones entered; only the six declared files plus the generated test report changed).

## 10. Proceed to amps/cabinets audit? (Q10) — **Yes**

P13B declared no blockers; FIX1 removes the only Medium findings. Nothing in this patch touches amp/cabinet code paths. Proceed.

---

## Observations register

| ID | Severity | Description | Action |
|---|---|---|---|
| OBS-1 | Low | Near-silence Delay/Flanger tests have low discriminating power (would pass pre-patch); they don't measure denormal CPU | Keep as guards; optional future profiling harness |
| OBS-2 | Low (pre-existing) | Dry buffer captured before input scrub in all pedals → NaN can transit dry leg during bypass crossfade; engine sanitizer/auto-heal backstops | Future hardening pass; not FIX1 scope |
| OBS-3 | Cosmetic | `sanitizeAudioSample` applied to non-audio quantities (`smoothedFreq`) in Octave | None required |
| OBS-4 | Info | Generated `audio-base-test-report.txt` included in diff | Commit as-is |
| OBS-5 | Low | F-P13B-004 (Reverb dead `processSample` path) not included in FIX1 | Track as follow-up before/with amps phase |

*Review was strictly read-only. No source, test, preset, or project files were modified.*
