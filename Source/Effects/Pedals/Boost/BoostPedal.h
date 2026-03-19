#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class BoostPedal final : public ProcessorBase
{
public:
    BoostPedal()
    {
        addParameter(gainParam = new juce::AudioParameterFloat("boostGain", "Gain", 0.0f, 24.0f, 8.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("boostTone", "Tone", 0.0f, 1.0f, 0.58f));
        addParameter(tightParam = new juce::AudioParameterFloat("boostTight", "Tight", 0.0f, 1.0f, 0.24f));
        addParameter(levelParam = new juce::AudioParameterFloat("boostLevel", "Level", 0.5f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Boost"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Booster",
            "Micro Boost",
            juce::Colour::fromString("ffEAB308"),
            {
                { "Gain", gainParam, [](float value) { return formatDecibels(value); } },
                { "Tone", toneParam, [](float value) { return formatPercent(value); } },
                { "Tight", tightParam, [](float value) { return formatPercent(value); } },
                { "Level", levelParam, [](float value) { return formatGain(value); } }
            },
            214,
            170);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock);
        spec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputHighPass.prepare(spec);
        presenceShelf.prepare(spec);
        airLowPass.prepare(spec);

        gainSmooth.reset(sampleRate, 0.04);
        levelSmooth.reset(sampleRate, 0.04);
        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 8.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        cachedTone = std::numeric_limits<float>::quiet_NaN();
        cachedTight = std::numeric_limits<float>::quiet_NaN();

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
        inputHighPass.reset();
        presenceShelf.reset();
        airLowPass.reset();

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 8.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateVoicingIfNeeded();
        gainSmooth.setTargetValue(gainParam != nullptr ? *gainParam : 8.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        inputHighPass.process(context);
        presenceShelf.process(context);
        airLowPass.process(context);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float gain = juce::Decibels::decibelsToGain(gainSmooth.getNextValue());
            const float level = levelSmooth.getNextValue();
            const float saturation = juce::jmap(gainSmooth.getCurrentValue(), 0.0f, 24.0f, 0.0f, 0.16f);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                float x = buffer.getSample(ch, sample) * gain;
                const float softened = std::tanh(x);
                x = juce::jmap(saturation, x, softened);
                buffer.setSample(ch, sample, x * level);
            }
        }

        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateVoicingIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.58f;
        const float tight = tightParam != nullptr ? *tightParam : 0.24f;

        const bool toneChanged = !std::isfinite(cachedTone) || std::abs(cachedTone - tone) > 1.0e-4f;
        const bool tightChanged = !std::isfinite(cachedTight) || std::abs(cachedTight - tight) > 1.0e-4f;
        if (!toneChanged && !tightChanged)
            return;

        cachedTone = tone;
        cachedTight = tight;

        const float lowCut = juce::jmap(tight, 28.0f, 220.0f);
        const float presenceDb = juce::jmap(tone, -4.0f, 6.0f);
        const float airCutoff = juce::jmap(tone, 4200.0f, 18000.0f);

        *inputHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, lowCut);
        *presenceShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate,
            1600.0f, 0.72f, juce::Decibels::decibelsToGain(presenceDb));
        *airLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, airCutoff, 0.72f);
    }

    Filter inputHighPass;
    Filter presenceShelf;
    Filter airLowPass;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    juce::AudioParameterFloat* gainParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float cachedTight = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
