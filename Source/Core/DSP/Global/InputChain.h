#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

#include "../../Constants.h"
#include "../SignalGuard.h"

/*
    NOVA InputChainProcessor - professional input boundary

    Goals:
    - No locks / no allocations in processBlock().
    - Thread-safe parameter handoff through atomics.
    - Sample-accurate smoothing for input gain, force-mono blend and gate threshold.
    - Stable instrument routing: avoids block-to-block L/R auto-detect chatter.
    - Cleaner input conditioning before pedals: scrub + subsonic/DC protection.
    - Musical noise gate: envelope follower + hysteresis + hold + smooth gain.
*/

class InputChainProcessor final : public juce::AudioProcessor
{
public:
    InputChainProcessor();
    ~InputChainProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    void setParams(float gainDb, float gateDb, bool forceMono);

    int getInvalidSampleCount() const noexcept { return invalidSampleCount.load(std::memory_order_relaxed); }
    int getClippedSampleCount() const noexcept { return clippedSampleCount.load(std::memory_order_relaxed); }
    int getDenormalSampleCount() const noexcept { return denormalSampleCount.load(std::memory_order_relaxed); }

    const juce::String getName() const override { return "InputChain"; }
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
    static constexpr int kMaxChannels = 8;

    struct OnePoleHighPass
    {
        void prepare(double newSampleRate, float cutoffHz) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            setCutoff(cutoffHz);
            reset();
        }

        void setCutoff(float cutoffHz) noexcept
        {
            const float cutoff = juce::jlimit(5.0f, 45.0f, cutoffHz);
            pole = (float)std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
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

    struct InputConditioner
    {
        void prepare(double sampleRate) noexcept
        {
            for (auto& hp : subsonic)
                hp.prepare(sampleRate, 24.0f);

            for (auto& dc : dcBlock)
                dc.prepare(sampleRate, 12.0f);
        }

        void reset() noexcept
        {
            for (auto& hp : subsonic)
                hp.reset();

            for (auto& dc : dcBlock)
                dc.reset();
        }

        void process(juce::AudioBuffer<float>& buffer) noexcept
        {
            const int channels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
            const int samples = buffer.getNumSamples();

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = buffer.getWritePointer(ch);
                auto& hp = subsonic[(size_t)ch];
                auto& dc = dcBlock[(size_t)ch];

                for (int i = 0; i < samples; ++i)
                    data[i] = dc.process(hp.process(data[i]));
            }
        }

        std::array<OnePoleHighPass, kMaxChannels> subsonic{};
        std::array<OnePoleHighPass, kMaxChannels> dcBlock{};
    };

    struct AutoRoutingDetector
    {
        enum class Mode
        {
            Stereo,
            LeftOnly,
            RightOnly
        };

        void reset() noexcept
        {
            currentMode = Mode::Stereo;
            pendingMode = Mode::Stereo;
            pendingBlocks = 0;
        }

        void process(juce::AudioBuffer<float>& buffer) noexcept
        {
            if (buffer.getNumChannels() < 2 || buffer.getNumSamples() <= 0)
                return;

            auto* l = buffer.getWritePointer(0);
            auto* r = buffer.getWritePointer(1);
            const int numSamples = buffer.getNumSamples();

            const Mode detected = detect(l, r, numSamples);
            updateStableMode(detected);
            apply(buffer, currentMode);
        }

        Mode currentMode = Mode::Stereo;
        Mode pendingMode = Mode::Stereo;
        int pendingBlocks = 0;

    private:
        static Mode detect(const float* l, const float* r, int numSamples) noexcept
        {
            float peakL = 0.0f;
            float peakR = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                peakL = juce::jmax(peakL, std::abs(l[i]));
                peakR = juce::jmax(peakR, std::abs(r[i]));
            }

            constexpr float activeThreshold = 0.0005f;
            constexpr float inactiveRelativeToActive = 0.08f;
            constexpr float absoluteNoiseFloor = 0.00015f;

            const bool leftOnly = peakL >= activeThreshold
                && peakR < juce::jmax(absoluteNoiseFloor, peakL * inactiveRelativeToActive);

            const bool rightOnly = peakR >= activeThreshold
                && peakL < juce::jmax(absoluteNoiseFloor, peakR * inactiveRelativeToActive);

            if (leftOnly)
                return Mode::LeftOnly;

            if (rightOnly)
                return Mode::RightOnly;

            return Mode::Stereo;
        }

        void updateStableMode(Mode detected) noexcept
        {
            if (detected == currentMode)
            {
                pendingMode = detected;
                pendingBlocks = 0;
                return;
            }

            if (detected != pendingMode)
            {
                pendingMode = detected;
                pendingBlocks = 1;
                return;
            }

            ++pendingBlocks;

            constexpr int confirmationBlocks = 3;
            if (pendingBlocks >= confirmationBlocks)
            {
                currentMode = pendingMode;
                pendingBlocks = 0;
            }
        }

