#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Stereo Delay — analog-voiced with ping-pong, modulated time, progressive sat
//  Reference: Strymon El Capistan warmth, Boss DD-500 versatility
// ============================================================================
namespace Nova { namespace DelayDSP {

// ---- Biquad filter ----
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

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

    float process(float x) noexcept
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ---- Delay line with cubic Hermite interpolation ----
struct DelayLine
{
    std::vector<float> buf;
    int writePos = 0;
    int size = 1;

    void allocate(int maxSamples)
    {
        size = juce::jmax(4, maxSamples);
        buf.assign((size_t)size, 0.0f);
        writePos = 0;
    }

    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); writePos = 0; }

    void write(float sample)
    {
        buf[(size_t)writePos] = sample;
        if (++writePos >= size) writePos = 0;
    }

    float readCubic(float delaySamples) const noexcept
    {
        float clampedDelay = juce::jlimit(1.0f, (float)(size - 2), delaySamples);
        float rp = (float)writePos - clampedDelay;
        while (rp < 0.0f) rp += (float)size;

        int i1 = ((int)rp) % size;
        int i0 = (i1 - 1 + size) % size;
        int i2 = (i1 + 1) % size;
        int i3 = (i1 + 2) % size;
        float f = rp - std::floor(rp);

        float y0 = buf[(size_t)i0], y1 = buf[(size_t)i1];
        float y2 = buf[(size_t)i2], y3 = buf[(size_t)i3];
        float a = y3 - y2 - y0 + y1;
        float b = y0 - y1 - a;
        float c = y2 - y0;
        return y1 + f * (c + f * (b + f * a));
    }
};

// ---- Analog soft saturation (unity small-signal gain) ----
inline float analogSaturate(float x) noexcept
{
    // tanh: unity gain at x→0, bounded output ±1, adds odd harmonics
    // Each pass through the feedback loop adds progressive warmth
    return std::tanh(x);
}

// ---- Short all-pass delay for feedback diffusion (tape head smearing) ----
struct AllPassFilter
{
    std::vector<float> buf;
    int writePos = 0;
    int delay = 1;
    float coeff = 0.5f;

    void allocate(int maxDelay)
    {
        buf.assign((size_t)juce::jmax(4, maxDelay + 1), 0.0f);
        writePos = 0;
    }

    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); writePos = 0; }

    void setDelay(int d) { delay = juce::jlimit(1, (int)buf.size() - 1, d); }

    float process(float input) noexcept
    {
        int readPos = writePos - delay;
        if (readPos < 0) readPos += (int)buf.size();
        float delayed = buf[(size_t)readPos];
        float w = input + coeff * delayed;
        buf[(size_t)writePos] = w;
        float output = -coeff * w + delayed;
        if (++writePos >= (int)buf.size()) writePos = 0;
        return output;
    }
};

}} // namespace Nova::DelayDSP


