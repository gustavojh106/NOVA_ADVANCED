#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

// ============================================================================
//  Metal Distortion — Tight multi-stage cascaded gain, 3-band EQ, integrated gate
//  Reference: Mesa Rectifier tightness, 5150 midrange, Diezel presence
// ============================================================================
namespace Nova { namespace MetalDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

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

    void setLowShelf(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float sinW0 = std::sin(w0), cosW0 = std::cos(w0);
        float alpha = sinW0 / (2.0f * q);
        float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
        float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha);
        b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
    }

    void setHighShelf(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float sinW0 = std::sin(w0), cosW0 = std::cos(w0);
        float alpha = sinW0 / (2.0f * q);
        float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
        float a0inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha);
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
    }

    void setPeak(float freq, float gainDb, float q, double sr)
    {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float sinW0 = std::sin(w0), cosW0 = std::cos(w0);
        float alpha = sinW0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha / A);
        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * cosW0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = b1;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float process(float x) noexcept
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Noise gate with hysteresis
struct Gate
{
    float envelope = 0.0f;
    float gainLinear = 0.0f;
    bool isOpen = false;

    void reset()
    {
        envelope = 0.0f;
        gainLinear = 0.0f;
        isOpen = false;
    }

    // threshDb: gate threshold in dB (e.g. -60)
    // Returns gain multiplier [0..1]
    float process(float inputLevel, float threshDb, float attackCoeff, float releaseCoeff) noexcept
    {
        // Envelope follower (peak)
        float absIn = std::abs(inputLevel);
        if (absIn > envelope)
            envelope = absIn + attackCoeff * (envelope - absIn);
        else
            envelope = absIn + releaseCoeff * (envelope - absIn);

        float envDb = 20.0f * std::log10(juce::jmax(envelope, 1.0e-8f));

        // Hysteresis: open at threshold, close at threshold - 6dB
        if (envDb > threshDb)
            isOpen = true;
        else if (envDb < threshDb - 6.0f)
            isOpen = false;

        // Smooth gain
        float target = isOpen ? 1.0f : 0.0f;
        float coeff = isOpen ? attackCoeff : releaseCoeff;
        gainLinear += coeff * (target - gainLinear);

        return gainLinear;
    }
};

}} // namespace Nova::MetalDSP


