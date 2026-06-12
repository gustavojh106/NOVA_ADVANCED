# P13A — Base Audio Platform Audit (post-P12D)

- **Task**: P13A-base-audio-platform-audit
- **Date**: 2026-06-10
- **Type**: Read-only engineering audit. **No source code, tests, or presets were modified.**
- **Scope**: Core/base audio system only (AudioEngine lifecycle, processBlock flow, graph build/publish/swap, runtime parameter snapshots, InputChain, ChannelStrip, OutputChain, dry/wet, bypass, tuner, latency/PDC, RT safety, lifecycle, diagnostics, base tests). Individual effect tone explicitly excluded.

---

## 1. Base signal path map

### Stage 0 — Host entry: `NOVAAudioProcessor::processBlock`
- **Files**: `Source/Core/PluginProcessor.cpp:551-558`, `PluginProcessor.h`
- **What it does**: `ScopedNoDenormals`; `refreshEngineEnabledIfNeeded(false)` and `refreshEngineGlobalParamsIfNeeded(false, false)` (reads atomic caches in `SessionStore`, polls host transport every 8 blocks); delegates to `AudioEngine::process`; pushes the post-engine buffer into `SimpleOscilloscope` (double-buffered fixed array, atomic frame publish).
- **Gain/latency/stereo/silence**: none directly.
- **Sample-rate dependent**: no.
- **RT safety**: Reads are atomic (`RuntimeGlobalParamAtomics`, `engineEnabledCache`). Edge cases: when the engine-enabled parameter *changes*, the audio thread itself calls `audioEngine.setEngineEnabled()` → `enqueueGraphCommand` (CriticalSection + `std::deque::push_back` allocation) + `SessionLogger::logEvent` (string building, spin-lock queue) + `triggerAsyncUpdate` (message post). Also `updateGlobalParams` triggers an async update on the very first block (audio thread ID not yet latched). Rare and bounded, but not strictly RT-clean. See F009.

### Stage 1 — Engine gatekeeping: `AudioEngine::process`
- **Files**: `Source/Core/AudioEngine.cpp:475-555`
- **What it does**, in order:
  1. Latches audio thread ID; increments `audioBlockCounter` (used by graph retirement grace window).
  2. Applies pending runtime reset (`audioRuntimeResetRequested`) — resets dry/wet ramp, dry delay line, tuner, health monitor.
  3. Starts `CpuMeter` timing (timer reads + atomics only).
  4. Sets dry/wet ramp target from `params.getOutputMixNormalized()`.
  5. Light input peak meter (decimated 16/4/1 blocks by diagnostics mode).
  6. **Engine off** → buffer passes through *unprocessed* (sanitize + meter only). Dry passthrough, no gain change, no latency.
  7. **Startup counter** (5 blocks cold prepare / 6 after rebuild) → same dry passthrough.
  8. **Tuner enabled** → buffer pushed to `TunerService` FIFO (lock-free `AbstractFifo`, preallocated), then **buffer cleared** (output silent).
  9. **No active graph** → buffer cleared (silent).
  10. Otherwise → `processWithSampleAccurateDryWet`, then `HealthMonitor::sanitizeAndMeterOutput` (NaN/Inf → 0, |x| > 24.0 hard-clamped, near-clip/spike scan in non-production modes), then health actions (auto-heal request flag only).
- **Gain**: hard clamp at `HARD_ABS_LIMIT_LINEAR` 24.0 (≈+27.6 dBFS). **Silence**: tuner and no-graph paths clear the buffer. **Latency**: engine-off/startup paths are zero-latency dry; the wet path carries graph latency — toggling engine therefore time-shifts audio with no crossfade (F008).
- **RT safety**: atomics + raw-pointer graph access only; no locks, no allocation, no logging on the steady-state path. Confirmed by `artifacts/audio-thread-policy-scan.txt` (status=PASS, 638 contract checks, 0 failures, 2026-05-19).

### Stage 2 — Runtime parameter snapshot
- **Files**: `Source/Core/Audio/RuntimeParameterSnapshot.h`, `SessionStore.h:129-137, 388-497`
- **What it does**: All global params (input gain/gate/forceMono, output vol/limiter/mix, switch mode, line gain/pan/width, host tempo) stored as individual relaxed atomics with a release-incremented `revision`. Control thread applies snapshots to graph processors only when revision changes (`GraphBuilder::applyRuntimeParamsToGraph`). The audio thread reads only `getOutputMixNormalized()`.
- **Caveat**: field-by-field atomics, not a seqlock — a reader can observe a torn *set* (e.g. new gainA with old panA) for one application pass. Benign here because every consumer field is independently sanitized and smoothed.
- **RT safety**: yes.

