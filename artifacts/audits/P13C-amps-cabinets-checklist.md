# P13C — Amps & Cabinets — Manual Listening & Validation Checklist

Run in Standalone (debug) unless noted. **Global mix = 100%**, line gain 1.0, avoid extreme-minimum knob positions for tone judgments (F012).

## A. Core amp → cab pairs

- [ ] **L1 — CleanAmp → Cabinet (Atlas)** (drive 0.25→0.8 sweep, reverb 0.2/0.6)
  - [ ] High headroom stays clean; reverb return audible but bounded; no pumping from the 1.25 soft limit
- [ ] **L2 — ClassicAmp → Cabinet** (drive 2.8→6.0, sag 0.45/0.9)
  - [ ] Sag feel musical, no volume collapse; bright+presence maxed not harsh through cab LP
- [ ] **L3 — HighGainAmp → Modern 4x12** (drive 5.5→10, tight 0.5/0.85)
  - [ ] Chunk tight, fizz controlled (post-presence LP working)
  - [ ] **L11:** noise floor between notes — gate floor (0.07–0.18) breathing/hiss modulation audible?
- [ ] **L6 — Fuzz → ClassicAmp → Cabinet**
  - [ ] Fuzz cleans up with guitar volume; no helicopter/stutter (p10e regression by ear)

## B. Stacked gain chains

- [ ] **L4 — Boost → HighGainAmp → Modern 4x12**: tightness, no mush, no sustained limiter engagement
- [ ] **L5 — Distortion → HighGainAmp → Modern 4x12**: stacked noise floor acceptable; palm mutes don't flub

## C. Gain staging extremes

- [ ] **L7 — High master preset:** ClassicAmp and BoutiqueAmp at level ≈2.0, shelves boosted, into each cab
  - [ ] Does the OutputChain limiter engage sustained (0.999-style pinning)? GR audible as pumping? → feeds F-P13C-006 decision
- [ ] **L8a — Low input / high output** and **L8b — High input / normal output**
  - [ ] Staging symmetric, no surprise noise-floor jump (respect F012 dead zones)
- [ ] **L9 — Palm muting, high gain:** low-end tight vs flubby; sag pump character
- [ ] **L10 — Sustained notes, high gain:** smooth bloom/decay; HGA gate does not chop sustain tails

## D. Finding-specific checks

- [ ] **L12 — BoutiqueAmp edge-of-breakup** (drive ~1.2, touch 0.55, medium picking)
  - [ ] Crackle/graininess when notes straddle the breakup threshold? → confirms/clears F-P13C-007
- [ ] **L13 — SR consistency:** same ClassicAmp/ChimeAmp/BoutiqueAmp preset at 44.1k vs 96k
  - [ ] Sag/touch/compression feel noticeably different? IR tail brightness different? → confirms/clears F-P13C-004
- [ ] **Vintage 2x12 hot test:** HighGainAmp (level 1.5) → Vintage2x12 (warmth +12, level 2.0)
  - [ ] Peaks vs Atlas/Modern at same settings; clipping audible before OutputChain? → feeds F-P13C-006
- [ ] **Cabinet first-block check:** add cabinet mid-playback — brief EQ-only tone audible or masked? (F-P13C-009, expect masked)

## E. Robustness (debug-only, after P13C-FIX1 lands)

- [ ] NaN injection upstream of each unguarded amp → no persistent silence (HighGainAmp gate case), recovery within heal window
- [ ] Host block > prepared block (offline render with larger buffer) → amps fall back dry, no crash

## F. Cross-checks with prior phases

- [ ] OutputChain limiter independence: high-gain chain output stable with limiterDb 0 vs −6 (mirror of p10c test, by ear)
- [ ] Bypass each amp mid-sustain: 10 ms crossfade clean, latency-matched dry (no comb/flam)
- [ ] DAW PDC (Release VST3, carry-over): null test with amp bypassed in-plugin vs DAW-bypassed

## Sign-off

| Section | Result | Notes |
|---|---|---|
| A Core pairs | ☐ pass / ☐ fail | |
| B Stacked chains | ☐ pass / ☐ fail | |
| C Gain extremes | ☐ pass / ☐ fail | F-006 decision: |
| D Finding checks | ☐ pass / ☐ fail | F-004: · F-007: |
| E Robustness | ☐ pass / ☐ fail | (post-FIX1) |
| F Cross-checks | ☐ pass / ☐ fail | |