class MetalDistortionPedal final : public ProcessorBase
{
public:
    MetalDistortionPedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam    = new juce::AudioParameterFloat("metalGain",    "Gain",     0.0f,   100.0f,  64.0f));
        addParameter(lowParam     = new juce::AudioParameterFloat("metalLow",     "Low",     -12.0f,  12.0f,   3.0f));
        addParameter(midParam     = new juce::AudioParameterFloat("metalMid",     "Mid",     -15.0f,  15.0f,  -3.5f));
        addParameter(midFreqParam = new juce::AudioParameterFloat("metalMidFreq", "Mid Freq", 250.0f, 5000.0f, 950.0f));
        addParameter(highParam    = new juce::AudioParameterFloat("metalHigh",    "High",    -12.0f,  12.0f,   4.0f));
        addParameter(levelParam   = new juce::AudioParameterFloat("metalLevel",   "Level",    0.0f,   1.0f,    0.68f));
    }

    const juce::String getName() const override { return "Metal Distortion"; }
    double getTailLengthSeconds() const override { return 0.1; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        innerSr = sr * 4.0;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        gainSmooth.reset(sr, 0.04);
        levelSmooth.reset(sr, 0.04);
        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 64.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.68f);

        // Gate coefficients (~1ms attack, ~20ms release at base sample rate)
        gateAttackCoeff  = std::exp(-1.0f / ((float)sr * 0.001f));
        gateReleaseCoeff = std::exp(-1.0f / ((float)sr * 0.020f));
        for (auto& g : gate) g.reset();

        // Reset all biquads
        for (auto& f : tightHP)      f.reset();
        for (auto& f : preLP)        f.reset();
        for (auto& f : interLP1)     f.reset();
        for (auto& f : interLP2)     f.reset();
        for (auto& f : lowShelf)     f.reset();
        for (auto& f : midPeak)      f.reset();
        for (auto& f : highShelf)    f.reset();
        for (auto& f : outputLP)     f.reset();
        for (auto& f : dcBlock)      f.reset();
        for (auto& f : presencePeak) f.reset();

        cachedLow = -999.0f;
        cachedMid = -999.0f;
        cachedMidFreq = -999.0f;
        cachedHigh = -999.0f;
        cachedGain = -999.0f;

        setProcessingLatency((int)oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        oversampler.reset();
        for (auto& f : tightHP)      f.reset();
        for (auto& f : preLP)        f.reset();
        for (auto& f : interLP1)     f.reset();
        for (auto& f : interLP2)     f.reset();
        for (auto& f : lowShelf)     f.reset();
        for (auto& f : midPeak)      f.reset();
        for (auto& f : highShelf)    f.reset();
        for (auto& f : outputLP)     f.reset();
        for (auto& f : dcBlock)      f.reset();
        for (auto& f : presencePeak) f.reset();
        for (auto& g : gate) g.reset();

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 64.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.68f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateFilters();
        gainSmooth.setTargetValue(gainParam != nullptr ? *gainParam : 64.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 0.68f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();

        // Noise gate on input (before oversampling for efficiency)
        {
            const float gain = gainSmooth.getCurrentValue();
            // Gate threshold scales with gain: more gain → higher threshold needed
            const float gateThreshDb = juce::jmap(gain, 0.0f, 100.0f, -80.0f, -48.0f);

            for (int s = 0; s < numSamples; ++s)
            {
                // Use max of both channels for linked gate
                float peak = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    peak = juce::jmax(peak, std::abs(buffer.getSample(ch, s)));

                float gateGain = gate[0].process(peak, gateThreshDb,
                    gateAttackCoeff, gateReleaseCoeff);
                currentGateGain = gateGain;

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample(ch, s, buffer.getSample(ch, s) * gateGain);
            }
        }

        // Upsample
        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        const int innerChannels = (int)upsampled.getNumChannels();
        const int innerSamples  = (int)upsampled.getNumSamples();

        // Process at 4x sample rate
        for (int s = 0; s < innerSamples; ++s)
        {
            const float driveControl = gainSmooth.getNextValue();

            // Stage 1 drive: 10dB base + gain-scaled
            const float drive1 = juce::Decibels::decibelsToGain(8.0f + driveControl * 0.35f);
            // Stage 2: harder
            const float drive2 = 1.2f + driveControl * 0.02f;
            // Stage 3: final shaping
            const float drive3 = 1.0f + driveControl * 0.012f;

            for (int ch = 0; ch < innerChannels; ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t)ch);
                float x = data[s];

                // Tight input filter: resonant HP removes mud
                x = tightHP[(size_t)ch].process(x);
                // Anti-alias pre-filter
                x = preLP[(size_t)ch].process(x);

                // ---- Stage 1: Soft clip (first tube stage) ----
                x *= drive1;
                x = std::tanh(x * 0.85f);

                // Inter-stage LP: tames harsh harmonics between stages
                x = interLP1[(size_t)ch].process(x);

                // ---- Stage 2: Medium clip (second tube stage) ----
                x *= drive2;
                // Asymmetric: positive clips harder than negative (tube-like)
                float pos = std::tanh(x * 1.55f);
                float neg = std::tanh(x * 1.18f);
                x = x >= 0.0f ? pos : neg;

                // Inter-stage LP
                x = interLP2[(size_t)ch].process(x);

                // ---- Stage 3: Hard clip (power amp saturation) ----
                x *= drive3;
                x = std::tanh(x * 1.3f);

                // Subtle harmonic enrichment at high amplitude
                if (std::abs(x) > 0.6f)
                    x += 0.06f * std::sin(x * juce::MathConstants<float>::twoPi);

                data[s] = x;
            }
        }

        // Post-clipping EQ and filtering (still at inner rate)
        for (int s = 0; s < innerSamples; ++s)
        {
            for (int ch = 0; ch < innerChannels; ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t)ch);
                float x = data[s];

                x = lowShelf[(size_t)ch].process(x);
                x = midPeak[(size_t)ch].process(x);
                x = highShelf[(size_t)ch].process(x);
                x = presencePeak[(size_t)ch].process(x);
                x = outputLP[(size_t)ch].process(x);
                x = dcBlock[(size_t)ch].process(x);

                data[s] = x;
            }
        }

        // Downsample
        oversampler.processSamplesDown(block);

        // Output level
        for (int s = 0; s < numSamples; ++s)
        {
            const float level = juce::jmap(levelSmooth.getNextValue(), 0.10f, 1.55f);
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, s, buffer.getSample(ch, s) * level);
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* gainParam    = nullptr;
    juce::AudioParameterFloat* lowParam     = nullptr;
    juce::AudioParameterFloat* midParam     = nullptr;
    juce::AudioParameterFloat* midFreqParam = nullptr;
    juce::AudioParameterFloat* highParam    = nullptr;
    juce::AudioParameterFloat* levelParam   = nullptr;
    float currentGateGain = 1.0f;  // For gate indicator in editor

    // For editor EQ curve display
    static float computeEqResponse(float freqHz, float lowDb, float midDb,
        float midFreqHz, float highDb, float gain, double sampleRate)
    {
        // Approximate combined EQ magnitude at a given frequency
        auto shelfMag = [](float freq, float center, float gainDb, bool isLow) -> float
        {
            float ratio = freq / center;
            if (isLow) ratio = 1.0f / ratio;
            float x = ratio * ratio;
            float blend = x / (1.0f + x);
            return gainDb * (1.0f - blend);
        };

        auto peakMag = [](float freq, float center, float gainDb, float q) -> float
        {
            float ratio = freq / center;
            float logRatio = std::log2(ratio);
            float denom = 1.0f + (logRatio * logRatio * q * q * 4.0f);
            return gainDb / denom;
        };

        float db = 0.0f;
        db += shelfMag(freqHz, 110.0f, lowDb, true);
        db += peakMag(freqHz, midFreqHz, midDb, 0.82f);
        db += shelfMag(freqHz, 3200.0f, highDb, false);

        // Presence peak (fixed)
        db += peakMag(freqHz, 3500.0f, 3.5f, 1.2f);

        // Output LP (gain-dependent)
        float topCut = juce::jmap(gain, 0.0f, 100.0f, 7600.0f, 10800.0f);
        float lpRatio = freqHz / topCut;
        float lpAtten = -12.0f * lpRatio * lpRatio / (1.0f + lpRatio * lpRatio);
        db += lpAtten;

        return db;
    }

