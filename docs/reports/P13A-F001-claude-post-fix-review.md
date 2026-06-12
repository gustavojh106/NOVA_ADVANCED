# P13A-F001 — Claude Post-Fix Review: Host PDC Latency Reporting

- **Task**: P13A-F001-claude-post-fix-review
- **Date**: 2026-06-10
- **Type**: Read-only review. **No source code, tests, or presets were modified by this review.**
- **Subject**: Codex 5.5 fix for P13A-F001 (host never told plugin latency).

---

## Verdict: **APPROVE WITH CONCERNS — cannot close P13A-F001 yet**

The fix design is correct, thread-safe, and well tested. However, the working tree is **damaged**: `Source/Core/PluginProcessor.h` and all four Codex F001 deliverable documents exist on disk as **null bytes only** (file content destroyed, sizes preserved). A clean rebuild of the project is currently impossible, the header-side half of the fix is unreviewable from disk, and the fix documentation is lost. F001 stays open until the files are restored and a clean build re-verified.

---

## 0. Blocker: null-file corruption in the working tree

| File | Size on disk | Content |
|---|---|---|
| `Source/Core/PluginProcessor.h` | 6,304 bytes | **100% 0x00 bytes** |
| `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-report.md` | 3,666 bytes | **100% 0x00 bytes** |
| `docs/reports/P13A-F001-host-pdc-latency-reporting-fix-summary.md` | 950 bytes | **100% 0x00 bytes** |
| `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-findings.json` | 2,296 bytes | **100% 0x00 bytes** |
| `artifacts/audits/P13A-F001-host-pdc-latency-reporting-fix-checklist.md` | 914 bytes | **100% 0x00 bytes** |

Facts established:
- Byte scan confirms zero non-null bytes in all five files. A full scan of `Source/Core/*`, `docs/reports/*`, and `artifacts/audits/*` found **no other corrupted files** — `PluginProcessor.cpp`, `AudioEngine.h/.cpp`, `AudioEngineTests.cpp` are intact, valid text.
- Timestamps: corrupted files were last written 13:22:11–13:25:35 on 2026-06-10. `PluginProcessor.cpp` (13:26:23) and `audio-base-test-report.txt` (13:28:10, `268 results / 7543 passes / 0 failures, status=PASS`) postdate them and survived.
- The build/test pass implies a **valid header existed at compile time** (the intact `.cpp` references `hostLatencyRefreshPending`, `audioEnginePreparedForHostLatency`, `lastReportedHostLatencySamples`, `handleAsyncUpdate`, `audioEngineLatencyChanged`, `getAudioEngine()` — all of which must be declared in that header). The content was destroyed afterward, with the write timestamp preserved — a pattern consistent with **OneDrive sync corruption** of files written in rapid succession (the repo lives under `OneDrive\Desktop`).
- Consequence: `git diff` shows the header as binary; any clean rebuild now will fail; git holds only the **pre-fix** header.

**Recovery options (user action, not performed by this review):** OneDrive web version history for each file (most likely to work, given timestamps), Visual Studio local history/backup, or having Codex re-emit the header and reports from its session. After restore: byte-scan again, clean rebuild, re-run validation.

This is not a defect in the fix design — but it is a hard blocker for closing F001, and a process risk for the whole repo (see R-008: repo under OneDrive).

---

## 1. Review questions answered

### Q1 — Is `setLatencySamples` called only from safe non-audio-thread context? **YES**

All host reporting funnels through `applyHostLatencyIfChanged` (PluginProcessor.cpp:500-513). Call paths:
- `prepareToPlay` → `updateHostLatencyFromEngineNow` (PluginProcessor.cpp:588-589) — host setup thread, non-RT by contract.
- `handleAsyncUpdate` (PluginProcessor.cpp:470-475) — message thread.
- `synchronizeEngineNow` (PluginProcessor.cpp:1090-1093) — message-thread callers only (state restore, preset load, pedal add/remove/move/bypass, engine toggle).
- `hardRefreshAudioEngineForCurrentIO` (PluginProcessor.cpp:1053) — message thread.

The audio thread can *cause* a latency change only indirectly (engine-enable parameter change → command queue → flush on message/control thread → `publishGraph`/`applyPedalBypassToActiveGraph` → `notifyLatencyChanged`). The listener callback (`audioEngineLatencyChanged`, PluginProcessor.cpp:478-484) never calls the host: it only checks an atomic gate and calls `triggerAsyncUpdate()` (safe from any thread). `processBlock` itself is untouched (PluginProcessor.cpp:604-611). **No path reaches `setLatencySamples` from the audio callback.**

### Q2 — Is `AudioEngine::LatencyListener` safe and minimal? **YES, with one small caveat**

