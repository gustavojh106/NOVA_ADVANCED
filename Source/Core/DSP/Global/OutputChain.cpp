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
    limiter.setThreshold(0.0f);
    limiter.setRelease(100.0f);
}

void OutputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gain.prepare(spec);
    limiter.prepare(spec);
    for (auto& filter : dcBlockers)
        filter.prepare(sampleRate);

    gain.setRampDurationSeconds(0.02);
    gain.setGainDecibels(outputVolDb);
    gain.reset();
    limiterSmooth.reset(sampleRate, 0.02);
    limiterSmooth.setCurrentAndTargetValue(limiterThresholdTarget);
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
    limiter.setThreshold(limiterThresholdTarget);
    limiter.setRelease(100.0f);
    limiterSmooth.setCurrentAndTargetValue(limiterThresholdTarget);

    for (auto& filter : dcBlockers)
        filter.reset();

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

void OutputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1) Master volume
    gain.setGainDecibels(outputVolDb);
    gain.process(context);

    // 2) Limiter with smoothed threshold
    const float limiterThreshold = limiterSmooth.getNextValue();
    if (limiterThreshold < -0.1f)
    {
        limiter.setThreshold(limiterThreshold);
        limiter.process(context);
    }

    const bool engageDCBlock = std::abs(outputVolDb) > 0.001f || limiterThreshold < -0.1f;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& dcBlock = dcBlockers[(size_t)juce::jmin(ch, (int)dcBlockers.size() - 1)];

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float sample = applySoftCeiling(data[i]);
            if (engageDCBlock)
                sample = dcBlock.process(sample);

            data[i] = sample;
        }
    }

    hardSyncParams = false;
}
