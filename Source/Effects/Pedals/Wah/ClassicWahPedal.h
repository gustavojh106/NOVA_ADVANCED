#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Classic Wah — Inductor-style resonant sweep, sweep-dependent Q,
//  JFET input saturation, voice voicing, Cry Baby / Vox character
// ============================================================================
namespace Nova { namespace WahDSP {

// Trapezoidal SVF for stable per-sample frequency modulation
struct SVF
{
    float ic1eq = 0.0f;
    float ic2eq = 0.0f;

    void reset() { ic1eq = ic2eq = 0.0f; }

    struct Output { float lp, bp, hp; };

    Output process(float x, float g, float k) noexcept
    {
        float hp = (x - (k + g) * ic1eq - ic2eq) / (1.0f + k * g + g * g);
        float bp = g * hp + ic1eq;
        ic1eq = g * hp + bp;
        float lp = g * bp + ic2eq;
        ic2eq = g * bp + lp;
        return { lp, bp, hp };
    }
};

// JFET-style soft saturation (asymmetric, warmer than tanh)
inline float jfetSaturate(float x) noexcept
{
    // Asymmetric: positive side clips earlier (like JFET drain characteristic)
    if (x > 0.0f)
    {
        float x2 = x * x;
        return x / (1.0f + 0.6f * x2);
    }
    else
    {
        float x2 = x * x;
        return x / (1.0f + 0.35f * x2);
    }
}

}} // namespace Nova::WahDSP


class ClassicWahPedal final : public ProcessorBase
{
public:
    ClassicWahPedal()
    {
        addParameter(sweepParam     = new juce::AudioParameterFloat("wahSweep",          "Sweep",     0.0f, 1.0f, 0.46f));
        addParameter(rangeParam     = new juce::AudioParameterFloat("wahManualRange",    "Range",     0.0f, 1.0f, 0.74f));
        addParameter(resonanceParam = new juce::AudioParameterFloat("wahManualResonance","Resonance", 0.5f, 8.0f, 4.0f));
        addParameter(voiceParam     = new juce::AudioParameterFloat("wahVoice",          "Voice",     0.0f, 1.0f, 0.34f));
        addParameter(mixParam       = new juce::AudioParameterFloat("wahManualMix",      "Mix",       0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "Wah"; }
    double getTailLengthSeconds() const override { return 0.1; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        piOverSr = juce::MathConstants<float>::pi / (float)sr;

        sweepSmooth.reset(sr, 0.012);  // Fast for treadle-like response
        rangeSmooth.reset(sr, 0.04);
        resoSmooth.reset(sr, 0.04);
        voiceSmooth.reset(sr, 0.04);
        mixSmooth.reset(sr, 0.04);

        sweepSmooth.setCurrentAndTargetValue(sweepParam != nullptr ? *sweepParam : 0.46f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.74f);
        resoSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 4.0f);
        voiceSmooth.setCurrentAndTargetValue(voiceParam != nullptr ? *voiceParam : 0.34f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

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
        for (auto& f : svf) f.reset();
        for (auto& f : svf2) f.reset();

        sweepSmooth.setCurrentAndTargetValue(sweepParam != nullptr ? *sweepParam : 0.46f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : 0.74f);
        resoSmooth.setCurrentAndTargetValue(resonanceParam != nullptr ? *resonanceParam : 4.0f);
        voiceSmooth.setCurrentAndTargetValue(voiceParam != nullptr ? *voiceParam : 0.34f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();

        if (scratchBuffer.getNumChannels() < numChannels
            || scratchBuffer.getNumSamples() < numSamples)
            scratchBuffer.setSize(numChannels, numSamples, false, false, true);

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch), numSamples);

        sweepSmooth.setTargetValue(sweepParam != nullptr ? *sweepParam : 0.46f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? *rangeParam : 0.74f);
        resoSmooth.setTargetValue(resonanceParam != nullptr ? *resonanceParam : 4.0f);
        voiceSmooth.setTargetValue(voiceParam != nullptr ? *voiceParam : 0.34f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);

        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int s = 0; s < numSamples; ++s)
        {
            const float sweep     = sweepSmooth.getNextValue();
            const float range     = rangeSmooth.getNextValue();
            const float resonance = resoSmooth.getNextValue();
            const float voice     = voiceSmooth.getNextValue();
            const float mix       = mixSmooth.getNextValue();

            // ---- Inductor-style frequency mapping ----
            // Voice shifts the entire sweep range: dark (0) → bright (1)
            // Real wah: heel-down ~350Hz, toe-down ~2200Hz (Cry Baby)
            //           or ~450Hz–2800Hz (Vox, brighter)
            float minFreq = juce::jmap(voice, 300.0f, 480.0f);
            float maxFreq = juce::jmap(voice, 1600.0f, 2800.0f) + range * 1600.0f;

            // Logarithmic sweep: more natural pedal feel
            float minLog = std::log(minFreq);
            float maxLog = std::log(maxFreq);
            float freq = std::exp(minLog + sweep * (maxLog - minLog));
            freq = juce::jlimit(80.0f, (float)(sr * 0.4), freq);

            // ---- Sweep-dependent Q (inductor characteristic) ----
            // Real wah: Q increases at top of sweep (more vocal/peaky at toe-down)
            // This is the key to authentic wah character
            float sweepQ = resonance * (0.75f + sweep * 0.5f);
            float k = 1.0f / juce::jmax(0.5f, sweepQ);

            float g = std::tan(piOverSr * freq);

            // Store for editor
            currentFreq = freq;
            currentSweep = sweep;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float dry = scratchBuffer.getSample(ch, s);
                float input = buffer.getSample(ch, s);

                // JFET input saturation: adds harmonics to feed the resonance
                // Subtle at low levels, more obvious when playing hard
                float saturated = Nova::WahDSP::jfetSaturate(input * 1.8f) * 0.56f;

                // Main SVF stage
                auto out1 = svf[(size_t)ch].process(saturated, g, k);

                // Second stage for steeper skirt (like real inductor circuit)
                // Lighter resonance on second stage to avoid excessive ringing
                auto out2 = svf2[(size_t)ch].process(out1.bp, g, k * 2.5f);

                // Blend BP from stage 1 (peaky) and LP from stage 2 (body)
                // Voice also shifts the blend: darker voice = more LP body
                float bodyBlend = juce::jmap(voice, 0.35f, 0.10f);
                float wet = out1.bp * sweepQ * 0.45f * (1.0f - bodyBlend)
                          + out2.lp * 1.8f * bodyBlend;

                // Gentle output saturation (inductor core saturation at high resonance)
                wet = std::tanh(wet * 0.9f) * 1.1f;

                // Equal-power mix
                float dryG = std::cos(mix * halfPi);
                float wetG = std::sin(mix * halfPi);
                buffer.setSample(ch, s, dry * dryG + wet * wetG);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* sweepParam     = nullptr;
    juce::AudioParameterFloat* rangeParam     = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* voiceParam     = nullptr;
    juce::AudioParameterFloat* mixParam       = nullptr;

    float currentFreq  = 400.0f;
    float currentSweep = 0.46f;

private:
    double sr = 44100.0;
    float piOverSr = juce::MathConstants<float>::pi / 44100.0f;

    std::array<Nova::WahDSP::SVF, 2> svf;
    std::array<Nova::WahDSP::SVF, 2> svf2;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sweepSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resoSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> voiceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioBuffer<float> scratchBuffer;
    bool isPrepared = false;
};

#include "ClassicWahEditor.h"
