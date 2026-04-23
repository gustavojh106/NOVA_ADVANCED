#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include "../../Constants.h"

class InputChainProcessor final : public juce::AudioProcessor
{
public:
    InputChainProcessor();
    ~InputChainProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    void setParams(float gainDb, float gateDb, bool forceMono, int inputTransposeSemitones = 0);

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
    struct DCBlocker
    {
        void prepare(double newSampleRate) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            constexpr double cutoff = 18.0;
            pole = (float) std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
            reset();
        }

        void reset() noexcept
        {
            x1 = 0.0f;
            y1 = 0.0f;
        }

        float process(float x) noexcept
        {
            const float y = x - x1 + pole * y1;
            x1 = x;
            y1 = y;
            return y;
        }

        double sampleRate = 44100.0;
        float pole = 0.995f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    static float semitonesToRatio(int semitones);
    float readPitchSample(int channel, float delaySamples) const;
    void resetPitchShifter();
    void processTranspose(juce::AudioBuffer<float>& buffer);

    using HighPassFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    juce::dsp::Gain<float> gain;
    HighPassFilter subsonicHighPass;
    juce::dsp::NoiseGate<float> gate;
    std::array<DCBlocker, 2> dcBlockers;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> transposeRatioSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> transposeMixSmooth;
    juce::AudioBuffer<float> pitchBuffer;

    float inputGainDb = 0.0f;
    float gateThreshold = -100.0f;
    int inputTranspose = 0;
    double currentSampleRate = 44100.0;
    float pitchPhase = 0.0f;
    int pitchWindowSamples = 0;
    int pitchMinDelaySamples = 0;
    int pitchBufferSize = 0;
    int pitchWritePos = 0;
    bool hardSyncParams = true;

    Nova::InputRouting currentRouting = Nova::InputRouting::Stereo;
};
