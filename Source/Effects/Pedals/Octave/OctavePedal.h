#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace Nova::OctaveDSP
{
inline float sanitizeAudioSample(float x) noexcept
{
    return std::isfinite(x) ? x : 0.0f;
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

    void setLowPass(float freq, float q, double sr)
    {
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float s0 = std::sin(w0), c0 = std::cos(w0);
        const float alpha = s0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f - c0) * 0.5f * a0inv;
        b1 = (1.0f - c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setHighPass(float freq, float q, double sr)
    {
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(10.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float s0 = std::sin(w0), c0 = std::cos(w0);
        const float alpha = s0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f + c0) * 0.5f * a0inv;
        b1 = -(1.0f + c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    float process(float x) noexcept
    {
        x = sanitizeAudioSample(x);
        if (!std::isfinite(z1) || !std::isfinite(z2))
            reset();

        const float y = b0 * x + z1;
        if (!std::isfinite(y))
        {
            reset();
            return 0.0f;
        }

        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        if (!std::isfinite(z1) || !std::isfinite(z2))
            reset();

        return y;
    }
};

struct EnvelopeFollower
{
    float state = 0.0f;
    float attackCoeff = 0.01f;
    float releaseCoeff = 0.999f;

    void prepare(double sr, float attackMs, float releaseMs)
    {
        const double safeSr = juce::jmax(1.0, sr);
        attackCoeff = 1.0f - (float) std::exp(-1.0 / (safeSr * attackMs * 0.001));
        releaseCoeff = 1.0f - (float) std::exp(-1.0 / (safeSr * releaseMs * 0.001));
    }

    void reset() noexcept { state = 0.0f; }

    float process(float absInput) noexcept
    {
        absInput = std::abs(sanitizeAudioSample(absInput));
        if (!std::isfinite(state))
            state = 0.0f;

        const float coeff = absInput > state ? attackCoeff : releaseCoeff;
        state += coeff * (absInput - state);
        state = sanitizeAudioSample(state);
        return state;
    }
};

struct PeriodTracker
{
    void prepare(double sampleRate)
    {
        sr = juce::jmax(1.0, sampleRate);
        decimatedSr = (float) (sr / (double) kDecimation);
        reset();
    }

    void reset() noexcept
    {
        history.fill(0.0f);
        writePos = 0;
        decimateAccumulator = 0.0f;
        decimateCounter = 0;
        analysisCounter = 0;
        trackedPeriod = juce::jlimit((float) getMinLag(), (float) getMaxLag(), decimatedSr / 110.0f);
        smoothedFreq = 110.0f;
        confidence = 0.0f;
        bestCorrelation = 0.0f;
    }

    float process(float conditioned, float envelope) noexcept
    {
        conditioned = sanitizeAudioSample(conditioned);
        envelope = std::abs(sanitizeAudioSample(envelope));
        if (!std::isfinite(trackedPeriod)
            || !std::isfinite(smoothedFreq)
            || !std::isfinite(confidence)
            || !std::isfinite(bestCorrelation)
            || !std::isfinite(decimateAccumulator))
        {
            reset();
        }

        decimateAccumulator += conditioned;
        if (++decimateCounter >= kDecimation)
        {
            pushDecimatedSample(decimateAccumulator * (1.0f / (float) kDecimation), envelope);
            decimateAccumulator = 0.0f;
            decimateCounter = 0;
        }

        const float envelopeFloor = juce::jlimit(0.0025f, 0.04f, 0.0035f + envelope * 0.045f);
        if (envelope < envelopeFloor)
            confidence *= 0.992f;

        const float targetFreq = juce::jlimit(48.0f, 920.0f, decimatedSr / juce::jmax(1.0f, trackedPeriod));
        const float trackingSlew = juce::jmap(confidence, 0.025f, 0.12f);
        smoothedFreq += (targetFreq - smoothedFreq) * trackingSlew;
        smoothedFreq = sanitizeAudioSample(smoothedFreq);
        return smoothedFreq;
    }

    float getConfidence() const noexcept { return confidence; }

private:
    static constexpr int kDecimation = 4;
    static constexpr int kHistorySize = 768;
    static constexpr int kAnalysisHop = 8;

    int getMinLag() const noexcept { return juce::jmax(6, (int) std::round(decimatedSr / 920.0f)); }
    int getMaxLag() const noexcept { return juce::jmin(kHistorySize / 3, juce::jmax(18, (int) std::round(decimatedSr / 48.0f))); }

    int wrapIndex(int index) const noexcept
    {
        while (index < 0)
            index += kHistorySize;
        while (index >= kHistorySize)
            index -= kHistorySize;
        return index;
    }

    float historyAt(int offsetFromNewest) const noexcept
    {
        return sanitizeAudioSample(history[(size_t) wrapIndex(writePos - 1 - offsetFromNewest)]);
    }

    float computeCorrelation(int lag, int window) const noexcept
    {
        float sumXY = 0.0f;
        float sumXX = 0.0f;
        float sumYY = 0.0f;

        for (int i = 0; i < window; ++i)
        {
            const float x = historyAt(i);
            const float y = historyAt(i + lag);
            sumXY += x * y;
            sumXX += x * x;
            sumYY += y * y;
        }

        const float denom = std::sqrt(juce::jmax(1.0e-12f, sumXX * sumYY));
        return denom > 0.0f ? sumXY / denom : 0.0f;
    }

    void analysePitch(float envelope) noexcept
    {
        const int minLag = getMinLag();
        const int maxLag = getMaxLag();
        const int window = juce::jlimit(96, 256, maxLag + 24);

        float bestScore = -1.0f;
        int bestLag = juce::jlimit(minLag, maxLag, juce::roundToInt(trackedPeriod));
        float halfLagScore = -1.0f;
        const int halfCandidate = juce::jlimit(minLag, maxLag, juce::jmax(minLag, bestLag / 2));

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const float correlation = computeCorrelation(lag, window);
            const float continuityBias = 1.0f / (1.0f + std::abs((float) lag - trackedPeriod) * 0.012f);
            const float biasedScore = correlation * (0.88f + continuityBias * 0.12f);

            if (lag == halfCandidate)
                halfLagScore = correlation;

            if (biasedScore > bestScore)
            {
                bestScore = biasedScore;
                bestLag = lag;
            }
        }

        if (bestLag / 2 >= minLag)
        {
            const int candidate = bestLag / 2;
            const float candidateScore = computeCorrelation(candidate, window);
            if (candidateScore > bestScore * 0.94f)
            {
                bestLag = candidate;
                bestScore = candidateScore;
            }
        }
        else if (halfLagScore > bestScore * 0.95f)
        {
            bestLag = halfCandidate;
            bestScore = halfLagScore;
        }

        bestCorrelation = juce::jlimit(0.0f, 1.0f, bestScore);

        const float envelopeWeight = juce::jlimit(0.0f, 1.0f, (envelope - 0.003f) * 38.0f);
        const float confidenceTarget = juce::jlimit(0.0f, 1.0f,
            juce::jmap(bestCorrelation, 0.30f, 0.96f, 0.0f, 1.0f) * envelopeWeight);

        if (confidenceTarget > 0.12f)
        {
            const float lagBlend = juce::jmap(confidenceTarget, 0.08f, 0.24f);
            trackedPeriod += ((float) bestLag - trackedPeriod) * lagBlend;
        }

        confidence += (confidenceTarget - confidence) * (confidenceTarget > confidence ? 0.24f : 0.10f);
    }

    void pushDecimatedSample(float sample, float envelope) noexcept
    {
        history[(size_t) writePos] = sanitizeAudioSample(sample);
        writePos = (writePos + 1) % kHistorySize;

        if (++analysisCounter >= kAnalysisHop)
        {
            analysisCounter = 0;
            analysePitch(envelope);
        }
    }

    double sr = 44100.0;
    float decimatedSr = 11025.0f;
    std::array<float, kHistorySize> history {};
    int writePos = 0;
    float decimateAccumulator = 0.0f;
    int decimateCounter = 0;
    int analysisCounter = 0;
    float trackedPeriod = 0.0f;
    float smoothedFreq = 110.0f;
    float confidence = 0.0f;
    float bestCorrelation = 0.0f;
};
}

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
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        subSmooth.reset(sr, 0.04);
        upperSmooth.reset(sr, 0.04);
        drySmooth.reset(sr, 0.04);
        toneSmooth.reset(sr, 0.06);
        levelSmooth.reset(sr, 0.04);

        subSmooth.setCurrentAndTargetValue(subParam != nullptr ? snapBlendTarget(subParam->get()) : 0.72f);
        upperSmooth.setCurrentAndTargetValue(upperParam != nullptr ? snapBlendTarget(upperParam->get()) : 0.28f);
        drySmooth.setCurrentAndTargetValue(dryParam != nullptr ? snapBlendTarget(dryParam->get()) : 0.82f);
        toneSmooth.setCurrentAndTargetValue(toneParam != nullptr ? juce::jlimit(0.0f, 1.0f, toneParam->get()) : 0.50f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? snapLevelTarget(levelParam->get()) : 1.0f);

        detectorEnv.prepare(sr, 0.9f, 40.0f);
        voiceEnv.prepare(sr, 2.5f, 110.0f);
        tracker.prepare(sr);

        resetFilters();
        updateToneModel(toneSmooth.getCurrentValue(), true);

        setProcessingLatency(0);
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        resetFilters();
        detectorEnv.reset();
        voiceEnv.reset();
        tracker.reset();

        subPhase = 0.0f;
        upperPhase = 0.0f;

        subSmooth.setCurrentAndTargetValue(subParam != nullptr ? snapBlendTarget(subParam->get()) : 0.72f);
        upperSmooth.setCurrentAndTargetValue(upperParam != nullptr ? snapBlendTarget(upperParam->get()) : 0.28f);
        drySmooth.setCurrentAndTargetValue(dryParam != nullptr ? snapBlendTarget(dryParam->get()) : 0.82f);
        toneSmooth.setCurrentAndTargetValue(toneParam != nullptr ? juce::jlimit(0.0f, 1.0f, toneParam->get()) : 0.50f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? snapLevelTarget(levelParam->get()) : 1.0f);

        updateToneModel(toneSmooth.getCurrentValue(), true);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        scrubInvalidSamples(buffer);

        subSmooth.setTargetValue(subParam != nullptr ? snapBlendTarget(subParam->get()) : 0.72f);
        upperSmooth.setTargetValue(upperParam != nullptr ? snapBlendTarget(upperParam->get()) : 0.28f);
        drySmooth.setTargetValue(dryParam != nullptr ? snapBlendTarget(dryParam->get()) : 0.82f);
        toneSmooth.setTargetValue(toneParam != nullptr ? juce::jlimit(0.0f, 1.0f, toneParam->get()) : 0.50f);
        levelSmooth.setTargetValue(levelParam != nullptr ? snapLevelTarget(levelParam->get()) : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float subAmt = subSmooth.getNextValue();
            const float upperAmt = upperSmooth.getNextValue();
            const float dryAmt = drySmooth.getNextValue();
            const float tone = toneSmooth.getNextValue();
            const float level = levelSmooth.getNextValue();

            updateToneModel(tone, false);

            const bool dryOnly = subAmt <= 1.0e-4f
                && upperAmt <= 1.0e-4f
                && dryAmt >= 0.9999f
                && std::abs(level - 1.0f) <= 1.0e-4f;

            if (dryOnly)
                continue;

            float detectorInput = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                detectorInput += buffer.getSample(ch, sample);

            detectorInput *= 1.0f / (float) juce::jmax(1, numChannels);
            detectorInput = detectorHighPass.process(detectorInput);
            detectorInput = detectorLowPass.process(detectorInput);
            detectorInput = std::tanh(detectorInput * 3.2f);

            const float detectorLevel = detectorEnv.process(std::abs(detectorInput));
            const float trackedFreq = tracker.process(detectorInput, detectorLevel);
            const float confidence = tracker.getConfidence();
            const float voiceLevel = voiceEnv.process(detectorLevel * (0.18f + 0.82f * confidence));

            subPhase += trackedFreq * 0.5f / (float) sr;
            upperPhase += trackedFreq * 2.0f / (float) sr;
            subPhase -= std::floor(subPhase);
            upperPhase -= std::floor(upperPhase);

            const float subSine = std::sin(subPhase * juce::MathConstants<float>::twoPi);
            const float subSquare = std::tanh(subSine * (1.6f + tone * 2.5f));
            const float subTextureBlend = juce::jlimit(0.0f, 1.0f, 0.26f + tone * 0.42f);
            const float subCore = juce::jmap(subTextureBlend, subSine, subSquare)
                * (0.10f + voiceLevel * 1.45f);

            const float upperFund = std::sin(upperPhase * juce::MathConstants<float>::twoPi);
            const float upperThird = std::sin(upperPhase * juce::MathConstants<float>::twoPi * 3.0f);
            const float upperFifth = std::sin(upperPhase * juce::MathConstants<float>::twoPi * 5.0f);
            const float upperOscCore = std::tanh((upperFund
                + upperThird * (0.11f + tone * 0.34f)
                + upperFifth * (0.03f + tone * 0.14f))
                * (0.95f + tone * 2.6f)) * (0.08f + voiceLevel * (1.04f + tone * 0.56f));

            const float wetTrim = 1.0f / (1.0f + subAmt * 0.38f + upperAmt * 0.24f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);

                float subVoice = subCore;
                subVoice = subHighPass[(size_t) ch].process(subVoice);
                subVoice = subLowPass[(size_t) ch].process(subVoice);

                const float rectified = juce::jmax(0.0f, std::abs(dry) * (1.7f + voiceLevel * 2.9f) - (0.060f - tone * 0.024f));
                float upperTexture = upperTextureHighPass[(size_t) ch].process(rectified);
                upperTexture = upperTextureLowPass[(size_t) ch].process(upperTexture);
                const float textureAmount = juce::jlimit(0.0f, 1.0f, 0.06f + tone * 0.74f);
                const float harmonicLift = 0.54f + tone * 0.94f;
                const float textureLift = 0.08f + tone * 0.72f + voiceLevel * 1.55f;
                const float voicedUpper = upperOscCore * harmonicLift + upperTexture * textureLift;
                float upperVoice = juce::jmap(textureAmount, upperOscCore, voicedUpper);
                upperVoice = upperHighPass[(size_t) ch].process(upperVoice);
                upperVoice = upperLowPass[(size_t) ch].process(upperVoice);
                upperVoice *= 0.62f + tone * 0.96f;

                float wet = subVoice * (subAmt * 0.96f) + upperVoice * (upperAmt * (0.62f + tone * 0.90f));
                wet = dcBlock[(size_t) ch].process(wet);

                buffer.setSample(ch, sample, (dry * dryAmt + wet) * level * wetTrim);
            }
        }

        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* subParam = nullptr;
    juce::AudioParameterFloat* upperParam = nullptr;
    juce::AudioParameterFloat* dryParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

private:
    static void scrubInvalidSamples(juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = Nova::OctaveDSP::sanitizeAudioSample(data[i]);
        }
    }

    static float snapBlendTarget(float value) noexcept
    {
        if (value <= 1.0e-4f)
            return 0.0f;
        if (value >= 0.9999f)
            return 1.0f;
        return value;
    }

    static float snapLevelTarget(float value) noexcept
    {
        return std::abs(value - 1.0f) <= 1.0e-4f ? 1.0f : value;
    }

    void resetFilters() noexcept
    {
        detectorHighPass.reset();
        detectorLowPass.reset();

        for (auto& filter : subHighPass) filter.reset();
        for (auto& filter : subLowPass) filter.reset();
        for (auto& filter : upperTextureHighPass) filter.reset();
        for (auto& filter : upperTextureLowPass) filter.reset();
        for (auto& filter : upperHighPass) filter.reset();
        for (auto& filter : upperLowPass) filter.reset();
        for (auto& filter : dcBlock) filter.reset();
    }

    void updateToneModel(float tone, bool force)
    {
        if (!force && std::abs(tone - cachedTone) <= 0.004f)
            return;

        cachedTone = tone;

        detectorHighPass.setHighPass(46.0f + tone * 24.0f, 0.707f, sr);
        detectorLowPass.setLowPass(1200.0f + tone * 1500.0f, 0.707f, sr);

        const float subHighPassFreq = 18.0f + tone * 42.0f;
        const float subLowPassFreq = 240.0f + tone * 430.0f;
        const float upperTextureHighPassFreq = 90.0f + tone * 220.0f;
        const float upperTextureLowPassFreq = 900.0f + tone * 6200.0f;
        const float upperHighPassFreq = 120.0f + tone * 130.0f;
        const float upperLowPassFreq = 1100.0f + tone * 7200.0f;

        for (auto& filter : subHighPass)
            filter.setHighPass(subHighPassFreq, 0.707f, sr);
        for (auto& filter : subLowPass)
            filter.setLowPass(subLowPassFreq, 0.62f, sr);
        for (auto& filter : upperTextureHighPass)
            filter.setHighPass(upperTextureHighPassFreq, 0.707f, sr);
        for (auto& filter : upperTextureLowPass)
            filter.setLowPass(upperTextureLowPassFreq, 0.707f, sr);
        for (auto& filter : upperHighPass)
            filter.setHighPass(upperHighPassFreq, 0.707f, sr);
        for (auto& filter : upperLowPass)
            filter.setLowPass(upperLowPassFreq, 0.65f, sr);
        for (auto& filter : dcBlock)
            filter.setHighPass(22.0f, 0.707f, sr);
    }

    double sr = 44100.0;
    bool isPrepared = false;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> subSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> upperSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> drySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> toneSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    Nova::OctaveDSP::Biquad detectorHighPass;
    Nova::OctaveDSP::Biquad detectorLowPass;
    Nova::OctaveDSP::EnvelopeFollower detectorEnv;
    Nova::OctaveDSP::EnvelopeFollower voiceEnv;
    Nova::OctaveDSP::PeriodTracker tracker;

    std::array<Nova::OctaveDSP::Biquad, 2> subHighPass;
    std::array<Nova::OctaveDSP::Biquad, 2> subLowPass;
    std::array<Nova::OctaveDSP::Biquad, 2> upperTextureHighPass;
    std::array<Nova::OctaveDSP::Biquad, 2> upperTextureLowPass;
    std::array<Nova::OctaveDSP::Biquad, 2> upperHighPass;
    std::array<Nova::OctaveDSP::Biquad, 2> upperLowPass;
    std::array<Nova::OctaveDSP::Biquad, 2> dcBlock;

    float subPhase = 0.0f;
    float upperPhase = 0.0f;
    float cachedTone = -1.0f;
};

#include "OctaveEditor.h"
