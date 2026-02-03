#pragma once

#include <JuceHeader.h>
#include "../../Constants.h" // Ajusta la ruta a Constants.h

class InputChainProcessor final : public juce::AudioProcessor
{
public:
    InputChainProcessor();
    ~InputChainProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Configuración del input chain
    // gainDb: ganancia de entrada en dB
    // gateDb: umbral del noise gate en dB
    // forceMono: fuerza ruteo mono (duplica L->R)
    // inputChannelIndex: reservado (API), actualmente no afecta el ruteo
    void setParams(float gainDb, float gateDb, bool forceMono, int inputChannelIndex = 0);

    // Boilerplate JUCE
    const juce::String getName() const override { return "InputChain"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    juce::dsp::Gain<float>      gain;
    juce::dsp::NoiseGate<float> gate;

    float inputGainDb = 0.0f;
    float gateThreshold = -100.0f;

    Nova::InputRouting currentRouting = Nova::InputRouting::Stereo;
};
