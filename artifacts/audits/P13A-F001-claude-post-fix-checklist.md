# P13A-F001 — Claude Post-Fix Review Checklist

Derived from `docs/reports/P13A-F001-claude-post-fix-review.md`. Review verdict: **approve with concerns — F001 stays open until blockers clear.**

## Blockers (before closing P13A-F001)

- [ ] **R001 — Restore the five null-corrupted files**:
  - `Source/Core/PluginProcessor.h` (6,304 zero bytes — the header half of the fix)
  - `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-report.md`
  - `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-summary.md`
  - `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-findings.json`
  - `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-checklist.md`
  - Recovery order: OneDrive web version history → Visual Studio local history/backup → Codex session re-emit.
- [ ] **Byte-verify restored files** (no all-null content; header parses as C++).
- [ ] **Clean rebuild** (SharedCode + Standalone) — must pass with the restored header.
- [ ] **Re-run base validation** — expect 268 results / 7,543 passes / 0 failures.
- [ ] **Commit immediately** so git holds the fix (corruption struck uncommitted work).

## Before any release tag (not blocking P13B start)

- [ ] **R006 — Release VST3 build** (never run for this change).
- [ ] **R006/R002 — Manual DAW PDC check in Reaper**: empty board reports ~96 samples @48 kHz; latency updates on pedal add; PDC value matches `getLatencyNumSamples()`.
- [ ] **R002 — Bypass-toggle listening test during playback**: evaluate host reaction to `restartComponent(kLatencyChanged)` on footswitch-style bypass. If audible glitching, switch host-facing policy to bypass-invariant (stable per-topology max) while keeping live latency internally.

## Later improvements

- [ ] **R003 — Document the LatencyListener destruction-order contract** (listener cleared → updates cancelled → engine member destruction stops control thread), or route notifications under existing control-plane sync.
- [ ] **R004 — Optional async-path test**: pump the message loop after a control-thread graph publish; assert host latency converges.
- [ ] **R005 — Retire/re-scope stale phase guards** (P9F–P11C) in `scripts/check-audio-thread-policy.ps1` after committing, so the policy scan returns to a meaningful PASS/FAIL.
- [ ] **R007 — Move the repo out of OneDrive sync** (or exclude the folder); add a null-byte corruption scan to validation scripts.

## Verified — no action needed

- [x] `setLatencySamples` never callable from the audio thread (all paths: prepare / message thread / AsyncUpdater).
- [x] All five trigger points wired: prepare, graph publish, bypass in-place rebuild, pedal add/remove, preset/session restore.
- [x] Constructor-time notifications correctly gated; recovered synchronously at `prepareToPlay`.
- [x] Coalesced + value-deduped async reporting; cannot miss the final value; no host notification loops.
- [x] Regression test proves processor latency > 0 after prepare, equals engine graph latency, and is stable across limiterDb 0/−6/−12 (`AudioEngineTests.cpp:4514-4552`).
- [x] RT policy scan FAIL is stale phase-gate noise; "No FAIL findings in active audio routes".
- [x] Do-not-touch list respected (RuntimeGraphManager, graph swap, DryWetMixer, RoutingMixer, PluginStateModel, pedal processBlocks, OutputChain/P12D limiter logic unchanged in behavior).

## Recommended next step

Restore + rebuild + commit (blockers above), close P13A-F001, then start **P13B effect-by-effect audits** (Delay, Reverb, Neural first). Release VST3 + Reaper PDC validation before any release tag.
