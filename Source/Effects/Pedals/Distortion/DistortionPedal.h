#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>

namespace Nova { namespace DistortionDSP {

inline float blend(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

inline float clamp01(float value) noexcept
{
    return juce::jlimit(0.0f, 1.0f, value);
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

struct Gate
{
    float envelope = 0.0f;
    float gainLinear = 1.0f;
    bool isOpen = true;

    void reset() noexcept
    {
        envelope = 0.0f;
        gainLinear = 1.0f;
        isOpen = true;
    }

    float process(float inputLevel,
        float openThresholdDb,
        float closeThresholdDb,
        float envelopeAttackCoeff,
        float envelopeReleaseCoeff,
        float gainAttackCoeff,
        float gainReleaseCoeff,
        float floorGain) noexcept
    {
        const float absIn = std::abs(inputLevel);
        const float envCoeff = absIn > envelope ? envelopeAttackCoeff : envelopeReleaseCoeff;
        envelope = absIn + envCoeff * (envelope - absIn);

        const float envDb = juce::Decibels::gainToDecibels(juce::jmax(envelope, 1.0e-6f), -120.0f);
        if (envDb >= openThresholdDb)
            isOpen = true;
        else if (envDb <= closeThresholdDb)
            isOpen = false;

        const float target = isOpen ? 1.0f : juce::jlimit(0.0f, 1.0f, floorGain);
        const float coeff = target > gainLinear ? gainAttackCoeff : gainReleaseCoeff;
        gainLinear = target + coeff * (gainLinear - target);
        return gainLinear;
    }
};

struct ModeConfig
{
    float preMidFreq = 900.0f;
    float preMidGainDb = 2.0f;
    float contourFreq = 900.0f;
    float contourGainDb = 0.0f;
    float contourBodyScale = 0.0f;
    float stage1Pos = 1.6f;
    float stage1Neg = 1.3f;
    float stage2Pos = 2.2f;
    float stage2Neg = 1.8f;
    float stage3Pos = 1.6f;
    float stage3Neg = 1.5f;
    float stage1BaseDb = 4.0f;
    float stage1DrivePerPct = 0.28f;
    float stage2Base = 1.15f;
    float stage2DriveScale = 2.15f;
    float stage2TightScale = 0.14f;
    float stage3Base = 1.0f;
    float stage3DriveScale = 0.70f;
    float stage3Mix = 0.10f;
    float diodeBlend = 0.5f;
    float ledBlend = 0.0f;
    float ampBlend = 0.4f;
    float hardBlend = 0.0f;
    float harmonic = 0.03f;
    float sagAmount = 0.0f;
    float outputTrim = 1.0f;
    float highShelfDb = 0.0f;
    float cleanParallel = 0.0f;
    float preLowPassHz = 16500.0f;
    float interCut1Low = 15500.0f;
    float interCut1High = 8400.0f;
    float interCut2Low = 12500.0f;
    float interCut2High = 6200.0f;
    float tightHpMin = 35.0f;
    float tightHpMax = 180.0f;
    float tightHpModeOffset = 18.0f;
    float tightQLoose = 0.72f;
    float tightQTight = 0.82f;
    float bodyFreq = 180.0f;
    float midFreq = 900.0f;
    float midFreqTightShift = 140.0f;
    float midBaseDb = 0.0f;
    float midBodyScale = 0.0f;
    float midQ = 0.92f;
    float presenceFreq = 3300.0f;
    float presenceDb = 0.0f;
    float presenceTightScale = 0.0f;
    float topCutLow = 13200.0f;
    float topCutHigh = 7600.0f;
    float gateFloor = 1.0f;
    float gateBaseDb = -96.0f;
    float gateGainLiftDb = 0.0f;
    float gateTightLiftDb = 0.0f;
    float gateHysteresisDb = 6.0f;
};

inline ModeConfig makeVintageConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 980.0f;
    config.preMidGainDb = 2.8f;
    config.contourFreq = 760.0f;
    config.contourGainDb = -1.8f;
    config.contourBodyScale = 0.7f;
    config.stage1Pos = 1.75f;
    config.stage1Neg = 1.34f;
    config.stage2Pos = 2.45f;
    config.stage2Neg = 1.85f;
    config.stage3Pos = 1.72f;
    config.stage3Neg = 1.50f;
    config.stage3Mix = 0.12f;
    config.diodeBlend = 0.72f;
    config.ampBlend = 0.28f;
    config.hardBlend = 0.05f;
    config.harmonic = 0.035f;
    config.sagAmount = 0.04f;
    config.outputTrim = 0.92f;
    config.highShelfDb = -0.8f;
    config.midFreq = 860.0f;
    config.midFreqTightShift = 120.0f;
    config.midBaseDb = 0.6f;
    config.midBodyScale = 1.2f;
    config.presenceFreq = 3150.0f;
    config.presenceDb = 0.4f;
    config.presenceTightScale = 0.5f;
    config.topCutLow = 12800.0f;
    config.topCutHigh = 7200.0f;
    return config;
}

inline ModeConfig makeTurboConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 860.0f;
    config.preMidGainDb = 1.6f;
    config.contourFreq = 820.0f;
    config.contourGainDb = 1.0f;
    config.contourBodyScale = 0.5f;
    config.stage1Pos = 1.55f;
    config.stage1Neg = 1.45f;
    config.stage2Pos = 1.95f;
    config.stage2Neg = 1.92f;
    config.stage3Pos = 1.88f;
    config.stage3Neg = 1.78f;
    config.stage3Base = 1.0f;
    config.stage3DriveScale = 0.82f;
    config.stage3Mix = 0.18f;
    config.diodeBlend = 0.24f;
    config.ledBlend = 0.66f;
    config.ampBlend = 0.28f;
    config.hardBlend = 0.08f;
    config.harmonic = 0.028f;
    config.sagAmount = 0.02f;
    config.outputTrim = 0.97f;
    config.highShelfDb = 0.5f;
    config.midFreq = 980.0f;
    config.midFreqTightShift = 110.0f;
    config.midBaseDb = 0.9f;
    config.midBodyScale = 0.9f;
    config.presenceFreq = 3450.0f;
    config.presenceDb = 0.8f;
    config.presenceTightScale = 0.7f;
    config.topCutLow = 13600.0f;
    config.topCutHigh = 8200.0f;
    return config;
}

inline ModeConfig makeAmpConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 1080.0f;
    config.preMidGainDb = 3.5f;
    config.contourFreq = 1120.0f;
    config.contourGainDb = 2.4f;
    config.contourBodyScale = 0.6f;
    config.stage1Pos = 1.90f;
    config.stage1Neg = 1.55f;
    config.stage2Pos = 2.15f;
    config.stage2Neg = 1.76f;
    config.stage3Pos = 1.96f;
    config.stage3Neg = 1.62f;
    config.stage3Base = 1.05f;
    config.stage3DriveScale = 0.92f;
    config.stage3Mix = 0.24f;
    config.diodeBlend = 0.16f;
    config.ampBlend = 0.68f;
    config.hardBlend = 0.10f;
    config.harmonic = 0.044f;
    config.sagAmount = 0.10f;
    config.outputTrim = 0.88f;
    config.highShelfDb = -0.3f;
    config.preLowPassHz = 15800.0f;
    config.interCut1Low = 14800.0f;
    config.interCut1High = 8200.0f;
    config.interCut2Low = 11800.0f;
    config.interCut2High = 5900.0f;
    config.tightHpMin = 42.0f;
    config.tightHpMax = 188.0f;
    config.tightHpModeOffset = 24.0f;
    config.bodyFreq = 155.0f;
    config.midFreq = 1120.0f;
    config.midFreqTightShift = 175.0f;
    config.midBaseDb = 1.8f;
    config.midBodyScale = 1.5f;
    config.midQ = 0.88f;
    config.presenceFreq = 3050.0f;
    config.presenceDb = 0.7f;
    config.presenceTightScale = 0.9f;
    config.topCutLow = 12200.0f;
    config.topCutHigh = 6800.0f;
    return config;
}

inline ModeConfig makeMetalConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 1220.0f;
    config.preMidGainDb = 2.2f;
    config.contourFreq = 690.0f;
    config.contourGainDb = -3.9f;
    config.contourBodyScale = 1.0f;
    config.stage1Pos = 1.82f;
    config.stage1Neg = 1.38f;
    config.stage2Pos = 2.30f;
    config.stage2Neg = 1.66f;
    config.stage3Pos = 2.44f;
    config.stage3Neg = 1.62f;
    config.stage1BaseDb = 6.0f;
    config.stage1DrivePerPct = 0.34f;
    config.stage2Base = 1.45f;
    config.stage2DriveScale = 2.65f;
    config.stage2TightScale = 0.30f;
    config.stage3Base = 1.16f;
    config.stage3DriveScale = 1.22f;
    config.stage3Mix = 0.74f;
    config.diodeBlend = 0.52f;
    config.ledBlend = 0.16f;
    config.ampBlend = 0.24f;
    config.hardBlend = 0.34f;
    config.harmonic = 0.056f;
    config.sagAmount = 0.02f;
    config.outputTrim = 0.82f;
    config.highShelfDb = -1.6f;
    config.cleanParallel = 0.02f;
    config.preLowPassHz = 15200.0f;
    config.interCut1Low = 11800.0f;
    config.interCut1High = 7200.0f;
    config.interCut2Low = 9000.0f;
    config.interCut2High = 5200.0f;
    config.tightHpMin = 72.0f;
    config.tightHpMax = 190.0f;
    config.tightHpModeOffset = 26.0f;
    config.tightQLoose = 1.10f;
    config.tightQTight = 0.82f;
    config.bodyFreq = 112.0f;
    config.midFreq = 980.0f;
    config.midFreqTightShift = 320.0f;
    config.midBaseDb = -2.6f;
    config.midBodyScale = 4.4f;
    config.midQ = 0.82f;
    config.presenceFreq = 3720.0f;
    config.presenceDb = 3.8f;
    config.presenceTightScale = 1.4f;
    config.topCutLow = 9800.0f;
    config.topCutHigh = 5400.0f;
    config.gateFloor = 0.08f;
    config.gateBaseDb = -70.0f;
    config.gateGainLiftDb = 20.0f;
    config.gateTightLiftDb = 8.0f;
    config.gateHysteresisDb = 5.5f;
    return config;
}

