#pragma once

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

#include "../../../JuceLibraryCode/BinaryData.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class Vintage2x12Cabinet final : public ProcessorBase
{
public:
    Vintage2x12Cabinet()
    {
        addParameter(warmthParam = new juce::AudioParameterFloat("v2x12Warmth", "Warmth", -12.0f, 12.0f, 3.0f));
        addParameter(sparkleParam = new juce::AudioParameterFloat("v2x12Sparkle", "Sparkle", -12.0f, 12.0f, 1.5f));
        addParameter(distanceParam = new juce::AudioParameterFloat("v2x12Distance", "Distance", 0.0f, 1.0f, 0.28f));
        addParameter(mixParam = new juce::AudioParameterFloat("v2x12Mix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("v2x12Level", "Level", 0.0f, 2.0f, 1.0f));

        loadImpulseResponse();
    }

    const juce::String getName() const override { return "Vintage 2x12"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Cabinet",
            "Vintage 2x12",
            juce::Colour::fromString("ffD97706"),
            {
                { "Warmth", warmthParam, [](float value) { return formatDecibels(value); } },
                { "Sparkle", sparkleParam, [](float value) { return formatDecibels(value); } },
                { "Distance", distanceParam, [](float value) { return formatPercent(value); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } },
                { "Level", levelParam, [](float value) { return formatGain(value); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock);
        spec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        convolution.prepare(spec);
        lowShelf.prepare(spec);
        highShelf.prepare(spec);
        contourLP.prepare(spec);
        contourHP.prepare(spec);

        mixSmooth.reset(sampleRate, 0.02);
        levelSmooth.reset(sampleRate, 0.02);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        currentSampleRate = sampleRate;
        cachedWarmth = std::numeric_limits<float>::quiet_NaN();
        cachedSparkle = std::numeric_limits<float>::quiet_NaN();
        cachedDistance = std::numeric_limits<float>::quiet_NaN();

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
        convolution.reset();
        lowShelf.reset();
        highShelf.reset();
        contourLP.reset();
        contourHP.reset();

        if (mixParam != nullptr)
            mixSmooth.setCurrentAndTargetValue(*mixParam);
        if (levelParam != nullptr)
            levelSmooth.setCurrentAndTargetValue(*levelParam);
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

        updateVoicing();
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        if (convolution.getCurrentIRSize() > 0)
            convolution.process(context);

        lowShelf.process(context);
        highShelf.process(context);
        contourLP.process(context);
        contourHP.process(context);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float dry = 1.0f - mix;
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample);
                buffer.setSample(ch, sample, ((wet * mix) + (clean * dry)) * level);
            }
        }

        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void loadImpulseResponse()
    {
        convolution.loadImpulseResponse(BinaryData::demo_wav,
            (size_t)BinaryData::demo_wavSize,
            juce::dsp::Convolution::Stereo::yes,
            juce::dsp::Convolution::Trim::yes,
            0,
            juce::dsp::Convolution::Normalise::yes);
    }

    void updateVoicing()
    {
        const float warmth = warmthParam != nullptr ? *warmthParam : 3.0f;
        const float sparkle = sparkleParam != nullptr ? *sparkleParam : 1.5f;
        const float distance = distanceParam != nullptr ? *distanceParam : 0.28f;

        const bool warmthChanged = !std::isfinite(cachedWarmth) || std::abs(cachedWarmth - warmth) > 1.0e-4f;
        const bool sparkleChanged = !std::isfinite(cachedSparkle) || std::abs(cachedSparkle - sparkle) > 1.0e-4f;
        const bool distanceChanged = !std::isfinite(cachedDistance) || std::abs(cachedDistance - distance) > 1.0e-4f;
        if (!warmthChanged && !sparkleChanged && !distanceChanged)
            return;

        cachedWarmth = warmth;
        cachedSparkle = sparkle;
        cachedDistance = distance;

        // Vintage 2x12 voicing: warmer, less aggressive high end, tighter bass
        const float lpFreq = juce::jmap(distance, 7200.0f, 3000.0f);
        const float hpFreq = juce::jmap(distance, 70.0f, 160.0f);

        *lowShelf.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentSampleRate,
            180.0f, 0.75f, juce::Decibels::decibelsToGain(warmth));
        *highShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate,
            4200.0f, 0.65f, juce::Decibels::decibelsToGain(sparkle));
        *contourLP.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, lpFreq, 0.62f);
        *contourHP.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, hpFreq, 0.65f);
    }

    juce::dsp::Convolution convolution;
    Filter lowShelf;
    Filter highShelf;
    Filter contourLP;
    Filter contourHP;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* warmthParam = nullptr;
    juce::AudioParameterFloat* sparkleParam = nullptr;
    juce::AudioParameterFloat* distanceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    float cachedWarmth = std::numeric_limits<float>::quiet_NaN();
    float cachedSparkle = std::numeric_limits<float>::quiet_NaN();
    float cachedDistance = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
