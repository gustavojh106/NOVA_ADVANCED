# P12A — OutputChain Limiter / Headroom Fix: Design Review

Scope: critical evaluation of the proposed fix direction for the high-gain WARN
(clipping at high volume and high gain). No code changes. Audio-engineering
trade-offs only. Pulls evidence from current sources.

Reviewer: Claude (independent senior-engineer pass).
Status: REVIEW ONLY — no code modified.

---

## 1. What the bug actually is

`OutputChain.cpp:278` gates the lookahead limiter behind
`isLimiterActiveDb(currentLimiterDb)`. The "active" predicate
(`OutputChain.cpp:28-31`) returns true only when the threshold sits below
`-0.0001 dB`. Default factory state = `0 dB` ⇒ predicate false ⇒
`limiter.process()` is NEVER called. The lookahead-delay tap, true-peak
estimator, hold/release, and threshold smoother all sit idle.

The only protection on the default path is `applyFinalSoftCeiling()`
(`OutputChain.cpp:33-54`):
- threshold = `0.985` (~ −0.13 dBFS)
- ceiling   = `0.999`
- knee      = `0.014` linear (~ 0.12 dB) shaped with `tanh`.

That knee is too narrow for sustained high-gain content. tanh above the knee
gives continuous saturation, not transparent limiting — audible as the WARN.

This contradicts the file’s own header (`OutputChain.h:14`): *"Stable latency:
the lookahead delay is always active and latency is reported once."* The header
states the design intent the code does not implement. So part of the fix is
just making code match its own contract.

## 2. Headroom budget today

Worst-credible signal path before OutputChain:

- HighGainAmp stage sum bound: `0.12 + 0.25 + 0.35 + 0.52 = 1.24` (linear,
  pre-trim) — `HighGainAmp.h:177`.
- Pro trim: `kProfessionalOutputTrim = 0.32` (`HighGainAmp.h:298`).
- Master inside amp: `0..2` (`HighGainAmp.h:24`).
- Worst case from amp alone: `1.24 × 0.32 × 2.0 ≈ 0.79`.
- Cabinet (Modern4x12) adds presence shelf up to `+4.7 dB`
  (`HighGainAmp.h:262`, same shape in cabinets) ⇒ realistic transient peaks
  comfortably exceed 1.0 before OutputChain sees the signal.
- Fuzz hard-clamped internally to `±1.25` (`FuzzPedal.h:568`), with
  `outputTrim ≈ 0.88–0.98` and user-`Level 0..1` ⇒ fuzz can deliver true peaks
  up to ~1.25 if `Level = 1`.
- ChannelStrip `gain ∈ [0, 2]` (`ChannelStrip.cpp:10`) — another up-to-6 dB.
- OutputChain master allowed range `-36..+12 dB` (`OutputChain.cpp:10`).

Conclusion: the chain admits ≥ +24 dB of accumulated nominal makeup gain
before the safety ceiling, with the safety ceiling effectively absent at the
factory default. The clipping is not a bug in any one stage — it is a
system-level headroom mis-budget.

## 3. The four proposed fix directions — evaluated

### 3.1 Limiter always-on at 0 dB (i.e. threshold ~ −0.3 dBTP, never bypassed)

Verdict: **YES, this is the load-bearing change. Do this first. Smallest
audible footprint.**

Engineering arguments for:
- The lookahead path (2 ms, ~88 samples @ 44.1 kHz, `OutputChain.h:254`,
  `:259`) is already implemented, sample-accurate, with linked-stereo
  Catmull-Rom true-peak estimation (`:269-292`) and program-dependent release
  (`:334-341`).
- Internally the limiter ALREADY has a per-sample guard at
  `thresholdLinear < 0.9885531` (~ −0.1 dB, `OutputChain.h:218`, `:361`).
  That means "always calling `limiter.process()`" with a threshold of e.g.
  −0.3 dBTP costs exactly: the lookahead delay + the per-sample comparison.
  When peaks stay under threshold, `currentGain` stays at 1.0 — bit-identical
  output, no ducking.
- Fixes the documented-but-unimplemented "stable latency" intent.
- Removes the soft-ceiling from being the primary safety net, which is the
  actual cause of high-gain colouration.

Engineering arguments against / risks:
- **Latency reporting changes.** `updateReportedLatencyForLimiter`
  (`OutputChain.cpp:172-175`) currently reports 0 or `lookaheadSamples` based
  on the threshold. If the limiter becomes always-on, `setLatencySamples()`
  becomes a constant ~88 samples (~2 ms @ 44.1 kHz). DAW PDC will
  re-negotiate exactly once on next prepare. Acceptable. Critically: latency
  must STOP being a function of the threshold knob — otherwise toggling the
  knob retriggers host PDC and causes audible glitches on some hosts.
