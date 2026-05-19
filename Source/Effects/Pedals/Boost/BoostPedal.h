#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>

namespace Nova::BoostDSP
{
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    void setHighPass(float freq, float q, double sr)
    {
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(10.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float s0 = std::sin(w0), c0 = std::cos(w0);
        const float alpha = s0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f + c0) * 0.5f * a0inv;
        b1 = -(1.0f + c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setLowPass(float freq, float q, double sr)
    {
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float s0 = std::sin(w0), c0 = std::cos(w0);
        const float alpha = s0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f - c0) * 0.5f * a0inv;
        b1 = (1.0f - c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setHighShelf(float freq, float gainDb, float q, double sr)
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float s0 = std::sin(w0), c0 = std::cos(w0);
        const float alpha = s0 / (2.0f * q);
        const float sqA = 2.0f * std::sqrt(A) * alpha;
        const float a0inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * c0 + sqA);
        b0 = A * ((A + 1.0f) + (A - 1.0f) * c0 + sqA) * a0inv;
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * c0) * a0inv;
        b2 = A * ((A + 1.0f) + (A - 1.0f) * c0 - sqA) * a0inv;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * c0) * a0inv;
        a2 = ((A + 1.0f) - (A - 1.0f) * c0 - sqA) * a0inv;
    }

    void setPeak(float freq, float gainDb, float q, double sr)
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float s0 = std::sin(w0), c0 = std::cos(w0);
        const float alpha = s0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha / A);
        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * c0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float magnitudeAt(float freq, double sr) const
    {
        const float w = juce::MathConstants<float>::twoPi * freq / (float) sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float c2w = std::cos(2.0f * w), s2w = std::sin(2.0f * w);
        const float nr = b0 + b1 * cw + b2 * c2w;
        const float ni = -(b1 * sw + b2 * s2w);
        const float dr = 1.0f + a1 * cw + a2 * c2w;
        const float di = -(a1 * sw + a2 * s2w);
        return std::sqrt((nr * nr + ni * ni) / juce::jmax(dr * dr + di * di, 1.0e-12f));
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline float boostClip(float x, float character) noexcept
{
    if (character <= 0.0001f)
        return x;

    const float asym = character * 0.12f;
    const float drive = 1.0f + character * 1.8f;
    const float biased = x + asym;
    float sat = std::tanh(biased * drive);
    sat -= std::tanh(asym * drive);
    const float tubeWarmth = std::tanh(juce::jmax(0.0f, biased) * (0.82f + drive * 0.42f))
        - std::tanh(juce::jmax(0.0f, asym) * (0.82f + drive * 0.42f));
    sat = sat * (0.84f - character * 0.10f) + tubeWarmth * (0.16f + character * 0.10f);
    return juce::jmap(character * 0.5f, x, sat);
}
}

