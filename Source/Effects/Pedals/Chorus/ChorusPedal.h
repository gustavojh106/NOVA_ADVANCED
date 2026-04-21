#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>

namespace Nova { namespace ChorusDSP {

static constexpr int kMaxVoices = 3;

inline float wrapPhase(float phase) noexcept
{
    phase -= std::floor(phase);
    return phase < 0.0f ? phase + 1.0f : phase;
}

inline float triangleLfo(float phase) noexcept
{
    const float wrapped = wrapPhase(phase);
    if (wrapped < 0.25f)
        return wrapped * 4.0f;
    if (wrapped < 0.75f)
        return 2.0f - wrapped * 4.0f;
    return wrapped * 4.0f - 4.0f;
}

inline float shapedLfo(float phase, float triangleBlend, float harmonicBlend) noexcept
{
    const float wrapped = wrapPhase(phase);
    const float sine = std::sin(juce::MathConstants<float>::twoPi * wrapped);
    const float triangle = triangleLfo(wrapped);
    const float harmonic = std::sin(juce::MathConstants<float>::twoPi * (wrapped * 2.0f + 0.17f));
    const float raw = sine * (1.0f - triangleBlend)
        + triangle * triangleBlend * 0.92f
        + harmonic * harmonicBlend * 0.12f;
    return juce::jlimit(-1.0f, 1.0f, raw);
}

inline float msToSamples(float milliseconds, double sampleRate) noexcept
{
    return milliseconds * (float) (sampleRate * 0.001);
}

inline float softClip(float x, float drive) noexcept
{
    return std::tanh(x * drive) / juce::jmax(1.0f, drive);
}

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void setLowPass(float freq, float q, double sr) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = (1.0f - cosW0) * 0.5f * a0inv;
        b1 = (1.0f - cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setHighPass(float freq, float q, double sr) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = (1.0f + cosW0) * 0.5f * a0inv;
        b1 = -(1.0f + cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct DelayLine
{
    std::vector<float> buffer;
    int writePos = 0;
    int size = 1;

    void allocate(int maxSamples)
    {
        size = juce::jmax(8, maxSamples);
        buffer.assign((size_t) size, 0.0f);
        writePos = 0;
    }

    void clear()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    void write(float sample) noexcept
    {
        buffer[(size_t) writePos] = sample;
        if (++writePos >= size)
            writePos = 0;
    }

    float readCubic(float delaySamples) const noexcept
    {
        const float clampedDelay = juce::jlimit(1.0f, (float) (size - 3), delaySamples);
        float readPos = (float) writePos - clampedDelay;
        while (readPos < 0.0f)
            readPos += (float) size;

        const int i1 = ((int) readPos) % size;
        const int i0 = (i1 - 1 + size) % size;
        const int i2 = (i1 + 1) % size;
        const int i3 = (i1 + 2) % size;
        const float frac = readPos - std::floor(readPos);

        const float y0 = buffer[(size_t) i0];
        const float y1 = buffer[(size_t) i1];
        const float y2 = buffer[(size_t) i2];
        const float y3 = buffer[(size_t) i3];

        const float a = y3 - y2 - y0 + y1;
        const float b = y0 - y1 - a;
        const float c = y2 - y0;
        return y1 + frac * (c + frac * (b + frac * a));
    }
};

struct ModeConfig
{
    int voices = 2;
    float triangleBlend = 0.0f;
    float harmonicBlend = 0.0f;
    float internalFeedback = 0.0f;
    float crossFeedback = 0.0f;
    float sideGain = 1.0f;
    float wetTrim = 1.0f;
    std::array<float, kMaxVoices> weights {};
    std::array<float, kMaxVoices> rateMul {};
    std::array<float, kMaxVoices> phaseBase {};
    std::array<float, kMaxVoices> lagMul {};
    std::array<float, kMaxVoices> lagOffsetMs {};
    std::array<float, kMaxVoices> depthMul {};
    std::array<float, kMaxVoices> stereoPhase {};
    std::array<float, kMaxVoices> stereoDelayMs {};
};

inline ModeConfig makeClassicConfig() noexcept
{
    ModeConfig config;
    config.voices = 2;
    config.triangleBlend = 0.10f;
    config.harmonicBlend = 0.12f;
    config.internalFeedback = 0.10f;
    config.crossFeedback = 0.03f;
    config.sideGain = 0.85f;
    config.wetTrim = 0.96f;
    config.weights = { 0.72f, 0.28f, 0.0f };
    config.rateMul = { 1.0f, 0.87f, 1.0f };
    config.phaseBase = { 0.00f, 0.33f, 0.0f };
    config.lagMul = { 1.00f, 1.10f, 1.0f };
    config.lagOffsetMs = { 0.0f, 0.8f, 0.0f };
    config.depthMul = { 1.00f, 0.78f, 1.0f };
    config.stereoPhase = { 0.10f, 0.22f, 0.0f };
    config.stereoDelayMs = { 0.18f, 0.48f, 0.0f };
    return config;
}

inline ModeConfig makeEnsembleConfig() noexcept
{
    ModeConfig config;
    config.voices = 3;
    config.triangleBlend = 0.22f;
    config.harmonicBlend = 0.24f;
    config.internalFeedback = 0.07f;
    config.crossFeedback = 0.08f;
    config.sideGain = 1.30f;
    config.wetTrim = 1.05f;
    config.weights = { 0.46f, 0.33f, 0.21f };
    config.rateMul = { 1.00f, 0.73f, 1.29f };
    config.phaseBase = { 0.00f, 0.21f, 0.58f };
    config.lagMul = { 1.00f, 1.12f, 0.88f };
    config.lagOffsetMs = { 0.0f, 1.6f, 2.8f };
    config.depthMul = { 0.88f, 0.68f, 0.50f };
    config.stereoPhase = { 0.18f, 0.34f, 0.47f };
    config.stereoDelayMs = { 0.35f, 0.95f, 1.55f };
    return config;
}

inline ModeConfig makeVibratoConfig() noexcept
{
    ModeConfig config;
    config.voices = 2;
    config.triangleBlend = 0.06f;
    config.harmonicBlend = 0.06f;
    config.internalFeedback = 0.03f;
    config.crossFeedback = 0.05f;
    config.sideGain = 1.05f;
    config.wetTrim = 0.92f;
    config.weights = { 0.78f, 0.22f, 0.0f };
    config.rateMul = { 1.00f, 1.22f, 1.0f };
    config.phaseBase = { 0.00f, 0.41f, 0.0f };
    config.lagMul = { 1.00f, 0.86f, 1.0f };
    config.lagOffsetMs = { 0.0f, -0.6f, 0.0f };
    config.depthMul = { 1.22f, 0.42f, 1.0f };
    config.stereoPhase = { 0.24f, 0.32f, 0.0f };
    config.stereoDelayMs = { 0.22f, 0.68f, 0.0f };
    return config;
}

inline ModeConfig getModeConfig(int modeIndex) noexcept
{
    switch (modeIndex)
    {
        case 1:  return makeEnsembleConfig();
        case 2:  return makeVibratoConfig();
        default: return makeClassicConfig();
    }
}

}} // namespace Nova::ChorusDSP

class ChorusPedal final : public ProcessorBase
{
public:
    ChorusPedal()
    {
        addParameter(rateParam = new juce::AudioParameterFloat("chorusRate", "Rate", 0.03f, 8.0f, 0.85f));
        addParameter(depthParam = new juce::AudioParameterFloat("chorusDepth", "Depth", 0.0f, 1.0f, 0.58f));
        addParameter(widthParam = new juce::AudioParameterFloat("chorusWidth", "Width", 0.0f, 1.0f, 0.72f));
        addParameter(toneParam = new juce::AudioParameterFloat("chorusTone", "Tone", 0.0f, 1.0f, 0.62f));
        addParameter(mixParam = new juce::AudioParameterFloat("chorusMix", "Mix", 0.0f, 1.0f, 0.36f));
        addParameter(lagParam = new juce::AudioParameterFloat("chorusLag", "Lag", 2.0f, 18.0f, 7.8f));
        addParameter(modeParam = new juce::AudioParameterChoice("chorusMode",
            "Mode",
            juce::StringArray{ "Classic", "Ensemble", "Vibrato" },
            0));
    }