### Stage 3 — Dry capture + graph render: `processWithSampleAccurateDryWet`
- **Files**: `AudioEngine.cpp:557-596`, `Source/Core/Audio/DryWetMixer.h`
- **What it does**: classifies mix endpoint (Dry / Wet / Mixed with 1e-5 epsilon). Dry endpoint: graph not run at all (consumes ramp only). Otherwise captures dry into preallocated scratch (capacity ≥ 8192 samples), runs `runtime.graph->processBlock`, then for Mixed: dry is run through a latency-compensation delay line (clamped to `MAX_GRAPH_LATENCY_SAMPLES` 8192) and mixed sample-accurately against the wet ramp (8 ms ramp).
- **Oversized fallback**: blocks larger than scratch capacity process wet-only with no allocation.
- **Gain**: yes (mix law `dry*(1-w) + wet*w`, bounded). **Latency**: dry path delayed to match wet graph latency — internally correct. **Silence**: dry endpoint with mix=0 returns input untouched.
- **RT safety**: yes — all buffers preallocated; atomic latency value.
- **Structural caveat**: the dry/wet mix happens **after** the OutputChain limiter, so at mix < 100% the final output is not limiter-protected (F003), and the dry component is raw host input (no gate/conditioning) by design.

### Stage 4 — Graph: `InputChain → [pedals] → ChannelStrip A/B → OutputChain`
- **Files**: `Source/Core/Audio/GraphBuilder.h`, `GraphRuntimeTypes.h`
- **Topology**: host in → `InputChainProcessor` → chain A pedals (zone-sorted Pre→Amp→FX→Cabinet via stable sort) → `ChannelStripProcessor` A; same input feeds chain B → strip B; both strips sum into `OutputChainProcessor` → host out. Mono input duplicated to both InputChain inputs.
- Both chains are **always connected and always process**; line muting is done by strip gain = 0 (`RoutingMixer`), with a muted fast-path `buffer.clear()`. CPU is spent on inactive-line pedals; correctness unaffected.

### Stage 5 — InputChain
- **Files**: `Source/Core/DSP/Global/InputChain.h/.cpp`
- **Order**: scrub (`SignalGuard`: NaN/Inf→0, denormal flush, tanh-taper above 24.0) → auto-routing detector (signal-level-based L↔R copy for single-jack input) → forceMono blend (smoothed 12 ms) → subsonic HP 24 Hz + DC block 12 Hz (one-pole cascades) → input gain (smoothed 20 ms, sanitized **−36..+24 dB**) → musical gate (envelope + hysteresis 1.5×/0.56×, 24 ms hold, smooth gain, threshold smoothed 35 ms, ≤−95 dB = off).
- **Gain**: yes (input gain, gate). **Stereo image**: yes — auto-routing copies one channel onto the other when only one side is active (instant switch into mono-copy mode on first detection block; 3-block confirmation only for leaving/changing); forceMono collapses image. **Phase**: one-pole HPs add LF phase shift (inherent). **Latency**: zero reported, zero structural.
- **Sample-rate dependent**: all coefficients derived from SR in `prepareToPlay`. Correct.
- **RT safety**: no locks/allocations; atomics for param handoff; counters via relaxed atomics.

### Stage 6 — ChannelStrip (A/B)
- **Files**: `Source/Core/DSP/Global/ChannelStrip.cpp/.h`
- **Order**: scrub → gain (smoothed 15 ms, sanitized 0..2, muted fast-path clear) → mid/side width (0..2, energy compensation `1/sqrt(0.5(1+w²))` above 1) → balance pan law (unity center, cos taper of opposite channel).
- **Gain/stereo/phase**: yes (by definition). No latency. SR-dependent smoothing only. RT-safe (telemetry is fixed-struct, lock-free queue publish every ~2 s).
- **Routing policy quirk**: `RoutingMixer::makeLineTarget` maps line gain ≤ 0.001 → **1.0 (unity)** unless line is muted by switch mode. Host parameter "Level A/B" allows 0.0 → user gets unity, not silence, with a discontinuity at 0.001 (F004).
- Dual-parallel applies fixed 0.5 compensation to both lines (sum bounded).

