#include "OutputChain.h"

float OutputChainProcessor::sanitizeOutputVolumeDb(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    // Master output should not be a distortion stage.
    // This still allows useful makeup gain, but prevents runaway presets.
    return juce::jlimit(-36.0f, 12.0f, value);
}

float OutputChainProcessor::sanitizeLimiterDb(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    // 0 dB means transparent safety mode.
    // Negative values enable stronger limiting. -24 is intentionally extreme but safe.
    return juce::jlimit(-24.0f, 0.0f, value);
}

float OutputChainProcessor::limiterDbToLinear(float limiterDb) noexcept
{
    return juce::Decibels::decibelsToGain(sanitizeLimiterDb(limiterDb), -120.0f);
}

float OutputChainProcessor::applySoftCeiling(float x) noexcept
{
    // Last-resort safety ceiling. It should almost never be doing heavy work;
    // the lookahead limiter should control normal overs.
    constexpr float ceiling = 0.999f;
    constexpr float threshold = 0.985f;
    constexpr float knee = ceiling - threshold;

    if (!std::isfinite(x))
        return 0.0f;

    const float mag = std::abs(x);

    if (mag <= threshold)
        return x;

    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float over = (mag - threshold) / knee;
    const float shaped = threshold + knee * std::tanh(over);

    return sign * juce::jmin(ceiling, shaped);
}

OutputChainProcessor::OutputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
}

void OutputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    const int channels = juce::jlimit(1, kMaxOutputChannels, getTotalNumOutputChannels());

    limiter.prepare(sampleRate, channels);

    for (auto& filter : dcBlockers)
        filter.prepare(sampleRate);

    outputGainSmooth.reset(sampleRate, 0.025);
    limiterThresholdLinearSmooth.reset(sampleRate, 0.050);

    currentOutputVolDb = sanitizeOutputVolumeDb(targetOutputVolDb.load(std::memory_order_acquire));
    currentLimiterDb = sanitizeLimiterDb(targetLimiterDb.load(std::memory_order_acquire));

    outputGainSmooth.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(currentOutputVolDb, -120.0f));
    limiterThresholdLinearSmooth.setCurrentAndTargetValue(limiterDbToLinear(currentLimiterDb));

    // Stable latency: always report and apply the lookahead delay.
    // Do not change this dynamically when the limiter threshold changes.
    setLatencySamples(limiter.getLookaheadSamples());

    signalTelemetry.reset();
    debugTelemetry.resetWindow();

    parametersPrepared = true;
    hardSyncParams = true;
}

void OutputChainProcessor::releaseResources()
{
    reset();
}

void OutputChainProcessor::reset()
{
    limiter.reset();

    for (auto& filter : dcBlockers)
        filter.reset();

    currentOutputVolDb = sanitizeOutputVolumeDb(targetOutputVolDb.load(std::memory_order_acquire));
    currentLimiterDb = sanitizeLimiterDb(targetLimiterDb.load(std::memory_order_acquire));

    outputGainSmooth.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(currentOutputVolDb, -120.0f));
    limiterThresholdLinearSmooth.setCurrentAndTargetValue(limiterDbToLinear(currentLimiterDb));

    setLatencySamples(limiter.getLookaheadSamples());

    signalTelemetry.reset();
    debugTelemetry.resetWindow();

    hardSyncParams = true;
}

void OutputChainProcessor::setParams(float volDb, float limitDb)
{
    targetOutputVolDb.store(sanitizeOutputVolumeDb(volDb), std::memory_order_release);
    targetLimiterDb.store(sanitizeLimiterDb(limitDb), std::memory_order_release);
}

void OutputChainProcessor::setTelemetryTag(const juce::String& newTag)
{
    telemetryTag = newTag.isNotEmpty() ? newTag : juce::String("output-chain");
    signalTelemetry.setTag(telemetryTag);
}

void OutputChainProcessor::applyParameterTargets(bool forceSync) noexcept
{
    const float newOutputDb = sanitizeOutputVolumeDb(targetOutputVolDb.load(std::memory_order_acquire));
    const float newLimiterDb = sanitizeLimiterDb(targetLimiterDb.load(std::memory_order_acquire));

    currentOutputVolDb = newOutputDb;
    currentLimiterDb = newLimiterDb;

    const float outputGain = juce::Decibels::decibelsToGain(currentOutputVolDb, -120.0f);
    const float limiterThreshold = limiterDbToLinear(currentLimiterDb);

    if (forceSync || !parametersPrepared)
    {
        outputGainSmooth.setCurrentAndTargetValue(outputGain);
        limiterThresholdLinearSmooth.setCurrentAndTargetValue(limiterThreshold);
    }
    else
    {
        outputGainSmooth.setTargetValue(outputGain);
        limiterThresholdLinearSmooth.setTargetValue(limiterThreshold);
    }
}

void OutputChainProcessor::applyDcBlockers(juce::AudioBuffer<float>& buffer) noexcept
{
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxOutputChannels);
    const int numSamples = buffer.getNumSamples();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& dc = dcBlockers[(size_t)ch];

        for (int i = 0; i < numSamples; ++i)
            data[i] = dc.process(data[i]);
    }
}

