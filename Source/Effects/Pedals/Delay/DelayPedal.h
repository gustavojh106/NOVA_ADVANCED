#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <cmath>

class DelayPedal final : public ProcessorBase
{
public:
    DelayPedal()
    {
        addParameter(timeParam = new juce::AudioParameterFloat("delayTime", "Time", 40.0f, 1400.0f, 420.0f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("delayFeedback", "Feedback", 0.0f, 0.9f, 0.42f));
        addParameter(toneParam = new juce::AudioParameterFloat("delayTone", "Tone", 600.0f, 12000.0f, 4800.0f));
        addParameter(spreadParam = new juce::AudioParameterFloat("delaySpread", "Spread", 0.0f, 1.0f, 0.42f));
        addParameter(mixParam = new juce::AudioParameterFloat("delayMix", "Mix", 0.0f, 1.0f, 0.3f));
    }

    const juce::String getName() const override { return "Delay"; }
    double getTailLengthSeconds() const override { return 2.5; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Delay",
            "Orbit",
            juce::Colour::fromString("ff7cb8ff"),
            {
                { "Time", timeParam, [](float value) { return formatMilliseconds(value); } },
                { "Feedback", feedbackParam, [](float value) { return formatPercent(value); } },
                { "Tone", toneParam, [](float value) { return formatHertz(value); } },
                { "Spread", spreadParam, [](float value) { return formatPercent(value); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } }
            },
            246,
            236);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;
        maxDelaySamples = juce::jmax(1, (int)std::ceil(sampleRate * 2.6));
        delayBuffer.setSize(2, maxDelaySamples + juce::jmax(1, samplesPerBlock) + 8, false, false, true);

        timeSmooth.reset(sampleRate, 0.04);
        feedbackSmooth.reset(sampleRate, 0.04);
        spreadSmooth.reset(sampleRate, 0.05);
        mixSmooth.reset(sampleRate, 0.04);

        timeSmooth.setCurrentAndTargetValue(timeParam != nullptr ? *timeParam : 420.0f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.42f);
        spreadSmooth.setCurrentAndTargetValue(spreadParam != nullptr ? *spreadParam : 0.42f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.3f);

        for (auto& filter : toneFilters)
        {
            filter.prepare(sampleRate);
            filter.setCutoff(toneParam != nullptr ? *toneParam : 4800.0f);
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
        delayBuffer.clear();
        delayWritePos = 0;

        for (auto& filter : toneFilters)
            filter.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        timeSmooth.setTargetValue(timeParam != nullptr ? *timeParam : 420.0f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.42f);
        spreadSmooth.setTargetValue(spreadParam != nullptr ? *spreadParam : 0.42f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.3f);

        const float cutoff = toneParam != nullptr ? *toneParam : 4800.0f;
        toneFilters[0].setCutoff(cutoff);
        toneFilters[1].setCutoff(cutoff);

        const int delayBufferLength = delayBuffer.getNumSamples();

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float timeMs = timeSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float spread = spreadSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            const float baseDelaySamples = juce::jlimit(1.0f,
                (float)(maxDelaySamples - 4),
                timeMs * (float)currentSampleRate * 0.001f);
            const float offset = baseDelaySamples * 0.16f * spread;
            const float delayLeft = juce::jlimit(1.0f, (float)(maxDelaySamples - 4), baseDelaySamples - offset);
            const float delayRight = juce::jlimit(1.0f, (float)(maxDelaySamples - 4), baseDelaySamples + offset);

            const float wetL = readDelaySample(0, delayLeft, delayBufferLength);
            const float wetR = readDelaySample(1, delayRight, delayBufferLength);

            const float crossfeed = spread * 0.82f;
            const float fbL = toneFilters[0].process((wetL * (1.0f - crossfeed)) + (wetR * crossfeed));
            const float fbR = toneFilters[1].process((wetR * (1.0f - crossfeed)) + (wetL * crossfeed));

            const float inL = buffer.getSample(0, sample);
            const float inR = buffer.getNumChannels() > 1 ? buffer.getSample(1, sample) : inL;

            delayBuffer.setSample(0, delayWritePos, std::tanh(inL + (fbL * feedback)));
            delayBuffer.setSample(1, delayWritePos, std::tanh(inR + (fbR * feedback)));

            buffer.setSample(0, sample, juce::jmap(mix, inL, wetL));
            if (buffer.getNumChannels() > 1)
                buffer.setSample(1, sample, juce::jmap(mix, inR, wetR));

            if (++delayWritePos >= delayBufferLength)
                delayWritePos = 0;
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
        float a0 = 0.15f;
        float b1 = 0.85f;
        float state = 0.0f;
    };

    float readDelaySample(int channel, float delayInSamples, int bufferLength) const noexcept
    {
        float readPosition = (float)delayWritePos - delayInSamples;
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
    std::array<OnePoleLowPass, 2> toneFilters;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> timeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> spreadSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* timeParam = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* spreadParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    int maxDelaySamples = 1;
    int delayWritePos = 0;
    bool isPrepared = false;
};