### Stage 7 — OutputChain (post-P12D)
- **Files**: `Source/Core/DSP/Global/OutputChain.cpp/.h`
- **Order**: telemetry input capture → param targets (atomics, sanitize: vol **−36..+12 dB**, limiter −12..0 dB) → scrub → dual cascaded DC blockers (10 Hz) → master gain (smoothed 25 ms, sample-accurate) → **always-armed lookahead limiter** (2 ms lookahead, 6 ms hold, program-dependent release 45–180 ms, linked true-peak estimate via Catmull-Rom inter-sample points, threshold = min(user dB, **0.97 linear ≈ −0.265 dB** transparent safety floor), threshold smoothed 50 ms) → final soft ceiling (tanh knee 0.985→0.999, last resort) → telemetry publish.
- **Gain**: yes. **Latency**: lookahead delay always active, reported once via `setLatencySamples(limiter.getLookaheadSamples())` (~96 samples @48 kHz). **Silence**: no. **SR-dependent**: lookahead/hold/release all derived from SR.
- **RT safety**: no allocation in processBlock; delay buffer preallocated in `prepareToPlay`; atomics for params.

### Stage 8 — Health/auto-heal + host output
- `HealthMonitor` (header-only): sanitizes final buffer (audio thread), counts corruption; 2 consecutive corrupt blocks → `graphResetRequested` flag → control thread rebuilds a fresh graph (immutable swap, 256-block cooldown). Sustained input-active/output-silent (0.75 s) → incident log via flag → background thread.

### Control plane (not audio path, but governs it)
- Commands (`AudioEngineCommandQueue`: CriticalSection + deque) are drained on the message thread or the low-priority `AudioEngineControlThread` (20 ms poll; 5 ms when tuner on). Topology changes rebuild a complete new `GraphRuntime` (new processor instances), `prepareToPlay` it off the audio thread, then publish via atomic raw-pointer swap (`RuntimeGraphManager::publish`). Old graph retired with an 8-block grace window (`GraphRetirementQueue`, bounded to 8 graphs), destroyed on the control thread.
- **Exception to the immutable-swap rule**: pedal bypass (`applyPedalBypassToActiveGraph`) mutates the **active** graph in place and calls `juce::AudioProcessorGraph::rebuild()` from the control thread while audio may be rendering that graph. This relies on JUCE's internal render-sequence locking; correctness holds, but the audio thread can block on the graph's callback lock during the swap (F005). The in-place behavior is intentional and asserted by tests (`AudioEngineTests.cpp:5670-5700` requires the same processor instance survive bypass).

---

## 2. P12D verification

| Claim | Verdict | Evidence |
|---|---|---|
| `limiterDb=0` no longer bypasses limiting | **Confirmed** | `isLimiterActiveDb()` returns `true` unconditionally (`OutputChain.cpp:38-43`); `limiterDbToLinear()` caps threshold at 0.97 (`OutputChain.cpp:24-36`); the armed test inside `PeakLimiter::process` (`threshold < 0.9885531`) is always true at threshold ≤ 0.97. No remaining branch skips the limiter. |
| `limiter.process` runs unconditionally | **Confirmed** | `processBlock` calls it with no guard (`OutputChain.cpp:293`). The only alternate path is `processWithoutLookahead` for degenerate buffer/delay states — still limits, just without lookahead. |
| Limiter latency reported consistently | **Confirmed (internally)** | `updateReportedLatencyForLimiter` ignores limiterDb and always sets lookahead samples; called from `prepareToPlay`, `reset`, and `setParams`. Test at `AudioEngineTests.cpp:4490` asserts identical latency at 0/−6/−12 dB. **However the host is never told any of this — see F001.** |
| `Result::active` means real gain reduction | **Confirmed** | `active = minGain < 0.999999 || touchedSamples > 0` in both paths (`OutputChain.h:257, 395`); the armed state is computed but no longer drives `active`. |
| Soft ceiling emergency-only | **Confirmed** | Limiter ceiling 0.97 sits below the soft-ceiling knee start 0.985; P12D hot-master test asserts `softCeilingTouchedSamples == 0` under +10.73 dB master with 0.5-amplitude program (`AudioEngineTests.cpp:4409`). The ceiling can still touch inter-sample overshoot the true-peak estimator misses — that is its intended role. |
| Remaining 0.999-pinning path via `outputVolumeDb + limiterDb`? | **None found inside OutputChain** | `outputVolumeDb` sanitized to ≤ +12 dB; limiter always armed at ≤0.97 with instant attack (lookahead absorbs the transient). Two adjacent caveats, not regressions: (a) at `outputMix < 100%` the dry/wet sum happens after the limiter, so the final output is not ceiling-guaranteed (F003); (b) the soft ceiling still exists for estimator misses, which is by design. |
| P12D tests meaningful or overfitted? | **Meaningful** | The hot-master test reproduces the exact failing field log (+10.73 dB), renders 64 blocks end-to-end through the real engine, and asserts output peak < 0.98, zero soft-ceiling work, and >0.5 dB actual limiter reduction. The quiet-signal companion asserts RMS pass-through within ±6% and zero limiter activity, guarding against the obvious overcorrection (always-on limiting squashing normal signal). The latency test asserts invariance, not a magic constant. These target behavior, not implementation internals (the debug snapshot is read-only telemetry). Not overfitted. |

