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
        updateWindowInterval(startMs);

        double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

        if (!std::isfinite(elapsedMs))
            return;

        elapsedMs = juce::jlimit(0.0, 10000.0, elapsedMs);
        lastProcessTimeMs.store(elapsedMs, std::memory_order_relaxed);

        if (elapsedMs > windowPeakProcessTimeMs.load(std::memory_order_relaxed))
            windowPeakProcessTimeMs.store(elapsedMs, std::memory_order_relaxed);

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

    // Measurement window for the Latency Autopilot buffer calibration: the worst block time
    // and the longest gap between block starts since the last reset. Any thread may request
    // the reset; the audio thread applies it at its next block, so it stays the only writer.
    void requestWindowReset() noexcept
    {
        windowResetRequested.store(true, std::memory_order_release);
    }

    double getWindowPeakProcessTimeMs() const noexcept
    {
        return windowPeakProcessTimeMs.load(std::memory_order_relaxed);
    }

    double getWindowMaxCallbackIntervalMs() const noexcept
    {
        return windowMaxCallbackIntervalMs.load(std::memory_order_relaxed);
    }

private:
    void updateWindowInterval(double startMs) noexcept
    {
        if (windowResetRequested.load(std::memory_order_relaxed)
            && windowResetRequested.exchange(false, std::memory_order_acq_rel))
        {
            windowPeakProcessTimeMs.store(0.0, std::memory_order_relaxed);
            windowMaxCallbackIntervalMs.store(0.0, std::memory_order_relaxed);
        }
        else if (previousBlockStartMs > 0.0)
        {
            const double intervalMs = startMs - previousBlockStartMs;
            if (std::isfinite(intervalMs) && intervalMs > windowMaxCallbackIntervalMs.load(std::memory_order_relaxed))
                windowMaxCallbackIntervalMs.store(intervalMs, std::memory_order_relaxed);
        }

        previousBlockStartMs = startMs;
    }

    std::atomic<double> cpuUsage{ 0.0 };
    std::atomic<double> lastProcessTimeMs{ 0.0 };
    std::atomic<double> averageProcessTimeMs{ 0.0 };
    std::atomic<double> peakProcessTimeMs{ 0.0 };

    std::atomic<bool> windowResetRequested{ false };
    std::atomic<double> windowPeakProcessTimeMs{ 0.0 };
    std::atomic<double> windowMaxCallbackIntervalMs{ 0.0 };
    double previousBlockStartMs = 0.0; // audio thread only
};
