#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

// ============================================================================
//  Noise Gate — Hysteresis, lookahead, sidechain HP, linked stereo detection
//  Reference: ISP Decimator precision, Zuul quick response
// ============================================================================
namespace Nova { namespace GateDSP {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    void setHighPass(float freq, float q, double sr)
    {
        float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float)(sr * 0.45), freq) / (float)sr;
        float s0 = std::sin(w0), c0 = std::cos(w0);
        float alpha = s0 / (2.0f * q);
        float a0inv = 1.0f / (1.0f + alpha);
        b0 = (1.0f + c0) * 0.5f * a0inv;
        b1 = -(1.0f + c0) * a0inv;
        b2 = b0;
        a1 = -2.0f * c0 * a0inv;
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

// Simple delay line for lookahead
struct LookaheadDelay
{
    std::vector<float> buf;
    int writePos = 0;
    int size = 1;

    void allocate(int maxSamples)
    {
        size = juce::jmax(1, maxSamples);
        buf.assign((size_t)size, 0.0f);
        writePos = 0;
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos = 0;
    }

    float process(float input, int delaySamples) noexcept
    {
        int d = juce::jlimit(0, size - 1, delaySamples);
        int readPos = writePos - d;
        if (readPos < 0) readPos += size;
        float out = buf[(size_t)readPos];
        buf[(size_t)writePos] = input;
        if (++writePos >= size) writePos = 0;
        return out;
    }
};

}} // namespace Nova::GateDSP


class NoiseGatePedal final : public ProcessorBase
{
public:
    NoiseGatePedal()
    {
        addParameter(thresholdParam = new juce::AudioParameterFloat("gateThreshold", "Threshold", -80.0f,  0.0f,  -45.0f));
        addParameter(attackParam    = new juce::AudioParameterFloat("gateAttack",    "Attack",      0.1f, 20.0f,    0.5f));
        addParameter(holdParam      = new juce::AudioParameterFloat("gateHold",      "Hold",        5.0f, 500.0f,  80.0f));
        addParameter(releaseParam   = new juce::AudioParameterFloat("gateRelease",   "Release",     5.0f, 500.0f,  85.0f));
        addParameter(rangeParam     = new juce::AudioParameterFloat("gateRange",     "Range",     -96.0f,  0.0f,  -96.0f));
    }

    const juce::String getName() const override { return "Noise Gate"; }
    double getTailLengthSeconds() const override { return 0.01; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        threshSmooth.reset(sr, 0.05);
        attackSmooth.reset(sr, 0.05);
        holdSmooth.reset(sr, 0.05);
        releaseSmooth.reset(sr, 0.05);
        rangeSmooth.reset(sr, 0.05);

        threshSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? *thresholdParam : -45.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 0.5f);
        holdSmooth.setCurrentAndTargetValue(holdParam != nullptr ? *holdParam : 80.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? *releaseParam : 85.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : -96.0f);

        // Sidechain HP: remove sub-bass rumble from detection
        for (auto& f : scHP)
            f.setHighPass(150.0f, 0.707f, sr);

        // Lookahead: ~2ms
        lookaheadSamples = juce::jmax(1, (int)(sr * 0.002));
        for (auto& d : lookahead)
            d.allocate(lookaheadSamples + 4);

        setProcessingLatency(lookaheadSamples);
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        envelope = 0.0f;
        gateGain = 0.0f;
        holdCounter = 0;
        gateOpen = false;

        for (auto& f : scHP) f.reset();
        for (auto& d : lookahead) d.clear();

