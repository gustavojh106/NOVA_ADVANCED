#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class AutoWahPedal final : public ProcessorBase
{
public:
    AutoWahPedal()
    {
        addParameter(sensitivityParam = new juce::AudioParameterFloat("wahSens", "Sensitivity", 0.0f, 1.0f, 0.6f));
        addParameter(rangeParam = new juce::AudioParameterFloat("wahRange", "Range", 0.0f, 1.0f, 0.72f));
        addParameter(resonanceParam = new juce::AudioParameterFloat("wahResonance", "Resonance", 0.5f, 8.0f, 3.2f));
        addParameter(decayParam = new juce::AudioParameterFloat("wahDecay", "Decay", 10.0f, 500.0f, 120.0f));
        addParameter(mixParam = new juce::AudioParameterFloat("wahMix", "Mix", 0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "Auto Wah"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Filter",
            "Envelope Filter",
            juce::Colour::fromString("ffF97316"),
            {
                { "Sens", sensitivityParam, [](float value) { return formatPercent(value); } },
                { "Range", rangeParam, [](float value) { return formatPercent(value); } },
                { "Reso", resonanceParam, [](float value) { return juce::String(value, 1) + "x"; } },
                { "Decay", decayParam, [](float value) { return formatMilliseconds(value); } },
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

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        sensSmooth.reset(sampleRate, 0.04);
        rangeSmooth.reset(sampleRate, 0.04);
        resoSmooth.reset(sampleRate, 0.04);
        decaySmooth.reset(sampleRate, 0.04);
        mixSmooth.reset(sampleRate, 0.04);

        sensSmooth.setCurrentAndTargetValue(sensitivityParam != nullptr ? *sensitivityParam : 0.6f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.72f);
        resoSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 3.2f);
        decaySmooth.setCurrentAndTargetValue(decayParam != nullptr ? *decayParam : 120.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        for (auto& s : svfState)
        {
            s.low = 0.0f;
            s.band = 0.0f;
        }

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
        for (auto& s : svfState)
        {
            s.low = 0.0f;
            s.band = 0.0f;
        }

        sensSmooth.setCurrentAndTargetValue(sensitivityParam != nullptr ? *sensitivityParam : 0.6f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.72f);
        resoSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 3.2f);
        decaySmooth.setCurrentAndTargetValue(decayParam != nullptr ? *decayParam : 120.0f);
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

        sensSmooth.setTargetValue(sensitivityParam != nullptr ? *sensitivityParam : 0.6f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? *rangeParam : 0.72f);
        resoSmooth.setTargetValue(resonanceParam != nullptr ? *resonanceParam : 3.2f);
        decaySmooth.setTargetValue(decayParam != nullptr ? *decayParam : 120.0f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        constexpr float minFreq = 250.0f;
        constexpr float maxFreq = 4800.0f;
        constexpr float attackMs = 1.0f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float sensitivity = sensSmooth.getNextValue();
            const float range = rangeSmooth.getNextValue();
            const float resonance = resoSmooth.getNextValue();
            const float decayMs = decaySmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            // Envelope follower
            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                detector = juce::jmax(detector, std::abs(buffer.getSample(ch, sample)));

            const float attackCoeff = std::exp(-1.0f / (attackMs * 0.001f * (float)currentSampleRate));
            const float releaseCoeff = std::exp(-1.0f / (decayMs * 0.001f * (float)currentSampleRate));

            if (detector > envelope)
                envelope = attackCoeff * envelope + (1.0f - attackCoeff) * detector;
            else
                envelope = releaseCoeff * envelope + (1.0f - releaseCoeff) * detector;

            // Map envelope to filter frequency
            const float envelopeScaled = juce::jlimit(0.0f, 1.0f, envelope * sensitivity * 12.0f);
            const float freq = minFreq + (maxFreq - minFreq) * range * envelopeScaled;

            // State Variable Filter (bandpass) - per-sample update
            const float clampedFreq = juce::jlimit(80.0f, (float)(currentSampleRate * 0.4), freq);
            const float g = std::tan(juce::MathConstants<float>::pi * clampedFreq / (float)currentSampleRate);
            const float k = 1.0f / juce::jmax(0.5f, resonance);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, sample);
                const float input = buffer.getSample(ch, sample);

                // SVF tick
                const float hp = (input - (k + g) * svfState[(size_t)ch].band - svfState[(size_t)ch].low)
                    / (1.0f + k * g + g * g);
                const float bp = g * hp + svfState[(size_t)ch].band;
                svfState[(size_t)ch].band = g * hp + bp;
                svfState[(size_t)ch].low += g * bp;

                // Use bandpass output scaled by resonance
                const float wet = bp * resonance * 0.7f;
                buffer.setSample(ch, sample, dry * (1.0f - mix) + wet * mix);
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

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sensSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resoSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decaySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* sensitivityParam = nullptr;
    juce::AudioParameterFloat* rangeParam = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* decayParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    float envelope = 0.0f;
    bool isPrepared = false;
};
