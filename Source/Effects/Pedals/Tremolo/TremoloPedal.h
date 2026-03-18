#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <cmath>

class TremoloPedal final : public ProcessorBase
{
public:
    TremoloPedal()
    {
        addParameter(rateParam = new juce::AudioParameterFloat("tremoloRate", "Rate", 0.5f, 15.0f, 4.5f));
        addParameter(depthParam = new juce::AudioParameterFloat("tremoloDepth", "Depth", 0.0f, 1.0f, 0.65f));
        addParameter(shapeParam = new juce::AudioParameterFloat("tremoloShape", "Shape", 0.0f, 1.0f, 0.3f));
        addParameter(stereoParam = new juce::AudioParameterFloat("tremoloStereo", "Stereo", 0.0f, 1.0f, 0.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("tremoloLevel", "Level", 0.5f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Tremolo"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Modulation",
            "Pulse",
            juce::Colour::fromString("ffFBBF24"),
            {
                { "Rate", rateParam, [](float value) { return juce::String(value, 1) + " Hz"; } },
                { "Depth", depthParam, [](float value) { return formatPercent(value); } },
                { "Shape", shapeParam, [](float value) { return formatPercent(value); } },
                { "Stereo", stereoParam, [](float value) { return formatPercent(value); } },
                { "Level", levelParam, [](float value) { return formatGain(value); } }
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
        shapeSmooth.reset(sampleRate, 0.04);
        stereoSmooth.reset(sampleRate, 0.04);
        levelSmooth.reset(sampleRate, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 4.5f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.65f);
        shapeSmooth.setCurrentAndTargetValue(shapeParam != nullptr ? *shapeParam : 0.3f);
        stereoSmooth.setCurrentAndTargetValue(stereoParam != nullptr ? *stereoParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

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

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 4.5f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.65f);
        shapeSmooth.setCurrentAndTargetValue(shapeParam != nullptr ? *shapeParam : 0.3f);
        stereoSmooth.setCurrentAndTargetValue(stereoParam != nullptr ? *stereoParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 4.5f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.65f);
        shapeSmooth.setTargetValue(shapeParam != nullptr ? *shapeParam : 0.3f);
        stereoSmooth.setTargetValue(stereoParam != nullptr ? *stereoParam : 0.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float shape = shapeSmooth.getNextValue();
            const float stereo = stereoSmooth.getNextValue();
            const float level = levelSmooth.getNextValue();

            const float sinLfo = std::sin(twoPi * lfoPhase);
            const float triLfo = 2.0f * std::abs(2.0f * (lfoPhase - std::floor(lfoPhase + 0.5f))) - 1.0f;
            const float lfoL = juce::jmap(shape, sinLfo, triLfo);

            const float phaseR = std::fmod(lfoPhase + stereo * 0.5f, 1.0f);
            const float sinLfoR = std::sin(twoPi * phaseR);
            const float triLfoR = 2.0f * std::abs(2.0f * (phaseR - std::floor(phaseR + 0.5f))) - 1.0f;
            const float lfoR = juce::jmap(shape, sinLfoR, triLfoR);

            const float gainL = (1.0f - depth * 0.5f * (1.0f - lfoL)) * level;
            const float gainR = (1.0f - depth * 0.5f * (1.0f - lfoR)) * level;

            buffer.setSample(0, sample, buffer.getSample(0, sample) * gainL);
            if (numChannels > 1)
                buffer.setSample(1, sample, buffer.getSample(1, sample) * gainR);

            lfoPhase += rate / (float)juce::jmax(1.0, currentSampleRate);
            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> shapeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stereoSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* shapeParam = nullptr;
    juce::AudioParameterFloat* stereoParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    float lfoPhase = 0.0f;
    bool isPrepared = false;
};
