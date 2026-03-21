#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

// ============================================================================
//  Distortion — Dual-stage asymmetric clip, diode-style hard edge, body/tone EQ
//  Reference: RAT crunch, DS-1 edge, ProCo mid-push
// ============================================================================
namespace Nova { namespace DistortionDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

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

    void setLowShelf(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float twoSqrtAa = 2.0f * std::sqrt(A) * alpha;
        float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * c0 + twoSqrtAa);
        b0 = A * ((A + 1.0f) - (A - 1.0f) * c0 + twoSqrtAa) * a0inv;
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c0) * a0inv;
        b2 = A * ((A + 1.0f) - (A - 1.0f) * c0 - twoSqrtAa) * a0inv;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * c0) * a0inv;
        a2 = ((A + 1.0f) + (A - 1.0f) * c0 - twoSqrtAa) * a0inv;
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
        a1 = b1;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float process(float x) noexcept
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Asymmetric diode-style clipping
inline float diodeClip(float x, float posThresh, float negThresh) noexcept
{
    if (x > posThresh)
        return posThresh + (x - posThresh) / (1.0f + std::abs(x - posThresh));
    if (x < -negThresh)
        return -negThresh + (x + negThresh) / (1.0f + std::abs(x + negThresh));
    return x;
}

}} // namespace Nova::DistortionDSP


