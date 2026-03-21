#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Stereo Chorus — dual-voice modulated delay with BBD warmth
//  Reference: Boss CE-2 warmth, TC Corona stereo spread
// ============================================================================
namespace Nova { namespace ChorusDSP {

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

// ---- Soft clipper for BBD warmth ----
inline float bbdSaturate(float x) noexcept
{
    // Gentle tanh-like saturation mimicking BBD compression
    const float x2 = x * x;
    return x * (1.0f - x2 * 0.05f);  // Very subtle — preserves dynamics
}

}} // namespace Nova::ChorusDSP


// ============================================================================
//  ChorusPedal — Processor
// ============================================================================
class ChorusPedal final : public ProcessorBase
{
public:
    ChorusPedal()
    {
        addParameter(rateParam  = new juce::AudioParameterFloat("chorusRate",  "Rate",  0.05f, 5.0f, 0.85f));
        addParameter(depthParam = new juce::AudioParameterFloat("chorusDepth", "Depth", 0.0f, 1.0f, 0.56f));
        addParameter(widthParam = new juce::AudioParameterFloat("chorusWidth", "Width", 0.0f, 1.0f, 0.68f));
        addParameter(toneParam  = new juce::AudioParameterFloat("chorusTone",  "Tone",  0.0f, 1.0f, 0.55f));
        addParameter(mixParam   = new juce::AudioParameterFloat("chorusMix",   "Mix",   0.0f, 1.0f, 0.34f));
    }

