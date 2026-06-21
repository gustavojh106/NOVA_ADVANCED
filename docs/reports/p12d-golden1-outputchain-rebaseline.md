# NOVA P12D-GOLDEN1 — OutputChain Golden Rebaseline

Date: 2026-06-20

## Scope

This patch updates the stale `output_chain_clean_path / impulse_transparency_line_a` offline diagnostic expectation and its p4 golden case. No OutputChain, limiter, latency, AudioEngine, pedal, amplifier, cabinet, schema, or unrelated test behavior changed.

The p3 baseline was inspected read-only. No active script or test references `docs/golden-metrics/p3-offline-qa-baseline.json`; only historical p3 documentation does, so p3 remains unchanged.

## Assertion semantics

The scenario no longer expects a unity impulse at sample 0. It now verifies that:

- the left and right peak indices equal `AudioEngine::getLatencyNumSamples()`;
- every sample before that reported latency is silent within `1.0e-7`;
- the peak matches the intentional `0.97` limiter ceiling within `1.0e-4`;
- the complete output is finite, bounded by the limiter ceiling, and stereo-consistent within `1.0e-6`.

The scenario notes now describe the intentional master-path limiter/lookahead response rather than clean pass-through.

## Why the new result is intentional

The master OutputChain limiter is always armed at its transparent safety setting. Its ceiling is `0.97` (approximately `-0.265 dBFS`), so a full-scale impulse is intentionally capped at that level. The limiter uses `2.0 ms` of lookahead; at `48,000 Hz`, `0.002 * 48,000 = 96` samples. The diagnostic derives the expected peak index from the engine-reported latency instead of hard-coding `96`.

The dry endpoint remains bit-exact. With `OUTPUT_MIX=0`, `dry_only_null_test` reports `null_rms=0.00000000`, with identical input and output peaks of `0.34999999`.

## Golden metrics: exact old versus new

| Metric | Old p4 baseline | New p4 baseline |
|---|---:|---:|
| `output_peak` | `0.99606001` | `0.97000003` |
| `output_rms` | `0.04413369` | `0.04297803` |
| `peak_index_left` | `0` | `96` |
| `peak_index_right` | `0` | `96` |
| `first_sample_left` | `0.99606001` | `0` |
| `first_sample_right` | `0.99606001` | `0` |

The old note, `Impulse stayed transparent`, changed to `Master-path limiter intentionally applied its reported lookahead latency and 0.97 safety ceiling`.

The baseline was regenerated with `scripts/run-golden-audio-metrics.ps1 -UpdateBaseline`. The generated update reported 74 passing scenarios and zero failures. Incidental metadata and unrelated-case metric refreshes were excluded, leaving only the requested p4 case changed.

## Files changed

- `Source/Core/OfflineQADiagnostics.h`
- `docs/golden-metrics/p4-offline-qa-baseline.json`
- `docs/reports/p12d-golden1-outputchain-rebaseline.md`

## Validation

| Check | Result |
|---|---|
| SharedCode Debug x64 build | PASS — 0 warnings, 0 errors |
| Standalone Debug x64 build | PASS — 0 warnings, 0 errors |
| `scripts/run-base-audio-validation.ps1 -TimeoutSeconds 240` | PASS — 297 results, 12,925 assertions, 0 failures |
| p4 baseline regeneration | PASS — 74 scenarios passed, 0 failed |
| p4 golden comparison after update | PASS — baseline accepted, 0 coverage gaps |
| p3 active-gate inspection | PASS — no active consumer; unchanged |
| Schema version | Unchanged |
| `git diff --check` | PASS |
| Audio-thread policy scan | Expected contract-layer FAIL; separated below |

### Policy scan separation

`scripts/check-audio-thread-policy.ps1` completed with scanner status `FAIL`: 595 contract checks and 23 contract failures. The underlying direct audio-thread `failures` array is empty.

- Expected for P12D-GOLDEN1: 21 historical scope contracts reject any golden-baseline edit, including `p9f_golden_baseline_updated` and `p9g_golden_baseline_updated`. This task is explicitly the authorized baseline rebaseline.
- Pre-existing and outside this patch: `p9f_audio_path_behavior_file_changed` and `p9g_audio_path_behavior_file_changed` detect already-dirty `Source/Core/Audio/GraphBuilder.h`, `Source/Core/AudioEngine.cpp`, and `Source/Core/AudioEngine.h`. Those files were modified before this task began and were not touched here.
- P9F’s stored technical report still passes with zero limiter, near-clip, clipped, and invalid-sample telemetry for all six draft presets.

## DSP behavior confirmation

No DSP implementation changed. This is an expectation and baseline correction for behavior already present before P13E: always-armed limiting, a `0.97` safety ceiling, and engine-reported lookahead latency.
