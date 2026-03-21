#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Auto Wah — Intelligent envelope filter with sidechain detection,
//  2-pole SVF (trapezoidal), LP/BP blend, soft saturation in feedback path
//  Reference: Mutron III funk, EHX Q-Tron+ quack, Moogerfooger sweep
// ============================================================================
namespace Nova { namespace AutoWahDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    void setBandPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = alpha * a0inv;
        b1 = 0.0f;
        b2 = -alpha * a0inv;
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

// State variable filter with trapezoidal integration — stable at high frequencies
struct SVF
{
    float ic1eq = 0.0f;  // band state
    float ic2eq = 0.0f;  // low state

    void reset() { ic1eq = ic2eq = 0.0f; }

    // Returns { lowpass, bandpass, highpass }
    struct Output { float lp, bp, hp; };

    Output process(float x, float g, float k) noexcept
    {
        // Trapezoidal integration (Zavalishin's SVF)
        float hp = (x - (k + g) * ic1eq - ic2eq) / (1.0f + k * g + g * g);
        float bp = g * hp + ic1eq;
        ic1eq = g * hp + bp;
        float lp = g * bp + ic2eq;
        ic2eq = g * bp + lp;
        return { lp, bp, hp };
    }
};

// Soft saturation for feedback path warmth
inline float softSaturate(float x) noexcept
{
    // Smooth approximation of tanh, cheaper
    if (x > 2.0f) return 1.0f;
    if (x < -2.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

}} // namespace Nova::AutoWahDSP


class AutoWahPedal final : public ProcessorBase
{
public:
    AutoWahPedal()
    {
        addParameter(sensitivityParam = new juce::AudioParameterFloat("wahSens",      "Sensitivity", 0.0f,  1.0f,   0.6f));
        addParameter(attackParam      = new juce::AudioParameterFloat("wahAttack",    "Attack",      0.5f,  30.0f,  2.0f));
        addParameter(decayParam       = new juce::AudioParameterFloat("wahDecay",     "Decay",       10.0f, 800.0f, 120.0f));
        addParameter(rangeParam       = new juce::AudioParameterFloat("wahRange",     "Range",       0.0f,  1.0f,   0.72f));
        addParameter(resonanceParam   = new juce::AudioParameterFloat("wahResonance", "Resonance",   0.5f,  10.0f,  3.2f));
        addParameter(mixParam         = new juce::AudioParameterFloat("wahMix",       "Mix",         0.0f,  1.0f,   1.0f));
    }

