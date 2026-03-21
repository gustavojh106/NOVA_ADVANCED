#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Octave Divider — analog-style sub-octave + upper octave
//  Reference: Boss OC-2 / EHX Micro POG character
// ============================================================================
namespace Nova { namespace OctaveDSP {

// ---- Biquad (2-pole) filter for proper smoothing ----
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    void setLowPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float sinW0 = std::sin(w0), cosW0 = std::cos(w0);
        float alpha = sinW0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f - cosW0) * 0.5f * a0inv;
        b1 = (1.0f - cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setHighPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float sinW0 = std::sin(w0), cosW0 = std::cos(w0);
        float alpha = sinW0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f + cosW0) * 0.5f * a0inv;
        b1 = -(1.0f + cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setBandPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float sinW0 = std::sin(w0), cosW0 = std::cos(w0);
        float alpha = sinW0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = alpha * a0inv;
        b1 = 0.0f;
        b2 = -alpha * a0inv;
        a1 = -2.0f * cosW0 * a0inv;
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

// ---- Envelope follower with separate attack/release ----
struct EnvelopeFollower
{
    float state = 0.0f;
    float attackCoeff = 0.01f;
    float releaseCoeff = 0.9995f;

    void prepare(double sr, float attackMs, float releaseMs)
    {
        attackCoeff  = 1.0f - (float)std::exp(-1.0 / (sr * attackMs * 0.001));
        releaseCoeff = 1.0f - (float)std::exp(-1.0 / (sr * releaseMs * 0.001));
    }

    void reset() { state = 0.0f; }

    float process(float absInput) noexcept
    {
        float coeff = (absInput > state) ? attackCoeff : releaseCoeff;
        state += coeff * (absInput - state);
        return state;
    }
};

}} // namespace Nova::OctaveDSP


// ============================================================================
//  OctavePedal — Processor
// ============================================================================
class OctavePedal final : public ProcessorBase
{
public:
    OctavePedal()
    {
        addParameter(subParam   = new juce::AudioParameterFloat("octaveSub",   "Sub",   0.0f, 1.0f, 0.72f));
        addParameter(upperParam = new juce::AudioParameterFloat("octaveUpper", "Upper", 0.0f, 1.0f, 0.28f));
        addParameter(dryParam   = new juce::AudioParameterFloat("octaveDry",   "Dry",   0.0f, 1.0f, 0.82f));
        addParameter(toneParam  = new juce::AudioParameterFloat("octaveTone",  "Tone",  0.0f, 1.0f, 0.50f));
        addParameter(levelParam = new juce::AudioParameterFloat("octaveLevel", "Level", 0.5f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Octave"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        // Smoothers
        subSmooth.reset(sr, 0.04);
        upperSmooth.reset(sr, 0.04);
        drySmooth.reset(sr, 0.04);
        levelSmooth.reset(sr, 0.04);
        subSmooth.setCurrentAndTargetValue(subParam != nullptr ? subParam->get() : 0.72f);
        upperSmooth.setCurrentAndTargetValue(upperParam != nullptr ? upperParam->get() : 0.28f);
        drySmooth.setCurrentAndTargetValue(dryParam != nullptr ? dryParam->get() : 0.82f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);

        // Input conditioning: bandpass 80-3000 Hz for clean zero-crossing detection
        for (auto& bp : inputBP)
        {
            bp.reset();
            bp.setBandPass(400.0f, 0.7f, sr);
        }

        // DC blocker on wet signals
        for (auto& hp : dcBlocker)
        {
            hp.reset();
            hp.setHighPass(25.0f, 0.707f, sr);
        }

        // Sub-octave smoothing: 2-pole LP to tame the square wave
        for (auto& lp : subSmoothFilter)
        {
            lp.reset();
            lp.setLowPass(800.0f, 0.6f, sr);
        }

        // Upper octave smoothing
        for (auto& lp : upperSmoothFilter)
        {
            lp.reset();
            lp.setLowPass(3500.0f, 0.707f, sr);
        }

        // Envelope followers
        for (auto& env : subEnvFollower)
            env.prepare(sr, 0.5, 18.0);  // Fast attack, moderate release

        for (auto& env : upperEnvFollower)
            env.prepare(sr, 0.3, 12.0);  // Slightly faster for upper

        cachedTone = -1.0f;

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        for (auto& bp : inputBP) bp.reset();
        for (auto& hp : dcBlocker) hp.reset();
        for (auto& lp : subSmoothFilter) lp.reset();
        for (auto& lp : upperSmoothFilter) lp.reset();
        for (auto& env : subEnvFollower) env.reset();
        for (auto& env : upperEnvFollower) env.reset();

        lastDetect.fill(0.0f);
        dividerState.fill(false);
        hysteresisLocked.fill(false);

        subSmooth.setCurrentAndTargetValue(subParam != nullptr ? subParam->get() : 0.72f);
        upperSmooth.setCurrentAndTargetValue(upperParam != nullptr ? upperParam->get() : 0.28f);
        drySmooth.setCurrentAndTargetValue(dryParam != nullptr ? dryParam->get() : 0.82f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        subSmooth.setTargetValue(subParam != nullptr ? subParam->get() : 0.72f);
        upperSmooth.setTargetValue(upperParam != nullptr ? upperParam->get() : 0.28f);
        drySmooth.setTargetValue(dryParam != nullptr ? dryParam->get() : 0.82f);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);
        updateToneIfNeeded();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float subAmt   = subSmooth.getNextValue();
            const float upperAmt = upperSmooth.getNextValue();
            const float dryAmt   = drySmooth.getNextValue();
            const float level    = levelSmooth.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);

                // ---- Input conditioning for detection ----
                const float detect = inputBP[(size_t)ch].process(dry);
                const float absDetect = std::abs(detect);

                // ---- Sub-octave: flip-flop divider with hysteresis ----
                // Hysteresis: require signal to cross above threshold before
                // allowing next zero-crossing toggle (prevents chatter)
                constexpr float kHystThreshold = 0.003f;
                constexpr float kHystRelease   = 0.001f;

                if (hysteresisLocked[(size_t)ch])
                {
                    // Wait for signal to drop below release threshold
                    if (absDetect < kHystRelease)
                        hysteresisLocked[(size_t)ch] = false;
                }

                // Zero-crossing: positive-going, signal must be above threshold
                if (!hysteresisLocked[(size_t)ch] &&
                    lastDetect[(size_t)ch] <= 0.0f && detect > 0.0f &&
                    absDetect > kHystThreshold)
                {
                    dividerState[(size_t)ch] = !dividerState[(size_t)ch];
                    hysteresisLocked[(size_t)ch] = true;
                }

                lastDetect[(size_t)ch] = detect;

                // Envelope-shaped sub: square wave * envelope
                const float subEnv = subEnvFollower[(size_t)ch].process(std::abs(dry));
                const float subRaw = dividerState[(size_t)ch] ? subEnv : -subEnv;
                const float subFiltered = subSmoothFilter[(size_t)ch].process(subRaw);

                // ---- Upper octave: full-wave rectification ----
                const float upperEnv = upperEnvFollower[(size_t)ch].process(std::abs(dry));
                const float rectified = std::abs(dry);
                // Scale by envelope ratio to maintain dynamics
                const float upperRaw = (upperEnv > 0.0001f)
                    ? rectified * (upperEnv / juce::jmax(0.0001f, std::abs(dry) + 0.0001f)) * upperEnv
                    : 0.0f;
                const float upperFiltered = upperSmoothFilter[(size_t)ch].process(rectified);

                // ---- Mix and DC-block ----
                const float wet = subFiltered * subAmt * 0.85f + upperFiltered * upperAmt * 0.75f;
                const float dcBlocked = dcBlocker[(size_t)ch].process(wet);

                buffer.setSample(ch, sample, (dry * dryAmt + dcBlocked) * level);
            }
        }

