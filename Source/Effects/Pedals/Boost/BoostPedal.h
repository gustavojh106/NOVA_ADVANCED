#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Boost — Clean micro-boost / preamp with JFET-style saturation character,
//  tight HP, mid hump, presence shelf, air rolloff, DC blocker.
//  Reference: Xotic EP Booster, Keeley Katana, TC Spark
// ============================================================================
namespace Nova { namespace BoostDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    void setHighPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(10.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f + c0) * 0.5f * a0inv;
        b1 = -(1.0f + c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setLowPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f - c0) * 0.5f * a0inv;
        b1 = (1.0f - c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setHighShelf(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float sqA = 2.0f * std::sqrt(A) * alpha;
        float a0inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * c0 + sqA);
        b0 = A * ((A + 1.0f) + (A - 1.0f) * c0 + sqA) * a0inv;
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * c0) * a0inv;
        b2 = A * ((A + 1.0f) + (A - 1.0f) * c0 - sqA) * a0inv;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * c0) * a0inv;
        a2 = ((A + 1.0f) - (A - 1.0f) * c0 - sqA) * a0inv;
    }

    void setPeak(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha / A);
        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * c0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float magnitudeAt(float freq, double sr) const
    {
        float w = juce::MathConstants<float>::twoPi * freq / (float)sr;
        float cw = std::cos(w), sw = std::sin(w);
        float c2w = std::cos(2.0f * w), s2w = std::sin(2.0f * w);
        float nr = b0 + b1 * cw + b2 * c2w;
        float ni = -(b1 * sw + b2 * s2w);
        float dr = 1.0f + a1 * cw + a2 * c2w;
        float di = -(a1 * sw + a2 * s2w);
        return std::sqrt((nr * nr + ni * ni) / juce::jmax(dr * dr + di * di, 1.0e-12f));
    }

    float process(float x) noexcept
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// JFET-style asymmetric saturation: DC bias → even harmonics, character-dependent
inline float boostClip(float x, float character) noexcept
{
    if (character < 0.01f) return x;

    // DC bias creates asymmetry → 2nd harmonic emphasis (JFET/tube characteristic)
    float asym = character * 0.12f;
    float drive = 1.0f + character * 1.8f;
    float biased = x + asym;
    float sat = std::tanh(biased * drive);
    sat -= std::tanh(asym * drive); // Remove DC offset from bias

    // Blend clean and saturated
    float mix = character * 0.5f;
    return x * (1.0f - mix) + sat * mix;
}

}} // namespace Nova::BoostDSP


class BoostPedal final : public ProcessorBase
{
public:
    BoostPedal()
    {
        addParameter(gainParam   = new juce::AudioParameterFloat("boostGain",   "Gain",      0.0f,  24.0f, 8.0f));
        addParameter(toneParam   = new juce::AudioParameterFloat("boostTone",   "Tone",      0.0f,  1.0f,  0.58f));
        addParameter(tightParam  = new juce::AudioParameterFloat("boostTight",  "Tight",     0.0f,  1.0f,  0.24f));
        addParameter(charParam   = new juce::AudioParameterFloat("boostChar",   "Character", 0.0f,  1.0f,  0.0f));
        addParameter(midParam    = new juce::AudioParameterFloat("boostMid",    "Mid",      -6.0f,  6.0f,  0.0f));
        addParameter(levelParam  = new juce::AudioParameterFloat("boostLevel",  "Level",     0.5f,  2.0f,  1.0f));
    }

    const juce::String getName() const override { return "Boost"; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0) return;
        sr = sampleRate;

        gainSmooth.reset(sr, 0.04);
        charSmooth.reset(sr, 0.04);
        levelSmooth.reset(sr, 0.04);

        gainSmooth.setCurrentAndTargetValue(gainParam ? *gainParam : 8.0f);
        charSmooth.setCurrentAndTargetValue(charParam ? *charParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam ? *levelParam : 1.0f);

        updateFilters();
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        for (auto& b : inputHP)   b.reset();
        for (auto& b : midPeak)   b.reset();
        for (auto& b : presShelf) b.reset();
        for (auto& b : airLP)     b.reset();
        for (auto& b : dcBlock)   b.reset();

        gainSmooth.setCurrentAndTargetValue(gainParam ? *gainParam : 8.0f);
        charSmooth.setCurrentAndTargetValue(charParam ? *charParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateFilters();

        gainSmooth.setTargetValue(gainParam ? *gainParam : 8.0f);
        charSmooth.setTargetValue(charParam ? *charParam : 0.0f);
        levelSmooth.setTargetValue(levelParam ? *levelParam : 1.0f);

        const int numCh = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int s = 0; s < numSamples; ++s)
        {
            const float gainDb = gainSmooth.getNextValue();
            const float gain   = juce::Decibels::decibelsToGain(gainDb);
            const float character = charSmooth.getNextValue();
            const float level  = levelSmooth.getNextValue();

            for (int ch = 0; ch < numCh; ++ch)
            {
                float x = buffer.getSample(ch, s);

                // 1. Input HP (tightens low end)
                x = inputHP[(size_t)ch].process(x);

                // 2. Gain stage
                x *= gain;

                // 3. Character saturation (JFET-style)
                x = Nova::BoostDSP::boostClip(x, character);

                // 4. Tone shaping (post-saturation)
                x = midPeak[(size_t)ch].process(x);
                x = presShelf[(size_t)ch].process(x);
                x = airLP[(size_t)ch].process(x);

                // 5. DC block
                x = dcBlock[(size_t)ch].process(x);

                buffer.setSample(ch, s, x * level);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* gainParam  = nullptr;
    juce::AudioParameterFloat* toneParam  = nullptr;
    juce::AudioParameterFloat* tightParam = nullptr;
    juce::AudioParameterFloat* charParam  = nullptr;
    juce::AudioParameterFloat* midParam   = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

private:
    double sr = 44100.0;
    bool isPrepared = false;

    std::array<Nova::BoostDSP::Biquad, 2> inputHP, midPeak, presShelf, airLP, dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> charSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    void updateFilters()
    {
        float tone  = toneParam  ? *toneParam  : 0.58f;
        float tight = tightParam ? *tightParam : 0.24f;
        float mid   = midParam   ? *midParam   : 0.0f;

        // Input HP: 28-220Hz, Q rises slightly with tight for resonant character
        float hpFreq = juce::jmap(tight, 28.0f, 220.0f);
        float hpQ    = juce::jmap(tight, 0.707f, 1.2f);
        for (auto& b : inputHP) b.setHighPass(hpFreq, hpQ, sr);

        // Mid peak: ±6dB at 1kHz (EP Booster-style mid hump)
        for (auto& b : midPeak) b.setPeak(1000.0f, mid, 0.7f, sr);

        // Presence shelf: -4 to +6dB at 1.6kHz
        float presDb = juce::jmap(tone, -4.0f, 6.0f);
        for (auto& b : presShelf) b.setHighShelf(1600.0f, presDb, 0.72f, sr);

        // Air LP: 4.2-18kHz (prevents fizz at high tone settings)
        float airFreq = juce::jmap(tone, 4200.0f, 18000.0f);
        for (auto& b : airLP) b.setLowPass(airFreq, 0.72f, sr);

        // DC block: 10Hz HP (catches asymmetric saturation DC offset)
        for (auto& b : dcBlock) b.setHighPass(10.0f, 0.707f, sr);
    }
};

#include "BoostEditor.h"
