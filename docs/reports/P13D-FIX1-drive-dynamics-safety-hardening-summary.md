# P13D-FIX1 Drive/Dynamics Safety Hardening Summary

Implemented the small P13D drive/dynamics safety fixes without intentional tone, schema, routing, or UI changes.

Top changes:

1. Boost now has an oversized-block guard before `oversampler.processSamplesUp`.
2. Boost now scrubs NaN/Inf input before stateful DSP and recovers to finite audible output.
3. Noise Gate now scrubs NaN/Inf input and protects detector, lookahead, and gate gain state.
4. Distortion, Fuzz, Compressor, and Noise Gate now use `juce::ScopedNoDenormals` at process entry.
5. P13D test coverage was added for oversized Boost fallback, Boost/Gate recovery, near-silence finite behavior, and nominal finite behavior.

Validation summary:

- SharedCode build: PASS
- Standalone build: PASS
- Base NOVA validation: PASS, 0 failures
- Full NOVA gate: PARTIAL, stopped at out-of-scope OutputChain golden-metrics drift
- RT profile scenarios: PASS, 16/16
- Audio-thread policy scan: PASS
- JSON parse: PASS
- Null-byte scan: PASS
- `git diff --check`: PASS

MetalDistortion was confirmed as legacy alias-only/unregistered and left untouched to avoid migration or source-membership risk.
