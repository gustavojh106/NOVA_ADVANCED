#pragma once

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

#include "../../../JuceLibraryCode/BinaryData.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class CabinetPedal final : public ProcessorBase
{
public:
    CabinetPedal()
    {
        addParameter(thumpParam = new juce::AudioParameterFloat("cabThump", "Thump", -12.0f, 12.0f, 1.5f));
        addParameter(airParam = new juce::AudioParameterFloat("cabAir", "Air", -12.0f, 12.0f, 0.0f));
        addParameter(distanceParam = new juce::AudioParameterFloat("cabDistance", "Distance", 0.0f, 1.0f, 0.34f));
        addParameter(mixParam = new juce::AudioParameterFloat("cabMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("cabLevel", "Level", 0.0f, 2.0f, 1.0f));

        loadImpulseResponse();
    }

    const juce::String getName() const override { return "Cabinet"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Cabinet",
            "Atlas 4x12",
            juce::Colour::fromString("ff6bd1ff"),
            {
                { "Thump", thumpParam, [](float value) { return formatDecibels(value); } },
                { "Air", airParam, [](float value) { return formatDecibels(value); } },
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
        contourLowPass.prepare(spec);
        contourHighPass.prepare(spec);

        mixSmooth.reset(sampleRate, 0.02);
        levelSmooth.reset(sampleRate, 0.02);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

        currentSampleRate = sampleRate;
        cachedThump = std::numeric_limits<float>::quiet_NaN();
        cachedAir = std::numeric_limits<float>::quiet_NaN();
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
        contourLowPass.reset();
        contourHighPass.reset();

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
            scratchBuffer.setSize(buffer.getNumChannels(),
                buffer.getNumSamples(),
                false,
                false,
                true);
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), buffer.getNumSamples());

        updateCabinetVoicing();
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        if (convolution.getCurrentIRSize() > 0)
            convolution.process(context);

        lowShelf.process(context);
        highShelf.process(context);
        contourLowPass.process(context);
        contourHighPass.process(context);

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

    void updateCabinetVoicing()
    {
        const float thump = thumpParam != nullptr ? *thumpParam : 1.5f;
        const float air = airParam != nullptr ? *airParam : 0.0f;
        const float distance = distanceParam != nullptr ? *distanceParam : 0.34f;

        const bool thumpChanged = !std::isfinite(cachedThump) || std::abs(cachedThump - thump) > 1.0e-4f;
        const bool airChanged = !std::isfinite(cachedAir) || std::abs(cachedAir - air) > 1.0e-4f;
        const bool distanceChanged = !std::isfinite(cachedDistance) || std::abs(cachedDistance - distance) > 1.0e-4f;
        if (!thumpChanged && !airChanged && !distanceChanged)
            return;

        cachedThump = thump;
        cachedAir = air;
        cachedDistance = distance;

        const float lowPass = juce::jmap(distance, 9800.0f, 3600.0f);
        const float highPass = juce::jmap(distance, 55.0f, 140.0f);

        *lowShelf.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentSampleRate,
            130.0f,
            0.8f,
            juce::Decibels::decibelsToGain(thump));
        *highShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate,
            3400.0f,
            0.72f,
            juce::Decibels::decibelsToGain(air));
        *contourLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate,
            lowPass,
            0.68f);
        *contourHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate,
            highPass,
            0.7f);
    }

    juce::dsp::Convolution convolution;
    Filter lowShelf;
    Filter highShelf;
    Filter contourLowPass;
    Filter contourHighPass;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* thumpParam = nullptr;
    juce::AudioParameterFloat* airParam = nullptr;
    juce::AudioParameterFloat* distanceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    float cachedThump = std::numeric_limits<float>::quiet_NaN();
    float cachedAir = std::numeric_limits<float>::quiet_NaN();
    float cachedDistance = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
