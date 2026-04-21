#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace Nova::OctaveDSP
{
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
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
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
        const float coeff = absInput > state ? attackCoeff : releaseCoeff;
        state += coeff * (absInput - state);
        return state;
    }
};

struct PeriodTracker
{
    void prepare(double sampleRate)
    {
        sr = juce::jmax(1.0, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        lastSample = 0.0f;
        trackedPeriod = (float) (sr / 110.0);
        smoothedFreq = 110.0f;
        confidence = 0.0f;
        sinceLastTrigger = 1000000;
        idleSamples = 0;
    }

    float process(float conditioned, float envelope) noexcept
    {
        ++sinceLastTrigger;
        ++idleSamples;

        const float threshold = juce::jlimit(0.0025f, 0.06f, 0.004f + envelope * 0.14f);
        const bool armed = lastSample <= threshold * 0.10f;
        const bool trigger = armed && conditioned >= threshold && sinceLastTrigger > getMinPeriodSamples();

        if (trigger)
        {
            const int candidate = sinceLastTrigger;
            if (candidate >= getMinPeriodSamples() && candidate <= getMaxPeriodSamples())
            {
                const float candidatePeriod = (float) candidate;
                const float tolerance = juce::jmax(6.0f, trackedPeriod * 0.55f);
                const float blend = std::abs(candidatePeriod - trackedPeriod) <= tolerance ? 0.24f : 0.11f;
                trackedPeriod += (candidatePeriod - trackedPeriod) * blend;
                confidence = juce::jmin(1.0f, confidence + 0.16f);
                idleSamples = 0;
            }

            sinceLastTrigger = 0;
        }
        else if (idleSamples > (int) std::round(trackedPeriod * 1.8f))
        {
            confidence *= 0.995f;
        }

        if (envelope < threshold * 0.75f)
            confidence *= 0.992f;

        const float targetFreq = juce::jlimit(48.0f, 920.0f, (float) (sr / juce::jmax(1.0f, trackedPeriod)));
        smoothedFreq += (targetFreq - smoothedFreq) * 0.075f;
        lastSample = conditioned;
        return smoothedFreq;
    }

    float getConfidence() const noexcept { return confidence; }

private:
    int getMinPeriodSamples() const noexcept { return juce::jmax(6, (int) std::round(sr / 950.0)); }
    int getMaxPeriodSamples() const noexcept { return juce::jmax(16, (int) std::round(sr / 45.0)); }

    double sr = 44100.0;
    float lastSample = 0.0f;
    float trackedPeriod = 0.0f;
    float smoothedFreq = 110.0f;
    float confidence = 0.0f;
    int sinceLastTrigger = 0;
    int idleSamples = 0;
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
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

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
                + upperThird * (0.16f + tone * 0.28f)
                + upperFifth * (0.04f + tone * 0.10f))
                * (1.25f + tone * 2.0f)) * (0.12f + voiceLevel * 1.38f);

            const float wetTrim = 1.0f / (1.0f + subAmt * 0.38f + upperAmt * 0.24f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);

                float subVoice = subCore;
                subVoice = subHighPass[(size_t) ch].process(subVoice);
                subVoice = subLowPass[(size_t) ch].process(subVoice);

                const float rectified = juce::jmax(0.0f, std::abs(dry) * (1.8f + voiceLevel * 2.8f) - (0.055f - tone * 0.02f));
                float upperTexture = upperTextureHighPass[(size_t) ch].process(rectified);
                upperTexture = upperTextureLowPass[(size_t) ch].process(upperTexture);
                const float textureAmount = juce::jlimit(0.0f, 1.0f, 0.22f + tone * 0.68f);
                float upperVoice = juce::jmap(textureAmount, upperOscCore, upperTexture * (0.40f + tone * 0.34f + voiceLevel * 1.75f));
                upperVoice = upperHighPass[(size_t) ch].process(upperVoice);
                upperVoice = upperLowPass[(size_t) ch].process(upperVoice);

                float wet = subVoice * (subAmt * 0.96f) + upperVoice * (upperAmt * (0.86f + tone * 0.22f));
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
        const float upperTextureHighPassFreq = 150.0f + tone * 360.0f;
        const float upperTextureLowPassFreq = 1100.0f + tone * 5100.0f;
        const float upperHighPassFreq = 260.0f + tone * 340.0f;
        const float upperLowPassFreq = 1500.0f + tone * 5600.0f;

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
