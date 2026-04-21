#pragma once

#include <JuceHeader.h>
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
    static float semitonesToRatio(int semitones);
    float readPitchSample(int channel, float delaySamples) const;
    void resetPitchShifter();
    void processTranspose(juce::AudioBuffer<float>& buffer);

    juce::dsp::Gain<float> gain;
    juce::dsp::NoiseGate<float> gate;
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
