# P13D — Drive & Dynamics Audit Checklist

Auditor: Claude (Opus 4.8) · Date: 2026-06-16 · Mode: audit-only · Code modified: **No**

## Inventory
- [x] All active drive effects found: Boost, Overdrive, Distortion, Fuzz, Neural
- [x] All active dynamics found: Compressor, Noise Gate
- [x] Dead/legacy identified: MetalDistortionPedal (unregistered; catalog maps to Distortion)
- [x] Per-effect params, processBlock path, prepare/reset, oversampling, latency, state documented

## Drive DSP safety (Boost/Overdrive/Distortion/Fuzz/Neural)
- [x] Input level expectations + internal gain mapped
- [x] Saturation stages intrinsically bounded (tanh/atan/exp)
- [x] Aliasing controlled — 8x oversampling on all drives
- [x] Output containment before OutputChain verified (soft ceilings 0.92–1.0)
- [x] Hot-input behaviour assessed (input/wet trims; Boost gap noted F002)
- [x] Into HighGainAmp/cabinets — compat trims + passing chain tests
- [x] No chronic P12D limiter 0.999 pinning (nominal output <= ~0.94)
- [x] Oversized-block handling — **Boost gap (F001)**

## Dynamics DSP safety (Compressor/Noise Gate)
- [x] Detector/envelope stability (jmax floors, no log(0)/sqrt(neg))
- [x] Attack/release coefficients SR-correct
- [x] NaN/Inf protection — Compressor scrubbed; **Gate gap (F003)**
- [x] Denormal protection — **gap (F004)** both
- [x] Threshold/range/ratio/knee sanitised
- [x] Stuck-open/closed risk — Gate hysteresis+hold OK; NaN latch noted F003
- [x] Pumping/click — focus HP + equal-power blend + smoothstep OK
- [x] Silence/near-silence behaviour assessed
- [x] Recovery after invalid input — Compressor OK; Gate gap F003

## RT safety
- [x] No allocations in steady-state processBlock (active effects)
- [x] No locks / logging / string work on audio path
- [x] Parameter reads RT-safe (raw ptr + SmoothedValue)
- [x] Denormal coverage mapped (4 effects missing — F004)
- [x] Invalid-sample/state latch mapped (Boost F002, Gate F003)

## Latency / PDC
- [x] Each effect reports correct latency (oversampler / 0 / lookahead)
- [x] Latency constant under parameter automation
- [x] Bypass latency consistency via ProcessorBase
- [x] No runtime latency mutation -> P13A-F001 listener coverage intact

## Tests
- [x] Existing coverage inventoried (round-trip, mix-zero, automation-stress, chains, dynamics)
- [x] Required chain tests confirmed present (boost/distortion/fuzz x amp x cab)
- [x] Oversized fallback coverage checked (Boost missing — F006/G1)
- [x] Missing tests identified (G1–G4)

## Manual listening matrix
- [x] 13-row practical listening matrix produced

## Scorecard
- [x] All 7 active effects scored across 9 dimensions

## Risk table & verdict
- [x] 6 findings (F001–F006) with severity/probability/evidence/files/fix/implementer
- [x] Direct verdict: safe to keep enabled; strongest=Overdrive; riskiest=Boost
- [x] No hard blocker before full-chain/preset audit
- [x] Codex 5.5 priority order: F001+F002 (Boost) -> F003 (Gate) -> F004 (denormals) -> F005/F006

## Scope compliance
- [x] No source/tests/presets modified
- [x] Only docs/reports + artifacts/audits files created
- [x] Do-not-touch zones (OutputChain/PDC/graph/mixers/schema/UI/time-based) untouched; amps/cabs referenced only for interaction