- **Constant 2 ms latency** on the Standalone path is fine for monitoring; a
  live player will not perceive it. For studio/DAW reamping this is below the
  threshold of audibility.
- **Threshold sweet spot.** −0.3 dBTP (~0.966 linear) leaves enough headroom
  for the true-peak estimator to avoid inter-sample overs. −0.1 dB
  (`0.9885531`) is too tight — equals the internal active gate; better to
  separate the "always-on" mode from the per-sample gate to avoid
  jitter at the boundary.
- **Reset / preload.** With always-on lookahead, every `reset()` and graph
  rebuild must pre-roll the delay buffer with silence (already true:
  `OutputChain.h:172-173`) and `currentGain = 1.0f` (`:167`). Confirmed safe.

Bottom line: this single change closes the WARN root cause. Cost = 2 ms
constant reported latency. No tone change when peaks are clean.

### 3.2 Master gain range changes

Current range −36..+12 dB. Question: should the upper bound drop?

Verdict: **NARROW the master range, but only modestly. Don’t over-correct.**

- Once the limiter is always-on, +12 dB master on a hot amp+cabinet output
  (≈0.8–1.0 peak nominal) translates into ~12 dB of sustained gain reduction.
  That is the "ducking" failure mode the user wants to avoid.
- The right ceiling depends on the worst sane preset peak entering OutputChain
  with master at 0 dB. Empirically (P9E reports: peak ≤ 0.57 on
  Classic Crunch, 0.16 on Tight Modern Rhythm) the headroom budget can absorb
  6–8 dB of post-limiter make-up before the limiter does audible work.
- Recommended: clamp master to `[-36 dB, +6 dB]`. Existing presets are
  unaffected (none ship with master > 0 dB in the draft bank). User-saved
  presets that exceed +6 will clamp on `sanitizeOutputVolumeDb`; this is a
  silent UX change but not a state-schema change.
- Do NOT drop below +6 dB without measuring: low-output guitars (single-coil
  passive) need real make-up. Going to 0 dB max as a "safety" choice removes
  legitimate utility and pushes users to overdrive amp masters instead —
  which is the bad-gain-staging trap.

### 3.3 Soft ceiling knee changes

Current: threshold 0.985, ceiling 0.999, knee 0.014.

Verdict: **WIDEN the knee, but ALSO move it after the limiter, not in
parallel with it. Treat the soft ceiling as oh-shit insurance only.**

- With the lookahead limiter always-on at e.g. −0.3 dBTP, the soft ceiling
  should see signal whose peak rarely exceeds threshold. The current narrow
  knee will essentially never engage — that is good.
- However, parameter-automation transients and graph-swap blocks can still
  poke above 1.0 linearly. The soft ceiling exists for that. Today’s
  threshold 0.985 means the soft ceiling can fight the limiter when limiter
  threshold is also near full-scale. Move soft-ceiling threshold to ~0.997
  and ceiling to ~0.9995 so it only ever acts on overs that escaped the
  limiter — i.e. NaN-recovery, denormal sticks, true edge cases.
- Knee widening (e.g. threshold 0.85 → ceiling 0.99) would compound with the
  amp's tanh stages and audibly soften transients. Reject this approach.
- The right framing: limiter handles musical peaks, soft-ceiling handles
  pathological peaks. Don’t blur their roles.

### 3.4 Chain-level headroom trims (per-pedal / per-amp output)

Verdict: **DON’T touch pedal `outputTrim` values to compensate for the limiter
choice. That is fake safety.**

- `HighGainAmp::kProfessionalOutputTrim = 0.32` and fuzz `outputTrim 0.88–0.98`
  are voiced numbers chosen for tone. Re-tuning them to "make the limiter not
  duck" trades a real audible attribute (amp/fuzz character) for the appearance
  of a clean meter. The user explicitly flagged this as a risk and it is the
  right risk to call out.
- The exception: if a SPECIFIC pedal+amp combination still ducks the limiter
  >3 dB on sustained content with limiter at −0.3 dBTP and master at 0 dB,
  that pedal/amp is misvoiced — fix at the source, not via the limiter.
- The P9E suite never exercised true high-gain at high level (proxy peak =
  0.12 — `p9e-draft-preset-gain-staging-report.txt:16`). New scenarios are
  required before any per-pedal trim is touched.

