#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

namespace Nova { namespace DistortionDSP {

inline float blend(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void setHighPass(float freq, float q, double sr) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = (1.0f + cosW0) * 0.5f * a0inv;
        b1 = -(1.0f + cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setLowPass(float freq, float q, double sr) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = (1.0f - cosW0) * 0.5f * a0inv;
        b1 = (1.0f - cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setLowShelf(float freq, float gainDb, float q, double sr) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
        const float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha);

        b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
    }

    void setHighShelf(float freq, float gainDb, float q, double sr) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
        const float a0inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha);

        b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
    }

    void setPeak(float freq, float gainDb, float q, double sr) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha / A);

        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * cosW0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = b1;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline float softClip(float x, float drive) noexcept
{
    return std::tanh(x * drive) / juce::jmax(1.0f, drive * 0.72f);
}

inline float asymSoftClip(float x, float posDrive, float negDrive) noexcept
{
    return x >= 0.0f
        ? std::tanh(x * posDrive) / juce::jmax(1.0f, posDrive * 0.72f)
        : std::tanh(x * negDrive) / juce::jmax(1.0f, negDrive * 0.72f);
}

inline float diodeClip(float x, float posThreshold, float negThreshold) noexcept
{
    if (x > posThreshold)
        return posThreshold + (x - posThreshold) / (1.0f + std::abs(x - posThreshold));
    if (x < -negThreshold)
        return -negThreshold + (x + negThreshold) / (1.0f + std::abs(x + negThreshold));
    return x;
}

inline float ledClip(float x, float threshold) noexcept
{
    if (x > threshold)
        return threshold + std::tanh((x - threshold) * 0.72f);
    if (x < -threshold)
        return -threshold + std::tanh((x + threshold) * 0.72f);
    return x;
}

struct ModeConfig
{
    float preMidFreq = 900.0f;
    float preMidGainDb = 2.0f;
    float contourFreq = 900.0f;
    float contourGainDb = 0.0f;
    float stage1Pos = 1.6f;
    float stage1Neg = 1.3f;
    float stage2Pos = 2.2f;
    float stage2Neg = 1.8f;
    float diodeBlend = 0.5f;
    float ledBlend = 0.0f;
    float ampBlend = 0.4f;
    float harmonic = 0.03f;
    float sagAmount = 0.0f;
    float outputTrim = 1.0f;
    float highShelfDb = 0.0f;
};

inline ModeConfig makeVintageConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 980.0f;
    config.preMidGainDb = 2.8f;
    config.contourFreq = 760.0f;
    config.contourGainDb = -1.8f;
    config.stage1Pos = 1.75f;
    config.stage1Neg = 1.34f;
    config.stage2Pos = 2.45f;
    config.stage2Neg = 1.85f;
    config.diodeBlend = 0.72f;
    config.ledBlend = 0.0f;
    config.ampBlend = 0.28f;
    config.harmonic = 0.035f;
    config.sagAmount = 0.04f;
    config.outputTrim = 0.92f;
    config.highShelfDb = -0.8f;
    return config;
}

inline ModeConfig makeTurboConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 860.0f;
    config.preMidGainDb = 1.5f;
    config.contourFreq = 820.0f;
    config.contourGainDb = 1.0f;
    config.stage1Pos = 1.55f;
    config.stage1Neg = 1.45f;
    config.stage2Pos = 1.95f;
    config.stage2Neg = 1.92f;
    config.diodeBlend = 0.26f;
    config.ledBlend = 0.64f;
    config.ampBlend = 0.30f;
    config.harmonic = 0.028f;
    config.sagAmount = 0.02f;
    config.outputTrim = 0.98f;
    config.highShelfDb = 0.4f;
    return config;
}

inline ModeConfig makeAmpConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 1080.0f;
    config.preMidGainDb = 3.6f;
    config.contourFreq = 1120.0f;
    config.contourGainDb = 2.4f;
    config.stage1Pos = 1.90f;
    config.stage1Neg = 1.55f;
    config.stage2Pos = 2.15f;
    config.stage2Neg = 1.76f;
    config.diodeBlend = 0.18f;
    config.ledBlend = 0.0f;
    config.ampBlend = 0.68f;
    config.harmonic = 0.045f;
    config.sagAmount = 0.10f;
    config.outputTrim = 0.88f;
    config.highShelfDb = -0.3f;
    return config;
}

inline ModeConfig getModeConfig(int modeIndex) noexcept
{
    switch (modeIndex)
    {
        case 1:  return makeTurboConfig();
        case 2:  return makeAmpConfig();
        default: return makeVintageConfig();
    }
}

}} // namespace Nova::DistortionDSP

