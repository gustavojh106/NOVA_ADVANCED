# P12A — Fix Design Summary

One-page summary of `P12A-claude-fix-design-review.md`.

## Question

What is the safest minimal fix for NOVA’s remaining high-gain clipping if the
limiter currently skips at 0 dB?

## Answer

A 5-line behavioural change in `Source/Core/DSP/Global/OutputChain.{h,cpp}`:

1. Remove the `if (isLimiterActiveDb(...))` gate at `OutputChain.cpp:278`.
   Always call `limiter.process()`.
2. Report lookahead latency once in `prepareToPlay`, independent of threshold.
3. Default `targetLimiterDb = -0.3` (was `0.0`). Clamp range `[-12, -0.1]`.
4. Tighten soft ceiling: threshold `0.997`, ceiling `0.9995`. Pure insurance.
5. Clamp master to `[-36, +6] dB` (was `[-36, +12]`).

Do **not** change pedal `outputTrim`, `kProfessionalOutputTrim`, schema, or
graph code.

## Why this is the right shape

- The lookahead path is already implemented and contract-tested. The bug is a
  guard that bypasses it at the factory default.
- The header (`OutputChain.h:14`) already states the intended design — code
  does not match. Fix realigns reality with documented intent.
- When peaks are under threshold, `currentGain = 1.0` → bit-identical output.
  No ducking on clean material. No tone change on fuzz at reasonable Level.
- Soft ceiling stops being the primary safety net (the cause of high-gain
  colouration) and becomes pathological-peak insurance only.

## Cost

- Constant ~2 ms reported latency (88 samples @ 44.1 kHz). Below perceptual
  threshold for monitoring.
- Master upper bound drops by 6 dB. Existing draft presets unaffected
  (all ship master ≤ 0 dB).

## Top 5 risks

1. Latency reporting must become static — host PDC must not retrigger when
   the threshold knob moves.
2. Fuzz `Level = 1` users hear the limiter on peaks. A/B null test required.
3. Test harness needs new high-gain scenario; current P9E proxy is
   under-driven (peak 0.12) and gives false PASS.
4. User-saved presets with master > +6 dB will silently clamp. UX comms.
5. Internal limiter active-gate at `0.9885531` (~ −0.1 dB) must not collide
   with the new default `-0.3 dBTP`. Fixed by separating clamp range.

## Code modification

NO code was modified by this review.
