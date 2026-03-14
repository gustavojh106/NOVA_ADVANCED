#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

class ClassicAmp final : public ProcessorBase
{
public:
    ClassicAmp()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("ampDrive", "Drive", 0.5f, 6.0f, 2.8f));
        addParameter(toneParam = new juce::AudioParameterFloat("ampTone", "Tone", 0.0f, 1.0f, 0.58f));
        addParameter(presenceParam = new juce::AudioParameterFloat("ampPresence", "Presence", 0.0f, 1.0f, 0.55f));
        addParameter(depthParam = new juce::AudioParameterFloat("ampDepth", "Depth", 0.0f, 1.0f, 0.46f));
        addParameter(levelParam = new juce::AudioParameterFloat("ampLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Classic Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Amplifier",
            "Classic",
            juce::Colour::fromString("fff4b942"),
            {
                { "Drive", driveParam, [](float value) { return formatGain(value); } },
                { "Tone", toneParam, [](float value) { return formatPercent(value); } },
                { "Presence", presenceParam, [](float value) { return formatPercent(value); } },
                { "Depth", depthParam, [](float value) { return formatPercent(value); } },
                { "Master", levelParam, [](float value) { return formatGain(value); } }
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

        inputHighPass.prepare(innerSpec);
        depthShelf.prepare(innerSpec);
        ampCore.prepare(innerSpec);
        contourLowPass.prepare(innerSpec);
        presenceShelf.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        masterSmooth.reset(sampleRate, 0.02);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
        cachedTone = std::numeric_limits<float>::quiet_NaN();
        cachedPresence = std::numeric_limits<float>::quiet_NaN();
        cachedDepth = std::numeric_limits<float>::quiet_NaN();

        setLatencySamples((int)oversampler.getLatencyInSamples());
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
        inputHighPass.reset();
        depthShelf.reset();
        contourLowPass.reset();
        presenceShelf.reset();
        dcBlock.reset();
        ampCore.reset();

        if (driveParam != nullptr)
            ampCore.setDriveTarget(*driveParam);

        if (levelParam != nullptr)
            masterSmooth.setCurrentAndTargetValue(*levelParam);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateVoicingIfNeeded();
        ampCore.setDriveTarget(driveParam != nullptr ? *driveParam : 2.8f);
        masterSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampled);

        inputHighPass.process(context);
        depthShelf.process(context);
        ampCore.process(context);
        contourLowPass.process(context);
        presenceShelf.process(context);
        dcBlock.process(context);
        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float master = masterSmooth.getNextValue();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * master);
        }

        endBypassProcess(buffer);
    }

private:
    class PremiumAmpCore
    {
    public:
        void prepare(const juce::dsp::ProcessSpec& spec)
        {
            driveSmooth.reset(spec.sampleRate, 0.012);
            reset();
        }

        void reset()
        {
            for (auto& env : sagEnvelope)
                env = 0.0f;
        }

        void setDriveTarget(float drive)
        {
            driveSmooth.setTargetValue(1.2f + drive * 1.75f);
        }

        template <typename ProcessContext>
        void process(const ProcessContext& context) noexcept
        {
            auto&& block = context.getOutputBlock();
            const int channels = (int)block.getNumChannels();
            const int samples = (int)block.getNumSamples();

            for (int sample = 0; sample < samples; ++sample)
            {
                const float drive = driveSmooth.getNextValue();
                const float stagePush = 1.7f + drive * 0.22f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = block.getChannelPointer((size_t)ch);
                    const float x = data[sample];

                    sagEnvelope[(size_t)ch] = juce::jmax(std::abs(x), sagEnvelope[(size_t)ch] * 0.9992f);
                    const float sag = 1.0f / (1.0f + sagEnvelope[(size_t)ch] * 0.55f);

                    const float biased = (x * drive * sag) + 0.045f;
                    const float stage1 = std::tanh(biased);
                    const float stage2 = std::tanh((stage1 * stagePush) - 0.035f);
                    const float stage3 = std::tanh((stage2 * 1.35f) + (stage1 * 0.18f));

                    data[sample] = (stage1 * 0.28f) + (stage2 * 0.38f) + (stage3 * 0.64f);
                }
            }
        }

    private:
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
        std::array<float, 2> sagEnvelope{ 0.0f, 0.0f };
    };

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateVoicingIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.58f;
        const float presence = presenceParam != nullptr ? *presenceParam : 0.55f;
        const float depth = depthParam != nullptr ? *depthParam : 0.46f;

        const bool toneChanged = !std::isfinite(cachedTone) || std::abs(cachedTone - tone) > 1.0e-4f;
        const bool presenceChanged = !std::isfinite(cachedPresence) || std::abs(cachedPresence - presence) > 1.0e-4f;
        const bool depthChanged = !std::isfinite(cachedDepth) || std::abs(cachedDepth - depth) > 1.0e-4f;
        if (!toneChanged && !presenceChanged && !depthChanged)
            return;

        cachedTone = tone;
        cachedPresence = presence;
        cachedDepth = depth;

        const float cutoff = juce::jmap(tone, 1800.0f, 8500.0f);
        const float presenceGain = juce::jmap(presence, -1.0f, 6.5f);
        const float depthGain = juce::jmap(depth, -3.0f, 7.5f);

        *inputHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 32.0f);
        *depthShelf.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentInnerRate,
            155.0f,
            0.78f,
            juce::Decibels::decibelsToGain(depthGain));
        *contourLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerRate,
            cutoff,
            0.66f);
        *presenceShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentInnerRate,
            2400.0f,
            0.72f,
            juce::Decibels::decibelsToGain(presenceGain));
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter inputHighPass;
    Filter depthShelf;
    PremiumAmpCore ampCore;
    Filter contourLowPass;
    Filter presenceShelf;
    Filter dcBlock;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* presenceParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerRate = 176400.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float cachedPresence = std::numeric_limits<float>::quiet_NaN();
    float cachedDepth = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
