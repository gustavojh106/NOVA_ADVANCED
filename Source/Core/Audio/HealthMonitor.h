#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

#include "../Constants.h"

class HealthMonitor
{
public:
    struct BlockStats
    {
        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        int invalidSamples = 0;
        int clippedSamples = 0;
        int nearClipSamples = 0;
        int clickSpikeSamples = 0;
        bool hadCorruption = false;
    };

    struct Context
    {
        bool engineOn = false;
        bool tunerEnabled = false;
        float outputMixNormalized = 1.0f;
        double sampleRate = 44100.0;
    };

    struct Actions
    {
        bool requestGraphReset = false;
        bool logAutoHeal = false;
        bool logSilentOutput = false;
        bool logSilentOutputRecovery = false;
    };

    void reset(bool resetMeters) noexcept
    {
        consecutiveCorruptBlocks = 0;
        recoveryCooldownBlocks = 0;
        silentOutputBlockCounter.store(0, std::memory_order_relaxed);
        silentOutputIncidentActive.store(false, std::memory_order_relaxed);

        if (resetMeters)
            lastOutputPeak.store(0.0f, std::memory_order_relaxed);
    }

    BlockStats sanitizeAndMeterOutput(juce::AudioBuffer<float>& buffer,
        float inputPeak,
        bool deepScan) noexcept
    {
        BlockStats stats;
        stats.inputPeak = inputPeak;

        constexpr float nearClip = Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD;
        constexpr float spikeDelta = Nova::Config::SIGNAL_SPIKE_DELTA_THRESHOLD;
        const float hardLimit = Nova::Config::HARD_ABS_LIMIT_LINEAR;

        float outputPeak = 0.0f;
        float previousL = 0.0f;
        float previousR = 0.0f;
        bool havePrev = false;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            float previous = ch == 0 ? previousL : previousR;

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                float v = data[i];

                if (!std::isfinite(v))
                {
                    v = 0.0f;
                    data[i] = 0.0f;
                    stats.hadCorruption = true;
                    ++stats.invalidSamples;
                }
                else if (std::abs(v) > hardLimit)
                {
                    v = juce::jlimit(-hardLimit, hardLimit, v);
                    data[i] = v;
                    stats.hadCorruption = true;
                    ++stats.clippedSamples;
                }

                const float absV = std::abs(v);
                outputPeak = juce::jmax(outputPeak, absV);

                if (deepScan)
                {
                    if (absV >= nearClip)
                        ++stats.nearClipSamples;

                    if (havePrev && std::abs(v - previous) >= spikeDelta)
                        ++stats.clickSpikeSamples;
                }

                previous = v;
                havePrev = true;
            }

            if (ch == 0) previousL = previous;
            if (ch == 1) previousR = previous;
        }

        stats.outputPeak = outputPeak;
        lastOutputPeak.store(outputPeak, std::memory_order_relaxed);
        return stats;
    }

    Actions afterBlock(const BlockStats& health,
        int numSamples,
        bool engineActuallyProcessed,
        const Context& context) noexcept
    {
        Actions actions;

        if (health.hadCorruption)
            ++consecutiveCorruptBlocks;
        else
            consecutiveCorruptBlocks = 0;

        if (recoveryCooldownBlocks > 0)
            --recoveryCooldownBlocks;

        if (consecutiveCorruptBlocks >= Nova::Config::CORRUPT_BLOCKS_BEFORE_HEAL
            && recoveryCooldownBlocks == 0)
        {
            actions.requestGraphReset = true;
            actions.logAutoHeal = true;
            autoHealCount.fetch_add(1, std::memory_order_relaxed);
            recoveryCooldownBlocks = Nova::Config::RECOVERY_COOLDOWN_BLOCKS;
            consecutiveCorruptBlocks = 0;
        }

        const bool suspiciousSilence = engineActuallyProcessed
            && context.engineOn
            && !context.tunerEnabled
            && context.outputMixNormalized > 0.01f
            && health.inputPeak >= Nova::Config::INPUT_ACTIVE_THRESHOLD
            && health.outputPeak <= Nova::Config::OUTPUT_SILENT_THRESHOLD;

        if (suspiciousSilence)
        {
            const int blocks = silentOutputBlockCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            const int triggerBlocks = juce::jmax(8, juce::roundToInt(
                (context.sampleRate / juce::jmax(1, numSamples)) * Nova::Config::SILENT_OUTPUT_TRIGGER_SECONDS));

            if (blocks >= triggerBlocks && !silentOutputIncidentActive.exchange(true, std::memory_order_relaxed))
                actions.logSilentOutput = true;
        }
        else
        {
            silentOutputBlockCounter.store(0, std::memory_order_relaxed);
            if (silentOutputIncidentActive.exchange(false, std::memory_order_relaxed))
                actions.logSilentOutputRecovery = true;
        }

        return actions;
    }

    float getLastOutputPeak() const noexcept
    {
        return lastOutputPeak.load(std::memory_order_relaxed);
    }

    int getAutoHealCount() const noexcept
    {
        return autoHealCount.load(std::memory_order_relaxed);
    }

private:
    std::atomic<float> lastOutputPeak{ 0.0f };
    std::atomic<int> autoHealCount{ 0 };
    std::atomic<int> silentOutputBlockCounter{ 0 };
    std::atomic<bool> silentOutputIncidentActive{ false };

    int consecutiveCorruptBlocks = 0;
    int recoveryCooldownBlocks = 0;
};
