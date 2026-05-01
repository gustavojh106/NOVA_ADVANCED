#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace Nova::Audio
{
class DryWetMixer
{
    struct MixRamp
    {
        double sampleRate = 44100.0;
        float current = 1.0f;
        float target = 1.0f;
        float step = 0.0f;
        int samplesRemaining = 0;
        int defaultRampSamples = 64;

        void prepare(double newSampleRate, double seconds) noexcept
        {
            sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
            defaultRampSamples = juce::jmax(1, juce::roundToInt(sampleRate * juce::jmax(0.001, seconds)));
            reset(current);
        }

        void reset(float value) noexcept
        {
            current = target = juce::jlimit(0.0f, 1.0f, value);
            step = 0.0f;
            samplesRemaining = 0;
        }

        void setTarget(float value) noexcept
        {
            const float newTarget = juce::jlimit(0.0f, 1.0f, value);
            if (std::abs(newTarget - target) < 1.0e-7f)
                return;

            target = newTarget;
            samplesRemaining = defaultRampSamples;
            step = (target - current) / (float)samplesRemaining;
        }

        float getNext() noexcept
        {
            if (samplesRemaining <= 0)
            {
                current = target;
                return current;
            }

            current += step;
            --samplesRemaining;

            if (samplesRemaining <= 0)
            {
                current = target;
                step = 0.0f;
            }

            return current;
        }

        float getCurrent() const noexcept { return current; }
        bool isSmoothing() const noexcept { return samplesRemaining > 0; }
    };

public:
    enum class EndpointPath
    {
        Dry,
        Wet,
        Mixed
    };

    void prepareMix(double sampleRate, double seconds) noexcept
    {
        wetMixRamp.prepare(sampleRate, seconds);
    }

    void resetMix(float currentMix) noexcept
    {
        wetMixRamp.reset(currentMix);
    }

    void setTargetMix(float normalizedMix) noexcept
    {
        wetMixRamp.setTarget(normalizedMix);
    }

    float getCurrentMix() const noexcept
    {
        return wetMixRamp.getCurrent();
    }

    bool isMixSmoothing() const noexcept
    {
        return wetMixRamp.isSmoothing();
    }

    EndpointPath classifyEndpoint() const noexcept
    {
        return classifyEndpoint(wetMixRamp.getCurrent(), wetMixRamp.isSmoothing());
    }

    void consumeRamp(int numSamples) noexcept
    {
        consumeRamp(wetMixRamp, numSamples);
    }

    void processDryEndpoint(int numSamples) noexcept
    {
        processDryEndpoint(wetMixRamp, numSamples);
    }

    void processWetEndpoint(int numSamples) noexcept
    {
        processWetEndpoint(wetMixRamp, numSamples);
    }

    void prepareScratch(int samplesPerBlock, int numChannels, int minimumBlockCapacity)
    {
        scratchBlockCapacity = juce::jmax(minimumBlockCapacity, samplesPerBlock * 4);
        scratchChannelCapacity = juce::jmax(2, numChannels);

        dryScratch.setSize(scratchChannelCapacity, scratchBlockCapacity, false, false, true);
        delayedDryScratch.setSize(scratchChannelCapacity, scratchBlockCapacity, false, false, true);
        dryScratch.clear();
        delayedDryScratch.clear();
    }

    void prepareDryDelay(int maxLatencySamples)
    {
        dryDelayMaxLatencySamples = juce::jmax(0, maxLatencySamples);
        dryDelayBufferSize = dryDelayMaxLatencySamples + scratchBlockCapacity + 8;
        dryDelay.assign((size_t)scratchChannelCapacity, std::vector<float>((size_t)dryDelayBufferSize, 0.0f));
        dryDelayWriteIndex = 0;
        currentDryLatencySamples.store(0, std::memory_order_relaxed);
    }

    void resetDryDelayLine() noexcept
    {
        for (auto& channel : dryDelay)
            std::fill(channel.begin(), channel.end(), 0.0f);

        dryDelayWriteIndex = 0;
    }

    void setLatencySamples(int latencySamples) noexcept
    {
        currentDryLatencySamples.store(juce::jlimit(0,
            dryDelayMaxLatencySamples,
            latencySamples), std::memory_order_release);
    }

    int getLatencySamples() const noexcept
    {
        return currentDryLatencySamples.load(std::memory_order_acquire);
    }

    int getDryDelayBufferSize() const noexcept
    {
        return dryDelayBufferSize;
    }

    int getDryDelayWriteIndex() const noexcept
    {
        return dryDelayWriteIndex;
    }

    int getScratchBlockCapacity() const noexcept
    {
        return scratchBlockCapacity;
    }

    int getScratchChannelCapacity() const noexcept
    {
        return scratchChannelCapacity;
    }

    int getProcessChannelCount(int availableChannels) const noexcept
    {
        return juce::jmin(availableChannels, scratchChannelCapacity);
    }

    bool canUseScratch(int numSamples) const noexcept
    {
        return canUseScratch(numSamples, scratchBlockCapacity);
    }

    bool shouldUseOversizedFallback(int numSamples) const noexcept
    {
        return shouldUseOversizedFallback(numSamples, scratchBlockCapacity);
    }

    void captureDry(const juce::AudioBuffer<float>& input, int numChannels, int numSamples) noexcept
    {
        captureDry(dryScratch, input, numChannels, numSamples);
    }

    static constexpr float endpointEpsilon() noexcept
    {
        return 1.0e-5f;
    }

    static bool canUseScratch(int numSamples, int scratchBlockCapacity) noexcept
    {
        return numSamples <= scratchBlockCapacity;
    }

    static bool shouldUseOversizedFallback(int numSamples, int scratchBlockCapacity) noexcept
    {
        return !canUseScratch(numSamples, scratchBlockCapacity);
    }

    static bool isDryEndpointSettled(float currentMix, bool isSmoothing) noexcept
    {
        return !isSmoothing && currentMix <= endpointEpsilon();
    }

    static bool isWetEndpointSettled(float currentMix, bool isSmoothing) noexcept
    {
        return !isSmoothing && currentMix >= (1.0f - endpointEpsilon());
    }

    static EndpointPath classifyEndpoint(float currentMix, bool isSmoothing) noexcept
    {
        if (isDryEndpointSettled(currentMix, isSmoothing))
            return EndpointPath::Dry;

        if (isWetEndpointSettled(currentMix, isSmoothing))
            return EndpointPath::Wet;

        return EndpointPath::Mixed;
    }

    static int clampLatencyForDelay(int latencySamples,
        int dryDelayBufferSize,
        int numSamples) noexcept
    {
        return juce::jlimit(0,
            juce::jmax(0, dryDelayBufferSize - numSamples - 1),
            latencySamples);
    }

    template <typename Ramp>
    static void consumeRamp(Ramp& ramp, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            (void)ramp.getNext();
    }

    template <typename Ramp>
    static void processDryEndpoint(Ramp& ramp, int numSamples) noexcept
    {
        consumeRamp(ramp, numSamples);
    }

    template <typename Ramp>
    static void processWetEndpoint(Ramp& ramp, int numSamples) noexcept
    {
        consumeRamp(ramp, numSamples);
    }

    static void captureDry(juce::AudioBuffer<float>& dryScratch,
        const juce::AudioBuffer<float>& input,
        int numChannels,
        int numSamples) noexcept
    {
        for (int ch = 0; ch < numChannels; ++ch)
            dryScratch.copyFrom(ch, 0, input, ch, 0, numSamples);
    }

    template <typename Ramp>
    static void mixWetDrySampleAccurate(juce::AudioBuffer<float>& wetBuffer,
        const juce::AudioBuffer<float>& dryBuffer,
        Ramp& wetMixRamp,
        int numChannels,
        int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float wet = wetMixRamp.getNext();
            const float dry = 1.0f - wet;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* wetData = wetBuffer.getWritePointer(ch);
                const auto* dryData = dryBuffer.getReadPointer(ch);
                wetData[i] = dryData[i] * dry + wetData[i] * wet;
            }
        }
    }

    void mixCapturedDryWithWet(juce::AudioBuffer<float>& wetBuffer,
        int numChannels,
        int numSamples) noexcept
    {
        mixCapturedDryWithWet(wetBuffer,
            dryScratch,
            delayedDryScratch,
            dryDelay,
            dryDelayWriteIndex,
            dryDelayBufferSize,
            currentDryLatencySamples.load(std::memory_order_acquire),
            wetMixRamp,
            numChannels,
            numSamples);
    }

