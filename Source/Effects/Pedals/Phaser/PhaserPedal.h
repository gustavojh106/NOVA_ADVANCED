#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

class PhaserPedal final : public ProcessorBase
{
public:
    PhaserPedal()
    {
        addParameter(rateParam = new juce::AudioParameterFloat("phaserRate", "Rate", 0.05f, 8.0f, 0.65f));
        addParameter(depthParam = new juce::AudioParameterFloat("phaserDepth", "Depth", 0.0f, 1.0f, 0.72f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("phaserFeedback", "Feedback", -0.85f, 0.85f, 0.45f));
        addParameter(stagesParam = new juce::AudioParameterFloat("phaserStages", "Stages", 2.0f, 12.0f, 6.0f));
        addParameter(mixParam = new juce::AudioParameterFloat("phaserMix", "Mix", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "Phaser"; }
    double getTailLengthSeconds() const override { return 0.1; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Modulation",
            "Phase Shift",
            juce::Colour::fromString("ffC084FC"),
            {
                { "Rate", rateParam, [](float value) { return juce::String(value, 2) + " Hz"; } },
                { "Depth", depthParam, [](float value) { return formatPercent(value); } },
                { "Feedback", feedbackParam, [](float value) { return juce::String(juce::roundToInt(value * 100.0f)) + "%"; } },
                { "Stages", stagesParam, [](float value) { return juce::String(juce::roundToInt(value)); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;

        rateSmooth.reset(sampleRate, 0.04);
        depthSmooth.reset(sampleRate, 0.04);
        feedbackSmooth.reset(sampleRate, 0.04);
        mixSmooth.reset(sampleRate, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

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
        lfoPhase = 0.0f;
        for (auto& ch : allPassStates)
            ch.fill(0.0f);
        feedbackState.fill(0.0f);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        const int numStages = juce::roundToInt(stagesParam != nullptr ? *stagesParam : 6.0f);
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        constexpr float minFreq = 200.0f;
        constexpr float maxFreq = 4500.0f;
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            const float lfoValue = 0.5f + 0.5f * std::sin(twoPi * lfoPhase);
            const float sweepFreq = minFreq + (maxFreq - minFreq) * lfoValue * depth;
            const float allPassCoeff = (std::tan(juce::MathConstants<float>::pi * sweepFreq / (float)currentSampleRate) - 1.0f)
                / (std::tan(juce::MathConstants<float>::pi * sweepFreq / (float)currentSampleRate) + 1.0f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                float x = dry + feedbackState[(size_t)ch] * feedback;

                for (int stage = 0; stage < numStages && stage < kMaxStages; ++stage)
                {
                    const float input = x;
                    x = allPassCoeff * input + allPassStates[(size_t)ch][(size_t)stage];
                    allPassStates[(size_t)ch][(size_t)stage] = input - allPassCoeff * x;
                }

                feedbackState[(size_t)ch] = std::tanh(x);

                const float wet = x;
                buffer.setSample(ch, sample, dry * (1.0f - mix) + wet * mix);
            }

            lfoPhase += rate / (float)juce::jmax(1.0, currentSampleRate);
            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

private:
    static constexpr int kMaxStages = 12;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* stagesParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    std::array<std::array<float, 12>, 2> allPassStates{};
    std::array<float, 2> feedbackState{};
    double currentSampleRate = 44100.0;
    float lfoPhase = 0.0f;
    bool isPrepared = false;
};