// ============================================================================
//  DelayPedal — Processor
// ============================================================================
class DelayPedal final : public ProcessorBase
{
public:
    DelayPedal()
    {
        addParameter(timeParam     = new juce::AudioParameterFloat("delayTime",     "Time",     40.0f, 1400.0f, 420.0f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("delayFeedback", "Feedback", 0.0f, 0.95f, 0.42f));
        addParameter(toneParam     = new juce::AudioParameterFloat("delayTone",     "Tone",     600.0f, 12000.0f, 4800.0f));
        addParameter(spreadParam   = new juce::AudioParameterFloat("delaySpread",   "Spread",   0.0f, 1.0f, 0.42f));
        addParameter(mixParam      = new juce::AudioParameterFloat("delayMix",      "Mix",      0.0f, 1.0f, 0.3f));
    }

    const juce::String getName() const override { return "Delay"; }
    double getTailLengthSeconds() const override { return 3.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        // Max delay: 2.6 seconds
        const int maxDelay = (int)std::ceil(sr * 2.6) + juce::jmax(1, samplesPerBlock) + 8;
        delayL.allocate(maxDelay);
        delayR.allocate(maxDelay);

        // Smoothers
        timeSmooth.reset(sr, 0.04);
        feedbackSmooth.reset(sr, 0.04);
        spreadSmooth.reset(sr, 0.05);
        mixSmooth.reset(sr, 0.04);

        timeSmooth.setCurrentAndTargetValue(timeParam != nullptr ? *timeParam : 420.0f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.42f);
        spreadSmooth.setCurrentAndTargetValue(spreadParam != nullptr ? *spreadParam : 0.42f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.3f);

        // Tone LP in feedback path (2-pole biquad)
        for (auto& lp : toneLPF) lp.reset();
        // Feedback rumble HP (80 Hz)
        for (auto& hp : fbHighPass) hp.reset();
        // DC blocker
        for (auto& hp : dcBlock) hp.reset();
        updateTone();

        for (auto& hp : fbHighPass)
            hp.setHighPass(40.0f, 0.707f, sr);
        for (auto& hp : dcBlock)
            hp.setHighPass(18.0f, 0.707f, sr);

        // Feedback diffusion all-pass filters (tape-head smearing)
        // Asymmetric L/R delays for stereo decorrelation
        const double ratio = sr / 48000.0;
        for (auto& ap : diffusionL) { ap.allocate(64); ap.coeff = 0.45f; }
        for (auto& ap : diffusionR) { ap.allocate(64); ap.coeff = 0.45f; }
        diffusionL[0].setDelay(juce::jmax(1, (int)(13.0 * ratio)));
        diffusionL[1].setDelay(juce::jmax(1, (int)(23.0 * ratio)));
        diffusionR[0].setDelay(juce::jmax(1, (int)(17.0 * ratio)));
        diffusionR[1].setDelay(juce::jmax(1, (int)(29.0 * ratio)));

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        delayL.clear();
        delayR.clear();
        lfoPhase = 0.0f;
        lfoPhase2 = 0.0f;

        timeSmooth.setCurrentAndTargetValue(timeParam != nullptr ? *timeParam : 420.0f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.42f);
        spreadSmooth.setCurrentAndTargetValue(spreadParam != nullptr ? *spreadParam : 0.42f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.3f);

        for (auto& lp : toneLPF) lp.reset();
        for (auto& hp : fbHighPass) hp.reset();
        for (auto& hp : dcBlock) hp.reset();
        for (auto& ap : diffusionL) ap.clear();
        for (auto& ap : diffusionR) ap.clear();

        lastWetL = lastWetR = 0.0f;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        timeSmooth.setTargetValue(timeParam != nullptr ? *timeParam : 420.0f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.42f);
        spreadSmooth.setTargetValue(spreadParam != nullptr ? *spreadParam : 0.42f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.3f);
        updateTone();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        // Dual-LFO analog drift — richer than single-rate wobble
        constexpr float kModRate1 = 0.37f;      // Hz — primary slow drift
        constexpr float kModRate2 = 0.71f;      // Hz — secondary shimmer
        constexpr float kModDepthMs1 = 0.20f;   // ms — primary depth
        constexpr float kModDepthMs2 = 0.12f;   // ms — secondary depth

        const float maxDelaySamples = (float)(delayL.size - 4);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float timeMs   = timeSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float spread   = spreadSmooth.getNextValue();
            const float mix      = mixSmooth.getNextValue();

            // Base delay in samples
            const float baseDelay = juce::jlimit(1.0f, maxDelaySamples,
                timeMs * (float)sr * 0.001f);

            // Dual-LFO analog drift modulation
            const float modDepth1 = kModDepthMs1 * (float)sr * 0.001f;
            const float modDepth2 = kModDepthMs2 * (float)sr * 0.001f;
            const float lfo1 = std::sin(twoPi * lfoPhase);
            const float lfo2 = std::sin(twoPi * lfoPhase2);
            const float modL = lfo1 * modDepth1 + lfo2 * modDepth2;
            const float modR = lfo1 * modDepth1 * 0.8f - lfo2 * modDepth2 * 0.6f;

            // Stereo time offset: subtle width (±3% based on spread)
            const float stereoOffset = baseDelay * 0.03f * spread;
            const float delayLSamples = juce::jlimit(1.0f, maxDelaySamples,
                baseDelay - stereoOffset + modL);
            const float delayRSamples = juce::jlimit(1.0f, maxDelaySamples,
                baseDelay + stereoOffset + modR);

            // Read delayed signal (cubic Hermite)
            float wetL = delayL.readCubic(delayLSamples);
            float wetR = delayR.readCubic(delayRSamples);

            // Input
            const float inL = buffer.getSample(0, sample);
            const float inR = numChannels > 1 ? buffer.getSample(1, sample) : inL;

            // Ping-pong crossfeed in feedback path
            const float fbStraight = 1.0f - spread;
            const float fbCross = spread;
            float fbL = wetL * fbStraight + wetR * fbCross;
            float fbR = wetR * fbStraight + wetL * fbCross;

            // Tone filter in feedback (each repeat gets darker)
            fbL = toneLPF[0].process(fbL);
            fbR = toneLPF[1].process(fbR);

            // High-pass to prevent sub-bass buildup
            fbL = fbHighPass[0].process(fbL);
            fbR = fbHighPass[1].process(fbR);

            // Soft saturation: unity small-signal gain, bounded output ±1
            // Each pass through tanh adds analog warmth (odd harmonics)
            fbL = Nova::DelayDSP::analogSaturate(fbL);
            fbR = Nova::DelayDSP::analogSaturate(fbR);

            // Apply feedback gain AFTER saturation — guarantees loop gain < 1
            // This is the critical fix: prevents helicopter self-oscillation
            fbL *= feedback;
            fbR *= feedback;

            // Feedback diffusion: subtle all-pass smearing for tape/analog character
            for (auto& ap : diffusionL) fbL = ap.process(fbL);
            for (auto& ap : diffusionR) fbR = ap.process(fbR);

            // Write to delay lines
            delayL.write(inL + fbL);
            delayR.write(inR + fbR);

            // DC block the wet output
            float outWetL = dcBlock[0].process(wetL);
            float outWetR = dcBlock[1].process(wetR);

            // Equal-power dry/wet mix
            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            buffer.setSample(0, sample, inL * dryGain + outWetL * wetGain);
            if (numChannels > 1)
                buffer.setSample(1, sample, inR * dryGain + outWetR * wetGain);

            // Advance dual LFOs (incommensurate rates prevent beating)
            lfoPhase += kModRate1 / (float)sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            lfoPhase2 += kModRate2 / (float)sr;
            if (lfoPhase2 >= 1.0f) lfoPhase2 -= 1.0f;
        }

        // Store last wet levels for editor (cheap, end of block)
        lastWetL = std::abs(delayL.readCubic(timeSmooth.getCurrentValue() * (float)sr * 0.001f));
        lastWetR = std::abs(delayR.readCubic(timeSmooth.getCurrentValue() * (float)sr * 0.001f));

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* timeParam     = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* toneParam     = nullptr;
    juce::AudioParameterFloat* spreadParam   = nullptr;
    juce::AudioParameterFloat* mixParam      = nullptr;
    float lfoPhase = 0.0f;
    float lfoPhase2 = 0.0f;
    float lastWetL = 0.0f;
    float lastWetR = 0.0f;

private:
    void updateTone()
    {
        const float cutoff = toneParam != nullptr ? *toneParam : 4800.0f;
        if (std::abs(cachedToneCutoff - cutoff) < 0.5f) return;
        cachedToneCutoff = cutoff;

        for (auto& lp : toneLPF)
            lp.setLowPass(cutoff, 0.707f, sr);
    }

    double sr = 44100.0;

    Nova::DelayDSP::DelayLine delayL, delayR;

    // Smoothers
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> timeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> spreadSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    // Feedback path filters (per channel)
    std::array<Nova::DelayDSP::Biquad, 2> toneLPF;
    std::array<Nova::DelayDSP::Biquad, 2> fbHighPass;
    std::array<Nova::DelayDSP::Biquad, 2> dcBlock;

    // Feedback diffusion: 2 all-pass stages per channel (asymmetric for stereo width)
    std::array<Nova::DelayDSP::AllPassFilter, 2> diffusionL;
    std::array<Nova::DelayDSP::AllPassFilter, 2> diffusionR;

    float cachedToneCutoff = -1.0f;
    bool isPrepared = false;
};

#include "DelayEditor.h"
