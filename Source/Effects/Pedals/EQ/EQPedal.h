#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace Nova::EqDSP
{
inline float clampFrequency(float frequencyHz, double sampleRate) noexcept
{
    return juce::jlimit(20.0f, (float) (sampleRate * 0.45), frequencyHz);
}

inline float proportionalQ(float baseQ, float maxQ, float gainDb) noexcept
{
    const float depthNorm = juce::jlimit(0.0f, 1.0f, std::abs(gainDb) / 12.0f);
    return juce::jmap(depthNorm, baseQ, maxQ);
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

    void setIdentity() noexcept
    {
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
    }

    void setLowShelf(float frequencyHz, float gainDb, float q, double sampleRate) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * clampFrequency(frequencyHz, sampleRate) / (float) sampleRate;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float sqrtTerm = 2.0f * std::sqrt(A) * alpha;
        const float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosW0 + sqrtTerm);

        b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + sqrtTerm) * a0inv;
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - sqrtTerm) * a0inv;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - sqrtTerm) * a0inv;
    }

    void setHighShelf(float frequencyHz, float gainDb, float q, double sampleRate) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * clampFrequency(frequencyHz, sampleRate) / (float) sampleRate;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float sqrtTerm = 2.0f * std::sqrt(A) * alpha;
        const float a0inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * cosW0 + sqrtTerm);

        b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + sqrtTerm) * a0inv;
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - sqrtTerm) * a0inv;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - sqrtTerm) * a0inv;
    }

    void setPeak(float frequencyHz, float gainDb, float q, double sampleRate) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * clampFrequency(frequencyHz, sampleRate) / (float) sampleRate;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha / A);

        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * cosW0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    void setLowPass(float frequencyHz, float q, double sampleRate) noexcept
    {
        const float w0 = juce::MathConstants<float>::twoPi * clampFrequency(frequencyHz, sampleRate) / (float) sampleRate;
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

    void setHighPass(float frequencyHz, float q, double sampleRate) noexcept
    {
        const float w0 = juce::MathConstants<float>::twoPi * clampFrequency(frequencyHz, sampleRate) / (float) sampleRate;
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

    float magnitudeAt(float frequencyHz, double sampleRate) const noexcept
    {
        const float w = juce::MathConstants<float>::twoPi * clampFrequency(frequencyHz, sampleRate) / (float) sampleRate;
        const float cosW = std::cos(w);
        const float sinW = std::sin(w);
        const float cos2W = std::cos(2.0f * w);
        const float sin2W = std::sin(2.0f * w);

        const float numeratorReal = b0 + b1 * cosW + b2 * cos2W;
        const float numeratorImag = -(b1 * sinW + b2 * sin2W);
        const float denominatorReal = 1.0f + a1 * cosW + a2 * cos2W;
        const float denominatorImag = -(a1 * sinW + a2 * sin2W);

        return std::sqrt((numeratorReal * numeratorReal + numeratorImag * numeratorImag)
            / juce::jmax(denominatorReal * denominatorReal + denominatorImag * denominatorImag, 1.0e-12f));
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

static constexpr float kLowShelfFreq = 90.0f;
static constexpr float kLowMidFreq = 280.0f;
static constexpr float kPresenceFreq = 2500.0f;
static constexpr float kHighShelfFreq = 7600.0f;
static constexpr float kShelfQ = 0.68f;
static constexpr float kCutQ = 0.7071f;
static constexpr float kLowMidBaseQ = 0.82f;
static constexpr float kLowMidMaxQ = 1.30f;
static constexpr float kPresenceBaseQ = 0.92f;
static constexpr float kPresenceMaxQ = 1.45f;
static constexpr int kCoefficientUpdateInterval = 8;
} // namespace Nova::EqDSP

// Studio EQ:
// - cleanup filters for low-end rumble and top-end fizz
// - proportional-Q bell sections for more musical boosts/cuts
// - sweepable mid band with dedicated Q control
// - smoothed control path with periodic coefficient refresh for safer automation
class EQPedal final : public ProcessorBase
{
public:
    EQPedal()
    {
        addParameter(lowCutParam    = new juce::AudioParameterFloat("eqLowCut",   "Low Cut",   20.0f,   400.0f,   20.0f));
        addParameter(lowParam       = new juce::AudioParameterFloat("eqLow",      "Low",      -12.0f,   12.0f,     0.0f));
        addParameter(lowMidParam    = new juce::AudioParameterFloat("eqLowMid",   "Low-Mid",  -12.0f,   12.0f,     0.0f));
        addParameter(midParam       = new juce::AudioParameterFloat("eqMid",      "Mid",      -12.0f,   12.0f,     0.0f));
        addParameter(midFreqParam   = new juce::AudioParameterFloat("eqMidFreq",  "Mid Freq", 200.0f, 5000.0f,   800.0f));
        addParameter(midQParam      = new juce::AudioParameterFloat("eqMidQ",     "Mid Q",      0.35f,    2.50f,    1.00f));
        addParameter(presenceParam  = new juce::AudioParameterFloat("eqPresence", "Presence", -12.0f,   12.0f,     0.0f));
        addParameter(highParam      = new juce::AudioParameterFloat("eqHigh",     "High",     -12.0f,   12.0f,     0.0f));
        addParameter(highCutParam   = new juce::AudioParameterFloat("eqHighCut",  "High Cut", 2500.0f, 20000.0f, 20000.0f));
        addParameter(levelParam     = new juce::AudioParameterFloat("eqLevel",    "Level",      0.0f,    2.0f,     1.0f));
    }

    const juce::String getName() const override { return "EQ"; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        lowCutSmooth.reset(sr, 0.04);
        lowSmooth.reset(sr, 0.05);
        lowMidSmooth.reset(sr, 0.05);
        midSmooth.reset(sr, 0.05);
        midFreqSmooth.reset(sr, 0.04);
        midQSmooth.reset(sr, 0.04);
        presenceSmooth.reset(sr, 0.05);
        highSmooth.reset(sr, 0.05);
        highCutSmooth.reset(sr, 0.04);
        levelSmooth.reset(sr, 0.04);

        syncSmoothersFromParams();
        coefficientUpdateCounter = 0;
        updateFilters(lowCutSmooth.getCurrentValue(),
            lowSmooth.getCurrentValue(),
            lowMidSmooth.getCurrentValue(),
            midSmooth.getCurrentValue(),
            midFreqSmooth.getCurrentValue(),
            midQSmooth.getCurrentValue(),
            presenceSmooth.getCurrentValue(),
            highSmooth.getCurrentValue(),
            highCutSmooth.getCurrentValue());

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
        for (auto& stage : highPass)
            stage.reset();
        for (auto& stage : lowShelf)
            stage.reset();
        for (auto& stage : lowMidPeak)
            stage.reset();
        for (auto& stage : midPeak)
            stage.reset();
        for (auto& stage : presencePeak)
            stage.reset();
        for (auto& stage : highShelf)
            stage.reset();
        for (auto& stage : lowPass)
            stage.reset();

        syncSmoothersFromParams();
        coefficientUpdateCounter = 0;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        lowCutSmooth.setTargetValue(lowCutParam != nullptr ? lowCutParam->get() : 20.0f);
        lowSmooth.setTargetValue(lowParam != nullptr ? lowParam->get() : 0.0f);
        lowMidSmooth.setTargetValue(lowMidParam != nullptr ? lowMidParam->get() : 0.0f);
        midSmooth.setTargetValue(midParam != nullptr ? midParam->get() : 0.0f);
        midFreqSmooth.setTargetValue(midFreqParam != nullptr ? midFreqParam->get() : 800.0f);
        midQSmooth.setTargetValue(midQParam != nullptr ? midQParam->get() : 1.0f);
        presenceSmooth.setTargetValue(presenceParam != nullptr ? presenceParam->get() : 0.0f);
        highSmooth.setTargetValue(highParam != nullptr ? highParam->get() : 0.0f);
        highCutSmooth.setTargetValue(highCutParam != nullptr ? highCutParam->get() : 20000.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float lowCutHz = lowCutSmooth.getNextValue();
            const float lowDb = lowSmooth.getNextValue();
            const float lowMidDb = lowMidSmooth.getNextValue();
            const float midDb = midSmooth.getNextValue();
            const float midFreqHz = midFreqSmooth.getNextValue();
            const float midQ = midQSmooth.getNextValue();
            const float presenceDb = presenceSmooth.getNextValue();
            const float highDb = highSmooth.getNextValue();
            const float highCutHz = highCutSmooth.getNextValue();
            const float outputGain = levelSmooth.getNextValue();

            if (coefficientUpdateCounter <= 0)
            {
                updateFilters(lowCutHz, lowDb, lowMidDb, midDb, midFreqHz, midQ, presenceDb, highDb, highCutHz);
                coefficientUpdateCounter = Nova::EqDSP::kCoefficientUpdateInterval;
            }

            --coefficientUpdateCounter;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float x = buffer.getSample(ch, sample);
                x = highPass[(size_t) ch].process(x);
                x = lowShelf[(size_t) ch].process(x);
                x = lowMidPeak[(size_t) ch].process(x);
                x = midPeak[(size_t) ch].process(x);
                x = presencePeak[(size_t) ch].process(x);
                x = highShelf[(size_t) ch].process(x);
                x = lowPass[(size_t) ch].process(x);
                buffer.setSample(ch, sample, x * outputGain);
            }
        }

        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* lowCutParam = nullptr;
    juce::AudioParameterFloat* lowParam = nullptr;
    juce::AudioParameterFloat* lowMidParam = nullptr;
    juce::AudioParameterFloat* midParam = nullptr;
    juce::AudioParameterFloat* midFreqParam = nullptr;
    juce::AudioParameterFloat* midQParam = nullptr;
    juce::AudioParameterFloat* presenceParam = nullptr;
    juce::AudioParameterFloat* highParam = nullptr;
    juce::AudioParameterFloat* highCutParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

private:
    void syncSmoothersFromParams()
    {
        lowCutSmooth.setCurrentAndTargetValue(lowCutParam != nullptr ? lowCutParam->get() : 20.0f);
        lowSmooth.setCurrentAndTargetValue(lowParam != nullptr ? lowParam->get() : 0.0f);
        lowMidSmooth.setCurrentAndTargetValue(lowMidParam != nullptr ? lowMidParam->get() : 0.0f);
        midSmooth.setCurrentAndTargetValue(midParam != nullptr ? midParam->get() : 0.0f);
        midFreqSmooth.setCurrentAndTargetValue(midFreqParam != nullptr ? midFreqParam->get() : 800.0f);
        midQSmooth.setCurrentAndTargetValue(midQParam != nullptr ? midQParam->get() : 1.0f);
        presenceSmooth.setCurrentAndTargetValue(presenceParam != nullptr ? presenceParam->get() : 0.0f);
        highSmooth.setCurrentAndTargetValue(highParam != nullptr ? highParam->get() : 0.0f);
        highCutSmooth.setCurrentAndTargetValue(highCutParam != nullptr ? highCutParam->get() : 20000.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);
    }

    void updateFilters(float lowCutHz,
        float lowDb,
        float lowMidDb,
        float midDb,
        float midFreqHz,
        float midQ,
        float presenceDb,
        float highDb,
        float highCutHz)
    {
        using namespace Nova::EqDSP;

        const float lowMidQ = proportionalQ(kLowMidBaseQ, kLowMidMaxQ, lowMidDb);
        const float effectiveMidQ = juce::jlimit(0.35f, 2.80f,
            midQ * juce::jmap(std::abs(midDb), 0.0f, 12.0f, 1.0f, 1.18f));
        const float presenceQ = proportionalQ(kPresenceBaseQ, kPresenceMaxQ, presenceDb);

        for (auto& stage : highPass)
        {
            if (lowCutHz <= 21.0f)
                stage.setIdentity();
            else
                stage.setHighPass(lowCutHz, kCutQ, sr);
        }

        for (auto& stage : lowShelf)
        {
            if (std::abs(lowDb) < 0.01f)
                stage.setIdentity();
            else
                stage.setLowShelf(kLowShelfFreq, lowDb, kShelfQ, sr);
        }

        for (auto& stage : lowMidPeak)
        {
            if (std::abs(lowMidDb) < 0.01f)
                stage.setIdentity();
            else
                stage.setPeak(kLowMidFreq, lowMidDb, lowMidQ, sr);
        }

        for (auto& stage : midPeak)
        {
            if (std::abs(midDb) < 0.01f)
                stage.setIdentity();
            else
                stage.setPeak(midFreqHz, midDb, effectiveMidQ, sr);
        }

        for (auto& stage : presencePeak)
        {
            if (std::abs(presenceDb) < 0.01f)
                stage.setIdentity();
            else
                stage.setPeak(kPresenceFreq, presenceDb, presenceQ, sr);
        }

        for (auto& stage : highShelf)
        {
            if (std::abs(highDb) < 0.01f)
                stage.setIdentity();
            else
                stage.setHighShelf(kHighShelfFreq, highDb, kShelfQ, sr);
        }

        for (auto& stage : lowPass)
        {
            if (highCutHz >= 19950.0f)
                stage.setIdentity();
            else
                stage.setLowPass(highCutHz, kCutQ, sr);
        }
    }

    double sr = 44100.0;
    bool isPrepared = false;
    int coefficientUpdateCounter = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> lowCutSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowMidSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> midFreqSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midQSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> presenceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> highCutSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    std::array<Nova::EqDSP::Biquad, 2> highPass;
    std::array<Nova::EqDSP::Biquad, 2> lowShelf;
    std::array<Nova::EqDSP::Biquad, 2> lowMidPeak;
    std::array<Nova::EqDSP::Biquad, 2> midPeak;
    std::array<Nova::EqDSP::Biquad, 2> presencePeak;
    std::array<Nova::EqDSP::Biquad, 2> highShelf;
    std::array<Nova::EqDSP::Biquad, 2> lowPass;
};

#include "EQEditor.h"
