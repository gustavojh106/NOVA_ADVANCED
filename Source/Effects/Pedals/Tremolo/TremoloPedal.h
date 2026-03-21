#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Tremolo — Standard + harmonic tremolo, sine/tri/square morphing,
//  stereo phase offset, level compensation
//  Reference: Fender Vibrolux harmonic, Vox AC30 bias, Fulltone Supa-Trem
// ============================================================================
namespace Nova { namespace TremoloDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

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

    void setHighPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f + c0) * 0.5f * a0inv;
        b1 = -(1.0f + c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    float process(float x) noexcept
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Multi-shape LFO: sine → triangle → soft square morphing
// shape: 0 = sine, 0.5 = triangle, 1.0 = soft square
inline float shapedLfo(float phase, float shape) noexcept
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    // Sine
    float sine = std::sin(twoPi * phase);

    // Triangle
    float tri = 2.0f * std::abs(2.0f * (phase - std::floor(phase + 0.5f))) - 1.0f;

    // Soft square (tanh-shaped, no aliasing)
    float sq = std::tanh(sine * 4.0f);

    // Morph: 0→0.5 = sine→tri, 0.5→1.0 = tri→square
    if (shape <= 0.5f)
    {
        float t = shape * 2.0f;
        return sine * (1.0f - t) + tri * t;
    }
    else
    {
        float t = (shape - 0.5f) * 2.0f;
        return tri * (1.0f - t) + sq * t;
    }
}

}} // namespace Nova::TremoloDSP


class TremoloPedal final : public ProcessorBase
{
public:
    TremoloPedal()
    {
        addParameter(rateParam     = new juce::AudioParameterFloat("tremoloRate",     "Rate",     0.5f,  15.0f, 4.5f));
        addParameter(depthParam    = new juce::AudioParameterFloat("tremoloDepth",    "Depth",    0.0f,  1.0f,  0.65f));
        addParameter(shapeParam    = new juce::AudioParameterFloat("tremoloShape",    "Shape",    0.0f,  1.0f,  0.3f));
        addParameter(stereoParam   = new juce::AudioParameterFloat("tremoloStereo",   "Stereo",   0.0f,  1.0f,  0.0f));
        addParameter(harmonicParam = new juce::AudioParameterFloat("tremoloHarmonic", "Harmonic", 0.0f,  1.0f,  0.0f));
        addParameter(levelParam    = new juce::AudioParameterFloat("tremoloLevel",    "Level",    0.5f,  2.0f,  1.0f));
    }

