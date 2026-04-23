#include "OutputChain.h"

float OutputChainProcessor::applySoftCeiling(float x) noexcept
{
    constexpr float ceiling = 0.9999f; // leave clean full-scale material intact, only soften true overs
    const float sign = juce::jlimit(-1.0f, 1.0f, x < 0.0f ? -1.0f : 1.0f);
    const float mag = std::abs(x);
    if (mag <= ceiling)
        return x;

    const float normalized = (mag - ceiling) / juce::jmax(0.0001f, 1.0f - ceiling);
    const float shaped = ceiling + ((1.0f - ceiling) * std::tanh(normalized));
    return sign * juce::jmin(1.0f, shaped);
}

OutputChainProcessor::OutputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    limiter.setThresholdDb(0.0f);
    limiter.setRelease(100.0f);
}

void OutputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gain.prepare(spec);
    limiter.prepare(sampleRate);
    for (auto& filter : dcBlockers)
        filter.prepare(sampleRate);

    gain.setRampDurationSeconds(0.02);
    gain.setGainDecibels(outputVolDb);
    gain.reset();
    limiterSmooth.reset(sampleRate, 0.02);
    limiterSmooth.setCurrentAndTargetValue(limiterThresholdTarget);
    scratchBuffer.setSize(juce::jmax(1, getTotalNumOutputChannels()),
        juce::jmax(1, samplesPerBlock), false, false, true);
    signalTelemetry.reset();
    debugTelemetry.resetWindow();
    hardSyncParams = true;
}

void OutputChainProcessor::releaseResources()
{
    reset();
}

void OutputChainProcessor::reset()
{
    gain.setGainDecibels(outputVolDb);
    gain.reset();

    limiter.reset();
    limiter.setThresholdDb(limiterThresholdTarget);
    limiter.setRelease(100.0f);
    limiterSmooth.setCurrentAndTargetValue(limiterThresholdTarget);

    for (auto& filter : dcBlockers)
        filter.reset();

    signalTelemetry.reset();
    debugTelemetry.resetWindow();
    hardSyncParams = true;
}

void OutputChainProcessor::setParams(float volDb, float limitDb)
{
    outputVolDb = volDb;
    limiterThresholdTarget = limitDb;

    if (hardSyncParams)
    {
        gain.setGainDecibels(outputVolDb);
        gain.reset();
        limiterSmooth.setCurrentAndTargetValue(limitDb);
    }
    else
    {
        limiterSmooth.setTargetValue(limitDb);
    }
}

void OutputChainProcessor::setTelemetryTag(const juce::String& newTag)
{
    telemetryTag = newTag.isNotEmpty() ? newTag : juce::String("output-chain");
    signalTelemetry.setTag(telemetryTag);
}

void OutputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    signalTelemetry.captureInput(buffer);

    if (scratchBuffer.getNumChannels() < buffer.getNumChannels()
        || scratchBuffer.getNumSamples() < buffer.getNumSamples())
    {
        scratchBuffer.setSize(juce::jmax(1, buffer.getNumChannels()),
            juce::jmax(1, buffer.getNumSamples()), false, false, true);
    }

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& dcBlock = dcBlockers[(size_t) juce::jmin(ch, (int) dcBlockers.size() - 1)];

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            data[i] = dcBlock.process(data[i]);
    }
    debugTelemetry.postDcStage.capture(buffer);

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1) Master volume
    gain.setGainDecibels(outputVolDb);
    gain.process(context);
    debugTelemetry.postGainStage.capture(buffer);

    // 2) Limiter with smoothed threshold
    const float limiterThreshold = limiterSmooth.getNextValue();
    const bool limiterActive = limiterThreshold < -0.1f;
    debugTelemetry.captureControls(outputVolDb, limiterThreshold, limiterActive);

    if (limiterActive)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                buffer.getNumSamples());

        limiter.setThresholdDb(limiterThreshold);
        limiter.process(buffer);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* before = scratchBuffer.getReadPointer(ch);
            const auto* after = buffer.getReadPointer(ch);

            for (int sampleIndex = 0; sampleIndex < buffer.getNumSamples(); ++sampleIndex)
            {
                const float delta = std::abs(before[sampleIndex] - after[sampleIndex]);
                debugTelemetry.limiterDeltaPeak = juce::jmax(debugTelemetry.limiterDeltaPeak, delta);
                if (delta >= 1.0e-5f)
                    ++debugTelemetry.limiterTouchedSamples;
            }
        }
    }
    debugTelemetry.postLimiterStage.capture(buffer);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
            buffer.getReadPointer(ch),
            buffer.getNumSamples());

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        const auto* preCeiling = scratchBuffer.getReadPointer(ch);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float shaped = applySoftCeiling(data[i]);
            const float delta = std::abs(preCeiling[i] - shaped);
            debugTelemetry.softCeilingDeltaPeak = juce::jmax(debugTelemetry.softCeilingDeltaPeak, delta);
            if (delta >= 1.0e-5f)
                ++debugTelemetry.softCeilingTouchedSamples;

            data[i] = shaped;
        }
    }

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
                   << ", softCeilingDeltaPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.softCeilingDeltaPeak)
                   << ", softCeilingTouchedSamples=" << debugTelemetry.softCeilingTouchedSamples
                   << juce::newLine
                   << "params: tag=" << telemetryTag
                   << ", outputVolDbMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.outputVolDbMin))
                   << ", outputVolDbMax=" << NovaDiagnostics::formatTelemetryScalar(juce::jmax(debugTelemetry.outputVolDbMax, safeMin(debugTelemetry.outputVolDbMin)))
                   << ", limiterThresholdMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.limiterThresholdMin))
                   << ", limiterThresholdMax=" << NovaDiagnostics::formatTelemetryScalar(juce::jmax(debugTelemetry.limiterThresholdMax, safeMin(debugTelemetry.limiterThresholdMin)))
                   << ", limiterActiveBlocks=" << debugTelemetry.limiterActiveBlocks
                   << ", hardSync=" << (hardSyncParams ? "true" : "false");
            return report;
        },
        [this]()
        {
            debugTelemetry.resetWindow();
        });

    hardSyncParams = false;
}