    const juce::String getName() const override { return "Chorus"; }
    double getTailLengthSeconds() const override { return 0.55; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        const int maxDelaySamples = (int) (sr * 0.045) + 128;
        for (auto& channelVoices : delayLines)
            for (auto& delay : channelVoices)
                delay.allocate(maxDelaySamples);

        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.05);
        widthSmooth.reset(sr, 0.05);
        mixSmooth.reset(sr, 0.04);
        lagSmooth.reset(sr, 0.06);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 0.85f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.58f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? widthParam->get() : 0.72f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? snapMixTarget(mixParam->get()) : 0.36f);
        lagSmooth.setCurrentAndTargetValue(lagParam != nullptr ? lagParam->get() : 7.8f);

        for (auto& filter : inputHPF)
            filter.reset();
        for (auto& filter : toneLPF)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        updateFilters(true);
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false;
    }

    void reset() override
    {
        for (auto& channelVoices : delayLines)
            for (auto& delay : channelVoices)
                delay.clear();

        for (auto& filter : inputHPF)
            filter.reset();
        for (auto& filter : toneLPF)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        feedbackState.fill(0.0f);
        wetState.fill(0.0f);
        lfoPhase = 0.0f;
        driftPhase = 0.0f;

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 0.85f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.58f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? widthParam->get() : 0.72f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? snapMixTarget(mixParam->get()) : 0.36f);
        lagSmooth.setCurrentAndTargetValue(lagParam != nullptr ? lagParam->get() : 7.8f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? rateParam->get() : 0.85f);
        depthSmooth.setTargetValue(depthParam != nullptr ? depthParam->get() : 0.58f);
        widthSmooth.setTargetValue(widthParam != nullptr ? widthParam->get() : 0.72f);
        const float requestedMix = mixParam != nullptr ? snapMixTarget(mixParam->get()) : 0.36f;
        if (requestedMix <= 1.0e-4f || requestedMix >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(requestedMix);
        else
            mixSmooth.setTargetValue(requestedMix);
        lagSmooth.setTargetValue(lagParam != nullptr ? lagParam->get() : 7.8f);
        updateFilters();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        const auto config = Nova::ChorusDSP::getModeConfig(modeParam != nullptr ? modeParam->getIndex() : 0);
        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float width = widthSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();
            const float lagMs = lagSmooth.getNextValue();
            const float baseExcursionMs = 0.18f + depth * 6.4f;
            const float driftBase = std::sin(twoPi * driftPhase);

            std::array<float, 2> dry { 0.0f, 0.0f };
            std::array<float, 2> wetRaw { 0.0f, 0.0f };
            std::array<float, 2> preFiltered { 0.0f, 0.0f };

            for (int ch = 0; ch < numChannels; ++ch)
            {
                dry[(size_t) ch] = buffer.getSample(ch, sample);
                preFiltered[(size_t) ch] = inputHPF[(size_t) ch].process(dry[(size_t) ch]);
            }

            if (numChannels == 1)
                dry[1] = dry[0];

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float stereoSign = ch == 0 ? -1.0f : 1.0f;
                const float previousLocalFeedback = feedbackState[(size_t) ch];
                const float previousCrossFeedback = feedbackState[(size_t) (1 - ch)];
                float voiceSum = 0.0f;

                for (int voice = 0; voice < config.voices; ++voice)
                {
                    const float voiceRate = juce::jlimit(0.03f, 10.0f, rate * config.rateMul[(size_t) voice]);
                    const float phase = Nova::ChorusDSP::wrapPhase(
                        lfoPhase * config.rateMul[(size_t) voice]
                        + config.phaseBase[(size_t) voice]
                        + stereoSign * width * config.stereoPhase[(size_t) voice]);
                    const float drift = 0.10f * std::sin(twoPi
                        * (driftPhase * (0.38f + 0.11f * (float) voice)
                            + config.phaseBase[(size_t) voice]
                            + 0.07f * (float) ch))
                        + 0.035f * driftBase;
                    const float flutter = 0.028f * std::sin(twoPi
                        * (phase * (2.4f + 0.31f * (float) voice)
                            + config.phaseBase[(size_t) voice] * 0.61f));
                    const float lfo = Nova::ChorusDSP::shapedLfo(
                        phase,
                        config.triangleBlend,
                        config.harmonicBlend);
                    const float excursionMs = baseExcursionMs * config.depthMul[(size_t) voice];
                    const float centreLagMs = lagMs * config.lagMul[(size_t) voice]
                        + config.lagOffsetMs[(size_t) voice]
                        + stereoSign * width * config.stereoDelayMs[(size_t) voice];
                    const float delayMs = juce::jlimit(0.85f,
                        32.0f,
                        centreLagMs + excursionMs * (lfo * 0.84f + drift + flutter));
                    const float delaySamples = Nova::ChorusDSP::msToSamples(delayMs, sr);

                    const float tapped = delayLines[(size_t) ch][(size_t) voice].readCubic(delaySamples);
                    voiceSum += tapped * config.weights[(size_t) voice];

                    const float voiceFeedback = previousLocalFeedback
                        * config.internalFeedback
                        * (0.72f + 0.10f * (float) voice);
                    const float crossFeedback = previousCrossFeedback
                        * config.crossFeedback
                        * width
                        * (0.32f + 0.18f * (float) voice);
                    const float drive = 1.08f + depth * 0.18f + 0.05f * (float) voice;
                    const float writeSample = Nova::ChorusDSP::softClip(
                        preFiltered[(size_t) ch] + voiceFeedback + crossFeedback,
                        drive);
                    delayLines[(size_t) ch][(size_t) voice].write(writeSample);

                    juce::ignoreUnused(voiceRate);
                }

                wetRaw[(size_t) ch] = voiceSum * config.wetTrim;
            }