class BoostPedal final : public ProcessorBase
{
public:
    BoostPedal()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam   = new juce::AudioParameterFloat("boostGain",   "Gain",      0.0f, 24.0f, 8.0f));
        addParameter(toneParam   = new juce::AudioParameterFloat("boostTone",   "Tone",      0.0f, 1.0f,  0.58f));
        addParameter(tightParam  = new juce::AudioParameterFloat("boostTight",  "Tight",     0.0f, 1.0f,  0.24f));
        addParameter(charParam   = new juce::AudioParameterFloat("boostChar",   "Character", 0.0f, 1.0f,  0.0f));
        addParameter(midParam    = new juce::AudioParameterFloat("boostMid",    "Mid",      -6.0f, 6.0f,  0.0f));
        addParameter(levelParam  = new juce::AudioParameterFloat("boostLevel",  "Level",     0.5f, 2.0f,  1.0f));
    }

    const juce::String getName() const override { return "Boost"; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        innerSr = sr * (double) oversamplingFactor();

        oversampler.reset();
        oversampler.initProcessing((size_t) juce::jmax(1, samplesPerBlock));

        gainSmooth.reset(sr, 0.04);
        toneSmooth.reset(sr, 0.05);
        tightSmooth.reset(sr, 0.05);
        charSmooth.reset(sr, 0.04);
        midSmooth.reset(sr, 0.05);
        levelSmooth.reset(sr, 0.04);

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 8.0f);
        toneSmooth.setCurrentAndTargetValue(toneParam != nullptr ? *toneParam : 0.58f);
        tightSmooth.setCurrentAndTargetValue(tightParam != nullptr ? *tightParam : 0.24f);
        charSmooth.setCurrentAndTargetValue(charParam != nullptr ? *charParam : 0.0f);
        midSmooth.setCurrentAndTargetValue(midParam != nullptr ? *midParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        resetFilters();
        updateFilters(toneSmooth.getCurrentValue(), tightSmooth.getCurrentValue(), midSmooth.getCurrentValue(), true);

        setProcessingLatency((int) std::round(oversampler.getLatencyInSamples()));
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        oversampler.reset();
        resetFilters();

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 8.0f);
        toneSmooth.setCurrentAndTargetValue(toneParam != nullptr ? *toneParam : 0.58f);
        tightSmooth.setCurrentAndTargetValue(tightParam != nullptr ? *tightParam : 0.24f);
        charSmooth.setCurrentAndTargetValue(charParam != nullptr ? *charParam : 0.0f);
        midSmooth.setCurrentAndTargetValue(midParam != nullptr ? *midParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        updateFilters(toneSmooth.getCurrentValue(), tightSmooth.getCurrentValue(), midSmooth.getCurrentValue(), true);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

        gainSmooth.setTargetValue(gainParam != nullptr ? *gainParam : 8.0f);
        toneSmooth.setTargetValue(toneParam != nullptr ? *toneParam : 0.58f);
        tightSmooth.setTargetValue(tightParam != nullptr ? *tightParam : 0.24f);
        charSmooth.setTargetValue(charParam != nullptr ? *charParam : 0.0f);
        midSmooth.setTargetValue(midParam != nullptr ? *midParam : 0.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int innerSamples = (int) upsampled.getNumSamples();
        const int oversampleRatio = juce::jmax(1, innerSamples / juce::jmax(1, buffer.getNumSamples()));
        const int numChannels = juce::jmin(2, (int) upsampled.getNumChannels());

        float currentGain = gainSmooth.getCurrentValue();
        float currentTone = toneSmooth.getCurrentValue();
        float currentTight = tightSmooth.getCurrentValue();
        float currentCharacter = charSmooth.getCurrentValue();
        float currentMid = midSmooth.getCurrentValue();
        updateFilters(currentTone, currentTight, currentMid, true);

        for (int sample = 0; sample < innerSamples; ++sample)
        {
            if ((sample % oversampleRatio) == 0)
            {
                currentGain = gainSmooth.getNextValue();
                currentTone = toneSmooth.getNextValue();
                currentTight = tightSmooth.getNextValue();
                currentCharacter = charSmooth.getNextValue();
                currentMid = midSmooth.getNextValue();
                updateFilters(currentTone, currentTight, currentMid, false);
            }

            const float gain = juce::Decibels::decibelsToGain(currentGain);
            const float driveWeight = juce::jlimit(0.0f, 1.0f, currentGain / 24.0f);
            const float character = juce::jlimit(0.0f, 1.0f, currentCharacter);
            const float clipAmount = character * (0.50f + driveWeight * 0.50f);
            const float denseDrive = 1.0f + character * 1.9f + driveWeight * 0.85f;
            const float voiceBlend = juce::jlimit(0.0f, 1.0f, 0.34f + currentTone * 0.16f);
            const float ampInputCompatibilityTrim = 1.0f / (1.0f + driveWeight * driveWeight * (0.72f + character * 0.28f));

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t) ch);
                float x = data[sample];

                x = inputHP[(size_t) ch].process(x);
                x *= gain;

                const float headroom = 1.0f / (1.0f + std::abs(x) * (0.10f + character * 0.52f));
                const float jfet = Nova::BoostDSP::boostClip(x * headroom, clipAmount);
                const float dense = std::atan((x + jfet * 0.14f) * denseDrive) * (2.0f / juce::MathConstants<float>::pi);
                x = character <= 0.0001f ? x : juce::jmap(voiceBlend, jfet, dense);

                x = midPeak[(size_t) ch].process(x);
                x = presShelf[(size_t) ch].process(x);
                x = airLP[(size_t) ch].process(x);
                x = dcBlock[(size_t) ch].process(x);
                data[sample] = x * ampInputCompatibilityTrim;
            }
        }

        oversampler.processSamplesDown(block);

        const int numSamples = buffer.getNumSamples();
        for (int s = 0; s < numSamples; ++s)
        {
            const float level = levelSmooth.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, s, containBoostOutput(buffer.getSample(ch, s) * level));
        }

        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* gainParam  = nullptr;
    juce::AudioParameterFloat* toneParam  = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterFloat* charParam  = nullptr;
    juce::AudioParameterFloat* midParam   = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

