#pragma once

#include <JuceHeader.h>

#include "Constants.h"

#include <array>
#include <atomic>
#include <cmath>
#include <utility>

namespace NovaDiagnostics
{
inline juce::String formatTelemetryScalar(float value)
{
    return juce::String(value, 6);
}

struct SignalStageWindowMetrics
{
    int blocks = 0;
    int totalSamples = 0;
    int nearClipSamples = 0;
    int invalidSamples = 0;
    int clippedSamples = 0;
    std::array<float, 2> peakMax{};
    std::array<double, 2> rmsSum{};
    std::array<float, 2> rmsMax{};
    std::array<float, 2> dcMax{};
    std::array<float, 2> deltaMax{};
    std::array<float, 2> previousSamples{};
    bool hasPreviousSamples = false;

    void reset() noexcept
    {
        *this = {};
    }

    bool hasData() const noexcept
    {
        return blocks > 0 && totalSamples > 0;
    }

    void capture(const juce::AudioBuffer<float>& buffer) noexcept
    {
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return;

        ++blocks;
        totalSamples += numSamples;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            double sumSquares = 0.0;
            double sum = 0.0;
            float channelPeak = 0.0f;
            float channelDelta = 0.0f;
            float previous = hasPreviousSamples ? previousSamples[(size_t) ch] : data[0];

            for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            {
                const float sample = data[sampleIndex];

                if (!std::isfinite(sample))
                {
                    ++invalidSamples;
                    continue;
                }

                const float absSample = std::abs(sample);
                channelPeak = juce::jmax(channelPeak, absSample);
                sumSquares += sample * sample;
                sum += sample;

                if (absSample >= Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                    ++nearClipSamples;

                if (absSample > Nova::Config::HARD_ABS_LIMIT_LINEAR)
                    ++clippedSamples;

                if (sampleIndex > 0 || hasPreviousSamples)
                    channelDelta = juce::jmax(channelDelta, std::abs(sample - previous));

                previous = sample;
            }

            previousSamples[(size_t) ch] = data[numSamples - 1];

            const float channelRms = std::sqrt((float) (sumSquares / juce::jmax(1, numSamples)));
            const float channelDc = std::abs((float) (sum / juce::jmax(1, numSamples)));

            peakMax[(size_t) ch] = juce::jmax(peakMax[(size_t) ch], channelPeak);
            rmsSum[(size_t) ch] += channelRms;
            rmsMax[(size_t) ch] = juce::jmax(rmsMax[(size_t) ch], channelRms);
            dcMax[(size_t) ch] = juce::jmax(dcMax[(size_t) ch], channelDc);
            deltaMax[(size_t) ch] = juce::jmax(deltaMax[(size_t) ch], channelDelta);
        }

        hasPreviousSamples = true;
    }

    juce::String buildSummary(const juce::String& label) const
    {
        const double divisor = (double) juce::jmax(1, blocks);
        juce::String summary;
        summary << label
                << ": peakLMax=" << formatTelemetryScalar(peakMax[0])
                << ", peakRMax=" << formatTelemetryScalar(peakMax[1])
                << ", rmsLAvg=" << formatTelemetryScalar((float) (rmsSum[0] / divisor))
                << ", rmsRAvg=" << formatTelemetryScalar((float) (rmsSum[1] / divisor))
                << ", rmsLMax=" << formatTelemetryScalar(rmsMax[0])
                << ", rmsRMax=" << formatTelemetryScalar(rmsMax[1])
                << ", dcLMax=" << formatTelemetryScalar(dcMax[0])
                << ", dcRMax=" << formatTelemetryScalar(dcMax[1])
                << ", deltaLMax=" << formatTelemetryScalar(deltaMax[0])
                << ", deltaRMax=" << formatTelemetryScalar(deltaMax[1])
                << ", nearClipSamples=" << nearClipSamples
                << ", invalidSamples=" << invalidSamples
                << ", clippedSamples=" << clippedSamples;
        return summary;
    }
};

struct PedalSignalBlockMetrics
{
    int numChannels = 0;
    int numSamples = 0;
    float peak = 0.0f;
    float rms = 0.0f;
    float dcAbs = 0.0f;
    float sampleDeltaPeak = 0.0f;
    int nearClipSamples = 0;
    int invalidSamples = 0;
    int clippedSamples = 0;
    std::array<float, 2> channelPeak{};
    std::array<float, 2> channelRms{};
    std::array<float, 2> channelDcAbs{};
    std::array<float, 2> channelSampleDeltaPeak{};
};

struct PedalSignalWindowAccumulator
{
    int blocks = 0;
    int totalSamples = 0;
    int inputActiveBlocks = 0;
    int spikeBlocks = 0;
    int dcAlertBlocks = 0;
    int nearClipSamples = 0;
    int invalidSamples = 0;
    int clippedSamples = 0;
    std::array<float, 2> inputPeakMax{};
    std::array<double, 2> inputRmsSum{};
    std::array<float, 2> inputRmsMax{};
    std::array<float, 2> inputDcMax{};
    std::array<float, 2> inputDeltaMax{};
    std::array<float, 2> outputPeakMax{};
    std::array<double, 2> outputRmsSum{};
    std::array<float, 2> outputRmsMax{};
    std::array<float, 2> outputDcMax{};
    std::array<float, 2> outputDeltaMax{};

