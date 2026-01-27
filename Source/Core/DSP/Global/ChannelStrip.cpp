#include "ChannelStrip.h"

ChannelStripProcessor::ChannelStripProcessor()
    : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    panner.setRule(juce::dsp::PannerRule::linear);
}

ChannelStripProcessor::~ChannelStripProcessor() {}

void ChannelStripProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };
    gain.prepare(spec);
    panner.prepare(spec);
}

void ChannelStripProcessor::releaseResources() {}

void ChannelStripProcessor::setParams(float gainVal, float panVal, float widthVal)
{
    // GainVal entra como escala lineal (0.0 a 2.0), lo convertimos a dB si fuera necesario, 
    // pero juce::dsp::Gain puede tomar lineal con setGainLinear.
    gain.setGainLinear(gainVal);
    panner.setPan(panVal);
    targetWidth = widthVal;
}

void ChannelStripProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1. Gain
    gain.process(context);

    // 2. Width (Mid/Side Processing)
    // Solo si el width es diferente de 1.0 (Normal)
    if (std::abs(targetWidth - 1.0f) > 0.01f && buffer.getNumChannels() == 2)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);
        int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            float mid = (l[i] + r[i]) * 0.5f;
            float side = (l[i] - r[i]) * 0.5f;

            // Ajustamos el side
            side *= targetWidth;

            // Reconstruimos L/R
            l[i] = mid + side;
            r[i] = mid - side;
        }
    }

    // 3. Pan
    panner.process(context);
}