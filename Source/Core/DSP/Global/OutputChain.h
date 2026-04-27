#pragma once

#include <JuceHeader.h>
#include "../../PedalSignalTelemetry.h"
#include "../SignalGuard.h"
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
        void prepare(double newSampleRate, int maxChannels) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            preparedChannels = juce::jlimit(1, (int) previousInputSamples.size(), maxChannels);
            lookaheadSamples = juce::jmax(1, juce::roundToInt(sampleRate * lookaheadMs * 0.001));
            delayBuffer.setSize(preparedChannels, lookaheadSamples + 1, false, false, true);
            updateReleaseCoefficient();
            reset();
        }

        void reset() noexcept
        {
            currentGain = 1.0f;
            writeIndex = 0;
            delayBuffer.clear();
            previousInputSamples.fill(0.0f);
        }

        void setThresholdDb(float newThresholdDb) noexcept
        {
            thresholdDb = newThresholdDb;
            thresholdLinear = juce::Decibels::decibelsToGain(thresholdDb, -120.0f);
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

            if (delayBuffer.getNumChannels() < numChannels
                || delayBuffer.getNumSamples() <= lookaheadSamples)
            {
                processWithoutLookahead(buffer);
                return;
            }

            const int delaySize = delayBuffer.getNumSamples();
            const int linkedChannels = juce::jmin(numChannels, (int) previousInputSamples.size());

            for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            {
                const float peak = estimateLinkedTruePeak(buffer, sampleIndex, linkedChannels);

                updateGainForPeak(peak);

                const int readIndex = (writeIndex + delaySize - lookaheadSamples) % delaySize;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    delayBuffer.setSample(ch, writeIndex, buffer.getSample(ch, sampleIndex));
                    buffer.setSample(ch, sampleIndex, delayBuffer.getSample(ch, readIndex) * currentGain);
                }

                if (++writeIndex >= delaySize)
                    writeIndex = 0;
            }
        }

        int getLookaheadSamples() const noexcept
        {
            return lookaheadSamples;
        }

        double sampleRate = 44100.0;
        float thresholdDb = 0.0f;
        float thresholdLinear = 1.0f;
        float lookaheadMs = 1.5f;
        float releaseMs = 100.0f;
        float releaseCoefficient = 0.0f;
        float currentGain = 1.0f;
        int lookaheadSamples = 66;
        int preparedChannels = 2;
        int writeIndex = 0;
        juce::AudioBuffer<float> delayBuffer;
        std::array<float, 8> previousInputSamples {};

    private:
        template <typename BufferType>
        float estimateLinkedTruePeak(BufferType& buffer, int sampleIndex, int numChannels) noexcept
        {
            float peak = 0.0f;
            const int numSamples = buffer.getNumSamples();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float x0 = previousInputSamples[(size_t) ch];
                const float x1 = buffer.getSample(ch, sampleIndex);
                const float x2 = sampleIndex + 1 < numSamples ? buffer.getSample(ch, sampleIndex + 1) : x1;
                const float x3 = sampleIndex + 2 < numSamples ? buffer.getSample(ch, sampleIndex + 2) : x2;

                peak = juce::jmax(peak, std::abs(x1));
                peak = juce::jmax(peak, std::abs(catmullRom(x0, x1, x2, x3, 0.25f)));
                peak = juce::jmax(peak, std::abs(catmullRom(x0, x1, x2, x3, 0.50f)));
                peak = juce::jmax(peak, std::abs(catmullRom(x0, x1, x2, x3, 0.75f)));

                previousInputSamples[(size_t) ch] = x1;
            }

            return peak;
        }

        static float catmullRom(float x0, float x1, float x2, float x3, float t) noexcept
        {
            const float t2 = t * t;
            const float t3 = t2 * t;
            return 0.5f * ((2.0f * x1)
                + ((-x0 + x2) * t)
                + ((2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3) * t2)
                + ((-x0 + 3.0f * x1 - 3.0f * x2 + x3) * t3));
        }

        void updateGainForPeak(float peak) noexcept
        {
            const float desiredGain = peak > thresholdLinear && thresholdLinear > 0.0f
                ? (thresholdLinear / peak)
                : 1.0f;

            if (desiredGain < currentGain)
                currentGain = desiredGain;
            else
                currentGain = desiredGain + releaseCoefficient * (currentGain - desiredGain);
        }

        template <typename BufferType>
        void processWithoutLookahead(BufferType& buffer) noexcept
        {
            const int numChannels = buffer.getNumChannels();
            const int numSamples = buffer.getNumSamples();
            const int linkedChannels = juce::jmin(numChannels, (int) previousInputSamples.size());

            for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            {
                const float peak = estimateLinkedTruePeak(buffer, sampleIndex, linkedChannels);

                updateGainForPeak(peak);

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample(ch, sampleIndex, buffer.getSample(ch, sampleIndex) * currentGain);
            }
        }

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
        int guardInvalidSamples = 0;
        int guardClippedSamples = 0;
        int guardDenormalSamples = 0;
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
            guardInvalidSamples = 0;
            guardClippedSamples = 0;
            guardDenormalSamples = 0;
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
