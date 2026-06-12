# P13B — Stateful / Time-Based Effects — Manual Listening & Validation Checklist

Run in Standalone (debug) unless a step says VST3. Keep **global mix = 100%** (F003), never use line gain 0 as silence (F004), avoid conclusions from extreme-minimum input/output knob positions (F012).

## A. Delay

- [ ] **L1 — Clean guitar → Delay** (Analog, time 480 ms, fb 0.46, mix 0.32)
  - [ ] Repeats clean, no zipper while turning Time (pitch-slew glide is intentional)
  - [ ] Stereo spread audible with Spread > 0.4; collapses gracefully to mono input
- [ ] **L2 — High gain → Delay** (Distortion or HighGainAmp before Delay, fb 0.75–0.97)
  - [ ] No level runaway at max feedback over 60 s sustained playing
  - [ ] Duck > 0.5: repeats clear space under playing, bloom back after
  - [ ] Freeze: engage mid-repeat — bed holds stable ≥60 s, no creep past ~unity, disengage is clean
  - [ ] Reverse mode: bloom arrives later than input, no clicks at window boundaries
- [ ] **Tempo sync:** sync on, change host/standalone BPM — time follows, no glitch burst

## B. Reverb

- [ ] **L3 — Clean → Reverb** (Hall, then Plate, decay 0.7)
  - [ ] Smooth decaying tail, no metallic ringing, no low-end thump (DC) after stopping
- [ ] **L4 — High gain → Reverb** (Shimmer then Cloud, decay 0.9)
  - [ ] Shimmer octave content musical, not screeching; feedback stable over 60 s
  - [ ] Freeze: pad holds stable ≥60 s; un-freeze decays naturally
  - [ ] Gate > 0.7: tail clamps after stopping; no chatter
- [ ] **Mode sweep:** step through all 6 modes while a tail rings — note any clicks (feeds F-P13B-008)

## C. Neural

- [ ] Clean and hot input: drive sweep 0→100, no harshness jumps, level compensation holds
- [ ] Mix at exactly 0: output nulls against bypass (transparency snap path)
- [ ] CPU: watch meter with 2× Neural instances at 64-sample block (F-P13B-006 baseline)

## D. Bypass / Tails (F-P13B-005 evidence)

- [ ] **L5 — Bypass mid-tail** on Delay and Reverb
  - [ ] Truncation is a clean ~10 ms fade, no click/pop
  - [ ] Un-bypass: no stale tail jumps back in (state reset working)
  - [ ] Judge: is truncation acceptable, or is a "trails" mode needed? → record decision
- [ ] **L6 — Preset switch mid-tail:** gap acceptable? clicks?
- [ ] **L8 — Remove pedal mid-tail** (drag out of chain): clean gap vs click (F-P13B-009)

## E. Automation Stress

- [ ] **L7 — Rapid automation:** continuously sweep Delay time + feedback, and Reverb size + mode, 30 s each
  - [ ] No NaN/dropout/auto-heal events (check session log for `processor.latency` / heal entries)
  - [ ] Note zipper on Reverb Size (decides F-P13B-008)

## F. Sample Rate / Block Size

- [ ] **L9 —** Switch 44.1k ↔ 48k ↔ 96k and block 64 ↔ 1024 in Standalone audio settings with active tails
  - [ ] Recovery clean each time, no stuck/silent chain (regression guard for p10e)
  - [ ] Delay time and reverb decay character consistent across rates

## G. Host PDC (VST3 — carries P13A pending item)

- [ ] **L10 —** Build Release VST3. In REAPER (or Live):
  - [ ] Duplicate a DI track; NOVA with Neural enabled on copy A, bypassed-in-NOVA on copy B; invert B
  - [ ] Phase-null confirms host PDC tracks Neural's oversampler latency through bypass toggles
  - [ ] Add/remove Neural during playback — PDC updates without audio-thread glitch

## H. Robustness (debug-only, destructive)

- [ ] **L11 —** Inject NaN upstream of Chorus (debug hook) → engine auto-heal recovers within ~2 blocks; confirms F-P13B-002 priority pre-fix

## Sign-off

| Section | Result | Notes |
|---|---|---|
| A Delay | ☐ pass / ☐ fail | |
| B Reverb | ☐ pass / ☐ fail | |
| C Neural | ☐ pass / ☐ fail | |
| D Bypass/Tails | ☐ pass / ☐ fail | trails decision: |
| E Automation | ☐ pass / ☐ fail | F-008 verdict: |
| F SR/Block | ☐ pass / ☐ fail | |
| G Host PDC | ☐ pass / ☐ fail | |
| H Robustness | ☐ pass / ☐ fail | |