private:
    static int oversamplingFactor() noexcept
    {
        return 8;
    }

    static float containBoostOutput(float x) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        constexpr float knee = 0.72f;
        constexpr float ceiling = 0.94f;
        const float ax = std::abs(x);
        if (ax <= knee)
            return x;

        const float over = ax - knee;
        const float shaped = knee + (ceiling - knee) * std::tanh(over / (ceiling - knee));
        return std::copysign(shaped, x);
    }

    void resetFilters()
    {
        for (auto& b : inputHP)   b.reset();
        for (auto& b : midPeak)   b.reset();
        for (auto& b : presShelf) b.reset();
        for (auto& b : airLP)     b.reset();
        for (auto& b : dcBlock)   b.reset();
        lastTone = std::numeric_limits<float>::quiet_NaN();
        lastTight = std::numeric_limits<float>::quiet_NaN();
        lastMid = std::numeric_limits<float>::quiet_NaN();
    }

    void updateFilters(float tone, float tight, float mid, bool force)
    {
        if (!force
            && std::abs(tone - lastTone) < 1.0e-4f
            && std::abs(tight - lastTight) < 1.0e-4f
            && std::abs(mid - lastMid) < 1.0e-4f)
        {
            return;
        }

        lastTone = tone;
        lastTight = tight;
        lastMid = mid;

        const float hpFreq = juce::jmap(tight, 28.0f, 260.0f);
        const float hpQ = juce::jmap(tight, 0.707f, 1.18f);
        for (auto& b : inputHP)
            b.setHighPass(hpFreq, hpQ, innerSr);

        const float midFreq = juce::jmap(tone, 780.0f, 1220.0f);
        const float midQ = juce::jmap(tight, 0.68f, 0.98f);
        for (auto& b : midPeak)
            b.setPeak(midFreq, mid, midQ, innerSr);

        const float presDb = juce::jmap(tone, -3.0f, 6.5f);
        const float presFreq = juce::jmap(tone, 1500.0f, 2850.0f);
        for (auto& b : presShelf)
            b.setHighShelf(presFreq, presDb, 0.72f, innerSr);

        const float airFreq = juce::jmap(tone, 4000.0f, 18500.0f);
        for (auto& b : airLP)
            b.setLowPass(airFreq, 0.72f, innerSr);

        for (auto& b : dcBlock)
            b.setHighPass(10.0f, 0.707f, innerSr);
    }

    double sr = 44100.0;
    double innerSr = 176400.0;
    bool isPrepared = false;

    juce::dsp::Oversampling<float> oversampler;
    std::array<Nova::BoostDSP::Biquad, 2> inputHP, midPeak, presShelf, airLP, dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> toneSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tightSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> charSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    float lastTone = std::numeric_limits<float>::quiet_NaN();
    float lastTight = std::numeric_limits<float>::quiet_NaN();
    float lastMid = std::numeric_limits<float>::quiet_NaN();
};

#include "BoostEditor.h"
