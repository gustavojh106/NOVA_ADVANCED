#pragma once

#include "Base/ProcessorBase.h"
#include "Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>

class ChorusPedal final : public ProcessorBase
{
public:
    ChorusPedal()
    {
        addParameter(rateParam = new juce::AudioParameterFloat("chorusRate", "Rate", 0.05f, 5.0f, 0.85f));
        addParameter(depthParam = new juce::AudioParameterFloat("chorusDepth", "Depth", 0.0f, 1.0f, 0.56f));
        addParameter(widthParam = new juce::AudioParameterFloat("chorusWidth", "Width", 0.0f, 1.0f, 0.68f));
        addParameter(toneParam = new juce::AudioParameterFloat("chorusTone", "Tone", 900.0f, 12000.0f, 6800.0f));
        addParameter(mixParam = new juce::AudioParameterFloat("chorusMix", "Mix", 0.0f, 1.0f, 0.34f));
    }

    const juce::String getName() const override { return "Chorus"; }
    double getTailLengthSeconds() const override { return 0.35; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Modulation",
            "Stereo Chorus",
            juce::Colour::fromString("ff818CF8"),
            {
                { "Rate", rateParam, [](float value) { return juce::String(value, 2) + " Hz"; } },
                { "Depth", depthParam, [](float value) { return formatPercent(value); } },
                { "Width", widthParam, [](float value) { return formatPercent(value); } },
                { "Tone", toneParam, [](float value) { return formatHertz(value); } },
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
        maxDelaySamples = juce::jmax(1, juce::roundToInt(sampleRate * 0.08));
        delayBuffer.setSize(2, maxDelaySamples + juce::jmax(1, samplesPerBlock) + 8, false, false, true);
        scratchBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);

        rateSmooth.reset(sampleRate, 0.04);
        depthSmooth.reset(sampleRate, 0.04);
        widthSmooth.reset(sampleRate, 0.04);
        mixSmooth.reset(sampleRate, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.85f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.56f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? *widthParam : 0.68f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.34f);

        for (auto& filter : toneFilters)
        {
            filter.prepare(sampleRate);
            filter.setCutoff(toneParam != nullptr ? *toneParam : 6800.0f);
        }

        cachedToneCutoff = std::numeric_limits<float>::quiet_NaN();
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
        delayBuffer.clear();
        scratchBuffer.clear();
        writePosition = 0;
        lfoPhase = 0.0f;

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.85f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.56f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? *widthParam : 0.68f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.34f);

        for (auto& filter : toneFilters)
            filter.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        if (scratchBuffer.getNumChannels() < buffer.getNumChannels()
            || scratchBuffer.getNumSamples() < buffer.getNumSamples())
        {
            scratchBuffer.setSize(buffer.getNumChannels(),
                buffer.getNumSamples(),
                false,
                false,
                true);
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), buffer.getNumSamples());

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 0.85f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.56f);
        widthSmooth.setTargetValue(widthParam != nullptr ? *widthParam : 0.68f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.34f);
        updateToneFiltersIfNeeded();

        const int bufferLength = delayBuffer.getNumSamples();
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float quarterTurn = 0.25f;

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float rate = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float width = widthSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            const float baseDelaySamples = (float)(currentSampleRate * 0.0115);
            const float excursionSamples = depth * (float)(currentSampleRate * 0.0065);

            const float phaseLeft = lfoPhase;
            const float phaseRight = std::fmod(lfoPhase + quarterTurn + (width * 0.15f), 1.0f);
            const float modLeft = 0.5f + 0.5f * std::sin(twoPi * phaseLeft);
            const float modRight = 0.5f + 0.5f * std::sin(twoPi * phaseRight);

            const float delayLeft = juce::jlimit(1.0f,
                (float)(maxDelaySamples - 3),
                baseDelaySamples + excursionSamples * (0.45f + modLeft));
            const float delayRight = juce::jlimit(1.0f,
                (float)(maxDelaySamples - 3),
                baseDelaySamples + excursionSamples * (0.45f + modRight));

            const float voiceLeft = readDelaySample(0, delayLeft, bufferLength);
            const float voiceRight = readDelaySample(1, delayRight, bufferLength);
            const float widthCrossfeed = width * 0.22f;

            const float inLeft = scratchBuffer.getSample(0, sample);
            const float inRight = numChannels > 1 ? scratchBuffer.getSample(1, sample) : inLeft;

            delayBuffer.setSample(0, writePosition, inLeft + (voiceRight * 0.08f));
            delayBuffer.setSample(1, writePosition, inRight + (voiceLeft * 0.08f));

            const float wetLeft = toneFilters[0].process((voiceLeft * (1.0f - widthCrossfeed)) + (voiceRight * widthCrossfeed));
            const float wetRight = toneFilters[1].process((voiceRight * (1.0f - widthCrossfeed)) + (voiceLeft * widthCrossfeed));

            const float dryGain = std::cos(mix * juce::MathConstants<float>::halfPi);
            const float wetGain = std::sin(mix * juce::MathConstants<float>::halfPi);

            buffer.setSample(0, sample, (inLeft * dryGain) + (wetLeft * wetGain));
            if (numChannels > 1)
                buffer.setSample(1, sample, (inRight * dryGain) + (wetRight * wetGain));

            writePosition = (writePosition + 1) % juce::jmax(1, bufferLength);
            lfoPhase += rate / (float)juce::jmax(1.0, currentSampleRate);
            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

private:
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
        float a0 = 0.2f;
        float b1 = 0.8f;
        float state = 0.0f;
    };

    void updateToneFiltersIfNeeded()
    {
        const float cutoff = toneParam != nullptr ? *toneParam : 6800.0f;
        if (std::isfinite(cachedToneCutoff) && std::abs(cachedToneCutoff - cutoff) <= 0.5f)
            return;

        cachedToneCutoff = cutoff;
        toneFilters[0].setCutoff(cutoff);
        toneFilters[1].setCutoff(cutoff);
    }

    float readDelaySample(int channel, float delaySamples, int bufferLength) const noexcept
    {
        float readPosition = (float)writePosition - delaySamples;
        while (readPosition < 0.0f)
            readPosition += (float)bufferLength;

        const int indexA = (int)readPosition;
        const int indexB = (indexA + 1) % bufferLength;
        const float frac = readPosition - (float)indexA;

        const float a = delayBuffer.getSample(channel, indexA);
        const float b = delayBuffer.getSample(channel, indexB);
        return a + (b - a) * frac;
    }

    juce::AudioBuffer<float> delayBuffer;
    juce::AudioBuffer<float> scratchBuffer;
    std::array<OnePoleLowPass, 2> toneFilters;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* widthParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    int maxDelaySamples = 1;
    int writePosition = 0;
    float lfoPhase = 0.0f;
    float cachedToneCutoff = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
