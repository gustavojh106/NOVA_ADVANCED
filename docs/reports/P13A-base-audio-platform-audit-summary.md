# P13A — Base Audio Platform Audit — Summary

**Date**: 2026-06-10 · **Type**: read-only audit (no code modified) · Full report: `P13A-base-audio-platform-audit-report.md`

## Verdict

**Solid beta. Release-candidate for Standalone; not yet for DAW/VST3 use.** The P12D OutputChain transparent-safety fix is verified structurally sound and properly regression-tested. The architecture (two-plane control/audio split, immutable graph swapping, atomic parameter snapshots) is production-grade. One true base blocker remains before DAW deployment: the plugin never reports its latency to the host.

## P12D verification — all claims confirmed

- `limiterDb=0` no longer bypasses the limiter anywhere; threshold is capped at 0.97 linear (≈−0.265 dB) and `limiter.process` runs unconditionally (`OutputChain.cpp:293`).
- Latency is reported consistently (always the lookahead delay) — but only *inside* the graph; the host never sees it.
- `PeakLimiter::Result::active` now means actual gain reduction.
- Soft ceiling is emergency-only (limiter ceiling 0.97 < ceiling knee 0.985; tests assert zero soft-ceiling work under +10.73 dB master).
- No remaining `outputVolumeDb + limiterDb` path recreates the 0.999 pinning bug within OutputChain. The P12D tests reproduce the exact field failure end-to-end and are not overfitted.

## Top findings

| ID | Finding | Sev/Prob |
|---|---|---|
| F001 | **Host PDC never reported** — `NOVAAudioProcessor` never calls `setLatencySamples`; wet path is ≥2 ms late (limiter lookahead + pedal latency) in every DAW session. Small, isolated fix. | High/High |
| F012 | **Param-range dead zones** — input gain knob spans −60..+24 dB but DSP clamps at −36; output volume −60..+12 vs −36. Bottom 24 dB of both knobs does nothing; output volume can never approach silence. | Medium/High |
| F003 | **Global mix < 100% defeats output safety** — dry/wet mixing happens after the limiter/soft ceiling; the mixed output is only protected by the 24.0 hard clamp. | Medium/Medium |
| F004 | **Line gain ≤0.001 remaps to unity** — setting Level A/B to 0 yields full volume (deliberate policy, pinned by tests, objectively surprising). | Medium/Medium |
| F002 | **Auto-heal loses pedal settings** — corruption-triggered graph rebuild recreates pedals with default parameters; only session-layer rebuilds replay saved state. | Medium (High consequence / Low prob) |

Lesser: in-place bypass `graph->rebuild()` contends the audio thread's graph lock (F005); `prepare()` callable from UI while audio runs, safe only by ordering (F006); auto-routing collapses asymmetric stereo with no crossfade (F007); engine on/off toggle time-shifts audio (F008); rare audio-thread lock/alloc/log on engine-param change (F009); dead safety code (F011).

## Scores

| Area | Score |
|---|---|
| Base signal architecture | 8.5 |
| Gain staging safety | 7.0 |
| Output safety after P12D | 8.5 |
| Latency/PDC safety | **4.0** |
| RT safety | 8.0 |
| Lifecycle/sample-rate safety | 7.5 |
| Bypass/dry-wet correctness | 8.0 |
| Diagnostics/telemetry usefulness | 8.5 |
| Test credibility | 8.0 |
| Base audio release readiness | 7.0 |

## Tests

267 results / 7,526 passes / 0 failures confirmed (`artifacts/p12d-tests/test-report.txt`). Coverage is behavioral and credible. Gaps: host PDC untested, no concurrency coverage, no auto-heal recovery test, true oversized-block fallback (>8192) never exercised end-to-end, no param-range/sanitizer agreement test.

## Recommendation

1. Fix **F001** (report engine latency to host) — one-file change + test; this is the only base blocker.
2. Make explicit policy decisions on F003 (mix placement vs final clamp), F004 (0 = mute vs unity), F012 (align ranges).
3. **Proceed to effect-by-effect audits now** — base contract is stable. Start with stateful/time-based pedals (Delay, Reverb, Neural) since they stress the latency and rebuild machinery most.
