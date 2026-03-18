#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class DistortionPedal final : public ProcessorBase
{
public:
    DistortionPedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam = new juce::AudioParameterFloat("distGain", "Gain", 0.0f, 100.0f, 55.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("distTone", "Tone", 0.0f, 1.0f, 0.52f));
        addParameter(bodyParam = new juce::AudioParameterFloat("distBody", "Body", 0.0f, 1.0f, 0.5f));
        addParameter(mixParam = new juce::AudioParameterFloat("distMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("distLevel", "Level", 0.0f, 1.0f, 0.62f));
    }

    const juce::String getName() const override { return "Distortion"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Distortion",
            "Shred",
            juce::Colour::fromString("ffEF4444"),
            {
                { "Gain", gainParam, [](float value) { return juce::String(juce::roundToInt(value)) + "%"; } },
                { "Tone", toneParam, [](float value) { return formatPercent(value); } },
                { "Body", bodyParam, [](float value) { return formatPercent(value); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } },
                { "Level", levelParam, [](float value) { return formatPercent(value); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        currentInnerRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = currentInnerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 4);
        innerSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        preHighPass.prepare(innerSpec);
        postLowPass.prepare(innerSpec);
        bodyFilter.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        gainSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 55.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.62f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        cachedTone = std::numeric_limits<float>::quiet_NaN();
        cachedBody = std::numeric_limits<float>::quiet_NaN();

        setProcessingLatency((int)oversampler.getLatencyInSamples());
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
        oversampler.reset();
        preHighPass.reset();
        postLowPass.reset();
        bodyFilter.reset();
        dcBlock.reset();

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 55.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.62f);
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

        updateFiltersIfNeeded();
        gainSmooth.setTargetValue(gainParam != nullptr ? *gainParam : 55.0f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 0.62f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        // Pre-filter
        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            preHighPass.process(ctx);
        }

        // Distortion stage - hard clipping with asymmetric saturation
        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples = (int)upsampled.getNumSamples();

            for (int sample = 0; sample < samples; ++sample)
            {
                const float gainDb = gainSmooth.getNextValue();
                const float drive = juce::Decibels::decibelsToGain(gainDb * 0.45f + 6.0f);

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = upsampled.getChannelPointer((size_t)ch);
                    float x = data[sample] * drive;

                    // Hard clip with asymmetric soft clipping
                    const float pos = std::tanh(x * 1.4f);
                    const float neg = std::tanh(x * 1.1f);
                    x = (x >= 0.0f) ? pos : neg;

                    // Add grit via wave folding
                    if (std::abs(x) > 0.7f)
                        x = x - 0.3f * std::sin(x * 3.14159f);

                    data[sample] = x;
                }
            }
        }

        // Post-filter
        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            bodyFilter.process(ctx);
            postLowPass.process(ctx);
            dcBlock.process(ctx);
        }

        oversampler.processSamplesDown(block);

        // Mix and level
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float dry = 1.0f - mix;
            const float level = juce::jmap(juce::jlimit(0.0f, 1.0f, levelSmooth.getNextValue()), 0.08f, 1.55f);

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

    void updateFiltersIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.52f;
        const float body = bodyParam != nullptr ? *bodyParam : 0.5f;

        const bool toneChanged = !std::isfinite(cachedTone) || std::abs(cachedTone - tone) > 1.0e-4f;
        const bool bodyChanged = !std::isfinite(cachedBody) || std::abs(cachedBody - body) > 1.0e-4f;
        if (!toneChanged && !bodyChanged)
            return;

        cachedTone = tone;
        cachedBody = body;

        const float cutoff = juce::jmap(tone, 2200.0f, 10000.0f);
        const float bodyGain = juce::jmap(body, -4.0f, 6.0f);

        *preHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 55.0f);
        *postLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerRate, cutoff, 0.68f);
        *bodyFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentInnerRate,
            200.0f, 0.75f, juce::Decibels::decibelsToGain(bodyGain));
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter preHighPass;
    Filter postLowPass;
    Filter bodyFilter;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* gainParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* bodyParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerRate = 176400.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float cachedBody = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
