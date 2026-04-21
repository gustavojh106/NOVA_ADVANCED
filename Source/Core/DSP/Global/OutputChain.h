#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

class OutputChainProcessor final : public juce::AudioProcessor
{
public:
    OutputChainProcessor();
    ~OutputChainProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
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
    struct DCBlocker
    {
        void prepare(double newSampleRate) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            const auto cutoff = 18.0;
            pole = (float)std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
            reset();
        }

        void reset() noexcept
        {
            x1 = 0.0f;
            y1 = 0.0f;
        }

        float process(float x) noexcept
        {
            const float y = x - x1 + (pole * y1);
            x1 = x;
            y1 = y;
            return y;
        }

        double sampleRate = 44100.0;
        float pole = 0.995f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    static float applySoftCeiling(float x) noexcept;

    juce::dsp::Gain<float> gain;
    juce::dsp::Limiter<float> limiter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> limiterSmooth;
    std::array<DCBlocker, 2> dcBlockers;

    float outputVolDb = 0.0f;
    float limiterThresholdTarget = 0.0f;
    bool hardSyncParams = true;
};