    const juce::String getName() const override { return "Tremolo"; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.04);
        shapeSmooth.reset(sr, 0.04);
        stereoSmooth.reset(sr, 0.04);
        harmonicSmooth.reset(sr, 0.04);
        levelSmooth.reset(sr, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 4.5f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.65f);
        shapeSmooth.setCurrentAndTargetValue(shapeParam != nullptr ? *shapeParam : 0.3f);
        stereoSmooth.setCurrentAndTargetValue(stereoParam != nullptr ? *stereoParam : 0.0f);
        harmonicSmooth.setCurrentAndTargetValue(harmonicParam != nullptr ? *harmonicParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        // Crossover filters for harmonic tremolo (~800Hz, Linkwitz-Riley style: 2x LP + 2x HP)
        for (auto& f : xoverLP1) f.setLowPass(800.0f, 0.707f, sr);
        for (auto& f : xoverLP2) f.setLowPass(800.0f, 0.707f, sr);
        for (auto& f : xoverHP1) f.setHighPass(800.0f, 0.707f, sr);
        for (auto& f : xoverHP2) f.setHighPass(800.0f, 0.707f, sr);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        lfoPhase = 0.0f;

        for (auto& f : xoverLP1) f.reset();
        for (auto& f : xoverLP2) f.reset();
        for (auto& f : xoverHP1) f.reset();
        for (auto& f : xoverHP2) f.reset();

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 4.5f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.65f);
        shapeSmooth.setCurrentAndTargetValue(shapeParam != nullptr ? *shapeParam : 0.3f);
        stereoSmooth.setCurrentAndTargetValue(stereoParam != nullptr ? *stereoParam : 0.0f);
        harmonicSmooth.setCurrentAndTargetValue(harmonicParam != nullptr ? *harmonicParam : 0.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 4.5f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.65f);
        shapeSmooth.setTargetValue(shapeParam != nullptr ? *shapeParam : 0.3f);
        stereoSmooth.setTargetValue(stereoParam != nullptr ? *stereoParam : 0.0f);
        harmonicSmooth.setTargetValue(harmonicParam != nullptr ? *harmonicParam : 0.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();

        for (int s = 0; s < numSamples; ++s)
        {
            const float rate     = rateSmooth.getNextValue();
            const float depth    = depthSmooth.getNextValue();
            const float shape    = shapeSmooth.getNextValue();
            const float stereo   = stereoSmooth.getNextValue();
            const float harmonic = harmonicSmooth.getNextValue();
            const float level    = levelSmooth.getNextValue();

            // Per-channel LFO with stereo offset
            float phaseL = lfoPhase;
            float phaseR = std::fmod(lfoPhase + stereo * 0.5f, 1.0f);

            // Shaped LFO: sine → triangle → soft square
            float lfoL = Nova::TremoloDSP::shapedLfo(phaseL, shape);
            float lfoR = Nova::TremoloDSP::shapedLfo(phaseR, shape);

            // Convert bipolar LFO (-1..+1) to gain modulator
            // Standard tremolo: modulate amplitude
            float stdGainL = 1.0f - depth * 0.5f * (1.0f - lfoL);
            float stdGainR = 1.0f - depth * 0.5f * (1.0f - lfoR);

            // Level compensation: deeper depth → boost to maintain perceived volume
            float compensation = 1.0f + depth * 0.15f;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float input = buffer.getSample(ch, s);
                float lfo = (ch == 0) ? lfoL : lfoR;
                float stdGain = (ch == 0) ? stdGainL : stdGainR;

                float output;

                if (harmonic < 0.01f)
                {
                    // Pure standard tremolo
                    output = input * stdGain;
                }
                else
                {
                    // ---- Harmonic tremolo ----
                    // Split into low and high bands via Linkwitz-Riley (cascaded butterworth)
                    float lo = xoverLP2[(size_t)ch].process(xoverLP1[(size_t)ch].process(input));
                    float hi = xoverHP2[(size_t)ch].process(xoverHP1[(size_t)ch].process(input));

                    // Modulate bands with opposite phase LFO
                    float loGain = 1.0f - depth * 0.5f * (1.0f - lfo);
                    float hiGain = 1.0f - depth * 0.5f * (1.0f + lfo);  // Inverted

                    float harmonicOut = lo * loGain + hi * hiGain;

                    // Blend standard and harmonic
                    output = input * stdGain * (1.0f - harmonic) + harmonicOut * harmonic;
                }

                buffer.setSample(ch, s, output * level * compensation);
            }

            // Advance LFO
            lfoPhase += rate / (float)sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* rateParam     = nullptr;
    juce::AudioParameterFloat* depthParam    = nullptr;
    juce::AudioParameterFloat* shapeParam    = nullptr;
    juce::AudioParameterFloat* stereoParam   = nullptr;
    juce::AudioParameterFloat* harmonicParam = nullptr;
    juce::AudioParameterFloat* levelParam    = nullptr;
    float lfoPhase = 0.0f;

private:
    double sr = 44100.0;

    // Linkwitz-Riley crossover for harmonic tremolo (2 cascaded per band per channel)
    std::array<Nova::TremoloDSP::Biquad, 2> xoverLP1, xoverLP2;
    std::array<Nova::TremoloDSP::Biquad, 2> xoverHP1, xoverHP2;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> shapeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stereoSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> harmonicSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    bool isPrepared = false;
};

#include "TremoloEditor.h"
