#include "OutputChain.h"

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

    gain.setRampDurationSeconds(0.02);
    limiterSmooth.reset(sampleRate, 0.02);
    limiterSmooth.setCurrentAndTargetValue(limiterThresholdTarget);
}

void OutputChainProcessor::releaseResources()
{
    limiter.reset();
}

void OutputChainProcessor::setParams(float volDb, float limitDb)
{
    outputVolDb = volDb;
    limiterThresholdTarget = limitDb;
    limiterSmooth.setTargetValue(limitDb);
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
}
