# P8B DistortionPedal Focused QA Results

Date: 2026-05-07

## Summary

P8B verifies the P8A DistortionPedal containment in real-use style chains without changing DSP, routing, UI, state schema, presets, OutputChain, Reverb, Chorus, or golden baselines.

P8A found that DistortionPedal could deliver destructive post mix/level energy, especially in Metal mode. P8A added a final post-level soft ceiling at 0.995f, a 10 Hz final DC blocker, and a second post-DC containment pass. P8B keeps that surgery intact and adds focused deterministic coverage around nominal expressiveness, sweeps, downstream Amp/Cabinet use, ambience recovery, and bypass/unbypass behavior.

## What Was Verified

- DistortionPedal processBlock still has no logging, juce::String, DBG, or obvious allocation patterns.
- P8A containment markers remain present in DistortionPedal.
- mix=0, mode signature, automation stress, round-trip state, and P8A Reverb/Chorus recovery coverage remain part of base validation.
- New P8B tests exercise realistic synthetic guitar signals and real-use downstream chains.

## Tests Added

- `P8B Distortion nominal modes remain expressive below containment knee`
- `P8B Distortion level mix gain sweep remains bounded and audible`
- `P8B Distortion into amp and cabinet remains bounded`
- `P8B Distortion bypass unbypass stays click bounded`

The existing P8A ambience regression test remains the primary Distortion -> Reverb -> Chorus recovery guard:

- `P8A Distortion Reverb Chorus bypass recovery stays bounded`

## Bounded Output Measurement

The P8B tests measure peak, RMS, DC, invalid samples, clipped samples, and near-clip samples using deterministic block-window metrics. Nominal mode tests require peaks below the containment knee region and no near-clip dominance. Sweep and downstream-chain tests require finite output, bounded peaks, no clipped samples, no DC accumulation, and non-silent output.

Base validation final result:

- `results=208 passes=6633 failures=0 failingResults=0`

This is above the P8A reference of `results=204 passes=6614 failures=0 failingResults=0`.

## Over-Containment Check

No DSP changes were made in P8B. The nominal-mode test checks that Studio, Classic, and Metal remain observably distinct below the containment knee. This is a technical signature check, not a substitute for manual listening QA.

## Policy Checks Added

- `p8b_distortion_focused_qa_doc_present`
- `p8b_distortion_manual_qa_checklist_present`
- `p8b_distortion_real_use_test_present`
- `p8b_distortion_processblock_no_logging_or_string`
- `p8b_distortion_processblock_no_obvious_allocation`
- `p8b_no_known_failure_ignore_added`
- `p8b_no_golden_baseline_update`

## Validation

Full P8B validation run:

- NOVA_SharedCode Debug x64: PASS, 0 warnings.
- NOVA_SharedCode Release x64: PASS, 0 warnings.
- NOVA_StandalonePlugin Debug x64: PASS, 0 warnings.
- NOVA_StandalonePlugin Release x64: PASS, 0 warnings.
- NOVA_VST3 Release x64: PASS, 0 warnings.
- git diff --check: PASS.
- Base validation run 1: PASS, `results=208 passes=6633 failures=0 failingResults=0`.
- Base validation run 2: PASS, `results=208 passes=6633 failures=0 failingResults=0`.
- Golden metrics: PASS against P4 baseline, no baseline update.
- RT profile Release: PASS, `16/16/0/0`.
- RT stability Release: PASS, all scenarios `3/0/0`.
- Policy scan: PASS, `failures=0 warnings=0 contractFailures=0 contractChecks=194`.
- Wrapper Fast Release: PASS.
- Diagnostics bundle: PASS.

No golden baseline updates, no known failures, and no P8B DSP/audio-path changes were made.

## Risks Remaining

- The tests use deterministic synthetic guitar input, so final tone judgment still needs manual guitar DI listening.
- Amp/Cabinet coverage uses public processor defaults to avoid reaching into private controls.
- Containment activation is inferred through bounded-output behavior; no invasive audio-thread instrumentation was added.

## Recommendation For P8C

Proceed to the next focused QA phase only if full validation remains green and manual listening does not report loss of character, excessive dullness, pumping, rumble, or slow ambience recovery.