**P12D conclusion**: the transparent-safety fix is structurally sound and properly regression-tested at the OutputChain and engine level.

---

## 3. Base gain staging

| Stage | Range / behavior | Issues |
|---|---|---|
| Input gain | Param **−60..+24 dB**; sanitizer clamps to **−36..+24 dB** (`InputChain.cpp:3-9`) | **Mismatch**: bottom 24 dB of host-visible knob travel is dead; cannot attenuate input below −36 dB (F012). NaN/Inf → 0 dB. |
| Gate threshold | Param −100..0 dB; sanitizer −120..0; ≤−95 dB = gate off; closed gain 1e-4 (−80 dB), hysteresis + hold | Sane. Quirk: `GraphBuilder.h:120-123` treats exactly 0.0 dB as "off" (remaps to −100) — a user maxing the gate gets no gate (F013, low). |
| Line gain A/B | Param 0..2; strip sanitizer 0..2; smoothed 15 ms | **`RoutingMixer` maps ≤0.001 → 1.0 (unity)** unless mode-muted. Setting Level A to 0 yields full volume; discontinuity at 0.001 (F004). Policy is documented in tests (4762, 5202) but objectively a gain surprise. |
| Dual parallel | Both lines ×0.5 compensation | Bounded; identical lines sum to unity (tested at 3856). |
| Pan/width | Pan: unity-center balance law, never boosts. Width 0..2 with energy compensation ≥1 → comp ≤1, never boosts | Safe. |
| Output volume | Param **−60..+12 dB**; sanitizer **−36..+12 dB** (`OutputChain.cpp:3-11`) | Same dead-zone mismatch: output cannot go below −36 dB; users expecting near-silence at knob minimum get −36 dB (F012). |
| Output limiter | −12..0 dB, threshold capped at 0.97 linear, always armed | Verified above. |
| Global mix | 0..100 raw, normalized atomically; 8 ms ramp; exact-dry and exact-wet endpoints | **Mixed output is post-limiter** (F003): `0.5*dry + 0.5*wet` can exceed the 0.97/0.999 ceilings if host input is hot; only the 24.0 hard clamp applies after mixing. Dry component bypasses gate/conditioning by design. |
| Double-gain risk | None found | Input gain applied once (InputChain); line gain once (strip); master once (OutputChain). RoutingMixer targets are computed from one snapshot and pushed to strips only on revision change. |
| Mono/stereo collapse | Two deliberate mechanisms: forceMono blend; auto-routing single-jack copy | Auto-routing is signal-dependent and enters mono-copy mode after a *single* detection block (instant `currentMode = detected` when leaving Stereo, `InputChain.h:221-227`) with a hard channel copy and no crossfade — asymmetric stereo program at the plugin input can be collapsed or clicked (F007). For guitar (mono source) this is the desired behavior. |

---

## 4. Base latency / PDC

