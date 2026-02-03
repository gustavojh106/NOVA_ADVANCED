#include "InputChain.h"

InputChainProcessor::InputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    // Defaults del gate (sensibles para guitarra)
    gate.setThreshold(-100.0f);
    gate.setRatio(12.0f);
    gate.setAttack(0.5f);   // ms
    gate.setRelease(50.0f); // ms

    gain.setGainDecibels(0.0f);
}

void InputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gate.prepare(spec);
    gain.prepare(spec);
}

void InputChainProcessor::releaseResources()
{
    gate.reset();
    gain.reset();
}

void InputChainProcessor::setParams(float gainDb, float gateDb, bool forceMono, int inputChannelIndex)
{
    // Actualmente no se usa, pero se mantiene por compatibilidad/API
    juce::ignoreUnused(inputChannelIndex);

    inputGainDb = gainDb;
    gateThreshold = gateDb;

    // Ruteo: si forzamos mono, asumimos guitarra por Input 1 (Left)
    currentRouting = forceMono ? Nova::InputRouting::Left
        : Nova::InputRouting::Stereo;
}

void InputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    // 1) Input routing (solo si hay al menos 2 canales)
    if (numCh > 1)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);

        switch (currentRouting)
        {
        case Nova::InputRouting::Left:
            juce::FloatVectorOperations::copy(r, l, numSamples); // L -> R
            break;

        case Nova::InputRouting::Right:
            juce::FloatVectorOperations::copy(l, r, numSamples); // R -> L
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

    // 3) Noise gate (evitar procesarlo cuando está prácticamente "apagado")
    if (gateThreshold > -95.0f)
    {
        gate.setThreshold(gateThreshold);
        gate.process(context);
    }
}