- Single pure-virtual method, no ownership, stored as `std::atomic<LatencyListener*>` (AudioEngine.h:33-37, 289), set before any engine activity in the processor constructor (PluginProcessor.cpp:379), cleared first thing in the destructor (PluginProcessor.cpp:410) followed by `cancelPendingUpdate()`.
- `notifyLatencyChanged` is acquire-load + call (AudioEngine.cpp:245-249). Minimal and appropriate.
- **Caveat (R-003, Low)**: an atomic pointer gives no lifetime guarantee — the engine control thread can load the pointer just before the processor destructor nulls it and invoke the listener during teardown. The window is tiny, the callee touches only atomics + `triggerAsyncUpdate`, the processor object is still alive during its destructor body, and JUCE's `AsyncUpdater` destructor cancels pending updates — so this is theoretical today. Worth a comment documenting the destruction-order contract (listener cleared → engine member destroyed → control thread stopped).

### Q3 — Are latency updates triggered at all required points? **YES — all five, verified**

| Trigger | Mechanism | Evidence |
|---|---|---|
| `prepareToPlay` | Synchronous: gate set true, then direct `updateHostLatencyFromEngineNow()` | PluginProcessor.cpp:588-589 |
| Graph publish (add/remove/move/clear, heal, enable rebuild) | `publishGraph` captures latency before move, calls `notifyLatencyChanged` → async refresh | AudioEngine.cpp:114-129 |
| Bypass in-place latency rebuild | `applyPedalBypassToActiveGraph` → `notifyLatencyChanged` after `updateDryDelayLatency` | AudioEngine.cpp:230-237 |
| Pedal add/remove (UI path) | Both: publish notification *and* synchronous `synchronizeEngineNow` → `updateHostLatencyFromEngineNow` | PluginProcessor.cpp:1090-1093 |
| Preset/session restore | `setStateInformation` / `loadPresetFromFile` / `resetSessionState` all end in `synchronizeEngineNow` | PluginProcessor.cpp (state restore + preset paths) |

Constructor-time ordering is correct: the listener is registered before `resetSessionState`/`restoreStartupPresetIfAvailable` fire graph publishes, but `audioEnginePreparedForHostLatency` is false, so those notifications are deliberately dropped — and recovered by the synchronous update in `prepareToPlay`. `releaseResources` re-arms the gate to false; the next `prepareToPlay` resynchronizes. No window loses a final value.

### Q4 — Is async/deduped reporting correct; can it miss updates? **Correct; cannot miss**

The pattern is flag-coalescing + re-read-latest:
- `requestHostLatencyRefresh` sets `hostLatencyRefreshPending` then `triggerAsyncUpdate` (PluginProcessor.cpp:486-490).
- `handleAsyncUpdate` exchanges the flag false and reads the **current** engine latency (not a captured stale value) (PluginProcessor.cpp:470-475, 492-498).
- If a new change lands between the exchange and the read, the new notification re-sets the flag and re-triggers; the second pass dedupes by value in `applyHostLatencyIfChanged` (`lastReportedHostLatencySamples.exchange`, PluginProcessor.cpp:503). Coalesced bursts always converge on the latest value. No missed-final-value scenario found.

### Q5 — Can it spam async updates or cause host notification loops? **No**

- Value dedupe: `setLatencySamples` is only called when the clamped value actually differs (PluginProcessor.cpp:503-505).
- Host-re-prepare loop: some hosts respond to latency changes by calling `prepareToPlay` again → engine re-prepare → publish → notify → async → same value → deduped → loop terminates after one round.
- `triggerAsyncUpdate` is itself coalescing (one pending update at a time).
- Burst sources (rapid bypass automation) produce at most one host notification per actual latency value change — but see Q6 for whether those changes *should* reach the host at all.

### Q6 — Does the live graph latency policy make sense for DAW/VST3? **Defensible and documented, but has a real cost (R-002)**

The policy comment (PluginProcessor.cpp:507-509) states live graph latency is reported. Pros: dry/wet-accurate PDC at all times; matches the engine's internal dry-delay model; simplest truthful answer. Cons: **bypassed pedals report 0 latency, so every bypass toggle changes graph latency and fires `restartComponent(kLatencyChanged)` at the host.** For a guitar plugin where bypass is a *performance gesture* (footswitch during playback), many hosts respond with an audio pause, graph flush, or click. Same applies to pedal add/remove mid-playback. This is a known industry tradeoff (the common alternative: report a stable per-topology maximum that ignores bypass state, accepting dry/wet misalignment in the host's view). The P13A audit asked for the policy to be *decided and documented* — it was. Recommend validating the felt cost in a real DAW (bypass automation during playback) before P13B locks it in; if hosts glitch audibly, switch to bypass-invariant reporting.

### Q7 — Do tests prove processor latency > 0 after prepare and equal to engine graph latency? **YES**

`AudioEngineTests.cpp:4514-4552` ("P13A host PDC reports live AudioEngine graph latency"): constructs a real `NOVAAudioProcessor`, calls `prepareToPlay`, asserts `baselineLatency > 0` (OutputChain lookahead) and `processor.getLatencySamples() == engine latency`; then adds Overdrive (asserts host latency rises with it), bypasses (asserts host latency returns to baseline), un-bypasses (asserts restoration). This covers prepare, pedal add, and bypass round-trip at the **processor/host boundary** — exactly the assertion P13A demanded. Suite result: 268 results / 7,543 passes / 0 failures (`audio-base-test-report.txt`, intact).