- **Internal graph latency**: computed per build from `juce::AudioProcessorGraph::getLatencySamples()`, clamped to 8192, stored in `GraphRuntime::latencySamples`. With no pedals it equals the OutputChain lookahead (~96 samples @48 kHz; verified by test 5670).
- **Dry-path compensation**: `DryWetMixer` delays the captured dry signal by the active graph latency (atomic, updated on publish and on in-place bypass rebuild). Unit-tested (4514–4724) including read-before-write order and clamping. Sound.
- **Bypass compensation**: `ProcessorBase` keeps per-pedal latency lines so the bypass crossfade is time-aligned, reports 0 latency when bypassed, and the engine rebuilds graph latency in place on bypass toggle (test 5670). Sound.
- **Host PDC: absent.** `NOVAAudioProcessor` never calls `setLatencySamples()` on itself (grep over `Source/` confirms the only call sites are OutputChain, ProcessorBase, and the dry mixer's internal value). The host believes the plugin has **zero latency** while the wet path is at minimum ~2 ms late (more with oversampled pedals: Overdrive measurably adds latency per test 5680-5685). In a DAW this is an audible timing/comb error against parallel tracks. **This is the single biggest base-platform gap** (F001).
- **Dynamic latency**: graph latency legitimately changes on pedal add/remove/bypass. Once host reporting is added, every such change will retrigger host PDC — a policy decision will be needed (report max-of-chain vs live value). Currently moot because nothing is reported.
- **Always-on limiter vs dry/bypass mismatch**: internally compensated (dry delay line tracks total latency including lookahead). Engine-off passthrough, however, is zero-latency while engine-on is not → toggling the engine time-shifts audio with no ramp (F008, cosmetic click risk).

---

## 5. RT safety

- **Steady-state audio path** (engine on, params stable): atomics and raw pointers only; no locks, allocation, strings, or logging. Scratch and delay buffers preallocated (`prepareScratchBuffers`, limiter delay, bypass latency lines). `ScopedNoDenormals` at every processor entry. Confirmed by code reading and by `artifacts/audio-thread-policy-scan.txt` (PASS, 32 active files, 638 contract checks, 0 failures).
- **Graph access**: audio thread reads `std::atomic<GraphRuntime*>`; publication keeps the old graph alive ≥8 audio blocks via shared_ptr retirement on the control thread. Destruction never happens on the audio thread. `RuntimeGraphManager` unit-tested (3735).
- **Telemetry**: `PedalSignalTelemetry` accumulates into fixed structs and publishes through a lock-free 256-slot CAS ring (`RealtimeSignalTelemetryQueue`) with drop counting — RT-safe. `SessionLogger` enqueues into a spin-locked bounded ring drained by a background thread — fine off the audio thread; it does build `juce::String`s at the call site, so the rare audio-thread call paths below matter.
- **Residual edges** (all rare, none steady-state):
  1. Engine-enabled parameter change observed in `processBlock` → command enqueue (CriticalSection + deque allocation) + `SessionLogger::logEvent` string work + `triggerAsyncUpdate` from the audio thread (`PluginProcessor.cpp:554`, `AudioEngine.cpp:378-385, 241-253`). (F009)
  2. First audio block after prepare: `updateGlobalParams` triggers an async update because the audio thread ID is not yet latched (`AudioEngine.cpp:190-192`). One-shot.
  3. Pedal bypass: in-place `graph->rebuild()` on the control thread contends JUCE's graph callback lock with the audio thread (F005).
- **Oversized blocks**: graceful wet-only fallback above scratch capacity (≥8192 samples) with zero allocation; below that, the graph is asked to process blocks larger than it was prepared for (tolerated by tests 3303/5399 at +32 samples, but the >8192 fallback itself is never exercised end-to-end — F010).
- **Health monitor**: header-inline, atomics + plain ints touched only from the audio thread; actions are flags consumed by the control thread. RT-safe.
- **Visualizer**: `SimpleOscilloscope::pushBuffer` runs on the audio thread; fixed-size history/frame copies, no allocation; bounded fixed cost per block. Acceptable.

---

## 6. State and lifecycle safety

- **prepareToPlay**: host path is clean — `AudioEngine::prepare` resets counters, rebuilds scratch, builds and publishes a fresh graph (built and `prepareToPlay`-ed off the audio thread). Sample-rate and block-size changes are handled by full re-prepare; all base DSP recomputes coefficients from SR. Engine-level test sweeps 44.1/48/96 kHz and multiple block sizes (3874).
- **releaseResources**: base processors reset state; no resource leaks observed (graphs retired/cleared in destructor with thread stop).
- **Engine on/off**: enable requests force a runtime reset + fresh graph rebuild before processing resumes; disable leaves a dry passthrough. Recovery round-trip tested (5553, 5587 — re-enable re-prepares released pedal processors).
- **UI-driven re-prepare race (F006)**: `toggleEngine()` calls `hardRefreshAudioEngineForCurrentIO()` → `audioEngine.prepare()` from the **message thread while the host audio callback may be running**. `prepareScratchBuffers` resizes the DryWetMixer scratch/delay vectors unsynchronized against the audio thread. Today this is safe only by ordering: the engine is necessarily *off* at that moment, so `process()` takes the early-return path that never touches those buffers. That invariant is implicit and fragile — any future caller of `prepare()` with the engine on is a data race.
- **Host state restore / presets**: restore goes through SessionCoordinator → full engine rebuild from state, pedal states reapplied via Base64 blobs (`SessionPersistence.h:48-123`), then forced param push + sync. Invalid payloads reset to a clean state rather than crash. Sound at the base level.
- **Auto-heal state loss (F002)**: the health-monitor-triggered `RebuildGraph` rebuilds pedals from `GraphChainNodeSpec` (type/zone/bypass only). New processor instances get **default parameters** — nobody replays the captured pedal states on this path (state reapplication exists only in the session-layer rebuild). A user surviving an auto-heal keeps their pedalboard but loses every knob setting, silently. Low probability (requires 2 consecutive corrupt blocks), high surprise.
- **Stuck-silent risk**: explicitly monitored (input-active/output-silent incident detection, 0.75 s window, with recovery logging). Startup counters suppress garbage blocks after prepare/rebuild. No-graph path outputs silence rather than noise. Good.
- **Graph rebuild while audio runs**: immutable swap pattern is correct; commands are order-preserving (tested 5738); generation counter monotonic; retired graphs bounded to 8.

---

## 7. Test quality

**Corpus**: 267 test results, 7,526 passes, 0 failures (`artifacts/p12d-tests/test-report.txt`). Base-audio coverage is substantial and mostly behavioral:

**Meaningful and targeting real failure modes:**
- P12D regression trio (4330/4414/4490): reproduces the exact field failure, asserts transparency and latency invariance. Strong.
- Dry/wet: exact-dry transparency with a latency-relevant wet chain present (4135), wet-only topology response (4185), ramp continuity (4253), plus DryWetMixer unit tests covering delay read-order, clamping, endpoint classification (4514–4724).
- Graph lifecycle: publish-before-retire (3735), retirement grace (3682), command order (5738), bypass latency in-place rebuild with processor identity (5670), ClearAll→add (5702), enable/disable recovery (5553, 5587), pre-prepare topology (6387).
- Routing: per-mode strip targets pinned to current `RoutingMixer` policy (4762–4814), dual-parallel unity (3856), low-gain fallback and inactive-line isolation (5202) — note these *pin* the gain≤0.001→unity policy rather than challenge it.
- Conditioning: forceMono collapse (2874), single-jack promotion (2900), DC stripping (3076/3113/3136), soft ceiling (3052), lookahead transient catch (3278), oversized OutputChain blocks (3303), SR/block-size sweep (3874), realistic guitar program with limiter protection (4018).

**What they prove**: the base path is finite, level-stable, latency-consistent, transparent at endpoints, and recovers from topology churn — under single-threaded, deterministic invocation at 48 kHz/64 with the documented policies.

**What they do not prove:**
1. **Host PDC** — nothing asserts `NOVAAudioProcessor::getLatencySamples()`; the gap of F001 is invisible to the suite.
2. **Concurrency** — all tests drive `process()` and the control plane from one thread. The publish/retire race, in-place bypass rebuild contention, and the F006 prepare race are untested (hard to test, but currently zero coverage).
3. **Auto-heal semantics** — no test injects corruption and verifies recovery, let alone pedal-state survival (would catch F002).
4. **True oversized fallback** — the "oversized" engine test uses prepared+32 samples; the wet-only >8192 fallback branch is unit-tested in DryWetMixer but never end-to-end.
5. **Param-range vs sanitizer agreement** (F012) — no test asserts the host parameter range maps onto the effective DSP range.
6. Tuner-path output muting, engine-off/on transition continuity.

**Adapted-to-pass check**: the P12D-adjusted tests (e.g. 4185's lookahead priming, 5670's "baseline = OutputChain latency, not zero") are legitimate accommodations of the new always-on latency, with comments explaining why — they still assert the original behaviors. No test was found that merely loosened a threshold to green-light P12D.

---

## 8. Scorecard

| Area | Score | Evidence | Key files | To improve |
|---|---|---|---|---|
| Base signal architecture | **8.5** | Clean two-plane split, immutable graph swap with retirement grace, zone-canonical builds, single-writer audio state | AudioEngine.*, GraphBuilder.h, RuntimeGraphManager.h | Remove the in-place bypass-rebuild exception or document its lock contract; delete dead `resetRuntimeGraph` |
| Gain staging safety | **7.0** | Sanitizers at every stage, no double-gain, bounded sums | InputChain.cpp, ChannelStrip.cpp, OutputChain.cpp, RoutingMixer.h | Fix param-range/sanitizer dead zones (F012); revisit gain≤0.001→unity (F004); gate 0 dB quirk (F013) |
| Output safety after P12D | **8.5** | Always-armed limiter at 0.97 floor, soft ceiling demoted to emergency, regression-tested against the exact field failure | OutputChain.*, AudioEngineTests 4330-4512 | Close the post-limiter dry/wet mix hole (F003) or document mix=100% as the only guaranteed-ceiling config |
| Latency/PDC safety | **4.0** | Internal compensation excellent (dry delay, bypass lines, tests); **host is never told the plugin latency** | PluginProcessor.cpp, OutputChain.cpp:184-188 | Report `audioEngine.getLatencyNumSamples()` to the host; define dynamic-latency policy; add a test on processor latency |
| RT safety | **8.0** | Policy scan PASS (638 checks), lock-free telemetry, preallocated everything, atomic graph access | audio-thread-policy-scan.txt, AudioEngine.cpp, PedalSignalTelemetry.h | Move engine-toggle command enqueue/logging off the audio thread (F009); bound bypass rebuild contention (F005) |
| Lifecycle/sample-rate safety | **7.5** | Full re-prepare on SR/block change, SR sweep test, robust state-restore fallback | AudioEngine.cpp:58-92, PluginProcessor.cpp:527-544 | Guard `prepare()` against running audio (F006); make auto-heal preserve pedal state (F002) |
| Bypass/dry-wet correctness | **8.0** | Crossfaded, latency-matched bypass; sample-accurate global mix with exact endpoints; thorough unit tests | ProcessorBase.h, DryWetMixer.h, tests 4107-4326 | Crossfade engine on/off transition (F008); end-to-end >8192 fallback test (F010) |
| Diagnostics/telemetry usefulness | **8.5** | Lock-free signal telemetry with alert cooldown, health monitor with silent-output incidents, structured session log, debug snapshots, policy scanner, RT profiling suite | HealthMonitor.h, PedalSignalTelemetry.h, SessionLogger.h, OfflineQADiagnostics.h | Wire unused auto-heal constants or remove; expose dropped-event counts in reports |
| Test credibility | **8.0** | 267/7526/0; behavioral assertions; regression tests tied to field logs; accommodations documented, not fudged | AudioEngineTests.cpp, p12d-tests/test-report.txt | Add host-PDC, auto-heal, true-oversized, and param-range tests; any concurrency smoke coverage |
| Base audio release readiness | **7.0** | Standalone-ready; DAW-readiness blocked on PDC | — | Fix F001; decide F003/F004 policy; then base is RC |

---

## 9. Risk table

| ID | Risk | Severity | Probability | Evidence | Affected files | Recommended next action | Suggested effort |
|---|---|---|---|---|---|---|---|
| P13A-F001 | Host PDC never reported: plugin always claims 0 latency while wet path has ≥2 ms (limiter lookahead) plus pedal latency; timing/comb errors in any DAW mix context | **High** | High (every VST3 session) | No `setLatencySamples` call on `NOVAAudioProcessor` anywhere in Source/; OutputChain reports ~96 samples internally | PluginProcessor.cpp, AudioEngine.cpp | Report `audioEngine.getLatencyNumSamples()` to the host on prepare and on graph publish (async-safe); add test | Claude Sonnet |
| P13A-F002 | Auto-heal graph rebuild recreates pedals with default parameters — live knob settings silently lost after corruption recovery | High consequence / **Medium** net | Low | GraphBuilder builds from `GraphChainNodeSpec` (no param state); state replay exists only in SessionPersistence rebuild path | AudioEngine.cpp:302-310, GraphBuilder.h:188-239, SessionPersistence.h | On heal, route rebuild through session layer or capture/replay pedal states around the internal rebuild; add corruption-injection test | Codex Medium |
| P13A-F003 | Global mix < 100% sums raw dry input with post-limiter wet **after** all output safety; final output not ceiling-protected (only 24.0 hard clamp) | Medium | Medium (any user lowering global mix) | `processWithSampleAccurateDryWet` mixes after `runtime.graph->processBlock` which contains OutputChain | AudioEngine.cpp:557-596, DryWetMixer.h | Decide policy: move mix before OutputChain, or apply a final clamp/ceiling after mixing; document | Codex High |
| P13A-F004 | Line gain ≤0.001 remapped to unity: Level A/B host param at 0 produces full volume with a discontinuity at 0.001 | Medium | Medium | RoutingMixer.h:66; pinned by tests 4762/5202 | RoutingMixer.h, PluginProcessor.cpp:434-446 | Decide: treat 0 as mute (preferred) or restrict param minimum; update pinned tests deliberately | Claude Sonnet |
| P13A-F005 | In-place `graph->rebuild()` on bypass mutates the active graph from the control thread; audio thread can stall on JUCE's graph callback lock | Medium | Low | AudioEngine.cpp:205-235; test 5670 requires in-place semantics | AudioEngine.cpp | Measure stall under bypass toggling at small block sizes; if visible, switch bypass-latency updates to immutable swap or defer rebuild | Codex High |
| P13A-F006 | `AudioEngine::prepare()` callable from message thread while audio callback runs (engine toggle); scratch/delay vectors resized unsynchronized — safe today only because engine is off by ordering | Medium | Low | PluginProcessor.cpp:854-869, 982-1001; AudioEngine.cpp:412-418 | PluginProcessor.cpp, AudioEngine.cpp, DryWetMixer.h | Assert/guard: refuse or defer prepare while `isEngineOn`; document the invariant | Claude Sonnet |
| P13A-F007 | InputChain auto-routing enters mono-copy mode after one detection block, hard channel copy, no crossfade — stereo-image mutation and click risk for asymmetric stereo input | Low | Medium (stereo sources), Low (guitar) | InputChain.h:147-260 (instant Stereo→detected transition at :221-227) | InputChain.h | Add short crossfade on mode change; require confirmation blocks before entering (not just leaving) mono modes | Claude Sonnet |
| P13A-F008 | Engine off/startup = zero-latency dry passthrough; engine on = latent wet — toggle time-shifts audio with no ramp (click/jump) | Low | Medium | AudioEngine.cpp:504-519 | AudioEngine.cpp | Optional: short fade on enable/disable transitions | Claude Fable |
| P13A-F009 | Audio-thread control work on engine-enabled param change: command-queue lock + deque allocation + SessionLogger strings + `triggerAsyncUpdate` from processBlock; also one-shot async trigger on first block | Low | Low | PluginProcessor.cpp:503-521, 551-558; AudioEngine.cpp:184-193, 241-253, 378-385 | PluginProcessor.cpp, AudioEngine.cpp | Defer engine-enable pushes to the control thread (flag + poll), keep processBlock pure | Claude Sonnet |
| P13A-F010 | True oversized-block fallback (>8192 samples, wet-only path) never exercised end-to-end; engine test uses prepared+32 samples | Low | Low | AudioEngineTests.cpp:5399-5444; kMinimumScratchBlocks=8192 | AudioEngineTests.cpp | Add an end-to-end test above scratch capacity asserting wet-only behavior and finiteness | Claude Fable |
| P13A-F011 | Dead safety code: `resetRuntimeGraph` never called; `STARTUP_COUNTER_AUTO_HEAL` and `RECOVERY_COOLDOWN_SHORT` unused (heal uses GRAPH_CHANGE counter) | Low | High (certain, but harmless) | grep: single definition, no call sites | AudioEngine.cpp:442-459, Constants.h:102-104 | Wire up or delete; pick the intended post-heal startup counter | Claude Fable |
| P13A-F012 | Host param ranges exceed sanitizer ranges: input gain −60..+24 vs effective −36..+24; output vol −60..+12 vs effective −36..+12 — bottom 24 dB of knob travel dead; output can never approach silence via the volume knob | Medium | High | PluginProcessor.cpp:427-449 vs InputChain.cpp:3-9, OutputChain.cpp:3-11 | PluginProcessor.cpp, InputChain.cpp, OutputChain.cpp | Align ranges (either widen sanitizers to −60 or narrow params to −36); add range-agreement test | Claude Sonnet |
| P13A-F013 | Gate threshold exactly 0 dB remapped to −100 (off): maxing the gate disables it | Low | Low | GraphBuilder.h:120-123 | GraphBuilder.h | Remove the 0.0 special case or clamp param max below 0 | Claude Fable |

---

## 10. Verdict

**Classification: solid beta — release-candidate for Standalone, not yet for DAW/VST3.**

**Strong:**
- The two-plane architecture with immutable graph swapping is genuinely well executed: atomic graph access, retirement grace windows, order-preserving commands, off-thread builds. This is production-grade structure.
- Output safety after P12D is verified sound: always-armed lookahead limiter with a transparent floor, soft ceiling demoted to emergency duty, regression tests anchored to the actual field failure.
- RT discipline on the steady-state path is excellent and *enforced* (policy scanner with contract checks, lock-free telemetry, preallocated buffers everywhere).
- Internal latency handling (dry-delay compensation, bypass latency lines, in-place latency rebuilds) is consistent and well tested.
- Test corpus is large, behavioral, and honest — P12D accommodations are documented adaptations, not fudges.

**Risky:**
- **F001 (host PDC)** is the one true base blocker for plugin deployment: every DAW session currently runs the plugin time-misaligned. The fix is small and isolated.
- Auto-heal silently resets pedal parameters (F002) — low probability, high user surprise.
- Two policy decisions are being made implicitly and should be made explicitly: post-limiter dry/wet mixing (F003) and the gain≤0.001→unity fallback (F004).

**Not proven:**
- Concurrency behavior (publish/retire, bypass rebuild contention, UI-triggered prepare) has zero automated coverage — currently safe by design and by ordering, not by test.
- Auto-heal end-to-end recovery, the true oversized-block fallback, and host latency reporting are untested.

**Continue to individual effects?** Yes — with one carve-out. Nothing in the base layer invalidates effect-level auditing, and the base signal contract (stereo in, conditioned, zone-ordered, strip-mixed, limiter-protected at mix=100%) is stable. Fix **F001 first** (it is a one-file change with an obvious test), and schedule F002/F003/F004 as policy decisions. Effect-by-effect review can start immediately afterward; recommended order: time-based/stateful pedals first (Delay, Reverb, Neural) since they interact most with the latency and rebuild machinery just audited.