        endBypassProcess(buffer);
    }

    // Public parameter accessors for the editor
    juce::AudioParameterFloat* subParam   = nullptr;
    juce::AudioParameterFloat* upperParam = nullptr;
    juce::AudioParameterFloat* dryParam   = nullptr;
    juce::AudioParameterFloat* toneParam  = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

private:
    void updateToneIfNeeded()
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 0.5f;
        if (std::abs(cachedTone - tone) <= 0.005f)
            return;

        cachedTone = tone;

        // Tone maps to sub smoothing filter cutoff and upper smoothing cutoff
        // Low tone = darker (more sub smoothing), high tone = brighter
        const float subCutoff   = 300.0f + tone * 1200.0f;   // 300-1500 Hz
        const float upperCutoff = 1500.0f + tone * 5000.0f;  // 1500-6500 Hz

        for (auto& lp : subSmoothFilter)
            lp.setLowPass(subCutoff, 0.6f, sr);
        for (auto& lp : upperSmoothFilter)
            lp.setLowPass(upperCutoff, 0.707f, sr);
    }

    double sr = 44100.0;

    // Smoothers
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> subSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> upperSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> drySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    // DSP
    std::array<Nova::OctaveDSP::Biquad, 2> inputBP;           // Bandpass for detection
    std::array<Nova::OctaveDSP::Biquad, 2> dcBlocker;         // DC block on wet
    std::array<Nova::OctaveDSP::Biquad, 2> subSmoothFilter;   // LP on sub-octave
    std::array<Nova::OctaveDSP::Biquad, 2> upperSmoothFilter; // LP on upper-octave
    std::array<Nova::OctaveDSP::EnvelopeFollower, 2> subEnvFollower;
    std::array<Nova::OctaveDSP::EnvelopeFollower, 2> upperEnvFollower;

    // Divider state
    std::array<float, 2> lastDetect{};
    std::array<bool, 2>  dividerState{};
    std::array<bool, 2>  hysteresisLocked{};

    float cachedTone = -1.0f;
    bool isPrepared = false;
};

#include "OctaveEditor.h"
