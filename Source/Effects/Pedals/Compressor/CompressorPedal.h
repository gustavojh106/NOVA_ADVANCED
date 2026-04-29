#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

namespace Nova::CompressorDSP
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

    void setHighPass(float frequencyHz, float q, double sampleRate) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sampleRate * 0.45), frequencyHz);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sampleRate;
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

        const float peakBlend = juce::jmap(juce::jlimit(0.0f, 1.0f, focusAmount), 0.34f, 0.74f);
        const float hybrid = (rmsEnvelope * (1.0f - peakBlend)) + (peakEnvelope * peakBlend);
        return juce::jmax(hybrid, peakEnvelope * 0.80f);
    }

    float getPeakEnvelope() const noexcept { return peakEnvelope; }
    float getRmsEnvelope() const noexcept { return std::sqrt(juce::jmax(rmsSquare, 0.0f)); }
};
} // namespace Nova::CompressorDSP

// Studio compressor:
// - linked stereo detection to keep the image stable
// - sidechain high-pass focus to avoid low-end pumping
// - hybrid peak/RMS detection for better transient/body balance
// - program-dependent release so sustained notes recover naturally
class CompressorPedal final : public ProcessorBase
{
public:
    CompressorPedal()
    {
        addParameter(thresholdParam = new juce::AudioParameterFloat("compThreshold", "Threshold", -42.0f, 0.0f, -20.0f));
        addParameter(ratioParam     = new juce::AudioParameterFloat("compRatio",     "Ratio",     1.0f, 12.0f, 4.0f));
        addParameter(attackParam    = new juce::AudioParameterFloat("compAttack",    "Attack",    1.0f, 80.0f, 12.0f));
        addParameter(releaseParam   = new juce::AudioParameterFloat("compRelease",   "Release",   25.0f, 350.0f, 120.0f));
        addParameter(kneeParam      = new juce::AudioParameterFloat("compKnee",      "Knee",      1.0f, 12.0f, 6.0f));
        addParameter(focusParam     = new juce::AudioParameterFloat("compFocus",     "Focus",     0.0f, 1.0f, 0.55f));
        addParameter(blendParam     = new juce::AudioParameterFloat("compBlend",     "Blend",     0.0f, 1.0f, 0.82f));
        addParameter(makeupParam    = new juce::AudioParameterFloat("compMakeup",    "Makeup",    0.0f, 18.0f, 2.5f));
    }

    const juce::String getName() const override { return "Compressor"; }
    double getTailLengthSeconds() const override { return 0.02; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxProcessingChannels, juce::jmax(2, getTotalNumOutputChannels()));

        dryBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);

        thresholdSmooth.reset(sampleRate, 0.05);
        ratioSmooth.reset(sampleRate, 0.05);
        attackSmooth.reset(sampleRate, 0.05);
        releaseSmooth.reset(sampleRate, 0.05);
        kneeSmooth.reset(sampleRate, 0.05);
        focusSmooth.reset(sampleRate, 0.05);
        blendSmooth.reset(sampleRate, 0.04);
        makeupSmooth.reset(sampleRate, 0.05);

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? thresholdParam->get() : -20.0f);
        ratioSmooth.setCurrentAndTargetValue(ratioParam != nullptr ? ratioParam->get() : 4.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? attackParam->get() : 12.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? releaseParam->get() : 120.0f);
        kneeSmooth.setCurrentAndTargetValue(kneeParam != nullptr ? kneeParam->get() : 6.0f);
        focusSmooth.setCurrentAndTargetValue(focusParam != nullptr ? focusParam->get() : 0.55f);
        blendSmooth.setCurrentAndTargetValue(blendParam != nullptr ? blendParam->get() : 0.82f);
        makeupSmooth.setCurrentAndTargetValue(makeupParam != nullptr ? makeupParam->get() : 2.5f);

        cachedFocus = -1.0f;
        updateSidechainFilters(focusParam != nullptr ? focusParam->get() : 0.55f);

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
        for (auto& filter : sidechainHighPass)
            filter.reset();

        thresholdSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? thresholdParam->get() : -20.0f);
        ratioSmooth.setCurrentAndTargetValue(ratioParam != nullptr ? ratioParam->get() : 4.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? attackParam->get() : 12.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? releaseParam->get() : 120.0f);
        kneeSmooth.setCurrentAndTargetValue(kneeParam != nullptr ? kneeParam->get() : 6.0f);
        focusSmooth.setCurrentAndTargetValue(focusParam != nullptr ? focusParam->get() : 0.55f);
        blendSmooth.setCurrentAndTargetValue(blendParam != nullptr ? blendParam->get() : 0.82f);
        makeupSmooth.setCurrentAndTargetValue(makeupParam != nullptr ? makeupParam->get() : 2.5f);

        detector.reset();
        linkedGainReductionDb = 0.0f;
        currentGainReductionDb.store(0.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            currentGainReductionDb.store(0.0f);
            endBypassProcess(buffer);
            return;
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(dryBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                buffer.getNumSamples());

        const float thresholdTarget = thresholdParam != nullptr ? thresholdParam->get() : -20.0f;
        const float ratioTarget = ratioParam != nullptr ? ratioParam->get() : 4.0f;
        const float attackTarget = attackParam != nullptr ? attackParam->get() : 12.0f;
        const float releaseTarget = releaseParam != nullptr ? releaseParam->get() : 120.0f;
        const float kneeTarget = kneeParam != nullptr ? kneeParam->get() : 6.0f;
        const float focusTarget = focusParam != nullptr ? focusParam->get() : 0.55f;
        const float blendTarget = blendParam != nullptr ? blendParam->get() : 0.82f;
        const float makeupTarget = makeupParam != nullptr ? makeupParam->get() : 2.5f;

        thresholdSmooth.setTargetValue(thresholdTarget);
        ratioSmooth.setTargetValue(ratioTarget);
        attackSmooth.setTargetValue(attackTarget);
        releaseSmooth.setTargetValue(releaseTarget);
        kneeSmooth.setTargetValue(kneeTarget);
        focusSmooth.setTargetValue(focusTarget);
        blendSmooth.setTargetValue(blendTarget);
        makeupSmooth.setTargetValue(makeupTarget);
        updateSidechainFilters(focusTarget);

        if (blendTarget <= 0.0001f || blendTarget >= 0.9999f)
            blendSmooth.setCurrentAndTargetValue(blendTarget <= 0.0001f ? 0.0f : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        float blockPeakGainReductionDb = 0.0f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float thresholdDb = thresholdSmooth.getNextValue();
            const float ratio = juce::jmax(1.0f, ratioSmooth.getNextValue());
            const float attackMs = attackSmooth.getNextValue();
            const float releaseMs = releaseSmooth.getNextValue();
            const float kneeDb = kneeSmooth.getNextValue();
            const float focusAmount = focusSmooth.getNextValue();
            const float blend = juce::jlimit(0.0f, 1.0f, blendSmooth.getNextValue());
            const float makeupGain = juce::Decibels::decibelsToGain(makeupSmooth.getNextValue());

            float fullBandPeak = 0.0f;
            float focusedPeak = 0.0f;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float input = buffer.getSample(ch, sample);
                const float filtered = sidechainHighPass[(size_t) ch].process(input);
                fullBandPeak = juce::jmax(fullBandPeak, std::abs(input));
                focusedPeak = juce::jmax(focusedPeak, std::abs(filtered));
            }

            const float focusBlend = juce::jmap(focusAmount, 0.08f, 0.88f);
            const float fullBandFloor = juce::jmap(focusAmount, 1.0f, 0.42f);
            const float detectorInput = juce::jmax(fullBandPeak * fullBandFloor,
                (fullBandPeak * (1.0f - focusBlend)) + (focusedPeak * focusBlend * 1.18f));

            const float peakAttackCoeff = coefficientForTime(juce::jmax(0.15f, attackMs * juce::jmap(focusAmount, 0.30f, 0.18f)));
            const float peakReleaseCoeff = coefficientForTime(juce::jmax(10.0f, releaseMs * 0.45f));
            const float rmsCoeff = coefficientForTime(juce::jmax(4.0f, attackMs * juce::jmap(focusAmount, 2.1f, 1.2f)));
            const float detectorLinear = detector.process(detectorInput,
                peakAttackCoeff,
                peakReleaseCoeff,
                rmsCoeff,
                focusAmount);

            const float detectorDb = juce::Decibels::gainToDecibels(juce::jmax(detectorLinear, 1.0e-6f), -120.0f);
            const float targetGainReductionDb = computeGainReductionDb(detectorDb, thresholdDb, ratio, kneeDb);

            const float peakDb = juce::Decibels::gainToDecibels(juce::jmax(detector.getPeakEnvelope(), 1.0e-6f), -120.0f);
            const float rmsDb = juce::Decibels::gainToDecibels(juce::jmax(detector.getRmsEnvelope(), 1.0e-6f), -120.0f);
            const float crestNorm = juce::jlimit(0.0f, 1.0f, (peakDb - rmsDb) / 18.0f);
            const float depthNorm = juce::jlimit(0.0f, 1.0f, targetGainReductionDb / 18.0f);

            const float adaptiveAttackMs = juce::jmax(0.5f,
                attackMs * juce::jmap(crestNorm, 1.0f, 0.72f));
            const float adaptiveReleaseMs = juce::jmax(18.0f,
                releaseMs
                    * juce::jmap(depthNorm, 0.0f, 1.0f, 0.78f, 1.34f)
                    * juce::jmap(crestNorm, 0.0f, 1.0f, 1.12f, 0.70f));

            const float attackCoeff = coefficientForTime(adaptiveAttackMs);
            const float releaseCoeff = coefficientForTime(adaptiveReleaseMs);

            if (targetGainReductionDb > linkedGainReductionDb)
                linkedGainReductionDb = targetGainReductionDb + attackCoeff * (linkedGainReductionDb - targetGainReductionDb);
            else
                linkedGainReductionDb = targetGainReductionDb + releaseCoeff * (linkedGainReductionDb - targetGainReductionDb);

            blockPeakGainReductionDb = juce::jmax(blockPeakGainReductionDb, linkedGainReductionDb);

            const float compressionGain = juce::Decibels::decibelsToGain(-linkedGainReductionDb) * makeupGain;
            const float dryGain = std::cos(blend * juce::MathConstants<float>::halfPi);
            const float wetGain = std::sin(blend * juce::MathConstants<float>::halfPi);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = dryBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample) * compressionGain;
                buffer.setSample(ch, sample, (dry * dryGain) + (wet * wetGain));
            }
        }

        currentGainReductionDb.store(blockPeakGainReductionDb);
        endBypassProcess(buffer);
    }

    static float computeGainReductionDb(float inputDb, float thresholdDb, float ratio, float kneeWidthDb) noexcept
    {
        const float overDb = inputDb - thresholdDb;
        const float slope = 1.0f - (1.0f / juce::jmax(1.0f, ratio));
        const float effectiveKneeDb = juce::jmax(0.25f, kneeWidthDb);

        if (overDb <= -0.5f * effectiveKneeDb)
            return 0.0f;

        if (overDb >= 0.5f * effectiveKneeDb)
            return slope * overDb;

        const float kneeProgress = overDb + (0.5f * effectiveKneeDb);
        return slope * kneeProgress * kneeProgress / (2.0f * effectiveKneeDb);
    }

    float getCurrentGainReductionDb() const noexcept
    {
        return currentGainReductionDb.load();
    }

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

    juce::AudioParameterFloat* thresholdParam = nullptr;
    juce::AudioParameterFloat* ratioParam = nullptr;
    juce::AudioParameterFloat* attackParam = nullptr;
    juce::AudioParameterFloat* releaseParam = nullptr;
    juce::AudioParameterFloat* kneeParam = nullptr;
    juce::AudioParameterFloat* focusParam = nullptr;
    juce::AudioParameterFloat* blendParam = nullptr;
    juce::AudioParameterFloat* makeupParam = nullptr;

