# P12C - Runtime Output Sanitizer Escape Audit

Task: `P12C-runtime-output-sanitizer-escape-audit`
Date: 2026-06-02
Branch: `master` (no commits since `d743d7f`)
Code modified: **No** (audit-only pass)

## 1. Scope

Investigate why manual logs still show unsafe runtime output settings after the P12A "P12B-style design" recommendation:

- `outputVolumeDb` around `+10.73 dB`
- `outputLimiterDb = 0`
- `output-chain` output `peakLMax = 0.999`
- High `nearClipSamples` in `output-chain.window` / `output-chain.alert`

Either the OutputChain safety stage was not actually upgraded in the tested build, or some runtime/preset/host/UI path is bypassing the sanitizer.

## 2. Log evidence (verbatim)

Source: `C:\Users\gusta\AppData\Roaming\NOVA\Logs\`.

### 2.1 `Preset con master alto.txt`

Master state at unsafe runtime:

```
[2026-06-02 13:30:54] [runtime.snapshot] context=runtime.push.forced,
  engineOn=true, switchMode=LineB_Only, inputGainDb=-19.68,
  gateThresholdDb=-100, forceMono=false,
  gainA=1.02, panA=-1, widthA=2,
  gainB=2, panB=0, widthB=0,
  outputVolumeDb=10.73, outputLimiterDb=0, outputMixRaw=68.4
```

Output-chain telemetry (representative):

```
[2026-06-02 13:22:33] [pedal.private.output-chain.window]
  output.signal: peakLMax=0.999000, peakRMax=0.999000,
  rmsLAvg=0.161500, rmsRAvg=0.161500,
  rmsLMax=0.434493, rmsRMax=0.434493 ...

[2026-06-02 13:30:45] [pedal.private.output-chain.window]
  anomalies: inputActiveBlocks=173, spikeBlocks=121, dcAlertBlocks=124,
  nearClipSamples=105226, invalidSamples=0, clippedSamples=0
```

`nearClipSamples` peaks above 100k per 2 s window. Output peak is exactly `0.999`, which is the `applySoftCeiling` ceiling constant.

ChannelStrip-A is already hot pre-output:

```
[2026-06-02 13:22:53] [pedal.private.channel-strip-a.window]
  output.signal: peakLMax=0.993999, peakRMax=0.993999, ...
```

ChannelStrip-B alerts up to 0.96:

```
[2026-06-02 13:30:31] [pedal.private.channel-strip-b.alert]
  output.signal: peakLMax=0.959512, peakRMax=0.959512, ...
```

### 2.2 Other manual logs

| Log | Master Vol | Limiter | output-chain peak | nearClipSamples |
|-----|-----------|---------|-------------------|-----------------|
| `HighGainAmp - Modern4x12.txt` | varied | 0 | up to ~0.99 | rises with master |
| `Boost - HighGainAmp - Modern4x12.txt` | varied | 0 | ~0.99 in alerts | present |
| `Distortion - HighGainAmp - Modern4x12.txt` | varied | 0 | ~0.99 in alerts | present |
| `Fuzz - ClassicAmp - Cabinet.txt` | varied | 0 | ~0.99 in alerts | present |
| `CleanAmp - Cabinet.txt` | varied | 0 | low (clean path) | low |
| `Preset con master alto.txt` | `+10.73` | 0 | sustained `0.999` | up to `~105k` |

`outputLimiterDb=0` is universal across every captured session. No log in the bundle shows the limiter engaged.

## 3. Code path audit (no edits)

### 3.1 OutputChain sanitizer + active branch

`Source/Core/DSP/Global/OutputChain.cpp`

```cpp
// line 13-21
float OutputChainProcessor::sanitizeLimiterDb(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    // 0 dB means transparent safety mode.
    return juce::jlimit(-12.0f, 0.0f, value);
}

// line 28-31
bool OutputChainProcessor::isLimiterActiveDb(float limiterDb) noexcept
{
    return sanitizeLimiterDb(limiterDb) < -0.0001f;
}
```

`Source/Core/DSP/Global/OutputChain.cpp:253-296` (`processBlock`):

```cpp
// 3) Lookahead limiter only when the ceiling is active.
PeakLimiter::Result limiterResult;
if (isLimiterActiveDb(currentLimiterDb))
    limiterResult = limiter.process(buffer, limiterThresholdLinearSmooth);
