# P10E High-Gain Mute / Helicopter / Reverb Investigation

Date: 2026-05-18

## Manual WARN Follow-up

User manual listening after P10D reported:

- HighGainAmp -> Modern4x12 sounds very good, with almost no fizz or artifacts.
- Boost -> HighGainAmp -> Modern4x12 produces artifacts, clipping, and constant ground-like noise.
- Distortion -> HighGainAmp -> Modern4x12 can mute after a few seconds; sample-rate reset revives it temporarily, then clipping/helicopter can return.
- Tight Modern Rhythm is not visible as a selectable preset.
- Distortion -> CleanAmp -> Cabinet still has helicopter, clipping, and fizz.
- Fuzz -> ClassicAmp -> Cabinet becomes unintelligible and can later go silent, without clear helicopter/clipping.
- Distortion -> Reverb/Chorus starts good, then helicopter appears; Reverb was suspected.

Manual listening QA general remains pending.
Distortion/high-gain listening QA remains pending.
P7F/Reaper remains pending.

## Chains Investigated

Deterministic P10E scenarios were added in `Source/Core/AudioEngineTests.cpp`:

- `p10e_distortion_highgain_mute_repro`
- `p10e_distortion_cleanamp_helicopter_repro`
- `p10e_distortion_reverb_helicopter_guard`
- `p10e_distortion_reverb_chorus_recovery_guard`
- `p10e_boost_highgain_noise_clipping_guard`
- `p10e_fuzz_classicamp_stuck_silence_guard`
- `p10e_sample_rate_reset_recovers_stuck_chain`
- `p10e_tight_modern_rhythm_availability_doc_check`

Signals include repeated palm mute, staccato/recovery phrases, long active runs, low-E burst, strong chord, and silence-after-phrase. Metrics include active input RMS, final RMS, consecutive silent blocks while input is active, peak, nearClip, clipped, invalid, DC, adjacentDeltaPeak, brightness/fizz proxy, high-frequency proxy, rumble proxy, tail RMS, 3-20 Hz modulation depth, block RMS variance, and reset recovery.

## Root Cause Notes

Likely causes found:

- Boost overdrive: Boost could feed downstream high-gain chains with a very hot stage output. Fix: local Boost output ceiling, not OutputChain.
- Distortion gate/stuck-state: Distortion did not hard-latch to zero in deterministic tests, but Metal/Studio adaptive gate floor and close release were aggressive enough to risk perceived mute/chatter into high-gain chains. Fix: higher floor and slower close release.
- Reverb: Distortion -> Reverb alone stayed bounded after restoring P10D Reverb behavior. The Distortion -> Reverb -> Chorus case shows intended chorus LFO energy in the 3-20 Hz proxy, so the guard treats Chorus differently. Reverb is bounded by recovery/tail/clipping tests but remains a manual listening focus.
- Generic Cabinet clipping: Distortion -> CleanAmp -> Cabinet and Fuzz -> ClassicAmp -> Cabinet clipped after the generic Cabinet stage. Fix: stage-local Cabinet output ceiling, not a global limiter.
- Fuzz silence: Fuzz did not latch to zero after gate floor changes. Its gate previously could close fully; P10E keeps a nonzero closed floor and slower close coefficient.
- Tight Modern Rhythm visibility: the draft preset file is checked as a generated artifact at `Resources/Presets/DraftFactory/generated/Tight-Modern-Rhythm.nova-preset`. DraftFactory output is not automatically seeded into the user preset browser; no APPDATA seeding was added.

## Before / After Metrics

Before final P10E fixes, deterministic failures showed:

- Distortion -> CleanAmp -> Cabinet: peak 1.1065, nearClipSamples 7, clippedSamples 6, adjacentDeltaPeak 0.4814, modulationDepth3To20 0.4044, no stuck mute.
- Fuzz -> ClassicAmp -> Cabinet: peak 3.6543, nearClipSamples 6723, clippedSamples 4799, adjacentDeltaPeak 3.1135, no stuck mute.
- Reverb over-trim experiment failed existing Reverb tests, so it was reverted. Final P10E does not reduce Reverb globally.

After final P10E fixes and guard calibration:

- Base validation PASS: results 237, passes 7227, failures 0.
- Distortion -> HighGainAmp -> Modern4x12: no stuck mute under long active/silence-recovery render; sample-rate reset recovery remains audible and bounded.
- Distortion -> CleanAmp -> Cabinet: final guard passes with peak under 0.99, nearClipSamples 0, clippedSamples 0, invalidSamples 0.
- Boost -> HighGainAmp -> Modern4x12: final guard passes with no nearClip/clipped, tailRms below 0.012, high-frequency proxy below 0.075.
- Distortion -> Reverb: final guard passes with no runaway tail, no invalid samples, no nearClip/clipped, modulation bounded.
- Distortion -> Reverb -> Chorus: final guard passes with no mute, no tail runaway, no nearClip/clipped; LFO modulation is treated separately from Reverb helicopter.
- Fuzz -> ClassicAmp -> Cabinet: after generic Cabinet containment, measured peak was 0.9700, nearClipSamples 0, clippedSamples 0, adjacentDeltaPeak 1.4911; guard passes with fuzz-specific adjacent-delta bound.

## Fixes Applied

- `BoostPedal`: added local post-level soft ceiling (`containBoostOutput`) to prevent overdriving HighGainAmp with destructive peaks.
- `DistortionPedal`: increased Metal/Studio gate floors and slowed gate close release; reduced Studio output trim and output ceiling modestly.
- `FuzzPedal`: added nonzero closed gate floor, slower gate close, and local post-level soft ceiling.
- `CabinetPedal`: added generic Cabinet stage-local soft ceiling to catch true overs after cabinet filtering.
- `ReverbPedal`: no final global revoice; the aggressive experimental trim was reverted to preserve existing Reverb tests and clean/ambient behavior.
- `OutputChain`: unchanged. P10E does not mask high-gain problems with OutputChain limiting.

## Technical Status

Technical status: WARN, improved and bounded for next manual listening.

Reason: deterministic mute/clipping/recovery guards pass, but the user explicitly requires a new manual listening pass before marking high-gain/distortion as PASS.

