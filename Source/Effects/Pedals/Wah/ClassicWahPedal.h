#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

class ClassicWahPedal final : public ProcessorBase
{
public:
    ClassicWahPedal()
    {
        addParameter(sweepParam = new juce::AudioParameterFloat("wahSweep", "Sweep", 0.0f, 1.0f, 0.46f));
        addParameter(rangeParam = new juce::AudioParameterFloat("wahManualRange", "Range", 0.0f, 1.0f, 0.74f));
        addParameter(resonanceParam = new juce::AudioParameterFloat("wahManualResonance", "Resonance", 0.5f, 8.0f, 4.0f));
        addParameter(voiceParam = new juce::AudioParameterFloat("wahVoice", "Voice", 0.0f, 1.0f, 0.34f));
        addParameter(mixParam = new juce::AudioParameterFloat("wahManualMix", "Mix", 0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "Wah"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Filter",
            "Classic Wah",
            juce::Colour::fromString("ffD97706"),
            {
                { "Sweep", sweepParam, [](float value) { return formatPercent(value); } },
                { "Range", rangeParam, [](float value) { return formatPercent(value); } },
                { "Reso", resonanceParam, [](float value) { return juce::String(value, 1) + "x"; } },
                { "Voice", voiceParam, [](float value) { return formatPercent(value); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } }
            },
            220,
            186);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;
        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        sweepSmooth.reset(sampleRate, 0.04);
        rangeSmooth.reset(sampleRate, 0.04);
        resonanceSmooth.reset(sampleRate, 0.04);
        voiceSmooth.reset(sampleRate, 0.04);
        mixSmooth.reset(sampleRate, 0.04);

        sweepSmooth.setCurrentAndTargetValue(sweepParam != nullptr ? *sweepParam : 0.46f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.74f);
        resonanceSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 4.0f);
        voiceSmooth.setCurrentAndTargetValue(voiceParam != nullptr ? *voiceParam : 0.34f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false;
    }

    void reset() override
    {
        for (auto& state : svfState)
        {
            state.low = 0.0f;
            state.band = 0.0f;
        }

        sweepSmooth.setCurrentAndTargetValue(sweepParam != nullptr ? *sweepParam : 0.46f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.74f);
        resonanceSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 4.0f);
        voiceSmooth.setCurrentAndTargetValue(voiceParam != nullptr ? *voiceParam : 0.34f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        if (scratchBuffer.getNumChannels() < buffer.getNumChannels()
            || scratchBuffer.getNumSamples() < buffer.getNumSamples())
        {
            scratchBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch), buffer.getNumSamples());

        sweepSmooth.setTargetValue(sweepParam != nullptr ? *sweepParam : 0.46f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? *rangeParam : 0.74f);
        resonanceSmooth.setTargetValue(resonanceParam != nullptr ? *resonanceParam : 4.0f);
        voiceSmooth.setTargetValue(voiceParam != nullptr ? *voiceParam : 0.34f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float sweep = sweepSmooth.getNextValue();
            const float range = rangeSmooth.getNextValue();
            const float resonance = resonanceSmooth.getNextValue();
            const float voice = voiceSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            const float minFreq = juce::jmap(voice, 300.0f, 520.0f);
            const float maxFreq = juce::jmap(voice, 1800.0f, 3200.0f) + range * 1800.0f;
            const float freq = juce::jlimit(80.0f, (float)(currentSampleRate * 0.4),
                juce::jmap(sweep, minFreq, maxFreq));

            const float g = std::tan(juce::MathConstants<float>::pi * freq / (float)currentSampleRate);
            const float k = 1.0f / juce::jmax(0.5f, resonance);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, sample);
                const float input = buffer.getSample(ch, sample);

                const float hp = (input - (k + g) * svfState[(size_t)ch].band - svfState[(size_t)ch].low)
                    / (1.0f + k * g + g * g);
                const float bp = g * hp + svfState[(size_t)ch].band;
                svfState[(size_t)ch].band = g * hp + bp;
                svfState[(size_t)ch].low += g * bp;

                const float wet = std::tanh(bp * (resonance * 0.72f));
                buffer.setSample(ch, sample, juce::jmap(mix, dry, wet));
            }
        }

        endBypassProcess(buffer);
    }

private:
    struct SVFState
    {
        float low = 0.0f;
        float band = 0.0f;
    };

    std::array<SVFState, 2> svfState{};
    juce::AudioBuffer<float> scratchBuffer;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sweepSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resonanceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> voiceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* sweepParam = nullptr;
    juce::AudioParameterFloat* rangeParam = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* voiceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    bool isPrepared = false;
};
