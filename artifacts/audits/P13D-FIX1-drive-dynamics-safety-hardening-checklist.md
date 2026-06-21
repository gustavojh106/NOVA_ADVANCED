# P13D-FIX1 Drive/Dynamics Safety Hardening Checklist

- [x] Add Boost oversized-block guard before `oversampler.processSamplesUp`.
- [x] Keep Boost fallback realtime-safe: no allocation, resize, lock, logging, or host API calls in fallback.
- [x] Add Boost NaN/Inf input scrub before filters, oversampler, and waveshaper.
- [x] Add Boost recovery test after NaN/Inf abuse.
- [x] Add Boost oversized-block test coverage.
- [x] Add Noise Gate NaN/Inf input scrub before detector/lookahead/gate state.
- [x] Add Noise Gate detector/lookahead/gateGain finite recovery guards.
- [x] Add Noise Gate recovery test after NaN/Inf abuse.
- [x] Add `juce::ScopedNoDenormals` to Distortion process entry.
- [x] Add `juce::ScopedNoDenormals` to Fuzz process entry.
- [x] Add `juce::ScopedNoDenormals` to Compressor process entry.
- [x] Add `juce::ScopedNoDenormals` to Noise Gate process entry.
- [x] Confirm MetalDistortion is legacy alias-only/unregistered.
- [x] Leave MetalDistortion untouched to avoid migration/source-membership risk.
- [x] Add near-silence finite tests for Distortion, Fuzz, Compressor, and Noise Gate.
- [x] Add nominal finite input behavior tests for Boost and Noise Gate.
- [x] Run SharedCode build.
- [x] Run Standalone build.
- [x] Run base NOVA validation.
- [x] Attempt full NOVA validation.
- [x] Run RT profile scenarios separately after full gate stopped.
- [x] Run audio-thread policy scan.
- [x] Parse JSON findings.
- [x] Run null-byte scan.
- [x] Run `git diff --check`.

Deferred/out of scope:

- [ ] Resolve out-of-scope OutputChain golden-metrics drift: `output_chain_clean_path / impulse_transparency_line_a`.