private:
    static void copyDryThroughLatency(const juce::AudioBuffer<float>& dryIn,
        juce::AudioBuffer<float>& dryOut,
        std::vector<std::vector<float>>& dryDelay,
        int& dryDelayWriteIndex,
        int dryDelayBufferSize,
        int latencySamples,
        int numChannels,
        int numSamples) noexcept
    {
        const int latency = clampLatencyForDelay(latencySamples, dryDelayBufferSize, numSamples);

        if (latency == 0 || dryDelayBufferSize <= 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                dryOut.copyFrom(ch, 0, dryIn, ch, 0, numSamples);
            return;
        }

        int writeIndex = dryDelayWriteIndex;
        const int delaySize = dryDelayBufferSize;

        for (int i = 0; i < numSamples; ++i)
        {
            const int readIndex = (writeIndex + delaySize - latency) % delaySize;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& delay = dryDelay[(size_t)ch];
                dryOut.setSample(ch, i, delay[(size_t)readIndex]);
                delay[(size_t)writeIndex] = dryIn.getSample(ch, i);
            }

            ++writeIndex;
            if (writeIndex >= delaySize)
                writeIndex = 0;
        }

        dryDelayWriteIndex = writeIndex;
    }

    template <typename Ramp>
    static void mixCapturedDryWithWet(juce::AudioBuffer<float>& wetBuffer,
        const juce::AudioBuffer<float>& dryScratch,
        juce::AudioBuffer<float>& delayedDryScratch,
        std::vector<std::vector<float>>& dryDelay,
        int& dryDelayWriteIndex,
        int dryDelayBufferSize,
        int latencySamples,
        Ramp& wetMixRamp,
        int numChannels,
        int numSamples) noexcept
    {
        copyDryThroughLatency(dryScratch,
            delayedDryScratch,
            dryDelay,
            dryDelayWriteIndex,
            dryDelayBufferSize,
            latencySamples,
            numChannels,
            numSamples);

        mixWetDrySampleAccurate(wetBuffer,
            delayedDryScratch,
            wetMixRamp,
            numChannels,
            numSamples);
    }

    MixRamp wetMixRamp;
    juce::AudioBuffer<float> dryScratch;
    juce::AudioBuffer<float> delayedDryScratch;
    std::vector<std::vector<float>> dryDelay;
    int dryDelayWriteIndex = 0;
    int dryDelayBufferSize = 0;
    int dryDelayMaxLatencySamples = 0;
    std::atomic<int> currentDryLatencySamples{ 0 };
    int scratchBlockCapacity = 0;
    int scratchChannelCapacity = 0;
};
}
