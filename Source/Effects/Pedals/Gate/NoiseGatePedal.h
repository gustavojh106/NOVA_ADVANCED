#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

// ============================================================================
//  Noise Gate
//  Linked stereo detector with lookahead, adjustable hysteresis, focused
//  sidechain filtering, hybrid peak/RMS detection, and adaptive closing.
// ============================================================================
namespace Nova { namespace GateDSP {

inline float smoothstep(float edge0, float edge1, float x) noexcept
{
    if (edge1 <= edge0)
        return x >= edge1 ? 1.0f : 0.0f;

    const float t = juce::jlimit(0.0f, 1.0f, (x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
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

    void setPeak(float freq, float gainDb, float q, double sr) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha / A);

        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * cosW0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = b1;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct LookaheadDelay
{
    std::vector<float> buffer;
    int writePos = 0;
    int size = 1;

    void allocate(int maxSamples)
    {
        size = juce::jmax(1, maxSamples);
        buffer.assign((size_t) size, 0.0f);
        writePos = 0;
    }

    void clear()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    float process(float input, int delaySamples) noexcept
    {
        const int clampedDelay = juce::jlimit(0, size - 1, delaySamples);
        int readPos = writePos - clampedDelay;
        if (readPos < 0)
            readPos += size;

        const float output = buffer[(size_t) readPos];
        buffer[(size_t) writePos] = input;

        if (++writePos >= size)
            writePos = 0;

        return output;
    }
};

struct HybridDetector
{
    float peakEnvelope = 0.0f;
    float rmsSquare = 0.0f;

    void reset() noexcept
    {
        peakEnvelope = 0.0f;
        rmsSquare = 0.0f;
    }

    float process(float inputLevel,
        float peakAttackCoeff,
        float peakReleaseCoeff,
        float rmsCoeff,
        float focusAmount) noexcept
    {
        const float absIn = std::abs(inputLevel);
        const float peakCoeff = absIn > peakEnvelope ? peakAttackCoeff : peakReleaseCoeff;
        peakEnvelope = absIn + peakCoeff * (peakEnvelope - absIn);

        const float squared = absIn * absIn;
        rmsSquare = squared + rmsCoeff * (rmsSquare - squared);
        const float rmsEnvelope = std::sqrt(juce::jmax(rmsSquare, 0.0f));

        const float peakBlend = juce::jmap(juce::jlimit(0.0f, 1.0f, focusAmount), 0.36f, 0.74f);
        const float hybrid = (rmsEnvelope * (1.0f - peakBlend)) + (peakEnvelope * peakBlend);
        return juce::jmax(hybrid, peakEnvelope * 0.82f);
    }
};

}} // namespace Nova::GateDSP

class NoiseGatePedal final : public ProcessorBase
{
public:
    NoiseGatePedal()
    {
        addParameter(thresholdParam = new juce::AudioParameterFloat("gateThreshold", "Threshold", -80.0f, 0.0f, -46.0f));
        addParameter(attackParam = new juce::AudioParameterFloat("gateAttack", "Attack", 0.02f, 25.0f, 0.25f));
        addParameter(holdParam = new juce::AudioParameterFloat("gateHold", "Hold", 0.0f, 400.0f, 70.0f));
        addParameter(releaseParam = new juce::AudioParameterFloat("gateRelease", "Release", 10.0f, 700.0f, 115.0f));
        addParameter(rangeParam = new juce::AudioParameterFloat("gateRange", "Range", -96.0f, 0.0f, -96.0f));
        addParameter(hysteresisParam = new juce::AudioParameterFloat("gateHysteresis", "Hysteresis", 1.0f, 18.0f, 8.0f));
        addParameter(focusParam = new juce::AudioParameterFloat("gateFocus", "Focus", 0.0f, 1.0f, 0.55f));
    }

    const juce::String getName() const override { return "Noise Gate"; }
    double getTailLengthSeconds() const override { return 0.02; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        thresholdSmooth.reset(sr, 0.05);
        attackSmooth.reset(sr, 0.05);
        holdSmooth.reset(sr, 0.05);
        releaseSmooth.reset(sr, 0.05);
        rangeSmooth.reset(sr, 0.05);
        hysteresisSmooth.reset(sr, 0.05);
        focusSmooth.reset(sr, 0.05);

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? thresholdParam->get() : -46.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? attackParam->get() : 0.25f);
        holdSmooth.setCurrentAndTargetValue(holdParam != nullptr ? holdParam->get() : 70.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? releaseParam->get() : 115.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? rangeParam->get() : -96.0f);
        hysteresisSmooth.setCurrentAndTargetValue(hysteresisParam != nullptr ? hysteresisParam->get() : 8.0f);
        focusSmooth.setCurrentAndTargetValue(focusParam != nullptr ? focusParam->get() : 0.55f);