class DistortionPedal final : public ProcessorBase
{
public:
    DistortionPedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam  = new juce::AudioParameterFloat("distGain",  "Gain",  0.0f,  100.0f, 55.0f));
        addParameter(toneParam  = new juce::AudioParameterFloat("distTone",  "Tone",  0.0f,  1.0f,   0.52f));
        addParameter(bodyParam  = new juce::AudioParameterFloat("distBody",  "Body",  0.0f,  1.0f,   0.5f));
        addParameter(mixParam   = new juce::AudioParameterFloat("distMix",   "Mix",   0.0f,  1.0f,   1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("distLevel", "Level", 0.0f,  1.0f,   0.62f));
    }

    const juce::String getName() const override { return "Distortion"; }
    double getTailLengthSeconds() const override { return 0.05; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        innerSr = sr * 4.0;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        gainSmooth.reset(sr, Nova::Config::SMOOTH_DRIVE_SECONDS);
        mixSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 55.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.62f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        for (auto& f : preHP)       f.reset();
        for (auto& f : preMidBoost) f.reset();
        for (auto& f : interLP)     f.reset();
        for (auto& f : bodyShelf)   f.reset();
        for (auto& f : toneLP)      f.reset();
        for (auto& f : dcBlock)     f.reset();

        cachedTone = -999.0f;
        cachedBody = -999.0f;
        cachedGain = -999.0f;

        setProcessingLatency((int)oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        oversampler.reset();
        for (auto& f : preHP)       f.reset();
        for (auto& f : preMidBoost) f.reset();
        for (auto& f : interLP)     f.reset();
        for (auto& f : bodyShelf)   f.reset();
        for (auto& f : toneLP)      f.reset();
        for (auto& f : dcBlock)     f.reset();

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 55.0f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.62f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();

        // Save dry signal for mix
        if (scratchBuffer.getNumChannels() < numChannels
            || scratchBuffer.getNumSamples() < numSamples)
            scratchBuffer.setSize(numChannels, numSamples, false, false, true);

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch), numSamples);

        updateFilters();
        gainSmooth.setTargetValue(gainParam != nullptr ? *gainParam : 55.0f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 0.62f);

        // Upsample 4x
        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        const int innerChannels = (int)upsampled.getNumChannels();
        const int innerSamples  = (int)upsampled.getNumSamples();

        for (int s = 0; s < innerSamples; ++s)
        {
            const float gainCtl = gainSmooth.getNextValue();
            const float drive = juce::Decibels::decibelsToGain(gainCtl * 0.45f + 6.0f);
            // Asymmetry increases with gain
            const float posThresh = juce::jmap(gainCtl, 0.0f, 100.0f, 0.85f, 0.45f);
            const float negThresh = juce::jmap(gainCtl, 0.0f, 100.0f, 0.90f, 0.60f);

            for (int ch = 0; ch < innerChannels; ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t)ch);
                float x = data[s];

                // Pre-filter: tight HP + mid presence boost
                x = preHP[(size_t)ch].process(x);
                x = preMidBoost[(size_t)ch].process(x);

                // ---- Stage 1: Soft asymmetric clip (preamp tube) ----
                x *= drive;
                float soft = std::tanh(x * 0.9f);

                // ---- Stage 2: Diode-style hard clip ----
                float hard = Nova::DistortionDSP::diodeClip(x, posThresh, negThresh);

                // Blend stages: more gain → more hard clip
                float blend = juce::jmap(gainCtl, 0.0f, 100.0f, 0.8f, 0.35f);
                x = soft * blend + hard * (1.0f - blend);

                // Inter-stage LP to tame aliasing harmonics
                x = interLP[(size_t)ch].process(x);

                // Subtle harmonic richness at high gain
                if (gainCtl > 40.0f && std::abs(x) > 0.5f)
                {
                    float richness = juce::jmap(gainCtl, 40.0f, 100.0f, 0.0f, 0.08f);
                    x += richness * std::sin(x * juce::MathConstants<float>::twoPi);
                }

                // Post EQ
                x = bodyShelf[(size_t)ch].process(x);
                x = toneLP[(size_t)ch].process(x);
                x = dcBlock[(size_t)ch].process(x);

                data[s] = x;
            }
        }

        // Downsample
        oversampler.processSamplesDown(block);

        // Equal-power dry/wet mix + level
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int s = 0; s < numSamples; ++s)
        {
            const float mix   = mixSmooth.getNextValue();
            const float level = juce::jmap(juce::jlimit(0.0f, 1.0f, levelSmooth.getNextValue()), 0.08f, 1.55f);
            const float dryG  = std::cos(mix * halfPi);
            const float wetG  = std::sin(mix * halfPi);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float dry = scratchBuffer.getSample(ch, s);
                float wet = buffer.getSample(ch, s);
                buffer.setSample(ch, s, (dry * dryG + wet * wetG) * level);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* gainParam  = nullptr;
    juce::AudioParameterFloat* toneParam  = nullptr;
    juce::AudioParameterFloat* bodyParam  = nullptr;
    juce::AudioParameterFloat* mixParam   = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    // For editor transfer curve display
    static float computeClipCurve(float inputLevel, float gainPct)
    {
        float drive = juce::Decibels::decibelsToGain(gainPct * 0.45f + 6.0f);
        float posT = juce::jmap(gainPct, 0.0f, 100.0f, 0.85f, 0.45f);
        float negT = juce::jmap(gainPct, 0.0f, 100.0f, 0.90f, 0.60f);
        float x = inputLevel * drive;
        float soft = std::tanh(x * 0.9f);
        float hard = Nova::DistortionDSP::diodeClip(x, posT, negT);
        float blend = juce::jmap(gainPct, 0.0f, 100.0f, 0.8f, 0.35f);
        return soft * blend + hard * (1.0f - blend);
    }

private:
    void updateFilters()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.52f;
        const float body = bodyParam != nullptr ? *bodyParam : 0.5f;
        const float gain = gainParam != nullptr ? *gainParam : 55.0f;

        bool changed = std::abs(cachedTone - tone) > 1.0e-4f
                     || std::abs(cachedBody - body) > 1.0e-4f
                     || std::abs(cachedGain - gain) > 0.5f;
        if (!changed) return;

        cachedTone = tone;
        cachedBody = body;
        cachedGain = gain;

        // Tight pre-HP: higher frequency with more gain for tighter response
        float hpFreq = juce::jmap(gain, 0.0f, 100.0f, 60.0f, 95.0f);
        for (auto& f : preHP)
            f.setHighPass(hpFreq, 0.85f, innerSr);

        // Pre-gain mid presence boost (~1kHz, gain-dependent)
        float midBoostDb = juce::jmap(gain, 0.0f, 100.0f, 1.0f, 4.0f);
        for (auto& f : preMidBoost)
            f.setPeak(1000.0f, midBoostDb, 0.9f, innerSr);

        // Inter-stage LP: tighter at higher gain
        float interFreq = juce::jmap(gain, 0.0f, 100.0f, 14000.0f, 8500.0f);
        for (auto& f : interLP)
            f.setLowPass(interFreq, 0.707f, innerSr);

        // Body: low shelf at 200Hz
        float bodyDb = juce::jmap(body, -5.0f, 7.0f);
        for (auto& f : bodyShelf)
            f.setLowShelf(200.0f, bodyDb, 0.75f, innerSr);

        // Tone: post LP
        float cutoff = juce::jmap(tone, 2200.0f, 12000.0f);
        for (auto& f : toneLP)
            f.setLowPass(cutoff, 0.68f, innerSr);

        for (auto& f : dcBlock)
            f.setHighPass(18.0f, 0.707f, innerSr);
    }

    double sr = 44100.0;
    double innerSr = 176400.0;

    juce::dsp::Oversampling<float> oversampler;

    std::array<Nova::DistortionDSP::Biquad, 2> preHP;
    std::array<Nova::DistortionDSP::Biquad, 2> preMidBoost;
    std::array<Nova::DistortionDSP::Biquad, 2> interLP;
    std::array<Nova::DistortionDSP::Biquad, 2> bodyShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> toneLP;
    std::array<Nova::DistortionDSP::Biquad, 2> dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    float cachedTone = -999.0f;
    float cachedBody = -999.0f;
    float cachedGain = -999.0f;
    bool isPrepared = false;
};

#include "DistortionEditor.h"
