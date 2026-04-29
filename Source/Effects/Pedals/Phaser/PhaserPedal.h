#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

// ============================================================================
//  Stereo Phaser
//  Upgraded with three voicings, slow analog drift and stronger wet-path
//  saturation so the pedal reads as a finished product rather than a bare
//  modulation primitive.
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
        addParameter(modeParam     = new juce::AudioParameterChoice("phaserMode",
            "Mode",
            juce::StringArray{ "Vintage", "Modern", "Vibe" },
            1));
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
        piOverSr = juce::MathConstants<float>::pi / (float) sr;
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxProcessingChannels, juce::jmax(2, getTotalNumOutputChannels()));

        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.04);
        feedbackSmooth.reset(sr, 0.04);
        mixSmooth.reset(sr, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);
        for (auto& filter : feedbackHighPass)
            filter.prepare(sampleRate, 20.0f);
        for (auto& dcBlock : feedbackDcBlock)
            dcBlock.prepare(sampleRate, 18.0f);
        for (auto& dcBlock : outputDcBlock)
            dcBlock.prepare(sampleRate, 18.0f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        lfoPhase = 0.0f;
        driftPhase = { 0.0f, 0.31f };
        for (auto& ch : allPassStates)
            ch.fill(0.0f);
        feedbackState.fill(0.0f);
        feedbackDampState.fill(0.0f);
        for (auto& filter : feedbackHighPass)
            filter.reset();
        for (auto& dcBlock : feedbackDcBlock)
            dcBlock.reset();
        for (auto& dcBlock : outputDcBlock)
            dcBlock.reset();

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.65f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.72f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.45f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            endBypassProcess(buffer);
            return;
        }

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

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto profile = getModeProfile(modeParam != nullptr ? modeParam->getIndex() : 1);
            const float rate     = rateSmooth.getNextValue();
            const float depth    = depthSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float mix      = snapMixTarget(mixSmooth.getNextValue());

            auto computeLfo = [twoPi, &profile](float phase) -> float
            {
                float tri = (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);
                float sine = std::sin(twoPi * phase);
                float harmonic = std::sin(twoPi * (phase * 2.0f + 0.11f));
                float raw = tri * profile.triangleBlend
                    + sine * (1.0f - profile.triangleBlend)
                    + harmonic * profile.harmonicBlend;
                raw = juce::jlimit(-1.0f, 1.0f, raw);
                return std::pow(juce::jlimit(0.0f, 1.0f, raw * 0.5f + 0.5f), profile.lfoSkew);
            };

            driftPhase[0] = wrapPhase(driftPhase[0] + profile.driftRateL / (float) sr);
            driftPhase[1] = wrapPhase(driftPhase[1] + profile.driftRateR / (float) sr);
            const float driftL = (std::sin(twoPi * driftPhase[0])
                + 0.43f * std::sin(twoPi * (driftPhase[0] * 0.41f + 0.09f))) * profile.driftDepth;
            const float driftR = (std::sin(twoPi * driftPhase[1])
                + 0.37f * std::sin(twoPi * (driftPhase[1] * 0.47f + 0.17f))) * profile.driftDepth;

            float lfoL = computeLfo(lfoPhase);
            float lfoR = (numChannels > 1)
                ? computeLfo(std::fmod(lfoPhase + profile.stereoOffset, 1.0f))
                : lfoL;
            lfoL = juce::jlimit(0.0f, 1.0f, lfoL + driftL);
            lfoR = juce::jlimit(0.0f, 1.0f, lfoR + driftR);

            auto computeBaseFreq = [depth, &profile](float lfo) -> float
            {
                const float sweep = juce::jlimit(0.0f, 1.0f, lfo) * depth;
                const float logMin = std::log(profile.minFreq);
                const float logMax = std::log(profile.maxFreq);
                return std::exp(logMin + sweep * (logMax - logMin));
            };

            const float baseFreqL = computeBaseFreq(lfoL);
            const float baseFreqR = computeBaseFreq(lfoR);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                float x = dry + feedbackState[(size_t) ch] * feedback;

                const float baseFreq = (ch == 0) ? baseFreqL : baseFreqR;
                const float stageCentre = 0.5f * (float) juce::jmax(0, numStages - 1);
                const float stageLfo = ch == 0 ? lfoL : lfoR;

                for (int stage = 0; stage < numStages && stage < kMaxStages; ++stage)
                {
                    const float spread = ((float) stage - stageCentre) * profile.stageSpread;
                    float stageRatio = std::exp(spread);
                    stageRatio *= (stage % 2 == 0)
                        ? (profile.evenStageMin + stageLfo * profile.evenStageDepth)
                        : (profile.oddStageMax - stageLfo * profile.oddStageDepth);
                    const float stageFreq = juce::jlimit(20.0f, (float) (sr * 0.45), baseFreq * stageRatio);

                    const float t = std::tan(piOverSr * stageFreq);
                    const float coeff = (t - 1.0f) / (t + 1.0f);

                    const float input = x;
                    x = coeff * input + allPassStates[(size_t) ch][(size_t) stage];
                    allPassStates[(size_t) ch][(size_t) stage] = input - coeff * x;
                }

                const float dampCoeff = 0.10f + depth * 0.12f;
                const float saturatedFeedback = std::tanh(x * profile.feedbackDrive)
                    / juce::jmax(1.0f, profile.feedbackDrive);
                feedbackDampState[(size_t) ch] += (saturatedFeedback - feedbackDampState[(size_t) ch]) * dampCoeff;
                float feedbackSample = feedbackHighPass[(size_t) ch].process(feedbackDampState[(size_t) ch]);
                feedbackSample = feedbackDcBlock[(size_t) ch].process(feedbackSample);
                feedbackState[(size_t) ch] = feedbackSample;
                float wet = std::tanh(x * profile.outputDrive) / juce::jmax(1.0f, profile.outputDrive);
                wet = outputDcBlock[(size_t) ch].process(wet);

                if (mix <= 0.0001f)
                {
                    buffer.setSample(ch, sample, dry);
                    continue;
                }

                const float dryGain = std::cos(mix * halfPi);
                const float wetGain = mix >= 0.9999f ? 1.0f : std::sin(mix * halfPi);
                buffer.setSample(ch, sample, dry * dryGain + wet * wetGain * profile.wetTrim);
            }

            lfoPhase = wrapPhase(lfoPhase + rate / (float) sr);
        }

        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* rateParam     = nullptr;
    juce::AudioParameterFloat* depthParam    = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* stagesParam   = nullptr;
    juce::AudioParameterFloat* mixParam      = nullptr;
    juce::AudioParameterChoice* modeParam    = nullptr;
    float lfoPhase = 0.0f;

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr int kMaxProcessingChannels = 2;
    static constexpr int kMaxStages = 12;

    bool canProcessBlock(const juce::AudioBuffer<float>& buffer) const noexcept
    {
        return buffer.getNumSamples() <= preparedBlockSize
            && buffer.getNumChannels() <= preparedChannels
            && buffer.getNumChannels() <= kMaxProcessingChannels;
    }

    static void scrubInvalidSamples(juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
        }
    }

    struct FeedbackHighPass
    {
        void prepare(double sampleRate, float cutoffHz) noexcept
        {
            const auto safeSampleRate = juce::jmax(1.0, sampleRate);
            const auto safeCutoff = juce::jlimit(10.0f, (float) (safeSampleRate * 0.45), cutoffHz);
            pole = (float) std::exp(-juce::MathConstants<double>::twoPi * (double) safeCutoff / safeSampleRate);
            reset();
        }

        float process(float x) noexcept
        {
            const float y = x - x1 + pole * y1;
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept
        {
            x1 = 0.0f;
            y1 = 0.0f;
        }

        float pole = 0.995f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    using FeedbackDCBlocker = FeedbackHighPass;

    struct ModeProfile
    {
        float triangleBlend = 0.56f;
        float harmonicBlend = 0.10f;
        float lfoSkew = 0.90f;
        float stereoOffset = 0.33f;
        float minFreq = 70.0f;
        float maxFreq = 5200.0f;
        float stageSpread = 0.17f;
        float evenStageMin = 0.96f;
        float evenStageDepth = 0.10f;
        float oddStageMax = 1.02f;
        float oddStageDepth = 0.08f;
        float feedbackDrive = 1.08f;
        float outputDrive = 1.10f;
        float wetTrim = 0.98f;
        float driftRateL = 0.10f;
        float driftRateR = 0.16f;
        float driftDepth = 0.045f;
    };

    static float snapMixTarget(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return juce::jlimit(0.0f, 1.0f, mix);
    }

    static float wrapPhase(float phase) noexcept
    {
        phase -= std::floor(phase);
        return phase < 0.0f ? phase + 1.0f : phase;
    }

    static ModeProfile getModeProfile(int modeIndex) noexcept
    {
        ModeProfile profile;

        switch (modeIndex)
        {
            case 0: // Vintage
                profile.triangleBlend = 0.62f;
                profile.harmonicBlend = 0.04f;
                profile.lfoSkew = 0.84f;
                profile.stereoOffset = 0.22f;
                profile.minFreq = 82.0f;
                profile.maxFreq = 3600.0f;
                profile.stageSpread = 0.13f;
                profile.evenStageMin = 0.94f;
                profile.evenStageDepth = 0.08f;
                profile.oddStageMax = 1.00f;
                profile.oddStageDepth = 0.06f;
                profile.feedbackDrive = 1.06f;
                profile.outputDrive = 1.02f;
                profile.wetTrim = 0.96f;
                profile.driftRateL = 0.09f;
                profile.driftRateR = 0.13f;
                profile.driftDepth = 0.035f;
                break;

            case 2: // Vibe
                profile.triangleBlend = 0.38f;
                profile.harmonicBlend = 0.16f;
                profile.lfoSkew = 0.74f;
                profile.stereoOffset = 0.27f;
                profile.minFreq = 58.0f;
                profile.maxFreq = 2600.0f;
                profile.stageSpread = 0.11f;
                profile.evenStageMin = 0.90f;
                profile.evenStageDepth = 0.14f;
                profile.oddStageMax = 1.07f;
                profile.oddStageDepth = 0.12f;
                profile.feedbackDrive = 1.12f;
                profile.outputDrive = 1.18f;
                profile.wetTrim = 0.92f;
                profile.driftRateL = 0.13f;
                profile.driftRateR = 0.19f;
                profile.driftDepth = 0.060f;
                break;

            case 1: // Modern
            default:
                break;
        }

        return profile;
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
    std::array<FeedbackHighPass, 2> feedbackHighPass {};
    std::array<FeedbackDCBlocker, 2> feedbackDcBlock {};
    std::array<FeedbackDCBlocker, 2> outputDcBlock {};
    std::array<float, 2> driftPhase {};
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};

#include "PhaserEditor.h"
