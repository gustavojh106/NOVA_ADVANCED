#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>
#include <vector>

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

class HighGainAmp final : public ProcessorBase
{
public:
    HighGainAmp()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("hgDrive", "Drive", 1.0f, 10.0f, 5.5f));
        addParameter(toneParam = new juce::AudioParameterFloat("hgTone", "Tone", 0.0f, 1.0f, 0.55f));
        addParameter(presenceParam = new juce::AudioParameterFloat("hgPresence", "Presence", 0.0f, 1.0f, 0.6f));
        addParameter(tightParam = new juce::AudioParameterFloat("hgTight", "Tight", 0.0f, 1.0f, 0.5f));
        addParameter(levelParam = new juce::AudioParameterFloat("hgLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "High Gain Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Amplifier",
            "Inferno",
            juce::Colour::fromString("ffDC2626"),
            {
                { "Drive", driveParam, [](float value) { return formatGain(value); } },
                { "Tone", toneParam, [](float value) { return formatPercent(value); } },
                { "Presence", presenceParam, [](float value) { return formatPercent(value); } },
                { "Tight", tightParam, [](float value) { return formatPercent(value); } },
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

        currentInnerRate = sampleRate * 8.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = currentInnerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 8);
        innerSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputHighPass.prepare(innerSpec);
        tightFilter.prepare(innerSpec);
        preBoost.prepare(innerSpec);
        contourLowPass.prepare(innerSpec);
        presenceShelf.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        driveSmooth.reset(currentInnerRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        masterSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 5.5f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        cachedTone = std::numeric_limits<float>::quiet_NaN();
        cachedPresence = std::numeric_limits<float>::quiet_NaN();
        cachedTight = std::numeric_limits<float>::quiet_NaN();

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
        inputHighPass.reset();
        tightFilter.reset();
        preBoost.reset();
        contourLowPass.reset();
        presenceShelf.reset();
        dcBlock.reset();

        for (auto& env : sagEnvelope)
            env = 0.0f;

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 5.5f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateVoicingIfNeeded();
        driveSmooth.setTargetValue(driveParam != nullptr ? *driveParam : 5.5f);
        masterSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampled);

        inputHighPass.process(context);
        tightFilter.process(context);
        preBoost.process(context);

        // Multi-stage high gain saturation
        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples = (int)upsampled.getNumSamples();
            std::vector<float*> channelData((size_t)channels);
            for (int ch = 0; ch < channels; ++ch)
                channelData[(size_t) ch] = upsampled.getChannelPointer((size_t) ch);

            for (int sample = 0; sample < samples; ++sample)
            {
                const float drive = driveSmooth.getNextValue();
                const float pushGain = 1.5f + drive * 1.8f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = channelData[(size_t) ch];
                    float x = data[sample];

                    // Power supply sag
                    sagEnvelope[(size_t)ch] = juce::jmax(std::abs(x), sagEnvelope[(size_t)ch] * Nova::Config::AMP_SAG_DECAY);
                    const float sag = 1.0f / (1.0f + sagEnvelope[(size_t)ch] * 0.4f);

                    // Stage 1: Preamp tube
                    x = std::tanh((x * pushGain * sag) + 0.05f);

                    // Stage 2: Push-pull
                    const float stage2 = std::tanh(x * (1.8f + drive * 0.3f) - 0.03f);

                    // Stage 3: Power amp
                    const float stage3 = std::tanh((stage2 * 1.5f) + (x * 0.15f));

                    // Stage 4: Extra saturation for high gain
                    const float stage4 = std::tanh(stage3 * (1.0f + drive * 0.12f));

                    data[sample] = (x * 0.12f) + (stage2 * 0.25f) + (stage3 * 0.35f) + (stage4 * 0.52f);
                }
            }
        }

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
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateVoicingIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.55f;
        const float presence = presenceParam != nullptr ? *presenceParam : 0.6f;
        const float tight = tightParam != nullptr ? *tightParam : 0.5f;

        const bool toneChanged = !std::isfinite(cachedTone) || std::abs(cachedTone - tone) > 1.0e-4f;
        const bool presenceChanged = !std::isfinite(cachedPresence) || std::abs(cachedPresence - presence) > 1.0e-4f;
        const bool tightChanged = !std::isfinite(cachedTight) || std::abs(cachedTight - tight) > 1.0e-4f;
        if (!toneChanged && !presenceChanged && !tightChanged)
            return;

        cachedTone = tone;
        cachedPresence = presence;
        cachedTight = tight;

        const float hpFreq = juce::jmap(tight, 45.0f, 180.0f);
        const float cutoff = juce::jmap(tone, 2500.0f, 9000.0f);
        const float presenceGain = juce::jmap(presence, -2.0f, 8.0f);
        const float boostGain = juce::jmap(tight, 0.0f, 4.5f);

        *inputHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, hpFreq);
        *tightFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate,
            juce::jmap(tight, 80.0f, 220.0f), 0.6f);
        *preBoost.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentInnerRate,
            1800.0f, 0.7f, juce::Decibels::decibelsToGain(boostGain));
        *contourLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerRate, cutoff, 0.62f);
        *presenceShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentInnerRate,
            2800.0f, 0.68f, juce::Decibels::decibelsToGain(presenceGain));
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter inputHighPass;
    Filter tightFilter;
    Filter preBoost;
    Filter contourLowPass;
    Filter presenceShelf;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    std::array<float, 2> sagEnvelope{ 0.0f, 0.0f };

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* presenceParam = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerRate = 352800.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float cachedPresence = std::numeric_limits<float>::quiet_NaN();
    float cachedTight = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
