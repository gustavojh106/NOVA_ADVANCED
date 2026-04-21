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
        feedbackDampState.fill(0.0f);

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
        const float targetMix = snapMixTarget(mixParam != nullptr ? *mixParam : 0.5f);
        if (targetMix <= 0.0001f || targetMix >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(targetMix);
        else
            mixSmooth.setTargetValue(targetMix);

        const int numStages = juce::roundToInt(stagesParam != nullptr ? *stagesParam : 6.0f);
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        // Wider sweep range than basic phaser
        constexpr float kMinFreq = 70.0f;
        constexpr float kMaxFreq = 5200.0f;

        // Stereo LFO offset (90 degrees)
        constexpr float kStereoOffset = 0.33f;

        // Stage frequency stagger — each stage sweeps slightly higher
        constexpr float kStageSpread = 0.17f;
        constexpr float kMinLog = 4.248495f; // log(70)
        constexpr float kMaxLog = 8.556414f; // log(5200)

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate     = rateSmooth.getNextValue();
            const float depth    = depthSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float mix      = snapMixTarget(mixSmooth.getNextValue());

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

            auto computeBaseFreq = [depth, kMinLog, kMaxLog](float lfo) -> float
            {
                const float sweep = std::pow(juce::jlimit(0.0f, 1.0f, lfo), 0.82f) * depth;
                return std::exp(kMinLog + sweep * (kMaxLog - kMinLog));
            };

            const float baseFreqL = computeBaseFreq(lfoL);
            const float baseFreqR = computeBaseFreq(lfoR);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                float x = dry + feedbackState[(size_t)ch] * feedback;

                float baseFreq = (ch == 0) ? baseFreqL : baseFreqR;
                const float stageCentre = 0.5f * (float) juce::jmax(0, numStages - 1);
                const float stageLfo = ch == 0 ? lfoL : lfoR;

                for (int stage = 0; stage < numStages && stage < kMaxStages; ++stage)
                {
                    const float spread = ((float) stage - stageCentre) * kStageSpread;
                    float stageRatio = std::exp(spread);
                    stageRatio *= (stage % 2 == 0)
                        ? (0.96f + stageLfo * 0.10f)
                        : (1.02f - stageLfo * 0.08f);
                    const float stageFreq = juce::jlimit(20.0f, (float)(sr * 0.45), baseFreq * stageRatio);

                    const float t = std::tan(piOverSr * stageFreq);
                    const float coeff = (t - 1.0f) / (t + 1.0f);

                    const float input = x;
                    x = coeff * input + allPassStates[(size_t)ch][(size_t)stage];
                    allPassStates[(size_t)ch][(size_t)stage] = input - coeff * x;
                }

                const float dampCoeff = 0.10f + depth * 0.12f;
                const float saturatedFeedback = std::tanh(x);
                feedbackDampState[(size_t) ch] += (saturatedFeedback - feedbackDampState[(size_t) ch]) * dampCoeff;
                feedbackState[(size_t) ch] = feedbackDampState[(size_t) ch];

                if (mix <= 0.0001f)
                {
                    buffer.setSample(ch, sample, dry);
                    continue;
                }

                const float dryGain = std::cos(mix * halfPi);
                const float wetGain = mix >= 0.9999f ? 1.0f : std::sin(mix * halfPi);
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

    static float snapMixTarget(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return juce::jlimit(0.0f, 1.0f, mix);
    }

    double sr = 44100.0;
    float piOverSr = juce::MathConstants<float>::pi / 44100.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    std::array<std::array<float, 12>, 2> allPassStates {};
    std::array<float, 2> feedbackState {};
    std::array<float, 2> feedbackDampState {};
    bool isPrepared = false;
};

#include "PhaserEditor.h"
