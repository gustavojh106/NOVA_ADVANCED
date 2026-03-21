#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Stereo Flanger — cubic Hermite delay, resonant feedback, through-zero feel
//  Reference: Electric Mistress depth, MXR Flanger jet
// ============================================================================
namespace Nova { namespace FlangerDSP {

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

}} // namespace Nova::FlangerDSP


class FlangerPedal final : public ProcessorBase
{
public:
    FlangerPedal()
    {
        addParameter(rateParam     = new juce::AudioParameterFloat("flangerRate",     "Rate",     0.05f, 5.0f, 0.35f));
        addParameter(depthParam    = new juce::AudioParameterFloat("flangerDepth",    "Depth",    0.0f, 1.0f, 0.64f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("flangerFeedback", "Feedback", -0.95f, 0.95f, 0.55f));
        addParameter(toneParam     = new juce::AudioParameterFloat("flangerTone",     "Tone",     1000.0f, 14000.0f, 8000.0f));
        addParameter(mixParam      = new juce::AudioParameterFloat("flangerMix",      "Mix",      0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "Flanger"; }
    double getTailLengthSeconds() const override { return 0.3; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        // Max delay: 20ms
        const int maxDelay = (int)(sr * 0.02) + 64;
        for (auto& dl : delayLines) dl.allocate(maxDelay);

        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.04);
        feedbackSmooth.reset(sr, 0.04);
        mixSmooth.reset(sr, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.35f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.64f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.55f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        for (auto& lp : toneLPF) lp.reset();
        for (auto& hp : dcBlock) hp.reset();
        updateTone();

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        for (auto& dl : delayLines) dl.clear();
        lfoPhase = 0.0f;
        feedbackState.fill(0.0f);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? *rateParam : 0.35f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? *depthParam : 0.64f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.55f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 0.5f);

        for (auto& lp : toneLPF) lp.reset();
        for (auto& hp : dcBlock) hp.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        rateSmooth.setTargetValue(rateParam != nullptr ? *rateParam : 0.35f);
        depthSmooth.setTargetValue(depthParam != nullptr ? *depthParam : 0.64f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? *feedbackParam : 0.55f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 0.5f);
        updateTone();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        constexpr float kBaseDelayMs = 1.0f;
        constexpr float kMaxExcursionMs = 7.0f;

        // Stereo LFO offset (120 degrees for wide flanger stereo)
        constexpr float kStereoOffset = 0.333f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate     = rateSmooth.getNextValue();
            const float depth    = depthSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float mix      = mixSmooth.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                // Per-channel LFO with stereo offset
                float chPhase = (ch == 0) ? lfoPhase
                    : std::fmod(lfoPhase + kStereoOffset, 1.0f);
                float lfo = 0.5f + 0.5f * std::sin(twoPi * chPhase);

                float delaySamples = juce::jlimit(1.0f, (float)(delayLines[0].size - 3),
                    (kBaseDelayMs + kMaxExcursionMs * depth * lfo) * (float)sr * 0.001f);

                const float dry = buffer.getSample(ch, sample);

                // Read delayed signal (cubic Hermite)
                float wet = delayLines[(size_t)ch].readCubic(delaySamples);

                // Tone filter + DC block on wet
                wet = dcBlock[(size_t)ch].process(toneLPF[(size_t)ch].process(wet));

                // Write input + feedback into delay line
                delayLines[(size_t)ch].write(
                    std::tanh(dry + feedbackState[(size_t)ch] * feedback));
                feedbackState[(size_t)ch] = wet;

                // Equal-power mix
                const float dryGain = std::cos(mix * halfPi);
                const float wetGain = std::sin(mix * halfPi);
                buffer.setSample(ch, sample, dry * dryGain + wet * wetGain);
            }

            lfoPhase += rate / (float)sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* rateParam     = nullptr;
    juce::AudioParameterFloat* depthParam    = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* toneParam     = nullptr;
    juce::AudioParameterFloat* mixParam      = nullptr;
    float lfoPhase = 0.0f;

private:
    void updateTone()
    {
        const float cutoff = toneParam != nullptr ? *toneParam : 8000.0f;
        if (std::abs(cachedTone - cutoff) < 0.5f) return;
        cachedTone = cutoff;

        for (auto& lp : toneLPF)
            lp.setLowPass(cutoff, 0.707f, sr);
        for (auto& hp : dcBlock)
            hp.setHighPass(20.0f, 0.707f, sr);
    }

    double sr = 44100.0;

    std::array<Nova::FlangerDSP::DelayLine, 2> delayLines;
    std::array<Nova::FlangerDSP::Biquad, 2> toneLPF;
    std::array<Nova::FlangerDSP::Biquad, 2> dcBlock;
    std::array<float, 2> feedbackState {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    float cachedTone = -1.0f;
    bool isPrepared = false;
};

#include "FlangerEditor.h"