else
{
    limiterResult.thresholdLinearMin = 1.0f;
    limiterResult.thresholdLinearMax = 1.0f;
}
...
// 4) Last-resort ceiling. This should only touch rare overs.
applyFinalSoftCeiling(buffer);
```

The lookahead limiter is structurally skipped when `limiterDb == 0`. The only full-scale protection left is `applySoftCeiling` (line 33-54):

```cpp
constexpr float ceiling = 0.999f;
constexpr float threshold = 0.985f;
constexpr float knee = ceiling - threshold;
...
return sign * juce::jmin(ceiling, shaped);
```

A 14 mdB knee tanh-shaped ceiling. With hot master gain, the signal sits permanently at `0.999`.

### 3.2 Master volume sanitizer

`Source/Core/DSP/Global/OutputChain.cpp:3-11`

```cpp
return juce::jlimit(-36.0f, 12.0f, value);
```

`+10.73 dB` is well within the sanitizer's allowed range. `+12 dB` linear ≈ `×3.98` makeup. There is no preset-level or runtime gate that refuses dangerous master-vs-limiter combinations.

### 3.3 Defaults that produce limiter-off out-of-the-box

| Site | Value |
|------|-------|
| `Source/Core/PluginProcessor.cpp:450-451` (host parameter) | range `-12..0`, **default `0.0f`** |
| `Source/Core/PluginStateModel.h:306` (default ValueTree reset) | `IDs::OUTPUT_LIMITER = 0.0f` |
| `Source/Core/PluginStateModel.h:149` (sanitization on load) | clamp `-12..0`, default `0.0f` |
| `Source/Core/SessionStore.h:206/266/392/421` (preset round-trip) | reads/writes `0.0f` default |
| `Source/Core/OfflineQADiagnostics.h:575` (offline default) | `outputLimiterDb = 0.0f` |
| `Source/Core/AudioEngineTests.cpp:3907/4091/4138/4196/4594` (unit tests) | `outputLimiterDb = 0.0f` |
| `Source/Core/AudioEngine.cpp:160` | `(float)settings.getProperty(IDs::OUTPUT_LIMITER, 0.0f)` |

Every default path produces `limiterDb = 0`. Every fallback path produces `limiterDb = 0`. The plugin ships with limiter off.

### 3.4 ChannelStrip secondary headroom escape

`Source/Core/DSP/Global/ChannelStrip.cpp:5-11`

```cpp
return juce::jlimit(0.0f, 2.0f, value);  // sanitizeGain
```

`gainB = 2` in the bad preset (`Preset con master alto.txt`) is `+6 dB` chain makeup. `widthA = 2` boosts side energy. ChannelStrip-A alone reaches `peakLMax = 0.993999` before OutputChain. After `+10.73 dB` master, soft ceiling pins at `0.999`.

### 3.5 nearClipSamples threshold

`Source/Core/Constants.h:112`

```cpp
static constexpr float SIGNAL_NEAR_CLIP_THRESHOLD = 0.98f;
```

Soft-ceiling output (`>= 0.985` shaped to `<= 0.999`) is **always** above `0.98`, so any soft-ceiling-engaged sample increments `nearClipSamples`. The high `nearClipSamples` count is therefore a *consequence* of the soft ceiling becoming the de facto main limiter, not of a separate bug.

### 3.6 Latency reporting consistency

`Source/Core/DSP/Global/OutputChain.cpp:172-175`

```cpp
void OutputChainProcessor::updateReportedLatencyForLimiter(float limiterDb)
{
    setLatencySamples(isLimiterActiveDb(limiterDb) ? limiter.getLookaheadSamples() : 0);
}
```

If P12D forces `limiterDb = 0` to engage the lookahead, this function must change at the same time, otherwise latency reporting will desync from actual lookahead processing.

## 4. Verified vs hypothetical bypass paths

Verified:

1. **Structural bypass at default** — `processBlock` skips the limiter when `limiterDb = 0`. Default of every code path producing `limiterDb` is `0.0f`.
2. **Soft ceiling is the only full-scale protection at default** — `0.985 → 0.999` tanh; it pins signal at `0.999` rather than absorbing it.
3. **Sanitizer permits `+12 dB` master makeup** — `+10.73 dB` is within sanitizer; no cross-check against limiter state.
4. **`nearClipSamples` threshold (`0.98`) sits below soft-ceiling ceiling (`0.999`)** — so soft-ceiling engagement guarantees `nearClipSamples > 0`.
5. **Chain-side makeup** — `ChannelStrip::sanitizeGain` allows linear `2.0` (+6 dB), and `widthB = 0` collapses to mono summing; combined this drives the output-chain input near full scale.

Hypothetical (NOT confirmed by code or log):

- Preset/host path bypassing sanitizer — no evidence. `SessionStore`/`SessionPersistence`/`PluginStateModel` all round-trip through the same `0..-12` sanitization. The unsafe value is `0`, which is within range; sanitizer is "honest", just permissive of off state.
- UI binding bypass — `PluginEditor.cpp:3458-3459` attaches `OUTPUT_LIMITER` parameter to a slider labelled `outputGain`. That is a **naming oddity**, not a bypass: the parameter object is still the limiter, sanitization still runs.
- Race condition on `targetLimiterDb` — `std::atomic<float>` load/store with sanitize on both ends. Not a vector here.

## 5. Verdict

P12B was not shipped. The OutputChain code in `master` is unchanged from the P12A audit: `limiterDb = 0` still means *limiter bypassed*, not *transparent safety mode*. There is no runtime/preset/UI path "escaping" the sanitizer — the sanitizer itself is honest. The product simply ships with the limiter off, and the only remaining full-scale protection is a 14 mdB tanh soft ceiling at `0.999`. With chain makeup (`gainB = 2`) and master makeup (`outputVolumeDb = +10.73`), the soft ceiling becomes the main limiter, sustaining output at `0.999` and producing the observed `nearClipSamples` spikes. The P12A design recommendation must be implemented (P12D) before any further high-gain authoring or release pass.

## 6. Suspected bypass path (exact)

```
PluginProcessor::outputLimiterParam (default 0.0f)
  -> SessionStore::runtimeParamsCache.outputLimiterDb (0.0f)
  -> RuntimeGlobalParams snapshot (outputLimiterDb = 0.0f)
  -> AudioEngine pushes snapshot to OutputChain via setParams(vol, 0.0f)
  -> OutputChain::sanitizeLimiterDb(0.0f) -> 0.0f (in-range, returned as-is)
  -> OutputChain::isLimiterActiveDb(0.0f) -> false
  -> processBlock() skips limiter.process(...)
  -> applyMasterGain applies +10.73 dB unattenuated
  -> applyFinalSoftCeiling() is the only ceiling; pins at 0.999
  -> output-chain.window reports peakLMax = 0.999 + nearClipSamples >> 0
