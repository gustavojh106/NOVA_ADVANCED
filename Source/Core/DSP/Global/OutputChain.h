#pragma once

#include <JuceHeader.h>
#include "../../PedalSignalTelemetry.h"
#include "../SignalGuard.h"

#include <array>
#include <atomic>
#include <cmath>

/*
    NOVA OutputChainProcessor - professional safety/master output stage

    Goals:
    - Stable latency: the lookahead delay is always active and latency is reported once.
    - No allocations in processBlock().
    - Thread-safe parameter handoff through atomics.
    - Sample-accurate smoothing for output gain and limiter threshold.
    - Cleaner DC blocking before the gain/limiter stage.
    - Less limiter "helicopter/pumping" through hold + program-dependent release.
    - Final soft ceiling as a last-resort safety stage, not as the main limiter.
*/

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
    // limitDb: limiter threshold in dB. 0.0 = transparent safety mode.
    void setParams(float volDb, float limitDb);
    void setTelemetryTag(const juce::String& newTag);

    // JUCE boilerplate
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

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet();
        const auto out = layouts.getMainOutputChannelSet();

        if (in != out)
            return false;

        return in == juce::AudioChannelSet::mono()
            || in == juce::AudioChannelSet::stereo();
    }

private:
    static constexpr int kMaxOutputChannels = 8;

    struct DCBlocker
    {
        void prepare(double newSampleRate) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);

            // Two gentle cascaded one-pole DC blockers.
            // Strong enough to keep limiters honest, but low enough to avoid thinning guitar tone.
            setCutoffHz(20.0f);
            reset();
        }

        void reset() noexcept
        {
            x1a = 0.0f;
            y1a = 0.0f;
            x1b = 0.0f;
            y1b = 0.0f;
        }

        float process(float x) noexcept
        {
            const float ya = x - x1a + (pole * y1a);
            x1a = x;
            y1a = ya;

            const float yb = ya - x1b + (pole * y1b);
            x1b = ya;
            y1b = yb;

            return yb;
        }

        void setCutoffHz(float cutoffHz) noexcept
        {
            const auto cutoff = juce::jlimit(5.0f, 40.0f, cutoffHz);
            pole = (float)std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
        }

        double sampleRate = 44100.0;
        float pole = 0.997f;

        float x1a = 0.0f;
        float y1a = 0.0f;
        float x1b = 0.0f;
        float y1b = 0.0f;
    };

    struct PeakLimiter
    {
        struct Result
        {
            bool active = false;
            int touchedSamples = 0;
            float deltaPeak = 0.0f;
            float minGain = 1.0f;
            float maxReductionDb = 0.0f;
            float thresholdLinearMin = 1.0f;
            float thresholdLinearMax = 1.0f;
        };

        void prepare(double newSampleRate, int maxChannels)
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            preparedChannels = juce::jlimit(1, kMaxOutputChannels, maxChannels);

            lookaheadSamples = juce::jmax(1, juce::roundToInt(sampleRate * lookaheadMs * 0.001));
            holdSamples = juce::jmax(1, juce::roundToInt(sampleRate * holdMs * 0.001));

            delayBuffer.setSize(preparedChannels,
                lookaheadSamples + 1,
                false,
                false,
                true);

            reset();
        }

        void reset() noexcept
        {
            currentGain = 1.0f;
            writeIndex = 0;
            holdCounter = 0;
            previousInputSamples.fill(0.0f);

            if (delayBuffer.getNumSamples() > 0)
                delayBuffer.clear();
        }

        int getLookaheadSamples() const noexcept
        {
            return lookaheadSamples;
        }

        template <typename BufferType>
        Result process(BufferType& buffer,
            juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& thresholdLinearSmoother) noexcept
        {
            Result result;

            const int numChannels = juce::jmin(buffer.getNumChannels(),
                delayBuffer.getNumChannels(),
                kMaxOutputChannels);

            const int numSamples = buffer.getNumSamples();

            if (numChannels <= 0 || numSamples <= 0 || delayBuffer.getNumSamples() <= lookaheadSamples)
            {
                return processWithoutLookahead(buffer, thresholdLinearSmoother);
            }

            const int delaySize = delayBuffer.getNumSamples();

            std::array<float*, kMaxOutputChannels> audio{};
            std::array<float*, kMaxOutputChannels> delay{};

            for (int ch = 0; ch < numChannels; ++ch)
            {
                audio[(size_t)ch] = buffer.getWritePointer(ch);
                delay[(size_t)ch] = delayBuffer.getWritePointer(ch);
            }

            for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            {
                const float thresholdLinear = juce::jlimit(0.000001f,
                    1.0f,
                    thresholdLinearSmoother.getNextValue());

                result.thresholdLinearMin = juce::jmin(result.thresholdLinearMin, thresholdLinear);
                result.thresholdLinearMax = juce::jmax(result.thresholdLinearMax, thresholdLinear);

                const bool active = thresholdLinear < 0.9885531f; // about -0.1 dB
                result.active = result.active || active;

                const float peak = estimateLinkedTruePeak(buffer, sampleIndex, numChannels);
                updateGainForPeak(peak, thresholdLinear, active);

                const int readIndex = (writeIndex + delaySize - lookaheadSamples) % delaySize;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    const float delayed = delay[(size_t)ch][readIndex];
                    delay[(size_t)ch][writeIndex] = audio[(size_t)ch][sampleIndex];

                    const float limited = delayed * currentGain;
                    audio[(size_t)ch][sampleIndex] = limited;

                    const float delta = std::abs(delayed - limited);
                    result.deltaPeak = juce::jmax(result.deltaPeak, delta);

                    if (delta >= 1.0e-5f)
                        ++result.touchedSamples;
                }

                result.minGain = juce::jmin(result.minGain, currentGain);

                if (++writeIndex >= delaySize)
                    writeIndex = 0;
            }

            if (result.minGain < 0.999999f)
                result.maxReductionDb = -juce::Decibels::gainToDecibels(result.minGain, -120.0f);

            return result;
        }

        double sampleRate = 44100.0;
        float lookaheadMs = 2.0f;
        float holdMs = 6.0f;
        float releaseFastMs = 70.0f;
        float releaseSlowMs = 650.0f;
        float currentGain = 1.0f;
        int lookaheadSamples = 88;
        int holdSamples = 265;
        int holdCounter = 0;
        int preparedChannels = 2;
        int writeIndex = 0;
        juce::AudioBuffer<float> delayBuffer;
        std::array<float, kMaxOutputChannels> previousInputSamples{};

    private:
        template <typename BufferType>
        float estimateLinkedTruePeak(BufferType& buffer, int sampleIndex, int numChannels) noexcept
        {
            float peak = 0.0f;
            const int numSamples = buffer.getNumSamples();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* data = buffer.getReadPointer(ch);

                const float x0 = previousInputSamples[(size_t)ch];
                const float x1 = data[sampleIndex];
                const float x2 = sampleIndex + 1 < numSamples ? data[sampleIndex + 1] : x1;
                const float x3 = sampleIndex + 2 < numSamples ? data[sampleIndex + 2] : x2;

                peak = juce::jmax(peak, std::abs(x1));
                peak = juce::jmax(peak, std::abs(catmullRom(x0, x1, x2, x3, 0.25f)));
                peak = juce::jmax(peak, std::abs(catmullRom(x0, x1, x2, x3, 0.50f)));
                peak = juce::jmax(peak, std::abs(catmullRom(x0, x1, x2, x3, 0.75f)));

                previousInputSamples[(size_t)ch] = x1;
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

        void updateGainForPeak(float peak, float thresholdLinear, bool active) noexcept
        {
            const float desiredGain = (active && thresholdLinear > 0.0f && peak > thresholdLinear)
                ? juce::jlimit(0.000001f, 1.0f, thresholdLinear / peak)
                : 1.0f;

            if (desiredGain < currentGain)
            {
                // Lookahead gives us time, so gain can clamp immediately without overshoot.
                currentGain = desiredGain;
                holdCounter = holdSamples;
                return;
            }

            if (holdCounter > 0)
            {
                --holdCounter;
                return;
            }

            const float releaseMs = getProgramDependentReleaseMs();
            const float coeff = (float)std::exp(-1.0 / (sampleRate * (releaseMs * 0.001f)));

            currentGain = desiredGain + coeff * (currentGain - desiredGain);

            if (currentGain > 0.999999f)
                currentGain = 1.0f;
        }

        float getProgramDependentReleaseMs() const noexcept
        {
            // Deeper gain reduction gets a slower release to avoid "helicopter" pumping.
            // Light limiting recovers faster and feels responsive.
            const float reduction = juce::jlimit(0.0f, 1.0f, 1.0f - currentGain);
            const float weight = std::sqrt(reduction);
            return releaseFastMs + ((releaseSlowMs - releaseFastMs) * weight);
        }

        template <typename BufferType>
        Result processWithoutLookahead(BufferType& buffer,
            juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& thresholdLinearSmoother) noexcept
        {
            Result result;

            const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxOutputChannels);
            const int numSamples = buffer.getNumSamples();

            for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            {
                const float thresholdLinear = juce::jlimit(0.000001f,
                    1.0f,
                    thresholdLinearSmoother.getNextValue());

                result.thresholdLinearMin = juce::jmin(result.thresholdLinearMin, thresholdLinear);
                result.thresholdLinearMax = juce::jmax(result.thresholdLinearMax, thresholdLinear);

                const bool active = thresholdLinear < 0.9885531f;
                result.active = result.active || active;

                const float peak = estimateLinkedTruePeak(buffer, sampleIndex, numChannels);
                updateGainForPeak(peak, thresholdLinear, active);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer(ch);
                    const float original = data[sampleIndex];
                    const float limited = original * currentGain;
                    data[sampleIndex] = limited;

                    const float delta = std::abs(original - limited);
                    result.deltaPeak = juce::jmax(result.deltaPeak, delta);

                    if (delta >= 1.0e-5f)
                        ++result.touchedSamples;
                }

                result.minGain = juce::jmin(result.minGain, currentGain);
            }

            if (result.minGain < 0.999999f)
                result.maxReductionDb = -juce::Decibels::gainToDecibels(result.minGain, -120.0f);

            return result;
        }
    };

    struct DebugTelemetry
    {
        NovaDiagnostics::SignalStageWindowMetrics postDcStage;
        NovaDiagnostics::SignalStageWindowMetrics postGainStage;
        NovaDiagnostics::SignalStageWindowMetrics postLimiterStage;

        float limiterDeltaPeak = 0.0f;
        float softCeilingDeltaPeak = 0.0f;
        float limiterMaxReductionDb = 0.0f;
        float limiterMinGain = 1.0f;

        int limiterTouchedSamples = 0;
        int softCeilingTouchedSamples = 0;
        int limiterActiveBlocks = 0;

        int guardInvalidSamples = 0;
        int guardClippedSamples = 0;
        int guardDenormalSamples = 0;

        float limiterThresholdDbMin = 1.0e9f;
        float limiterThresholdDbMax = -1.0e9f;
        float outputVolDbMin = 1.0e9f;
        float outputVolDbMax = -1.0e9f;

        void resetWindow() noexcept
        {
            postDcStage.reset();
            postGainStage.reset();
            postLimiterStage.reset();

            limiterDeltaPeak = 0.0f;
            softCeilingDeltaPeak = 0.0f;
            limiterMaxReductionDb = 0.0f;
            limiterMinGain = 1.0f;

            limiterTouchedSamples = 0;
            softCeilingTouchedSamples = 0;
            limiterActiveBlocks = 0;

            guardInvalidSamples = 0;
            guardClippedSamples = 0;
            guardDenormalSamples = 0;

            limiterThresholdDbMin = 1.0e9f;
            limiterThresholdDbMax = -1.0e9f;
            outputVolDbMin = 1.0e9f;
            outputVolDbMax = -1.0e9f;
        }

        void captureControls(float outputDb,
            float limiterThresholdDb,
            const PeakLimiter::Result& limiterResult) noexcept
        {
            outputVolDbMin = juce::jmin(outputVolDbMin, outputDb);
            outputVolDbMax = juce::jmax(outputVolDbMax, outputDb);

            limiterThresholdDbMin = juce::jmin(limiterThresholdDbMin, limiterThresholdDb);
            limiterThresholdDbMax = juce::jmax(limiterThresholdDbMax, limiterThresholdDb);

            if (limiterResult.active)
                ++limiterActiveBlocks;

            limiterDeltaPeak = juce::jmax(limiterDeltaPeak, limiterResult.deltaPeak);
            limiterTouchedSamples += limiterResult.touchedSamples;
            limiterMinGain = juce::jmin(limiterMinGain, limiterResult.minGain);
            limiterMaxReductionDb = juce::jmax(limiterMaxReductionDb, limiterResult.maxReductionDb);
        }
    };

    static float applySoftCeiling(float x) noexcept;
    static float sanitizeOutputVolumeDb(float value) noexcept;
    static float sanitizeLimiterDb(float value) noexcept;
    static float limiterDbToLinear(float limiterDb) noexcept;

    void applyParameterTargets(bool forceSync) noexcept;
    void applyMasterGain(juce::AudioBuffer<float>& buffer) noexcept;
    void applyDcBlockers(juce::AudioBuffer<float>& buffer) noexcept;
    void applyFinalSoftCeiling(juce::AudioBuffer<float>& buffer) noexcept;

    PeakLimiter limiter;
    std::array<DCBlocker, kMaxOutputChannels> dcBlockers{};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> limiterThresholdLinearSmooth;

    NovaDiagnostics::PedalSignalTelemetry signalTelemetry{ "output-chain" };
    DebugTelemetry debugTelemetry;
    juce::String telemetryTag{ "output-chain" };

    std::atomic<float> targetOutputVolDb{ 0.0f };
    std::atomic<float> targetLimiterDb{ 0.0f };

    float currentOutputVolDb = 0.0f;
    float currentLimiterDb = 0.0f;

    bool parametersPrepared = false;
    bool hardSyncParams = true;
};