inline ModeConfig makeStudioConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 1120.0f;
    config.preMidGainDb = 2.8f;
    config.contourFreq = 930.0f;
    config.contourGainDb = 1.2f;
    config.contourBodyScale = 0.6f;
    config.stage1Pos = 1.72f;
    config.stage1Neg = 1.42f;
    config.stage2Pos = 2.08f;
    config.stage2Neg = 1.74f;
    config.stage3Pos = 2.05f;
    config.stage3Neg = 1.66f;
    config.stage1BaseDb = 5.0f;
    config.stage1DrivePerPct = 0.30f;
    config.stage2Base = 1.28f;
    config.stage2DriveScale = 2.34f;
    config.stage2TightScale = 0.18f;
    config.stage3Base = 1.06f;
    config.stage3DriveScale = 0.90f;
    config.stage3Mix = 0.50f;
    config.diodeBlend = 0.34f;
    config.ledBlend = 0.12f;
    config.ampBlend = 0.40f;
    config.hardBlend = 0.16f;
    config.harmonic = 0.040f;
    config.sagAmount = 0.05f;
    config.outputTrim = 0.88f;
    config.highShelfDb = -0.9f;
    config.cleanParallel = 0.08f;
    config.preLowPassHz = 16000.0f;
    config.interCut1Low = 13200.0f;
    config.interCut1High = 8000.0f;
    config.interCut2Low = 10500.0f;
    config.interCut2High = 6100.0f;
    config.tightHpMin = 55.0f;
    config.tightHpMax = 172.0f;
    config.tightHpModeOffset = 20.0f;
    config.tightQLoose = 0.86f;
    config.tightQTight = 0.78f;
    config.bodyFreq = 128.0f;
    config.midFreq = 920.0f;
    config.midFreqTightShift = 240.0f;
    config.midBaseDb = 1.4f;
    config.midBodyScale = 2.8f;
    config.midQ = 0.88f;
    config.presenceFreq = 3360.0f;
    config.presenceDb = 2.1f;
    config.presenceTightScale = 0.9f;
    config.topCutLow = 11200.0f;
    config.topCutHigh = 6600.0f;
    config.gateFloor = 0.16f;
    config.gateBaseDb = -76.0f;
    config.gateGainLiftDb = 16.0f;
    config.gateTightLiftDb = 6.0f;
    config.gateHysteresisDb = 5.0f;
    return config;
}

