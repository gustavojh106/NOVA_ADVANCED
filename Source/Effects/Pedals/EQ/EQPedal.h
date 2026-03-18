#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class EQPedal final : public ProcessorBase
{
public:
    EQPedal()
    {
        addParameter(lowParam = new juce::AudioParameterFloat("eqLow", "Low", -12.0f, 12.0f, 0.0f));
        addParameter(midParam = new juce::AudioParameterFloat("eqMid", "Mid", -12.0f, 12.0f, 0.0f));
        addParameter(highParam = new juce::AudioParameterFloat("eqHigh", "High", -12.0f, 12.0f, 0.0f));
        addParameter(midFreqParam = new juce::AudioParameterFloat("eqMidFreq", "Mid Freq", 200.0f, 5000.0f, 800.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("eqLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "EQ"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "EQ",
            "3-Band EQ",
            juce::Colour::fromString("ff4ADE80"),
            {
                { "Low", lowParam, [](float value) { return formatDecibels(value); } },
                { "Mid", midParam, [](float value) { return formatDecibels(value); } },
                { "High", highParam, [](float value) { return formatDecibels(value); } },
                { "Mid Freq", midFreqParam, [](float value) { return formatHertz(value); } },
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

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock);
        spec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        lowShelf.prepare(spec);
        midPeak.prepare(spec);
        highShelf.prepare(spec);

        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        cachedLow = std::numeric_limits<float>::quiet_NaN();
        cachedMid = std::numeric_limits<float>::quiet_NaN();
        cachedHigh = std::numeric_limits<float>::quiet_NaN();
        cachedMidFreq = std::numeric_limits<float>::quiet_NaN();

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
        lowShelf.reset();
        midPeak.reset();
        highShelf.reset();

        if (levelParam != nullptr)
            levelSmooth.setCurrentAndTargetValue(*levelParam);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateFiltersIfNeeded();
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        lowShelf.process(context);
        midPeak.process(context);
        highShelf.process(context);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float level = levelSmooth.getNextValue();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * level);
        }

        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateFiltersIfNeeded()
    {
        const float low = lowParam != nullptr ? *lowParam : 0.0f;
        const float mid = midParam != nullptr ? *midParam : 0.0f;
        const float high = highParam != nullptr ? *highParam : 0.0f;
        const float midFreq = midFreqParam != nullptr ? *midFreqParam : 800.0f;

        const bool lowChanged = !std::isfinite(cachedLow) || std::abs(cachedLow - low) > 0.01f;
        const bool midChanged = !std::isfinite(cachedMid) || std::abs(cachedMid - mid) > 0.01f;
        const bool highChanged = !std::isfinite(cachedHigh) || std::abs(cachedHigh - high) > 0.01f;
        const bool midFreqChanged = !std::isfinite(cachedMidFreq) || std::abs(cachedMidFreq - midFreq) > 0.5f;

        if (!lowChanged && !midChanged && !highChanged && !midFreqChanged)
            return;

        cachedLow = low;
        cachedMid = mid;
        cachedHigh = high;
        cachedMidFreq = midFreq;

        *lowShelf.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentSampleRate,
            250.0f, 0.72f, juce::Decibels::decibelsToGain(low));
        *midPeak.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate,
            midFreq, 1.0f, juce::Decibels::decibelsToGain(mid));
        *highShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate,
            3500.0f, 0.72f, juce::Decibels::decibelsToGain(high));
    }

    Filter lowShelf;
    Filter midPeak;
    Filter highShelf;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    juce::AudioParameterFloat* lowParam = nullptr;
    juce::AudioParameterFloat* midParam = nullptr;
    juce::AudioParameterFloat* highParam = nullptr;
    juce::AudioParameterFloat* midFreqParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    float cachedLow = std::numeric_limits<float>::quiet_NaN();
    float cachedMid = std::numeric_limits<float>::quiet_NaN();
    float cachedHigh = std::numeric_limits<float>::quiet_NaN();
    float cachedMidFreq = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
