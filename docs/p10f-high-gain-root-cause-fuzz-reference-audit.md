# P10F High-Gain Root Cause / Fuzz Reference Audit

## Manual Feedback

After P10E, manual listening still reported a root high-gain issue: some routes feel like the volume drops suddenly to contain the signal. That behavior is unacceptable because it sounds like defensive ducking/auto-attenuation instead of a professional high-gain circuit.

Positive reference:
- Fuzz -> ClassicAmp -> Cabinet works well.
- Fuzz keeps high gain exciting without horrible static/clipping.
- With a low Noise Gate setting around 4%, Fuzz loses most guitar idle static without audible active-note collapse.
- Fuzz is the P10F reference behavior, not a tone target to copy literally.

## Comparative Findings

### Fuzz Reference

Fuzz stages the signal as a musical circuit:
- Input high-pass and pre-contour happen before heavy clipping.
- Gain is split across asymmetrical clipping, square/velcro variants, sag, body/tone filtering, and DC cleanup.
- The gate is internal, envelope-based, and has a non-zero closed floor. It can reduce idle fuzz but does not hard-mute active input.
- Output containment is local and late, but normal operation is already shaped before the final knee.

### Distortion

Distortion had the highest ducking risk:
- Metal/Studio modes used an integrated adaptive gate before saturation.
- Previous high-gain gate thresholds rose with gain/tightness, so active low-level input could be attenuated before the distortion stage.
- Stage 1 gain could become much hotter than Fuzz before later filtering, especially in high-gain modes.
- Final containment was asked to catch too much post-level energy.

Root risk: pre-saturation gate movement and excessive stage-1 push can be perceived as volume collapse, especially before HighGainAmp.

### Boost

Boost had compatibility risk when feeding HighGainAmp:
- Clean boost mode can pass large linear gain before final containment.
- Character saturation is optional, so high boost settings can push the next amp outside its intended input range.
- Final containment prevents hard overs but can make the following amp react to already over-dense signal.

Root risk: the boost should behave like an amp input driver, not just a louder line amplifier.

### HighGainAmp

HighGainAmp -> Modern4x12 is a good baseline, but hot upstream processors exposed its input behavior:
- The amp sag envelope was peak-driven after pre-filter/pre-boost.
- Hot Boost/Distortion input could trigger dynamic sag reduction, creating an audible containment feel.
- The amp needed static input conditioning before sag so destructive input is shaped as preamp drive instead of translated into gain reduction.

Root risk: dynamic sag was doing too much work when the input was already overdriven.

### Cabinet Interaction

Cabinet and Modern4x12 are filtering/cabinet stages, not high-gain rescue limiters:
- CabinetPedal has local containment for true overs.
- Modern4x12 trims and filters high-gain cab output without final clipping containment.
- P10F treats cabinet containment as a last guard only. The fix is upstream gain architecture.

## P10F Metrics

P10F adds deterministic active-input ducking metrics:
- `gainDropDuringActiveInput`
- `blockRmsDropRatio`
- `envelopeDuckDepth`
- `recoveryTimeAfterDuck`
- `activeInputToOutputRmsRatio`
- `consecutiveGainReductionBlocks`
- `outputRmsWhileInputActive`
- `gateGainProxy`
- `ceilingTouchedSamples`

These metrics are intended to detect sudden active-input volume collapse separately from simple clipping/finite checks.

## Root Cause

The bad routes were not primarily an OutputChain problem. The issue was upstream:
- Distortion high-gain modes could reduce signal before saturation through an adaptive gate.
- Distortion stage-1 gain staging could overfeed later stages compared with the Fuzz reference.
- Boost could overfeed HighGainAmp as a line boost rather than a conditioned amp driver.
- HighGainAmp sag reacted dynamically to overly hot input and could sound like defensive attenuation.

Fuzz works because it filters and saturates progressively, keeps its gate floor musical, and sends ClassicAmp/Cabinet a shaped signal instead of a destructive signal that needs late containment.

## Scope Preserved

- No UI/UX work.
- No schema/ID changes.
- No golden baseline update.
- No known-failure additions.
- No OutputChain-only masking.
- No factory preset approval.
- Manual listening QA general remains pending.
- Distortion/high-gain listening QA remains pending.
- P7F/Reaper remains pending.

