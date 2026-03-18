#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class FuzzPedal final : public ProcessorBase
{
public:
    FuzzPedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(fuzzParam = new juce::AudioParameterFloat("fuzzAmount", "Fuzz", 0.0f, 100.0f, 65.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("fuzzTone", "Tone", 0.0f, 1.0f, 0.5f));
        addParameter(gateParam = new juce::AudioParameterFloat("fuzzGate", "Gate", 0.0f, 1.0f, 0.3f));
        addParameter(mixParam = new juce::AudioParameterFloat("fuzzMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("fuzzLevel", "Level", 0.0f, 1.0f, 0.55f));
    }

    const juce::String getName() const override { return "Fuzz"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Fuzz",
            "Velvet Fuzz",
            juce::Colour::fromString("ffA855F7"),
            {
                { "Fuzz", fuzzParam, [](float value) { return juce::String(juce::roundToInt(value)) + "%"; } },
                { "Tone", toneParam, [](float value) { return formatPercent(value); } },
                { "Gate", gateParam, [](float value) { return formatPercent(value); } },
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

        inputFilter.prepare(innerSpec);
        toneFilter.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        fuzzSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        gateSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        fuzzSmooth.setCurrentAndTargetValue(fuzzParam != nullptr ? *fuzzParam : 65.0f);
        gateSmooth.setCurrentAndTargetValue(gateParam != nullptr ? *gateParam : 0.3f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.55f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        cachedTone = std::numeric_limits<float>::quiet_NaN();

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
        inputFilter.reset();
        toneFilter.reset();
        dcBlock.reset();
        inputEnvelope = 0.0f;

        fuzzSmooth.setCurrentAndTargetValue(fuzzParam != nullptr ? *fuzzParam : 65.0f);
        gateSmooth.setCurrentAndTargetValue(gateParam != nullptr ? *gateParam : 0.3f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.55f);
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

        updateToneIfNeeded();
        fuzzSmooth.setTargetValue(fuzzParam != nullptr ? *fuzzParam : 65.0f);
        gateSmooth.setTargetValue(gateParam != nullptr ? *gateParam : 0.3f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 0.55f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            inputFilter.process(ctx);
        }

        // Fuzz processing - germanium transistor emulation
        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples = (int)upsampled.getNumSamples();

            for (int sample = 0; sample < samples; ++sample)
            {
                const float fuzzAmount = fuzzSmooth.getNextValue();
                const float gateAmount = gateSmooth.getNextValue();
                const float drive = 2.0f + fuzzAmount * 0.48f;
                const float bias = 0.06f + fuzzAmount * 0.002f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = upsampled.getChannelPointer((size_t)ch);
                    float x = data[sample];

                    // Envelope follower for gating
                    const float absX = std::abs(x);
                    inputEnvelope = juce::jmax(absX, inputEnvelope * 0.9995f);
                    const float gateThreshold = gateAmount * 0.02f;
                    const float gateGain = (inputEnvelope > gateThreshold) ? 1.0f : (inputEnvelope / juce::jmax(gateThreshold, 0.0001f));

                    // Germanium-style asymmetric clipping
                    x = x * drive + bias;
                    const float positive = 1.0f - std::exp(-x);
                    const float negative = -1.0f + std::exp(x);
                    x = (x >= 0.0f) ? positive : negative * 0.82f;

                    // Octave-up ghost from full-wave rectification bleed
                    x += std::abs(x) * 0.12f * (fuzzAmount * 0.01f);

                    data[sample] = x * gateGain;
                }
            }
        }

        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            toneFilter.process(ctx);
            dcBlock.process(ctx);
        }

        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float dry = 1.0f - mix;
            const float level = juce::jmap(juce::jlimit(0.0f, 1.0f, levelSmooth.getNextValue()), 0.08f, 1.6f);

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

    void updateToneIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.5f;
        if (std::isfinite(cachedTone) && std::abs(cachedTone - tone) <= 1.0e-4f)
            return;

        cachedTone = tone;
        const float cutoff = juce::jmap(tone, 800.0f, 6500.0f);

        *inputFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 60.0f);
        *toneFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerRate, cutoff, 0.65f);
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter inputFilter;
    Filter toneFilter;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fuzzSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* fuzzParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* gateParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerRate = 176400.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float inputEnvelope = 0.0f;
    bool isPrepared = false;
};
