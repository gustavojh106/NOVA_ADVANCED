# P13A — Base Audio Platform Checklist (post-P12D)

Derived from `docs/reports/P13A-base-audio-platform-audit-report.md` and `artifacts/audits/P13A-base-audio-platform-findings.json`.

## Base blockers (fix before DAW/VST3 release)

- [ ] **F001 — Report plugin latency to host.** Call `setLatencySamples(audioEngine.getLatencyNumSamples())` on `NOVAAudioProcessor` at `prepareToPlay` and on every graph-publish latency change (message-thread safe). Decide dynamic-latency policy (live value vs stable max). Add test asserting processor latency equals graph latency and is > 0 with no pedals (OutputChain lookahead).

## Before effect-by-effect audit (policy decisions + cheap correctness)

- [ ] **F012 — Align parameter ranges with sanitizers.** Input gain (−60..+24 param vs −36..+24 effective) and output volume (−60..+12 vs −36..+12). Pick one direction; add a range-agreement test.
- [ ] **F004 — Decide line-gain-zero policy.** `RoutingMixer` maps gain ≤0.001 → unity. Either make 0 = mute (continuous) or restrict the parameter minimum; update pinned tests (4762, 5202) deliberately.
- [ ] **F003 — Decide global-mix safety policy.** Mix < 100% sums raw dry with post-limiter wet; final output unprotected below the 24.0 hard clamp. Move mix pre-OutputChain, add post-mix ceiling, or document mix=100% as the guaranteed-ceiling configuration.
- [ ] **F002 — Auto-heal must preserve pedal state.** Capture/replay pedal states around the HealthMonitor-triggered rebuild (or route heal through the session layer). Add a corruption-injection test.
- [ ] **F013 — Remove the gate `== 0.0 dB → off` special case** in `GraphBuilder::applyRuntimeParamsToGraph`.
- [ ] **F011 — Delete or wire dead safety code**: `resetRuntimeGraph`, `STARTUP_COUNTER_AUTO_HEAL`, `RECOVERY_COOLDOWN_SHORT`.

## Later improvements (not blocking)

- [ ] **F005 — Bound bypass-rebuild contention.** Measure audio-thread stall from in-place `graph->rebuild()` during rapid bypass toggles at 32/64-sample blocks; if visible, move to immutable swap or lock-free latency handoff.
- [ ] **F006 — Guard `AudioEngine::prepare()` against running audio.** Assert/defer when engine is on; document the "engine off during UI re-prepare" invariant.
- [ ] **F009 — Keep processBlock pure on all branches.** Defer engine-enable command enqueue + logging to the control thread; remove the first-block `triggerAsyncUpdate` path.
- [ ] **F007 — Soften auto-routing transitions.** Confirmation blocks before entering mono-copy mode; crossfade the channel copy.
- [ ] **F008 — Optional fade on engine enable/disable** to mask the latency/time jump.
- [ ] **F010 — End-to-end oversized-block test** (>8192 samples) exercising the wet-only fallback through `AudioEngine::process`.
- [ ] Add a host-PDC regression test once F001 lands (latency change on pedal add/remove/bypass propagates or is policy-clamped).
- [ ] Consider seqlock-style snapshot for `RuntimeParameterSnapshot` if torn multi-field reads ever matter (currently benign).

## Manual validation needed (cannot be proven by unit tests)

- [ ] **DAW timing check after F001**: record a parallel dry track in Reaper/Live/Cubase, verify null (or documented offset) against the plugin at mix=100% with and without oversampled pedals.
- [ ] **Bypass-toggle stress under load**: small buffer (32/64), heavy chain, rapid bypass automation — listen/measure for dropouts from the in-place rebuild lock (F005).
- [ ] **Engine toggle while audio runs** in a host with automation on the Engine parameter — confirm no glitch/crash from the UI-side `prepare()` path (F006) and assess the audibility of the time jump (F008).
- [ ] **Auto-heal field behavior**: force corruption (debug hook) in Standalone, confirm recovery, observe pedal-state loss (F002) before/after fix.
- [ ] **Stereo input material** through the input stage (e.g., synth in FX-loop use case): verify auto-routing does not collapse or click on asymmetric stereo (F007).
- [ ] **Knob-feel check** of input gain / output volume bottom range after F012 (dead travel today).
- [ ] **Host transport/tempo edge cases**: tempo changes mid-playback propagate to tempo-synced pedals within the 8-block poll window without artifacts.
- [ ] Sample-rate switch (44.1 → 96 kHz) mid-session in a host: confirm clean re-prepare, no stuck silence, limiter lookahead rescales.

## Recommended next audit target

P13B: effect-by-effect audits, starting with stateful/time-based pedals — **Delay, Reverb, Neural** — since they interact most with the latency, bypass, and rebuild machinery validated here.
