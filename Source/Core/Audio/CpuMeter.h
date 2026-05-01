#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

class CpuMeter
{
public:
    void reset() noexcept
    {
        cpuUsage.store(0.0, std::memory_order_relaxed);
        lastProcessTimeMs.store(0.0, std::memory_order_relaxed);
        averageProcessTimeMs.store(0.0, std::memory_order_relaxed);
        peakProcessTimeMs.store(0.0, std::memory_order_relaxed);
    }

    double beginBlock() noexcept
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    void endBlock(double startMs, int numSamples, double sampleRate) noexcept
    {
        double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

        if (!std::isfinite(elapsedMs))
            return;

        elapsedMs = juce::jlimit(0.0, 10000.0, elapsedMs);
        lastProcessTimeMs.store(elapsedMs, std::memory_order_relaxed);

        const double previousAverage = averageProcessTimeMs.load(std::memory_order_relaxed);
        const double average = previousAverage <= 0.0
            ? elapsedMs
            : (previousAverage * 0.90) + (elapsedMs * 0.10);
        averageProcessTimeMs.store(average, std::memory_order_relaxed);

        const double previousPeak = peakProcessTimeMs.load(std::memory_order_relaxed);
        const double decayedPeak = previousPeak * 0.995;
        peakProcessTimeMs.store(juce::jmax(elapsedMs, decayedPeak), std::memory_order_relaxed);

        if (sampleRate <= 0.0 || numSamples <= 0)
            return;

        const double blockMs = ((double)numSamples / sampleRate) * 1000.0;
        if (blockMs <= 0.0)
            return;

        const double blockCpu = juce::jlimit(0.0, 10000.0, (elapsedMs / blockMs) * 100.0);
        const double previousCpu = cpuUsage.load(std::memory_order_relaxed);
        const double smoothedCpu = previousCpu <= 0.0
            ? blockCpu
            : (previousCpu * 0.90) + (blockCpu * 0.10);
        cpuUsage.store(smoothedCpu, std::memory_order_relaxed);
    }

    double getCpuLoad() const noexcept
    {
        return cpuUsage.load(std::memory_order_relaxed);
    }

    double getLastProcessTimeMs() const noexcept
    {
        return lastProcessTimeMs.load(std::memory_order_relaxed);
    }

    double getAverageProcessTimeMs() const noexcept
    {
        return averageProcessTimeMs.load(std::memory_order_relaxed);
    }

    double getPeakProcessTimeMs() const noexcept
    {
        return peakProcessTimeMs.load(std::memory_order_relaxed);
    }

private:
    std::atomic<double> cpuUsage{ 0.0 };
    std::atomic<double> lastProcessTimeMs{ 0.0 };
    std::atomic<double> averageProcessTimeMs{ 0.0 };
    std::atomic<double> peakProcessTimeMs{ 0.0 };
};