    const juce::String getName() const override { return "Chorus"; }
    double getTailLengthSeconds() const override { return 0.35; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        // Delay lines: 80ms max per voice
        const int maxDelay = (int)(sr * 0.08) + 64;
        for (auto& dl : delayL) dl.allocate(maxDelay);
        for (auto& dl : delayR) dl.allocate(maxDelay);

        // Feedback delay
        fbDelayL.allocate(maxDelay);
        fbDelayR.allocate(maxDelay);

        // Smoothers
        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.04);
        widthSmooth.reset(sr, 0.04);
        mixSmooth.reset(sr, 0.04);
        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 0.85f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.56f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? widthParam->get() : 0.68f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 0.34f);

        // Tone: 2-pole LP on wet
        for (auto& lp : toneLPF) lp.reset();
        // DC blocker
        for (auto& hp : dcBlock) hp.reset();
        updateTone();

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        for (auto& dl : delayL) dl.clear();
        for (auto& dl : delayR) dl.clear();
        fbDelayL.clear();
        fbDelayR.clear();
        lfoPhase = 0.0f;

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 0.85f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.56f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? widthParam->get() : 0.68f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 0.34f);

        for (auto& lp : toneLPF) lp.reset();
        for (auto& hp : dcBlock) hp.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? rateParam->get() : 0.85f);
        depthSmooth.setTargetValue(depthParam != nullptr ? depthParam->get() : 0.56f);
        widthSmooth.setTargetValue(widthParam != nullptr ? widthParam->get() : 0.68f);
        mixSmooth.setTargetValue(mixParam != nullptr ? mixParam->get() : 0.34f);
        updateTone();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        // Base delay and excursion in samples
        const float baseDelay = (float)(sr * 0.007);  // 7ms center delay (CE-2 style)

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate  = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float width = widthSmooth.getNextValue();
            const float mix   = mixSmooth.getNextValue();

            const float excursion = depth * (float)(sr * 0.004);  // ±4ms max swing

            // ---- Dual LFO voices (voice 2 at golden ratio offset) ----
            const float lfo1 = std::sin(twoPi * lfoPhase);
            const float lfo2 = std::sin(twoPi * std::fmod(lfoPhase * 1.618f + 0.37f, 1.0f));

            // Stereo phase offset from width parameter
            const float stereoOffset = 0.25f + width * 0.20f;  // 90°-162° spread
            const float lfo1R = std::sin(twoPi * std::fmod(lfoPhase + stereoOffset, 1.0f));
            const float lfo2R = std::sin(twoPi * std::fmod(lfoPhase * 1.618f + 0.37f + stereoOffset * 0.7f, 1.0f));

            // Delay times: voice 1 (main) + voice 2 (detune) blended
            const float delayL1 = baseDelay + excursion * lfo1;
            const float delayL2 = baseDelay * 1.12f + excursion * 0.73f * lfo2;
            const float delayR1 = baseDelay + excursion * lfo1R;
            const float delayR2 = baseDelay * 1.12f + excursion * 0.73f * lfo2R;

            // Read input
            const float inL = buffer.getSample(0, sample);
            const float inR = numChannels > 1 ? buffer.getSample(1, sample) : inL;

            // Write into delay lines (with subtle feedback for resonance)
            constexpr float fbAmount = 0.12f;
            float fbL = fbDelayL.readCubic(baseDelay) * fbAmount;
            float fbR = fbDelayR.readCubic(baseDelay) * fbAmount;

            delayL[0].write(inL + fbR * width * 0.3f);  // Cross-feedback for stereo
            delayL[1].write(inL + fbL * 0.15f);
            delayR[0].write(inR + fbL * width * 0.3f);
            delayR[1].write(inR + fbR * 0.15f);

            // Read modulated taps (cubic interpolation)
            float voiceL = delayL[0].readCubic(delayL1) * 0.6f
                         + delayL[1].readCubic(delayL2) * 0.4f;
            float voiceR = delayR[0].readCubic(delayR1) * 0.6f
                         + delayR[1].readCubic(delayR2) * 0.4f;

            // BBD warmth (subtle saturation)
            voiceL = Nova::ChorusDSP::bbdSaturate(voiceL);
            voiceR = Nova::ChorusDSP::bbdSaturate(voiceR);

            // Stereo width crossfeed
            const float xfeed = width * 0.18f;
            float wetL = voiceL * (1.0f - xfeed) + voiceR * xfeed;
            float wetR = voiceR * (1.0f - xfeed) + voiceL * xfeed;

            // Tone filter + DC block
            wetL = dcBlock[0].process(toneLPF[0].process(wetL));
            wetR = dcBlock[1].process(toneLPF[1].process(wetR));

            // Write to feedback delay
            fbDelayL.write(wetL);
            fbDelayR.write(wetR);

            // Equal-power mix
            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            buffer.setSample(0, sample, inL * dryGain + wetL * wetGain);
            if (numChannels > 1)
                buffer.setSample(1, sample, inR * dryGain + wetR * wetGain);

            // Advance LFO
            lfoPhase += rate / (float)sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* rateParam  = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* widthParam = nullptr;
    juce::AudioParameterFloat* toneParam  = nullptr;
    juce::AudioParameterFloat* mixParam   = nullptr;
    float lfoPhase = 0.0f;  // Exposed for editor visualization

private:
    void updateTone()
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 0.55f;
        if (std::abs(cachedTone - tone) < 0.005f) return;
        cachedTone = tone;

        // Tone 0→1 maps to 1500→12000 Hz LP
        float cutoff = 1500.0f + tone * 10500.0f;
        for (auto& lp : toneLPF)
            lp.setLowPass(cutoff, 0.707f, sr);
        for (auto& hp : dcBlock)
            hp.setHighPass(20.0f, 0.707f, sr);
    }

    double sr = 44100.0;

    // Dual delay lines per channel (2 voices)
    std::array<Nova::ChorusDSP::DelayLine, 2> delayL;
    std::array<Nova::ChorusDSP::DelayLine, 2> delayR;
    Nova::ChorusDSP::DelayLine fbDelayL, fbDelayR;

    // Smoothers
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    // Tone + DC block (per channel)
    std::array<Nova::ChorusDSP::Biquad, 2> toneLPF;
    std::array<Nova::ChorusDSP::Biquad, 2> dcBlock;

    float cachedTone = -1.0f;
    bool isPrepared = false;
};

#include "ChorusEditor.h"
