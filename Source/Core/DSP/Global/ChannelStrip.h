#pragma once

#include <JuceHeader.h>

class ChannelStripProcessor final : public juce::AudioProcessor
{
public:
    ChannelStripProcessor();
    ~ChannelStripProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // gainVal: escala lineal (0.0 .. 2.0)
    // panVal : -1.0 (L) .. +1.0 (R)
    // widthVal: 0.0 (mono) .. 2.0 (extra ancho), 1.0 = normal
    void setParams(float gainVal, float panVal, float widthVal);

    // Boilerplate (JUCE)
    const juce::String getName() const override { return "ChannelStrip"; }
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
    juce::dsp::Gain<float>   gain;
    juce::dsp::Panner<float> panner;

    // Width con Mid/Side manual (JUCE dsp no trae un width simple estándar)
    float targetWidth = 1.0f;
};
