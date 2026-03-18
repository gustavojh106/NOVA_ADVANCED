#pragma once

#include "Base/ProcessorBase.h"
#include "Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

class CompressorPedal final : public ProcessorBase
{
public:
    CompressorPedal()
    {
        addParameter(thresholdParam = new juce::AudioParameterFloat("compThreshold", "Threshold", -42.0f, 0.0f, -20.0f));
        addParameter(ratioParam = new juce::AudioParameterFloat("compRatio", "Ratio", 1.0f, 12.0f, 4.0f));
        addParameter(attackParam = new juce::AudioParameterFloat("compAttack", "Attack", 1.0f, 80.0f, 12.0f));
        addParameter(releaseParam = new juce::AudioParameterFloat("compRelease", "Release", 25.0f, 350.0f, 120.0f));
        addParameter(blendParam = new juce::AudioParameterFloat("compBlend", "Blend", 0.0f, 1.0f, 0.82f));
        addParameter(makeupParam = new juce::AudioParameterFloat("compMakeup", "Makeup", 0.0f, 18.0f, 2.5f));
    }

    const juce::String getName() const override { return "Compressor"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Dynamics",
            "Studio Comp",
            juce::Colour::fromString("ff34D399"),
            {
                { "Thresh", thresholdParam, [](float value) { return formatDecibels(value); } },
                { "Ratio", ratioParam, [](float value) { return juce::String(value, 1) + ":1"; } },
                { "Attack", attackParam, [](float value) { return formatMilliseconds(value); } },
                { "Release", releaseParam, [](float value) { return formatMilliseconds(value); } },
                { "Blend", blendParam, [](float value) { return formatPercent(value); } },
                { "Makeup", makeupParam, [](float value) { return formatDecibels(value); } }
            },
            220,
            186);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;
        dryBuffer.setSize(juce::jmax(1, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

        thresholdSmooth.reset(sampleRate, 0.05);
        ratioSmooth.reset(sampleRate, 0.05);
        attackSmooth.reset(sampleRate, 0.05);
        releaseSmooth.reset(sampleRate, 0.05);
        blendSmooth.reset(sampleRate, 0.04);
        makeupSmooth.reset(sampleRate, 0.05);

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? *thresholdParam : -20.0f);
        ratioSmooth.setCurrentAndTargetValue(ratioParam != nullptr ? *ratioParam : 4.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 12.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? *releaseParam : 120.0f);
        blendSmooth.setCurrentAndTargetValue(blendParam != nullptr ? *blendParam : 0.82f);
        makeupSmooth.setCurrentAndTargetValue(makeupParam != nullptr ? *makeupParam : 2.5f);

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
        linkedEnvelope = 0.0f;

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? *thresholdParam : -20.0f);
        ratioSmooth.setCurrentAndTargetValue(ratioParam != nullptr ? *ratioParam : 4.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 12.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? *releaseParam : 120.0f);
        blendSmooth.setCurrentAndTargetValue(blendParam != nullptr ? *blendParam : 0.82f);
        makeupSmooth.setCurrentAndTargetValue(makeupParam != nullptr ? *makeupParam : 2.5f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        if (dryBuffer.getNumChannels() < buffer.getNumChannels()
            || dryBuffer.getNumSamples() < buffer.getNumSamples())
        {
            dryBuffer.setSize(buffer.getNumChannels(),
                buffer.getNumSamples(),
                false,
                false,
                true);
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(dryBuffer.getWritePointer(ch), buffer.getReadPointer(ch), buffer.getNumSamples());

        thresholdSmooth.setTargetValue(thresholdParam != nullptr ? *thresholdParam : -20.0f);
        ratioSmooth.setTargetValue(ratioParam != nullptr ? *ratioParam : 4.0f);
        attackSmooth.setTargetValue(attackParam != nullptr ? *attackParam : 12.0f);
        releaseSmooth.setTargetValue(releaseParam != nullptr ? *releaseParam : 120.0f);
        blendSmooth.setTargetValue(blendParam != nullptr ? *blendParam : 0.82f);
        makeupSmooth.setTargetValue(makeupParam != nullptr ? *makeupParam : 2.5f);

        constexpr float kneeWidthDb = 6.0f;
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float thresholdDb = thresholdSmooth.getNextValue();
            const float ratio = juce::jmax(1.0f, ratioSmooth.getNextValue());
            const float blend = blendSmooth.getNextValue();
            const float makeupGain = juce::Decibels::decibelsToGain(makeupSmooth.getNextValue());

            const float attackCoeff = coefficientForTime(attackSmooth.getNextValue());
            const float releaseCoeff = coefficientForTime(releaseSmooth.getNextValue());

            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                detector = juce::jmax(detector, std::abs(buffer.getSample(ch, sample)));

            if (detector > linkedEnvelope)
                linkedEnvelope = (attackCoeff * linkedEnvelope) + ((1.0f - attackCoeff) * detector);
            else
                linkedEnvelope = (releaseCoeff * linkedEnvelope) + ((1.0f - releaseCoeff) * detector);

            const float envDb = juce::Decibels::gainToDecibels(linkedEnvelope + 1.0e-6f, -96.0f);
            const float reductionDb = computeGainReductionDb(envDb, thresholdDb, ratio, kneeWidthDb);
            const float gain = juce::Decibels::decibelsToGain(-reductionDb) * makeupGain;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = dryBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample) * gain;
                buffer.setSample(ch, sample, juce::jmap(blend, dry, wet));
            }
        }

        endBypassProcess(buffer);
    }

private:
    float coefficientForTime(float milliseconds) const noexcept
    {
        const auto timeInSeconds = juce::jmax(0.001f, milliseconds * 0.001f);
        return std::exp(-1.0f / (timeInSeconds * (float)juce::jmax(1.0, currentSampleRate)));
    }

    static float computeGainReductionDb(float inputDb, float thresholdDb, float ratio, float kneeWidthDb) noexcept
    {
        const float overDb = inputDb - thresholdDb;
        const float slope = 1.0f - (1.0f / juce::jmax(1.0f, ratio));

        if (overDb <= -0.5f * kneeWidthDb)
            return 0.0f;

        if (overDb >= 0.5f * kneeWidthDb)
            return slope * overDb;

        const float kneeProgress = overDb + (0.5f * kneeWidthDb);
        return slope * kneeProgress * kneeProgress / (2.0f * kneeWidthDb);
    }

    juce::AudioBuffer<float> dryBuffer;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> thresholdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> ratioSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> releaseSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> blendSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> makeupSmooth;

    juce::AudioParameterFloat* thresholdParam = nullptr;
    juce::AudioParameterFloat* ratioParam = nullptr;
    juce::AudioParameterFloat* attackParam = nullptr;
    juce::AudioParameterFloat* releaseParam = nullptr;
    juce::AudioParameterFloat* blendParam = nullptr;
    juce::AudioParameterFloat* makeupParam = nullptr;

    double currentSampleRate = 44100.0;
    float linkedEnvelope = 0.0f;
    bool isPrepared = false;
};
