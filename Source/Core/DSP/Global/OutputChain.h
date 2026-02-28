#pragma once

#include <JuceHeader.h>

class OutputChainProcessor final : public juce::AudioProcessor
{
public:
    OutputChainProcessor();
    ~OutputChainProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // volDb: master output gain in dB
    // limitDb: limiter threshold in dB (0.0 = off)
    void setParams(float volDb, float limitDb);

    // Boilerplate JUCE
    const juce::String getName() const override { return "OutputChain"; }
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
    juce::dsp::Gain<float> gain;
    juce::dsp::Limiter<float> limiter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> limiterSmooth;

    float outputVolDb = 0.0f;
    float limiterThresholdTarget = 0.0f;
};
