#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>

class FlangerPedal final : public ProcessorBase
{
public:
    FlangerPedal()
    {
        addParameter(rateParam = new juce::AudioParameterFloat("flangerRate", "Rate", 0.05f, 5.0f, 0.35f));
        addParameter(depthParam = new juce::AudioParameterFloat("flangerDepth", "Depth", 0.0f, 1.0f, 0.64f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("flangerFeedback", "Feedback", -0.95f, 0.95f, 0.55f));
        addParameter(toneParam = new juce::AudioParameterFloat("flangerTone", "Tone", 1000.0f, 14000.0f, 8000.0f));
        addParameter(mixParam = new juce::AudioParameterFloat("flangerMix", "Mix", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "Flanger"; }
    double getTailLengthSeconds() const override { return 0.3; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Modulation",
            "Jet Flange",
            juce::Colour::fromString("ff38BDF8"),
            {
                { "Rate", rateParam, [](float value) { return juce::String(value, 2) + " Hz"; } },
                { "Depth", depthParam, [](float value) { return formatPercent(value); } },
                { "Feedback", feedbackParam, [](float value) { return juce::String(juce::roundToInt(value * 100.0f)) + "%"; } },
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
        maxDelaySamples = juce::jmax(1, juce::roundToInt(sampleRate * 0.02));
        delayBuffer.setSize(2, maxDelaySamples + juce::jmax(1, samplesPerBlock) + 8, false, false, true);

        rateSmooth.reset(sampleRate, 0.04);
        depthSmooth.reset(sampleRate, 0.04);
        feedbackSmooth.reset(sampleRate, 0.04);
        mixSmooth.reset(sampleRate, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.35f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.64f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.55f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        for (auto& f : toneFilters)
        {
            f.prepare(sampleRate);
            f.setCutoff(toneParam != nullptr ? *toneParam : 8000.0f);
        }

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
        delayBuffer.clear();
        writePosition = 0;
        lfoPhase = 0.0f;
        feedbackState.fill(0.0f);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.35f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.64f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.55f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        for (auto& f : toneFilters)
            f.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 0.35f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.64f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.55f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.5f);
        updateToneFiltersIfNeeded();

        const int bufferLength = delayBuffer.getNumSamples();
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float baseDelayMs = 1.0f;
        constexpr float maxExcursionMs = 7.0f;

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float rate = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            const float lfo = 0.5f + 0.5f * std::sin(twoPi * lfoPhase);
            const float delaySamples = juce::jlimit(1.0f,
                (float)(maxDelaySamples - 3),
                (baseDelayMs + maxExcursionMs * depth * lfo) * (float)currentSampleRate * 0.001f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                const float wet = toneFilters[(size_t)ch].process(readDelaySample(ch, delaySamples, bufferLength));

                delayBuffer.setSample(ch, writePosition,
                    std::tanh(dry + feedbackState[(size_t)ch] * feedback));
                feedbackState[(size_t)ch] = wet;

                buffer.setSample(ch, sample, dry * (1.0f - mix) + wet * mix);
            }

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

        void reset() { state = 0.0f; }

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
        float a0 = 0.2f, b1 = 0.8f, state = 0.0f;
    };

    void updateToneFiltersIfNeeded()
    {
        const float cutoff = toneParam != nullptr ? *toneParam : 8000.0f;
        if (std::isfinite(cachedTone) && std::abs(cachedTone - cutoff) <= 0.5f)
            return;

        cachedTone = cutoff;
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

        return delayBuffer.getSample(channel, indexA)
            + (delayBuffer.getSample(channel, indexB) - delayBuffer.getSample(channel, indexA)) * frac;
    }

    juce::AudioBuffer<float> delayBuffer;
    std::array<OnePoleLowPass, 2> toneFilters;
    std::array<float, 2> feedbackState{};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    int maxDelaySamples = 1;
    int writePosition = 0;
    float lfoPhase = 0.0f;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
