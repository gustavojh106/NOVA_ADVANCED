#pragma once
#include <JuceHeader.h>

class ProcessorBase : public juce::AudioProcessor
{
public:
    ProcessorBase()
        : AudioProcessor(BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo())
            .withOutput("Output", juce::AudioChannelSet::stereo()))
    {
    }

    // --- ESTA ES LA CLAVE ---
    // Le decimos a JUCE: "Acepto Mono, Acepto Stereo, Acepto lo que me des"
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        // Inputs y Outputs deben tener el mismo número de canales (para simplificar)
        if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
            return false;

        // Solo aceptamos Mono (1) o Stereo (2)
        if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::mono()
            && layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
            return false;

        return true;
    }

    // Boilerplate estándar
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "Base"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProcessorBase)
};