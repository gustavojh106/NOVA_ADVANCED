#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Stereo Phaser — staggered all-pass chain, tri-sine LFO, analog feedback
//  Reference: MXR Phase 90 simplicity, EHX Small Stone depth
// ============================================================================
class PhaserPedal final : public ProcessorBase
{
public:
    PhaserPedal()
    {
        addParameter(rateParam     = new juce::AudioParameterFloat("phaserRate",     "Rate",     0.05f, 8.0f, 0.65f));
        addParameter(depthParam    = new juce::AudioParameterFloat("phaserDepth",    "Depth",    0.0f, 1.0f, 0.72f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("phaserFeedback", "Feedback", -0.85f, 0.85f, 0.45f));
        addParameter(stagesParam   = new juce::AudioParameterFloat("phaserStages",   "Stages",   2.0f, 12.0f, 6.0f));
        addParameter(mixParam      = new juce::AudioParameterFloat("phaserMix",      "Mix",      0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "Phaser"; }
    double getTailLengthSeconds() const override { return 0.15; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        piOverSr = juce::MathConstants<float>::pi / (float)sr;

        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.04);
        feedbackSmooth.reset(sr, 0.04);
        mixSmooth.reset(sr, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        lfoPhase = 0.0f;
        for (auto& ch : allPassStates)
            ch.fill(0.0f);
        feedbackState.fill(0.0f);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        const int numStages = juce::roundToInt(stagesParam != nullptr ? *stagesParam : 6.0f);
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        // Wider sweep range than basic phaser
        constexpr float kMinFreq = 80.0f;
        constexpr float kMaxFreq = 5500.0f;

        // Stereo LFO offset (90 degrees)
        constexpr float kStereoOffset = 0.25f;

        // Stage frequency stagger — each stage sweeps slightly higher
        constexpr float kStaggerPerStage = 0.12f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate     = rateSmooth.getNextValue();
            const float depth    = depthSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float mix      = mixSmooth.getNextValue();

            // ---- Tri-sine hybrid LFO (60% triangle + 40% sine for musical sweep) ----
            auto computeLfo = [twoPi](float phase) -> float
            {
                float tri = (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);
                float sine = std::sin(twoPi * phase);
                float raw = tri * 0.6f + sine * 0.4f;
                return raw * 0.5f + 0.5f;  // 0-1
            };

            float lfoL = computeLfo(lfoPhase);
            float lfoR = (numChannels > 1)
                ? computeLfo(std::fmod(lfoPhase + kStereoOffset, 1.0f))
                : lfoL;

            float baseFreqL = kMinFreq + (kMaxFreq - kMinFreq) * lfoL * depth;
            float baseFreqR = kMinFreq + (kMaxFreq - kMinFreq) * lfoR * depth;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                float x = dry + feedbackState[(size_t)ch] * feedback;

                float baseFreq = (ch == 0) ? baseFreqL : baseFreqR;

                for (int stage = 0; stage < numStages && stage < kMaxStages; ++stage)
                {
                    float stagger = 1.0f + (float)stage * kStaggerPerStage;
                    float stageFreq = juce::jlimit(20.0f, (float)(sr * 0.45), baseFreq * stagger);

                    float t = std::tan(piOverSr * stageFreq);
                    float coeff = (t - 1.0f) / (t + 1.0f);

                    float input = x;
                    x = coeff * input + allPassStates[(size_t)ch][(size_t)stage];
                    allPassStates[(size_t)ch][(size_t)stage] = input - coeff * x;
                }

                feedbackState[(size_t)ch] = std::tanh(x);

                // Equal-power dry/wet mix
                const float dryGain = std::cos(mix * halfPi);
                const float wetGain = std::sin(mix * halfPi);
                buffer.setSample(ch, sample, dry * dryGain + x * wetGain);
            }

            lfoPhase += rate / (float)sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* rateParam     = nullptr;
    juce::AudioParameterFloat* depthParam    = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* stagesParam   = nullptr;
    juce::AudioParameterFloat* mixParam      = nullptr;
    float lfoPhase = 0.0f;

private:
    static constexpr int kMaxStages = 12;

    double sr = 44100.0;
    float piOverSr = juce::MathConstants<float>::pi / 44100.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    std::array<std::array<float, 12>, 2> allPassStates {};
    std::array<float, 2> feedbackState {};
    bool isPrepared = false;
};

#include "PhaserEditor.h"
