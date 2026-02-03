#include "OutputChain.h"

OutputChainProcessor::OutputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    limiter.setThreshold(0.0f);  // dB
    limiter.setRelease(100.0f);  // ms
}

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

    // 1) Master volume
    gain.setGainDecibels(outputVolDb);
    gain.process(context);

    // 2) Limiter (solo si está “activado” según tu threshold)
    if (limiterThreshold < -0.1f)
    {
        limiter.setThreshold(limiterThreshold);
        limiter.process(context);
    }
}
