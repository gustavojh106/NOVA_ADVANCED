#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <cmath>

class NoiseGatePedal final : public ProcessorBase
{
public:
    NoiseGatePedal()
    {
        addParameter(thresholdParam = new juce::AudioParameterFloat("gateThreshold", "Threshold", -80.0f, 0.0f, -45.0f));
        addParameter(attackParam = new juce::AudioParameterFloat("gateAttack", "Attack", 0.1f, 20.0f, 0.5f));
        addParameter(holdParam = new juce::AudioParameterFloat("gateHold", "Hold", 5.0f, 500.0f, 80.0f));
        addParameter(releaseParam = new juce::AudioParameterFloat("gateRelease", "Release", 5.0f, 500.0f, 85.0f));
        addParameter(rangeParam = new juce::AudioParameterFloat("gateRange", "Range", -96.0f, 0.0f, -96.0f));
    }

    const juce::String getName() const override { return "Noise Gate"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Dynamics",
            "Silencer",
            juce::Colour::fromString("ff10B981"),
            {
                { "Thresh", thresholdParam, [](float value) { return formatDecibels(value); } },
                { "Attack", attackParam, [](float value) { return formatMilliseconds(value); } },
                { "Hold", holdParam, [](float value) { return formatMilliseconds(value); } },
                { "Release", releaseParam, [](float value) { return formatMilliseconds(value); } },
                { "Range", rangeParam, [](float value) { return formatDecibels(value); } }
            },
            220,
            186);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;

        thresholdSmooth.reset(sampleRate, 0.05);
        attackSmooth.reset(sampleRate, 0.05);
        holdSmooth.reset(sampleRate, 0.05);
        releaseSmooth.reset(sampleRate, 0.05);
        rangeSmooth.reset(sampleRate, 0.05);

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? *thresholdParam : -45.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 0.5f);
        holdSmooth.setCurrentAndTargetValue(holdParam != nullptr ? *holdParam : 80.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? *releaseParam : 85.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : -96.0f);

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
        envelope = 0.0f;
        gateGain = 0.0f;
        holdCounter = 0;

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? *thresholdParam : -45.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 0.5f);
        holdSmooth.setCurrentAndTargetValue(holdParam != nullptr ? *holdParam : 80.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? *releaseParam : 85.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : -96.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        thresholdSmooth.setTargetValue(thresholdParam != nullptr ? *thresholdParam : -45.0f);
        attackSmooth.setTargetValue(attackParam != nullptr ? *attackParam : 0.5f);
        holdSmooth.setTargetValue(holdParam != nullptr ? *holdParam : 80.0f);
        releaseSmooth.setTargetValue(releaseParam != nullptr ? *releaseParam : 85.0f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? *rangeParam : -96.0f);

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float threshDb = thresholdSmooth.getNextValue();
            const float attackMs = attackSmooth.getNextValue();
            const float holdMs = holdSmooth.getNextValue();
            const float releaseMs = releaseSmooth.getNextValue();
            const float rangeDb = rangeSmooth.getNextValue();

            const float threshLin = juce::Decibels::decibelsToGain(threshDb, -96.0f);
            const float rangeLin = juce::Decibels::decibelsToGain(rangeDb, -96.0f);
            const float attackCoeff = coefficientForTime(attackMs);
            const float releaseCoeff = coefficientForTime(releaseMs);
            const int holdSamples = juce::roundToInt(holdMs * 0.001f * (float)currentSampleRate);

            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                detector = juce::jmax(detector, std::abs(buffer.getSample(ch, sample)));

            envelope = (detector > envelope)
                ? (attackCoeff * envelope) + ((1.0f - attackCoeff) * detector)
                : (releaseCoeff * envelope) + ((1.0f - releaseCoeff) * detector);

            float targetGain;
            if (envelope >= threshLin)
            {
                targetGain = 1.0f;
                holdCounter = holdSamples;
            }
            else if (holdCounter > 0)
            {
                targetGain = 1.0f;
                --holdCounter;
            }
            else
            {
                targetGain = rangeLin;
            }

            const float smoothCoeff = (targetGain > gateGain) ? (1.0f - attackCoeff) : (1.0f - releaseCoeff);
            gateGain += smoothCoeff * (targetGain - gateGain);

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * gateGain);
        }

        endBypassProcess(buffer);
    }

private:
    float coefficientForTime(float milliseconds) const noexcept
    {
        const auto timeInSeconds = juce::jmax(0.0001f, milliseconds * 0.001f);
        return std::exp(-1.0f / (timeInSeconds * (float)juce::jmax(1.0, currentSampleRate)));
    }

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> thresholdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> holdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> releaseSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;

    juce::AudioParameterFloat* thresholdParam = nullptr;
    juce::AudioParameterFloat* attackParam = nullptr;
    juce::AudioParameterFloat* holdParam = nullptr;
    juce::AudioParameterFloat* releaseParam = nullptr;
    juce::AudioParameterFloat* rangeParam = nullptr;

    double currentSampleRate = 44100.0;
    float envelope = 0.0f;
    float gateGain = 0.0f;
    int holdCounter = 0;
    bool isPrepared = false;
};
