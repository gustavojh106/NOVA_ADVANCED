#pragma once

#include "../Base/ProcessorBase.h"
#include "../Distortion/DistortionPedal.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

class MetalDistortionPedal final : public ProcessorBase
{
public:
    MetalDistortionPedal()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam = new juce::AudioParameterFloat("metalGain", "Gain", 0.0f, 100.0f, 68.0f));
        addParameter(lowParam = new juce::AudioParameterFloat("metalLow", "Low", -12.0f, 12.0f, 3.0f));
        addParameter(midParam = new juce::AudioParameterFloat("metalMid", "Mid", -15.0f, 15.0f, -3.5f));
        addParameter(midFreqParam = new juce::AudioParameterFloat("metalMidFreq", "Mid Freq", 250.0f, 5000.0f, 950.0f));
        addParameter(highParam = new juce::AudioParameterFloat("metalHigh", "High", -12.0f, 12.0f, 4.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("metalLevel", "Level", 0.0f, 1.0f, 0.68f));
    }

    const juce::String getName() const override { return "Metal Distortion"; }
    double getTailLengthSeconds() const override { return 0.05; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        innerSr = sr * 8.0;

        oversampler.reset();
        oversampler.initProcessing((size_t) juce::jmax(1, samplesPerBlock));

        gainSmooth.reset(sr, Nova::Config::SMOOTH_DRIVE_SECONDS);
        lowSmooth.reset(sr, 0.05);
        midSmooth.reset(sr, 0.05);
        midFreqSmooth.reset(sr, 0.06);
        highSmooth.reset(sr, 0.05);
        levelSmooth.reset(sr, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? gainParam->get() : 68.0f);
        lowSmooth.setCurrentAndTargetValue(lowParam != nullptr ? lowParam->get() : 3.0f);
        midSmooth.setCurrentAndTargetValue(midParam != nullptr ? midParam->get() : -3.5f);
        midFreqSmooth.setCurrentAndTargetValue(midFreqParam != nullptr ? midFreqParam->get() : 950.0f);
        highSmooth.setCurrentAndTargetValue(highParam != nullptr ? highParam->get() : 4.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelFromControl(levelParam->get()) : 1.0f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

        resetFiltersAndDynamics();
        updateFilters(gainParam != nullptr ? gainParam->get() : 68.0f,
            lowParam != nullptr ? lowParam->get() : 3.0f,
            midParam != nullptr ? midParam->get() : -3.5f,
            midFreqParam != nullptr ? midFreqParam->get() : 950.0f,
            highParam != nullptr ? highParam->get() : 4.0f,
            true);

        gateEnvelopeAttackCoeff = std::exp(-1.0f / ((float) sr * 0.0010f));
        gateEnvelopeReleaseCoeff = std::exp(-1.0f / ((float) sr * 0.032f));
        gateOpenCoeff = std::exp(-1.0f / ((float) sr * 0.0008f));
        gateCloseCoeff = std::exp(-1.0f / ((float) sr * 0.046f));
        sagAttackCoeff = std::exp(-1.0f / ((float) innerSr * 0.0024f));
        sagReleaseCoeff = std::exp(-1.0f / ((float) innerSr * 0.082f));

        setProcessingLatency((int) std::round(oversampler.getLatencyInSamples()));
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
        oversampler.reset();
        resetFiltersAndDynamics();
        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? gainParam->get() : 68.0f);
        lowSmooth.setCurrentAndTargetValue(lowParam != nullptr ? lowParam->get() : 3.0f);
        midSmooth.setCurrentAndTargetValue(midParam != nullptr ? midParam->get() : -3.5f);
        midFreqSmooth.setCurrentAndTargetValue(midFreqParam != nullptr ? midFreqParam->get() : 950.0f);
        highSmooth.setCurrentAndTargetValue(highParam != nullptr ? highParam->get() : 4.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelFromControl(levelParam->get()) : 1.0f);
        updateFilters(gainParam != nullptr ? gainParam->get() : 68.0f,
            lowParam != nullptr ? lowParam->get() : 3.0f,
            midParam != nullptr ? midParam->get() : -3.5f,
            midFreqParam != nullptr ? midFreqParam->get() : 950.0f,
            highParam != nullptr ? highParam->get() : 4.0f,
            true);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        if (scratchBuffer.getNumChannels() < numChannels
            || scratchBuffer.getNumSamples() < numSamples)
        {
            scratchBuffer.setSize(numChannels,
                numSamples,
                false,
                false,
                true);
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), numSamples);

        gainSmooth.setTargetValue(gainParam != nullptr ? gainParam->get() : 68.0f);
        lowSmooth.setTargetValue(lowParam != nullptr ? lowParam->get() : 3.0f);
        midSmooth.setTargetValue(midParam != nullptr ? midParam->get() : -3.5f);
        midFreqSmooth.setTargetValue(midFreqParam != nullptr ? midFreqParam->get() : 950.0f);
        highSmooth.setTargetValue(highParam != nullptr ? highParam->get() : 4.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelFromControl(levelParam->get()) : 1.0f);
        updateFilters(gainParam != nullptr ? gainParam->get() : 68.0f,
            lowParam != nullptr ? lowParam->get() : 3.0f,
            midParam != nullptr ? midParam->get() : -3.5f,
            midFreqParam != nullptr ? midFreqParam->get() : 950.0f,
            highParam != nullptr ? highParam->get() : 4.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int innerSamples = (int) upsampled.getNumSamples();
        const int oversampleRatio = juce::jmax(1, innerSamples / juce::jmax(1, numSamples));

        float currentGain = gainSmooth.getCurrentValue();
        float currentLow = lowSmooth.getCurrentValue();
        float currentMid = midSmooth.getCurrentValue();
        float currentMidFreq = midFreqSmooth.getCurrentValue();
        float currentHigh = highSmooth.getCurrentValue();
        updateFilters(currentGain, currentLow, currentMid, currentMidFreq, currentHigh, true);

        for (int sample = 0; sample < innerSamples; ++sample)
        {
            if ((sample % oversampleRatio) == 0)
            {
                currentGain = gainSmooth.getNextValue();
                currentLow = lowSmooth.getNextValue();
                currentMid = midSmooth.getNextValue();
                currentMidFreq = midFreqSmooth.getNextValue();
                currentHigh = highSmooth.getNextValue();
                updateFilters(currentGain, currentLow, currentMid, currentMidFreq, currentHigh, true);
            }

            const float driveNorm = juce::jlimit(0.0f, 1.0f, currentGain * 0.01f);

            for (int ch = 0; ch < (int) upsampled.getNumChannels(); ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t) ch);
                float x = data[sample];

                x = inputHP[(size_t) ch].process(x);
                x = preFocus[(size_t) ch].process(x);
                x = preLP[(size_t) ch].process(x);

                sagEnvelope[(size_t) ch] = updateEnvelope(sagEnvelope[(size_t) ch], std::abs(x), sagAttackCoeff, sagReleaseCoeff);
                const float sag = 1.0f - sagEnvelope[(size_t) ch] * (0.08f + driveNorm * 0.22f);
                const float sagGain = juce::jlimit(0.42f, 1.0f, sag);

                float stage = x * juce::Decibels::decibelsToGain(13.0f + currentGain * 0.25f) * sagGain;
                stage = Nova::DistortionDSP::asymSoftClip(stage, 1.70f + driveNorm * 0.65f, 1.42f + driveNorm * 0.32f);

                const float diode = Nova::DistortionDSP::diodeClip(stage * (1.48f + driveNorm * 2.00f), 0.54f - driveNorm * 0.20f, 0.62f - driveNorm * 0.12f);
                const float led = Nova::DistortionDSP::ledClip((diode + stage * 0.12f) * (1.12f + driveNorm * 1.18f), 0.72f - driveNorm * 0.18f);
                const float hard = juce::jlimit(-1.0f, 1.0f, led * (1.0f + driveNorm * 0.10f));

                float out = Nova::DistortionDSP::blend(diode, led, 0.58f);
                out = Nova::DistortionDSP::blend(out, hard, 0.24f + driveNorm * 0.18f);
                out += 0.035f * std::sin(out * juce::MathConstants<float>::pi * (1.2f + driveNorm * 1.4f));

                out = lowShelf[(size_t) ch].process(out);
                out = midPeak[(size_t) ch].process(out);
                out = highShelf[(size_t) ch].process(out);
                out = topCut[(size_t) ch].process(out);
                out = dcBlock[(size_t) ch].process(out);
                data[sample] = out * juce::Decibels::decibelsToGain(-1.8f - driveNorm * 1.8f);

                juce::ignoreUnused(currentLow, currentMid, currentMidFreq, currentHigh);
            }
        }

        oversampler.processSamplesDown(block);
        applyAdaptiveGate(buffer, numChannels, numSamples);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float level = levelSmooth.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * level);
        }

        endBypassProcess(buffer);
    }

    static float computeEqResponse(float freq, float lowDb, float midDb, float midFreq, float highDb, float gainPercent, double sampleRate)
    {
        struct ResponseBiquad
        {
            float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
            float a1 = 0.0f, a2 = 0.0f;

            void setLowShelf(float frequency, float gainDb, float q, double sr) noexcept
            {
                const float A = std::pow(10.0f, gainDb / 40.0f);
                const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), frequency) / (float) sr;
                const float sinW0 = std::sin(w0);
                const float cosW0 = std::cos(w0);
                const float alpha = sinW0 / (2.0f * q);
                const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
                const float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha);

                b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
                b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
                b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
                a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
                a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
            }

            void setHighShelf(float frequency, float gainDb, float q, double sr) noexcept
            {
                const float A = std::pow(10.0f, gainDb / 40.0f);
                const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), frequency) / (float) sr;
                const float sinW0 = std::sin(w0);
                const float cosW0 = std::cos(w0);
                const float alpha = sinW0 / (2.0f * q);
                const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
                const float a0inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha);

                b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
                b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
                b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
                a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
                a2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
            }

            void setPeak(float frequency, float gainDb, float q, double sr) noexcept
            {
                const float A = std::pow(10.0f, gainDb / 40.0f);
                const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), frequency) / (float) sr;
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

            float magnitudeAt(float frequency, double sr) const noexcept
            {
                const float w = juce::MathConstants<float>::twoPi * frequency / (float) sr;
                const float cw = std::cos(w);
                const float sw = std::sin(w);
                const float c2w = std::cos(2.0f * w);
                const float s2w = std::sin(2.0f * w);
                const float nr = b0 + b1 * cw + b2 * c2w;
                const float ni = -(b1 * sw + b2 * s2w);
                const float dr = 1.0f + a1 * cw + a2 * c2w;
                const float di = -(a1 * sw + a2 * s2w);
                return std::sqrt((nr * nr + ni * ni) / juce::jmax(dr * dr + di * di, 1.0e-12f));
            }
        };

        ResponseBiquad lowShelfViz;
        ResponseBiquad midPeakViz;
        ResponseBiquad highShelfViz;

        lowShelfViz.setLowShelf(110.0f, lowDb, 0.72f, sampleRate);
        midPeakViz.setPeak(midFreq, midDb + juce::jmap(juce::jlimit(0.0f, 100.0f, gainPercent) * 0.01f, -0.6f, 0.8f), 0.85f, sampleRate);
        highShelfViz.setHighShelf(3600.0f, highDb - juce::jmap(juce::jlimit(0.0f, 100.0f, gainPercent) * 0.01f, 0.0f, 1.8f), 0.72f, sampleRate);

        const float magnitude = lowShelfViz.magnitudeAt(freq, sampleRate)
            * midPeakViz.magnitudeAt(freq, sampleRate)
            * highShelfViz.magnitudeAt(freq, sampleRate);
        return juce::Decibels::gainToDecibels(juce::jmax(1.0e-6f, magnitude));
    }

    juce::AudioParameterFloat* gainParam = nullptr;
    juce::AudioParameterFloat* lowParam = nullptr;
    juce::AudioParameterFloat* midParam = nullptr;
    juce::AudioParameterFloat* midFreqParam = nullptr;
    juce::AudioParameterFloat* highParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    float currentGateGain = 1.0f;