    void reset() noexcept
    {
        *this = {};
    }

    bool hasData() const noexcept
    {
        return blocks > 0 && totalSamples > 0;
    }
};

struct RealtimeSignalTelemetryEvent
{
    static constexpr size_t kMaxTagChars = 64;

    std::array<char, kMaxTagChars> tag{};
    PedalSignalWindowAccumulator window;
    uint32_t elapsedMs = 0;
    bool alert = false;
};

class RealtimeSignalTelemetryQueue final
{
public:
    static RealtimeSignalTelemetryQueue& instance() noexcept
    {
        static RealtimeSignalTelemetryQueue queue;
        return queue;
    }

    bool push(const RealtimeSignalTelemetryEvent& event) noexcept
    {
        uint32_t write = writeSequence.load(std::memory_order_relaxed);

        for (;;)
        {
            const uint32_t read = readSequence.load(std::memory_order_acquire);
            if (write - read >= (uint32_t) slots.size())
            {
                droppedEvents.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (writeSequence.compare_exchange_weak(write,
                    write + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                break;
        }

        auto& slot = slots[(size_t) (write % (uint32_t) slots.size())];
        slot.event = event;
        slot.sequence.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(RealtimeSignalTelemetryEvent& event) noexcept
    {
        const uint32_t read = readSequence.load(std::memory_order_relaxed);
        auto& slot = slots[(size_t) (read % (uint32_t) slots.size())];

        if (slot.sequence.load(std::memory_order_acquire) != read + 1)
            return false;

        event = slot.event;
        readSequence.store(read + 1, std::memory_order_release);
        return true;
    }

    int consumeDroppedEvents() noexcept
    {
        return droppedEvents.exchange(0, std::memory_order_acq_rel);
    }

private:
    struct Slot
    {
        std::atomic<uint32_t> sequence{ 0 };
        RealtimeSignalTelemetryEvent event;
    };

    RealtimeSignalTelemetryQueue() = default;

    std::array<Slot, 256> slots{};
    std::atomic<uint32_t> writeSequence{ 0 };
    std::atomic<uint32_t> readSequence{ 0 };
    std::atomic<int> droppedEvents{ 0 };
};

class PedalSignalTelemetry
{
public:
    explicit PedalSignalTelemetry(juce::String tag) noexcept
    {
        setTag(tag);
        reset();
    }

    void setTag(const juce::String& tag) noexcept
    {
        fixedTag.fill(0);

        const auto* source = tag.isNotEmpty() ? tag.toRawUTF8() : "unknown";
        for (size_t i = 0; i + 1 < fixedTag.size() && source[i] != '\0'; ++i)
            fixedTag[i] = source[i];
    }

    void prepare(double newSampleRate) noexcept
    {
        sampleRate = juce::jmax(1.0, newSampleRate);
        intervalSamples = samplesFromMilliseconds(Nova::Config::SIGNAL_TELEMETRY_INTERVAL_MS);
        alertCooldownSamples = samplesFromMilliseconds(Nova::Config::SIGNAL_ALERT_COOLDOWN_MS);
        reset();
    }

    void reset() noexcept
    {
        window.reset();
        samplesSinceLastAlert = alertCooldownSamples;
        previousInputSamples = { { 0.0f, 0.0f } };
        previousOutputSamples = { { 0.0f, 0.0f } };
        hasPreviousInputSamples = false;
        hasPreviousOutputSamples = false;
        pendingInputMetrics = {};
        hasPendingInputMetrics = false;
    }

    void captureInput(const juce::AudioBuffer<float>& inputBuffer) noexcept
    {
        pendingInputMetrics = analyzeBuffer(inputBuffer, previousInputSamples, hasPreviousInputSamples);
        hasPendingInputMetrics = true;
    }

    bool captureOutputAndPublishIfNeeded(const juce::AudioBuffer<float>& outputBuffer) noexcept
    {
        if (!hasPendingInputMetrics)
            captureInput(outputBuffer);

        accumulateWindow(pendingInputMetrics,
            analyzeBuffer(outputBuffer, previousOutputSamples, hasPreviousOutputSamples));
        hasPendingInputMetrics = false;

        return publishIfNeeded();
    }

private:
    int samplesFromMilliseconds(int milliseconds) const noexcept
    {
        return juce::jmax(1, juce::roundToInt(sampleRate * (double) milliseconds * 0.001));
    }

    uint32_t millisecondsFromSamples(int samples) const noexcept
    {
        return (uint32_t) juce::jmax(1, juce::roundToInt((double) samples * 1000.0 / sampleRate));
    }

    static PedalSignalBlockMetrics analyzeBuffer(const juce::AudioBuffer<float>& buffer,
        std::array<float, 2>& previousSamples,
        bool& hasPreviousSamples) noexcept
    {
        PedalSignalBlockMetrics metrics;
        metrics.numChannels = juce::jmin(2, buffer.getNumChannels());
        metrics.numSamples = buffer.getNumSamples();

        if (metrics.numChannels <= 0 || metrics.numSamples <= 0)
            return metrics;

        double combinedSumSquares = 0.0;
        int combinedSampleCount = 0;

        for (int ch = 0; ch < metrics.numChannels; ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            double sumSquares = 0.0;
            double sum = 0.0;
            float channelPeak = 0.0f;
            float channelDelta = 0.0f;
            float previous = hasPreviousSamples ? previousSamples[(size_t) ch] : data[0];

            for (int i = 0; i < metrics.numSamples; ++i)
            {
                const float sample = data[i];

                if (!std::isfinite(sample))
                {
                    ++metrics.invalidSamples;
                    continue;
                }

                const float absSample = std::abs(sample);
                channelPeak = juce::jmax(channelPeak, absSample);
                sumSquares += sample * sample;
                sum += sample;

                if (absSample >= Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                    ++metrics.nearClipSamples;

                if (absSample > Nova::Config::HARD_ABS_LIMIT_LINEAR)
                    ++metrics.clippedSamples;

                if (i > 0 || hasPreviousSamples)
                    channelDelta = juce::jmax(channelDelta, std::abs(sample - previous));

                previous = sample;
            }

            previousSamples[(size_t) ch] = data[metrics.numSamples - 1];

            const float channelRms = std::sqrt((float) (sumSquares / juce::jmax(1, metrics.numSamples)));
            const float channelDcAbs = std::abs((float) (sum / juce::jmax(1, metrics.numSamples)));

            metrics.channelPeak[(size_t) ch] = channelPeak;
            metrics.channelRms[(size_t) ch] = channelRms;
            metrics.channelDcAbs[(size_t) ch] = channelDcAbs;
            metrics.channelSampleDeltaPeak[(size_t) ch] = channelDelta;
            metrics.peak = juce::jmax(metrics.peak, channelPeak);
            metrics.dcAbs = juce::jmax(metrics.dcAbs, channelDcAbs);
            metrics.sampleDeltaPeak = juce::jmax(metrics.sampleDeltaPeak, channelDelta);

            combinedSumSquares += sumSquares;
            combinedSampleCount += metrics.numSamples;
        }

        hasPreviousSamples = true;
        if (combinedSampleCount > 0)
            metrics.rms = std::sqrt((float) (combinedSumSquares / combinedSampleCount));

        return metrics;
    }

    void accumulateWindow(const PedalSignalBlockMetrics& inputMetrics,
        const PedalSignalBlockMetrics& outputMetrics) noexcept
    {
        ++window.blocks;
        window.totalSamples += juce::jmax(0, outputMetrics.numSamples);
        samplesSinceLastAlert = juce::jmin(samplesSinceLastAlert + juce::jmax(0, outputMetrics.numSamples),
            alertCooldownSamples);
        window.nearClipSamples += outputMetrics.nearClipSamples;
        window.invalidSamples += outputMetrics.invalidSamples;
        window.clippedSamples += outputMetrics.clippedSamples;

        if (inputMetrics.peak >= Nova::Config::INPUT_ACTIVE_THRESHOLD)
            ++window.inputActiveBlocks;

        if (outputMetrics.sampleDeltaPeak >= Nova::Config::SIGNAL_SPIKE_DELTA_THRESHOLD)
            ++window.spikeBlocks;

        if (outputMetrics.dcAbs >= Nova::Config::SIGNAL_DC_ALERT_THRESHOLD)
            ++window.dcAlertBlocks;

        for (int ch = 0; ch < 2; ++ch)
        {
            window.inputPeakMax[(size_t) ch] = juce::jmax(window.inputPeakMax[(size_t) ch], inputMetrics.channelPeak[(size_t) ch]);
            window.inputRmsSum[(size_t) ch] += inputMetrics.channelRms[(size_t) ch];
            window.inputRmsMax[(size_t) ch] = juce::jmax(window.inputRmsMax[(size_t) ch], inputMetrics.channelRms[(size_t) ch]);
            window.inputDcMax[(size_t) ch] = juce::jmax(window.inputDcMax[(size_t) ch], inputMetrics.channelDcAbs[(size_t) ch]);
            window.inputDeltaMax[(size_t) ch] = juce::jmax(window.inputDeltaMax[(size_t) ch], inputMetrics.channelSampleDeltaPeak[(size_t) ch]);

            window.outputPeakMax[(size_t) ch] = juce::jmax(window.outputPeakMax[(size_t) ch], outputMetrics.channelPeak[(size_t) ch]);
            window.outputRmsSum[(size_t) ch] += outputMetrics.channelRms[(size_t) ch];
            window.outputRmsMax[(size_t) ch] = juce::jmax(window.outputRmsMax[(size_t) ch], outputMetrics.channelRms[(size_t) ch]);
            window.outputDcMax[(size_t) ch] = juce::jmax(window.outputDcMax[(size_t) ch], outputMetrics.channelDcAbs[(size_t) ch]);
            window.outputDeltaMax[(size_t) ch] = juce::jmax(window.outputDeltaMax[(size_t) ch], outputMetrics.channelSampleDeltaPeak[(size_t) ch]);
        }
    }

    bool publishIfNeeded() noexcept
    {
        if (!window.hasData())
            return false;

        const bool hasAlertCondition = (window.invalidSamples > 0
            || window.clippedSamples > 0
            || window.nearClipSamples > 0
            || window.dcAlertBlocks > 0
            || window.spikeBlocks >= 8);
        const bool intervalElapsed = window.totalSamples >= intervalSamples;
        const bool alertCooldownElapsed = samplesSinceLastAlert >= alertCooldownSamples;

        if (!intervalElapsed && !(hasAlertCondition && alertCooldownElapsed))
            return false;

        RealtimeSignalTelemetryEvent event;
        event.tag = fixedTag;
        event.window = window;
        event.elapsedMs = millisecondsFromSamples(window.totalSamples);
        event.alert = hasAlertCondition && alertCooldownElapsed;

        RealtimeSignalTelemetryQueue::instance().push(event);
        window.reset();

        if (event.alert)
            samplesSinceLastAlert = 0;

        return true;
    }

    std::array<char, RealtimeSignalTelemetryEvent::kMaxTagChars> fixedTag{};
    PedalSignalWindowAccumulator window;
    double sampleRate = 44100.0;
    int intervalSamples = juce::roundToInt(44100.0 * (double) Nova::Config::SIGNAL_TELEMETRY_INTERVAL_MS * 0.001);
    int alertCooldownSamples = juce::roundToInt(44100.0 * (double) Nova::Config::SIGNAL_ALERT_COOLDOWN_MS * 0.001);
    int samplesSinceLastAlert = alertCooldownSamples;
    std::array<float, 2> previousInputSamples{};
    std::array<float, 2> previousOutputSamples{};
    bool hasPreviousInputSamples = false;
    bool hasPreviousOutputSamples = false;
    PedalSignalBlockMetrics pendingInputMetrics;
    bool hasPendingInputMetrics = false;
};
}