        lookaheadSamples = juce::jlimit(16, 192, juce::roundToInt(sr * 0.0015));
        for (auto& delay : lookahead)
            delay.allocate(samplesPerBlock + lookaheadSamples + 8);

        cachedFocus = -1.0f;
        updateDetectorFilters();

        setProcessingLatency(lookaheadSamples);
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
        for (auto& filter : scHighPass)
            filter.reset();
        for (auto& filter : scPresence)
            filter.reset();
        for (auto& filter : scLowPass)
            filter.reset();
        for (auto& delay : lookahead)
            delay.clear();

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? thresholdParam->get() : -46.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? attackParam->get() : 0.25f);
        holdSmooth.setCurrentAndTargetValue(holdParam != nullptr ? holdParam->get() : 70.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? releaseParam->get() : 115.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? rangeParam->get() : -96.0f);
        hysteresisSmooth.setCurrentAndTargetValue(hysteresisParam != nullptr ? hysteresisParam->get() : 8.0f);
        focusSmooth.setCurrentAndTargetValue(focusParam != nullptr ? focusParam->get() : 0.55f);

        detector.reset();
        gateGain = 1.0f;
        holdCounter = 0;
        gateOpen = true;
        currentGateGainAtomic.store(1.0f);
        currentDetectorDbAtomic.store(-96.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        thresholdSmooth.setTargetValue(thresholdParam != nullptr ? thresholdParam->get() : -46.0f);
        attackSmooth.setTargetValue(attackParam != nullptr ? attackParam->get() : 0.25f);
        holdSmooth.setTargetValue(holdParam != nullptr ? holdParam->get() : 70.0f);
        releaseSmooth.setTargetValue(releaseParam != nullptr ? releaseParam->get() : 115.0f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? rangeParam->get() : -96.0f);
        hysteresisSmooth.setTargetValue(hysteresisParam != nullptr ? hysteresisParam->get() : 8.0f);
        focusSmooth.setTargetValue(focusParam != nullptr ? focusParam->get() : 0.55f);
        updateDetectorFilters();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float thresholdDb = thresholdSmooth.getNextValue();
            const float attackMs = attackSmooth.getNextValue();
            const float holdMs = holdSmooth.getNextValue();
            const float releaseMs = releaseSmooth.getNextValue();
            const float rangeDb = rangeSmooth.getNextValue();
            const float hysteresisDb = hysteresisSmooth.getNextValue();
            const float focusAmount = focusSmooth.getNextValue();

            const float openThresholdDb = thresholdDb;
            const float closeThresholdDb = thresholdDb - hysteresisDb;
            const int holdSamples = juce::jmax(0, juce::roundToInt(holdMs * 0.001f * (float) sr));

            float fullBandPeak = 0.0f;
            float focusedPeak = 0.0f;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float input = buffer.getSample(ch, sample);
                const float filtered = scLowPass[(size_t) ch].process(
                    scPresence[(size_t) ch].process(
                        scHighPass[(size_t) ch].process(input)));

                fullBandPeak = juce::jmax(fullBandPeak, std::abs(input));
                focusedPeak = juce::jmax(focusedPeak, std::abs(filtered));
            }

            const float focusedBlend = juce::jmap(focusAmount, 0.10f, 0.95f);
            const float fullBandFloor = juce::jmap(focusAmount, 0.90f, 0.18f);
            const float detectorInput = juce::jmax(
                fullBandPeak * fullBandFloor,
                (fullBandPeak * (1.0f - focusedBlend)) + (focusedPeak * focusedBlend));

            const float detectorLin = detector.process(detectorInput,
                coeffForMs(0.08f),
                coeffForMs(juce::jmap(focusAmount, 0.0f, 1.0f, 36.0f, 18.0f)),
                coeffForMs(juce::jmap(focusAmount, 0.0f, 1.0f, 22.0f, 12.0f)),
                focusAmount);

            const float detectorDb = juce::Decibels::gainToDecibels(juce::jmax(detectorLin, 1.0e-6f), -120.0f);

            if (detectorDb >= openThresholdDb)
            {
                gateOpen = true;
                holdCounter = holdSamples;
            }
            else if (gateOpen)
            {
                if (detectorDb > closeThresholdDb)
                {
                    if (holdCounter > 0)
                        --holdCounter;
                }
                else if (holdCounter > 0)
                {
                    --holdCounter;
                }
                else
                {
                    gateOpen = false;
                }
            }

