#pragma once

#include "Constants.h"
#include "SessionLogger.h"

#include <array>
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

class PedalSignalTelemetry
{
public:
    explicit PedalSignalTelemetry(juce::String tag) noexcept
        : pedalTag(std::move(tag))
    {
        reset();
    }

    void setTag(const juce::String& tag) noexcept
    {
        pedalTag = tag;
    }

    void reset() noexcept
    {
        window.reset();
        windowStartMs = juce::Time::getMillisecondCounter();
        lastAlertMs = 0;
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

    template <typename BuildExtra, typename ResetExtra>
    void captureOutputAndEmitIfNeeded(const juce::AudioBuffer<float>& outputBuffer,
        BuildExtra&& buildExtra,
        ResetExtra&& resetExtra) noexcept
    {
        if (!hasPendingInputMetrics)
            captureInput(outputBuffer);

        accumulateWindow(pendingInputMetrics,
            analyzeBuffer(outputBuffer, previousOutputSamples, hasPreviousOutputSamples));
        hasPendingInputMetrics = false;

        emitIfNeeded(std::forward<BuildExtra>(buildExtra), std::forward<ResetExtra>(resetExtra));
    }

private:
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

    juce::String buildReport(const PedalSignalWindowAccumulator& snapshot, uint32_t elapsedMs) const
    {
        const double divisor = (double) juce::jmax(1, snapshot.blocks);
        juce::String report;
        report << "windowMs=" << (int) elapsedMs
               << ", blocks=" << snapshot.blocks
               << ", avgSamplesPerBlock=" << formatTelemetryScalar((float) snapshot.totalSamples / (float) juce::jmax(1, snapshot.blocks))
               << juce::newLine
               << "input.signal: peakLMax=" << formatTelemetryScalar(snapshot.inputPeakMax[0])
               << ", peakRMax=" << formatTelemetryScalar(snapshot.inputPeakMax[1])
               << ", rmsLAvg=" << formatTelemetryScalar((float) (snapshot.inputRmsSum[0] / divisor))
               << ", rmsRAvg=" << formatTelemetryScalar((float) (snapshot.inputRmsSum[1] / divisor))
               << ", rmsLMax=" << formatTelemetryScalar(snapshot.inputRmsMax[0])
               << ", rmsRMax=" << formatTelemetryScalar(snapshot.inputRmsMax[1])
               << ", dcLMax=" << formatTelemetryScalar(snapshot.inputDcMax[0])
               << ", dcRMax=" << formatTelemetryScalar(snapshot.inputDcMax[1])
               << ", deltaLMax=" << formatTelemetryScalar(snapshot.inputDeltaMax[0])
               << ", deltaRMax=" << formatTelemetryScalar(snapshot.inputDeltaMax[1])
               << juce::newLine
               << "output.signal: peakLMax=" << formatTelemetryScalar(snapshot.outputPeakMax[0])
               << ", peakRMax=" << formatTelemetryScalar(snapshot.outputPeakMax[1])
               << ", rmsLAvg=" << formatTelemetryScalar((float) (snapshot.outputRmsSum[0] / divisor))
               << ", rmsRAvg=" << formatTelemetryScalar((float) (snapshot.outputRmsSum[1] / divisor))
               << ", rmsLMax=" << formatTelemetryScalar(snapshot.outputRmsMax[0])
               << ", rmsRMax=" << formatTelemetryScalar(snapshot.outputRmsMax[1])
               << ", dcLMax=" << formatTelemetryScalar(snapshot.outputDcMax[0])
               << ", dcRMax=" << formatTelemetryScalar(snapshot.outputDcMax[1])
               << ", deltaLMax=" << formatTelemetryScalar(snapshot.outputDeltaMax[0])
               << ", deltaRMax=" << formatTelemetryScalar(snapshot.outputDeltaMax[1])
               << juce::newLine
               << "anomalies: inputActiveBlocks=" << snapshot.inputActiveBlocks
               << ", spikeBlocks=" << snapshot.spikeBlocks
               << ", dcAlertBlocks=" << snapshot.dcAlertBlocks
               << ", nearClipSamples=" << snapshot.nearClipSamples
               << ", invalidSamples=" << snapshot.invalidSamples
               << ", clippedSamples=" << snapshot.clippedSamples;
        return report;
    }

    template <typename BuildExtra, typename ResetExtra>
    void emitIfNeeded(BuildExtra&& buildExtra, ResetExtra&& resetExtra) noexcept
    {
        if (!window.hasData())
            return;

        const uint32_t now = juce::Time::getMillisecondCounter();
        if (windowStartMs == 0)
            windowStartMs = now;

        const uint32_t elapsedMs = now - windowStartMs;
        const bool hasAlertCondition = (window.invalidSamples > 0
            || window.clippedSamples > 0
            || window.nearClipSamples > 0
            || window.dcAlertBlocks > 0
            || window.spikeBlocks >= 8);
        const bool intervalElapsed = elapsedMs >= (uint32_t) Nova::Config::SIGNAL_TELEMETRY_INTERVAL_MS;
        const bool alertCooldownElapsed = (lastAlertMs == 0
            || (now - lastAlertMs) >= (uint32_t) Nova::Config::SIGNAL_ALERT_COOLDOWN_MS);

        if (!intervalElapsed && !(hasAlertCondition && alertCooldownElapsed))
            return;

        const auto snapshot = window;
        window.reset();
        windowStartMs = now;

        const bool emitAsAlert = hasAlertCondition && alertCooldownElapsed;
        if (emitAsAlert)
            lastAlertMs = now;

        const auto emitBuildStart = juce::Time::getMillisecondCounterHiRes();
        auto report = buildReport(snapshot, juce::jmax<uint32_t>(1, elapsedMs));
        const auto extra = buildExtra();
        if (extra.isNotEmpty())
            report << juce::newLine << extra;

        const auto loggerStats = SessionLogger::getQueueStats();
        const auto emitBuildMs = juce::Time::getMillisecondCounterHiRes() - emitBuildStart;
        report << juce::newLine
               << "telemetry.emit: buildMs=" << formatTelemetryScalar((float) emitBuildMs)
               << ", loggerQueued=" << loggerStats.queuedEntries
               << ", loggerPeak=" << loggerStats.peakQueuedEntries
               << ", loggerDropped=" << loggerStats.droppedEntries;

        NovaDiagnostics::SessionLogger::logEvent("pedal.private." + juce::String(pedalTag) + (emitAsAlert ? ".alert" : ".window"),
            report);
        resetExtra();
    }

    juce::String pedalTag;
    PedalSignalWindowAccumulator window;
    uint32_t windowStartMs = 0;
    uint32_t lastAlertMs = 0;
    std::array<float, 2> previousInputSamples{};
    std::array<float, 2> previousOutputSamples{};
    bool hasPreviousInputSamples = false;
    bool hasPreviousOutputSamples = false;
    PedalSignalBlockMetrics pendingInputMetrics;
    bool hasPendingInputMetrics = false;
};
}
