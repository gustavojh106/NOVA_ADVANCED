#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  5-Band Parametric EQ — Low shelf, Low-Mid peak, Mid peak (sweepable),
//  Presence peak, High shelf. Custom Biquad DSP, per-band magnitude for viz.
//  Reference: Boss GE-7, MXR 10-Band, Empress ParaEQ
// ============================================================================
namespace Nova { namespace EqDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    void setLowShelf(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float sqA = 2.0f * std::sqrt(A) * alpha;
        float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * c0 + sqA);
        b0 = A * ((A + 1.0f) - (A - 1.0f) * c0 + sqA) * a0inv;
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c0) * a0inv;
        b2 = A * ((A + 1.0f) - (A - 1.0f) * c0 - sqA) * a0inv;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * c0) * a0inv;
        a2 = ((A + 1.0f) + (A - 1.0f) * c0 - sqA) * a0inv;
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

// Band configuration
static constexpr float kLowFreq      = 100.0f;
static constexpr float kLowMidFreq   = 400.0f;
static constexpr float kPresenceFreq = 2500.0f;
static constexpr float kHighFreq     = 8000.0f;
static constexpr float kShelfQ       = 0.72f;
static constexpr float kPeakQ        = 0.9f;
static constexpr float kMidQ         = 1.0f;

}} // namespace Nova::EqDSP


class EQPedal final : public ProcessorBase
{
public:
    EQPedal()
    {
        addParameter(lowParam      = new juce::AudioParameterFloat("eqLow",      "Low",      -12.0f, 12.0f, 0.0f));
        addParameter(lowMidParam   = new juce::AudioParameterFloat("eqLowMid",   "Low-Mid",  -12.0f, 12.0f, 0.0f));
        addParameter(midParam      = new juce::AudioParameterFloat("eqMid",      "Mid",      -12.0f, 12.0f, 0.0f));
        addParameter(midFreqParam  = new juce::AudioParameterFloat("eqMidFreq",  "Mid Freq", 200.0f, 5000.0f, 800.0f));
        addParameter(presenceParam = new juce::AudioParameterFloat("eqPresence", "Presence", -12.0f, 12.0f, 0.0f));
        addParameter(highParam     = new juce::AudioParameterFloat("eqHigh",     "High",     -12.0f, 12.0f, 0.0f));
        addParameter(levelParam    = new juce::AudioParameterFloat("eqLevel",    "Level",    0.0f,   2.0f,  1.0f));
    }

    const juce::String getName() const override { return "EQ"; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0) return;
        sr = sampleRate;

        levelSmooth.reset(sr, 0.04);
        levelSmooth.setCurrentAndTargetValue(levelParam ? *levelParam : 1.0f);

        updateFilters(
            lowParam      ? *lowParam      : 0.0f,
            lowMidParam   ? *lowMidParam   : 0.0f,
            midParam      ? *midParam      : 0.0f,
            midFreqParam  ? *midFreqParam  : 800.0f,
            presenceParam ? *presenceParam : 0.0f,
            highParam     ? *highParam     : 0.0f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        for (auto& b : lowShelf)     b.reset();
        for (auto& b : lowMidPeak)   b.reset();
        for (auto& b : midPeak)      b.reset();
        for (auto& b : presencePeak) b.reset();
        for (auto& b : highShelf)    b.reset();
        levelSmooth.setCurrentAndTargetValue(levelParam ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        // Update filter coefficients once per block (sufficient for EQ knob movements)
        updateFilters(
            lowParam      ? *lowParam      : 0.0f,
            lowMidParam   ? *lowMidParam   : 0.0f,
            midParam      ? *midParam      : 0.0f,
            midFreqParam  ? *midFreqParam  : 800.0f,
            presenceParam ? *presenceParam : 0.0f,
            highParam     ? *highParam     : 0.0f);

        levelSmooth.setTargetValue(levelParam ? *levelParam : 1.0f);

        const int numCh = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int s = 0; s < numSamples; ++s)
        {
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < numCh; ++ch)
            {
                float x = buffer.getSample(ch, s);
                x = lowShelf[(size_t)ch].process(x);
                x = lowMidPeak[(size_t)ch].process(x);
                x = midPeak[(size_t)ch].process(x);
                x = presencePeak[(size_t)ch].process(x);
                x = highShelf[(size_t)ch].process(x);
                buffer.setSample(ch, s, x * level);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* lowParam      = nullptr;
    juce::AudioParameterFloat* lowMidParam   = nullptr;
    juce::AudioParameterFloat* midParam      = nullptr;
    juce::AudioParameterFloat* midFreqParam  = nullptr;
    juce::AudioParameterFloat* presenceParam = nullptr;
    juce::AudioParameterFloat* highParam     = nullptr;
    juce::AudioParameterFloat* levelParam    = nullptr;

private:
    double sr = 44100.0;
    bool isPrepared = false;

    std::array<Nova::EqDSP::Biquad, 2> lowShelf, lowMidPeak, midPeak, presencePeak, highShelf;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    void updateFilters(float lo, float lm, float md, float mf, float pr, float hi)
    {
        using namespace Nova::EqDSP;
        for (auto& b : lowShelf)     b.setLowShelf(kLowFreq, lo, kShelfQ, sr);
        for (auto& b : lowMidPeak)   b.setPeak(kLowMidFreq, lm, kPeakQ, sr);
        for (auto& b : midPeak)      b.setPeak(juce::jlimit(200.0f, 5000.0f, mf), md, kMidQ, sr);
        for (auto& b : presencePeak) b.setPeak(kPresenceFreq, pr, kPeakQ, sr);
        for (auto& b : highShelf)    b.setHighShelf(kHighFreq, hi, kShelfQ, sr);
    }
};

#include "EQEditor.h"
