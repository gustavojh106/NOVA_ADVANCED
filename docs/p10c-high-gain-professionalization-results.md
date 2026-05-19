# P10C High-Gain Professionalization Results

Date: 2026-05-17

## Summary

P10C added deterministic high-gain diagnostics and applied a minimal local gain-staging fix. Clean-path DSP, UI/UX, schema/IDs, golden baselines, factory approval, Reaper smoke, and manual-listening status were not changed.

Manual listening QA general remains pending. Distortion/high-gain listening QA remains pending. P7F/Reaper remains pending.

## Diagnosis

Clean and ambient paths were already stable. The new high-gain diagnostics reproduced the reported problem as excessive level in high-gain stages rather than invalid samples, DC runaway, or OutputChain limiter dependency.

Before fix:

| Scenario | Peak | RMS | DC | nearClip | clipped | limiter dependency |
|---|---:|---:|---:|---:|---:|---|
| `p10c_high_gain_amp_extreme_gain_bounded` | 2.6788 | 1.2294 | 0.04825 | 17615 | 17222 | none |
| `p10c_distortion_highgainamp_modern4x12_nominal` | 3.2895 | 0.8111 | 0.00060 | 5765 | 5518 | none |

Likely cause: HighGainAmp output lacked enough internal make-down gain for dense saturation, and Modern4x12 wet IR level amplified high-gain material beyond a professional cabinet output range.

## Changes Applied

- `HighGainAmp`: added local post-amp trim `kProfessionalOutputTrim = 0.34`.
- `Modern4x12Cabinet`: added wet-only cabinet compensation `kProfessionalCabinetTrim = 0.70`.
- `AudioEngineTests`: added eight P10C high-gain scenarios and metric proxies.
- `check-audio-thread-policy.ps1`: added `p10c_*` contract checks for docs, scenarios, no schema bump, no golden baseline update, no known-failure bypass, no OutputChain-only masking, no factory approval, and pending manual/Reaper statuses.

## After-Fix Result

`run-base-audio-validation.ps1 -Configuration Debug -Platform x64`:

- `results=222`
- `passes=7094`
- `failures=0`
- `failingResults=0`
- `status=PASS`

For the two failing deterministic windows, the applied trims reduce the measured failing peaks from `2.6788 -> ~0.9108` for HighGainAmp extreme output and `3.2895 -> ~0.7829` for Distortion -> HighGainAmp -> Modern4x12 final cabinet output. The full P10C scenario suite passes with zero invalid samples and no OutputChain limiter dependency in the nominal limiter-independence scenario.

## Clean Impact

No clean-tone processors were changed. Clean Studio, Wide Ambient Clean, Clean Amp, clean chorus/delay/reverb, draft preset validation, golden metrics, and manual listening still need their normal downstream validation gates before release claims. This pass only confirms the Debug base validation suite after the high-gain fix.

## Risks

- The fix intentionally lowers HighGainAmp and Modern4x12 gain staging; draft high-gain preset output trims may need rebalancing in P10D.
- This is a technical proxy pass, not final tone approval.
- Listening may still identify fizz, feel, palm-mute envelope, or cabinet voicing issues that deterministic metrics cannot judge.

## Recommendation For P10D

Run focused manual high-gain listening on the eight P10C chains, then rebalance draft high-gain preset level now that the HighGainAmp/Modern4x12 path is no longer over-hot. Keep Reaper/P7F, factory approval, UI/UX, release packaging, and golden baseline updates separate.