class DistortionPedal final : public ProcessorBase
{
public:
    DistortionPedal()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam = new juce::AudioParameterFloat("distGain", "Gain", 0.0f, 100.0f, 58.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("distTone", "Tone", 0.0f, 1.0f, 0.54f));
        addParameter(bodyParam = new juce::AudioParameterFloat("distBody", "Body", 0.0f, 1.0f, 0.52f));
        addParameter(mixParam = new juce::AudioParameterFloat("distMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("distLevel", "Level", 0.0f, 1.0f, 0.64f));
        addParameter(tightParam = new juce::AudioParameterFloat("distTight", "Tight", 0.0f, 1.0f, 0.42f));
        addParameter(modeParam = new juce::AudioParameterChoice("distMode",
            "Mode",
            juce::StringArray{ "Vintage", "Turbo", "Amp" },
            0));
    }

    const juce::String getName() const override { return "Distortion"; }
    double getTailLengthSeconds() const override { return 0.06; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        innerSr = sr * 8.0;

        oversampler.reset();
        oversampler.initProcessing((size_t) juce::jmax(1, samplesPerBlock));

        gainSmooth.reset(sr, Nova::Config::SMOOTH_DRIVE_SECONDS);
        mixSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        tightSmooth.reset(sr, 0.05);

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? gainParam->get() : 58.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 0.64f);
        tightSmooth.setCurrentAndTargetValue(tightParam != nullptr ? tightParam->get() : 0.42f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

        for (auto& filter : inputHP)
            filter.reset();
        for (auto& filter : preContour)
            filter.reset();
        for (auto& filter : interLP1)
            filter.reset();
        for (auto& filter : interLP2)
            filter.reset();
        for (auto& filter : bodyShelf)
            filter.reset();
        for (auto& filter : modeContour)
            filter.reset();
        for (auto& filter : toneLP)
            filter.reset();
        for (auto& filter : toneShelf)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        sagEnvelope.fill(0.0f);
        sagAttackCoeff = std::exp(-1.0f / ((float) innerSr * 0.0015f));
        sagReleaseCoeff = std::exp(-1.0f / ((float) innerSr * 0.055f));

        cachedTone = -999.0f;
        cachedBody = -999.0f;
        cachedGain = -999.0f;
        cachedTight = -999.0f;
        cachedMode = -1;

        setProcessingLatency((int) std::round(oversampler.getLatencyInSamples()));
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

        for (auto& filter : inputHP)
            filter.reset();
        for (auto& filter : preContour)
            filter.reset();
        for (auto& filter : interLP1)
            filter.reset();
        for (auto& filter : interLP2)
            filter.reset();
        for (auto& filter : bodyShelf)
            filter.reset();
        for (auto& filter : modeContour)
            filter.reset();
        for (auto& filter : toneLP)
            filter.reset();
        for (auto& filter : toneShelf)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        sagEnvelope.fill(0.0f);
        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? gainParam->get() : 58.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 0.64f);
        tightSmooth.setCurrentAndTargetValue(tightParam != nullptr ? tightParam->get() : 0.42f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        if (scratchBuffer.getNumChannels() < numChannels
            || scratchBuffer.getNumSamples() < numSamples)
        {
            scratchBuffer.setSize(numChannels, numSamples, false, false, true);
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                numSamples);

        gainSmooth.setTargetValue(gainParam != nullptr ? gainParam->get() : 58.0f);
        mixSmooth.setTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelParam->get() : 0.64f);
        tightSmooth.setTargetValue(tightParam != nullptr ? tightParam->get() : 0.42f);
        updateFilters();

        const auto config = Nova::DistortionDSP::getModeConfig(modeParam != nullptr ? modeParam->getIndex() : 0);
        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int innerChannels = (int) upsampled.getNumChannels();
        const int innerSamples = (int) upsampled.getNumSamples();

        for (int s = 0; s < innerSamples; ++s)
        {
            const float gainCtl = gainSmooth.getNextValue();
            const float tightCtl = tightSmooth.getNextValue();
            const float driveNorm = juce::jlimit(0.0f, 1.0f, gainCtl * 0.01f);

            for (int ch = 0; ch < innerChannels; ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t) ch);
                float x = data[s];

                x = inputHP[(size_t) ch].process(x);
                x = preContour[(size_t) ch].process(x);

                const float detector = std::abs(x);
                auto& envelope = sagEnvelope[(size_t) ch];
                const float sagCoeff = detector > envelope ? sagAttackCoeff : sagReleaseCoeff;
                envelope = detector + sagCoeff * (envelope - detector);
                const float sagGain = 1.0f - config.sagAmount * juce::jlimit(0.0f, 1.0f, envelope * 0.72f);

                const float stage1Drive = juce::Decibels::decibelsToGain(4.0f + gainCtl * 0.28f) * sagGain;
                const float stage2Drive = 1.15f + driveNorm * 2.15f + tightCtl * 0.14f;
                const float bias = (config.contourGainDb > 0.0f ? 0.03f : -0.015f) * driveNorm;

                x = Nova::DistortionDSP::asymSoftClip(x * stage1Drive + bias,
                    config.stage1Pos,
                    config.stage1Neg);
                x = interLP1[(size_t) ch].process(x);

                const float stage2In = x * stage2Drive;
                const float ampVoice = Nova::DistortionDSP::asymSoftClip(stage2In,
                    config.stage2Pos,
                    config.stage2Neg);
                const float silicon = Nova::DistortionDSP::diodeClip(stage2In,
                    juce::jmap(driveNorm, 0.0f, 1.0f, 0.96f, 0.36f),
                    juce::jmap(driveNorm, 0.0f, 1.0f, 1.02f, 0.52f));
                const float led = Nova::DistortionDSP::ledClip(stage2In,
                    juce::jmap(driveNorm, 0.0f, 1.0f, 1.18f, 0.62f));

                x = ampVoice * config.ampBlend
                    + silicon * config.diodeBlend
                    + led * config.ledBlend;
                x = interLP2[(size_t) ch].process(x);

                const float harmonic = config.harmonic * (0.35f + driveNorm * 0.85f);
                x += harmonic * std::sin(x * juce::MathConstants<float>::pi);

                x = bodyShelf[(size_t) ch].process(x);
                x = modeContour[(size_t) ch].process(x);
                x = toneShelf[(size_t) ch].process(x);
                x = toneLP[(size_t) ch].process(x);
                x = dcBlock[(size_t) ch].process(x);

                data[s] = x * config.outputTrim;
            }
        }

        oversampler.processSamplesDown(block);

        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        for (int s = 0; s < numSamples; ++s)
        {
            const float mix = mixSmooth.getNextValue();
            const float level = juce::jmap(juce::jlimit(0.0f, 1.0f, levelSmooth.getNextValue()), 0.08f, 1.6f);
            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, s);
                const float wet = buffer.getSample(ch, s);
                buffer.setSample(ch, s, (dry * dryGain + wet * wetGain) * level);
            }
        }

        endBypassProcess(buffer);
    }

    static float toneToCutoffHz(float tone) noexcept
    {
        return 2300.0f + juce::jlimit(0.0f, 1.0f, tone) * 10700.0f;
    }

    static float bodyToGainDb(float body) noexcept
    {
        return juce::jmap(juce::jlimit(0.0f, 1.0f, body), -6.0f, 7.5f);
    }

    static float tightToCutoffHz(float tight) noexcept
    {
        return 35.0f + juce::jlimit(0.0f, 1.0f, tight) * 145.0f;
    }

    static juce::String getModeDescription(int modeIndex)
    {
        switch (modeIndex)
        {
            case 1:  return "Higher headroom clipping with broader lows and a more open upper bite";
            case 2:  return "Amp-voiced saturation with tighter palm mutes, mids forward and more sag";
            default: return "Classic silicon-forward distortion with sharp edge and controlled scoop";
        }
    }

    static float computeClipCurve(float inputLevel, float gainPct, int modeIndex)
    {
        const auto config = Nova::DistortionDSP::getModeConfig(modeIndex);
        const float driveNorm = juce::jlimit(0.0f, 1.0f, gainPct * 0.01f);
        const float stage1Drive = juce::Decibels::decibelsToGain(4.0f + gainPct * 0.28f);
        const float stage2Drive = 1.15f + driveNorm * 2.15f;
        const float bias = (config.contourGainDb > 0.0f ? 0.03f : -0.015f) * driveNorm;

        float x = Nova::DistortionDSP::asymSoftClip(inputLevel * stage1Drive + bias,
            config.stage1Pos,
            config.stage1Neg);
        const float stage2In = x * stage2Drive;
        const float ampVoice = Nova::DistortionDSP::asymSoftClip(stage2In,
            config.stage2Pos,
            config.stage2Neg);
        const float silicon = Nova::DistortionDSP::diodeClip(stage2In,
            juce::jmap(driveNorm, 0.0f, 1.0f, 0.96f, 0.36f),
            juce::jmap(driveNorm, 0.0f, 1.0f, 1.02f, 0.52f));
        const float led = Nova::DistortionDSP::ledClip(stage2In,
            juce::jmap(driveNorm, 0.0f, 1.0f, 1.18f, 0.62f));

        x = ampVoice * config.ampBlend
            + silicon * config.diodeBlend
            + led * config.ledBlend;
        x += config.harmonic * (0.35f + driveNorm * 0.85f) * std::sin(x * juce::MathConstants<float>::pi);
        return juce::jlimit(-1.0f, 1.0f, x * config.outputTrim);
    }

    juce::AudioParameterFloat* gainParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* bodyParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterChoice* modeParam = nullptr;