void OutputChainProcessor::applyMasterGain(juce::AudioBuffer<float>& buffer) noexcept
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels <= 0 || numSamples <= 0)
        return;

    std::array<float*, kMaxOutputChannels> channelData{};
    const int channelsToProcess = juce::jmin(numChannels, kMaxOutputChannels);

    for (int ch = 0; ch < channelsToProcess; ++ch)
        channelData[(size_t)ch] = buffer.getWritePointer(ch);

    if (!outputGainSmooth.isSmoothing())
    {
        const float gain = outputGainSmooth.getCurrentValue();

        if (std::abs(gain - 1.0f) <= 1.0e-6f)
            return;

        for (int ch = 0; ch < channelsToProcess; ++ch)
            juce::FloatVectorOperations::multiply(channelData[(size_t)ch], gain, numSamples);

        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = outputGainSmooth.getNextValue();

        for (int ch = 0; ch < channelsToProcess; ++ch)
            channelData[(size_t)ch][i] *= g;
    }
}

void OutputChainProcessor::applyFinalSoftCeiling(juce::AudioBuffer<float>& buffer) noexcept
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);

        for (int i = 0; i < numSamples; ++i)
        {
            const float original = data[i];
            const float shaped = applySoftCeiling(original);
            const float delta = std::abs(original - shaped);

            debugTelemetry.softCeilingDeltaPeak = juce::jmax(debugTelemetry.softCeilingDeltaPeak, delta);

            if (delta >= 1.0e-5f)
                ++debugTelemetry.softCeilingTouchedSamples;

            data[i] = shaped;
        }
    }
}

void OutputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    signalTelemetry.captureInput(buffer);

    applyParameterTargets(hardSyncParams);

    // Final-stage scrub before any dynamics. If upstream sends NaN/Inf/denormals,
    // the limiter must not lock its gain reduction to garbage.
    const auto guardStats = Nova::DSP::scrub(buffer);
    debugTelemetry.guardInvalidSamples += guardStats.invalidSamples;
    debugTelemetry.guardClippedSamples += guardStats.clippedSamples;
    debugTelemetry.guardDenormalSamples += guardStats.denormalSamples;

    // 1) DC protection before gain and limiter.
    applyDcBlockers(buffer);
    debugTelemetry.postDcStage.capture(buffer);

    // 2) Sample-accurate master gain smoothing.
    applyMasterGain(buffer);
    debugTelemetry.postGainStage.capture(buffer);

    // 3) Stable-latency lookahead limiter.
    const auto limiterResult = limiter.process(buffer, limiterThresholdLinearSmooth);
    debugTelemetry.captureControls(currentOutputVolDb, currentLimiterDb, limiterResult);
    debugTelemetry.postLimiterStage.capture(buffer);

    // 4) Last-resort ceiling. This should only touch rare overs.
    applyFinalSoftCeiling(buffer);

    signalTelemetry.captureOutputAndEmitIfNeeded(buffer,
        [this]()
        {
            auto safeMin = [](float value)
                {
                    return value >= 1.0e8f ? 0.0f : value;
                };

            juce::String report;
            report << debugTelemetry.postDcStage.buildSummary("dcBlock.stage") << juce::newLine
                << debugTelemetry.postGainStage.buildSummary("gain.stage") << juce::newLine
                << debugTelemetry.postLimiterStage.buildSummary("limiter.stage") << juce::newLine
                << "stage.deltas: limiterDeltaPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.limiterDeltaPeak)
                << ", limiterTouchedSamples=" << debugTelemetry.limiterTouchedSamples
                << ", limiterMaxReductionDb=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.limiterMaxReductionDb)
                << ", limiterMinGain=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.limiterMinGain)
                << ", softCeilingDeltaPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.softCeilingDeltaPeak)
                << ", softCeilingTouchedSamples=" << debugTelemetry.softCeilingTouchedSamples
                << juce::newLine
                << "params: tag=" << telemetryTag
                << ", outputVolDbMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.outputVolDbMin))
                << ", outputVolDbMax=" << NovaDiagnostics::formatTelemetryScalar(juce::jmax(debugTelemetry.outputVolDbMax, safeMin(debugTelemetry.outputVolDbMin)))
                << ", limiterThresholdDbMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.limiterThresholdDbMin))
                << ", limiterThresholdDbMax=" << NovaDiagnostics::formatTelemetryScalar(juce::jmax(debugTelemetry.limiterThresholdDbMax, safeMin(debugTelemetry.limiterThresholdDbMin)))
                << ", limiterActiveBlocks=" << debugTelemetry.limiterActiveBlocks
                << juce::newLine
                << "guard: invalid=" << debugTelemetry.guardInvalidSamples
                << ", clipped=" << debugTelemetry.guardClippedSamples
                << ", denormals=" << debugTelemetry.guardDenormalSamples
                << juce::newLine
                << "params: hardSync=" << (hardSyncParams ? "true" : "false");
            return report;
        },
        [this]()
        {
            debugTelemetry.resetWindow();
        });

    hardSyncParams = false;
}
