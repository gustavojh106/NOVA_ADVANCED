#include "ChannelStrip.h"
#include <cmath>

ChannelStripProcessor::ChannelStripProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
}

void ChannelStripProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gain.prepare(spec);
    gain.setRampDurationSeconds(0.02);

    panSmooth.reset(sampleRate, 0.02);
    widthSmooth.reset(sampleRate, 0.02);
    panSmooth.setCurrentAndTargetValue(targetPan);
    widthSmooth.setCurrentAndTargetValue(targetWidth);
}

void ChannelStripProcessor::releaseResources()
{
}

void ChannelStripProcessor::setParams(float gainVal, float panVal, float widthVal)
{
    gain.setGainLinear(juce::jlimit(0.0f, 2.0f, gainVal));

    targetPan = juce::jlimit(-1.0f, 1.0f, panVal);
    targetWidth = juce::jlimit(0.0f, 2.0f, widthVal);

    panSmooth.setTargetValue(targetPan);
    widthSmooth.setTargetValue(targetWidth);
}

void ChannelStripProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1) Gain (smoothed internally)
    gain.process(context);

    if (buffer.getNumChannels() != 2)
        return;

    auto* l = buffer.getWritePointer(0);
    auto* r = buffer.getWritePointer(1);
    const int numSamples = buffer.getNumSamples();

    // 2) Width + equal-power pan with per-sample smoothing
    for (int i = 0; i < numSamples; ++i)
    {
        const float width = widthSmooth.getNextValue();
        const float pan = panSmooth.getNextValue();

        const float mid = (l[i] + r[i]) * 0.5f;
        float side = (l[i] - r[i]) * 0.5f;
        side *= width;

        float sampleL = mid + side;
        float sampleR = mid - side;

        const float theta = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        const float gainL = std::cos(theta);
        const float gainR = std::sin(theta);

        l[i] = sampleL * gainL;
        r[i] = sampleR * gainR;
    }
}
