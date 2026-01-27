#include "InputChain.h"

InputChainProcessor::InputChainProcessor() 
    : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo())
                                      .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    // Configuración SOTA por defecto
    gate.setThreshold(-100.0f);
    gate.setRatio(12.0f);
    gate.setAttack(0.5f); // 0.5ms para transitorios de metal
    gate.setRelease(50.0f); // 50ms release natural
    
    gain.setGainDecibels(0.0f);
}

InputChainProcessor::~InputChainProcessor() {}

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
    inputGainDb = gainDb;
    gateThreshold = gateDb;

    // Lógica de ruteo
    if (forceMono) 
        currentRouting = Nova::InputRouting::Left; // Asumimos guitarra en Input 1
    else 
        currentRouting = Nova::InputRouting::Stereo;
}

void InputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    
    int numSamples = buffer.getNumSamples();
    int numCh = buffer.getNumChannels();

    // 1. INPUT ROUTING LOGIC (SOTA)
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
                for (int i = 0; i < numSamples; ++i) {
                    float sum = (l[i] + r[i]) * 0.5f;
                    l[i] = sum; r[i] = sum;
                }
                break;
            default: break;
        }
    }

    // 2. Input Trim
    gain.setGainDecibels(inputGainDb);
    gain.process(context);

    // 3. Noise Gate (Con optimización de CPU)
    if (gateThreshold > -95.0f)
    {
        gate.setThreshold(gateThreshold);
        gate.process(context);
    }
}