inline ModeConfig getModeConfig(int modeIndex) noexcept
{
    switch (modeIndex)
    {
        case 1:  return makeTurboConfig();
        case 2:  return makeAmpConfig();
        case 3:  return makeMetalConfig();
        case 4:  return makeStudioConfig();
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
            juce::StringArray{ "Vintage", "Turbo", "Amp", "Metal", "Studio" },
            0));
    }

    const juce::String getName() const override { return "Distortion"; }
    double getTailLengthSeconds() const override { return 0.08; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        innerSr = sr * 8.0;
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxProcessingChannels, juce::jmax(2, getTotalNumOutputChannels()));

        oversampler.reset();
        oversampler.initProcessing((size_t) preparedBlockSize);

        gainSmooth.reset(sr, Nova::Config::SMOOTH_DRIVE_SECONDS);
        mixSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        tightSmooth.reset(sr, 0.05);

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? gainParam->get() : 58.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 0.64f);
        tightSmooth.setCurrentAndTargetValue(tightParam != nullptr ? tightParam->get() : 0.42f);

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);

        resetFiltersAndDynamics();

        sagAttackCoeff = std::exp(-1.0f / ((float) innerSr * 0.0015f));
        sagReleaseCoeff = std::exp(-1.0f / ((float) innerSr * 0.055f));
        gateEnvelopeAttackCoeff = std::exp(-1.0f / ((float) sr * 0.0018f));
        gateEnvelopeReleaseCoeff = std::exp(-1.0f / ((float) sr * 0.028f));
        gateOpenCoeff = std::exp(-1.0f / ((float) sr * 0.0012f));
        gateCloseCoeff = std::exp(-1.0f / ((float) sr * 0.038f));

        invalidateCachedResponse();

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
        resetFiltersAndDynamics();
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

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            endBypassProcess(buffer);
            return;
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                numSamples);

        const float mixTarget = mixParam != nullptr ? mixParam->get() : 1.0f;

        gainSmooth.setTargetValue(gainParam != nullptr ? gainParam->get() : 58.0f);
        mixSmooth.setTargetValue(mixTarget);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelParam->get() : 0.64f);
        tightSmooth.setTargetValue(tightParam != nullptr ? tightParam->get() : 0.42f);

        if (mixTarget <= 1.0e-4f)
        {
            mixSmooth.setCurrentAndTargetValue(0.0f);
            currentGateGain = 1.0f;
            endBypassProcess(buffer);
            return;
        }

        updateFilters();

        const auto config = Nova::DistortionDSP::getModeConfig(modeParam != nullptr ? modeParam->getIndex() : 0);
        const float gainCtlNow = gainSmooth.getCurrentValue();
        const float tightCtlNow = tightSmooth.getCurrentValue();
        const float driveNormNow = juce::jlimit(0.0f, 1.0f, gainCtlNow * 0.01f);
        applyAdaptiveGate(buffer, numChannels, numSamples, config, driveNormNow, tightCtlNow);

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
                const float dryTransient = data[s];
                float x = dryTransient;

                x = inputHP[(size_t) ch].process(x);
                x = preContour[(size_t) ch].process(x);
                x = preLP[(size_t) ch].process(x);

                const float detector = std::abs(x);
                auto& envelope = sagEnvelope[(size_t) ch];
                const float sagCoeff = detector > envelope ? sagAttackCoeff : sagReleaseCoeff;
                envelope = detector + sagCoeff * (envelope - detector);
                const float sagGain = 1.0f - config.sagAmount * juce::jlimit(0.0f, 1.0f, envelope * 0.72f);

                const float stage1Drive = juce::Decibels::decibelsToGain(config.stage1BaseDb + gainCtl * config.stage1DrivePerPct) * sagGain;
                const float stage2Drive = config.stage2Base + driveNorm * config.stage2DriveScale + tightCtl * config.stage2TightScale;
                const float stage3Drive = config.stage3Base + driveNorm * config.stage3DriveScale + tightCtl * 0.06f;
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
                    juce::jmap(driveNorm, 0.0f, 1.0f, 0.96f, 0.34f),
                    juce::jmap(driveNorm, 0.0f, 1.0f, 1.02f, 0.50f));
                const float led = Nova::DistortionDSP::ledClip(stage2In,
                    juce::jmap(driveNorm, 0.0f, 1.0f, 1.18f, 0.58f));

                x = ampVoice * config.ampBlend
                    + silicon * config.diodeBlend
                    + led * config.ledBlend;
                x = interLP2[(size_t) ch].process(x);

                const float stage3In = x * stage3Drive;
                const float stage3Tube = Nova::DistortionDSP::asymSoftClip(stage3In,
                    config.stage3Pos,
                    config.stage3Neg);
                const float stage3Hard = Nova::DistortionDSP::diodeClip(stage3In,
                    juce::jmap(driveNorm, 0.0f, 1.0f, 1.12f, 0.46f),
                    juce::jmap(driveNorm, 0.0f, 1.0f, 1.14f, 0.52f));
                const float stage3Blend = stage3Tube * (1.0f - config.hardBlend)
                    + stage3Hard * config.hardBlend;
                x = Nova::DistortionDSP::blend(x, stage3Blend, config.stage3Mix);

                const float harmonic = config.harmonic * (0.35f + driveNorm * 0.85f);
                x += harmonic * std::sin(x * juce::MathConstants<float>::pi);
                x += config.cleanParallel * dryTransient * (0.25f + (1.0f - driveNorm) * 0.35f);

                x = bodyShelf[(size_t) ch].process(x);
                x = midPeak[(size_t) ch].process(x);
                x = modeContour[(size_t) ch].process(x);
                x = presencePeak[(size_t) ch].process(x);
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
                buffer.setSample(ch, s, dry * dryGain + (wet * wetGain * level));
            }
        }

        endBypassProcess(buffer);
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement xml("DISTORTION_STATE");
        xml.setAttribute("version", 2);
        xml.setAttribute("modeIndex", modeParam != nullptr ? modeParam->getIndex() : 0);
        writeFloat(xml, "distGain", gainParam);
        writeFloat(xml, "distTone", toneParam);
        writeFloat(xml, "distBody", bodyParam);
        writeFloat(xml, "distMix", mixParam);
        writeFloat(xml, "distLevel", levelParam);
        writeFloat(xml, "distTight", tightParam);
        copyXmlToBinary(xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        auto xml = std::unique_ptr<juce::XmlElement>(getXmlFromBinary(data, sizeInBytes));
        if (xml == nullptr)
            return;

        if (xml->hasTagName("DISTORTION_STATE"))
        {
            setChoiceIndex(modeParam, xml->getIntAttribute("modeIndex", 0));
            restoreFloat(*xml, "distGain", gainParam);
            restoreFloat(*xml, "distTone", toneParam);
            restoreFloat(*xml, "distBody", bodyParam);
            restoreFloat(*xml, "distMix", mixParam);
            restoreFloat(*xml, "distLevel", levelParam);
            restoreFloat(*xml, "distTight", tightParam);
        }
        else if (xml->hasTagName("PLUGIN_STATE"))
        {
            bool sawLegacyDistortion = false;
            bool sawLegacyMetal = false;

            float legacyMetalLowDb = 3.0f;
            float legacyMetalMidDb = -3.5f;
            float legacyMetalMidFreq = 950.0f;
            float legacyMetalHighDb = 4.0f;
            float legacyMetalGain = 0.64f;

            for (auto* child : xml->getChildIterator())
            {
                if (!child->hasTagName("PARAM"))
                    continue;

                const auto id = child->getStringAttribute("id");
                const float value = (float) child->getDoubleAttribute("value");

                if (id == "distMode")
                {
                    setChoiceIndex(modeParam, juce::jlimit(0, 2, juce::roundToInt(value * 2.0f)));
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "distGain")
                {
                    applyNormalisedValue(gainParam, value);
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "distTone")
                {
                    applyNormalisedValue(toneParam, value);
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "distBody")
                {
                    applyNormalisedValue(bodyParam, value);
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "distMix")
                {
                    applyNormalisedValue(mixParam, value);
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "distLevel")
                {
                    applyNormalisedValue(levelParam, value);
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "distTight")
                {
                    applyNormalisedValue(tightParam, value);
                    sawLegacyDistortion = true;
                    continue;
                }

                if (id == "metalGain")
                {
                    legacyMetalGain = value;
                    applyNormalisedValue(gainParam, value);
                    sawLegacyMetal = true;
                    continue;
                }

                if (id == "metalLow")
                {
                    legacyMetalLowDb = denormaliseLegacyLinear(value, -12.0f, 12.0f);
                    sawLegacyMetal = true;
                    continue;
                }

                if (id == "metalMid")
                {
                    legacyMetalMidDb = denormaliseLegacyLinear(value, -15.0f, 15.0f);
                    sawLegacyMetal = true;
                    continue;
                }

                if (id == "metalMidFreq")
                {
                    legacyMetalMidFreq = denormaliseLegacyLinear(value, 250.0f, 5000.0f);
                    sawLegacyMetal = true;
                    continue;
                }

                if (id == "metalHigh")
                {
                    legacyMetalHighDb = denormaliseLegacyLinear(value, -12.0f, 12.0f);
                    sawLegacyMetal = true;
                    continue;
                }

                if (id == "metalLevel")
                {
                    applyNormalisedValue(levelParam, value);
                    sawLegacyMetal = true;
                    continue;
                }
            }

            if (sawLegacyMetal && !sawLegacyDistortion)
            {
                setChoiceIndex(modeParam, 3);
                setRawValue(mixParam, 1.0f);
                setRawValue(bodyParam, mapLegacyMetalBody(legacyMetalLowDb, legacyMetalMidDb));
                setRawValue(toneParam, mapLegacyMetalTone(legacyMetalHighDb, legacyMetalGain));
                setRawValue(tightParam, mapLegacyMetalTight(legacyMetalGain, legacyMetalMidFreq, legacyMetalLowDb, legacyMetalHighDb));
            }
        }

        invalidateCachedResponse();
        inputGate.reset();
        currentGateGain = 1.0f;
        if (isPrepared)
            reset();
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

    static bool modeUsesAdaptiveGate(int modeIndex) noexcept
    {
        return modeIndex >= 3;
    }

    static juce::String getModeDescription(int modeIndex)
    {
        switch (modeIndex)
        {
            case 1:  return "Higher headroom clipping, broader lows and a more open LED bite";
            case 2:  return "Amp-voiced saturation with firmer mids, bloom and touch-sensitive sag";
            case 3:  return "Integrated metal circuit with adaptive gate, focused scoop and brutal palm-mute clamp";
            case 4:  return "Mix-ready high gain with adaptive gate, tighter presence and polished studio contour";
            default: return "Classic silicon-forward distortion with sharp edge, asymmetry and controlled grind";
        }
    }

    static juce::String getControlHint(int modeIndex)
    {
        switch (modeIndex)
        {
            case 1:  return "Tight sharpens attack and opens the top end before LED clipping";
            case 2:  return "Tight firms the low end before the amp-style sag stage";
            case 3:  return "Tight steers gate depth, palm-mute clamp and mid focus";
            case 4:  return "Tight steers gate depth, transient clamp and the mix-ready contour";
            default: return "Tight trims lows, bloom and pick attack before the clipping network";
        }
    }

    static float computeClipCurve(float inputLevel, float gainPct, int modeIndex)
    {
        const auto config = Nova::DistortionDSP::getModeConfig(modeIndex);
        const float driveNorm = juce::jlimit(0.0f, 1.0f, gainPct * 0.01f);
        const float stage1Drive = juce::Decibels::decibelsToGain(config.stage1BaseDb + gainPct * config.stage1DrivePerPct);
        const float stage2Drive = config.stage2Base + driveNorm * config.stage2DriveScale;
        const float stage3Drive = config.stage3Base + driveNorm * config.stage3DriveScale;
        const float bias = (config.contourGainDb > 0.0f ? 0.03f : -0.015f) * driveNorm;

        float x = Nova::DistortionDSP::asymSoftClip(inputLevel * stage1Drive + bias,
            config.stage1Pos,
            config.stage1Neg);
        const float stage2In = x * stage2Drive;
        const float ampVoice = Nova::DistortionDSP::asymSoftClip(stage2In,
            config.stage2Pos,
            config.stage2Neg);
        const float silicon = Nova::DistortionDSP::diodeClip(stage2In,
            juce::jmap(driveNorm, 0.0f, 1.0f, 0.96f, 0.34f),
            juce::jmap(driveNorm, 0.0f, 1.0f, 1.02f, 0.50f));
        const float led = Nova::DistortionDSP::ledClip(stage2In,
            juce::jmap(driveNorm, 0.0f, 1.0f, 1.18f, 0.58f));

        x = ampVoice * config.ampBlend
            + silicon * config.diodeBlend
            + led * config.ledBlend;

        const float stage3In = x * stage3Drive;
        const float stage3Tube = Nova::DistortionDSP::asymSoftClip(stage3In,
            config.stage3Pos,
            config.stage3Neg);
        const float stage3Hard = Nova::DistortionDSP::diodeClip(stage3In,
            juce::jmap(driveNorm, 0.0f, 1.0f, 1.12f, 0.46f),
            juce::jmap(driveNorm, 0.0f, 1.0f, 1.14f, 0.52f));
        x = Nova::DistortionDSP::blend(x,
            stage3Tube * (1.0f - config.hardBlend) + stage3Hard * config.hardBlend,
            config.stage3Mix);

        x += config.harmonic * (0.35f + driveNorm * 0.85f) * std::sin(x * juce::MathConstants<float>::pi);
        x += config.cleanParallel * inputLevel * (0.25f + (1.0f - driveNorm) * 0.35f);
        return juce::jlimit(-1.0f, 1.0f, x * config.outputTrim);
    }

    juce::AudioParameterFloat* gainParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* bodyParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterChoice* modeParam = nullptr;
    float currentGateGain = 1.0f;

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

private:
    static void writeFloat(juce::XmlElement& xml, const char* key, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            xml.setAttribute(key, param->get());
    }

    static void restoreFloat(const juce::XmlElement& xml, const char* key, juce::AudioParameterFloat* param)
    {
        if (param != nullptr && xml.hasAttribute(key))
            param->setValueNotifyingHost(param->convertTo0to1((float) xml.getDoubleAttribute(key)));
    }

    static void applyNormalisedValue(juce::AudioParameterFloat* param, float normalised)
    {
        if (param != nullptr)
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalised));
    }

    static void setRawValue(juce::AudioParameterFloat* param, float rawValue)
    {
        if (param != nullptr)
            param->setValueNotifyingHost(param->convertTo0to1(rawValue));
    }

    static void setChoiceIndex(juce::AudioParameterChoice* param, int index)
    {
        if (param == nullptr)
            return;

        const int bounded = juce::jlimit(0, (int) param->choices.size() - 1, index);
        param->setValueNotifyingHost((float) bounded / (float) juce::jmax(1, param->choices.size() - 1));
    }

    static float denormaliseLegacyLinear(float normalised, float minimum, float maximum) noexcept
    {
        return minimum + juce::jlimit(0.0f, 1.0f, normalised) * (maximum - minimum);
    }

    static float mapLegacyMetalBody(float lowDb, float midDb) noexcept
    {
        return juce::jlimit(0.0f, 1.0f, 0.54f + lowDb / 18.0f + midDb / 50.0f);
    }

    static float mapLegacyMetalTone(float highDb, float gainNormalised) noexcept
    {
        return juce::jlimit(0.0f, 1.0f, 0.52f + highDb / 18.0f - (gainNormalised - 0.5f) * 0.10f);
    }

    static float mapLegacyMetalTight(float gainNormalised, float midFreqHz, float lowDb, float highDb) noexcept
    {
        const float midFocus = juce::jmap(juce::jlimit(250.0f, 5000.0f, midFreqHz), 250.0f, 5000.0f, -0.04f, 0.24f);
        const float edge = (highDb - lowDb) / 48.0f;
        return juce::jlimit(0.0f, 1.0f, 0.56f + (gainNormalised - 0.5f) * 0.45f + midFocus + edge);
    }

    void resetFiltersAndDynamics()
    {
        for (auto& filter : inputHP)
            filter.reset();
        for (auto& filter : preContour)
            filter.reset();
        for (auto& filter : preLP)
            filter.reset();
        for (auto& filter : interLP1)
            filter.reset();
        for (auto& filter : interLP2)
            filter.reset();
        for (auto& filter : bodyShelf)
            filter.reset();
        for (auto& filter : midPeak)
            filter.reset();
        for (auto& filter : modeContour)
            filter.reset();
        for (auto& filter : presencePeak)
            filter.reset();
        for (auto& filter : toneLP)
            filter.reset();
        for (auto& filter : toneShelf)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        sagEnvelope.fill(0.0f);
        inputGate.reset();
        currentGateGain = 1.0f;
    }

    void invalidateCachedResponse()
    {
        cachedTone = -999.0f;
        cachedBody = -999.0f;
        cachedGain = -999.0f;
        cachedTight = -999.0f;
        cachedMode = -1;
    }

    void applyAdaptiveGate(juce::AudioBuffer<float>& buffer,
        int numChannels,
        int numSamples,
        const Nova::DistortionDSP::ModeConfig& config,
        float driveNorm,
        float tightAmount)
    {
        if (config.gateFloor >= 0.999f)
        {
            currentGateGain = 1.0f;
            return;
        }

        const float openThresholdDb = config.gateBaseDb + driveNorm * config.gateGainLiftDb + tightAmount * config.gateTightLiftDb;
        const float closeThresholdDb = openThresholdDb - config.gateHysteresisDb;

        for (int s = 0; s < numSamples; ++s)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                peak = juce::jmax(peak, std::abs(buffer.getSample(ch, s)));

            const float gateGain = inputGate.process(peak,
                openThresholdDb,
                closeThresholdDb,
                gateEnvelopeAttackCoeff,
                gateEnvelopeReleaseCoeff,
                gateOpenCoeff,
                gateCloseCoeff,
                config.gateFloor);
            currentGateGain = gateGain;

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, s, buffer.getSample(ch, s) * gateGain);
        }
    }

    void updateFilters()
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 0.54f;
        const float body = bodyParam != nullptr ? bodyParam->get() : 0.52f;
        const float gain = gainParam != nullptr ? gainParam->get() : 58.0f;
        const float tight = tightParam != nullptr ? tightParam->get() : 0.42f;
        const int modeIndex = modeParam != nullptr ? modeParam->getIndex() : 0;

        const bool changed = std::abs(cachedTone - tone) > 1.0e-4f
            || std::abs(cachedBody - body) > 1.0e-4f
            || std::abs(cachedGain - gain) > 0.25f
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
        const float bodySigned = juce::jmap(body, 0.0f, 1.0f, -1.0f, 1.0f);
        const float tightShape = tight * tight;
        const float tightHpExtra = modeIndex >= 2 ? 32.0f : 18.0f;
        const float hpFreq = juce::jmap(tightShape, config.tightHpMin, config.tightHpMax) + driveNorm * config.tightHpModeOffset + tight * tightHpExtra;
        const float hpQ = juce::jmap(tight, config.tightQLoose, config.tightQTight);
        const float toneCutoff = juce::jlimit(1800.0f,
            (float) innerSr * 0.45f,
            0.55f * toneToCutoffHz(tone) + 0.45f * juce::jmap(driveNorm, 0.0f, 1.0f, config.topCutLow, config.topCutHigh));
        const float tightLowTrim = modeIndex >= 2 ? (1.6f + driveNorm * 2.4f) : (0.9f + driveNorm * 1.6f);
        const float bodyGain = bodyToGainDb(body) - tight * tightLowTrim;
        const float interCut1 = juce::jmap(driveNorm, 0.0f, 1.0f, config.interCut1Low, config.interCut1High);
        const float interCut2 = juce::jmap(driveNorm, 0.0f, 1.0f, config.interCut2Low, config.interCut2High);
        const float highShelfDb = config.highShelfDb + juce::jmap(tone, -4.5f, 5.0f) + bodySigned * 0.35f;
        const float preLowPassFreq = juce::jlimit(2500.0f,
            (float) innerSr * 0.45f,
            config.preLowPassHz - driveNorm * 1200.0f);
        const float midFreq = juce::jlimit(280.0f,
            2600.0f,
            config.midFreq + bodySigned * 70.0f + juce::jmap(tight, -config.midFreqTightShift, config.midFreqTightShift));
        const float midGain = config.midBaseDb + bodySigned * config.midBodyScale + juce::jmap(tight, -0.8f, 1.2f) * 0.45f;
        const float contourGain = config.contourGainDb + bodySigned * config.contourBodyScale;
        const float presenceGain = config.presenceDb + tight * config.presenceTightScale + juce::jmap(tone, -0.4f, 0.8f);

        for (auto& filter : inputHP)
            filter.setHighPass(hpFreq, hpQ, innerSr);
        for (auto& filter : preContour)
            filter.setPeak(config.preMidFreq, config.preMidGainDb + driveNorm * 1.2f + tight * 0.5f, 0.85f, innerSr);
        for (auto& filter : preLP)
            filter.setLowPass(preLowPassFreq, 0.707f, innerSr);
        for (auto& filter : interLP1)
            filter.setLowPass(interCut1, 0.75f, innerSr);
        for (auto& filter : interLP2)
            filter.setLowPass(interCut2, 0.72f, innerSr);
        for (auto& filter : bodyShelf)
            filter.setLowShelf(config.bodyFreq, bodyGain, 0.78f, innerSr);
        for (auto& filter : midPeak)
            filter.setPeak(midFreq, midGain, config.midQ, innerSr);
        for (auto& filter : modeContour)
            filter.setPeak(config.contourFreq, contourGain, 0.95f, innerSr);
        for (auto& filter : presencePeak)
            filter.setPeak(config.presenceFreq, presenceGain, 1.05f, innerSr);
        for (auto& filter : toneShelf)
            filter.setHighShelf(2500.0f, highShelfDb, 0.72f, innerSr);
        for (auto& filter : toneLP)
            filter.setLowPass(toneCutoff, 0.68f, innerSr);
        for (auto& filter : dcBlock)
            filter.setHighPass(18.0f, 0.707f, innerSr);
    }

    double sr = 44100.0;
    double innerSr = 352800.0;
    static constexpr int kMaxProcessingChannels = 2;

    bool canProcessBlock(const juce::AudioBuffer<float>& buffer) const noexcept
    {
        return buffer.getNumSamples() <= preparedBlockSize
            && buffer.getNumChannels() <= preparedChannels
            && buffer.getNumChannels() <= kMaxProcessingChannels;
    }

    static void scrubInvalidSamples(juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
        }
    }

    juce::dsp::Oversampling<float> oversampler;

    std::array<Nova::DistortionDSP::Biquad, 2> inputHP;
    std::array<Nova::DistortionDSP::Biquad, 2> preContour;
    std::array<Nova::DistortionDSP::Biquad, 2> preLP;
    std::array<Nova::DistortionDSP::Biquad, 2> interLP1;
    std::array<Nova::DistortionDSP::Biquad, 2> interLP2;
    std::array<Nova::DistortionDSP::Biquad, 2> bodyShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> midPeak;
    std::array<Nova::DistortionDSP::Biquad, 2> modeContour;
    std::array<Nova::DistortionDSP::Biquad, 2> presencePeak;
    std::array<Nova::DistortionDSP::Biquad, 2> toneLP;
    std::array<Nova::DistortionDSP::Biquad, 2> toneShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tightSmooth;
    juce::AudioBuffer<float> scratchBuffer;
    std::array<float, 2> sagEnvelope {};
    Nova::DistortionDSP::Gate inputGate;
    float sagAttackCoeff = 0.0f;
    float sagReleaseCoeff = 0.0f;
    float gateEnvelopeAttackCoeff = 0.0f;
    float gateEnvelopeReleaseCoeff = 0.0f;
    float gateOpenCoeff = 0.0f;
    float gateCloseCoeff = 0.0f;
    int preparedBlockSize = 0;
    int preparedChannels = 0;

    float cachedTone = -999.0f;
    float cachedBody = -999.0f;
    float cachedGain = -999.0f;
    float cachedTight = -999.0f;
    int cachedMode = -1;
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};

#include "DistortionEditor.h"