            if (numChannels > 1)
            {
                const float mid = 0.5f * (wetRaw[0] + wetRaw[1]);
                float side = 0.5f * (wetRaw[0] - wetRaw[1]);
                side *= 0.40f + width * config.sideGain;
                wetRaw[0] = mid + side;
                wetRaw[1] = mid - side;
            }
            else
            {
                wetRaw[1] = wetRaw[0];
            }

            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float wet = Nova::ChorusDSP::softClip(wetRaw[(size_t) ch], 1.12f + depth * 0.16f);
                wet = toneLPF[(size_t) ch].process(wet);
                wet = dcBlock[(size_t) ch].process(wet);
                wetState[(size_t) ch] = wet;
                feedbackState[(size_t) ch] = wet * (0.86f - 0.10f * mix);
                buffer.setSample(ch, sample, dry[(size_t) ch] * dryGain + wet * wetGain);
            }

            lfoPhase += rate / (float) sr;
            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;

            driftPhase += (0.045f + rate * 0.035f) / (float) sr;
            if (driftPhase >= 1.0f)
                driftPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

    static float toneToCutoffHz(float tone) noexcept
    {
        return 1700.0f + juce::jlimit(0.0f, 1.0f, tone) * 12300.0f;
    }

    static float snapMixTarget(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return mix;
    }

    static juce::String getModeDescription(int modeIndex)
    {
        switch (modeIndex)
        {
            case 1:  return "Studio ensemble with wider decorrelation and lower seasick factor";
            case 2:  return "Pitch-forward vibrato voice with wetter, more animated motion";
            default: return "Analog-forward classic chorus with focused centre and musical swirl";
        }
    }

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* widthParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* lagParam = nullptr;
    juce::AudioParameterChoice* modeParam = nullptr;
    float lfoPhase = 0.0f;