Limitation (R-004, Low): the test exercises the **synchronous** reporting paths (`prepareToPlay`, `synchronizeEngineNow`). The control-thread `notify → AsyncUpdater → handleAsyncUpdate` path needs a pumping message loop and has no automated coverage. The synchronous path is the dominant one (all UI operations end in `synchronizeEngineNow`), so risk is low.

### Q8 — Do tests cover limiterDb 0/−6/−12 stability after P12D? **YES**

Same test, lines 4525-4537: sweeps the `OUTPUT_LIMITER` parameter through 0/−6/−12 dB with processing between, asserting both engine graph latency and processor host latency stay at baseline. Complements the existing P12D OutputChain-level latency-invariance test (4490-region pre-fix numbering).

### Q9 — Is the RT policy scan FAIL real? **No — stale phase-gate noise; active routes pass**

`artifacts/audio-thread-policy-scan.txt` (2026-06-10 13:28): `status=FAIL`, 12 contract failures — **all 12 are phase-scoped git-diff guards** from earlier tasks (P9F/P9G "must not change OutputChain/AudioEngine files", P10C–P10G / P11A–P11C "no OutputChain masking") tripping because the uncommitted working tree accumulates P12D + F001 changes to those files. The scan itself states: **"No FAIL findings in active audio routes"** (line 53), and all RT-pattern checks over the 32 active files pass. No audio-thread-safety regression. Concern (R-005): a permanently-FAILing gate stops protecting anything — the phase guards need retiring/re-scoping once these changes are committed.

### Q10 — Safe enough to close P13A-F001? **NOT YET**

Design: yes. Tests: yes. Working tree: **no** — the header implementing half the fix is null bytes on disk, and the fix documentation is destroyed. Closing criteria: (1) restore the five corrupted files, (2) byte-verify, (3) clean rebuild + re-run validation (expect 268/7543/0), (4) then close.

### Q11 — Additional validation required before P13B? **Yes — three items, all cheap**

1. **Restore + clean rebuild** (blocker above).
2. **Release VST3 build** — never run for this change; the latency-reporting path goes through the VST3 wrapper (`restartComponent`), which Debug Standalone never exercises.
3. **One manual DAW PDC check** — load in Reaper (shows PDC per-FX), confirm ~96 samples @48 kHz reported with empty board, confirm it changes on pedal add, and *listen* to a bypass toggle during playback to evaluate the live-latency policy cost (Q6).
P13B effect audits may start in parallel after item 1; items 2–3 should land before any release tag.

---

## 2. Findings

| ID | Finding | Severity | Probability |
|---|---|---|---|
| P13A-F001-R001 | `PluginProcessor.h` + all 4 Codex F001 deliverable docs are null-byte corrupted on disk; clean rebuild impossible; header design unreviewable from disk; fix docs lost. Restore from OneDrive version history / VS local history / Codex re-emit before closing F001. | **Critical** (repo state, not design) | Certain (present now) |
| P13A-F001-R002 | Live-latency policy: bypass toggles and pedal edits during playback fire host `restartComponent(kLatencyChanged)`; hosts may pause/click. Policy documented but felt cost unvalidated in a real DAW; bypass-as-performance-gesture is core to a guitar plugin. | Medium | Medium |
| P13A-F001-R003 | Listener teardown: atomic pointer carries no lifetime guarantee; control thread may invoke listener during processor destruction window. Practically mitigated (atomics-only callee, `cancelPendingUpdate`, JUCE AsyncUpdater dtor); document destruction-order contract. | Low | Low |
| P13A-F001-R004 | Async reporting path (control-thread notify → AsyncUpdater → host) has no automated coverage; test exercises only synchronous paths. | Low | Medium |
| P13A-F001-R005 | RT policy scan permanently FAIL from stale phase-scoped git-diff guards (P9F–P11C); gate no longer binary-meaningful. Retire/re-scope guards after committing. | Low | High |
| P13A-F001-R006 | Release VST3 build and manual DAW PDC inspection not run (acknowledged by Codex). Required before release tag; not required to start P13B. | Medium | High |
| P13A-F001-R007 | Repo lives under `OneDrive\Desktop`; sync corruption is the likely cause of R001 and can recur on any rapid-write burst (builds, codegen, agents). Consider moving the working repo outside OneDrive or excluding it from sync; rely on git for history. | Medium | Medium |

Verified-good (no finding): no audio-thread host calls; correct constructor/prepare ordering with the prepared-gate; coalesced+deduped async reporting that cannot miss the final value; no notification loops; all five trigger points wired; processor-boundary test with limiter sweep; suite 268/7543/0.

---

## 3. Closing position

**Approve with concerns.** The fix itself resolves P13A-F001 as designed — correct thread contract, complete trigger coverage, dedicated processor-boundary regression test. Keep **P13A-F001 open** until the corrupted files are restored and a clean rebuild re-validates. Treat R007 (OneDrive) as a standing process risk: this incident silently destroyed a source file *after* a green build — the next one might destroy something without a green build to prove what existed.
