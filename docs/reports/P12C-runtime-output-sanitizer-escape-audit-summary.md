# P12C Output Sanitizer Escape - Summary

Code modified: **No** (audit only).

## Verdict

P12B never shipped. `master` OutputChain still treats `limiterDb = 0` as limiter bypass, not transparent safety. Every default path (host parameter, ValueTree, preset, SessionStore, tests) ships `limiterDb = 0`. With `outputVolumeDb = +10.73 dB` and `ChannelStrip gainB = 2`, hot chain signal is amplified above 1.0 and pinned at the 0.999 `applySoftCeiling` ceiling, producing the observed sustained `peakLMax = 0.999` and `nearClipSamples` spikes >100k per 2 s window. There is no runtime/preset/UI path "escaping" the sanitizer; the sanitizer is honest. The escape is the sanitizer's own permissive default state.

## Top 5 findings

1. **Limiter structurally bypassed at default** - `OutputChain.cpp:278` calls `limiter.process(...)` only inside `isLimiterActiveDb(currentLimiterDb)`. `isLimiterActiveDb(0.0f) == false`. Default everywhere is `0.0f`.
2. **Soft ceiling is the only safety net at default** - `applySoftCeiling` (threshold 0.985, ceiling 0.999, knee 0.014) becomes the main limiter under hot conditions and pins output at exactly 0.999. Logs confirm.
3. **Sanitizer is permissive, not protective** - `sanitizeOutputVolumeDb` allows `-36..+12 dB`; `+10.73 dB` is in range. `sanitizeLimiterDb` allows `-12..0`; `0` is in range. There is no cross-check between master gain and limiter state.
4. **ChannelStrip secondary headroom escape** - `ChannelStrip::sanitizeGain` allows linear `0..2` (+6 dB). The bad preset uses `gainB = 2` (max) and `widthB = 0` (mono collapse), driving ChannelStrip output to `peakLMax ~ 0.96-0.99` before OutputChain.
5. **`nearClipSamples` threshold (0.98) sits below soft-ceiling output (~0.999)** - any soft-ceiling-engaged sample trips the counter. The high `nearClipSamples` is a consequence of the soft ceiling becoming the de facto main limiter, not a separate bug.

## Exact suspected bypass path

```
outputLimiterParam default 0.0f
  -> SessionStore.runtimeParamsCache.outputLimiterDb = 0.0f
  -> AudioEngine push snapshot to OutputChain
  -> OutputChain::sanitizeLimiterDb(0.0f) = 0.0f
  -> OutputChain::isLimiterActiveDb(0.0f) = false
  -> processBlock skips limiter.process()
  -> applyMasterGain applies +10.73 dB
  -> applyFinalSoftCeiling pins at 0.999
  -> telemetry: peakLMax=0.999, nearClipSamples high
```

Sanitizer is not escaped. Sanitizer permits the off state.

## P12D scope (recommended)

1. Redefine `limiterDb = 0` as engaged transparent safety (threshold ~-0.3 dB linear, very slow release).
2. Always-on lookahead -> remove the `0 vs lookahead` branch in `updateReportedLatencyForLimiter`.
3. Telemetry-only warning when `outputVolumeDb > +6 dB` AND chain gain >= `1.5`.
4. Update tests that encode "limiter inactive at 0 dB" / "0 lookahead at 0 dB" to expect always-on lookahead. Add regression on `Preset con master alto` state asserting `output-chain.window.peakLMax < 0.97`. Add CleanAmp no-regression test.
5. Out of scope: `RuntimeGraphManager`, `AudioEngine` graph swap, `DryWetMixer`, `RoutingMixer`, `PluginStateModel` schema, pedal `processBlock` bodies, UI visual design, test file structure.
