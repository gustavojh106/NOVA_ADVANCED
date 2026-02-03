#include "ChannelStrip.h"

ChannelStripProcessor::ChannelStripProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    panner.setRule(juce::dsp::PannerRule::linear);
}

void ChannelStripProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gain.prepare(spec);
    panner.prepare(spec);
}

void ChannelStripProcessor::releaseResources()
{
}

void ChannelStripProcessor::setParams(float gainVal, float panVal, float widthVal)
{
    // gainVal viene como escala lineal (0.0 .. 2.0)
    gain.setGainLinear(gainVal);

    // panVal: -1..+1
    panner.setPan(panVal);

    // widthVal: se aplica en Mid/Side
    targetWidth = widthVal;
}

void ChannelStripProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1) Gain
    gain.process(context);

    // 2) Width (Mid/Side) — solo si es estéreo y width != 1.0
    if (buffer.getNumChannels() == 2 && std::abs(targetWidth - 1.0f) > 0.01f)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);
        const int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const float mid = (l[i] + r[i]) * 0.5f;
            float side = (l[i] - r[i]) * 0.5f;

            side *= targetWidth;

            l[i] = mid + side;
            r[i] = mid - side;
        }
    }

    // 3) Pan
    panner.process(context);
}
