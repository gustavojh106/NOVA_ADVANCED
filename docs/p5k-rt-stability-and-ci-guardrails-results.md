# P5K - RT Stability Tracking & CI Guardrails Results

Date: 2026-04-30

## Summary

P5K converted RT stability verification into reusable guardrails for CI/nightly without touching audio/DSP source code.

Scope stayed in `scripts` and `docs` only.

## Files Modified

- `scripts/run-rt-profile-stability.ps1`
- `scripts/run-audio-quality-gates.ps1` (new)
- `docs/rt-profile-stability-guide.md` (new)
- `docs/p5k-audio-quality-gates.md` (new)
- `docs/p5k-rt-stability-and-ci-guardrails-results.md` (new)

Generated validation artifacts:

- `audio-base-test-report.txt`
- `artifacts/p4-offline-qa-report.txt`
- `artifacts/rt-profile-release-x64-report-p5k.json`
- `artifacts/rt-profile-stability-release-x64-p5k.json`
- `artifacts/rt-profile-stability-runs-release-x64/run-01..03.json`
- `artifacts/rt-profile-release-x64-report-gate.json`
- `artifacts/audio-thread-policy-scan.txt`
- `artifacts/audio-thread-policy-scan.json`

## Scripts Updated

### `run-rt-profile-stability.ps1`

Added CI/nightly guardrail options:

- `-CiMode`
- `-NightlyMode`
- `-FailOnReleaseWarn`
- `-FailOnReleaseFail`
- `-AllowDebugWarn`
- `-AllowDebugFail`
- `-ScenarioFilter`
- `-Runs`

Behavior updates:

- gate policy is reported in output JSON (`gatePolicy`, `gateSummary`)
- when `ScenarioFilter` is provided, blocking is evaluated on filtered scenarios only
- per-run gate evaluation is recorded (`evaluatedWarnCount`, `evaluatedFailCount`)

### `run-audio-quality-gates.ps1` (new)

Wrapper flow:

1. base validation
2. golden metrics (`Full` only)
3. release RT single-run
4. policy scan
5. release RT stability (`Full` only)

Modes:

- `-Fast`
- `-Full`

Inputs:

- `-Configuration`
- `-ValidationConfiguration` (defaults to `Debug` for base/golden)
- `-Platform`
- `-TimeoutSeconds`
- `-StabilityRuns`

## Guides Created

- `docs/rt-profile-stability-guide.md`
  - command usage
  - Debug/Release examples
  - scenario recommendations
  - min/median/max interpretation
  - CI/nightly blocking policy
  - artifact handling and phase comparison

- `docs/p5k-audio-quality-gates.md`
  - Fast / pre-commit / nightly / release-candidate gate levels
  - wrapper usage recommendations
  - blocking semantics (Release strict, Debug warn-only by default)

## Commands Executed

Validation required by P5K:

- `git diff --check`
- `scripts/run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 120`
- `scripts/run-golden-audio-metrics.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`
- `scripts/run-rt-profile-scenarios.ps1 -Configuration Release -Platform x64 -TimeoutSeconds 240 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ReportPath artifacts/rt-profile-release-x64-report-p5k.json`
- `scripts/run-rt-profile-stability.ps1 -Configuration Release -Platform x64 -Runs 3 -TimeoutSeconds 240 -BaselinePath docs/rt-profile/p4c-rt-profile-release-baseline.json -ScenarioFilter stress_block_32,sample_rate_44100,sample_rate_96000,overdrive_cleanamp_reverb_chain_nominal -CiMode -OutputPath artifacts/rt-profile-stability-release-x64-p5k.json`
- `scripts/check-audio-thread-policy.ps1`

Wrapper execution:

- `scripts/run-audio-quality-gates.ps1 -Fast -Configuration Release -Platform x64 -TimeoutSeconds 240`

## Results

`git diff --check`:

- PASS (only existing LF/CRLF working-copy warnings)

Base validation:

- PASS `results=143 passes=5830 failures=0 failingResults=0`

Golden metrics:

- PASS against baseline P4

RT Release single-run:

- PASS `total=16 pass=16 warn=0 fail=0`

RT Stability Release (3 runs, prioritized scenarios):

- gate policy: `ciMode=true`, `failOnReleaseWarn=true`, `failOnReleaseFail=true`
- gate summary: `runsWithFail=0`, `runsWithWarn=0`, `runsBlocked=0`
- prioritized scenarios:
  - `overdrive_cleanamp_reverb_chain_nominal`: `3/0/0`, median `maxBudgetRatio=0.1075875`
  - `stress_block_32`: `3/0/0`, median `maxBudgetRatio=0.14085`
  - `sample_rate_44100`: `3/0/0`, median `maxBudgetRatio=0.11969016`
  - `sample_rate_96000`: `3/0/0`, median `maxBudgetRatio=0.262575`

Policy scan:

- WARN (non-blocking)
- `failures=0`
- `contractFailures=0`

Wrapper (`-Fast`) after stabilization updates:

- PASS end-to-end
- uses `ValidationConfiguration=Debug` for base validation reliability
- keeps release RT single-run + policy in the same pass

## No Audio Code Changes

No files under these areas were modified in P5K:

- `Source/Core/AudioEngine.*`
- `Source/Core/Audio/*.h`
- `Source/Effects/*`
- `Source/Core/PluginProcessor*`
- `SessionStore`
- DSP processors

## Remaining Risks

- Release RT can still show occasional non-priority spikes on some hosts. Guardrail now supports filtered blocking for prioritized scenarios, but broad full-scenario stability should still be monitored in nightly.
- Debug remains a sensitive signal and can produce WARN variance by environment.
- Wrapper `Fast` intentionally skips golden and stability; use `Full` or dedicated nightly jobs for stronger coverage.

## Recommendation For P5L

1. Integrate `run-rt-profile-stability.ps1 -Configuration Release -NightlyMode` into nightly CI with phase-tagged artifacts.
2. Keep Debug stability as WARN-only signal unless FAIL appears recurrently.
3. Use `run-audio-quality-gates.ps1 -Full` in pre-release checks, and keep `-Fast` for local iteration.
4. Revisit scenario set quarterly and add/remove only with documented rationale and trend data.