        static void apply(juce::AudioBuffer<float>& buffer, Mode mode) noexcept
        {
            if (mode == Mode::Stereo)
                return;

            auto* l = buffer.getWritePointer(0);
            auto* r = buffer.getWritePointer(1);
            const int numSamples = buffer.getNumSamples();

            if (mode == Mode::LeftOnly)
                juce::FloatVectorOperations::copy(r, l, numSamples);
            else if (mode == Mode::RightOnly)
                juce::FloatVectorOperations::copy(l, r, numSamples);
        }
    };

    struct MusicalInputGate
    {
        void prepare(double newSampleRate) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            holdSamples = juce::jmax(1, juce::roundToInt(sampleRate * holdMs * 0.001));
            updateCoefficients();
            reset();
        }

        void reset() noexcept
        {
            envelope = 0.0f;
            gateGain = 1.0f;
            isOpen = true;
            holdCounter = holdSamples;
        }

        void process(juce::AudioBuffer<float>& buffer,
            juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& thresholdLinearSmooth) noexcept
        {
            const int channels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
            const int samples = buffer.getNumSamples();

            if (channels <= 0 || samples <= 0)
                return;

            std::array<float*, kMaxChannels> data{};
            for (int ch = 0; ch < channels; ++ch)
                data[(size_t)ch] = buffer.getWritePointer(ch);

            for (int i = 0; i < samples; ++i)
            {
                float detector = 0.0f;
                for (int ch = 0; ch < channels; ++ch)
                    detector = juce::jmax(detector, std::abs(data[(size_t)ch][i]));

                const float envCoeff = detector > envelope ? envelopeAttackCoeff : envelopeReleaseCoeff;
                envelope = detector + envCoeff * (envelope - detector);

                const float thresholdLinear = thresholdLinearSmooth.getNextValue();

                float targetGain = 1.0f;

                if (thresholdLinear > 0.0f)
                {
                    const float openThreshold = thresholdLinear * openHysteresisMultiplier;
                    const float closeThreshold = thresholdLinear * closeHysteresisMultiplier;

                    if (isOpen)
                    {
                        if (envelope < closeThreshold)
                        {
                            if (holdCounter > 0)
                                --holdCounter;
                            else
                                isOpen = false;
                        }
                        else
                        {
                            holdCounter = holdSamples;
                        }
                    }
                    else
                    {
                        if (envelope > openThreshold)
                        {
                            isOpen = true;
                            holdCounter = holdSamples;
                        }
                    }

                    targetGain = isOpen ? 1.0f : closedGainLinear;
                }
                else
                {
                    isOpen = true;
                    holdCounter = holdSamples;
                    targetGain = 1.0f;
                }

                const float gainCoeff = targetGain > gateGain ? gateOpenCoeff : gateCloseCoeff;
                gateGain = targetGain + gainCoeff * (gateGain - targetGain);

                if (std::abs(gateGain - 1.0f) > 1.0e-6f)
                    for (int ch = 0; ch < channels; ++ch)
                        data[(size_t)ch][i] *= gateGain;
            }
        }

        double sampleRate = 44100.0;
        float envelope = 0.0f;
        float gateGain = 1.0f;
        bool isOpen = true;
        int holdCounter = 0;
        int holdSamples = 441;

        float envelopeAttackMs = 1.0f;
        float envelopeReleaseMs = 90.0f;
        float gateOpenMs = 4.0f;
        float gateCloseMs = 65.0f;
        float holdMs = 24.0f;

        float envelopeAttackCoeff = 0.0f;
        float envelopeReleaseCoeff = 0.0f;
        float gateOpenCoeff = 0.0f;
        float gateCloseCoeff = 0.0f;

        float closedGainLinear = 0.0001f;
        float openHysteresisMultiplier = 1.50f;
        float closeHysteresisMultiplier = 0.56f;

    private:
        void updateCoefficients() noexcept
        {
            envelopeAttackCoeff = timeToCoeff(envelopeAttackMs);
            envelopeReleaseCoeff = timeToCoeff(envelopeReleaseMs);
            gateOpenCoeff = timeToCoeff(gateOpenMs);
            gateCloseCoeff = timeToCoeff(gateCloseMs);
        }

        float timeToCoeff(float ms) const noexcept
        {
            const float seconds = juce::jmax(0.001f, ms * 0.001f);
            return (float)std::exp(-1.0 / (sampleRate * seconds));
        }
    };

    static float sanitizeInputGainDb(float value) noexcept;
    static float sanitizeGateDb(float value) noexcept;
    static float gateDbToLinear(float gateDb) noexcept;

    void applyParameterTargets(bool forceSync) noexcept;
    void applyForceMonoBlend(juce::AudioBuffer<float>& buffer) noexcept;
    void applyInputGain(juce::AudioBuffer<float>& buffer) noexcept;

    InputConditioner conditioner;
    AutoRoutingDetector autoRouting;
    MusicalInputGate inputGate;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGainLinearSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> monoBlendSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gateThresholdLinearSmooth;

    std::atomic<float> targetInputGainDb{ 0.0f };
    std::atomic<float> targetGateDb{ -100.0f };
    std::atomic<bool> targetForceMono{ false };

    float currentInputGainDb = 0.0f;
    float currentGateDb = -100.0f;
    bool currentForceMono = false;

    bool hardSyncParams = true;

    std::atomic<int> invalidSampleCount{ 0 };
    std::atomic<int> clippedSampleCount{ 0 };
    std::atomic<int> denormalSampleCount{ 0 };
};
