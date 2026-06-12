# P12C Output Sanitizer Escape - Verification Checklist

Code modified: No. Audit-only pass.

## Evidence gathering
- [x] Located manual logs in `C:\Users\gusta\AppData\Roaming\NOVA\Logs\`
- [x] Read `HighGainAmp - Modern4x12.txt`
- [x] Read `Boost - HighGainAmp - Modern4x12.txt` (grep)
- [x] Read `Distortion - HighGainAmp - Modern4x12.txt` (grep)
- [x] Read `Fuzz - ClassicAmp - Cabinet.txt` (grep)
- [x] Read `CleanAmp - Cabinet.txt` (grep)
- [x] Read `Preset con master alto.txt` (grep)
- [x] Confirmed `outputVolumeDb = 10.73` and `outputLimiterDb = 0` together in one runtime snapshot
- [x] Confirmed `output-chain.window output.signal peakLMax = 0.999`
- [x] Confirmed `nearClipSamples` > 100k in one window

## Code path audit
- [x] `OutputChain.h` - read full file
- [x] `OutputChain.cpp` - read full file
- [x] `sanitizeOutputVolumeDb` clamp range = `[-36, +12]`
- [x] `sanitizeLimiterDb` clamp range = `[-12, 0]`
- [x] `isLimiterActiveDb` uses `< -0.0001f`
- [x] `processBlock` calls `limiter.process()` only inside `isLimiterActiveDb`
- [x] `applySoftCeiling` ceiling = `0.999`, threshold = `0.985`, knee = `0.014`
- [x] `Constants.h SIGNAL_NEAR_CLIP_THRESHOLD = 0.98`
- [x] `PluginProcessor.cpp:451` outputLimiterParam default `0.0f`
- [x] `PluginStateModel.h:149` sanitize default `0.0f`, range `-12..0`
- [x] `PluginStateModel.h:306` reset writes `0.0f`
- [x] `ChannelStrip::sanitizeGain` clamps `[0, 2]`
- [x] Verified host parameter, ValueTree default, preset round-trip all default `0.0f`
- [x] Verified no `P12B` commit exists in git log

## Bypass hypothesis rejection
- [x] Preset path bypass - rejected (sanitize is invoked, allows 0.0f)
- [x] UI binding bypass - rejected (RangedAudioParameter constrains)
- [x] Race condition - rejected (atomic + sanitize on both ends)

## Deliverables
- [x] `docs/reports/P12C-runtime-output-sanitizer-escape-audit-report.md`
- [x] `docs/reports/P12C-runtime-output-sanitizer-escape-audit-summary.md`
- [x] `artifacts/audits/P12C-runtime-output-sanitizer-escape-findings.json`
- [x] `artifacts/audits/P12C-runtime-output-sanitizer-escape-checklist.md` (this file)

## Non-destructive constraints
- [x] No source file edited
- [x] No build run
- [x] No state/preset file modified
- [x] No schema change proposed for P12D
- [x] Pedal `processBlock` bodies untouched in recommendation
- [x] `RuntimeGraphManager`, `AudioEngine` graph swap, `DryWetMixer`, `RoutingMixer` untouched in recommendation
- [x] Test file structure preserved in recommendation
- [x] UI visual design untouched in recommendation

## Recommended next gates (P12D)
- [ ] Implement always-on transparent safety limiter at `limiterDb = 0`
- [ ] Update `updateReportedLatencyForLimiter` to report lookahead unconditionally
- [ ] Update existing unit tests that encode `limiter inactive at 0 dB`
- [ ] Add regression test on `Preset con master alto` state - assert `peakLMax < 0.97`
- [ ] Add CleanAmp quiet-signal no-regression test
- [ ] Re-run manual log with bad preset and confirm `peakLMax < 0.97` and small `nearClipSamples`
