#include "OutputChain.h"

OutputChainProcessor::OutputChainProcessor()
    : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    limiter.setThreshold(0.0f); // 0dB
    limiter.setRelease(100.0f); // 100ms
}

OutputChainProcessor::~OutputChainProcessor() {}

void OutputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };
    gain.prepare(spec);
    limiter.prepare(spec);
}

void OutputChainProcessor::releaseResources()
{
    limiter.reset();
}

void OutputChainProcessor::setParams(float volDb, float limitDb)
{
    outputVolDb = volDb;
    limiterThreshold = limitDb;
}

void OutputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1. Master Volume
    gain.setGainDecibels(outputVolDb);
    gain.process(context);

    // 2. Limiter (Seguridad)
    if (limiterThreshold < -0.1f)
    {
        limiter.setThreshold(limiterThreshold);
        limiter.process(context);
    }
}