private:
    void updateFilters(bool force = false)
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 0.62f;
        const int modeIndex = modeParam != nullptr ? modeParam->getIndex() : 0;

        if (!force
            && std::abs(cachedTone - tone) < 0.002f
            && cachedMode == modeIndex)
            return;

        cachedTone = tone;
        cachedMode = modeIndex;

        const float cutoff = toneToCutoffHz(tone);
        const float inputCutoff = modeIndex == 2 ? 45.0f : 70.0f;

        for (auto& filter : inputHPF)
            filter.setHighPass(inputCutoff, 0.707f, sr);
        for (auto& filter : toneLPF)
            filter.setLowPass(cutoff, 0.62f, sr);
        for (auto& filter : dcBlock)
            filter.setHighPass(20.0f, 0.707f, sr);
    }

    double sr = 44100.0;

    std::array<std::array<Nova::ChorusDSP::DelayLine, Nova::ChorusDSP::kMaxVoices>, 2> delayLines;
    std::array<Nova::ChorusDSP::Biquad, 2> inputHPF;
    std::array<Nova::ChorusDSP::Biquad, 2> toneLPF;
    std::array<Nova::ChorusDSP::Biquad, 2> dcBlock;
    std::array<float, 2> feedbackState {};
    std::array<float, 2> wetState {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lagSmooth;

    float driftPhase = 0.0f;
    float cachedTone = -1.0f;
    int cachedMode = -1;
    bool isPrepared = false;
};

#include "ChorusEditor.h"
