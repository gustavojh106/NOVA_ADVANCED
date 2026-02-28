#include "InputChain.h"

InputChainProcessor::InputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    gate.setThreshold(-100.0f);
    gate.setRatio(12.0f);
    gate.setAttack(0.5f);
    gate.setRelease(50.0f);

    gain.setGainDecibels(0.0f);
}

void InputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gate.prepare(spec);
    gain.prepare(spec);
    gain.setRampDurationSeconds(0.02);
}

void InputChainProcessor::releaseResources()
{
    gate.reset();
    gain.reset();
}

void InputChainProcessor::setParams(float gainDb, float gateDb, bool forceMono, int inputChannelIndex)
{
    juce::ignoreUnused(inputChannelIndex);

    inputGainDb = gainDb;
    gateThreshold = gateDb;

    currentRouting = forceMono ? Nova::InputRouting::Left
        : Nova::InputRouting::Stereo;
}

void InputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    // 1) Input routing
    if (numCh > 1)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);

        switch (currentRouting)
        {
            case Nova::InputRouting::Left:
                juce::FloatVectorOperations::copy(r, l, numSamples);
                break;

            case Nova::InputRouting::Right:
                juce::FloatVectorOperations::copy(l, r, numSamples);
                break;

            case Nova::InputRouting::Sum:
                for (int i = 0; i < numSamples; ++i)
                {
                    const float sum = (l[i] + r[i]) * 0.5f;
                    l[i] = sum;
                    r[i] = sum;
                }
                break;

            default:
                break;
        }
    }

    // 2) Input trim
    gain.setGainDecibels(inputGainDb);
    gain.process(context);

    // 3) Noise gate
    if (gateThreshold > -95.0f)
    {
        gate.setThreshold(gateThreshold);
        gate.process(context);
    }
}
