#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>

class OctavePedal final : public ProcessorBase
{
public:
    OctavePedal()
    {
        addParameter(subParam = new juce::AudioParameterFloat("octaveSub", "Sub", 0.0f, 1.0f, 0.72f));
        addParameter(upperParam = new juce::AudioParameterFloat("octaveUpper", "Upper", 0.0f, 1.0f, 0.28f));
        addParameter(dryParam = new juce::AudioParameterFloat("octaveDry", "Dry", 0.0f, 1.0f, 0.82f));
        addParameter(toneParam = new juce::AudioParameterFloat("octaveTone", "Tone", 600.0f, 8000.0f, 2800.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("octaveLevel", "Level", 0.5f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Octave"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Pitch",
            "Octave Divider",
            juce::Colour::fromString("ff22C55E"),
            {
                { "Sub", subParam, [](float value) { return formatPercent(value); } },
                { "Upper", upperParam, [](float value) { return formatPercent(value); } },
                { "Dry", dryParam, [](float value) { return formatPercent(value); } },
                { "Tone", toneParam, [](float value) { return formatHertz(value); } },
                { "Level", levelParam, [](float value) { return formatGain(value); } }
            },
            220,
            186);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;

        subSmooth.reset(sampleRate, 0.04);
        upperSmooth.reset(sampleRate, 0.04);
        drySmooth.reset(sampleRate, 0.04);
        levelSmooth.reset(sampleRate, 0.04);

        subSmooth.setCurrentAndTargetValue(subParam != nullptr ? *subParam : 0.72f);
        upperSmooth.setCurrentAndTargetValue(upperParam != nullptr ? *upperParam : 0.28f);
        drySmooth.setCurrentAndTargetValue(dryParam != nullptr ? *dryParam : 0.82f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        for (auto& filter : inputHighPass)
            filter.prepare(sampleRate);

        for (auto& filter : octaveToneFilters)
            filter.prepare(sampleRate);

        cachedTone = std::numeric_limits<float>::quiet_NaN();

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
        lastInput.fill(0.0f);
        subEnvelope.fill(0.0f);
        dividerState.fill(false);

        subSmooth.setCurrentAndTargetValue(subParam != nullptr ? *subParam : 0.72f);
        upperSmooth.setCurrentAndTargetValue(upperParam != nullptr ? *upperParam : 0.28f);
        drySmooth.setCurrentAndTargetValue(dryParam != nullptr ? *dryParam : 0.82f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        for (auto& filter : inputHighPass)
            filter.reset();

        for (auto& filter : octaveToneFilters)
            filter.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        subSmooth.setTargetValue(subParam != nullptr ? *subParam : 0.72f);
        upperSmooth.setTargetValue(upperParam != nullptr ? *upperParam : 0.28f);
        drySmooth.setTargetValue(dryParam != nullptr ? *dryParam : 0.82f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
        updateToneIfNeeded();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float subAmount = subSmooth.getNextValue();
            const float upperAmount = upperSmooth.getNextValue();
            const float dryAmount = drySmooth.getNextValue();
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                const float input = inputHighPass[(size_t)ch].process(dry);
                const float absInput = std::abs(input);

                subEnvelope[(size_t)ch] = juce::jmax(absInput, subEnvelope[(size_t)ch] * 0.992f);
                if (lastInput[(size_t)ch] <= 0.0f && input > 0.0f)
                    dividerState[(size_t)ch] = !dividerState[(size_t)ch];

                lastInput[(size_t)ch] = input;

                const float subCarrier = dividerState[(size_t)ch] ? subEnvelope[(size_t)ch] : -subEnvelope[(size_t)ch];
                const float upper = juce::jlimit(-1.0f, 1.0f, (std::abs(input) * 1.7f) - 0.18f);
                const float wet = octaveToneFilters[(size_t)ch].process((subCarrier * subAmount * 0.9f) + (upper * upperAmount));

                buffer.setSample(ch, sample, ((dry * dryAmount) + wet) * level);
            }
        }

        endBypassProcess(buffer);
    }

private:
    struct OnePoleHighPass
    {
        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            setCutoff(45.0f);
            reset();
        }

        void reset()
        {
            x1 = 0.0f;
            y1 = 0.0f;
        }

        void setCutoff(float hz)
        {
            const auto cutoff = juce::jlimit(20.0f, (float)(sampleRate * 0.45), hz);
            pole = (float)std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
        }

        float process(float x) noexcept
        {
            const float y = x - x1 + (pole * y1);
            x1 = x;
            y1 = y;
            return y;
        }

        double sampleRate = 44100.0;
        float pole = 0.99f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    struct OnePoleLowPass
    {
        void prepare(double newSampleRate)
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            reset();
        }

        void reset()
        {
            state = 0.0f;
        }

        void setCutoff(float hz)
        {
            const auto cutoff = juce::jlimit(20.0f, (float)(sampleRate * 0.45), hz);
            const auto coeff = std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
            b1 = (float)coeff;
            a0 = 1.0f - b1;
        }

        float process(float x) noexcept
        {
            state = (a0 * x) + (b1 * state);
            return state;
        }

        double sampleRate = 44100.0;
        float a0 = 0.22f;
        float b1 = 0.78f;
        float state = 0.0f;
    };

    void updateToneIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 2800.0f;
        if (std::isfinite(cachedTone) && std::abs(cachedTone - tone) <= 0.5f)
            return;

        cachedTone = tone;
        octaveToneFilters[0].setCutoff(tone);
        octaveToneFilters[1].setCutoff(tone);
    }

    std::array<OnePoleHighPass, 2> inputHighPass;
    std::array<OnePoleLowPass, 2> octaveToneFilters;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> subSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> upperSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> drySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    juce::AudioParameterFloat* subParam = nullptr;
    juce::AudioParameterFloat* upperParam = nullptr;
    juce::AudioParameterFloat* dryParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    std::array<float, 2> lastInput{};
    std::array<float, 2> subEnvelope{};
    std::array<bool, 2> dividerState{};
    double currentSampleRate = 44100.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