```

There is no escape *around* the sanitizer. The escape *is* the sanitizer's permissive default.

## 7. Recommended P12D scope

Minimal, contained, no schema change:

1. **Redefine `limiterDb = 0` as engaged transparent safety, not bypass.**
   - `isLimiterActiveDb(0.0f)` returns `true`.
   - At `0 dB` the lookahead limiter runs with threshold `~-0.3 dB linear` (e.g. `0.95 .. 0.97`) and very high release, so audible artefact is negligible on normal signal but full-scale runaway is caught.
   - Keep parameter range `-12..0`.
2. **Latency reporting** — always report `limiter.getLookaheadSamples()` once limiter is permanently on; remove the `0 vs lookahead` branch in `updateReportedLatencyForLimiter`.
3. **Master gain × limiter cross-check** — keep `outputVolumeDb` range `-60..+12`, but expose a runtime warning (telemetry only, no audio change) when `outputVolumeDb > +6 dB` AND `ChannelStrip` chain gain ≥ `1.5`. Used only in logs/QA gate.
4. **`SIGNAL_NEAR_CLIP_THRESHOLD` vs soft-ceiling alignment** — keep `0.98` but make sure once the lookahead is permanently on, sustained `nearClipSamples > 0` becomes a real, rare event again rather than the default state.
5. **Test deltas** —
   - Update existing tests that encode "limiter inactive at 0 dB" / "0 lookahead at 0 dB" to expect always-on lookahead.
   - Add a regression that runs the `Preset con master alto`-style state and asserts `output-chain.window.peakLMax < 0.97` and `nearClipSamples < small_budget`.
   - Add a CleanAmp regression to verify no tonal regression on quiet signal.
6. **Out of scope (per task contract)** — `RuntimeGraphManager`, `AudioEngine` graph swap, `DryWetMixer`, `RoutingMixer`, `PluginStateModel` schema, pedal `processBlock` bodies, UI visual design, test file structure.

## 8. Files created by this audit

- `docs/reports/P12C-runtime-output-sanitizer-escape-audit-report.md` (this file)
- `docs/reports/P12C-runtime-output-sanitizer-escape-audit-summary.md`
- `artifacts/audits/P12C-runtime-output-sanitizer-escape-findings.json`
- `artifacts/audits/P12C-runtime-output-sanitizer-escape-checklist.md`

## 9. Files NOT modified

No source files were modified. No state, preset, or schema files were modified. No build was run. This is an audit-only pass.
