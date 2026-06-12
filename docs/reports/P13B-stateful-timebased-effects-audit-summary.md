# P13B — Stateful / Time-Based Effects Audit — Summary

**Date:** 2026-06-12 · **Auditor:** Claude Fable (read-only) · **Full report:** `P13B-stateful-timebased-effects-audit-report.md`

## Verdict

Delay, Reverb, and Neural are **safe to keep enabled**. Chorus, Flanger, and Octave (secondary stateful effects) are also release-viable. **No effect needs to be hidden or marked beta-only, and there is no blocker before the amps/cabinets audit.** All feedback loops are saturation-bounded, latency reporting is correct everywhere (Neural reports oversampler latency; Delay/Reverb correctly report zero — their delay time is creative, not PDC), audio paths are allocation- and lock-free, and the P13A-F001 host PDC pipeline correctly tracks add/remove/bypass latency changes (unit-tested; manual DAW null test still pending from P13A).

## Scorecard (1–10)

| Effect | DSP | PDC | Tails | RT | Params | Tests | Release |
|---|---|---|---|---|---|---|---|
| Delay | 9 | 10 | 8 | 8 | 9 | 9 | **9** |
| Reverb | 9 | 10 | 8 | 8 | 9 | 10 | **9** |
| Neural | 8 | 9 | 9 | 7 | 9 | 7 | **8** |
| Chorus | 7 | 10 | 9 | 8 | 9 | 8 | **8** |
| Flanger | 8 | 10 | 9 | 8 | 9 | 9 | **9** |
| Octave | 8 | 10 | 9 | 8 | 9 | 8 | **8** |

## Top findings (none Critical/High)

1. **F-P13B-001 (Medium):** Delay, Chorus, Flanger lack `ScopedNoDenormals` in their `processBlock` DSP scope (base-class guard doesn't cover the pedal loop). Decaying feedback tails risk denormal CPU spikes on hosts without FTZ. Three one-line fixes.
2. **F-P13B-002/003 (Medium/Low):** Chorus and Octave have no NaN/Inf input scrub — a NaN latches permanently in filter/feedback state until engine auto-heal fires (audible dropout). Copy Flanger's scrub helper.
3. **F-P13B-007 (Low docs / Med product):** "Neural" pedal contains **no RTNeural inference** — it's an analytic waveshaper at 8× oversampling. CLAUDE.md's claim is wrong; vendored RTNeural/models are unused by it.
4. **F-P13B-005 (UX decision):** Bypass intentionally truncates delay/reverb tails (state reset on un-bypass, commented in `ProcessorBase`). Correct and safe, but undocumented for users; no "trails" mode exists.
5. **F-P13B-006 (Low, constant cost):** CPU hotspots — Neural recomputes 7 exp-based filter cutoffs per base-rate sample; Reverb runs 2 exp/sample on output tone; Delay rebuilds feedback biquads every 8 samples. Bounded and correct; profile before optimizing.

## Recommended next steps (Codex 5.5)

1. Add `ScopedNoDenormals` to Delay/Chorus/Flanger `processBlock`.
2. Add NaN/Inf scrub to Chorus + Octave, with matching abuse tests.
3. Delete or align the unused `Reverb::Engine::processSample` (diverges from `processBlock` safety clamps).
4. Fix the RTNeural claim in CLAUDE.md.
5. Carry-over from P13A: Release VST3 build + manual DAW PDC null test (listening item L10).

No code was modified during this audit.