private:
    static float levelFromControl(float control) noexcept
    {
        const float levelDb = juce::jmap(juce::jlimit(0.0f, 1.0f, control), -18.0f, 6.0f);
        return juce::Decibels::decibelsToGain(levelDb);
    }

    static float updateEnvelope(float current, float detector, float attackCoeff, float releaseCoeff) noexcept
    {
        const float coeff = detector > current ? attackCoeff : releaseCoeff;
        return detector + coeff * (current - detector);
    }

    void resetFiltersAndDynamics()
    {
        for (auto& filter : inputHP)
            filter.reset();
        for (auto& filter : preFocus)
            filter.reset();
        for (auto& filter : preLP)
            filter.reset();
        for (auto& filter : lowShelf)
            filter.reset();
        for (auto& filter : midPeak)
            filter.reset();
        for (auto& filter : highShelf)
            filter.reset();
        for (auto& filter : topCut)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        inputGate.reset();
        sagEnvelope.fill(0.0f);
        currentGateGain = 1.0f;
        cachedGain = -999.0f;
        cachedLow = -999.0f;
        cachedMid = -999.0f;
        cachedMidFreq = -999.0f;
        cachedHigh = -999.0f;
    }

    void updateFilters(float gain, float low, float mid, float midFreq, float high, bool force = false)
    {
        if (!force
            && std::abs(cachedGain - gain) < 0.1f
            && std::abs(cachedLow - low) < 0.05f
            && std::abs(cachedMid - mid) < 0.05f
            && std::abs(cachedMidFreq - midFreq) < 1.0f
            && std::abs(cachedHigh - high) < 0.05f)
            return;

        cachedGain = gain;
        cachedLow = low;
        cachedMid = mid;
        cachedMidFreq = midFreq;
        cachedHigh = high;

        const float driveNorm = juce::jlimit(0.0f, 1.0f, gain * 0.01f);
        const float hpFreq = juce::jmap(driveNorm, 72.0f, 168.0f);
        const float topCutHz = juce::jmap(driveNorm, 9200.0f, 5600.0f) - high * 90.0f;
        const float focusBoost = 1.5f + driveNorm * 2.4f;

        for (auto& filter : inputHP)
            filter.setHighPass(hpFreq, 0.92f, innerSr);
        for (auto& filter : preFocus)
            filter.setPeak(1220.0f, focusBoost, 0.84f, innerSr);
        for (auto& filter : preLP)
            filter.setLowPass(14800.0f - driveNorm * 2200.0f, 0.72f, innerSr);
        for (auto& filter : lowShelf)
            filter.setLowShelf(110.0f, low, 0.72f, innerSr);
        for (auto& filter : midPeak)
            filter.setPeak(midFreq, mid + driveNorm * 0.8f, 0.85f, innerSr);
        for (auto& filter : highShelf)
            filter.setHighShelf(3600.0f, high - driveNorm * 1.8f, 0.72f, innerSr);
        for (auto& filter : topCut)
            filter.setLowPass(juce::jlimit(3200.0f, (float) innerSr * 0.45f, topCutHz), 0.72f, innerSr);
        for (auto& filter : dcBlock)
            filter.setHighPass(18.0f, 0.707f, innerSr);
    }

    void applyAdaptiveGate(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples)
    {
        const float driveNorm = juce::jlimit(0.0f, 1.0f, (gainParam != nullptr ? gainParam->get() : 68.0f) * 0.01f);
        const float openThresholdDb = -72.0f + driveNorm * 18.0f;
        const float closeThresholdDb = openThresholdDb - 6.0f;
        const float floorGain = 0.08f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                peak = juce::jmax(peak, std::abs(buffer.getSample(ch, sample)));

            currentGateGain = inputGate.process(peak,
                openThresholdDb,
                closeThresholdDb,
                gateEnvelopeAttackCoeff,
                gateEnvelopeReleaseCoeff,
                gateOpenCoeff,
                gateCloseCoeff,
                floorGain);

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * currentGateGain);
        }
    }

    double sr = 44100.0;
    double innerSr = 352800.0;

    juce::dsp::Oversampling<float> oversampler;
    std::array<Nova::DistortionDSP::Biquad, 2> inputHP;
    std::array<Nova::DistortionDSP::Biquad, 2> preFocus;
    std::array<Nova::DistortionDSP::Biquad, 2> preLP;
    std::array<Nova::DistortionDSP::Biquad, 2> lowShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> midPeak;
    std::array<Nova::DistortionDSP::Biquad, 2> highShelf;
    std::array<Nova::DistortionDSP::Biquad, 2> topCut;
    std::array<Nova::DistortionDSP::Biquad, 2> dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midFreqSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    Nova::DistortionDSP::Gate inputGate;
    std::array<float, 2> sagEnvelope {};
    float gateEnvelopeAttackCoeff = 0.0f;
    float gateEnvelopeReleaseCoeff = 0.0f;
    float gateOpenCoeff = 0.0f;
    float gateCloseCoeff = 0.0f;
    float sagAttackCoeff = 0.0f;
    float sagReleaseCoeff = 0.0f;

    float cachedGain = -999.0f;
    float cachedLow = -999.0f;
    float cachedMid = -999.0f;
    float cachedMidFreq = -999.0f;
    float cachedHigh = -999.0f;
    bool isPrepared = false;
};

#include "MetalEditor.h"