            float targetGainDb = 0.0f;
            if (gateOpen)
            {
                if (holdCounter > 0)
                {
                    targetGainDb = 0.0f;
                }
                else
                {
                    const float zone = Nova::GateDSP::smoothstep(closeThresholdDb, openThresholdDb, detectorDb);
                    targetGainDb = rangeDb * (1.0f - zone) * 0.24f;
                }
            }
            else
            {
                const float kneeStartDb = closeThresholdDb - juce::jmax(2.0f, hysteresisDb * 0.6f);
                const float zone = Nova::GateDSP::smoothstep(kneeStartDb, openThresholdDb, detectorDb);
                targetGainDb = rangeDb * (1.0f - zone);
            }

            const float targetGain = juce::Decibels::decibelsToGain(targetGainDb, -96.0f);
            const float openAttackMs = juce::jmax(0.02f, attackMs * juce::jmap(focusAmount, 1.0f, 0.72f));
            const float closeDepth = juce::jlimit(0.0f, 1.0f, (closeThresholdDb - detectorDb) / 18.0f);
            const float adaptiveReleaseMs = juce::jmax(8.0f,
                releaseMs
                    * juce::jmap(focusAmount, 0.0f, 1.0f, 1.10f, 0.78f)
                    * juce::jmap(closeDepth, 0.0f, 1.0f, 1.10f, 0.72f));

            const float coeff = targetGain > gateGain
                ? coeffForMs(openAttackMs)
                : coeffForMs(adaptiveReleaseMs);
            gateGain = targetGain + coeff * (gateGain - targetGain);

            currentGateGainAtomic.store(gateGain);
            currentDetectorDbAtomic.store(detectorDb);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = buffer.getSample(ch, sample);
                const float delayed = lookahead[(size_t) ch].process(dry, lookaheadSamples);
                buffer.setSample(ch, sample, delayed * gateGain);
            }
        }

        endBypassProcess(buffer);
    }

    static juce::String getFocusDescription(float focusAmount)
    {
        if (focusAmount < 0.20f)
            return "Wide detector that follows the full note envelope and leaves decays natural";
        if (focusAmount < 0.45f)
            return "Balanced detector with gentle low-end rejection and smooth studio-style tracking";
        if (focusAmount < 0.75f)
            return "Focused detector tuned for modern guitars, tighter lows and cleaner idle noise";
        return "Aggressive key filter that ignores rumble and clamps high-gain rigs fast";
    }

    juce::AudioParameterFloat* thresholdParam = nullptr;
    juce::AudioParameterFloat* attackParam = nullptr;
    juce::AudioParameterFloat* holdParam = nullptr;
    juce::AudioParameterFloat* releaseParam = nullptr;
    juce::AudioParameterFloat* rangeParam = nullptr;
    juce::AudioParameterFloat* hysteresisParam = nullptr;
    juce::AudioParameterFloat* focusParam = nullptr;

    std::atomic<float> currentGateGainAtomic{ 1.0f };
    std::atomic<float> currentDetectorDbAtomic{ -96.0f };

private:
    float coeffForMs(float ms) const noexcept
    {
        const float seconds = juce::jmax(0.00002f, ms * 0.001f);
        return std::exp(-1.0f / (seconds * (float) juce::jmax(1.0, sr)));
    }

    void updateDetectorFilters()
    {
        if (sr <= 0.0)
            return;

        const float focusAmount = focusParam != nullptr ? focusParam->get() : 0.55f;
        if (std::abs(focusAmount - cachedFocus) < 1.0e-4f)
            return;

        cachedFocus = focusAmount;

        const float hpFreq = juce::jmap(focusAmount, 40.0f, 340.0f);
        const float peakFreq = juce::jmap(focusAmount, 850.0f, 2900.0f);
        const float peakGainDb = juce::jmap(focusAmount, 0.0f, 12.0f);
        const float peakQ = juce::jmap(focusAmount, 0.72f, 1.35f);
        const float lpFreq = juce::jmap(focusAmount, 9500.0f, 5200.0f);

        for (auto& filter : scHighPass)
            filter.setHighPass(hpFreq, 0.707f, sr);
        for (auto& filter : scPresence)
            filter.setPeak(peakFreq, peakGainDb, peakQ, sr);
        for (auto& filter : scLowPass)
            filter.setLowPass(lpFreq, 0.78f, sr);
    }

    double sr = 44100.0;

    std::array<Nova::GateDSP::Biquad, 2> scHighPass;
    std::array<Nova::GateDSP::Biquad, 2> scPresence;
    std::array<Nova::GateDSP::Biquad, 2> scLowPass;
    std::array<Nova::GateDSP::LookaheadDelay, 2> lookahead;
    Nova::GateDSP::HybridDetector detector;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> thresholdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> holdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> releaseSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> hysteresisSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> focusSmooth;

    float gateGain = 1.0f;
    float cachedFocus = -1.0f;
    int holdCounter = 0;
    int lookaheadSamples = 64;
    bool gateOpen = true;
    bool isPrepared = false;
};

#include "GateEditor.h"