private:
    void updateFilters()
    {
        const float gain    = gainParam    != nullptr ? *gainParam    : 64.0f;
        const float low     = lowParam     != nullptr ? *lowParam     : 3.0f;
        const float mid     = midParam     != nullptr ? *midParam     : -3.5f;
        const float midFreq = midFreqParam != nullptr ? *midFreqParam : 950.0f;
        const float high    = highParam    != nullptr ? *highParam    : 4.0f;

        bool changed = std::abs(cachedGain - gain) > 0.1f
                     || std::abs(cachedLow - low) > 0.01f
                     || std::abs(cachedMid - mid) > 0.01f
                     || std::abs(cachedMidFreq - midFreq) > 0.5f
                     || std::abs(cachedHigh - high) > 0.01f;

        if (!changed) return;

        cachedGain = gain;
        cachedLow = low;
        cachedMid = mid;
        cachedMidFreq = midFreq;
        cachedHigh = high;

        // Tight input HP: resonant peak at ~100Hz for tight palm mutes
        // Higher Q at lower gain for articulation, lower Q at high gain for thickness
        float tightQ = juce::jmap(gain, 0.0f, 100.0f, 1.2f, 0.72f);
        float tightFreq = juce::jmap(gain, 0.0f, 100.0f, 80.0f, 110.0f);
        for (auto& f : tightHP)
            f.setHighPass(tightFreq, tightQ, innerSr);

        // Pre-clip anti-alias LP
        for (auto& f : preLP)
            f.setLowPass(16000.0f, 0.707f, innerSr);

        // Inter-stage LPs: tame between clipping stages
        float interFreq1 = juce::jmap(gain, 0.0f, 100.0f, 12000.0f, 8000.0f);
        for (auto& f : interLP1)
            f.setLowPass(interFreq1, 0.707f, innerSr);

        float interFreq2 = juce::jmap(gain, 0.0f, 100.0f, 10000.0f, 6500.0f);
        for (auto& f : interLP2)
            f.setLowPass(interFreq2, 0.65f, innerSr);

        // Post EQ
        for (auto& f : lowShelf)
            f.setLowShelf(110.0f, low, 0.74f, innerSr);
        for (auto& f : midPeak)
            f.setPeak(midFreq, mid, 0.82f, innerSr);
        for (auto& f : highShelf)
            f.setHighShelf(3200.0f, high, 0.72f, innerSr);

        // Fixed presence peak for cut and attack
        for (auto& f : presencePeak)
            f.setPeak(3500.0f, 3.5f, 1.2f, innerSr);

        // Gain-dependent output LP: higher gain → tighter top to reduce fizz
        float topCut = juce::jmap(gain, 0.0f, 100.0f, 7600.0f, 10800.0f);
        for (auto& f : outputLP)
            f.setLowPass(topCut, 0.75f, innerSr);

        for (auto& f : dcBlock)
            f.setHighPass(18.0f, 0.707f, innerSr);
    }

    double sr = 44100.0;
    double innerSr = 176400.0;

    juce::dsp::Oversampling<float> oversampler;

    // Per-channel biquad filters (index 0 = L, 1 = R)
    std::array<Nova::MetalDSP::Biquad, 2> tightHP;
    std::array<Nova::MetalDSP::Biquad, 2> preLP;
    std::array<Nova::MetalDSP::Biquad, 2> interLP1;
    std::array<Nova::MetalDSP::Biquad, 2> interLP2;
    std::array<Nova::MetalDSP::Biquad, 2> lowShelf;
    std::array<Nova::MetalDSP::Biquad, 2> midPeak;
    std::array<Nova::MetalDSP::Biquad, 2> highShelf;
    std::array<Nova::MetalDSP::Biquad, 2> presencePeak;
    std::array<Nova::MetalDSP::Biquad, 2> outputLP;
    std::array<Nova::MetalDSP::Biquad, 2> dcBlock;

    std::array<Nova::MetalDSP::Gate, 2> gate;
    float gateAttackCoeff  = 0.99f;
    float gateReleaseCoeff = 0.999f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    float cachedGain    = -999.0f;
    float cachedLow     = -999.0f;
    float cachedMid     = -999.0f;
    float cachedMidFreq = -999.0f;
    float cachedHigh    = -999.0f;
    bool isPrepared = false;
};

#include "MetalEditor.h"