private:
    static constexpr int kMaxProcessingChannels = 2;

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

    float coefficientForTime(float milliseconds) const noexcept
    {
        const float clampedMs = juce::jmax(0.02f, milliseconds);
        const float timeInSeconds = clampedMs * 0.001f;
        return std::exp(-1.0f / (timeInSeconds * (float) juce::jmax(1.0, currentSampleRate)));
    }

    void updateSidechainFilters(float focusAmount)
    {
        if (std::abs(focusAmount - cachedFocus) < 0.001f)
            return;

        const float highPassHz = juce::jmap(juce::jlimit(0.0f, 1.0f, focusAmount), 35.0f, 220.0f);
        for (auto& filter : sidechainHighPass)
            filter.setHighPass(highPassHz, 0.7071f, currentSampleRate);

        cachedFocus = focusAmount;
    }

    juce::AudioBuffer<float> dryBuffer;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> thresholdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> ratioSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> releaseSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> kneeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> focusSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> blendSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> makeupSmooth;

    std::array<Nova::CompressorDSP::Biquad, 2> sidechainHighPass;
    Nova::CompressorDSP::HybridDetector detector;

    double currentSampleRate = 44100.0;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    float linkedGainReductionDb = 0.0f;
    float cachedFocus = -1.0f;
    std::atomic<float> currentGainReductionDb{ 0.0f };
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};

#include "CompressorEditor.h"