        threshSmooth.setCurrentAndTargetValue(thresholdParam != nullptr ? *thresholdParam : -45.0f);
        attackSmooth.setCurrentAndTargetValue(attackParam != nullptr ? *attackParam : 0.5f);
        holdSmooth.setCurrentAndTargetValue(holdParam != nullptr ? *holdParam : 80.0f);
        releaseSmooth.setCurrentAndTargetValue(releaseParam != nullptr ? *releaseParam : 85.0f);
        rangeSmooth.setCurrentAndTargetValue(rangeParam != nullptr ? *rangeParam : -96.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        threshSmooth.setTargetValue(thresholdParam != nullptr ? *thresholdParam : -45.0f);
        attackSmooth.setTargetValue(attackParam != nullptr ? *attackParam : 0.5f);
        holdSmooth.setTargetValue(holdParam != nullptr ? *holdParam : 80.0f);
        releaseSmooth.setTargetValue(releaseParam != nullptr ? *releaseParam : 85.0f);
        rangeSmooth.setTargetValue(rangeParam != nullptr ? *rangeParam : -96.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();

        for (int s = 0; s < numSamples; ++s)
        {
            const float threshDb  = threshSmooth.getNextValue();
            const float attackMs  = attackSmooth.getNextValue();
            const float holdMs    = holdSmooth.getNextValue();
            const float releaseMs = releaseSmooth.getNextValue();
            const float rangeDb   = rangeSmooth.getNextValue();

            const float threshLin = juce::Decibels::decibelsToGain(threshDb, -96.0f);
            const float rangeLin  = juce::Decibels::decibelsToGain(rangeDb, -96.0f);

            // Hysteresis: close threshold is 6dB below open threshold
            const float closeLin  = threshLin * 0.5f;

            const float attackCoeff  = coeffForMs(attackMs);
            const float releaseCoeff = coeffForMs(releaseMs);
            const int   holdSamps    = juce::roundToInt(holdMs * 0.001f * (float)sr);

            // ---- Sidechain detection (HP filtered, linked stereo peak) ----
            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float raw = buffer.getSample(ch, s);
                float filtered = scHP[(size_t)ch].process(raw);
                detector = juce::jmax(detector, std::abs(filtered));
            }

            // RMS-ish envelope (fast attack on peaks, slower release)
            if (detector > envelope)
                envelope = detector + attackCoeff * (envelope - detector);
            else
                envelope = detector + releaseCoeff * (envelope - detector);

            // ---- Gate state machine with hysteresis ----
            float targetGain;
            if (!gateOpen)
            {
                // Gate is closed — need to exceed open threshold to open
                if (envelope >= threshLin)
                {
                    gateOpen = true;
                    holdCounter = holdSamps;
                    targetGain = 1.0f;
                }
                else
                {
                    targetGain = rangeLin;
                }
            }
            else
            {
                // Gate is open
                if (envelope >= closeLin)
                {
                    // Still above close threshold — stay open, reset hold
                    if (envelope >= threshLin)
                        holdCounter = holdSamps;
                    targetGain = 1.0f;
                }
                else if (holdCounter > 0)
                {
                    // Below close threshold but still in hold phase
                    --holdCounter;
                    targetGain = 1.0f;
                }
                else
                {
                    // Below close threshold, hold expired — close gate
                    gateOpen = false;
                    targetGain = rangeLin;
                }
            }

            // Smooth gain transitions
            const float smoothCoeff = (targetGain > gateGain)
                ? (1.0f - attackCoeff)
                : (1.0f - releaseCoeff);
            gateGain += smoothCoeff * (targetGain - gateGain);

            // Store for editor metering
            currentGateGainAtomic = gateGain;
            currentEnvelopeDb = 20.0f * std::log10(juce::jmax(envelope, 1.0e-8f));

            // ---- Apply gain with lookahead ----
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float dry = buffer.getSample(ch, s);
                float delayed = lookahead[(size_t)ch].process(dry, lookaheadSamples);
                buffer.setSample(ch, s, delayed * gateGain);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* thresholdParam = nullptr;
    juce::AudioParameterFloat* attackParam    = nullptr;
    juce::AudioParameterFloat* holdParam      = nullptr;
    juce::AudioParameterFloat* releaseParam   = nullptr;
    juce::AudioParameterFloat* rangeParam     = nullptr;

    float currentGateGainAtomic = 0.0f;  // For editor metering
    float currentEnvelopeDb     = -96.0f;

private:
    float coeffForMs(float ms) const noexcept
    {
        float sec = juce::jmax(0.0001f, ms * 0.001f);
        return std::exp(-1.0f / (sec * (float)juce::jmax(1.0, sr)));
    }

    double sr = 44100.0;

    std::array<Nova::GateDSP::Biquad, 2> scHP;           // Sidechain high-pass
    std::array<Nova::GateDSP::LookaheadDelay, 2> lookahead;
    int lookaheadSamples = 88;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> threshSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> holdSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> releaseSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;

    float envelope = 0.0f;
    float gateGain = 0.0f;
    int holdCounter = 0;
    bool gateOpen = false;
    bool isPrepared = false;
};

#include "GateEditor.h"
