#pragma once

#include <JuceHeader.h>
#include "../../PedalSignalTelemetry.h"
#include <array>
#include <cmath>

class OutputChainProcessor final : public juce::AudioProcessor
{
public:
    OutputChainProcessor();
    ~OutputChainProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // volDb: master output gain in dB
    // limitDb: limiter threshold in dB (0.0 = off)
    void setParams(float volDb, float limitDb);
    void setTelemetryTag(const juce::String& newTag);

    // Boilerplate JUCE
    const juce::String getName() const override { return "OutputChain"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    struct PeakLimiter
    {
        void prepare(double newSampleRate) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            updateReleaseCoefficient();
            reset();
        }

        void reset() noexcept
        {
            currentGain = 1.0f;
        }

        void setThresholdDb(float newThresholdDb) noexcept
        {
            thresholdDb = newThresholdDb;
            thresholdLinear = juce::Decibels::decibelsToGain(thresholdDb, 0.0f);
        }

        void setRelease(float newReleaseMs) noexcept
        {
            releaseMs = juce::jmax(1.0f, newReleaseMs);
            updateReleaseCoefficient();
        }

        template <typename BufferType>
        void process(BufferType& buffer) noexcept
        {
            const int numChannels = buffer.getNumChannels();
            const int numSamples = buffer.getNumSamples();

            if (numChannels <= 0 || numSamples <= 0)
                return;

            for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    peak = juce::jmax(peak, std::abs(buffer.getSample(ch, sampleIndex)));

                const float desiredGain = peak > thresholdLinear && thresholdLinear > 0.0f
                    ? (thresholdLinear / peak)
                    : 1.0f;

                if (desiredGain < currentGain)
                    currentGain = desiredGain;
                else
                    currentGain = desiredGain + releaseCoefficient * (currentGain - desiredGain);

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample(ch, sampleIndex, buffer.getSample(ch, sampleIndex) * currentGain);
            }
        }

        double sampleRate = 44100.0;
        float thresholdDb = 0.0f;
        float thresholdLinear = 1.0f;
        float releaseMs = 100.0f;
        float releaseCoefficient = 0.0f;
        float currentGain = 1.0f;

    private:
        void updateReleaseCoefficient() noexcept
        {
            const auto releaseSeconds = juce::jmax(0.001, (double) releaseMs * 0.001);
            releaseCoefficient = (float) std::exp(-1.0 / (sampleRate * releaseSeconds));
        }
    };

    struct DebugTelemetry
    {
        NovaDiagnostics::SignalStageWindowMetrics postDcStage;
        NovaDiagnostics::SignalStageWindowMetrics postGainStage;
        NovaDiagnostics::SignalStageWindowMetrics postLimiterStage;
        float limiterDeltaPeak = 0.0f;
        float softCeilingDeltaPeak = 0.0f;
        int limiterTouchedSamples = 0;
        int softCeilingTouchedSamples = 0;
        int limiterActiveBlocks = 0;
        float limiterThresholdMin = 1.0e9f;
        float limiterThresholdMax = -1.0e9f;
        float outputVolDbMin = 1.0e9f;
        float outputVolDbMax = -1.0e9f;

        void resetWindow() noexcept
        {
            postDcStage.reset();
            postGainStage.reset();
            postLimiterStage.reset();
            limiterDeltaPeak = 0.0f;
            softCeilingDeltaPeak = 0.0f;
            limiterTouchedSamples = 0;
            softCeilingTouchedSamples = 0;
            limiterActiveBlocks = 0;
            limiterThresholdMin = 1.0e9f;
            limiterThresholdMax = -1.0e9f;
            outputVolDbMin = 1.0e9f;
            outputVolDbMax = -1.0e9f;
        }

        void captureControls(float outputDb, float limiterThreshold, bool limiterActive) noexcept
        {
            outputVolDbMin = juce::jmin(outputVolDbMin, outputDb);
            outputVolDbMax = juce::jmax(outputVolDbMax, outputDb);
            limiterThresholdMin = juce::jmin(limiterThresholdMin, limiterThreshold);
            limiterThresholdMax = juce::jmax(limiterThresholdMax, limiterThreshold);
            if (limiterActive)
                ++limiterActiveBlocks;
        }
    };

    struct DCBlocker
    {
        void prepare(double newSampleRate) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            const auto cutoff = 18.0;
            pole = (float)std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
            reset();
        }

        void reset() noexcept
        {
            x1 = 0.0f;
            y1 = 0.0f;
        }

        float process(float x) noexcept
        {
            const float y = x - x1 + (pole * y1);
            x1 = x;
            y1 = y;
            return y;
        }

        double sampleRate = 44100.0;
        float pole = 0.995f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    static float applySoftCeiling(float x) noexcept;

    juce::dsp::Gain<float> gain;
    PeakLimiter limiter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> limiterSmooth;
    std::array<DCBlocker, 2> dcBlockers;
    juce::AudioBuffer<float> scratchBuffer;
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "output-chain" };
    DebugTelemetry debugTelemetry;
    juce::String telemetryTag { "output-chain" };

    float outputVolDb = 0.0f;
    float limiterThresholdTarget = 0.0f;
    bool hardSyncParams = true;
};