    const juce::String getName() const override { return "Auto Wah"; }
    double getTailLengthSeconds() const override { return 0.2; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        piOverSr = juce::MathConstants<float>::pi / (float)sr;

        sensSmooth.reset(sr, 0.04);
        attackSmooth.reset(sr, 0.04);
        decaySmooth.reset(sr, 0.04);
        rangeSmooth.reset(sr, 0.04);
        resoSmooth.reset(sr, 0.04);
        mixSmooth.reset(sr, 0.04);

        sensSmooth.setCurrentAndTargetValue(sensitivityParam != nullptr ? *sensitivityParam : 0.6f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 2.0f);
        decaySmooth.setCurrentAndTargetValue(decayParam != nullptr ? *decayParam : 120.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.72f);
        resoSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 3.2f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        // Sidechain bandpass: 200Hz – 3kHz, focuses detection on guitar attack
        for (auto& f : scBP)
            f.setBandPass(800.0f, 0.7f, sr);

        for (auto& f : svf) f.reset();
        for (auto& f : svf2) f.reset();

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        envelope = 0.0f;
        envelopeSmoothed = 0.0f;

        for (auto& f : scBP) f.reset();
        for (auto& f : svf)  f.reset();
        for (auto& f : svf2) f.reset();

        sensSmooth.setCurrentAndTargetValue(sensitivityParam != nullptr ? *sensitivityParam : 0.6f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 2.0f);
        decaySmooth.setCurrentAndTargetValue(decayParam != nullptr ? *decayParam : 120.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.72f);
        resoSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 3.2f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
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

        sensSmooth.setTargetValue(sensitivityParam != nullptr ? *sensitivityParam : 0.6f);
        attackSmooth.setTargetValue(attackParam != nullptr ? *attackParam : 2.0f);
        decaySmooth.setTargetValue(decayParam != nullptr ? *decayParam : 120.0f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? *rangeParam : 0.72f);
        resoSmooth.setTargetValue(resonanceParam != nullptr ? *resonanceParam : 3.2f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        // Frequency sweep range (logarithmic for musical response)
        constexpr float kMinFreqLog = 5.298317f;   // ln(200)
        constexpr float kMaxFreqLog = 8.613531f;    // ln(5500)
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int s = 0; s < numSamples; ++s)
        {
            const float sensitivity = sensSmooth.getNextValue();
            const float attackMs    = attackSmooth.getNextValue();
            const float decayMs     = decaySmooth.getNextValue();
            const float range       = rangeSmooth.getNextValue();
            const float resonance   = resoSmooth.getNextValue();
            const float mix         = mixSmooth.getNextValue();

            // ---- Intelligent envelope detection ----
            // Use sidechain bandpass to focus on guitar attack range (200-3kHz)
            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float raw = buffer.getSample(ch, s);
                float filtered = scBP[(size_t)ch].process(raw);
                detector = juce::jmax(detector, std::abs(filtered));
            }

            // Envelope follower with separate attack/decay
            float attackCoeff  = std::exp(-1.0f / (juce::jmax(0.1f, attackMs) * 0.001f * (float)sr));
            float releaseCoeff = std::exp(-1.0f / (juce::jmax(1.0f, decayMs) * 0.001f * (float)sr));

            if (detector > envelope)
                envelope = detector + attackCoeff * (envelope - detector);
            else
                envelope = detector + releaseCoeff * (envelope - detector);

            // Secondary smoothing for filter stability (prevents zipper noise)
            float smoothCoeff = std::exp(-1.0f / (0.003f * (float)sr));  // ~3ms
            envelopeSmoothed += (1.0f - smoothCoeff) * (envelope - envelopeSmoothed);

            // ---- Map envelope to frequency (logarithmic) ----
            // Scale envelope by sensitivity: higher sens = more responsive to quiet playing
            float envScaled = juce::jlimit(0.0f, 1.0f, envelopeSmoothed * sensitivity * 14.0f);

            // Logarithmic frequency mapping: more natural-sounding sweep
            float sweepRange = range * (kMaxFreqLog - kMinFreqLog);
            float freqLog = kMinFreqLog + sweepRange * envScaled;
            float freq = std::exp(freqLog);
            freq = juce::jlimit(80.0f, (float)(sr * 0.4), freq);

            // Store for editor visualization
            currentFilterFreq = freq;
            currentEnvelope = envScaled;

            // ---- SVF parameters ----
            float g = std::tan(piOverSr * freq);
            // Resonance → Q mapping: higher resonance = narrower/taller peak
            float k = 1.0f / juce::jmax(0.5f, resonance);

            // LP/BP blend: lower resonance → more LP (warm), higher → more BP (quacky)
            // Smooth crossfade between 100% LP at Q=0.5 and 85% BP at Q=10
            float bpBlend = juce::jlimit(0.0f, 1.0f, (resonance - 1.0f) / 6.0f);

            // Auto-gain compensation: higher resonance amplifies the peak
            float gainComp = 1.0f / (1.0f + (resonance - 1.0f) * 0.12f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float dry = scratchBuffer.getSample(ch, s);
                float input = buffer.getSample(ch, s);

                // Add subtle saturation to the input → filter feedback path
                // This gives analog warmth, especially at high resonance
                float saturatedInput = input + Nova::AutoWahDSP::softSaturate(
                    svf[(size_t)ch].ic1eq * resonance * 0.15f) * 0.1f;

                // First SVF stage (main filter)
                auto out1 = svf[(size_t)ch].process(saturatedInput, g, k);

                // Second SVF stage (cascade for steeper slope at low resonance)
                auto out2 = svf2[(size_t)ch].process(out1.lp, g, k * 1.5f);

                // Blend LP and BP based on resonance:
                // Low resonance: mostly cascaded LP (warm, round)
                // High resonance: mostly single-stage BP (quacky, vocal)
                float lpOut = out2.lp;
                float bpOut = out1.bp * resonance * 0.55f;
                float wet = (lpOut * (1.0f - bpBlend) + bpOut * bpBlend) * gainComp;

                // Equal-power dry/wet mix
                float dryG = std::cos(mix * halfPi);
                float wetG = std::sin(mix * halfPi);
                buffer.setSample(ch, s, dry * dryG + wet * wetG);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* sensitivityParam = nullptr;
    juce::AudioParameterFloat* attackParam      = nullptr;
    juce::AudioParameterFloat* decayParam       = nullptr;
    juce::AudioParameterFloat* rangeParam       = nullptr;
    juce::AudioParameterFloat* resonanceParam   = nullptr;
    juce::AudioParameterFloat* mixParam         = nullptr;

    // For editor visualization
    float currentFilterFreq = 200.0f;
    float currentEnvelope   = 0.0f;

private:
    double sr = 44100.0;
    float piOverSr = juce::MathConstants<float>::pi / 44100.0f;

    // Sidechain bandpass for intelligent detection
    std::array<Nova::AutoWahDSP::Biquad, 2> scBP;

    // Dual SVF stages
    std::array<Nova::AutoWahDSP::SVF, 2> svf;    // Main filter
    std::array<Nova::AutoWahDSP::SVF, 2> svf2;   // Cascade stage (LP only)

    float envelope = 0.0f;
    float envelopeSmoothed = 0.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sensSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decaySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resoSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioBuffer<float> scratchBuffer;
    bool isPrepared = false;
};

#include "AutoWahEditor.h"
