#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <cmath>

class ReverbPedal final : public ProcessorBase
{
public:
    ReverbPedal()
    {
        addParameter(sizeParam = new juce::AudioParameterFloat("reverbSize", "Size", 0.0f, 1.0f, 0.62f));
        addParameter(dampingParam = new juce::AudioParameterFloat("reverbDamping", "Damping", 0.0f, 1.0f, 0.38f));
        addParameter(toneParam = new juce::AudioParameterFloat("reverbTone", "Tone", 1200.0f, 12000.0f, 6200.0f));
        addParameter(preDelayParam = new juce::AudioParameterFloat("reverbPredelay", "Predelay", 0.0f, 120.0f, 24.0f));
        addParameter(mixParam = new juce::AudioParameterFloat("reverbMix", "Mix", 0.0f, 1.0f, 0.26f));
    }

    const juce::String getName() const override { return "Reverb"; }
    double getTailLengthSeconds() const override { return 6.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Reverb",
            "Nimbus",
            juce::Colour::fromString("ff98d9d1"),
            {
                { "Size", sizeParam, [](float value) { return formatPercent(value); } },
                { "Damping", dampingParam, [](float value) { return formatPercent(value); } },
                { "Tone", toneParam, [](float value) { return formatHertz(value); } },
                { "Predelay", preDelayParam, [](float value) { return formatMilliseconds(value); } },
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
        maxPredelaySamples = juce::jmax(1, (int)std::ceil(sampleRate * 0.16));
        predelayBuffer.setSize(2, maxPredelaySamples + juce::jmax(1, samplesPerBlock) + 8, false, false, true);
        wetBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
        dryBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock);
        spec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());
        reverb.prepare(spec);

        preDelaySmooth.reset(sampleRate, 0.04);
        mixSmooth.reset(sampleRate, 0.04);
        preDelaySmooth.setCurrentAndTargetValue(preDelayParam != nullptr ? *preDelayParam : 24.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.26f);

        for (auto& filter : toneFilters)
        {
            filter.prepare(sampleRate);
            filter.setCutoff(toneParam != nullptr ? *toneParam : 6200.0f);
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
        predelayBuffer.clear();
        wetBuffer.clear();
        dryBuffer.clear();
        predelayWritePos = 0;
        reverb.reset();

        for (auto& filter : toneFilters)
            filter.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        ensureScratchBuffers(buffer.getNumSamples());

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            juce::FloatVectorOperations::copy(dryBuffer.getWritePointer(ch), buffer.getReadPointer(ch), buffer.getNumSamples());
            juce::FloatVectorOperations::clear(wetBuffer.getWritePointer(ch), buffer.getNumSamples());
        }

        juce::dsp::Reverb::Parameters params;
        params.roomSize = sizeParam != nullptr ? *sizeParam : 0.62f;
        params.damping = dampingParam != nullptr ? *dampingParam : 0.38f;
        params.width = 1.0f;
        params.freezeMode = 0.0f;
        params.wetLevel = 1.0f;
        params.dryLevel = 0.0f;
        reverb.setParameters(params);

        preDelaySmooth.setTargetValue(preDelayParam != nullptr ? *preDelayParam : 24.0f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.26f);

        const float toneCutoff = toneParam != nullptr ? *toneParam : 6200.0f;
        toneFilters[0].setCutoff(toneCutoff);
        toneFilters[1].setCutoff(toneCutoff);

        const int predelayLength = predelayBuffer.getNumSamples();

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float predelaySamples = juce::jlimit(0.0f,
                (float)(maxPredelaySamples - 2),
                preDelaySmooth.getNextValue() * (float)currentSampleRate * 0.001f);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float input = dryBuffer.getSample(ch, sample);
                wetBuffer.setSample(ch, sample, readPredelaySample(ch, predelaySamples, predelayLength));
                predelayBuffer.setSample(ch, predelayWritePos, input);
            }

            if (++predelayWritePos >= predelayLength)
                predelayWritePos = 0;
        }

        juce::dsp::AudioBlock<float> wetBlock(wetBuffer);
        auto wetSubBlock = wetBlock.getSubBlock(0, (size_t)buffer.getNumSamples());
        juce::dsp::ProcessContextReplacing<float> wetContext(wetSubBlock);
        reverb.process(wetContext);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();

            const float wetL = toneFilters[0].process(wetBuffer.getSample(0, sample));
            buffer.setSample(0, sample, juce::jmap(mix, dryBuffer.getSample(0, sample), wetL));

            if (buffer.getNumChannels() > 1)
            {
                const float wetR = toneFilters[1].process(wetBuffer.getSample(1, sample));
                buffer.setSample(1, sample, juce::jmap(mix, dryBuffer.getSample(1, sample), wetR));
            }
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

    void ensureScratchBuffers(int numSamples)
    {
        if (wetBuffer.getNumSamples() < numSamples)
            wetBuffer.setSize(2, numSamples, false, false, true);

        if (dryBuffer.getNumSamples() < numSamples)
            dryBuffer.setSize(2, numSamples, false, false, true);
    }

    float readPredelaySample(int channel, float delayInSamples, int bufferLength) const noexcept
    {
        float readPosition = (float)predelayWritePos - delayInSamples;
        while (readPosition < 0.0f)
            readPosition += (float)bufferLength;

        const int indexA = (int)readPosition;
        const int indexB = (indexA + 1) % bufferLength;
        const float frac = readPosition - (float)indexA;

        const float a = predelayBuffer.getSample(channel, indexA);
        const float b = predelayBuffer.getSample(channel, indexB);
        return a + (b - a) * frac;
    }

    juce::dsp::Reverb reverb;
    juce::AudioBuffer<float> predelayBuffer;
    juce::AudioBuffer<float> wetBuffer;
    juce::AudioBuffer<float> dryBuffer;
    std::array<OnePoleLowPass, 2> toneFilters;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> preDelaySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioParameterFloat* sizeParam = nullptr;
    juce::AudioParameterFloat* dampingParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* preDelayParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    int maxPredelaySamples = 1;
    int predelayWritePos = 0;
    bool isPrepared = false;
};