## 4. Side-effect risks (independent of which fix path is chosen)

1. **Latency-reporting flip-flop.** Today, twisting the limiter knob across
   0 dB changes reported latency. Hosts re-PDC mid-session. Whichever fix
   ships, `setLatencySamples` must be called ONCE at `prepare()` and must not
   depend on the threshold value.
2. **Preset compatibility.** No schema bump required for any of these
   changes if (and only if) `sanitizeLimiterDb` / `sanitizeOutputVolumeDb`
   absorb out-of-range legacy values. Both already clamp
   (`OutputChain.cpp:3-21`). Safe.
3. **Telemetry must keep working.** `DebugSnapshot` is what tests assert on
   (`OutputChain.h:27-38`). Always-on limiter means `limiterTouchedSamples`
   and `limiterActiveBlocks` become useful metrics rather than zero-by-design.
   Test thresholds will need updating (cheap-model work).
4. **Hardware-bypass crossfade interaction.** `ProcessorBase` bypass crossfade
   is upstream of OutputChain, so unaffected.
5. **Stereo-link.** Limiter is already linked (`estimateLinkedTruePeak`,
   `OutputChain.h:269-292`). No new image-shift risk.

## 5. Safest minimal fix (the answer to the brief’s question)

In priority order, do exactly these and stop:

1. **Make the limiter call unconditional.** Remove the
   `if (isLimiterActiveDb(...))` gate at `OutputChain.cpp:278`. Always call
   `limiter.process(buffer, limiterThresholdLinearSmooth)`.
2. **Decouple latency reporting from the knob.** In `prepareToPlay`, call
   `setLatencySamples(limiter.getLookaheadSamples())` unconditionally. Delete
   `updateReportedLatencyForLimiter`’s threshold-dependent branch.
3. **Change the default `targetLimiterDb` from `0.0f` to `-0.3 dB`.** Keep
   `sanitizeLimiterDb` range `[-12, 0]` unchanged. Optional: change to
   `[-12, -0.1]` so the knob cannot be parked in a state where the threshold
   sits exactly at the internal gate (`0.9885531`).
4. **Tighten the soft ceiling.** Threshold `0.997`, ceiling `0.9995`. Keep
   tanh shape. This makes the soft ceiling pure insurance.
5. **Clamp master to `[-36, +6] dB`.** Silently clamp legacy presets via the
   existing sanitizer.

What this fix deliberately does NOT do:
- Does not change `kProfessionalOutputTrim` or any pedal `outputTrim`.
- Does not modify `RuntimeGraphManager`, `AudioEngine` graph swap, schema, or
  any pedal `processBlock` body.
- Does not introduce a new global gain stage anywhere.

## 6. What stays broken after this fix

- Manual listening QA still NOT_RUN for all 6 drafts
  (`results-template.csv`). The fix is auditable only after that runs.
- P9E "high_gain_staccato_proxy" scenario peak is 0.12 — does not exercise
  the failure. A new scenario at realistic input level (e.g. pink noise +
  chord transient at −6 dBFS into HighGainAmp drive=10, Modern4x12,
  master = 0 dB) must assert `limiterActiveBlocks > 0` AND
  `softCeilingTouchedSamples == 0`.
- Master-gain UX: dropping the ceiling from +12 to +6 dB needs a comms note
  for any user presets that relied on the extra headroom.

## 7. Pre-merge checklist (see also the checklist artifact)

- Latency reported once at prepare, independent of knob. Verified by host
  PDC delta across knob sweep = 0.
- `currentGain == 1.0` when input peaks ≤ threshold. Bit-identical output on
  clean material. Verified by null-test against pre-fix build with limiter
  forced bypass.
- High-gain scenario: limiter `maxReductionDb` ≤ 6 dB on a worst-credible
  preset at master 0 dB.
- Fuzz reference: A/B against pre-fix on a fuzz-only preset at `Level = 1`.
  Output character difference < perceptual threshold (limiter touches transients
  only, not sustain).
- Soft ceiling `softCeilingDeltaPeak < 1.0e-4` on all PASS draft presets.
- No new allocations / locks in `processBlock`. Re-run `audio-thread-policy-scan`.

## 8. Verdict

The safest minimal fix is a 5-line behavioural change: always call the limiter,
always report its latency, set its default just below 0 dB, tighten the soft
ceiling to pure insurance, and narrow the master upper bound. No tone
re-voicing, no pedal-level edits, no schema work. Every other proposed
direction either changes tone (chain trims, knee widening) or papers over the
gain budget without fixing it.