private:
    void updateFilters()
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 0.54f;
        const float body = bodyParam != nullptr ? bodyParam->get() : 0.52f;
        const float gain = gainParam != nullptr ? gainParam->get() : 58.0f;
        const float tight = tightParam != nullptr ? tightParam->get() : 0.42f;
        const int modeIndex = modeParam != nullptr ? modeParam->getIndex() : 0;

        const bool changed = std::abs(cachedTone - tone) > 1.0e-4f
            || std::abs(cachedBody - body) > 1.0e-4f
            || std::abs(cachedGain - gain) > 0.35f
            || std::abs(cachedTight - tight) > 1.0e-4f
            || cachedMode != modeIndex;
        if (!changed)
            return;

        cachedTone = tone;
        cachedBody = body;
        cachedGain = gain;
        cachedTight = tight;
        cachedMode = modeIndex;

        const auto config = Nova::DistortionDSP::getModeConfig(modeIndex);
        const float driveNorm = juce::jlimit(0.0f, 1.0f, gain * 0.01f);
        const float hpFreq = tightToCutoffHz(tight) + driveNorm * 18.0f + (modeIndex == 2 ? 12.0f : 0.0f);
        const float hpQ = modeIndex == 2 ? 0.78f : 0.72f;
        const float toneCutoff = toneToCutoffHz(tone) - driveNorm * 1000.0f;
        const float bodyGain = bodyToGainDb(body);
        const float interCut1 = juce::jmap(driveNorm, 0.0f, 1.0f, 15500.0f, 8400.0f);
        const float interCut2 = juce::jmap(driveNorm, 0.0f, 1.0f, 12500.0f, 6200.0f);
        const float highShelfDb = config.highShelfDb + juce::jmap(tone, -4.5f, 5.0f);

        for (auto& filter : inputHP)
            filter.setHighPass(hpFreq, hpQ, innerSr);
        for (auto& filter : preContour)
            filter.setPeak(config.preMidFreq, config.preMidGainDb + driveNorm * 1.2f, 0.85f, innerSr);
        for (auto& filter : interLP1)
            filter.setLowPass(interCut1, 0.75f, innerSr);
        for (auto& filter : interLP2)
            filter.setLowPass(interCut2, 0.72f, innerSr);
        for (auto& filter : bodyShelf)
            filter.setLowShelf(180.0f, bodyGain, 0.78f, innerSr);
        for (auto& filter : modeContour)
            filter.setPeak(config.contourFreq, config.contourGainDb, 0.95f, innerSr);
        for (auto& filter : toneShelf)
            filter.setHighShelf(2500.0f, highShelfDb, 0.72f, innerSr);
        for (auto& filter : toneLP)
            filter.setLowPass(toneCutoff, 0.68f, innerSr);
        for (auto& filter : dcBlock)
            filter.setHighPass(18.0f, 0.707f, innerSr);
    }

    double sr = 44100.0;
    double innerSr = 352800.0;

    juce::dsp::Oversampling<float> oversampler;

    std::array<Nova::DistortionDSP::Biquad, 2> inputHP;
    std::array<Nova::DistortionDSP::Biquad, 2> preContour;
    std::array<Nova::DistortionDSP::Biquad, 2> interLP1;
    std::array<Nova::DistortionDSP::Biquad, 2> interLP2;
    std::array<Nova::DistortionDSP::Biquad, 2> bodyShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> modeContour;
    std::array<Nova::DistortionDSP::Biquad, 2> toneLP;
    std::array<Nova::DistortionDSP::Biquad, 2> toneShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tightSmooth;
    juce::AudioBuffer<float> scratchBuffer;
    std::array<float, 2> sagEnvelope {};
    float sagAttackCoeff = 0.0f;
    float sagReleaseCoeff = 0.0f;

    float cachedTone = -999.0f;
    float cachedBody = -999.0f;
    float cachedGain = -999.0f;
    float cachedTight = -999.0f;
    int cachedMode = -1;
    bool isPrepared = false;
};

#include "DistortionEditor.h"
