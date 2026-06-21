# P13C — Amps & Cabinets Audit — Summary

**Date:** 2026-06-12 · **Auditor:** Claude Fable (read-only) · **Full report:** `P13C-amps-cabinets-audit-report.md`

## Verdict

All five amps (Clean, Classic, High Gain, Boutique, Chime) and all three cabinets (Atlas 4x12, Modern 4x12, Vintage 2x12) are **safe to keep enabled**, and there is **no blocker before the drive/dynamics audit**. Every saturation stage is tanh-bounded, outputs are backstopped by the always-on P12D limiter, cabinet IRs are stable synthetic convolutions with zero latency, all amps report oversampler latency through the P13A-F001 host PDC pipeline, and the high-gain chain is the best-tested path in the product. The amp family does, however, lag the P13B-FIX1 hardening standard: four of five amps are missing the oversize-block guard, NaN scrub, and denormal guard that CleanAmp, the cabinets, and the stateful FX already have.

- **Strongest:** CleanAmp (reference-grade safety implementation) and Cabinet/Atlas 4x12.
- **Riskiest:** BoutiqueAmp (least tested, discontinuous waveshaper at edge-of-breakup, all guards missing). HighGainAmp has the worst single failure mode (NaN latches its gate envelope → persistent silence) but the best test coverage.

## Scorecard (release readiness)

| Model | DSP | Gain | Tone | HG-stab | Cab-stab | PDC | RT | Params | Tests | **Release** |
|---|---|---|---|---|---|---|---|---|---|---|
| Clean Amp | 9 | 9 | 8 | 9 | — | 9 | 9 | 9 | 8 | **9** |
| Classic Amp | 6 | 7 | 8 | 8 | — | 9 | 8 | 8 | 8 | **7** |
| High Gain Amp | 6 | 8 | 8 | 9 | — | 9 | 8 | 8 | 9 | **8** |
| Boutique Amp | 6 | 7 | 7 | 7 | — | 9 | 8 | 8 | 6 | **7** |
| Chime Amp | 6 | 8 | 8 | 8 | — | 9 | 8 | 8 | 6 | **7** |
| Cabinet (Atlas) | 8 | 9 | 8 | — | 9 | 10 | 9 | 9 | 9 | **9** |
| Modern 4x12 | 7 | 8 | 8 | — | 9 | 10 | 9 | 9 | 9 | **9** |
| Vintage 2x12 | 7 | 7 | 8 | — | 8 | 10 | 9 | 9 | 7 | **8** |

## Top findings (no Critical)

1. **F-001 (Med-High/Low):** Classic/HighGain/Boutique/Chime have **no oversize-block guard** before `oversampler.processSamplesUp` — a host block larger than prepared overruns JUCE Oversampling's preallocated buffers (heap corruption in release). CleanAmp, Neural, and all cabinets guard this; the P1 oversize test covers cabinets but no amps.
2. **F-002 (Med/Low-Med):** Same four amps lack NaN/Inf input scrub. Worst case is HighGainAmp: NaN latches the noise-gate envelope, whose NaN gain then zeroes the signal — **persistent silence** until reset/auto-heal.
3. **F-003 (Med/Med):** Same four amps **plus all three cabinets** lack `ScopedNoDenormals` — IIR/convolution/envelope tails risk denormal CPU spikes (exact P13B F-001 pattern; 7 one-line fixes).
4. **F-004 (Low-Med/Certain):** Envelope time constants are fixed per-sample (sag `AMP_SAG_DECAY`, Chime cathode bias, Boutique touch envelopes — whose comments claim 4× while code runs 8×), and SyntheticIR's decay rate is per-sample — feel and IR tail vary with host sample rate. Tone consistency, not safety.
5. **F-007 (Low/quality):** BoutiqueAmp's piecewise stage1 is **discontinuous** (~6% step) at `cleanThreshold` — its signature edge-of-breakup zone can crackle. Also F-006: output-safety inconsistency (Vintage2x12 has no trim/containment; Classic/Boutique masters uncontained at level 2).

## Recommended next (Codex 5.5, P13C-FIX1)

1. Oversize-block dry fallback in 4 amps + extend P1 oversize test to all amps (F-001).
2. NaN scrub + envelope finite guards in 4 amps + latch/recovery tests (F-002).
3. `ScopedNoDenormals` ×7 (F-003).
4. Latency rounding + oversampling comment fixes in the same pass (F-005/F-008).
5. Defer F-004/F-006/F-007 (tone-affecting) until the listening matrix confirms by ear.

No code was modified during this audit.
