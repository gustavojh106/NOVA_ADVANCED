#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <limits>

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
        addParameter(resonanceParam = new juce::AudioParameterFloat("hgResonance", "Resonance", 0.0f, 1.0f, 0.46f));
        addParameter(feelParam = new juce::AudioParameterFloat("hgFeel", "Feel", 0.0f, 1.0f, 0.55f));
        addParameter(levelParam = new juce::AudioParameterFloat("hgLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "High Gain Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumHardwareEditor(*this,
            PremiumHardwareEditor::Skin::Amplifier,
            "Amplifier",
            "High Gain Amp",
            "Inferno channel / tight power",
            juce::Colour::fromString("ffDC2626"),
            {
                { "Drive", driveParam, [](float value) { return formatGain(value); } },
                { "Tone", toneParam, [](float value) { return formatPercent(value); } },
                { "Presence", presenceParam, [](float value) { return formatPercent(value); } },
                { "Tight", tightParam, [](float value) { return formatPercent(value); } },
                { "Resonance", resonanceParam, [](float value) { return formatPercent(value); } },
                { "Feel", feelParam, [](float value) { return formatPercent(value); } },
                { "Master", levelParam, [](float value) { return formatGain(value); } }
            },
            640,
            326);
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
        resonanceShelf.prepare(innerSpec);
        contourLowPass.prepare(innerSpec);
        presenceShelf.prepare(innerSpec);
        postPresenceLowPass.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        driveSmooth.reset(currentInnerRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        masterSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 5.5f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        cachedTone = std::numeric_limits<float>::quiet_NaN();
        cachedPresence = std::numeric_limits<float>::quiet_NaN();
        cachedTight = std::numeric_limits<float>::quiet_NaN();
        cachedResonance = std::numeric_limits<float>::quiet_NaN();

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
        resonanceShelf.reset();
        contourLowPass.reset();
        presenceShelf.reset();
        postPresenceLowPass.reset();
        dcBlock.reset();

        for (auto& env : sagEnvelope)
            env = 0.0f;
        for (auto& env : inputNoiseEnvelope)
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
            const float feel = readFeel();
            std::array<float*, kMaxAmpChannels> channelData{};
            const int channelsToProcess = juce::jmin(channels, kMaxAmpChannels);
            for (int ch = 0; ch < channelsToProcess; ++ch)
                channelData[(size_t) ch] = upsampled.getChannelPointer((size_t) ch);

            for (int sample = 0; sample < samples; ++sample)
            {
                const float drive = driveSmooth.getNextValue();
                const float pushGain = 1.5f + drive * 1.8f;

                for (int ch = 0; ch < channelsToProcess; ++ch)
                {
                    auto* data = channelData[(size_t) ch];
                    float x = data[sample];

                    x *= computeInputNoiseRejectGain(x, ch, drive, feel);
                    x = conditionPreampInput(x);

                    // Power supply sag
                    sagEnvelope[(size_t)ch] = juce::jmax(std::abs(x), sagEnvelope[(size_t)ch] * Nova::Config::AMP_SAG_DECAY);
                    const float sagDepth = 0.18f + (1.0f - feel) * 0.08f;
                    const float sag = 1.0f / (1.0f + sagEnvelope[(size_t)ch] * sagDepth);

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

        resonanceShelf.process(context);
        contourLowPass.process(context);
        presenceShelf.process(context);
        postPresenceLowPass.process(context);
        dcBlock.process(context);
        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float master = masterSmooth.getNextValue() * kProfessionalOutputTrim;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * master);
        }

        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    static float conditionPreampInput(float x) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        const float ax = std::abs(x);
        if (ax <= 0.72f)
            return x;

        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float extra = ax - 0.72f;
        return sign * (0.72f + std::atan(extra * 0.82f) * 0.54f);
    }

    float computeInputNoiseRejectGain(float input, int channel, float drive, float feel) noexcept
    {
        const size_t index = (size_t) juce::jlimit(0, kMaxAmpChannels - 1, channel);
        const float absolute = std::abs(input);
        auto& env = inputNoiseEnvelope[index];
        const float coeff = absolute > env ? 0.08f : 0.0022f;
        env += (absolute - env) * coeff;

        const float driveNorm = juce::jlimit(0.0f, 1.0f, (drive - 1.0f) / 9.0f);
        const float feelNorm = juce::jlimit(0.0f, 1.0f, feel);
        const float openThreshold = (0.00012f + driveNorm * 0.00042f) * (1.15f - feelNorm * 0.28f);
        const float closeThreshold = openThreshold * 0.30f;
        const float zone = juce::jlimit(0.0f, 1.0f, (env - closeThreshold) / juce::jmax(1.0e-7f, openThreshold - closeThreshold));
        const float smoothZone = zone * zone * (3.0f - 2.0f * zone);
        const float floor = juce::jmap(driveNorm, 0.18f, 0.07f) * (0.90f + feelNorm * 0.18f);
        return floor + smoothZone * (1.0f - floor);
    }

    float readFeel() const noexcept
    {
        const float value = feelParam != nullptr ? feelParam->get() : 0.55f;
        return std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.55f;
    }

    void updateVoicingIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.55f;
        const float presence = presenceParam != nullptr ? *presenceParam : 0.6f;
        const float tight = tightParam != nullptr ? *tightParam : 0.5f;
        const float resonance = resonanceParam != nullptr ? *resonanceParam : 0.46f;

        const bool toneChanged = !std::isfinite(cachedTone) || std::abs(cachedTone - tone) > 1.0e-4f;
        const bool presenceChanged = !std::isfinite(cachedPresence) || std::abs(cachedPresence - presence) > 1.0e-4f;
        const bool tightChanged = !std::isfinite(cachedTight) || std::abs(cachedTight - tight) > 1.0e-4f;
        const bool resonanceChanged = !std::isfinite(cachedResonance) || std::abs(cachedResonance - resonance) > 1.0e-4f;
        if (!toneChanged && !presenceChanged && !tightChanged && !resonanceChanged)
            return;

        cachedTone = tone;
        cachedPresence = presence;
        cachedTight = tight;
        cachedResonance = resonance;

        const float hpFreq = juce::jmap(tight, 45.0f, 180.0f);
        const float cutoff = juce::jmap(tone, 2400.0f, 7800.0f);
        const float presenceGain = juce::jmap(presence, -2.0f, 4.7f);
        const float resonanceGain = juce::jmap(resonance, -2.0f, 3.5f);
        const float boostGain = juce::jmap(tight, 0.0f, 3.0f);
        const float postPresenceCutoff = juce::jlimit(4700.0f,
            8200.0f,
            juce::jmap(tone, 5200.0f, 7600.0f) - presence * 900.0f);

        *inputHighPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentInnerRate, hpFreq);
        *tightFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentInnerRate,
            juce::jmap(tight, 80.0f, 220.0f), 0.6f);
        *preBoost.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(currentInnerRate,
            1800.0f, 0.7f, juce::Decibels::decibelsToGain(boostGain));
        *resonanceShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf(currentInnerRate,
            125.0f, 0.72f, juce::Decibels::decibelsToGain(resonanceGain));
        *contourLowPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(currentInnerRate, cutoff, 0.62f);
        *presenceShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(currentInnerRate,
            2800.0f, 0.68f, juce::Decibels::decibelsToGain(presenceGain));
        *postPresenceLowPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(currentInnerRate,
            postPresenceCutoff, 0.70f);
        *dcBlock.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter inputHighPass;
    Filter tightFilter;
    Filter preBoost;
    Filter resonanceShelf;
    Filter contourLowPass;
    Filter presenceShelf;
    Filter postPresenceLowPass;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    static constexpr int kMaxAmpChannels = 8;
    static constexpr float kProfessionalOutputTrim = 0.32f;
    std::array<float, kMaxAmpChannels> sagEnvelope{};
    std::array<float, kMaxAmpChannels> inputNoiseEnvelope{};

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* presenceParam = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* feelParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerRate = 352800.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float cachedPresence = std::numeric_limits<float>::quiet_NaN();
    float cachedTight = std::numeric_limits<float>::quiet_NaN();
    float cachedResonance = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
